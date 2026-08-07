/*
 * parser.h — VUS 语法分析器接口
 *
 * 将 Token 流解析为 AST（抽象语法树）。
 * 使用递归下降解析算法，支持函数风格（中英别名关键字）。
 */

#ifndef VUS_PARSER_H
#define VUS_PARSER_H

#include "token.h"
#include "ast.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============ 语法分析器结构体 ============ */
typedef struct {
    VusToken    *tokens;
    size_t       token_count;
    size_t       pos;
    int          error;
    char         error_msg[256];
    int          in_function;   /* 1=当前正在解析函数体, 0=全局作用域 */
} VusParser;

/* ============ 语法分析器操作 ============ */

/* 创建语法分析器 */
VusParser *vus_parser_new(VusToken *tokens, size_t count);

/* 执行语法分析，返回 AST 根节点 */
VusAstProgram *vus_parser_parse(VusParser *parser);

/* 释放语法分析器 */
void vus_parser_free(VusParser *parser);

/* 获取错误信息 */
const char *vus_parser_error(VusParser *parser);

/* 打印 AST（调试用） */
void vus_ast_print(VusAstNode *node, int indent);

#ifdef __cplusplus
}
#endif

#endif /* VUS_PARSER_H */