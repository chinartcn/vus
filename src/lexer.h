/*
 * lexer.h — VUS 词法分析器接口
 *
 * 将 VUS 源码字符串转换为 Token 流。
 * 支持函数风格（中英别名）和易语言风格（点前缀）。
 * 使用缩进栈处理 INDENT/DEDENT。
 */

#ifndef VUS_LEXER_H
#define VUS_LEXER_H

#include "token.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============ 词法分析器结构体 ============ */
typedef struct {
    const char *source;        /* 源码指针 */
    size_t      source_len;    /* 源码长度 */
    size_t      pos;           /* 当前读取位置 */
    int         line;          /* 当前行号 */
    int         column;        /* 当前列号 */
    const char *style;         /* "函数" 或 "易语言" */

    /* 输出 */
    VusToken   *tokens;        /* Token 数组 */
    size_t      token_count;   /* Token 数量 */
    size_t      token_cap;     /* Token 容量 */

    /* 缩进栈 */
    int        *indent_stack;
    size_t      indent_depth;  /* 栈深度 */
    size_t      indent_cap;    /* 栈容量 */

    /* 状态 */
    int         at_line_start;
    int         error;         /* 是否发生错误 */
    char        error_msg[256];/* 错误信息 */
} VusLexer;

/* ============ 词法分析器操作 ============ */

/* 创建并初始化词法分析器 */
VusLexer *vus_lexer_new(const char *source, size_t source_len, const char *style);

/* 执行词法分析，返回 Token 数组 */
VusToken *vus_lexer_tokenize(VusLexer *lexer, size_t *out_count);

/* 获取 Token 数组所有权（调用方负责释放） */
VusToken *vus_lexer_steal_tokens(VusLexer *lexer, size_t *out_count);

/* 释放 Token 数组及其动态值 */
void vus_lexer_free_tokens(VusToken *tokens, size_t count);

/* 释放词法分析器 */
void vus_lexer_free(VusLexer *lexer);

/* 获取错误信息 */
const char *vus_lexer_error(VusLexer *lexer);

/* 打印 Token 流（调试用） */
void vus_lexer_print_tokens(VusToken *tokens, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* VUS_LEXER_H */