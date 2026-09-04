/*
 * generator.h — VUS C 代码生成器接口
 *
 * 将 AST 转换为合法的 C 源码，并可选调用 GCC 编译为可执行文件。
 */

#ifndef VUS_GENERATOR_H
#define VUS_GENERATOR_H

#include "ast.h"
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============ 内部缓冲区类型（跨模块可见） ============ */

typedef struct {
    char   *data;
    size_t  len;
    size_t  cap;
    int     indent;
} GenBuf;

/* ============ 代码生成器 ============ */

/* 将 AST 生成 C 代码，返回分配的字符串 */
char *vus_generate_c(VusAstProgram *program, VusConfig *config);

/* 将 C 代码编译为可执行文件（调用 GCC）
 * extra_objects — 额外 .o 文件列表（空格分隔），可为 NULL */
int vus_compile_c(const char *c_source_path, const char *output_path,
                  VusConfig *config, char *error_msg, size_t error_size,
                  const char *extra_objects);

/* 释放生成的 C 代码字符串 */
void vus_generate_free(char *code);

/* ============ 表达式生成（跨模块，供 gen_builtin_*.c 调用） ============ */

/* 返回 malloc 分配的字符串，调用方需 free */
char *gen_expr(GenBuf *buf, VusAstNode *node);

/* 将中文/特殊字符名转为合法的 C 标识符 */
void gen_sanitize_name(const char *name, char *out, size_t out_size);

/* 字符串转义 */
void gen_string_escape(const char *input, char *output, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* VUS_GENERATOR_H */