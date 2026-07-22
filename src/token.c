/*
 * token.c — VUS Token 操作实现
 *
 * 实现 Token 创建/释放、关键字查找、字符分类函数。
 * 关键字查找表覆盖英文关键字、中文别名和类型关键字。
 */

#define _POSIX_C_SOURCE 200809L

#include "token.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ============ Token 操作函数 ============ */

VusToken vus_token_make(VusTokenType type, const char *start,
                        size_t len, int line, int col)
{
    VusToken tok;
    tok.type   = type;
    tok.start  = start;
    tok.length = len;
    tok.line   = line;
    tok.column = col;
    tok.value  = NULL;
    return tok;
}

VusToken vus_token_make_value(VusTokenType type, const char *value,
                              int line, int col)
{
    VusToken tok;
    tok.type   = type;
    tok.start  = NULL;
    tok.length = 0;
    tok.line   = line;
    tok.column = col;
    tok.value  = value ? strdup(value) : NULL;
    return tok;
}

void vus_token_free(VusToken *tok)
{
    if (tok && tok->value) {
        free(tok->value);
        tok->value = NULL;
    }
}

/* ============ Token 类型名称 ============ */

const char *vus_token_type_name(VusTokenType type)
{
    switch (type) {
    case VUS_TOKEN_IDENTIFIER:   return "标识符";
    case VUS_TOKEN_STRING:       return "字符串";
    case VUS_TOKEN_NUMBER:       return "数字";

    /* 英文关键字 */
    case VUS_TOKEN_DEF:          return "def";
    case VUS_TOKEN_IF:           return "if";
    case VUS_TOKEN_ELIF:         return "elif";
    case VUS_TOKEN_ELSE:         return "else";
    case VUS_TOKEN_FOR:          return "for";
    case VUS_TOKEN_WHILE:        return "while";
    case VUS_TOKEN_RETURN:       return "return";
    case VUS_TOKEN_IMPORT:       return "import";
    case VUS_TOKEN_FROM:         return "from";
    case VUS_TOKEN_TRUE:         return "true";
    case VUS_TOKEN_FALSE:        return "false";
    case VUS_TOKEN_NULL:         return "null";
    case VUS_TOKEN_AND:          return "and";
    case VUS_TOKEN_OR:           return "or";
    case VUS_TOKEN_NOT:          return "not";
    case VUS_TOKEN_TRY:          return "try";
    case VUS_TOKEN_EXCEPT:       return "except";
    case VUS_TOKEN_GLOBAL:       return "global";
    case VUS_TOKEN_BREAK:        return "break";
    case VUS_TOKEN_CONTINUE:     return "continue";
    case VUS_TOKEN_THROW:        return "throw";
    case VUS_TOKEN_IN:           return "in";

    /* 中文别名关键字 */
    case VUS_TOKEN_CN_DEF:       return "定义";
    case VUS_TOKEN_CN_IF:        return "如果";
    case VUS_TOKEN_CN_ELIF:      return "否则如果";
    case VUS_TOKEN_CN_ELSE:      return "否则";
    case VUS_TOKEN_CN_FOR:       return "循环";
    case VUS_TOKEN_CN_WHILE:     return "当循环";
    case VUS_TOKEN_CN_RETURN:    return "返回";
    case VUS_TOKEN_CN_IMPORT:    return "导入";
    case VUS_TOKEN_CN_FROM:      return "从";
    case VUS_TOKEN_CN_TO:        return "到";
    case VUS_TOKEN_CN_TRUE:      return "真";
    case VUS_TOKEN_CN_FALSE:     return "假";
    case VUS_TOKEN_CN_NULL:      return "空";
    case VUS_TOKEN_CN_AND:       return "和";
    case VUS_TOKEN_CN_OR:        return "或";
    case VUS_TOKEN_CN_NOT:       return "非";
    case VUS_TOKEN_CN_TRY:       return "尝试";
    case VUS_TOKEN_CN_EXCEPT:    return "捕获";
    case VUS_TOKEN_CN_GLOBAL:    return "全局";
    case VUS_TOKEN_CN_BREAK:     return "跳出";
    case VUS_TOKEN_CN_CONTINUE:  return "继续";
    case VUS_TOKEN_CN_THROW:     return "抛出";
    case VUS_TOKEN_CN_IN:        return "在";

    case VUS_TOKEN_STRUCT:       return "struct";
    case VUS_TOKEN_CN_STRUCT:    return "结构";

    /* 线程关键字 */
    case VUS_TOKEN_CN_THREAD:       return "线程";
    case VUS_TOKEN_CN_JOIN_THREAD:  return "等待线程";
    case VUS_TOKEN_CN_THREAD_SLEEP: return "睡眠";

    /* 协程关键字 */
    case VUS_TOKEN_CN_COROUTINE:    return "协程";
    case VUS_TOKEN_CN_RESUME:       return "恢复";
    case VUS_TOKEN_CN_YIELD:        return "让出";
    case VUS_TOKEN_CN_AWAIT:        return "等待";

    /* 类型关键字 */
    case VUS_TOKEN_TYPE_INT:     return "整型";
    case VUS_TOKEN_TYPE_FLOAT:   return "浮点型";
    case VUS_TOKEN_TYPE_STR:     return "字符串";
    case VUS_TOKEN_TYPE_BOOL:    return "布尔型";
    case VUS_TOKEN_TYPE_LIST:    return "列表";
    case VUS_TOKEN_TYPE_DICT:    return "字典";

    /* 分隔符 */
    case VUS_TOKEN_LPAREN:       return "左括号";
    case VUS_TOKEN_RPAREN:       return "右括号";
    case VUS_TOKEN_LBRACKET:     return "左中括号";
    case VUS_TOKEN_RBRACKET:     return "右中括号";
    case VUS_TOKEN_LBRACE:       return "左花括号";
    case VUS_TOKEN_RBRACE:       return "右花括号";
    case VUS_TOKEN_COMMA:        return "逗号";
    case VUS_TOKEN_COLON:        return "冒号";
    case VUS_TOKEN_DOT:          return "点号";

    /* 运算符 */
    case VUS_TOKEN_PLUS:         return "加号";
    case VUS_TOKEN_MINUS:        return "减号";
    case VUS_TOKEN_STAR:         return "乘号";
    case VUS_TOKEN_SLASH:        return "除号";
    case VUS_TOKEN_PERCENT:      return "百分号";
    case VUS_TOKEN_CONCAT:       return "拼接";
    case VUS_TOKEN_ASSIGN:       return "赋值";
    case VUS_TOKEN_EQ:           return "等于";
    case VUS_TOKEN_NEQ:          return "不等于";
    case VUS_TOKEN_LT:           return "小于";
    case VUS_TOKEN_GT:           return "大于";
    case VUS_TOKEN_LE:           return "小于等于";
    case VUS_TOKEN_GE:           return "大于等于";
    case VUS_TOKEN_BIT_AND:      return "按位与";
    case VUS_TOKEN_BIT_OR:       return "按位或";
    case VUS_TOKEN_BIT_XOR:      return "按位异或";
    case VUS_TOKEN_SHL:          return "左移";
    case VUS_TOKEN_SHR:          return "右移";

    /* 特殊 */
    case VUS_TOKEN_NEWLINE:      return "换行";
    case VUS_TOKEN_INDENT:       return "缩进";
    case VUS_TOKEN_DEDENT:       return "取消缩进";
    case VUS_TOKEN_EOF:          return "文件结束";
    case VUS_TOKEN_ERROR:        return "错误";
    }
    return "未知";
}

/* ============ 关键字查找表 ============ */

typedef struct {
    const char  *keyword;
    size_t       len;
    VusTokenType type;
} KeywordEntry;

/* 英文关键字 + 中文别名 + 类型关键字 */
static const KeywordEntry s_keywords[] = {
    /* 英文关键字 */
    {"and",      3, VUS_TOKEN_AND},
    {"bool",     4, VUS_TOKEN_TYPE_BOOL},
    {"break",    5, VUS_TOKEN_BREAK},
    {"continue", 8, VUS_TOKEN_CONTINUE},
    {"def",      3, VUS_TOKEN_DEF},
    {"dict",     4, VUS_TOKEN_TYPE_DICT},
    {"elif",     4, VUS_TOKEN_ELIF},
    {"else",     4, VUS_TOKEN_ELSE},
    {"except",   6, VUS_TOKEN_EXCEPT},
    {"false",    5, VUS_TOKEN_FALSE},
    {"float",    5, VUS_TOKEN_TYPE_FLOAT},
    {"for",      3, VUS_TOKEN_FOR},
    {"from",     4, VUS_TOKEN_FROM},
    {"global",   6, VUS_TOKEN_GLOBAL},
    {"if",       2, VUS_TOKEN_IF},
    {"import",   6, VUS_TOKEN_IMPORT},
    {"in",       2, VUS_TOKEN_IN},
    {"int",      3, VUS_TOKEN_TYPE_INT},
    {"list",     4, VUS_TOKEN_TYPE_LIST},
    {"not",      3, VUS_TOKEN_NOT},
    {"null",     4, VUS_TOKEN_NULL},
    {"or",       2, VUS_TOKEN_OR},
    {"return",   6, VUS_TOKEN_RETURN},
    {"str",      3, VUS_TOKEN_TYPE_STR},
    {"throw",    5, VUS_TOKEN_THROW},
    {"true",     4, VUS_TOKEN_TRUE},
    {"try",      3, VUS_TOKEN_TRY},
    {"struct",   6, VUS_TOKEN_STRUCT},
    {"while",    5, VUS_TOKEN_WHILE},

    /* 中文别名关键字 */
    {"从",       3, VUS_TOKEN_CN_FROM},
    {"假",       3, VUS_TOKEN_CN_FALSE},
    {"全局",     6, VUS_TOKEN_CN_GLOBAL},
    {"到",       3, VUS_TOKEN_CN_TO},
    {"和",       3, VUS_TOKEN_CN_AND},
    {"在",       3, VUS_TOKEN_CN_IN},
    {"如果",     6, VUS_TOKEN_CN_IF},
    {"尝试",     6, VUS_TOKEN_CN_TRY},
    {"定义",     6, VUS_TOKEN_CN_DEF},
    {"导入",     6, VUS_TOKEN_CN_IMPORT},
    {"布尔型",   9, VUS_TOKEN_TYPE_BOOL},
    {"当循环",   9, VUS_TOKEN_CN_WHILE},
    {"返回",     6, VUS_TOKEN_CN_RETURN},
    {"抛出",     6, VUS_TOKEN_CN_THROW},
    {"捕获",     6, VUS_TOKEN_CN_EXCEPT},
    {"整型",     6, VUS_TOKEN_TYPE_INT},
    {"或",       3, VUS_TOKEN_CN_OR},
    {"跳出",     6, VUS_TOKEN_CN_BREAK},
    {"继续",     6, VUS_TOKEN_CN_CONTINUE},
    {"空",       3, VUS_TOKEN_CN_NULL},
    {"字符串",   9, VUS_TOKEN_TYPE_STR},
    {"循环",     6, VUS_TOKEN_CN_FOR},
    {"非",       3, VUS_TOKEN_CN_NOT},
    {"真",       3, VUS_TOKEN_CN_TRUE},
    {"否则",     6, VUS_TOKEN_CN_ELSE},
    {"否则如果", 12, VUS_TOKEN_CN_ELIF},
    {"浮点型",   9, VUS_TOKEN_TYPE_FLOAT},
    {"结构",     6, VUS_TOKEN_CN_STRUCT},

    /* 线程关键字 */
    {"线程",     6, VUS_TOKEN_CN_THREAD},
    {"等待线程", 12, VUS_TOKEN_CN_JOIN_THREAD},
    {"睡眠",     6, VUS_TOKEN_CN_THREAD_SLEEP},

    /* 协程关键字 */
    {"协程",     6, VUS_TOKEN_CN_COROUTINE},
    {"恢复",     6, VUS_TOKEN_CN_RESUME},
    {"让出",     6, VUS_TOKEN_CN_YIELD},
    {"等待",     6, VUS_TOKEN_CN_AWAIT},
};

#define KEYWORD_COUNT (sizeof(s_keywords) / sizeof(s_keywords[0]))

/*
 * 线性扫描关键字查找表。
 * 表较小，线性扫描足够快且避免 UTF-8 排序问题。
 */
static VusTokenType lookup_linear(const KeywordEntry *table, size_t count,
                                  const char *start, size_t len)
{
    for (size_t i = 0; i < count; i++) {
        if (table[i].len == len &&
            strncmp(table[i].keyword, start, len) == 0) {
            return table[i].type;
        }
    }
    return VUS_TOKEN_IDENTIFIER;
}

VusTokenType vus_keyword_lookup(const char *start, size_t len)
{
    return lookup_linear(s_keywords, KEYWORD_COUNT, start, len);
}

/* ============ UTF-8 解码函数 ============ */

/*
 * 解码 UTF-8 序列，返回 Unicode 码点。
 * s: 输入字符串（至少 1 字节）
 * bytes_consumed: 输出参数，表示消耗的字节数（1-4）
 * 返回值：Unicode 码点，或 -1（无效序列）
 */
int vus_utf8_decode(const char *s, int *bytes_consumed)
{
    unsigned char c = (unsigned char)s[0];

    /* 单字节 ASCII */
    if (c < 0x80) {
        *bytes_consumed = 1;
        return c;
    }

    /* 2 字节 UTF-8: 110xxxxx 10xxxxxx */
    if ((c & 0xE0) == 0xC0) {
        if (!(s[1] && ((unsigned char)s[1] & 0xC0) == 0x80)) {
            *bytes_consumed = 0;
            return -1;
        }
        *bytes_consumed = 2;
        return ((int)(c & 0x1F) << 6) |
               ((int)(s[1] & 0x3F));
    }

    /* 3 字节 UTF-8: 1110xxxx 10xxxxxx 10xxxxxx */
    if ((c & 0xF0) == 0xE0) {
        if (!(s[1] && ((unsigned char)s[1] & 0xC0) == 0x80 &&
              s[2] && ((unsigned char)s[2] & 0xC0) == 0x80)) {
            *bytes_consumed = 0;
            return -1;
        }
        *bytes_consumed = 3;
        return ((int)(c & 0x0F) << 12) |
               ((int)(s[1] & 0x3F) << 6) |
               ((int)(s[2] & 0x3F));
    }

    /* 4 字节 UTF-8: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx */
    if ((c & 0xF8) == 0xF0) {
        if (!(s[1] && ((unsigned char)s[1] & 0xC0) == 0x80 &&
              s[2] && ((unsigned char)s[2] & 0xC0) == 0x80 &&
              s[3] && ((unsigned char)s[3] & 0xC0) == 0x80)) {
            *bytes_consumed = 0;
            return -1;
        }
        *bytes_consumed = 4;
        return ((int)(c & 0x07) << 18) |
               ((int)(s[1] & 0x3F) << 12) |
               ((int)(s[2] & 0x3F) << 6) |
               ((int)(s[3] & 0x3F));
    }

    /* 无效序列 */
    *bytes_consumed = 0;
    return -1;
}

/* ============ 字符分类函数 ============ */

int vus_is_chinese_id_char(int c)
{
    long cp = (long)c;

    /* CJK Unified Ideographs */
    if (cp >= 0x4E00 && cp <= 0x9FFF) return 1;
    /* CJK Unified Ideographs Extension A */
    if (cp >= 0x3400 && cp <= 0x4DBF) return 1;
    /* CJK Unified Ideographs Extension B */
    if (cp >= 0x20000 && cp <= 0x2A6DF) return 1;
    /* CJK Radicals Supplement */
    if (cp >= 0x2E80 && cp <= 0x2EFF) return 1;
    /* CJK Compatibility Ideographs */
    if (cp >= 0xF900 && cp <= 0xFAFF) return 1;
    /* CJK Compatibility Ideographs Supplement */
    if (cp >= 0x2F800 && cp <= 0x2FA1F) return 1;
    /* Fullwidth letters (identifier characters) */
    if (cp >= 0xFF41 && cp <= 0xFF5A) return 1; /* fullwidth a-z */
    if (cp >= 0xFF21 && cp <= 0xFF3A) return 1; /* fullwidth A-Z */
    if (cp >= 0xFF10 && cp <= 0xFF19) return 1; /* fullwidth 0-9 */

    return 0;
}

int vus_is_ident_start(int c)
{
    long cp = (long)c;

    /* ASCII letters and underscore */
    if ((cp >= 'a' && cp <= 'z') ||
        (cp >= 'A' && cp <= 'Z') ||
        cp == '_')
        return 1;

    /* Chinese characters */
    return vus_is_chinese_id_char(cp);
}

int vus_is_ident_continue(int c)
{
    long cp = (long)c;

    /* ASCII letters, digits, underscore */
    if ((cp >= 'a' && cp <= 'z') ||
        (cp >= 'A' && cp <= 'Z') ||
        (cp >= '0' && cp <= '9') ||
        cp == '_')
        return 1;

    /* Chinese characters */
    return vus_is_chinese_id_char(cp);
}