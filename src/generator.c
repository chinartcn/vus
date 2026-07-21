/*
 * generator.c — VUS C 代码生成器实现
 *
 * 将 AST 转换为合法的 C 源码，并可选调用 GCC 编译为可执行文件。
 * 生成的 C 代码依赖 libvus_rt 运行时库。
 */

#define _GNU_SOURCE
#include "generator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdarg.h>

/* ============ 内部缓冲区结构 ============ */

typedef struct {
    char   *data;
    size_t  len;
    size_t  cap;
    int     indent;
} GenBuf;

/* ============ 缓冲区操作 ============ */

static GenBuf *gen_buf_new(void) {
    GenBuf *buf = (GenBuf *)malloc(sizeof(GenBuf));
    if (!buf) return NULL;
    buf->cap = 4096;
    buf->len = 0;
    buf->indent = 0;
    buf->data = (char *)malloc(buf->cap);
    if (!buf->data) {
        free(buf);
        return NULL;
    }
    buf->data[0] = '\0';
    return buf;
}

static void gen_buf_grow(GenBuf *buf, size_t needed) {
    if (buf->len + needed < buf->cap) return;
    while (buf->len + needed >= buf->cap) {
        buf->cap *= 2;
    }
    buf->data = (char *)realloc(buf->data, buf->cap);
}

static void gen_emit(GenBuf *buf, const char *s) {
    if (!s) return;
    size_t slen = strlen(s);
    gen_buf_grow(buf, slen + 1);
    memcpy(buf->data + buf->len, s, slen);
    buf->len += slen;
    buf->data[buf->len] = '\0';
}

static void gen_emit_indent(GenBuf *buf) {
    for (int i = 0; i < buf->indent; i++) {
        gen_emit(buf, "    ");
    }
}

static void gen_emitf(GenBuf *buf, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    size_t avail = buf->cap - buf->len;
    int needed = vsnprintf(buf->data + buf->len, avail, fmt, ap);
    va_end(ap);
    if (needed < 0) { va_end(ap2); return; }
    if ((size_t)needed >= avail) {
        gen_buf_grow(buf, (size_t)needed + 1);
        vsnprintf(buf->data + buf->len, buf->cap - buf->len, fmt, ap2);
    }
    va_end(ap2);
    buf->len += (size_t)needed;
}

static void gen_emit_line(GenBuf *buf, const char *line) {
    gen_emit_indent(buf);
    gen_emit(buf, line);
    gen_emit(buf, "\n");
}

static void gen_emit_linef(GenBuf *buf, const char *fmt, ...) {
    gen_emit_indent(buf);
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    size_t avail = buf->cap - buf->len;
    int needed = vsnprintf(buf->data + buf->len, avail, fmt, ap);
    va_end(ap);
    if (needed < 0) { va_end(ap2); return; }
    if ((size_t)needed >= avail) {
        gen_buf_grow(buf, (size_t)needed + 1);
        vsnprintf(buf->data + buf->len, buf->cap - buf->len, fmt, ap2);
    }
    va_end(ap2);
    buf->len += (size_t)needed;
    gen_emit(buf, "\n");
}

/* ============ 名称清理 ============ */

/* 将中文/特殊字符转换为 _XXXX 形式 */
static void gen_sanitize_name(const char *name, char *out, size_t out_size) {
    size_t i = 0, o = 0;
    int first = 1;
    while (name[i] && o < out_size - 1) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x80 && (isalnum(c) || c == '_')) {
            if (first && isdigit(c)) {
                /* 数字开头的名字加前缀 */
                if (o + 6 < out_size) {
                    out[o++] = '_';
                }
            }
            out[o++] = (char)c;
            first = 0;
            i++;
        } else if (c >= 0x80) {
            /* 多字节 UTF-8 字符 → _XXXX */
            unsigned int code = 0;
            int bytes = 0;
            if ((c & 0xE0) == 0xC0) {
                code = c & 0x1F;
                bytes = 2;
            } else if ((c & 0xF0) == 0xE0) {
                code = c & 0x0F;
                bytes = 3;
            } else if ((c & 0xF8) == 0xF0) {
                code = c & 0x07;
                bytes = 4;
            } else {
                i++;
                continue;
            }
            int j;
            for (j = 1; j < bytes; j++) {
                if (name[i + j]) {
                    code = (code << 6) | ((unsigned char)name[i + j] & 0x3F);
                }
            }
            i += (size_t)bytes;
            if (o + 7 < out_size) {
                int n = snprintf(out + o, out_size - o, "_%04X", code);
                if (n > 0) o += (size_t)n;
            }
            first = 0;
        } else {
            /* 非字母数字ASCII字符 → 下划线 */
            out[o++] = '_';
            first = 0;
            i++;
        }
    }
    out[o] = '\0';
}

/* ============ 字符串转义 ============ */

static void gen_string_escape(const char *input, char *output, size_t out_size) {
    size_t i = 0, o = 0;
    while (input[i] && o < out_size - 1) {
        unsigned char c = (unsigned char)input[i];
        switch (c) {
            case '\\': if (o + 2 < out_size) { output[o++] = '\\'; output[o++] = '\\'; } break;
            case '"':  if (o + 2 < out_size) { output[o++] = '\\'; output[o++] = '"'; } break;
            case '\n': if (o + 2 < out_size) { output[o++] = '\\'; output[o++] = 'n'; } break;
            case '\r': if (o + 2 < out_size) { output[o++] = '\\'; output[o++] = 'r'; } break;
            case '\t': if (o + 2 < out_size) { output[o++] = '\\'; output[o++] = 't'; } break;
            default:
                if (c < 0x20) {
                    /* 控制字符转义为 \xHH */
                    if (o + 4 < out_size) {
                        snprintf(output + o, out_size - o, "\\x%02X", c);
                        o += 4;
                    }
                } else {
                    output[o++] = (char)c;
                }
                break;
        }
        i++;
    }
    output[o] = '\0';
}

/* ============ 表达式生成 ============ */

/* 返回 malloc 分配的字符串，调用方需 free */
static char *gen_expr(GenBuf *buf, VusAstNode *node);

static char *gen_expr_binary(GenBuf *buf, VusAstBinaryOp *bin) {
    char *left = gen_expr(buf, bin->left);
    char *right = gen_expr(buf, bin->right);
    char *result = NULL;

    if (strcmp(bin->op, "..") == 0) {
        /* 字符串拼接 */
        size_t sz = strlen(left) + strlen(right) + 64;
        result = (char *)malloc(sz);
        snprintf(result, sz, "vus_string_concat(%s, %s)", left, right);
    } else if (strcmp(bin->op, "==") == 0) {
        size_t sz = strlen(left) + strlen(right) + 128;
        result = (char *)malloc(sz);
        snprintf(result, sz,
            "vus_string_new((vus_to_int(%s, &_err) == vus_to_int(%s, &_err)) ? \"true\" : \"false\")",
            left, right);
    } else if (strcmp(bin->op, "!=") == 0) {
        size_t sz = strlen(left) + strlen(right) + 128;
        result = (char *)malloc(sz);
        snprintf(result, sz,
            "vus_string_new((vus_to_int(%s, &_err) != vus_to_int(%s, &_err)) ? \"true\" : \"false\")",
            left, right);
    } else if (strcmp(bin->op, "<") == 0) {
        size_t sz = strlen(left) + strlen(right) + 128;
        result = (char *)malloc(sz);
        snprintf(result, sz,
            "vus_string_new((vus_to_int(%s, &_err) < vus_to_int(%s, &_err)) ? \"true\" : \"false\")",
            left, right);
    } else if (strcmp(bin->op, ">") == 0) {
        size_t sz = strlen(left) + strlen(right) + 128;
        result = (char *)malloc(sz);
        snprintf(result, sz,
            "vus_string_new((vus_to_int(%s, &_err) > vus_to_int(%s, &_err)) ? \"true\" : \"false\")",
            left, right);
    } else if (strcmp(bin->op, "<=") == 0) {
        size_t sz = strlen(left) + strlen(right) + 128;
        result = (char *)malloc(sz);
        snprintf(result, sz,
            "vus_string_new((vus_to_int(%s, &_err) <= vus_to_int(%s, &_err)) ? \"true\" : \"false\")",
            left, right);
    } else if (strcmp(bin->op, ">=") == 0) {
        size_t sz = strlen(left) + strlen(right) + 128;
        result = (char *)malloc(sz);
        snprintf(result, sz,
            "vus_string_new((vus_to_int(%s, &_err) >= vus_to_int(%s, &_err)) ? \"true\" : \"false\")",
            left, right);
    } else if (strcmp(bin->op, "+") == 0) {
        size_t sz = strlen(left) + strlen(right) + 128;
        result = (char *)malloc(sz);
        snprintf(result, sz,
            "vus_to_string(vus_to_int(%s, &_err) + vus_to_int(%s, &_err))",
            left, right);
    } else if (strcmp(bin->op, "-") == 0) {
        size_t sz = strlen(left) + strlen(right) + 128;
        result = (char *)malloc(sz);
        snprintf(result, sz,
            "vus_to_string(vus_to_int(%s, &_err) - vus_to_int(%s, &_err))",
            left, right);
    } else if (strcmp(bin->op, "*") == 0) {
        size_t sz = strlen(left) + strlen(right) + 128;
        result = (char *)malloc(sz);
        snprintf(result, sz,
            "vus_to_string(vus_to_int(%s, &_err) * vus_to_int(%s, &_err))",
            left, right);
    } else if (strcmp(bin->op, "/") == 0) {
        size_t sz = strlen(left) + strlen(right) + 128;
        result = (char *)malloc(sz);
        snprintf(result, sz,
            "vus_to_string(vus_to_int(%s, &_err) / vus_to_int(%s, &_err))",
            left, right);
    } else if (strcmp(bin->op, "%") == 0) {
        size_t sz = strlen(left) + strlen(right) + 128;
        result = (char *)malloc(sz);
        snprintf(result, sz,
            "vus_to_string(vus_to_int(%s, &_err) %% vus_to_int(%s, &_err))",
            left, right);
    } else if (strcmp(bin->op, "and") == 0 || strcmp(bin->op, "和") == 0) {
        size_t sz = strlen(left) + strlen(right) + 128;
        result = (char *)malloc(sz);
        snprintf(result, sz,
            "vus_string_new((strcmp(vus_string_cstr(%s), \"true\") == 0 && strcmp(vus_string_cstr(%s), \"true\") == 0) ? \"true\" : \"false\")",
            left, right);
    } else if (strcmp(bin->op, "or") == 0 || strcmp(bin->op, "或") == 0) {
        size_t sz = strlen(left) + strlen(right) + 128;
        result = (char *)malloc(sz);
        snprintf(result, sz,
            "vus_string_new((strcmp(vus_string_cstr(%s), \"true\") == 0 || strcmp(vus_string_cstr(%s), \"true\") == 0) ? \"true\" : \"false\")",
            left, right);
    } else if (strcmp(bin->op, "&") == 0) {
        size_t sz = strlen(left) + strlen(right) + 128;
        result = (char *)malloc(sz);
        snprintf(result, sz,
            "vus_to_string(vus_to_int(%s, &_err) & vus_to_int(%s, &_err))",
            left, right);
    } else if (strcmp(bin->op, "|") == 0) {
        size_t sz = strlen(left) + strlen(right) + 128;
        result = (char *)malloc(sz);
        snprintf(result, sz,
            "vus_to_string(vus_to_int(%s, &_err) | vus_to_int(%s, &_err))",
            left, right);
    } else if (strcmp(bin->op, "^") == 0) {
        size_t sz = strlen(left) + strlen(right) + 128;
        result = (char *)malloc(sz);
        snprintf(result, sz,
            "vus_to_string(vus_to_int(%s, &_err) ^ vus_to_int(%s, &_err))",
            left, right);
    } else if (strcmp(bin->op, "<<") == 0) {
        size_t sz = strlen(left) + strlen(right) + 128;
        result = (char *)malloc(sz);
        snprintf(result, sz,
            "vus_to_string(vus_to_int(%s, &_err) << vus_to_int(%s, &_err))",
            left, right);
    } else if (strcmp(bin->op, ">>") == 0) {
        size_t sz = strlen(left) + strlen(right) + 128;
        result = (char *)malloc(sz);
        snprintf(result, sz,
            "vus_to_string(vus_to_int(%s, &_err) >> vus_to_int(%s, &_err))",
            left, right);
    } else {
        /* 兜底：未知运算符 */
        size_t sz = strlen(left) + strlen(right) + 64;
        result = (char *)malloc(sz);
        snprintf(result, sz, "vus_string_concat(%s, %s)", left, right);
    }

    free(left);
    free(right);
    return result;
}

static char *gen_expr_unary(GenBuf *buf, VusAstUnaryOp *un) {
    char *operand = gen_expr(buf, un->operand);
    char *result = NULL;

    if (strcmp(un->op, "not") == 0 || strcmp(un->op, "非") == 0) {
        size_t sz = strlen(operand) + 128;
        result = (char *)malloc(sz);
        snprintf(result, sz,
            "vus_string_new((strcmp(vus_string_cstr(%s), \"true\") == 0) ? \"false\" : \"true\")",
            operand);
    } else if (strcmp(un->op, "-") == 0) {
        size_t sz = strlen(operand) + 128;
        result = (char *)malloc(sz);
        snprintf(result, sz,
            "vus_to_string(-vus_to_int(%s, &_err))",
            operand);
    } else {
        result = strdup(operand);
    }

    free(operand);
    return result;
}

static char *gen_expr_call(GenBuf *buf, VusAstCall *call) {
    /* 处理内置函数 */
    if (strcmp(call->func_name, "打印") == 0) {
        if (call->args && call->args->count > 0) {
            char *arg = gen_expr(buf, call->args->items[0]);
            size_t sz = strlen(arg) + 64;
            char *result = (char *)malloc(sz);
            snprintf(result, sz, "vus_print(%s)", arg);
            free(arg);
            return result;
        }
        return strdup("vus_print(vus_string_new(\"\"))");
    }
    if (strcmp(call->func_name, "输入") == 0) {
        if (call->args && call->args->count > 0) {
            char *arg = gen_expr(buf, call->args->items[0]);
            size_t sz = strlen(arg) + 64;
            char *result = (char *)malloc(sz);
            snprintf(result, sz, "vus_input(%s)", arg);
            free(arg);
            return result;
        }
        return strdup("vus_input(vus_string_new(\"\"))");
    }
    if (strcmp(call->func_name, "转数字") == 0) {
        if (call->args && call->args->count > 0) {
            char *arg = gen_expr(buf, call->args->items[0]);
            size_t sz = strlen(arg) + 64;
            char *result = (char *)malloc(sz);
            snprintf(result, sz, "vus_to_int(%s, &_err)", arg);
            free(arg);
            return result;
        }
        return strdup("(int64_t)0");
    }
    if (strcmp(call->func_name, "转文本") == 0) {
        if (call->args && call->args->count > 0) {
            char *arg = gen_expr(buf, call->args->items[0]);
            size_t sz = strlen(arg) + 64;
            char *result = (char *)malloc(sz);
            snprintf(result, sz, "vus_to_string(%s)", arg);
            free(arg);
            return result;
        }
        return strdup("vus_to_string(0)");
    }

    /* 普通函数调用 */
    char san[256];
    gen_sanitize_name(call->func_name, san, sizeof(san));

    /* 构建参数数组 */
    size_t nargs = call->args ? call->args->count : 0;

    /* 先计算所有参数的表达式字符串 */
    char **arg_exprs = NULL;
    if (nargs > 0) {
        arg_exprs = (char **)calloc(nargs, sizeof(char *));
        for (size_t i = 0; i < nargs; i++) {
            arg_exprs[i] = gen_expr(buf, call->args->items[i]);
        }
    }

    /* 构建返回字符串：vus_函数名(args...) */
    /* 对于函数调用，生成 vus_函数名(args) 形式，参数为 VusString* 数组 */
    char args_buf[4096] = {0};
    size_t pos = 0;
    pos += snprintf(args_buf + pos, sizeof(args_buf) - pos, "({VusString* _vus_args[%zu];", nargs + 1);
    for (size_t i = 0; i < nargs; i++) {
        pos += snprintf(args_buf + pos, sizeof(args_buf) - pos,
            "_vus_args[%zu]=%s;", i + 1, arg_exprs[i]);
    }
    pos += snprintf(args_buf + pos, sizeof(args_buf) - pos,
        "vus_%s(_vus_args);_vus_args[0];})", san);

    /* 释放参数表达式 */
    for (size_t i = 0; i < nargs; i++) {
        free(arg_exprs[i]);
    }
    free(arg_exprs);

    return strdup(args_buf);
}

static char *gen_expr_identifier(GenBuf *buf, VusAstIdentifier *ident) {
    (void)buf;
    char san[256];
    gen_sanitize_name(ident->name, san, sizeof(san));
    size_t sz = strlen(san) + 16;
    char *result = (char *)malloc(sz);
    snprintf(result, sz, "vus_%s", san);
    return result;
}

static char *gen_expr_string(GenBuf *buf, VusAstString *str) {
    (void)buf;
    char escaped[4096];
    gen_string_escape(str->value, escaped, sizeof(escaped));
    size_t sz = strlen(escaped) + 64;
    char *result = (char *)malloc(sz);
    snprintf(result, sz, "vus_string_new(\"%s\")", escaped);
    return result;
}

static char *gen_expr_number(GenBuf *buf, VusAstNumber *num) {
    (void)buf;
    if (num->is_float) {
        /* 浮点数作为字符串处理 */
        size_t sz = strlen(num->value) + 64;
        char *result = (char *)malloc(sz);
        snprintf(result, sz, "vus_string_new(\"%s\")", num->value);
        return result;
    }
    size_t sz = strlen(num->value) + 64;
    char *result = (char *)malloc(sz);
    snprintf(result, sz, "vus_to_string(%s)", num->value);
    return result;
}

static char *gen_expr_bool(GenBuf *buf, VusAstBool *b) {
    (void)buf;
    if (b->value) {
        return strdup("vus_string_new(\"true\")");
    }
    return strdup("vus_string_new(\"false\")");
}

static char *gen_expr(GenBuf *buf, VusAstNode *node) {
    if (!node) return strdup("NULL");

    switch (node->type) {
        case VUS_AST_BINARY_OP:
            return gen_expr_binary(buf, (VusAstBinaryOp *)node);
        case VUS_AST_UNARY_OP:
            return gen_expr_unary(buf, (VusAstUnaryOp *)node);
        case VUS_AST_CALL:
            return gen_expr_call(buf, (VusAstCall *)node);
        case VUS_AST_IDENTIFIER:
            return gen_expr_identifier(buf, (VusAstIdentifier *)node);
        case VUS_AST_STRING_LITERAL:
            return gen_expr_string(buf, (VusAstString *)node);
        case VUS_AST_NUMBER_LITERAL:
            return gen_expr_number(buf, (VusAstNumber *)node);
        case VUS_AST_BOOL_LITERAL:
            return gen_expr_bool(buf, (VusAstBool *)node);
        case VUS_AST_NULL_LITERAL:
            return strdup("NULL");
        default:
            return strdup("NULL");
    }
}

/* ============ 语句生成 ============ */

static void gen_statement(GenBuf *buf, VusAstNode *node);

static void gen_stmt_assign(GenBuf *buf, VusAstAssign *assign) {
    char san[256];
    gen_sanitize_name(assign->target, san, sizeof(san));

    char *val = gen_expr(buf, assign->value);

    /* 使用临时变量避免重复计算（如 x = x + 1 时 vus_x 可能被释放） */
    gen_emit_linef(buf, "{ VusString* _tmp = %s;", val);
    gen_emit_linef(buf, "vus_ref(_tmp);");
    gen_emit_linef(buf, "vus_unref(vus_%s);", san);
    gen_emit_linef(buf, "vus_%s = _tmp; }", san);

    free(val);
}

static void gen_stmt_expr(GenBuf *buf, VusAstExprStmt *stmt) {
    char *expr = gen_expr(buf, stmt->expr);
    gen_emit_linef(buf, "%s;", expr);
    free(expr);
}

static void gen_stmt_if(GenBuf *buf, VusAstIf *if_stmt) {
    char *cond = gen_expr(buf, if_stmt->condition);

    gen_emit_linef(buf, "if (strcmp(vus_string_cstr(%s), \"true\") == 0) {", cond);
    free(cond);
    buf->indent++;

    if (if_stmt->then_body) {
        for (size_t i = 0; i < if_stmt->then_body->count; i++) {
            gen_statement(buf, if_stmt->then_body->items[i]);
        }
    }

    buf->indent--;
    gen_emit_line(buf, "}");

    /* elif 子句 */
    if (if_stmt->elif_conditions) {
        for (size_t i = 0; i < if_stmt->elif_conditions->count; i++) {
            char *econd = gen_expr(buf, if_stmt->elif_conditions->items[i]);
            gen_emit_linef(buf, "else if (strcmp(vus_string_cstr(%s), \"true\") == 0) {", econd);
            free(econd);
            buf->indent++;
            if (if_stmt->elif_bodies && i < if_stmt->elif_bodies->count) {
                /* elif_bodies 存储的是 VusAstList* 指针（每个 elif 的语句体） */
                VusAstList *body = (VusAstList *)if_stmt->elif_bodies->items[i];
                if (body) {
                    for (size_t j = 0; j < body->count; j++) {
                        gen_statement(buf, body->items[j]);
                    }
                }
            }
            buf->indent--;
            gen_emit_line(buf, "}");
        }
    }

    /* else 子句 */
    if (if_stmt->else_body && if_stmt->else_body->count > 0) {
        gen_emit_line(buf, "else {");
        buf->indent++;
        for (size_t i = 0; i < if_stmt->else_body->count; i++) {
            gen_statement(buf, if_stmt->else_body->items[i]);
        }
        buf->indent--;
        gen_emit_line(buf, "}");
    }
}

static void gen_stmt_for_range(GenBuf *buf, VusAstForRange *fr) {
    char san[256];
    gen_sanitize_name(fr->var_name, san, sizeof(san));

    char *start = gen_expr(buf, fr->start);
    char *end = gen_expr(buf, fr->end);

    gen_emit_linef(buf, "{");
    buf->indent++;
    gen_emit_linef(buf, "VusString* vus_%s = NULL;", san);
    gen_emit_linef(buf, "int64_t _start = vus_to_int(%s, &_err);", start);
    gen_emit_linef(buf, "int64_t _end = vus_to_int(%s, &_err);", end);
    gen_emit_linef(buf, "for (int64_t _i = _start; _i < _end; _i++) {");
    buf->indent++;
    gen_emit_linef(buf, "vus_ref(vus_to_string(_i));");
    gen_emit_linef(buf, "vus_unref(vus_%s);", san);
    gen_emit_linef(buf, "vus_%s = vus_to_string(_i);", san);

    if (fr->body) {
        for (size_t i = 0; i < fr->body->count; i++) {
            gen_statement(buf, fr->body->items[i]);
        }
    }

    buf->indent--;
    gen_emit_line(buf, "}");
    buf->indent--;
    gen_emit_line(buf, "}");

    free(start);
    free(end);
}

static void gen_stmt_for_each(GenBuf *buf, VusAstForEach *fe) {
    char san[256];
    gen_sanitize_name(fe->var_name, san, sizeof(san));

    char *iter = gen_expr(buf, fe->iterable);

    gen_emit_linef(buf, "{");
    buf->indent++;
    gen_emit_linef(buf, "VusString* vus_%s = NULL;", san);
    gen_emit_linef(buf, "VusList* _list = (VusList*)%s;", iter);
    gen_emit_linef(buf, "for (int _i = 0; _i < vus_list_len(_list); _i++) {");
    buf->indent++;
    gen_emit_linef(buf, "vus_ref(vus_list_get(_list, _i));");
    gen_emit_linef(buf, "vus_unref(vus_%s);", san);
    gen_emit_linef(buf, "vus_%s = (VusString*)vus_list_get(_list, _i);", san);

    if (fe->body) {
        for (size_t i = 0; i < fe->body->count; i++) {
            gen_statement(buf, fe->body->items[i]);
        }
    }

    buf->indent--;
    gen_emit_line(buf, "}");
    buf->indent--;
    gen_emit_line(buf, "}");

    free(iter);
}

static void gen_stmt_while(GenBuf *buf, VusAstWhile *wl) {
    char *cond = gen_expr(buf, wl->condition);

    gen_emit_linef(buf, "while (strcmp(vus_string_cstr(%s), \"true\") == 0) {", cond);
    free(cond);
    buf->indent++;

    if (wl->body) {
        for (size_t i = 0; i < wl->body->count; i++) {
            gen_statement(buf, wl->body->items[i]);
        }
    }

    buf->indent--;
    gen_emit_line(buf, "}");
}

static void gen_stmt_return(GenBuf *buf, VusAstReturn *ret) {
    if (ret->value) {
        char *val = gen_expr(buf, ret->value);
        gen_emit_linef(buf, "{ VusString* _tmp = %s;", val);
        gen_emit_line(buf, "VusString** _vus_params = (VusString**)_args;");
        gen_emit_line(buf, "vus_ref(_tmp);");
        gen_emit_line(buf, "vus_unref(_vus_params[0]);");
        gen_emit_line(buf, "_vus_params[0] = _tmp; }");
        gen_emit_line(buf, "return;");
        free(val);
    } else {
        gen_emit_line(buf, "return;");
    }
}

static void gen_stmt_break(GenBuf *buf) {
    gen_emit_line(buf, "break;");
}

static void gen_stmt_continue(GenBuf *buf) {
    gen_emit_line(buf, "continue;");
}

static void gen_stmt_global_decl(GenBuf *buf, VusAstGlobalDecl *gd) {
    char san[256];
    gen_sanitize_name(gd->name, san, sizeof(san));
    /* 全局声明在函数中只是标记，不需要生成代码 */
    /* 但需要确保变量在函数中可见 */
    gen_emit_linef(buf, "/* global: vus_%s */", san);
}

static void gen_stmt_try(GenBuf *buf, VusAstTry *try_stmt) {
    /* 简单的 try-catch 实现：使用错误码链 */
    gen_emit_line(buf, "{");
    buf->indent++;
    gen_emit_line(buf, "VusError* _vus_err = NULL;");
    gen_emit_line(buf, "int _vus_caught = 0;");

    /* try 块 */
    gen_emit_line(buf, "/* try block */");
    if (try_stmt->try_body) {
        for (size_t i = 0; i < try_stmt->try_body->count; i++) {
            gen_statement(buf, try_stmt->try_body->items[i]);
        }
    }

    /* 简单的 except 块 */
    if (try_stmt->except_bodies && try_stmt->except_bodies->count > 0) {
        /* 使用最后一条 except 作为兜底 */
        VusAstList *last_body = NULL;
        for (size_t i = 0; i < try_stmt->except_bodies->count; i++) {
            last_body = (VusAstList *)try_stmt->except_bodies->items[i];
        }
        if (last_body) {
            gen_emit_line(buf, "if (_vus_err) {");
            buf->indent++;
            gen_emit_line(buf, "vus_error_print(_vus_err);");
            gen_emit_line(buf, "vus_error_free(_vus_err);");
            gen_emit_line(buf, "_vus_err = NULL;");
            buf->indent--;
            gen_emit_line(buf, "}");
        }
    }

    buf->indent--;
    gen_emit_line(buf, "}");
}

static void gen_stmt_throw(GenBuf *buf, VusAstThrow *thr) {
    char *val = gen_expr(buf, thr->value);
    gen_emit_linef(buf, "_vus_err = vus_error_new(1, vus_string_cstr(%s), __LINE__, __func__);", val);
    gen_emit_linef(buf, "vus_unref(%s);", val);
    free(val);
}

static void gen_statement(GenBuf *buf, VusAstNode *node) {
    if (!node) return;

    switch (node->type) {
        case VUS_AST_ASSIGN:
            gen_stmt_assign(buf, (VusAstAssign *)node);
            break;
        case VUS_AST_EXPR_STMT:
            gen_stmt_expr(buf, (VusAstExprStmt *)node);
            break;
        case VUS_AST_IF:
            gen_stmt_if(buf, (VusAstIf *)node);
            break;
        case VUS_AST_FOR_RANGE:
            gen_stmt_for_range(buf, (VusAstForRange *)node);
            break;
        case VUS_AST_FOR_EACH:
            gen_stmt_for_each(buf, (VusAstForEach *)node);
            break;
        case VUS_AST_WHILE:
            gen_stmt_while(buf, (VusAstWhile *)node);
            break;
        case VUS_AST_RETURN:
            gen_stmt_return(buf, (VusAstReturn *)node);
            break;
        case VUS_AST_BREAK:
            gen_stmt_break(buf);
            break;
        case VUS_AST_CONTINUE:
            gen_stmt_continue(buf);
            break;
        case VUS_AST_GLOBAL_DECL:
            gen_stmt_global_decl(buf, (VusAstGlobalDecl *)node);
            break;
        case VUS_AST_TRY:
            gen_stmt_try(buf, (VusAstTry *)node);
            break;
        case VUS_AST_THROW:
            gen_stmt_throw(buf, (VusAstThrow *)node);
            break;
        default:
            break;
    }
}

/* ============ 函数定义生成 ============ */

static void gen_function(GenBuf *buf, VusAstFunctionDef *func) {
    char san[256];
    gen_sanitize_name(func->name, san, sizeof(san));

    /* 函数注释 */
    gen_emit_linef(buf, "/* VUS function: %s */", func->name);

    /* 函数签名：void vus_xxx(void* _args) */
    gen_emit_linef(buf, "void vus_%s(void* _args) {", san);
    buf->indent++;

    /* 参数提取 */
    size_t nparams = func->params ? func->params->count : 0;
    if (nparams > 0) {
        gen_emit_line(buf, "VusString** _vus_params = (VusString**)_args;");
        for (size_t i = 0; i < nparams; i++) {
            VusAstNode *pnode = func->params->items[i];
            if (pnode->type == VUS_AST_PARAM) {
                VusAstParam *param = (VusAstParam *)pnode;
                char psan[256];
                gen_sanitize_name(param->name, psan, sizeof(psan));
                gen_emit_linef(buf, "VusString* vus_%s = _vus_params[%zu];", psan, i + 1);
                gen_emit_linef(buf, "vus_ref(vus_%s);", psan);
            } else if (pnode->type == VUS_AST_PARAM_DEFAULT) {
                VusAstParamDefault *param = (VusAstParamDefault *)pnode;
                char psan[256];
                gen_sanitize_name(param->name, psan, sizeof(psan));
                gen_emit_linef(buf, "VusString* vus_%s = _vus_params[%zu] ? _vus_params[%zu] : NULL;", psan, i + 1, i + 1);
                gen_emit_linef(buf, "if (vus_%s) vus_ref(vus_%s);", psan, psan);
            }
        }
    }

    /* 返回值变量 */
    gen_emit_line(buf, "VusString* _vus_result = NULL;");
    gen_emit_line(buf, "int _err = 0;");

    /* 函数体 */
    if (func->body) {
        for (size_t i = 0; i < func->body->count; i++) {
            gen_statement(buf, func->body->items[i]);
        }
    }

    /* 设置返回值（在 args[0] 中） */
    gen_emit_line(buf, "if (_vus_result) {");
    buf->indent++;
    gen_emit_line(buf, "VusString** _vus_params = (VusString**)_args;");
    gen_emit_line(buf, "vus_ref(_vus_result);");
    gen_emit_line(buf, "vus_unref(_vus_params[0]);");
    gen_emit_line(buf, "_vus_params[0] = _vus_result;");
    buf->indent--;
    gen_emit_line(buf, "}");

    buf->indent--;
    gen_emit_line(buf, "}\n");
}

/* ============ 主函数生成 ============ */

static void gen_main_function(GenBuf *buf, VusAstProgram *program) {
    gen_emit_line(buf, "int main(void) {");
    buf->indent++;

    gen_emit_line(buf, "int _err = 0;");
    gen_emit_line(buf, "VusError* _vus_err = NULL;");

    /* 遍历所有顶层语句（跳过函数定义，它们已单独生成） */
    if (program->statements) {
        for (size_t i = 0; i < program->statements->count; i++) {
            VusAstNode *node = program->statements->items[i];
            if (node->type != VUS_AST_FUNCTION_DEF) {
                gen_statement(buf, node);
            }
        }
    }

    gen_emit_line(buf, "return 0;");
    buf->indent--;
    gen_emit_line(buf, "}");
}

/* ============ 公开 API 实现 ============ */

char *vus_generate_c(VusAstProgram *program, VusConfig *config) {
    if (!program || !config) return NULL;

    GenBuf *buf = gen_buf_new();
    if (!buf) return NULL;

    /* 文件头注释 */
    gen_emit(buf, "/*\n");
    gen_emit(buf, " * Generated by VUS Compiler v0.1\n");
    if (config->name[0]) {
        gen_emitf(buf, " * Project: %s\n", config->name);
    }
    gen_emit(buf, " */\n\n");

    /* 标准头文件 */
    gen_emit(buf, "#include <stdio.h>\n");
    gen_emit(buf, "#include <stdlib.h>\n");
    gen_emit(buf, "#include <string.h>\n");
    gen_emit(buf, "#include <stdint.h>\n");
    gen_emit(buf, "#include <stdbool.h>\n\n");

    /* 运行时头文件 */
    if (config->rt_dir[0]) {
        gen_emit(buf, "#include \"libvus_rt.h\"\n\n");
    }

    /* 全局变量声明 */
    if (program->statements) {
        for (size_t i = 0; i < program->statements->count; i++) {
            VusAstNode *node = program->statements->items[i];
            if (node->type == VUS_AST_ASSIGN) {
                VusAstAssign *assign = (VusAstAssign *)node;
                char san[256];
                gen_sanitize_name(assign->target, san, sizeof(san));
                gen_emitf(buf, "VusString* vus_%s = NULL;\n", san);
            } else if (node->type == VUS_AST_GLOBAL_DECL) {
                VusAstGlobalDecl *gd = (VusAstGlobalDecl *)node;
                char san[256];
                gen_sanitize_name(gd->name, san, sizeof(san));
                gen_emitf(buf, "VusString* vus_%s = NULL;\n", san);
            }
        }
    }
    gen_emit(buf, "\n");

    /* 函数定义 */
    if (program->statements) {
        for (size_t i = 0; i < program->statements->count; i++) {
            VusAstNode *node = program->statements->items[i];
            if (node->type == VUS_AST_FUNCTION_DEF) {
                gen_function(buf, (VusAstFunctionDef *)node);
            }
        }
    }

    /* 主函数 */
    gen_main_function(buf, program);

    char *result = strdup(buf->data);
    free(buf->data);
    free(buf);
    return result;
}

int vus_compile_c(const char *c_source_path, const char *output_path,
                  VusConfig *config, char *error_msg, size_t error_size) {
    if (!c_source_path || !output_path || !config) {
        if (error_msg && error_size > 0) {
            snprintf(error_msg, error_size, "Invalid arguments to vus_compile_c");
        }
        return -1;
    }

    /* 确定优化级别 */
    const char *opt_level = "-O2";
    if (config->optimization[0]) {
        if (strcmp(config->optimization, "体积") == 0) {
            opt_level = "-Os";
        } else if (strcmp(config->optimization, "调试") == 0) {
            opt_level = "-O0 -g";
        }
    }

    /* 确定 rt 源文件路径 */
    char rt_source[2048];
    if (config->rt_dir[0]) {
        snprintf(rt_source, sizeof(rt_source), "%s/libvus_rt.c", config->rt_dir);
    } else {
        rt_source[0] = '\0';
    }

    /* 构建 GCC 命令 */
    char cmd[8192];
    int n = snprintf(cmd, sizeof(cmd),
        "gcc %s -g -I\"%s\" \"%s\" \"%s\" -o \"%s\" -lm 2>&1",
        opt_level,
        config->rt_dir,
        c_source_path,
        rt_source,
        output_path);

    if (n >= (int)sizeof(cmd)) {
        if (error_msg && error_size > 0) {
            snprintf(error_msg, error_size, "Command line too long");
        }
        return -1;
    }

    /* 执行编译 */
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        if (error_msg && error_size > 0) {
            snprintf(error_msg, error_size, "Failed to execute GCC: popen() failed");
        }
        return -1;
    }

    /* 读取错误输出 */
    char err_buf[4096] = {0};
    size_t total_read = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        size_t llen = strlen(line);
        if (total_read + llen < sizeof(err_buf) - 1) {
            memcpy(err_buf + total_read, line, llen);
            total_read += llen;
        }
    }

    int status = pclose(fp);

    if (status != 0) {
        if (error_msg && error_size > 0) {
            snprintf(error_msg, error_size, "GCC compilation failed:\n%s", err_buf);
        }
        return status;
    }

    return 0;
}

void vus_generate_free(char *code) {
    free(code);
}