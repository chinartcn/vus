/*
 * vus_vusx.c — VUS .vusx 插件系统实现
 *
 * 实现 .vusx 插件的解析、编译、链接。
 * .vusx 插件是用 VUS 编写的功能扩展，在编译时由编译器自动
 * 解析并编译为 .o 文件，然后链接到最终可执行文件中。
 */

#define _POSIX_C_SOURCE 200809L

#include "vus_vusx.h"
#include "vus_lang.h"
#include "lexer.h"
#include "parser.h"
#include "generator.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ===================================================================
 * JSON 解析（简易版，仅用于 .vusx）
 * =================================================================== */

/* 在 JSON 中查找指定 key 的字符串值 */
static int json_find_string(const char *json, const char *key,
                            char *out, size_t out_size) {
    if (!json || !key || !out || out_size == 0) return -1;

    /* 查找 "key":  */
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);

    const char *p = strstr(json, search);
    if (!p) return -1;

    p += strlen(search);
    /* 跳过空白和 : */
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    if (*p != ':') return -1;
    p++;
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;

    /* 期望字符串值 */
    if (*p != '"') return -1;
    p++;

    size_t o = 0;
    while (*p && *p != '"' && o < out_size - 1) {
        if (*p == '\\' && *(p+1)) {
            p++;
            switch (*p) {
                case 'n': out[o++] = '\n'; break;
                case 'r': out[o++] = '\r'; break;
                case 't': out[o++] = '\t'; break;
                case '\\': out[o++] = '\\'; break;
                case '"': out[o++] = '"'; break;
                default: out[o++] = *p; break;
            }
        } else {
            out[o++] = *p;
        }
        p++;
    }
    out[o] = '\0';
    return (int)o;
}

/* 在 JSON 中查找字符串数组值（如 "导出": ["a", "b"]） */
static int json_find_string_array(const char *json, const char *key,
                                  char out[][VUS_VUSX_NAME_LEN], int max_count) {
    if (!json || !key || !out || max_count <= 0) return 0;

    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);

    const char *p = strstr(json, search);
    if (!p) return 0;

    p += strlen(search);
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    if (*p != ':') return 0;
    p++;
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    if (*p != '[') return 0;
    p++;

    int count = 0;
    while (*p && *p != ']' && count < max_count) {
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',')) p++;
        if (*p == '"') {
            p++;
            size_t o = 0;
            while (*p && *p != '"' && o < VUS_VUSX_NAME_LEN - 1) {
                if (*p == '\\' && *(p+1)) {
                    p++;
                    switch (*p) {
                        case 'n': out[count][o++] = '\n'; break;
                        case 'r': out[count][o++] = '\r'; break;
                        case 't': out[count][o++] = '\t'; break;
                        case '\\': out[count][o++] = '\\'; break;
                        case '"': out[count][o++] = '"'; break;
                        default: out[count][o++] = *p; break;
                    }
                } else {
                    out[count][o++] = *p;
                }
                p++;
            }
            out[count][o] = '\0';
            if (*p == '"') p++;
            count++;
        } else if (*p == ']') {
            break;
        } else {
            p++;
        }
    }
    return count;
}

/* 读取文件全部内容 */
static char *read_file(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    if (len < 0) { fclose(fp); return NULL; }
    rewind(fp);

    char *buf = (char*)malloc((size_t)len + 1);
    if (!buf) { fclose(fp); return NULL; }

    size_t n = fread(buf, 1, (size_t)len, fp);
    fclose(fp);
    buf[n] = '\0';
    if (out_len) *out_len = n;
    return buf;
}

/* ===================================================================
 * 解析
 * =================================================================== */

int vus_vusx_resolve(const char *path, VusVusxPlugin *plugin) {
    if (!path || !plugin) return -1;
    memset(plugin, 0, sizeof(*plugin));

    /* 检查目录是否存在 */
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;

    /* 保存目录路径 */
    strncpy(plugin->dir, path, sizeof(plugin->dir) - 1);
    plugin->dir[sizeof(plugin->dir) - 1] = '\0';

    /* 读取 vusx.json */
    char meta_path[VUS_VUSX_PATH_LEN];
    snprintf(meta_path, sizeof(meta_path), "%s/vusx.json", path);

    char *meta_json = read_file(meta_path, NULL);
    if (!meta_json) return -2;

    /* 解析元数据 */
    json_find_string(meta_json, "名称", plugin->name, sizeof(plugin->name));
    json_find_string(meta_json, "版本", plugin->version, sizeof(plugin->version));

    /* 如果没有名称，使用目录名 */
    if (plugin->name[0] == '\0') {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        strncpy(plugin->name, base, sizeof(plugin->name) - 1);
        /* 去掉 .vusx 后缀 */
        char *dot = strrchr(plugin->name, '.');
        if (dot) *dot = '\0';
    }

    /* 解析入口文件 */
    char entry[VUS_VUSX_PATH_LEN] = "main.vus";
    json_find_string(meta_json, "入口", entry, sizeof(entry));
    snprintf(plugin->main_vus, sizeof(plugin->main_vus), "%s/%s", path, entry);

    /* 解析导出函数 */
    plugin->export_count = json_find_string_array(meta_json, "导出",
                                                   plugin->exports,
                                                   VUS_MAX_VUSX_EXPORTS);

    free(meta_json);

    /* 检查 main.vus 是否存在 */
    if (stat(plugin->main_vus, &st) != 0) {
        fprintf(stderr, "vus_vusx: main.vus not found in '%s'\n", path);
        return -1;
    }

    return 0;
}

/* ===================================================================
 * 编译
 * =================================================================== */

int vus_vusx_compile(VusVusxPlugin *plugin, VusConfig *config) {
    if (!plugin || !plugin->main_vus[0]) return -1;

    /* 确定输出路径 */
    char build_dir[1024];
    if (config->build_dir[0]) {
        strncpy(build_dir, config->build_dir, sizeof(build_dir) - 1);
    } else {
        snprintf(build_dir, sizeof(build_dir), "%s/构建", config->project_dir);
    }
    build_dir[sizeof(build_dir) - 1] = '\0';

    /* 确保构建目录存在 */
    struct stat st;
    if (stat(build_dir, &st) != 0) {
        mkdir(build_dir, 0755);
    }

    /* C 输出路径：构建/<名称>.c */
    snprintf(plugin->c_output, sizeof(plugin->c_output),
             "%s/%s.c", build_dir, plugin->name);

    /* .o 输出路径：构建/<名称>.o */
    snprintf(plugin->obj_output, sizeof(plugin->obj_output),
             "%s/%s.o", build_dir, plugin->name);

    /* 1. 读取 VUS 源码 */
    size_t source_len = 0;
    char *source = read_file(plugin->main_vus, &source_len);
    if (!source) {
        fprintf(stderr, "vus_vusx: failed to read '%s'\n", plugin->main_vus);
        return -1;
    }

    /* 2. 语言插件预处理 */
    char *preprocessed = NULL;
    if (config->language_plugin[0] != '\0') {
        if (vus_lang_preprocess(config->language_plugin, source, &preprocessed) == 0 && preprocessed) {
            free(source);
            source = preprocessed;
            source_len = strlen(source);
        }
    }

    /* 3. 词法分析 */
    VusLexer *lexer = vus_lexer_new(source, source_len);
    if (!lexer) {
        free(source);
        return -1;
    }

    size_t token_count = 0;
    VusToken *tokens = vus_lexer_tokenize(lexer, &token_count);
    if (lexer->error) {
        fprintf(stderr, "vus_vusx: lexer error in '%s': %s\n",
                plugin->main_vus, vus_lexer_error(lexer));
        vus_lexer_free_tokens(tokens, token_count);
        vus_lexer_free(lexer);
        free(source);
        return -1;
    }

    tokens = vus_lexer_steal_tokens(lexer, &token_count);
    vus_lexer_free(lexer);
    /* 注意：source 不能在此释放——标识符 Token 的 start 指向 source 内存，
     * parser 在 AST 生成期间仍会读取这些指针。与 vus_compile_to_c 保持一致，
     * 待 parse 完成、AST 生成后再释放 source。 */

    /* 4. 语法分析 */
    VusParser *parser = vus_parser_new(tokens, token_count);
    if (!parser) {
        vus_lexer_free_tokens(tokens, token_count);
        free(source);
        return -1;
    }

    VusAstProgram *program = vus_parser_parse(parser);
    if (parser->error) {
        fprintf(stderr, "vus_vusx: parser error in '%s': %s\n",
                plugin->main_vus, vus_parser_error(parser));
        vus_parser_free(parser);
        vus_lexer_free_tokens(tokens, token_count);
        free(source);
        return -1;
    }

    vus_parser_free(parser);
    vus_lexer_free_tokens(tokens, token_count);

    /* 5. 代码生成（库式：不生成 main，插件 .o 链接进宿主程序） */
    VusConfig gen_config = *config;
    gen_config.omit_main = 1;
    char *c_code = vus_generate_c(program, &gen_config, plugin->main_vus);
    if (!c_code) {
        fprintf(stderr, "vus_vusx: code generation failed for '%s'\n", plugin->main_vus);
        vus_ast_node_free((VusAstNode *)program);
        free(source);
        return -1;
    }

    vus_ast_node_free((VusAstNode *)program);
    free(source);

    /* 6. 写入 C 文件 */
    FILE *fp = fopen(plugin->c_output, "w");
    if (!fp) {
        fprintf(stderr, "vus_vusx: failed to write '%s'\n", plugin->c_output);
        vus_generate_free(c_code);
        return -1;
    }
    fwrite(c_code, 1, strlen(c_code), fp);
    fclose(fp);
    vus_generate_free(c_code);

    /* 7. 编译为 .o 文件 */
    char abs_rt_dir[2048];
    if (config->rt_dir[0] == '/') {
        strncpy(abs_rt_dir, config->rt_dir, sizeof(abs_rt_dir) - 1);
    } else if (config->project_dir[0]) {
        snprintf(abs_rt_dir, sizeof(abs_rt_dir), "%s/%s", config->project_dir, config->rt_dir);
    } else {
        strncpy(abs_rt_dir, config->rt_dir, sizeof(abs_rt_dir) - 1);
    }
    abs_rt_dir[sizeof(abs_rt_dir) - 1] = '\0';

    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
             "gcc -O2 -g -I\"%s\" -c \"%s\" -o \"%s\" 2>&1",
             abs_rt_dir, plugin->c_output, plugin->obj_output);

    FILE *gcc_fp = popen(cmd, "r");
    if (!gcc_fp) {
        fprintf(stderr, "vus_vusx: failed to compile '%s'\n", plugin->c_output);
        return -1;
    }

    char gcc_output[2048] = "";
    size_t gcc_n = fread(gcc_output, 1, sizeof(gcc_output) - 1, gcc_fp);
    gcc_output[gcc_n] = '\0';
    int status = pclose(gcc_fp);

    if (status != 0) {
        fprintf(stderr, "vus_vusx: GCC compilation of '%s' failed:\n%s\n",
                plugin->c_output, gcc_output);
        return -1;
    }

    return 0;
}

/* ===================================================================
 * 批量操作
 * =================================================================== */

int vus_vusx_resolve_all(VusConfig *config, VusVusxPlugin *plugins, int *count) {
    if (!config || !plugins || !count) return -1;

    /* 解析 vus.json 中的 vusx 依赖列表 */
    /* 注意：vusx 依赖列表存储在 config 中，由 config.c 解析 */
    /* 为了保持简单，这里我们不解析 JSON，而是由调用方传递路径 */

    /* 该函数当前由调用方直接管理，后续可扩展 */
    return 0;
}

int vus_vusx_compile_all(VusVusxPlugin *plugins, int count, VusConfig *config) {
    if (!plugins || count <= 0) return 0;

    int success = 0;
    for (int i = 0; i < count; i++) {
        if (vus_vusx_compile(&plugins[i], config) == 0) {
            success++;
        } else {
            fprintf(stderr, "vus_vusx: failed to compile '%s'\n", plugins[i].name);
        }
    }
    return (success == count) ? 0 : -1;
}

int vus_vusx_append_objects(char *cmd, size_t cmd_size,
                            VusVusxPlugin *plugins, int count) {
    if (!cmd || !plugins) return 0;

    for (int i = 0; i < count; i++) {
        if (plugins[i].obj_output[0]) {
            size_t len = strlen(cmd);
            int n = snprintf(cmd + len, cmd_size - len,
                             " \"%s\"", plugins[i].obj_output);
            if (n < 0 || (size_t)n >= cmd_size - len) return -1;
        }
    }
    return 0;
}

void vus_vusx_cleanup_all(VusVusxPlugin *plugins, int count) {
    if (!plugins) return;
    /* 目前不需要额外清理，临时文件保留在构建目录中 */
    (void)plugins;
    (void)count;
}