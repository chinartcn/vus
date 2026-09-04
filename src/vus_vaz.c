/* =============================================================================
 * vus_vaz.c — VUS Android 扩展包（.vaz）处理
 *
 * .vaz 是可选插件包（zip 或目录形式），用于按需引入控件模板与 VUS 逻辑库，
 * 核心分层（页面 .vua + 逻辑 .vus）保持不变：
 *   controls/ 目录    复合模板控件：用现有原语（行/文本/按钮…）组合成原子控件，
 *                    页面出现该 type 时在构建期展开为模板子树，{参数} 占位替换。
 *   logic/ 目录       可复用 VUS 函数库，构建期合并输出，供页面逻辑直接调用。
 *   vaz.json          清单："名称/版本/控件/逻辑"。
 *
 * 依赖：yyjson（JSON 解析/操作）、unzip（解压 .vaz zip，目录形式无需）。
 * =============================================================================
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>

#include "yyjson.h"
#include "vus_vaz.h"

#define VAZ_MAX_CTRL  128
#define VAZ_MAX_KEYS  64

/* 控件模板注册项 */
typedef struct {
    char name[128];          /* 中文名（页面 type 匹配用） */
    char enname[64];         /* 英文名（页面 type 匹配用） */
    char file[512];          /* 来源文件（错误提示用） */
    yyjson_mut_val *tpl;     /* 模板子树（属于模板 doc） */
} VazCtrl;

/* 文本缓冲区：轻量动态拼接 */
typedef struct { char *buf; size_t len, cap; } StrB;

static void strb_init(StrB *b) { b->buf = NULL; b->len = 0; b->cap = 0; }
static void strb_free(StrB *b) { free(b->buf); }
static void strb_putc(StrB *b, char c) {
    if (b->len + 2 > b->cap) {
        b->cap = b->cap ? b->cap * 2 : 256;
        b->buf = realloc(b->buf, b->cap);
    }
    b->buf[b->len++] = c;
    b->buf[b->len] = '\0';
}
static void strb_puts(StrB *b, const char *s) {
    if (!s) return;
    while (*s) strb_putc(b, *s++);
}

/* 读取文件全部内容（返回 malloc 缓冲，调用者 free） */
static char *read_file_all(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    if (len < 0) { fclose(fp); return NULL; }
    rewind(fp);
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t n = fread(buf, 1, (size_t)len, fp);
    fclose(fp);
    buf[n] = '\0';
    if (out_len) *out_len = n;
    return buf;
}

/* 拼接两条路径（目录 + 相对文件） */
static void path_join(char *out, size_t sz, const char *dir, const char *name) {
    snprintf(out, sz, "%s%s%s", dir, (dir[0] && dir[strlen(dir) - 1] == '/') ? "" : "/", name);
}

/* ---------------- .vaz 解包 ---------------- */

/* 若 vaz_path 是 .vaz zip 则解包到临时目录并返回该目录（malloc）。
 * 若是已有目录则原样返回（malloc 副本）。失败返回 NULL。 */
static char *vaz_prepare(const char *vaz_path, char *err, size_t errsz) {
    struct stat st;
    if (stat(vaz_path, &st) != 0) {
        snprintf(err, errsz, "找不到扩展包: %s", vaz_path);
        return NULL;
    }
    if (S_ISDIR(st.st_mode)) {
        return strdup(vaz_path);
    }
    /* 文件：按 .vaz 解包 */
    const char *dot = strrchr(vaz_path, '.');
    if (!dot || strcmp(dot, ".vaz") != 0) {
        snprintf(err, errsz, "扩展包需为 .vaz 文件或目录: %s", vaz_path);
        return NULL;
    }
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "/tmp/vus_vaz_%ld", (long)getpid());
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s && unzip -q -o \"%s\" -d %s",
             tmp, tmp, vaz_path, tmp);
    if (system(cmd) != 0) {
        snprintf(err, errsz, "解压扩展包失败: %s", vaz_path);
        return NULL;
    }
    return strdup(tmp);
}

/* ---------------- 模板注册 ---------------- */

/* 从控件描述 JSON doc 注册控件（中文名/英文名 → 模板） */
static int vaz_register_ctrl(VazCtrl *ctrls, int *count,
                             yyjson_mut_doc *doc, const char *file) {
    if (*count >= VAZ_MAX_CTRL) return -1;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    if (!root || !yyjson_mut_is_obj(root)) return -1;

    const char *cn = NULL, *en = NULL;
    yyjson_mut_val *tpl = NULL;
    size_t idx, max;
    yyjson_mut_val *key, *val;
    yyjson_mut_obj_foreach(root, idx, max, key, val) {
        const char *ks = key ? yyjson_mut_get_str(key) : NULL;
        if (!ks) continue;
        if (strcmp(ks, "中文名") == 0 || strcmp(ks, "名称") == 0) cn = yyjson_mut_get_str(val);
        else if (strcmp(ks, "英文名") == 0 || strcmp(ks, "name") == 0) en = yyjson_mut_get_str(val);
        else if (strcmp(ks, "模板") == 0) tpl = val;
    }
    if (!tpl) tpl = root;      /* 无 "模板" 字段时整文件即模板 */
    if (!cn && !en) return -1; /* 至少一个名字 */

    if (cn) {
        VazCtrl *c = &ctrls[*count];
        snprintf(c->name, sizeof(c->name), "%s", cn);
        c->enname[0] = '\0';
        snprintf(c->file, sizeof(c->file), "%s", file);
        c->tpl = tpl;
        (*count)++;
    }
    if (en) {
        /* 英文名若与中文名不同才单独注册 */
        if (!cn || strcmp(cn, en) != 0) {
            if (*count >= VAZ_MAX_CTRL) return -1;
            VazCtrl *c = &ctrls[*count];
            c->name[0] = '\0';
            snprintf(c->enname, sizeof(c->enname), "%s", en);
            snprintf(c->file, sizeof(c->file), "%s", file);
            c->tpl = tpl;
            (*count)++;
        }
    }
    return 0;
}

/* 查找控件：按中文名或英文名匹配 */
static VazCtrl *vaz_find_ctrl(VazCtrl *ctrls, int count, const char *type) {
    for (int i = 0; i < count; i++) {
        if ((ctrls[i].name[0] && strcmp(ctrls[i].name, type) == 0) ||
            (ctrls[i].enname[0] && strcmp(ctrls[i].enname, type) == 0)) {
            return &ctrls[i];
        }
        if (ctrls[i].name[0] == '\0' && ctrls[i].enname[0] == '\0') continue;
    }
    return NULL;
}

/* ---------------- 占位符替换 ---------------- */

/* 在字符串 s 中查找 {name} 占位；命中返回 1 并给出值起始/结束偏移。
 * 整值占位（s 恰好等于 {name}）通过 whole 指示。 */
static int find_placeholder(const char *s, const char **vstart, size_t *vlen, int *whole) {
    const char *lb = strchr(s, '{');
    if (!lb) return 0;
    const char *rb = strchr(lb, '}');
    if (!rb || rb < lb) return 0;
    *vstart = lb + 1;
    *vlen = (size_t)(rb - lb - 1);
    /* 整值占位：{x} 前后无其他字符 */
    *whole = ((size_t)(lb - s) == 0 && s[rb - s + 1] == '\0') ? 1 : 0;
    return 1;
}

static const char *vaz_str_key(const char *s) {
    return s ? s : "";
}

/* 递归替换 node 内所有字符串字段的 {参数} 占位。
 * params：页面节点（自定义控件的调用点），字段名 → 参数值。 */
static void vaz_fill_params(yyjson_mut_doc *doc, yyjson_mut_val *node,
                            yyjson_mut_val *params);

/* 处理单个字符串字段的占位替换 */
static int vaz_fill_str(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key,
                        const char *s, yyjson_mut_val *params) {
    const char *vstart; size_t vlen; int whole;
    if (!find_placeholder(s, &vstart, &vlen, &whole)) return 0;

    char pname[128];
    if (vlen >= sizeof(pname)) return 0;
    memcpy(pname, vstart, vlen);
    pname[vlen] = '\0';

    yyjson_mut_val *pval = yyjson_mut_obj_get(params, pname);
    if (!pval) return 0;   /* 参数未提供：占位保持原样 */

    if (whole) {
        /* 整值占位：直接放入参数值（字符串/数字/布尔/对象/数组） */
        if (yyjson_mut_is_str(pval)) {
            yyjson_mut_obj_put(obj, yyjson_mut_strcpy(doc, key), yyjson_mut_strcpy(doc, yyjson_mut_get_str(pval)));
        } else {
            yyjson_mut_obj_put(obj, yyjson_mut_strcpy(doc, key), yyjson_mut_val_mut_copy(doc, pval));
        }
        return 1;
    }
    /* 部分占位：仅参数为字符串时拼接替换 */
    if (!yyjson_mut_is_str(pval)) return 0;
    const char *pv = yyjson_mut_get_str(pval);
    StrB b; strb_init(&b);
    strb_puts(&b, s ? "" : "");   /* 起始空串（s 非空） */
    {
        /* 手工分段拼接：s 中 {pname} 原样替换为 pv */
        const char *p = vaz_str_key(s);
        while (*p) {
            const char *lb = strchr(p, '{');
            if (!lb) { strb_puts(&b, p); break; }
            const char *rb = strchr(lb, '}');
            if (!rb) { strb_puts(&b, p); break; }
            size_t name_len = (size_t)(rb - lb - 1);
            if (name_len == vlen && memcmp(lb + 1, pname, vlen) == 0) {
                strb_puts(&b, pv);
                p = rb + 1;
            } else {
                strb_putc(&b, '{');
                p = lb + 1;
            }
        }
    }
    yyjson_mut_obj_put(obj, yyjson_mut_strcpy(doc, key), yyjson_mut_strcpy(doc, b.buf ? b.buf : ""));
    strb_free(&b);
    return 1;
}

static void vaz_fill_params(yyjson_mut_doc *doc, yyjson_mut_val *node,
                            yyjson_mut_val *params) {
    if (!node) return;
    /* 数组：递归到每个元素；字符串元素支持整值占位替换 */
    if (yyjson_mut_is_arr(node)) {
        size_t idx, max;
        yyjson_mut_val *child;
        yyjson_mut_arr_foreach(node, idx, max, child) {
            if (!child) continue;
            if (yyjson_mut_is_str(child)) {
                const char *s = yyjson_mut_get_str(child);
                const char *vstart; size_t vlen; int whole;
                if (find_placeholder(s, &vstart, &vlen, &whole) && whole) {
                    char pname[128];
                    if (vlen < sizeof(pname)) {
                        memcpy(pname, vstart, vlen);
                        pname[vlen] = '\0';
                        yyjson_mut_val *pval = yyjson_mut_obj_get(params, pname);
                        if (pval) {
                            yyjson_mut_arr_replace(node, idx, yyjson_mut_val_mut_copy(doc, pval));
                        }
                    }
                }
            } else {
                vaz_fill_params(doc, child, params);
            }
        }
        return;
    }
    if (!yyjson_mut_is_obj(node)) return;
    /* 收集字符串键（避免遍历中改动集合） */
    const char *keys[VAZ_MAX_KEYS];
    yyjson_mut_val *vals[VAZ_MAX_KEYS];
    int kc = 0;
    size_t idx, max;
    yyjson_mut_val *key, *val;
    yyjson_mut_obj_foreach(node, idx, max, key, val) {
        const char *ks = key ? yyjson_mut_get_str(key) : NULL;
        if (!ks) continue;
        if (kc < VAZ_MAX_KEYS) {
            keys[kc] = ks;
            vals[kc] = val;
            kc++;
        }
    }
    for (int i = 0; i < kc; i++) {
        yyjson_mut_val *val = vals[i];
        if (yyjson_mut_is_str(val)) {
            vaz_fill_str(doc, node, keys[i], yyjson_mut_get_str(val), params);
        } else if (yyjson_mut_is_obj(val) || yyjson_mut_is_arr(val)) {
            vaz_fill_params(doc, val, params);   /* 递归进嵌套结构 */
        }
    }
}

/* ---------------- 模板实例化与展开 ---------------- */

/* 递归展开 node 内部（处理其子组件） */
static void vaz_expand_into(yyjson_mut_doc *doc, yyjson_mut_val *node,
                            VazCtrl *ctrls, int ctrl_count);

/* 展开 parent_obj 的子组件数组 */
static void vaz_expand_children(yyjson_mut_doc *doc, yyjson_mut_val *parent_obj,
                                VazCtrl *ctrls, int ctrl_count) {
    yyjson_mut_val *arr = yyjson_mut_obj_get(parent_obj, "子组件");
    if (!arr || !yyjson_mut_is_arr(arr)) arr = yyjson_mut_obj_get(parent_obj, "children");
    if (!arr || !yyjson_mut_is_arr(arr)) return;

    size_t n = yyjson_mut_arr_size(arr);
    size_t i = 0;
    while (i < n) {
        yyjson_mut_val *child = yyjson_mut_arr_get(arr, i);
        if (child && yyjson_mut_is_obj(child)) {
            yyjson_mut_val *tv = yyjson_mut_obj_get(child, "type");
            const char *type = tv ? yyjson_mut_get_str(tv) : NULL;
            VazCtrl *c = type ? vaz_find_ctrl(ctrls, ctrl_count, type) : NULL;
            if (c) {
                /* 命中模板：用页面节点作参数实例化，替换当前位置 */
                yyjson_mut_val *repl = yyjson_mut_val_mut_copy(doc, c->tpl);
                vaz_fill_params(doc, repl, child);
                vaz_expand_into(doc, repl, ctrls, ctrl_count);  /* 模板可能嵌套自定义控件 */
                yyjson_mut_val *old = yyjson_mut_arr_remove(arr, i);
                if (old && repl) {
                    yyjson_mut_arr_insert(arr, repl, i);
                }
                n = yyjson_mut_arr_size(arr);
                i++;   /* 新替换节点已展开，跳过其位置 */
                continue;
            }
            vaz_expand_into(doc, child, ctrls, ctrl_count);
        }
        i++;
    }
}

static void vaz_expand_into(yyjson_mut_doc *doc, yyjson_mut_val *node,
                            VazCtrl *ctrls, int ctrl_count) {
    if (!node || !yyjson_mut_is_obj(node)) return;
    vaz_expand_children(doc, node, ctrls, ctrl_count);
}

/* 展开单个页面文件（原地写回） */
static int vaz_expand_file(VazCtrl *ctrls, int ctrl_count, const char *path,
                           char *info_buf, size_t info_sz) {
    yyjson_doc *idoc = yyjson_read_file(path, 0, NULL, NULL);
    if (!idoc) {
        snprintf(info_buf, info_sz, "  跳过(解析失败): %s", path);
        return 1;
    }
    yyjson_mut_doc *mdoc = yyjson_doc_mut_copy(idoc, NULL);
    if (!mdoc) {
        yyjson_doc_free(idoc);
        snprintf(info_buf, info_sz, "  跳过(转换失败): %s", path);
        return 1;
    }
    yyjson_mut_val *root = yyjson_mut_doc_get_root(mdoc);
    vaz_expand_into(mdoc, root, ctrls, ctrl_count);
    yyjson_mut_write_file(path, mdoc, YYJSON_WRITE_PRETTY, NULL, NULL);
    yyjson_mut_doc_free(mdoc);
    yyjson_doc_free(idoc);
    snprintf(info_buf, info_sz, "  已展开: %s", path);
    return 0;
}

/* ---------------- 逻辑合并 ---------------- */

/* 把包内各逻辑库文件内容合并写出 */
static int vaz_merge_logic(const char *dir, char **logic_files, int count,
                           const char *out_logic, char *err, size_t errsz) {
    if (count <= 0 || !out_logic) return 1;
    FILE *out = fopen(out_logic, "wb");
    if (!out) {
        snprintf(err, errsz, "无法写出逻辑文件: %s", out_logic);
        return -1;
    }
    for (int i = 0; i < count; i++) {
        char full[1024];
        path_join(full, sizeof(full), dir, logic_files[i]);
        size_t len = 0;
        char *content = read_file_all(full, &len);
        if (!content) {
            fprintf(out, "# (缺少逻辑文件: %s)\n", logic_files[i]);
            continue;
        }
        fwrite(content, 1, len, out);
        fwrite("\n", 1, 1, out);
        free(content);
    }
    fclose(out);
    return 0;
}

/* =============================================================================
 * 依赖包导入：源码 `导入 "包名"` → 编译器内建解析 vaz 逻辑库
 * ============================================================================= */

/* 判断目录是否为有效包（含 vaz.json 清单） */
static int vaz_dir_is_pkg(const char *dir) {
    char p[1024];
    path_join(p, sizeof(p), dir, "vaz.json");
    struct stat st;
    return (stat(p, &st) == 0 && S_ISREG(st.st_mode)) ? 1 : 0;
}

/* 解包 .vaz zip 到目标目录（已解包则跳过） */
static int vaz_unpack_into(const char *zip, const char *dest) {
    struct stat st;
    if (stat(dest, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\" && unzip -q -o \"%s\" -d \"%s\"",
             dest, zip, dest);
    return (system(cmd) == 0) ? 0 : -1;
}

/* 拼接包内逻辑库源码（读 vaz.json 清单 logic 数组）；返回 malloc 缓冲 */
static char *vaz_collect_logic(const char *pkg_dir) {
    char meta_path[1024];
    path_join(meta_path, sizeof(meta_path), pkg_dir, "vaz.json");
    yyjson_doc *md = yyjson_read_file(meta_path, 0, NULL, NULL);
    if (!md) return NULL;
    yyjson_mut_doc *mm = yyjson_doc_mut_copy(md, NULL);
    yyjson_doc_free(md);
    if (!mm) return NULL;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(mm);
    yyjson_mut_val *logic = yyjson_mut_obj_get(root, "逻辑");
    if (!logic || !yyjson_mut_is_arr(logic)) logic = yyjson_mut_obj_get(root, "logic");
    StrB b; strb_init(&b);
    if (logic && yyjson_mut_is_arr(logic)) {
        size_t n = yyjson_mut_arr_size(logic);
        for (size_t i = 0; i < n; i++) {
            yyjson_mut_val *f = yyjson_mut_arr_get(logic, i);
            if (!yyjson_mut_is_str(f)) continue;
            char full[1024];
            path_join(full, sizeof(full), pkg_dir, yyjson_mut_get_str(f));
            size_t len = 0;
            char *content = read_file_all(full, &len);
            if (!content) continue;
            strb_puts(&b, "\n# vaz 逻辑库: ");
            strb_puts(&b, yyjson_mut_get_str(f));
            strb_puts(&b, "\n");
            strb_puts(&b, content);
            strb_puts(&b, "\n");
            free(content);
        }
    }
    yyjson_mut_doc_free(mm);
    if (!b.len) { strb_free(&b); return NULL; }
    return b.buf;   /* NUL 结尾的 malloc 缓冲 */
}

/* 包清单 vaz.json 的"名称"字段是否等于 modname */
static int vaz_name_matches(const char *pkg_dir, const char *modname) {
    char meta[1024];
    path_join(meta, sizeof(meta), pkg_dir, "vaz.json");
    yyjson_doc *d = yyjson_read_file(meta, 0, NULL, NULL);
    if (!d) return 0;
    yyjson_mut_doc *m = yyjson_doc_mut_copy(d, NULL);
    yyjson_doc_free(d);
    if (!m) return 0;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(m);
    yyjson_mut_val *nv = root ? yyjson_mut_obj_get(root, "名称") : NULL;
    const char *name = nv ? yyjson_mut_get_str(nv) : NULL;
    if (!name) {
        nv = root ? yyjson_mut_obj_get(root, "name") : NULL;
        name = nv ? yyjson_mut_get_str(nv) : NULL;
    }
    int ok = name ? (strcmp(name, modname) == 0) : 0;
    yyjson_mut_doc_free(m);
    return ok;
}

/* 在 root/vaz、root/deps 下按清单"名称"遍历匹配（目录名可英文，导入名中文） */
static int vaz_locate_by_name(const char *root, const char *modname,
                              char *out_pkgdir, size_t pkgdir_sz) {
    static const char *subs[] = { "vaz", "deps" };
    for (int s = 0; s < 2; s++) {
        char vd[1024];
        snprintf(vd, sizeof(vd), "%s/%s", root, subs[s]);
        DIR *dp = opendir(vd);
        if (!dp) continue;
        struct dirent *de;
        while ((de = readdir(dp))) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            char cand[1024];
            snprintf(cand, sizeof(cand), "%s/%s", vd, de->d_name);
            struct stat st;
            if (stat(cand, &st) == 0 && S_ISDIR(st.st_mode) && vaz_name_matches(cand, modname)) {
                snprintf(out_pkgdir, pkgdir_sz, "%s", cand);
                closedir(dp);
                return 1;
            }
        }
        closedir(dp);
    }
    return 0;
}

/* 在 root 下定位包：目录直接可用；zip 则解压缓存到 root/.vus/vaz-cache/<名>。
 * 命中填 out_pkgdir 并返回 1，未命中返回 0。 */
static int vaz_locate(const char *modname, const char *root,
                      char *out_pkgdir, size_t pkgdir_sz) {
    char mb[128];
    snprintf(mb, sizeof(mb), "%s", modname);
    size_t ml = strlen(mb);
    if (ml > 4 && strcmp(mb + ml - 4, ".vaz") == 0) mb[ml - 4] = '\0';
    if (mb[0] == '\0') return 0;

    struct stat st;
    char cand[1024];
    /* 目录形式：root/vaz/<名>、root/deps/<名>、root/<名> */
    snprintf(cand, sizeof(cand), "%s/vaz/%s", root, mb);
    if (vaz_dir_is_pkg(cand)) { snprintf(out_pkgdir, pkgdir_sz, "%s", cand); return 1; }
    snprintf(cand, sizeof(cand), "%s/deps/%s", root, mb);
    if (vaz_dir_is_pkg(cand)) { snprintf(out_pkgdir, pkgdir_sz, "%s", cand); return 1; }
    snprintf(cand, sizeof(cand), "%s/%s", root, mb);
    if (vaz_dir_is_pkg(cand)) { snprintf(out_pkgdir, pkgdir_sz, "%s", cand); return 1; }

    /* zip 形式：root/vaz/<名>.vaz、root/deps/<名>.vaz、root/<名>.vaz */
    char cache[1024];
    snprintf(cache, sizeof(cache), "%s/.vus/vaz-cache/%s", root, mb);
    snprintf(cand, sizeof(cand), "%s/vaz/%s.vaz", root, mb);
    if (stat(cand, &st) == 0 && S_ISREG(st.st_mode)) {
        if (vaz_unpack_into(cand, cache) == 0 && vaz_dir_is_pkg(cache)) {
            snprintf(out_pkgdir, pkgdir_sz, "%s", cache);
            return 1;
        }
        return 0;
    }
    snprintf(cand, sizeof(cand), "%s/deps/%s.vaz", root, mb);
    if (stat(cand, &st) == 0 && S_ISREG(st.st_mode)) {
        if (vaz_unpack_into(cand, cache) == 0 && vaz_dir_is_pkg(cache)) {
            snprintf(out_pkgdir, pkgdir_sz, "%s", cache);
            return 1;
        }
        return 0;
    }
    snprintf(cand, sizeof(cand), "%s/%s.vaz", root, mb);
    if (stat(cand, &st) == 0 && S_ISREG(st.st_mode)) {
        if (vaz_unpack_into(cand, cache) == 0 && vaz_dir_is_pkg(cache)) {
            snprintf(out_pkgdir, pkgdir_sz, "%s", cache);
            return 1;
        }
        return 0;
    }

    /* 按清单"名称"匹配（目录名可英文，导入名中文） */
    return vaz_locate_by_name(root, mb, out_pkgdir, pkgdir_sz);
}

int vus_vaz_import(const char *modname, const char *base_dir,
                   char **out_src, size_t *out_len,
                   char *out_libdir, size_t libdir_sz) {
    if (!modname || !out_src) return -1;
    *out_src = NULL;
    if (out_len) *out_len = 0;
    if (out_libdir) out_libdir[0] = '\0';

    /* 候选根：主脚本目录、当前工作目录、VUS_VAZ_PATH 各项、$HOME/.vus/vaz */
    char *roots[16];
    int rc = 0;
    char wd[1024], home_dir[1024];
    char path_copy[4096];
    const char *base = (base_dir && base_dir[0] && strcmp(base_dir, ".") != 0) ? base_dir : NULL;
    if (base) {
        char *bd = strdup(base);
        if (bd) { roots[rc++] = bd; }
    }
    if (getcwd(wd, sizeof(wd))) {
        char *c = strdup(wd);
        if (c) roots[rc++] = c;
    }
    const char *env_path = getenv("VUS_VAZ_PATH");
    if (env_path && env_path[0]) {
        snprintf(path_copy, sizeof(path_copy), "%s", env_path);
        char *save = NULL;
        char *tok = strtok_r(path_copy, ":", &save);
        while (tok && rc < 16) {
            if (tok[0]) {
                char *t = strdup(tok);
                if (t) roots[rc++] = t;
            }
            tok = strtok_r(NULL, ":", &save);
        }
    }
    const char *home = getenv("HOME");
    if (home) {
        snprintf(home_dir, sizeof(home_dir), "%s/.vus/vaz", home);
        char *c = strdup(home_dir);
        if (c) roots[rc++] = c;
    }

    char libdir[1024];
    libdir[0] = '\0';
    char *src = NULL;
    for (int i = 0; i < rc; i++) {
        if (!roots[i]) continue;
        if (vaz_locate(modname, roots[i], libdir, sizeof(libdir))) {
            src = vaz_collect_logic(libdir);
            if (src) break;
            libdir[0] = '\0';
        }
    }
    for (int i = 0; i < rc; i++) free(roots[i]);

    if (!src) return 1;   /* 未找到（保留 import 原行的宽容行为） */
    *out_src = src;
    if (out_len) *out_len = strlen(src);
    if (out_libdir && libdir_sz > 0) snprintf(out_libdir, libdir_sz, "%s", libdir);
    return 0;
}

/* ---------------- 主入口 ---------------- */

int vus_vaz_expand(const char *vaz_path, const char *pages_dir,
                   const char *out_logic, char *err, size_t errsz) {
    /* 1. 定位扩展包（解包或直接目录） */
    char *dir = vaz_prepare(vaz_path, err, errsz);
    if (!dir) return -1;

    /* 2. 读清单 vaz.json */
    char meta_path[1024];
    path_join(meta_path, sizeof(meta_path), dir, "vaz.json");
    yyjson_doc *meta_doc = yyjson_read_file(meta_path, 0, NULL, NULL);
    if (!meta_doc) {
        snprintf(err, errsz, "扩展包缺少 vaz.json 清单");
        free(dir);
        return -1;
    }
    yyjson_mut_doc *meta = yyjson_doc_mut_copy(meta_doc, NULL);
    yyjson_doc_free(meta_doc);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(meta);

    /* 3. 注册控件模板 */
    VazCtrl ctrls[VAZ_MAX_CTRL];
    memset(ctrls, 0, sizeof(ctrls));
    int ctrl_count = 0;

    yyjson_mut_val *ctrl_arr = yyjson_mut_obj_get(root, "控件");
    if (!ctrl_arr || !yyjson_mut_is_arr(ctrl_arr)) ctrl_arr = yyjson_mut_obj_get(root, "controls");
    if (ctrl_arr && yyjson_mut_is_arr(ctrl_arr)) {
        size_t n = yyjson_mut_arr_size(ctrl_arr);
        for (size_t i = 0; i < n; i++) {
            yyjson_mut_val *f = yyjson_mut_arr_get(ctrl_arr, i);
            if (!yyjson_mut_is_str(f)) continue;
            const char *rel = yyjson_mut_get_str(f);
            char full[1024];
            path_join(full, sizeof(full), dir, rel);
            yyjson_doc *cdoc_i = yyjson_read_file(full, 0, NULL, NULL);
            if (!cdoc_i) {
                fprintf(stderr, "vaz: 控件文件解析失败: %s\n", full);
                continue;
            }
            yyjson_mut_doc *mdoc_i = yyjson_doc_mut_copy(cdoc_i, NULL);
            yyjson_doc_free(cdoc_i);
            if (!mdoc_i) continue;
            if (vaz_register_ctrl(ctrls, &ctrl_count, mdoc_i, full) != 0) {
                fprintf(stderr, "vaz: 控件注册失败: %s\n", full);
                yyjson_mut_doc_free(mdoc_i);
                continue;
            }
        }
    }

    /* 4. 展开页面目录下所有 *.vua */
    if (ctrl_count > 0) {
        printf("vaz: 加载控件模板 %d 个\n", ctrl_count);
        DIR *dp = opendir(pages_dir);
        if (dp) {
            struct dirent *de;
            while ((de = readdir(dp))) {
                const char *name = de->d_name;
                size_t nl = strlen(name);
                if (nl < 4 || strcmp(name + nl - 4, ".vua") != 0) continue;
                char full[1024];
                path_join(full, sizeof(full), pages_dir, name);
                char info[256];
                vaz_expand_file(ctrls, ctrl_count, full, info, sizeof(info));
                printf("%s\n", info);
            }
            closedir(dp);
        }
    }

    /* 5. 合并逻辑库 */
    char **logic_files = NULL;
    size_t logic_count = 0;
    yyjson_mut_val *logic_arr = yyjson_mut_obj_get(root, "逻辑");
    if (!logic_arr || !yyjson_mut_is_arr(logic_arr)) logic_arr = yyjson_mut_obj_get(root, "logic");
    if (logic_arr && yyjson_mut_is_arr(logic_arr)) {
        size_t n = yyjson_mut_arr_size(logic_arr);
        if (n > 0) {
            logic_files = calloc(n, sizeof(char*));
            for (size_t i = 0; i < n; i++) {
                yyjson_mut_val *f = yyjson_mut_arr_get(logic_arr, i);
                if (yyjson_mut_is_str(f)) {
                    logic_files[i] = strdup(yyjson_mut_get_str(f));
                    logic_count++;
                }
            }
            vaz_merge_logic(dir, logic_files, (int)logic_count, out_logic, err, errsz);
            printf("vaz: 逻辑库 %d 个 -> %s\n", (int)logic_count,
                   out_logic ? out_logic : "(未指定输出)");
            for (size_t i = 0; i < n; i++) free(logic_files[i]);
            free(logic_files);
        }
    }

    /* 6. 释放 */
    yyjson_mut_doc_free(meta);
    int is_tmp = strncmp(dir, "/tmp/vus_vaz_", 13) == 0;
    if (is_tmp) {
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
        if (system(cmd) != 0) { /* 清理失败不影响结果 */ }
    }
    free(dir);
    return 0;
}