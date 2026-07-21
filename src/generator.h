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

/* ============ 代码生成器 ============ */

/* 将 AST 生成 C 代码，返回分配的字符串 */
char *vus_generate_c(VusAstProgram *program, VusConfig *config);

/* 将 C 代码编译为可执行文件（调用 GCC） */
int vus_compile_c(const char *c_source_path, const char *output_path,
                  VusConfig *config, char *error_msg, size_t error_size);

/* 释放生成的 C 代码字符串 */
void vus_generate_free(char *code);

#ifdef __cplusplus
}
#endif

#endif /* VUS_GENERATOR_H */