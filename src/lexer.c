/*
 * lexer.c — VUS 词法分析器实现
 *
 * 将 VUS 源码字符串转换为 Token 流。
 * 支持函数风格（中英别名关键字）。
 * 使用缩进栈处理 INDENT/DEDENT。
 *
 * 缩进规则：
 *   - 每行开头计算缩进级别（空格=1，制表符=4）
 *   - 空行和纯注释行不产生 NEWLINE，不影响缩进栈
 *   - 缩进增加时发出 INDENT，减少时发出一个或多个 DEDENT
 *   - 缩进栈初始为 [0]
 *
 * 注释：
 *   - # 开头到行尾
 *   - // 开头到行尾
 */

#include "lexer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ============ 静态辅助函数声明 ============ */

static void     lexer_advance(VusLexer *lexer);
static char     lexer_peek_next(VusLexer *lexer);
static void     lexer_skip_whitespace(VusLexer *lexer);
static void     lexer_skip_comment(VusLexer *lexer);
static void     lexer_handle_indent(VusLexer *lexer);
static void     lexer_read_string(VusLexer *lexer);
static void     lexer_read_number(VusLexer *lexer);
static void     lexer_skip_ident(VusLexer *lexer);
static void     lexer_read_identifier(VusLexer *lexer);
static void     lexer_add_token(VusLexer *lexer, VusTokenType type,
                                const char *start, size_t length,
                                int line, int column);
static void     lexer_add_token_value(VusLexer *lexer, VusTokenType type,
                                      const char *value, int line, int column);
static void     lexer_set_error(VusLexer *lexer, const char *msg);

/* ============ 静态辅助函数实现 ============ */

/*
 * 消费一个字符，更新位置、行号和列号。
 */
static void lexer_advance(VusLexer *lexer)
{
    if (lexer->pos < lexer->source_len) {
        if (lexer->source[lexer->pos] == '\n') {
            lexer->line++;
            lexer->column = 1;
        } else {
            lexer->column++;
        }
        lexer->pos++;
    }
}

/*
 * 查看下一个字符（不消费）。到达末尾返回 '\0'。
 */
static char lexer_peek_next(VusLexer *lexer)
{
    if (lexer->pos + 1 < lexer->source_len) {
        return lexer->source[lexer->pos + 1];
    }
    return '\0';
}

/*
 * 跳过行内空白（空格和制表符，不包括换行）。
 */
static void lexer_skip_whitespace(VusLexer *lexer)
{
    while (lexer->pos < lexer->source_len) {
        char c = lexer->source[lexer->pos];
        if (c == ' ' || c == '\t') {
            lexer_advance(lexer);
        } else {
            break;
        }
    }
}

/*
 * 从当前位置跳过注释到行尾（不消费换行符）。
 * 调用前已确认当前字符是注释起始符（# 或 //）。
 */
static void lexer_skip_comment(VusLexer *lexer)
{
    while (lexer->pos < lexer->source_len) {
        char c = lexer->source[lexer->pos];
        if (c == '\n') {
            break;
        }
        lexer_advance(lexer);
    }
}

/*
 * 处理行首缩进。
 *
 * 1. 测量缩进级别（空格=1，制表符=4）
 * 2. 若该行是空行或纯注释行，跳过整行（不产生 Token），
 *    保持 at_line_start = 1
 * 3. 否则根据缩进栈发出 INDENT / DEDENT，设置 at_line_start = 0
 */
static void lexer_handle_indent(VusLexer *lexer)
{
    int indent = 0;
    int saved_line = lexer->line;
    int saved_col  = lexer->column;

    /* 测量缩进 */
    while (lexer->pos < lexer->source_len) {
        char c = lexer->source[lexer->pos];
        if (c == ' ') {
            indent++;
            lexer->pos++;
            lexer->column++;
        } else if (c == '\t') {
            indent += 4;
            lexer->pos++;
            lexer->column++;
        } else {
            break;
        }
    }

    /* 检查是否到达行尾或纯注释行 */
    if (lexer->pos >= lexer->source_len ||
        lexer->source[lexer->pos] == '\n' ||
        lexer->source[lexer->pos] == '\r' ||
        lexer->source[lexer->pos] == '#')
    {
        /* 跳过注释（如果有） */
        if (lexer->pos < lexer->source_len &&
            lexer->source[lexer->pos] == '#')
        {
            lexer_skip_comment(lexer);
        }
        /* 跳过换行符 */
        if (lexer->pos < lexer->source_len &&
            lexer->source[lexer->pos] == '\r')
        {
            lexer_advance(lexer);
        }
        if (lexer->pos < lexer->source_len &&
            lexer->source[lexer->pos] == '\n')
        {
            lexer_advance(lexer);
        }
        /* 保持行首状态，继续处理下一行 */
        lexer->at_line_start = 1;
        return;
    }

    /* 检查 // 注释 */
    if (lexer->pos + 1 < lexer->source_len &&
        lexer->source[lexer->pos] == '/' &&
        lexer->source[lexer->pos + 1] == '/')
    {
        lexer_skip_comment(lexer);
        /* 跳过换行符 */
        if (lexer->pos < lexer->source_len &&
            lexer->source[lexer->pos] == '\r')
        {
            lexer_advance(lexer);
        }
        if (lexer->pos < lexer->source_len &&
            lexer->source[lexer->pos] == '\n')
        {
            lexer_advance(lexer);
        }
        lexer->at_line_start = 1;
        return;
    }

    /* 处理缩进变化 */
    int top = lexer->indent_stack[lexer->indent_depth - 1];

    if (indent > top) {
        /* 压入新的缩进级别 */
        if (lexer->indent_depth >= lexer->indent_cap) {
            size_t new_cap = lexer->indent_cap == 0 ? 8 : lexer->indent_cap * 2;
            int *new_stack = realloc(lexer->indent_stack,
                                     new_cap * sizeof(int));
            if (!new_stack) {
                lexer_set_error(lexer, "内存不足：无法扩展缩进栈");
                return;
            }
            lexer->indent_stack = new_stack;
            lexer->indent_cap   = new_cap;
        }
        lexer->indent_stack[lexer->indent_depth++] = indent;
        lexer_add_token(lexer, VUS_TOKEN_INDENT,
                        lexer->source + lexer->pos, 0,
                        saved_line, saved_col);
    } else if (indent < top) {
        /* 弹出缩进级别，发出一个或多个 DEDENT */
        while (lexer->indent_depth > 0 &&
               lexer->indent_stack[lexer->indent_depth - 1] > indent)
        {
            lexer->indent_depth--;
            lexer_add_token(lexer, VUS_TOKEN_DEDENT,
                            lexer->source + lexer->pos, 0,
                            saved_line, saved_col);
        }
        if (lexer->indent_depth == 0 ||
            lexer->indent_stack[lexer->indent_depth - 1] != indent)
        {
            lexer_set_error(lexer, "缩进不一致");
            return;
        }
    }

    /* 更新列号（已消耗缩进空白） */
    lexer->column = indent + 1;
    lexer->at_line_start = 0;
}

/*
 * 读取双引号字符串字面量，支持转义序列。
 * 转义：\n \r \t \\ \" \xHH \uHHHH
 */
static void lexer_read_string(VusLexer *lexer)
{
    int line = lexer->line;
    int col  = lexer->column;

    /* 跳过开头的 " */
    lexer_advance(lexer);

    /* 动态缓冲区存放解码后的字符串 */
    size_t cap = 64;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        lexer_set_error(lexer, "内存不足：无法分配字符串缓冲区");
        return;
    }

    while (lexer->pos < lexer->source_len) {
        char c = lexer->source[lexer->pos];

        if (c == '"') {
            /* 闭合引号 */
            lexer_advance(lexer);
            buf[len] = '\0';
            lexer_add_token_value(lexer, VUS_TOKEN_STRING, buf, line, col);
            free(buf);
            return;
        }

        if (c == '\n') {
            lexer_set_error(lexer, "字符串未闭合（遇到换行）");
            free(buf);
            return;
        }

        if (c == '\\') {
            /* 转义序列 */
            lexer_advance(lexer);
            if (lexer->pos >= lexer->source_len) {
                lexer_set_error(lexer, "字符串中反斜杠后缺少字符");
                free(buf);
                return;
            }
            char esc = lexer->source[lexer->pos];
            lexer_advance(lexer);

            switch (esc) {
            case 'n':
                if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                buf[len++] = '\n';
                break;
            case 'r':
                if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                buf[len++] = '\r';
                break;
            case 't':
                if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                buf[len++] = '\t';
                break;
            case '\\':
                if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                buf[len++] = '\\';
                break;
            case '"':
                if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                buf[len++] = '"';
                break;
            case 'x': {
                /* \xHH — 两位十六进制 */
                char hex[3] = {0};
                if (lexer->pos < lexer->source_len &&
                    isxdigit((unsigned char)lexer->source[lexer->pos])) {
                    hex[0] = lexer->source[lexer->pos];
                    lexer_advance(lexer);
                } else {
                    lexer_set_error(lexer, "\\x 后需要两位十六进制数字");
                    free(buf);
                    return;
                }
                if (lexer->pos < lexer->source_len &&
                    isxdigit((unsigned char)lexer->source[lexer->pos])) {
                    hex[1] = lexer->source[lexer->pos];
                    lexer_advance(lexer);
                } else {
                    lexer_set_error(lexer, "\\x 后需要两位十六进制数字");
                    free(buf);
                    return;
                }
                unsigned long val = strtoul(hex, NULL, 16);
                if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                buf[len++] = (char)val;
                break;
            }
            case 'u': {
                /* \uHHHH — 四位十六进制，编码为 UTF-8 */
                char hex[5] = {0};
                int i;
                for (i = 0; i < 4; i++) {
                    if (lexer->pos < lexer->source_len &&
                        isxdigit((unsigned char)lexer->source[lexer->pos])) {
                        hex[i] = lexer->source[lexer->pos];
                        lexer_advance(lexer);
                    } else {
                        break;
                    }
                }
                if (i < 4) {
                    lexer_set_error(lexer, "\\u 后需要四位十六进制数字");
                    free(buf);
                    return;
                }
                unsigned long cp = strtoul(hex, NULL, 16);
                /* 编码为 UTF-8 */
                if (cp < 0x80) {
                    if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                    buf[len++] = (char)cp;
                } else if (cp < 0x800) {
                    if (len + 2 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                    buf[len++] = (char)(0xC0 | (cp >> 6));
                    buf[len++] = (char)(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    if (len + 3 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                    buf[len++] = (char)(0xE0 | (cp >> 12));
                    buf[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    buf[len++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    /* 超出 BMP，暂不支持 */
                    lexer_set_error(lexer, "\\u 转义超出基本多文种平面");
                    free(buf);
                    return;
                }
                break;
            }
            default:
                /* 未知转义序列，保持原样 */
                if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                buf[len++] = esc;
                break;
            }
        } else {
            /* 普通字符 */
            if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
            buf[len++] = c;
            lexer_advance(lexer);
        }
    }

    /* 遇到文件末尾仍未闭合 */
    lexer_set_error(lexer, "字符串未闭合（遇到文件末尾）");
    free(buf);
}

/*
 * 读取数字字面量。
 * 支持：十进制整数/浮点数、十六进制 (0x)、二进制 (0b)。
 */
static void lexer_read_number(VusLexer *lexer)
{
    int line = lexer->line;
    int col  = lexer->column;
    const char *start = lexer->source + lexer->pos;

    /* 检查十六进制或二进制前缀 */
    if (lexer->pos < lexer->source_len &&
        lexer->source[lexer->pos] == '0' &&
        lexer->pos + 1 < lexer->source_len)
    {
        char nxt = lexer->source[lexer->pos + 1];
        if (nxt == 'x' || nxt == 'X') {
            /* 0x... 十六进制 */
            lexer_advance(lexer); /* 0 */
            lexer_advance(lexer); /* x */
            while (lexer->pos < lexer->source_len &&
                   isxdigit((unsigned char)lexer->source[lexer->pos])) {
                lexer_advance(lexer);
            }
            size_t len = (lexer->source + lexer->pos) - start;
            lexer_add_token(lexer, VUS_TOKEN_NUMBER, start, len, line, col);
            return;
        }
        if (nxt == 'b' || nxt == 'B') {
            /* 0b... 二进制 */
            lexer_advance(lexer); /* 0 */
            lexer_advance(lexer); /* b */
            while (lexer->pos < lexer->source_len &&
                   (lexer->source[lexer->pos] == '0' ||
                    lexer->source[lexer->pos] == '1')) {
                lexer_advance(lexer);
            }
            size_t len = (lexer->source + lexer->pos) - start;
            lexer_add_token(lexer, VUS_TOKEN_NUMBER, start, len, line, col);
            return;
        }
    }

    /* 十进制整数部分 */
    while (lexer->pos < lexer->source_len &&
           isdigit((unsigned char)lexer->source[lexer->pos])) {
        lexer_advance(lexer);
    }

    /* 检查浮点数：小数点后跟数字 */
    if (lexer->pos < lexer->source_len &&
        lexer->source[lexer->pos] == '.' &&
        lexer->pos + 1 < lexer->source_len &&
        isdigit((unsigned char)lexer->source[lexer->pos + 1]))
    {
        lexer_advance(lexer); /* . */
        while (lexer->pos < lexer->source_len &&
               isdigit((unsigned char)lexer->source[lexer->pos])) {
            lexer_advance(lexer);
        }
    }

    size_t len = (lexer->source + lexer->pos) - start;
    lexer_add_token(lexer, VUS_TOKEN_NUMBER, start, len, line, col);
}

/*
 * 跳过标识符字符（支持 UTF-8 多字节中文字符）。
 * 不创建 Token，仅移动 pos。
 */
static void lexer_skip_ident(VusLexer *lexer)
{
    while (lexer->pos < lexer->source_len) {
        unsigned char b = (unsigned char)lexer->source[lexer->pos];
        if (b < 0x80) {
            /* ASCII */
            if (!vus_is_ident_continue(b))
                break;
            lexer_advance(lexer);
        } else {
            /* 多字节 UTF-8：解码后检查 */
            int consumed = 0;
            int cp = vus_utf8_decode(lexer->source + lexer->pos, &consumed);
            if (cp <= 0 || !vus_is_ident_continue(cp))
                break;
            for (int i = 0; i < consumed; i++)
                lexer_advance(lexer);
        }
    }
}

/*
 * 读取标识符或关键字。
 * 标识符以字母 / 下划线 / 中文字符开头，后续可以是字母数字下划线中文。
 * 通过 vus_keyword_lookup() 匹配关键字。
 */
static void lexer_read_identifier(VusLexer *lexer)
{
    int line = lexer->line;
    int col  = lexer->column;
    const char *start = lexer->source + lexer->pos;

    lexer_skip_ident(lexer);

    size_t len = (lexer->source + lexer->pos) - start;
    VusTokenType type = vus_keyword_lookup(start, len);
    lexer_add_token(lexer, type, start, len, line, col);
}

/*
 * 将 Token 添加到动态数组。
 */
static void lexer_add_token(VusLexer *lexer, VusTokenType type,
                            const char *start, size_t length,
                            int line, int column)
{
    if (lexer->token_count >= lexer->token_cap) {
        size_t new_cap = lexer->token_cap == 0 ? 64 : lexer->token_cap * 2;
        VusToken *new_tokens = realloc(lexer->tokens,
                                       new_cap * sizeof(VusToken));
        if (!new_tokens) {
            lexer_set_error(lexer, "内存不足：无法扩展 Token 数组");
            return;
        }
        lexer->tokens    = new_tokens;
        lexer->token_cap = new_cap;
    }

    VusToken tok = vus_token_make(type, start, length, line, column);
    lexer->tokens[lexer->token_count++] = tok;
}

/*
 * 将带动态值的 Token 添加到数组（value 会被 strdup）。
 */
static void lexer_add_token_value(VusLexer *lexer, VusTokenType type,
                                  const char *value, int line, int column)
{
    if (lexer->token_count >= lexer->token_cap) {
        size_t new_cap = lexer->token_cap == 0 ? 64 : lexer->token_cap * 2;
        VusToken *new_tokens = realloc(lexer->tokens,
                                       new_cap * sizeof(VusToken));
        if (!new_tokens) {
            lexer_set_error(lexer, "内存不足：无法扩展 Token 数组");
            return;
        }
        lexer->tokens    = new_tokens;
        lexer->token_cap = new_cap;
    }

    VusToken tok = vus_token_make_value(type, value, line, column);
    lexer->tokens[lexer->token_count++] = tok;
}

/*
 * 设置错误状态。
 */
static void lexer_set_error(VusLexer *lexer, const char *msg)
{
    if (lexer->error) return;
    lexer->error = 1;
    strncpy(lexer->error_msg, msg, sizeof(lexer->error_msg) - 1);
    lexer->error_msg[sizeof(lexer->error_msg) - 1] = '\0';
}

/* ============ 公开 API 实现 ============ */

/*
 * 创建并初始化词法分析器。
 */
VusLexer *vus_lexer_new(const char *source, size_t source_len)
{
    VusLexer *lexer = calloc(1, sizeof(VusLexer));
    if (!lexer) return NULL;

    lexer->source     = source;
    lexer->source_len = source_len;
    lexer->pos        = 0;
    lexer->line       = 1;
    lexer->column     = 1;

    lexer->tokens     = NULL;
    lexer->token_count = 0;
    lexer->token_cap  = 0;

    /* 缩进栈初始为 [0] */
    lexer->indent_stack = malloc(sizeof(int));
    if (!lexer->indent_stack) {
        free(lexer);
        return NULL;
    }
    lexer->indent_stack[0] = 0;
    lexer->indent_depth    = 1;
    lexer->indent_cap      = 1;

    lexer->at_line_start = 1;
    lexer->error         = 0;
    lexer->error_msg[0]  = '\0';

    return lexer;
}

/*
 * 执行词法分析，返回 Token 数组。
 *
 * 主循环逐个字符处理，按顺序识别：
 *   行首缩进 → 空白 → 注释 / 换行 / 标识符 / 数字 / 字符串 / 运算符 / 分隔符
 * 遇到错误时停止并返回已收集的 Token。
 */
VusToken *vus_lexer_tokenize(VusLexer *lexer, size_t *out_count)
{
    if (!lexer) {
        if (out_count) *out_count = 0;
        return NULL;
    }

    lexer->at_line_start = 1;

    while (lexer->pos < lexer->source_len && !lexer->error) {
        /* 行首：处理缩进 */
        if (lexer->at_line_start) {
            lexer_handle_indent(lexer);
            if (lexer->error) break;
            if (lexer->at_line_start) {
                /* 空行/注释行已被跳过，继续下一行 */
                continue;
            }
        }

        /* 跳过行内空白 */
        lexer_skip_whitespace(lexer);
        if (lexer->pos >= lexer->source_len) break;

        char c = lexer->source[lexer->pos];
        int line = lexer->line;
        int col  = lexer->column;

        /* === 换行 === */
        if (c == '\n') {
            lexer_advance(lexer);
            lexer_add_token(lexer, VUS_TOKEN_NEWLINE,
                            lexer->source + (lexer->pos - 1), 1, line, col);
            lexer->at_line_start = 1;
            continue;
        }

        /* === 回车（\r\n 兼容） === */
        if (c == '\r') {
            lexer_advance(lexer);
            /* 如果后面是 \n 也一并跳过 */
            if (lexer->pos < lexer->source_len &&
                lexer->source[lexer->pos] == '\n') {
                lexer_advance(lexer);
            }
            lexer_add_token(lexer, VUS_TOKEN_NEWLINE,
                            lexer->source + (lexer->pos - 1), 1, line, col);
            lexer->at_line_start = 1;
            continue;
        }

        /* === 井号注释 === */
        if (c == '#') {
            lexer_skip_comment(lexer);
            /* 注释后必定是换行或 EOF，由后续循环处理 */
            continue;
        }

        /* === 注释 // 或除号/运算符 === */
        if (c == '/') {
            if (lexer_peek_next(lexer) == '/') {
                /* // 注释 */
                lexer_skip_comment(lexer);
                continue;
            }
            /* 除号运算符 */
            lexer_advance(lexer);
            lexer_add_token(lexer, VUS_TOKEN_SLASH,
                            lexer->source + (lexer->pos - 1), 1, line, col);
            continue;
        }

        /* === 标识符或关键字（支持 UTF-8 中文） === */
        if ((unsigned char)c >= 0x80) {
            /* 非 ASCII：解码 UTF-8 后检查 */
            int consumed = 0;
            int cp = vus_utf8_decode(lexer->source + lexer->pos, &consumed);
            if (cp > 0 && vus_is_ident_start(cp)) {
                lexer_read_identifier(lexer);
                continue;
            }
        } else if (vus_is_ident_start((unsigned char)c)) {
            lexer_read_identifier(lexer);
            continue;
        }

        /* === 字符串字面量 === */
        if (c == '"') {
            lexer_read_string(lexer);
            continue;
        }

        /* === 数字字面量 === */
        if (isdigit((unsigned char)c)) {
            lexer_read_number(lexer);
            continue;
        }

        /* === 点号 / 连接运算符 === */
        if (c == '.') {
            if (lexer_peek_next(lexer) == '.') {
                /* .. 连接运算符 */
                lexer_advance(lexer); /* 第一个 . */
                lexer_advance(lexer); /* 第二个 . */
                lexer_add_token(lexer, VUS_TOKEN_CONCAT,
                                lexer->source + (lexer->pos - 2), 2, line, col);
                continue;
            }

            /* 普通点号 */
            lexer_advance(lexer);
            lexer_add_token(lexer, VUS_TOKEN_DOT,
                            lexer->source + (lexer->pos - 1), 1, line, col);
            continue;
        }

        /* === 多字符运算符 === */

        /* == 和 = */
        if (c == '=') {
            if (lexer_peek_next(lexer) == '=') {
                lexer_advance(lexer);
                lexer_advance(lexer);
                lexer_add_token(lexer, VUS_TOKEN_EQ,
                                lexer->source + (lexer->pos - 2), 2, line, col);
            } else {
                lexer_advance(lexer);
                lexer_add_token(lexer, VUS_TOKEN_ASSIGN,
                                lexer->source + (lexer->pos - 1), 1, line, col);
            }
            continue;
        }

        /* != */
        if (c == '!') {
            if (lexer_peek_next(lexer) == '=') {
                lexer_advance(lexer);
                lexer_advance(lexer);
                lexer_add_token(lexer, VUS_TOKEN_NEQ,
                                lexer->source + (lexer->pos - 2), 2, line, col);
            } else {
                lexer_advance(lexer);
                lexer_add_token(lexer, VUS_TOKEN_NOT,
                                lexer->source + (lexer->pos - 1), 1, line, col);
            }
            continue;
        }

        /* <, <=, << */
        if (c == '<') {
            if (lexer_peek_next(lexer) == '=') {
                lexer_advance(lexer);
                lexer_advance(lexer);
                lexer_add_token(lexer, VUS_TOKEN_LE,
                                lexer->source + (lexer->pos - 2), 2, line, col);
            } else if (lexer_peek_next(lexer) == '<') {
                lexer_advance(lexer);
                lexer_advance(lexer);
                lexer_add_token(lexer, VUS_TOKEN_SHL,
                                lexer->source + (lexer->pos - 2), 2, line, col);
            } else {
                lexer_advance(lexer);
                lexer_add_token(lexer, VUS_TOKEN_LT,
                                lexer->source + (lexer->pos - 1), 1, line, col);
            }
            continue;
        }

        /* >, >=, >> */
        if (c == '>') {
            if (lexer_peek_next(lexer) == '=') {
                lexer_advance(lexer);
                lexer_advance(lexer);
                lexer_add_token(lexer, VUS_TOKEN_GE,
                                lexer->source + (lexer->pos - 2), 2, line, col);
            } else if (lexer_peek_next(lexer) == '>') {
                lexer_advance(lexer);
                lexer_advance(lexer);
                lexer_add_token(lexer, VUS_TOKEN_SHR,
                                lexer->source + (lexer->pos - 2), 2, line, col);
            } else {
                lexer_advance(lexer);
                lexer_add_token(lexer, VUS_TOKEN_GT,
                                lexer->source + (lexer->pos - 1), 1, line, col);
            }
            continue;
        }

        /* === 单字符分隔符和运算符 === */
        switch (c) {
        case '(':
            lexer_advance(lexer);
            lexer_add_token(lexer, VUS_TOKEN_LPAREN,
                            lexer->source + (lexer->pos - 1), 1, line, col);
            break;
        case ')':
            lexer_advance(lexer);
            lexer_add_token(lexer, VUS_TOKEN_RPAREN,
                            lexer->source + (lexer->pos - 1), 1, line, col);
            break;
        case '[':
            lexer_advance(lexer);
            lexer_add_token(lexer, VUS_TOKEN_LBRACKET,
                            lexer->source + (lexer->pos - 1), 1, line, col);
            break;
        case ']':
            lexer_advance(lexer);
            lexer_add_token(lexer, VUS_TOKEN_RBRACKET,
                            lexer->source + (lexer->pos - 1), 1, line, col);
            break;
        case '{':
            lexer_advance(lexer);
            lexer_add_token(lexer, VUS_TOKEN_LBRACE,
                            lexer->source + (lexer->pos - 1), 1, line, col);
            break;
        case '}':
            lexer_advance(lexer);
            lexer_add_token(lexer, VUS_TOKEN_RBRACE,
                            lexer->source + (lexer->pos - 1), 1, line, col);
            break;
        case ',':
            lexer_advance(lexer);
            lexer_add_token(lexer, VUS_TOKEN_COMMA,
                            lexer->source + (lexer->pos - 1), 1, line, col);
            break;
        case ':':
            lexer_advance(lexer);
            lexer_add_token(lexer, VUS_TOKEN_COLON,
                            lexer->source + (lexer->pos - 1), 1, line, col);
            break;
        case '+':
            lexer_advance(lexer);
            lexer_add_token(lexer, VUS_TOKEN_PLUS,
                            lexer->source + (lexer->pos - 1), 1, line, col);
            break;
        case '-':
            lexer_advance(lexer);
            lexer_add_token(lexer, VUS_TOKEN_MINUS,
                            lexer->source + (lexer->pos - 1), 1, line, col);
            break;
        case '*':
            lexer_advance(lexer);
            lexer_add_token(lexer, VUS_TOKEN_STAR,
                            lexer->source + (lexer->pos - 1), 1, line, col);
            break;
        case '%':
            lexer_advance(lexer);
            lexer_add_token(lexer, VUS_TOKEN_PERCENT,
                            lexer->source + (lexer->pos - 1), 1, line, col);
            break;
        case '&':
            lexer_advance(lexer);
            lexer_add_token(lexer, VUS_TOKEN_BIT_AND,
                            lexer->source + (lexer->pos - 1), 1, line, col);
            break;
        case '|':
            lexer_advance(lexer);
            lexer_add_token(lexer, VUS_TOKEN_BIT_OR,
                            lexer->source + (lexer->pos - 1), 1, line, col);
            break;
        case '^':
            lexer_advance(lexer);
            lexer_add_token(lexer, VUS_TOKEN_BIT_XOR,
                            lexer->source + (lexer->pos - 1), 1, line, col);
            break;
        case '~':
            lexer_advance(lexer);
            lexer_add_token(lexer, VUS_TOKEN_BIT_NOT,
                            lexer->source + (lexer->pos - 1), 1, line, col);
            break;
        default:
            /* 未知字符 */
            {
                char err_msg[128];
                snprintf(err_msg, sizeof(err_msg),
                         "无法识别的字符 '%c' (0x%02x) 在行 %d 列 %d",
                         c, (unsigned char)c, line, col);
                lexer_set_error(lexer, err_msg);
            }
            break;
        }
    }

    /* 结束：发出剩余的 DEDENT */
    while (lexer->indent_depth > 1) {
        lexer->indent_depth--;
        lexer_add_token(lexer, VUS_TOKEN_DEDENT,
                        lexer->source + lexer->pos, 0,
                        lexer->line, lexer->column);
    }

    /* 发出 EOF */
    lexer_add_token(lexer, VUS_TOKEN_EOF,
                    lexer->source + lexer->pos, 0,
                    lexer->line, lexer->column);

    if (out_count) {
        *out_count = lexer->token_count;
    }

    return lexer->tokens;
}

/*
 * 释放词法分析器及其所有资源。
 * 注意：不释放 Token 数组（调用方通过 vus_lexer_steal_tokens() 拥有所有权）。
 */
void vus_lexer_free(VusLexer *lexer)
{
    if (!lexer) return;

    free(lexer->indent_stack);
    free(lexer);
}

/*
 * 获取 Token 数组的所有权，内部置空指针防止重复释放。
 * 调用方需在不再使用时通过 free_tokens() 释放。
 */
VusToken *vus_lexer_steal_tokens(VusLexer *lexer, size_t *out_count)
{
    if (!lexer) {
        if (out_count) *out_count = 0;
        return NULL;
    }
    VusToken *tokens = lexer->tokens;
    if (out_count) *out_count = lexer->token_count;
    lexer->tokens = NULL;
    lexer->token_count = 0;
    return tokens;
}

/*
 * 释放 Token 数组及其动态值。
 */
void vus_lexer_free_tokens(VusToken *tokens, size_t count)
{
    if (!tokens) return;
    for (size_t i = 0; i < count; i++) {
        vus_token_free(&tokens[i]);
    }
    free(tokens);
}

/*
 * 获取错误信息。
 */
const char *vus_lexer_error(VusLexer *lexer)
{
    if (!lexer || !lexer->error) return NULL;
    return lexer->error_msg;
}

/*
 * 打印 Token 流（调试用）。
 */
void vus_lexer_print_tokens(VusToken *tokens, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        VusToken *tok = &tokens[i];
        printf("%s\tline=%d\tcol=%d",
               vus_token_type_name(tok->type),
               tok->line, tok->column);

        if (tok->value) {
            printf("\tvalue=\"%s\"", tok->value);
        } else if (tok->start && tok->length > 0) {
            printf("\ttext=\"%.*s\"", (int)tok->length, tok->start);
        }

        printf("\n");
    }
}