/*
 * vus_abi.c — VUS 编译器 C ABI 接口实现
 *
 * 实现 vus_abi.h 中声明的所有函数。
 * 依赖编译器内部模块（lexer、parser、generator）完成编译流水线。
 */

#define _GNU_SOURCE
#include "../include/vus/vus_abi.h"
#include "generator.h"
#include "parser.h"
#include "lexer.h"
#include "config.h"
#include "vus_lang.h"
#include "vus_vusx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ============ 内部辅助 ============ */

/* 读取文件全部内容 */
static char *read_file(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    if (len < 0) { fclose(fp); return NULL; }
    fseek(fp, 0, SEEK_SET);

    char *data = (char *)malloc((size_t)len + 1);
    if (!data) { fclose(fp); return NULL; }

    size_t nread = fread(data, 1, (size_t)len, fp);
    fclose(fp);
    data[nread] = '\0';

    if (out_len) *out_len = nread;
    return data;
}

/* ============ import 展开（多文件合并编译） ============
 * VUS 的 导入 / 从...导入 目前仅作语法解析，不会把外部模块函数编入主程序，
 * 导致 dlsym（多页导航等回调反查）找不到外部模块符号。这里在词法分析前，
 * 把源码中的 导入 X / 从 X 导入 行就地替换为对应 X.vus 的源码（递归展开其
 * 自身的 import），使外部模块的全部函数随主程序一起编译，从而可被 dlsym
 * 反查，支撑“外部 .vus 页面”等多文件场景。找不到模块时保留原行，不破坏
 * 既有仅作语法验证的脚本。 */
#define VUS_IMPORT_MAX_DEPTH 32
typedef struct {
    char   path[VUS_IMPORT_MAX_DEPTH][512];
    int    count;
} VusImportSet;

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} VusGrow;

static void grow_init(VusGrow *g) { g->buf = NULL; g->len = 0; g->cap = 0; }
static void grow_add(VusGrow *g, const char *s, size_t n) {
    if (g->len + n + 1 > g->cap) {
        size_t nc = g->cap ? g->cap * 2 : 256;
        while (nc < g->len + n + 1) nc *= 2;
        char *nb = (char *)realloc(g->buf, nc);
        if (!nb) return;
        g->buf = nb; g->cap = nc;
    }
    if (n) { memcpy(g->buf + g->len, s, n); g->len += n; }
    g->buf[g->len] = '\0';
}

/* 判断行是否为导入语句；是则解析出模块名（mod/mod_len）并返回 1。 */
static int import_line(const char *line, const char **mod_out, size_t *mod_len) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    /* 整模块导入：导入 X / import X */
    if (strncmp(p, "导入", 6) == 0 && (p[6] == ' ' || p[6] == '\t')) {
        p += 6; while (*p == ' ' || *p == '\t') p++;
        *mod_out = p;
        while (*p && *p != '\n' && *p != '\r' && *p != ' ' && *p != '\t') p++;
        *mod_len = (size_t)(p - *mod_out);
        return 1;
    }
    if (strncmp(p, "import", 6) == 0 && (p[6] == ' ' || p[6] == '\t')) {
        p += 6; while (*p == ' ' || *p == '\t') p++;
        *mod_out = p;
        while (*p && *p != '\n' && *p != '\r' && *p != ' ' && *p != '\t') p++;
        *mod_len = (size_t)(p - *mod_out);
        return 1;
    }
    /* from 导入：从 X 导入 Y / from X import Y */
    if (strncmp(p, "从", 3) == 0 && (p[3] == ' ' || p[3] == '\t')) {
        p += 3; while (*p == ' ' || *p == '\t') p++;
        *mod_out = p;
        while (*p && *p != '\n' && *p != '\r' && *p != ' ' && *p != '\t') p++;
        const char *q = p; while (*q == ' ' || *q == '\t') q++;
        /* 必须有后续「导入」限定符，避免误判其他以「从」开头的普通行 */
        if (strncmp(q, "导入", 6) != 0) return 0;
        *mod_len = (size_t)(p - *mod_out);
        return 1;
    }
    if (strncmp(p, "from", 4) == 0 && (p[4] == ' ' || p[4] == '\t')) {
        p += 4; while (*p == ' ' || *p == '\t') p++;
        *mod_out = p;
        while (*p && *p != '\n' && *p != '\r' && *p != ' ' && *p != '\t') p++;
        const char *q = p; while (*q == ' ' || *q == '\t') q++;
        /* 必须有后续 import 限定符（带词边界），否则诸如 "from xxx" 的普通句子会误判 */
        if (strncmp(q, "import", 6) != 0 ||
            (q[6] != ' ' && q[6] != '\t' && q[6] != '\0' && q[6] != '\n' && q[6] != '\r'))
            return 0;
        *mod_len = (size_t)(p - *mod_out);
        return 1;
    }
    return 0;
}

static int set_has(const VusImportSet *s, const char *key) {
    for (int i = 0; i < s->count; i++) if (strcmp(s->path[i], key) == 0) return 1;
    return 0;
}
static void set_add(VusImportSet *s, const char *key) {
    if (s->count >= VUS_IMPORT_MAX_DEPTH) return;
    if (set_has(s, key)) return;
    snprintf(s->path[s->count], sizeof(s->path[s->count]), "%s", key);
    s->count++;
}

/* 递归展开 content：import 行替换为模块源码，其余行原样保留。 */
static void expand_rec(const char *content, const char *base_dir,
                       VusImportSet *set, VusGrow *out) {
    const char *p = content;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t linelen = nl ? (size_t)(nl - p) : strlen(p);
        char *line = (char *)malloc(linelen + 1);
        if (line) { memcpy(line, p, linelen); line[linelen] = '\0'; }

        const char *mod = NULL; size_t mlen = 0;
        if (line && import_line(line, &mod, &mlen)) {
            char modname[256];
            size_t clen = mlen < sizeof(modname) - 1 ? mlen : sizeof(modname) - 1;
            memcpy(modname, mod, clen); modname[clen] = '\0';

            char file[1024]; int found = 0;
            struct stat st;
            /* 优先主脚本同目录，其次当前工作目录 */
            if (base_dir && base_dir[0] && strcmp(base_dir, ".") != 0) {
                snprintf(file, sizeof(file), "%s/%s.vus", base_dir, modname);
                if (stat(file, &st) == 0 && S_ISREG(st.st_mode)) found = 1;
                if (!found) {
                    snprintf(file, sizeof(file), "%s/%s", base_dir, modname);
                    if (stat(file, &st) == 0 && S_ISREG(st.st_mode)) found = 1;
                }
            }
            if (!found) {
                snprintf(file, sizeof(file), "%s.vus", modname);
                if (stat(file, &st) == 0 && S_ISREG(st.st_mode)) found = 1;
            }

            if (found && !set_has(set, file)) {
                size_t mco_len = 0;
                char *mco = read_file(file, &mco_len);
                if (mco) {
                    set_add(set, file);
                    char mdir[1024];
                    const char *slash = strrchr(file, '/');
                    if (slash) {
                        size_t dl = (size_t)(slash - file);
                        if (dl > 0) snprintf(mdir, sizeof(mdir), "%.*s", (int)dl, file);
                        else strcpy(mdir, ".");
                    } else {
                        strcpy(mdir, ".");
                    }
                    expand_rec(mco, mdir, set, out);
                    free(mco);
                } else {
                    grow_add(out, line, linelen);
                    grow_add(out, "\n", 1);
                }
            } else if (found) {
                /* 已展开过（避免循环 import 重复/无限） */
            } else {
                grow_add(out, line, linelen);
                grow_add(out, "\n", 1);
            }
        } else {
            if (line) grow_add(out, line, linelen);
            grow_add(out, "\n", 1);
        }
        free(line);
        if (!nl) break;
        p = nl + 1;
    }
}

/* 返回展开后的源码（malloc），失败返回 NULL（此时应沿用原 source）。
 * 供 main.c 的 vus_compile_to_c 等多条编译流水线共用，保证 import 展开
 * 在 `vus run` / `vus build` 等路径上行为一致。 */
char *vus_source_expand_imports(const char *source, const char *source_name) {
    if (!source) return NULL;
    VusImportSet set; set.count = 0;
    char base[1024];
    const char *slash = source_name ? strrchr(source_name, '/') : NULL;
    if (slash) {
        size_t dl = (size_t)(slash - source_name);
        if (dl > 0) snprintf(base, sizeof(base), "%.*s", (int)dl, source_name);
        else strcpy(base, ".");
    } else {
        strcpy(base, ".");
    }
    VusGrow out; grow_init(&out);
    expand_rec(source, base, &set, &out);
    return out.buf; /* 可能为 NULL（无内容） */
}

/* 将 VUS 源码字符串编译为 C 代码，核心编译流水线 */
static VusResult compile_source(const char *source, size_t source_len,
                                VusConfig *config, const char *output_c_path,
                                const char *source_name) {
    VusResult result;
    memset(&result, 0, sizeof(result));

    /* 语言插件预处理：将源码转换为标准 VUS 函数风格 */
    const char *processed_source = source;
    size_t processed_len = source_len;
    char *preprocessed = NULL;

    if (config->language_plugin[0] != '\0') {
        if (vus_lang_preprocess(config->language_plugin, source, &preprocessed) == 0 && preprocessed) {
            processed_source = preprocessed;
            processed_len = strlen(preprocessed);
        }
    }

    /* import 展开：把 导入 X / 从 X 导入 替换为 X.vus 源码（递归），使外部模块
     * 函数随主程序一起编译，支撑“外部 .vus 页面”与多文件回调 dlsym 反查。 */
    {
        char *expanded = vus_source_expand_imports(processed_source, source_name);
        if (expanded) {
            if (preprocessed) free(preprocessed);
            preprocessed = expanded;
            processed_source = preprocessed;
            processed_len = strlen(preprocessed);
        }
    }

    /* 词法分析 */
    VusLexer *lexer = vus_lexer_new(processed_source, processed_len);
    if (!lexer) {
        snprintf(result.error_msg, sizeof(result.error_msg), "文件 %s: 词法分析器创建失败", source_name);
        free(preprocessed);
        return result;
    }

    size_t token_count = 0;
    VusToken *tokens = vus_lexer_tokenize(lexer, &token_count);
    if (lexer->error) {
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "文件 %s: 词法分析错误: %s", source_name, vus_lexer_error(lexer));
        vus_lexer_free_tokens(tokens, token_count);
        vus_lexer_free(lexer);
        free(preprocessed);
        return result;
    }

    tokens = vus_lexer_steal_tokens(lexer, &token_count);
    vus_lexer_free(lexer);

    /* 语法分析 */
    VusParser *parser = vus_parser_new(tokens, token_count);
    if (!parser) {
        snprintf(result.error_msg, sizeof(result.error_msg), "文件 %s: 语法分析器创建失败", source_name);
        vus_lexer_free_tokens(tokens, token_count);
        free(preprocessed);
        return result;
    }

    VusAstProgram *program = vus_parser_parse(parser);
    if (parser->error) {
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "文件 %s: 语法分析错误: %s", source_name, vus_parser_error(parser));
        vus_parser_free(parser);
        vus_lexer_free_tokens(tokens, token_count);
        free(preprocessed);
        return result;
    }

    vus_parser_free(parser);
    vus_lexer_free_tokens(tokens, token_count);

    /* 代码生成 */
    char *c_code = vus_generate_c(program, config);
    if (!c_code) {
        snprintf(result.error_msg, sizeof(result.error_msg), "文件 %s: C 代码生成失败", source_name);
        vus_ast_node_free((VusAstNode *)program);
        free(preprocessed);
        return result;
    }

    /* 写入 C 文件 */
    FILE *fp = fopen(output_c_path, "w");
    if (!fp) {
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "文件 %s: C 文件写入失败: %s", source_name, output_c_path);
        vus_generate_free(c_code);
        vus_ast_node_free((VusAstNode *)program);
        free(preprocessed);
        return result;
    }

    size_t c_len = strlen(c_code);
    fwrite(c_code, 1, c_len, fp);
    fclose(fp);

    /* 填充结果 */
    result.success = 1;
    strncpy(result.c_output_path, output_c_path, sizeof(result.c_output_path) - 1);
    result.c_output_path[sizeof(result.c_output_path) - 1] = '\0';

    vus_generate_free(c_code);
    vus_ast_node_free((VusAstNode *)program);

    free(preprocessed);
    return result;
}

/* 从源码编译 C 并链接为可执行文件 */
static VusResult compile_source_to_exe(const char *source, size_t source_len,
                                       VusConfig *config, const char *output_c_path,
                                       const char *output_exe_path,
                                       const char *source_name) {
    VusResult result = compile_source(source, source_len, config, output_c_path, source_name);
    if (!result.success) return result;

    /* 编译 vusx 依赖 */
    VusVusxPlugin vusx_plugins[VUS_MAX_VUSX_DEPS];
    int vusx_count = 0;
    char extra_objects[4096] = "";

    if (config->vusx_deps_count > 0) {
        memset(vusx_plugins, 0, sizeof(vusx_plugins));
        int resolved = 0;
        for (int i = 0; i < config->vusx_deps_count && resolved < VUS_MAX_VUSX_DEPS; i++) {
            if (vus_vusx_resolve(config->vusx_deps[i], &vusx_plugins[resolved]) == 0) {
                resolved++;
            } else {
                fprintf(stderr, "警告: vusx 依赖 '%s' 解析失败，跳过\n", config->vusx_deps[i]);
            }
        }
        vusx_count = resolved;

        if (vusx_count > 0) {
            if (vus_vusx_compile_all(vusx_plugins, vusx_count, config) != 0) {
                result.success = 0;
                snprintf(result.error_msg, sizeof(result.error_msg),
                         "vusx 依赖编译失败");
                vus_vusx_cleanup_all(vusx_plugins, vusx_count);
                return result;
            }

            /* 构建额外 .o 文件列表 */
            for (int i = 0; i < vusx_count; i++) {
                size_t len = strlen(extra_objects);
                snprintf(extra_objects + len, sizeof(extra_objects) - len,
                         " \"%s\"", vusx_plugins[i].obj_output);
            }
        }
    }

    /* 调用 GCC 链接 */
    char error_msg[512];
    int cr = vus_compile_c(result.c_output_path, output_exe_path,
                           config, error_msg, sizeof(error_msg),
                           extra_objects[0] ? extra_objects : NULL);
    if (cr != 0) {
        result.success = 0;
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "GCC 编译失败: %s", error_msg);
        if (vusx_count > 0) vus_vusx_cleanup_all(vusx_plugins, vusx_count);
        return result;
    }

    if (vusx_count > 0) vus_vusx_cleanup_all(vusx_plugins, vusx_count);

    strncpy(result.exe_output_path, output_exe_path, sizeof(result.exe_output_path) - 1);
    result.exe_output_path[sizeof(result.exe_output_path) - 1] = '\0';
    result.success = 1;
    return result;
}

/* 确保目录存在 */
static void ensure_dir(const char *dir) {
    struct stat st;
    if (stat(dir, &st) != 0) {
        mkdir(dir, 0755);
    }
}

/* 从源字符串生成一个安全的文件名 */
static void source_to_filename(const char *source, char *buf, size_t buf_size) {
    /* 取前 16 个可打印字符，非字母数字转下划线 */
    size_t i = 0, o = 0;
    while (source[i] && o < buf_size - 1) {
        unsigned char c = (unsigned char)source[i];
        if (c >= 0x80) {
            /* 中文字符转 _ */
            if (o + 1 < buf_size) buf[o++] = '_';
            /* 跳过 UTF-8 后续字节 */
            int skip = 1;
            if ((c & 0xE0) == 0xC0) skip = 2;
            else if ((c & 0xF0) == 0xE0) skip = 3;
            else if ((c & 0xF8) == 0xF0) skip = 4;
            i += (size_t)skip;
        } else if (c >= 'a' && c <= 'z') {
            buf[o++] = (char)c;
            i++;
        } else if (c >= 'A' && c <= 'Z') {
            buf[o++] = (char)c;
            i++;
        } else if (c >= '0' && c <= '9') {
            buf[o++] = (char)c;
            i++;
        } else {
            buf[o++] = '_';
            i++;
        }
        if (o >= 16) break;
    }
    buf[o] = '\0';
    if (o == 0) {
        strncpy(buf, "eval", buf_size - 1);
        buf[buf_size - 1] = '\0';
    }
}

/* ============ ABI 版本 ============ */

int vus_abi_version(void) {
    return (VUS_ABI_VERSION_MAJOR << 16)
         | (VUS_ABI_VERSION_MINOR << 8)
         | VUS_ABI_VERSION_PATCH;
}

const char *vus_abi_version_string(void) {
    return "1.0.0";
}

/* ============ vus_compile_file ============ */

VusResult vus_compile_file(const char *path, VusConfig *config) {
    VusResult result;
    memset(&result, 0, sizeof(result));

    if (!path) {
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "参数无效: 路径为空");
        return result;
    }

    /* 读取源文件 */
    size_t source_len = 0;
    char *source = read_file(path, &source_len);
    if (!source) {
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "文件读取失败: %s", path);
        return result;
    }

    /* 使用传入的配置，或默认配置 */
    VusConfig local_config;
    if (!config) {
        memset(&local_config, 0, sizeof(local_config));
        strcpy(local_config.style, "函数");
        strcpy(local_config.project_dir, ".");
        strcpy(local_config.rt_dir, "rt");
        strcpy(local_config.build_dir, "构建");
        strcpy(local_config.optimization, "速度");
        config = &local_config;
    }

    /* 确保构建目录存在 */
    char build_dir[1024];
    if (config->build_dir[0]) {
        strncpy(build_dir, config->build_dir, sizeof(build_dir) - 1);
    } else {
        snprintf(build_dir, sizeof(build_dir), "%s/构建", config->project_dir);
    }
    build_dir[sizeof(build_dir) - 1] = '\0';
    ensure_dir(build_dir);

    /* 从文件名确定输出路径 */
    const char *basename = strrchr(path, '/');
    basename = basename ? basename + 1 : path;
    char name_buf[512];
    strncpy(name_buf, basename, sizeof(name_buf) - 1);
    name_buf[sizeof(name_buf) - 1] = '\0';
    char *dot = strrchr(name_buf, '.');
    if (dot) *dot = '\0';

    char c_output_path[1024];
    snprintf(c_output_path, sizeof(c_output_path), "%s/%s.c", build_dir, name_buf);

    result = compile_source(source, source_len, config, c_output_path, path);
    free(source);
    return result;
}

/* ============ vus_compile_string ============ */

VusResult vus_compile_string(const char *source, VusConfig *config) {
    VusResult result;
    memset(&result, 0, sizeof(result));

    if (!source) {
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "参数无效: 源码为空");
        return result;
    }

    VusConfig local_config;
    if (!config) {
        memset(&local_config, 0, sizeof(local_config));
        strcpy(local_config.style, "函数");
        strcpy(local_config.project_dir, ".");
        strcpy(local_config.rt_dir, "rt");
        strcpy(local_config.build_dir, "构建");
        strcpy(local_config.optimization, "速度");
        config = &local_config;
    }

    /* 确保构建目录存在 */
    char build_dir[1024];
    if (config->build_dir[0]) {
        strncpy(build_dir, config->build_dir, sizeof(build_dir) - 1);
    } else {
        snprintf(build_dir, sizeof(build_dir), "%s/构建", config->project_dir);
    }
    build_dir[sizeof(build_dir) - 1] = '\0';
    ensure_dir(build_dir);

    /* 从源码生成文件名 */
    char name_buf[64];
    source_to_filename(source, name_buf, sizeof(name_buf));

    char c_output_path[1024];
    snprintf(c_output_path, sizeof(c_output_path), "%s/%s.c", build_dir, name_buf);

    size_t source_len = strlen(source);
    result = compile_source(source, source_len, config, c_output_path, name_buf);
    return result;
}

/* ============ vus_compile_string_to_exe ============ */

VusResult vus_compile_string_to_exe(const char *source, VusConfig *config) {
    VusResult result;
    memset(&result, 0, sizeof(result));

    if (!source) {
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "参数无效: 源码为空");
        return result;
    }

    VusConfig local_config;
    if (!config) {
        memset(&local_config, 0, sizeof(local_config));
        strcpy(local_config.style, "函数");
        strcpy(local_config.project_dir, ".");
        strcpy(local_config.rt_dir, "rt");
        strcpy(local_config.build_dir, "构建");
        strcpy(local_config.optimization, "速度");
        config = &local_config;
    }

    char build_dir[1024];
    if (config->build_dir[0]) {
        strncpy(build_dir, config->build_dir, sizeof(build_dir) - 1);
    } else {
        snprintf(build_dir, sizeof(build_dir), "%s/构建", config->project_dir);
    }
    build_dir[sizeof(build_dir) - 1] = '\0';
    ensure_dir(build_dir);

    char name_buf[64];
    source_to_filename(source, name_buf, sizeof(name_buf));

    char c_output_path[1024];
    char exe_output_path[1024];
    snprintf(c_output_path, sizeof(c_output_path), "%s/%s.c", build_dir, name_buf);
    snprintf(exe_output_path, sizeof(exe_output_path), "%s/%s", build_dir, name_buf);

    size_t source_len = strlen(source);
    result = compile_source_to_exe(source, source_len, config,
                                   c_output_path, exe_output_path, name_buf);
    return result;
}

/* ============ vus_eval ============ */

VusResult vus_eval(const char *code, VusConfig *config, char *output) {
    VusResult result;
    memset(&result, 0, sizeof(result));

    if (!code) {
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "参数无效: 代码为空");
        if (output) output[0] = '\0';
        return result;
    }

    /* 将代码片段包装为完整 VUS 程序 */
    char wrapped[16384];
    int n = snprintf(wrapped, sizeof(wrapped),
        "#// vus_eval 自动生成的包装程序\n"
        "打印(%s)\n",
        code);

    if (n >= (int)sizeof(wrapped)) {
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "代码过长，vus_eval 最多支持 %zu 字节", sizeof(wrapped) - 50);
        if (output) output[0] = '\0';
        return result;
    }

    /* 编译为可执行文件 */
    VusConfig local_config;
    if (!config) {
        memset(&local_config, 0, sizeof(local_config));
        strcpy(local_config.style, "函数");
        strcpy(local_config.project_dir, ".");
        strcpy(local_config.rt_dir, "rt");
        strcpy(local_config.build_dir, "构建");
        strcpy(local_config.optimization, "速度");
        config = &local_config;
    }

    char build_dir[1024];
    if (config->build_dir[0]) {
        strncpy(build_dir, config->build_dir, sizeof(build_dir) - 1);
    } else {
        snprintf(build_dir, sizeof(build_dir), "%s/构建", config->project_dir);
    }
    build_dir[sizeof(build_dir) - 1] = '\0';
    ensure_dir(build_dir);

    char name_buf[64];
    source_to_filename(code, name_buf, sizeof(name_buf));

    char c_output_path[1024];
    char exe_output_path[1024];
    snprintf(c_output_path, sizeof(c_output_path), "%s/_eval_%s.c", build_dir, name_buf);
    snprintf(exe_output_path, sizeof(exe_output_path), "%s/_eval_%s", build_dir, name_buf);

    size_t source_len = strlen(wrapped);
    result = compile_source_to_exe(wrapped, source_len, config,
                                   c_output_path, exe_output_path, name_buf);
    if (!result.success) {
        if (output) output[0] = '\0';
        return result;
    }

    /* 运行可执行文件并捕获 stdout */
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "%s 2>/dev/null", exe_output_path);

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        result.success = 0;
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "无法执行编译后的程序");
        if (output) output[0] = '\0';
        return result;
    }

    size_t total = 0;
    char buf[256];
    while (fgets(buf, sizeof(buf), fp) && total < 4095) {
        size_t blen = strlen(buf);
        if (total + blen > 4095) blen = 4095 - total;
        memcpy(output + total, buf, blen);
        total += blen;
    }
    output[total] = '\0';

    int status = pclose(fp);
    result.success = (status == 0) ? 1 : 0;

    if (status != 0 && !result.error_msg[0]) {
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "程序退出，返回码 %d", status);
    }

    return result;
}