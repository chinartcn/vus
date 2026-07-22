/*
 * token.h — VUS 词法单元类型定义
 *
 * 定义 TokenType 枚举和 Token 结构体，是词法分析器和语法分析器的共享接口。
 * 所有关键字（英文、中文别名）都在此集中定义。
 */

#ifndef VUS_TOKEN_H
#define VUS_TOKEN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============ Token 类型枚举 ============ */
typedef enum {
    /* 标识符与字面量 */
    VUS_TOKEN_IDENTIFIER,
    VUS_TOKEN_STRING,
    VUS_TOKEN_NUMBER,

    /* 关键字 — 函数风格英文 */
    VUS_TOKEN_DEF,
    VUS_TOKEN_IF,
    VUS_TOKEN_ELIF,
    VUS_TOKEN_ELSE,
    VUS_TOKEN_FOR,
    VUS_TOKEN_WHILE,
    VUS_TOKEN_RETURN,
    VUS_TOKEN_IMPORT,
    VUS_TOKEN_FROM,
    VUS_TOKEN_TRUE,
    VUS_TOKEN_FALSE,
    VUS_TOKEN_NULL,
    VUS_TOKEN_AND,
    VUS_TOKEN_OR,
    VUS_TOKEN_NOT,
    VUS_TOKEN_TRY,
    VUS_TOKEN_EXCEPT,
    VUS_TOKEN_GLOBAL,
    VUS_TOKEN_BREAK,
    VUS_TOKEN_CONTINUE,
    VUS_TOKEN_THROW,
    VUS_TOKEN_IN,

    /* 关键字 — 函数风格中文别名 */
    VUS_TOKEN_CN_DEF,
    VUS_TOKEN_CN_IF,
    VUS_TOKEN_CN_ELIF,
    VUS_TOKEN_CN_ELSE,
    VUS_TOKEN_CN_FOR,
    VUS_TOKEN_CN_WHILE,
    VUS_TOKEN_CN_RETURN,
    VUS_TOKEN_CN_IMPORT,
    VUS_TOKEN_CN_FROM,
    VUS_TOKEN_CN_TO,
    VUS_TOKEN_CN_TRUE,
    VUS_TOKEN_CN_FALSE,
    VUS_TOKEN_CN_NULL,
    VUS_TOKEN_CN_AND,
    VUS_TOKEN_CN_OR,
    VUS_TOKEN_CN_NOT,
    VUS_TOKEN_CN_TRY,
    VUS_TOKEN_CN_EXCEPT,
    VUS_TOKEN_CN_GLOBAL,
    VUS_TOKEN_CN_BREAK,
    VUS_TOKEN_CN_CONTINUE,
    VUS_TOKEN_CN_THROW,
    VUS_TOKEN_CN_IN,

    /* 结构体关键字 */
    VUS_TOKEN_STRUCT,
    VUS_TOKEN_CN_STRUCT,

    /* 线程关键字 */
    VUS_TOKEN_CN_THREAD,
    VUS_TOKEN_CN_JOIN_THREAD,
    VUS_TOKEN_CN_THREAD_SLEEP,

    /* 协程关键字 */
    VUS_TOKEN_CN_COROUTINE,
    VUS_TOKEN_CN_RESUME,
    VUS_TOKEN_CN_YIELD,
    VUS_TOKEN_CN_AWAIT,

    /* 类型注解关键字 */
    VUS_TOKEN_TYPE_INT,
    VUS_TOKEN_TYPE_FLOAT,
    VUS_TOKEN_TYPE_STR,
    VUS_TOKEN_TYPE_BOOL,
    VUS_TOKEN_TYPE_LIST,
    VUS_TOKEN_TYPE_DICT,

    /* 分隔符 */
    VUS_TOKEN_LPAREN,
    VUS_TOKEN_RPAREN,
    VUS_TOKEN_LBRACKET,
    VUS_TOKEN_RBRACKET,
    VUS_TOKEN_LBRACE,
    VUS_TOKEN_RBRACE,
    VUS_TOKEN_COMMA,
    VUS_TOKEN_COLON,
    VUS_TOKEN_DOT,

    /* 运算符 */
    VUS_TOKEN_PLUS,
    VUS_TOKEN_MINUS,
    VUS_TOKEN_STAR,
    VUS_TOKEN_SLASH,
    VUS_TOKEN_PERCENT,
    VUS_TOKEN_CONCAT,
    VUS_TOKEN_ASSIGN,
    VUS_TOKEN_EQ,
    VUS_TOKEN_NEQ,
    VUS_TOKEN_LT,
    VUS_TOKEN_GT,
    VUS_TOKEN_LE,
    VUS_TOKEN_GE,
    VUS_TOKEN_BIT_AND,
    VUS_TOKEN_BIT_OR,
    VUS_TOKEN_BIT_XOR,
    VUS_TOKEN_SHL,
    VUS_TOKEN_SHR,

    /* 特殊 */
    VUS_TOKEN_NEWLINE,
    VUS_TOKEN_INDENT,
    VUS_TOKEN_DEDENT,
    VUS_TOKEN_EOF,
    VUS_TOKEN_ERROR,
} VusTokenType;

/* ============ Token 结构体 ============ */
typedef struct {
    VusTokenType type;
    const char  *start;       /* 指向源码中的起始位置 */
    size_t       length;      /* Token 文本长度 */
    int          line;        /* 行号（从 1 开始） */
    int          column;      /* 列号（从 1 开始） */
    char        *value;       /* 动态分配的字符串值（字面量时使用） */
} VusToken;

/* ============ Token 操作函数 ============ */

/* 创建 Token（栈上使用，value 不拷贝） */
VusToken vus_token_make(VusTokenType type, const char *start,
                        size_t len, int line, int col);

/* 创建带动态值的 Token（value 会被 strdup） */
VusToken vus_token_make_value(VusTokenType type, const char *value,
                              int line, int col);

/* 释放 Token 中动态分配的资源 */
void vus_token_free(VusToken *tok);

/* 获取 Token 类型的中文名称（用于报错） */
const char *vus_token_type_name(VusTokenType type);

/* ============ 关键字查找表 ============ */

/* 根据字符串查找关键字类型，返回 VUS_TOKEN_IDENTIFIER 表示不是关键字 */
VusTokenType vus_keyword_lookup(const char *start, size_t len);

/* 解码 UTF-8 序列，返回 Unicode 码点 */
int vus_utf8_decode(const char *s, int *bytes_consumed);

/* 判断字符是否为中文标识符允许的字符 */
int vus_is_chinese_id_char(int c);

/* 判断字符是否为标识符起始字符 */
int vus_is_ident_start(int c);

/* 判断字符是否为标识符延续字符 */
int vus_is_ident_continue(int c);

#ifdef __cplusplus
}
#endif

#endif /* VUS_TOKEN_H */