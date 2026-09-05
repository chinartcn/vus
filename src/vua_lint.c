/*
 * vua_lint.c — .vua 离线校验（vus lint 命令）与 LSP 校验闭环实现
 *
 * 复用 rt/vua.c 的严格校验与渲染树归一：
 *   vua_control_table_load / vua_dict_load → 登记控件表/词典
 *   vua_screen_load → 解析 + 严格校验（type 在控件表、属性在词典）
 *   vua_screen_dump_rendertree → 渲染树归一可产即通过（tests/vua_smoke.c 同套行为）
 *
 * CLI：vus lint [--controls <控件表.json>] [--dict <词典.json>] <file.vua>...
 * LSP：vua_lint_text 供诊断发布（见 src/lsp/lsp.c）。
 */

#define _GNU_SOURCE
#include "vua_lint.h"
#include <stdlib.h>
#include <string.h>

/* ============ 文件读取 ============ */

static char *read_file_alloc(const char *path, char *errbuf, size_t errsz) {
    FILE *fp = path ? fopen(path, "rb") : NULL;
    if (!fp) {
        if (errbuf && errsz)
            snprintf(errbuf, errsz, "无法打开文件: %s", path ? path : "(空路径)");
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len < 0) {
        if (errbuf && errsz) snprintf(errbuf, errsz, "无法读取文件: %s", path);
        fclose(fp);
        return NULL;
    }
    char *data = (char *)malloc((size_t)len + 1);
    if (!data) {
        if (errbuf && errsz) snprintf(errbuf, errsz, "内存不足");
        fclose(fp);
        return NULL;
    }
    size_t n = fread(data, 1, (size_t)len, fp);
    fclose(fp);
    data[n] = '\0';
    return data;
}

/* ============ 进程级控件表/词典加载（幂等） ============ */

int vua_lint_ensure_catalog(const char *hint_dir, char *info, size_t info_sz) {
    static int ensured = 0;
    static char summary[256] = "";
    if (!ensured) {
        const char *cands[10];
        char pbuf[8][1024];
        int n = 0;

        const char *env = getenv("VUS_VUA_CONTROLS");
        if (env && env[0]) cands[n++] = env;
        if (hint_dir && hint_dir[0]) {
            snprintf(pbuf[0], sizeof(pbuf[0]), "%s/vua_controls.json", hint_dir);
            cands[n++] = pbuf[0];
            snprintf(pbuf[1], sizeof(pbuf[1]), "%s/testdata/vua_controls.json", hint_dir);
            cands[n++] = pbuf[1];
        }
        cands[n++] = "vua_controls.json";
        cands[n++] = "testdata/vua_controls.json";
        if (n > 10) n = 10;

        char *json = NULL;
        const char *used = NULL;
        for (int i = 0; i < n && !json; i++) {
            if (!cands[i] || !cands[i][0]) continue;
            json = read_file_alloc(cands[i], NULL, 0);
            if (json) used = cands[i];
        }
        if (json) {
            VuaError e;
            memset(&e, 0, sizeof(e));
            if (vua_control_table_load(json, &e) != 0) {
                snprintf(summary, sizeof(summary),
                         "控件表加载失败(%s): %s", used ? used : "", e.msg);
            } else {
                snprintf(summary, sizeof(summary), "控件表: %s", used ? used : "");
            }
            free(json);
            /* 词典（可选）：VUS_VUA_DICT 指向 {"词典":{...}} JSON 文件 */
            const char *denv = getenv("VUS_VUA_DICT");
            if (denv && denv[0]) {
                char *dj = read_file_alloc(denv, NULL, 0);
                if (dj) {
                    VuaError de;
                    memset(&de, 0, sizeof(de));
                    if (vua_dict_load(dj, &de) == 0) {
                        size_t used_len = strlen(summary);
                        snprintf(summary + used_len, sizeof(summary) - used_len,
                                 "  词典: %s", denv);
                    }
                    free(dj);
                }
            }
        } else {
            snprintf(summary, sizeof(summary),
                     "控件表: 未找到（仅做基础 JSON/结构校验；可用 --controls 或 VUS_VUA_CONTROLS 指定）");
        }
        ensured = 1;
    }
    if (info && info_sz > 0) {
        snprintf(info, info_sz, "%s", summary);
        return summary[0] ? (strstr(summary, "控件表: ") ? 0 : -1) : 0;
    }
    return 0;
}

/* ============ 文本校验（CLI 与 LSP 共用核心） ============ */

int vua_lint_text(const char *vua_json, const char *display, VuaError *err,
                  unsigned long long *hash_out) {
    (void)display;
    VuaScreen *screen = vua_screen_load(vua_json ? vua_json : "", err);
    if (!screen) return -1;
    const char *rt = vua_screen_dump_rendertree(screen);
    if (hash_out) *hash_out = vua_screen_rendertree_hash(screen);
    (void)rt; /* 渲染树可归一化（含严格校验）即视为通过 */
    vua_screen_free(screen);
    return 0;
}

void vua_lint_print_error(FILE *f, const char *display, const VuaError *err) {
    if (!f || !err) return;
    for (const VuaError *e = err; e; e = e->next) {
        if (display && display[0]) fprintf(f, "%s: ", display);
        fprintf(f, "第 %d 行 [VUA_ERR_%d]: %s\n", e->line, e->code, e->msg);
    }
}

/* ============ CLI：vus lint ============ */

int vus_lint_cmd(int argn, char **args) {
    const char *controls_path = NULL;
    const char *dict_path = NULL;

    /* 先数文件个数（去掉 --controls/--dict 及其取值） */
    int nfiles = 0;
    for (int i = 0; i < argn; i++) {
        if ((strcmp(args[i], "--controls") == 0 || strcmp(args[i], "--dict") == 0) && i + 1 < argn) {
            i++;
            continue;
        }
        if ((strcmp(args[i], "--controls") == 0 || strcmp(args[i], "--dict") == 0)) continue;
        nfiles++;
    }
    if (nfiles == 0) {
        fprintf(stderr, "用法: vus lint [--controls <控件表.json>] [--dict <词典.json>] <file.vua>...\n");
        return 2;
    }
    char **files = (char **)calloc((size_t)nfiles, sizeof(char *));
    if (!files) {
        fprintf(stderr, "内存不足\n");
        return 1;
    }
    int fi = 0;
    for (int i = 0; i < argn; i++) {
        if (strcmp(args[i], "--controls") == 0 && i + 1 < argn) {
            controls_path = args[++i];
            continue;
        }
        if (strcmp(args[i], "--dict") == 0 && i + 1 < argn) {
            dict_path = args[++i];
            continue;
        }
        files[fi++] = args[i];
    }

    char errbuf[512];
    if (controls_path) {
        char *j = read_file_alloc(controls_path, errbuf, sizeof(errbuf));
        if (!j) {
            fprintf(stderr, "控件表读取失败: %s\n", errbuf);
            free(files);
            return 1;
        }
        VuaError e;
        memset(&e, 0, sizeof(e));
        if (vua_control_table_load(j, &e) != 0) {
            fprintf(stderr, "控件表解析失败: %s\n", e.msg);
            free(j);
            free(files);
            return 1;
        }
        free(j);
    } else {
        char info[256];
        vua_lint_ensure_catalog(NULL, info, sizeof(info));
        printf("%s\n", info);
    }
    if (dict_path) {
        char *j = read_file_alloc(dict_path, errbuf, sizeof(errbuf));
        if (!j) {
            fprintf(stderr, "词典读取失败: %s\n", errbuf);
            free(files);
            return 1;
        }
        VuaError e;
        memset(&e, 0, sizeof(e));
        if (vua_dict_load(j, &e) != 0) {
            fprintf(stderr, "词典解析失败: %s\n", e.msg);
            free(j);
            free(files);
            return 1;
        }
        free(j);
    }

    int pass = 0, fail = 0;
    for (int i = 0; i < nfiles; i++) {
        const char *file = files[i];
        char *json = read_file_alloc(file, errbuf, sizeof(errbuf));
        if (!json) {
            printf("校验失败: %s （%s）\n", file, errbuf);
            fail++;
            continue;
        }
        VuaError err;
        memset(&err, 0, sizeof(err));
        unsigned long long hash = 0;
        if (vua_lint_text(json, file, &err, &hash) == 0) {
            printf("校验通过: %s (渲染树指纹 0x%016llx)\n", file, hash);
            pass++;
        } else {
            printf("校验失败: %s\n", file);
            vua_lint_print_error(stderr, file, &err);
            fail++;
        }
        free(json);
    }
    free(files);

    printf("共 %d 个文件，通过 %d，失败 %d\n", nfiles, pass, fail);
    return fail ? 1 : 0;
}