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

/* 将 VUS 源码字符串编译为 C 代码，核心编译流水线 */
static VusResult compile_source(const char *source, size_t source_len,
                                VusConfig *config, const char *output_c_path) {
    VusResult result;
    memset(&result, 0, sizeof(result));

    /* 词法分析 */
    VusLexer *lexer = vus_lexer_new(source, source_len, config->style);
    if (!lexer) {
        snprintf(result.error_msg, sizeof(result.error_msg), "Failed to create lexer");
        return result;
    }

    size_t token_count = 0;
    VusToken *tokens = vus_lexer_tokenize(lexer, &token_count);
    if (lexer->error) {
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "Lexer error: %s", vus_lexer_error(lexer));
        vus_lexer_free_tokens(tokens, token_count);
        vus_lexer_free(lexer);
        return result;
    }

    tokens = vus_lexer_steal_tokens(lexer, &token_count);
    vus_lexer_free(lexer);

    /* 语法分析 */
    VusParser *parser = vus_parser_new(tokens, token_count, config->style);
    if (!parser) {
        snprintf(result.error_msg, sizeof(result.error_msg), "Failed to create parser");
        vus_lexer_free_tokens(tokens, token_count);
        return result;
    }

    VusAstProgram *program = vus_parser_parse(parser);
    if (parser->error) {
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "Parser error: %s", vus_parser_error(parser));
        vus_parser_free(parser);
        vus_lexer_free_tokens(tokens, token_count);
        return result;
    }

    vus_parser_free(parser);
    vus_lexer_free_tokens(tokens, token_count);

    /* 代码生成 */
    char *c_code = vus_generate_c(program, config);
    if (!c_code) {
        snprintf(result.error_msg, sizeof(result.error_msg), "Failed to generate C code");
        vus_ast_node_free((VusAstNode *)program);
        return result;
    }

    /* 写入 C 文件 */
    FILE *fp = fopen(output_c_path, "w");
    if (!fp) {
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "Failed to write C file: %s", output_c_path);
        vus_generate_free(c_code);
        vus_ast_node_free((VusAstNode *)program);
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
    return result;
}

/* 从源码编译 C 并链接为可执行文件 */
static VusResult compile_source_to_exe(const char *source, size_t source_len,
                                       VusConfig *config, const char *output_c_path,
                                       const char *output_exe_path) {
    VusResult result = compile_source(source, source_len, config, output_c_path);
    if (!result.success) return result;

    /* 调用 GCC 链接 */
    char error_msg[512];
    int cr = vus_compile_c(result.c_output_path, output_exe_path,
                           config, error_msg, sizeof(error_msg));
    if (cr != 0) {
        result.success = 0;
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "GCC compilation failed: %s", error_msg);
        return result;
    }

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
                 "Invalid argument: path is NULL");
        return result;
    }

    /* 读取源文件 */
    size_t source_len = 0;
    char *source = read_file(path, &source_len);
    if (!source) {
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "Failed to read file: %s", path);
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

    result = compile_source(source, source_len, config, c_output_path);
    free(source);
    return result;
}

/* ============ vus_compile_string ============ */

VusResult vus_compile_string(const char *source, VusConfig *config) {
    VusResult result;
    memset(&result, 0, sizeof(result));

    if (!source) {
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "Invalid argument: source is NULL");
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
    result = compile_source(source, source_len, config, c_output_path);
    return result;
}

/* ============ vus_compile_string_to_exe ============ */

VusResult vus_compile_string_to_exe(const char *source, VusConfig *config) {
    VusResult result;
    memset(&result, 0, sizeof(result));

    if (!source) {
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "Invalid argument: source is NULL");
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
                                   c_output_path, exe_output_path);
    return result;
}

/* ============ vus_eval ============ */

VusResult vus_eval(const char *code, VusConfig *config, char *output) {
    VusResult result;
    memset(&result, 0, sizeof(result));

    if (!code) {
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "Invalid argument: code is NULL");
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
                 "Code too long for vus_eval (max %zu bytes)", sizeof(wrapped) - 50);
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
                                   c_output_path, exe_output_path);
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
                 "Failed to execute compiled program");
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
                 "Program exited with code %d", status);
    }

    return result;
}