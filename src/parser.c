/*
 * parser.c — VUS 语法分析器实现
 *
 * 递归下降解析器，将 Token 流转换为 AST。
 * 支持函数风格（中英别名关键字）。
 *
 * 表达式优先级（从低到高）：
 *   logical_or → logical_and → comparison → concat → additive → multiplicative → unary → primary
 */

#define _GNU_SOURCE
#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ==================================================================
 * 本地导入/FromImport AST 节点结构定义
 * 因 ast.h 中仅声明了枚举值 VUS_AST_IMPORT / VUS_AST_FROM_IMPORT
 * 但未提供对应的创建函数，此处自行定义供 parse 使用。
 * ================================================================== */
typedef struct {
    VusAstNodeType type;   /* VUS_AST_IMPORT */
    int            line;
    int            column;
    VusAstList    *names;  /* Identifier 节点列表 */
} ParserAstImport;

typedef struct {
    VusAstNodeType type;   /* VUS_AST_FROM_IMPORT */
    int            line;
    int            column;
    char          *module;
    VusAstList    *names;  /* Identifier 节点列表 */
} ParserAstFromImport;

/* ==================================================================
 * 内部辅助函数声明
 * ================================================================== */
static void        parser_advance(VusParser *parser);
static VusToken   *parser_peek(VusParser *parser);
static VusToken   *parser_peek_next(VusParser *parser);
static VusToken   *parser_expect(VusParser *parser, VusTokenType type);
static int         parser_match(VusParser *parser, VusTokenType type);
static void        parser_skip_newlines(VusParser *parser);
static void        parser_set_error(VusParser *parser, const char *fmt, ...);

/* 递归下降解析函数 */
static VusAstList *parse_statements(VusParser *parser);
static VusAstNode *parse_statement(VusParser *parser);
static VusAstNode *parse_function_def(VusParser *parser);
static VusAstList *parse_params(VusParser *parser);
static VusAstNode *parse_if_stmt(VusParser *parser);
static VusAstNode *parse_for_stmt(VusParser *parser);
static VusAstNode *parse_while_stmt(VusParser *parser);
static VusAstNode *parse_try_stmt(VusParser *parser);
static VusAstNode *parse_return_stmt(VusParser *parser);
static VusAstNode *parse_import_stmt(VusParser *parser);
static VusAstNode *parse_from_import_stmt(VusParser *parser);
static VusAstNode *parse_break_stmt(VusParser *parser);
static VusAstNode *parse_continue_stmt(VusParser *parser);
static VusAstNode *parse_throw_stmt(VusParser *parser);
static VusAstNode *parse_global_stmt(VusParser *parser);
static VusAstNode *parse_assign_or_expr(VusParser *parser);
static VusAstNode *parse_struct_def(VusParser *parser);

/* 表达式解析 */
static VusAstNode *parse_expr(VusParser *parser);
static VusAstNode *parse_logical_or(VusParser *parser);
static VusAstNode *parse_logical_and(VusParser *parser);
static VusAstNode *parse_comparison(VusParser *parser);
static VusAstNode *parse_concat(VusParser *parser);
static VusAstNode *parse_additive(VusParser *parser);
static VusAstNode *parse_multiplicative(VusParser *parser);
static VusAstNode *parse_unary(VusParser *parser);
static VusAstNode *parse_primary(VusParser *parser);
static VusAstList *parse_call_args(VusParser *parser);
/* 位运算 */
static VusAstNode *parse_bitwise_or(VusParser *parser);
static VusAstNode *parse_bitwise_xor(VusParser *parser);
static VusAstNode *parse_bitwise_and(VusParser *parser);
static VusAstNode *parse_shift(VusParser *parser);

/* ==================================================================
 * 辅助函数实现
 * ================================================================== */

static void parser_advance(VusParser *parser) {
    if (parser->pos < parser->token_count) {
        parser->pos++;
    }
}

static VusToken *parser_peek(VusParser *parser) {
    if (parser->pos >= parser->token_count) {
        return NULL;
    }
    return &parser->tokens[parser->pos];
}

static VusToken *parser_peek_next(VusParser *parser) {
    if (parser->pos + 1 >= parser->token_count) {
        return NULL;
    }
    return &parser->tokens[parser->pos + 1];
}

static VusToken *parser_expect(VusParser *parser, VusTokenType type) {
    VusToken *token = parser_peek(parser);
    if (!token || token->type != type) {
        const char *expected = vus_token_type_name(type);
        if (token) {
            const char *got = vus_token_type_name(token->type);
            parser_set_error(parser, "期望 %s，但遇到 %s（第 %d 行第 %d 列）",
                             expected, got, token->line, token->column);
        } else {
            parser_set_error(parser, "期望 %s，但遇到文件结尾", expected);
        }
        return NULL;
    }
    parser_advance(parser);
    return token;
}

static int parser_match(VusParser *parser, VusTokenType type) {
    VusToken *token = parser_peek(parser);
    if (token && token->type == type) {
        parser_advance(parser);
        return 1;
    }
    return 0;
}

static void parser_skip_newlines(VusParser *parser) {
    while (1) {
        VusToken *token = parser_peek(parser);
        if (!token || token->type != VUS_TOKEN_NEWLINE) {
            break;
        }
        parser_advance(parser);
    }
}

static void parser_set_error(VusParser *parser, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(parser->error_msg, sizeof(parser->error_msg), fmt, args);
    va_end(args);
    parser->error = 1;
}

/* ==================================================================
 * 语句块解析
 * ================================================================== */

/*
 * 解析语句列表，直到遇到 DEDENT 或 EOF。
 */
static VusAstList *parse_statements(VusParser *parser) {
    VusAstList *list = vus_ast_list_new();
    if (!list) return NULL;

    while (1) {
        parser_skip_newlines(parser);

        VusToken *token = parser_peek(parser);
        if (!token) break;

        /* 块终止条件 */
        if (token->type == VUS_TOKEN_DEDENT) break;
        if (token->type == VUS_TOKEN_EOF) break;

        VusAstNode *stmt = parse_statement(parser);
        if (parser->error) {
            vus_ast_list_free(list);
            return NULL;
        }

        if (stmt) {
            vus_ast_list_push(list, stmt);
        }
    }

    return list;
}

/*
 * 解析单条语句，根据当前 Token 类型分发到具体解析函数。
 */
static VusAstNode *parse_statement(VusParser *parser) {
    VusToken *token = parser_peek(parser);
    if (!token) return NULL;

    switch (token->type) {
        /* ===== 函数定义 ===== */
        case VUS_TOKEN_DEF:
        case VUS_TOKEN_CN_DEF:
            return parse_function_def(parser);

        /* ===== 条件语句 ===== */
        case VUS_TOKEN_IF:
        case VUS_TOKEN_CN_IF:
            return parse_if_stmt(parser);

        /* ===== 循环语句 ===== */
        case VUS_TOKEN_FOR:
        case VUS_TOKEN_CN_FOR:
            return parse_for_stmt(parser);

        /* ===== While 循环 ===== */
        case VUS_TOKEN_WHILE:
        case VUS_TOKEN_CN_WHILE:
            return parse_while_stmt(parser);

        /* ===== 异常处理 ===== */
        case VUS_TOKEN_TRY:
        case VUS_TOKEN_CN_TRY:
            return parse_try_stmt(parser);

        /* ===== 返回语句 ===== */
        case VUS_TOKEN_RETURN:
        case VUS_TOKEN_CN_RETURN:
            return parse_return_stmt(parser);

        /* ===== 导入语句 ===== */
        case VUS_TOKEN_IMPORT:
        case VUS_TOKEN_CN_IMPORT:
            return parse_import_stmt(parser);

        /* ===== From-Import 语句 ===== */
        case VUS_TOKEN_FROM:
        case VUS_TOKEN_CN_FROM:
            return parse_from_import_stmt(parser);

        /* ===== Break ===== */
        case VUS_TOKEN_BREAK:
        case VUS_TOKEN_CN_BREAK:
            return parse_break_stmt(parser);

        /* ===== Continue ===== */
        case VUS_TOKEN_CONTINUE:
        case VUS_TOKEN_CN_CONTINUE:
            return parse_continue_stmt(parser);

        /* ===== Throw ===== */
        case VUS_TOKEN_THROW:
        case VUS_TOKEN_CN_THROW:
            return parse_throw_stmt(parser);

        /* ===== Global ===== */
        case VUS_TOKEN_GLOBAL:
        case VUS_TOKEN_CN_GLOBAL:
            return parse_global_stmt(parser);

        /* ===== 结构体定义 ===== */
        case VUS_TOKEN_STRUCT:
        case VUS_TOKEN_CN_STRUCT:
            return parse_struct_def(parser);

        /* ===== 赋值或表达式 ===== */
        default:
            return parse_assign_or_expr(parser);
    }
}

/* ==================================================================
 * 函数定义解析
 * ==================================================================
 *
 * 函数风格（def/定义）:
 *   def name(params):\n    body
 *   def name(params) -> type:\n    body
 *
 * 中文别名:
 *   定义 name(params):\n    body
 */
static VusAstNode *parse_function_def(VusParser *parser) {
    VusToken *keyword = parser_peek(parser); /* 消耗 def/定义 */
    int line = keyword->line;
    int col = keyword->column;
    parser_advance(parser);

    /* 函数名 */
    VusToken *name_token = parser_expect(parser, VUS_TOKEN_IDENTIFIER);
    if (!name_token) return NULL;
    char *name = strndup(name_token->start, name_token->length);

    /* 泛型类型参数 <T> 或 <T, U> */
    VusAstList *type_params = NULL;
    if (parser_peek(parser) && parser_peek(parser)->type == VUS_TOKEN_LT) {
        type_params = vus_ast_list_new();
        parser_advance(parser); /* skip < */
        while (1) {
            VusToken *tp_tok = parser_expect(parser, VUS_TOKEN_IDENTIFIER);
            if (!tp_tok) { free(name); vus_ast_list_free(type_params); return NULL; }
            char *tp_name = strndup(tp_tok->start, tp_tok->length);
            VusAstParam *tp = vus_ast_param_new(tp_name, NULL, tp_tok->line, tp_tok->column);
            free(tp_name);
            vus_ast_list_push(type_params, (VusAstNode *)tp);

            if (parser_match(parser, VUS_TOKEN_COMMA)) {
                continue;
            } else if (parser_match(parser, VUS_TOKEN_GT)) {
                break;
            } else {
                parser_set_error(parser, "期望 '>' 或 ',' 在泛型参数中（第 %d 行第 %d 列）",
                                 tp_tok->line, tp_tok->column);
                free(name); vus_ast_list_free(type_params); return NULL;
            }
        }
    }

    /* 参数列表 */
    VusAstList *params = NULL;
    if (parser_match(parser, VUS_TOKEN_LPAREN)) {
        params = parse_params(parser);
        if (parser->error) { free(name); vus_ast_list_free(type_params); return NULL; }
        parser_expect(parser, VUS_TOKEN_RPAREN);
        if (parser->error) { free(name); vus_ast_list_free(type_params); vus_ast_list_free(params); return NULL; }
    } else {
        params = vus_ast_list_new();
    }

    /* 期望冒号 */
    parser_expect(parser, VUS_TOKEN_COLON);
    if (parser->error) { free(name); vus_ast_list_free(type_params); vus_ast_list_free(params); return NULL; }

    /* 解析函数体 */
    parser_skip_newlines(parser);

    VusAstList *body = NULL;

    /* 期望 INDENT，然后解析到 DEDENT */
    VusToken *indent = parser_expect(parser, VUS_TOKEN_INDENT);
    if (!indent) { free(name); vus_ast_list_free(type_params); vus_ast_list_free(params); return NULL; }

    parser->in_function = 1;  /* 进入函数体作用域 */

    body = parse_statements(parser);
    if (parser->error) { free(name); vus_ast_list_free(type_params); vus_ast_list_free(params); vus_ast_list_free(body); return NULL; }

    parser_expect(parser, VUS_TOKEN_DEDENT);
    if (parser->error) { free(name); vus_ast_list_free(type_params); vus_ast_list_free(params); vus_ast_list_free(body); return NULL; }

    parser->in_function = 0;  /* 退出函数体作用域 */

    VusAstFunctionDef *node = vus_ast_func_def_new(name, type_params, params, body, line, col);
    free(name);
    return (VusAstNode*)node;
}

/*
 * 解析参数列表：param, param, ... 或 param:type, param=default, ...
 */
static VusAstList *parse_params(VusParser *parser) {
    VusAstList *list = vus_ast_list_new();
    if (!list) return NULL;

    /* 空参数列表 */
    if (parser_peek(parser)->type == VUS_TOKEN_RPAREN) {
        return list;
    }

    while (1) {
        VusToken *name_token = parser_expect(parser, VUS_TOKEN_IDENTIFIER);
        if (!name_token) { vus_ast_list_free(list); return NULL; }
        char *param_name = strndup(name_token->start, name_token->length);
        int line = name_token->line;
        int col = name_token->column;

        /* 类型注解 */
        char *type_ann = NULL;
        if (parser_match(parser, VUS_TOKEN_COLON)) {
            VusToken *type_token = parser_expect(parser, VUS_TOKEN_IDENTIFIER);
            if (!type_token) { free(param_name); vus_ast_list_free(list); return NULL; }
            type_ann = strndup(type_token->start, type_token->length);
        }

        /* 默认值 */
        if (parser_match(parser, VUS_TOKEN_ASSIGN)) {
            VusAstNode *default_val = parse_expr(parser);
            if (!default_val) { free(param_name); free(type_ann); vus_ast_list_free(list); return NULL; }
            VusAstParamDefault *pdef = vus_ast_param_default_new(param_name, type_ann, default_val, line, col);
            free(param_name);
            free(type_ann);
            vus_ast_list_push(list, (VusAstNode*)pdef);
        } else {
            VusAstParam *p = vus_ast_param_new(param_name, type_ann, line, col);
            free(param_name);
            free(type_ann);
            vus_ast_list_push(list, (VusAstNode*)p);
        }

        if (!parser_match(parser, VUS_TOKEN_COMMA)) {
            break;
        }
    }

    return list;
}

/* ==================================================================
 * 条件语句解析
 * ==================================================================
 *
 * 函数风格:
 *   if cond:\n    body
 *   elif cond:\n    body
 *   else:\n    body
 *
 * 中文别名:
 *   如果 cond:\n    body
 *   否则如果 cond:\n    body
 *   否则:\n    body
 */
static VusAstNode *parse_if_stmt(VusParser *parser) {
    VusToken *keyword = parser_peek(parser); /* 消耗 if/如果 */
    int line = keyword->line;
    int col = keyword->column;
    parser_advance(parser);

    /* 条件表达式 */
    VusAstNode *cond = parse_expr(parser);
    if (!cond) return NULL;

    /* 期望冒号 */
    parser_expect(parser, VUS_TOKEN_COLON);
    if (parser->error) return NULL;

    /* 解析 then 体 */
    parser_skip_newlines(parser);
    VusToken *indent = parser_expect(parser, VUS_TOKEN_INDENT);
    if (!indent) return NULL;

    VusAstList *then_body = parse_statements(parser);
    if (parser->error) return NULL;

    /* 消耗 DEDENT */
    parser_expect(parser, VUS_TOKEN_DEDENT);
    if (parser->error) return NULL;

    VusAstIf *if_node = vus_ast_if_new(cond, then_body, line, col);
    if (!if_node) return NULL;

    /* 解析 elif 子句 */
    while (1) {
        parser_skip_newlines(parser);
        VusToken *next = parser_peek(parser);
        if (!next) break;

        if (next->type != VUS_TOKEN_ELIF && next->type != VUS_TOKEN_CN_ELIF) break;

        parser_advance(parser); /* 消耗 elif */

        VusAstNode *elif_cond = parse_expr(parser);
        if (!elif_cond) return NULL;

        parser_expect(parser, VUS_TOKEN_COLON);
        if (parser->error) return NULL;

        parser_skip_newlines(parser);
        VusToken *elif_indent = parser_expect(parser, VUS_TOKEN_INDENT);
        if (!elif_indent) return NULL;

        VusAstList *elif_body = parse_statements(parser);
        if (parser->error) return NULL;

        parser_expect(parser, VUS_TOKEN_DEDENT);
        if (parser->error) return NULL;

        vus_ast_if_add_elif(if_node, elif_cond, elif_body);
    }

    /* 解析 else 子句 */
    {
        parser_skip_newlines(parser);
        VusToken *next = parser_peek(parser);
        if (next && (next->type == VUS_TOKEN_ELSE || next->type == VUS_TOKEN_CN_ELSE)) {
            parser_advance(parser); /* 消耗 else */

            parser_expect(parser, VUS_TOKEN_COLON);
            if (parser->error) return NULL;

            parser_skip_newlines(parser);
            VusToken *else_indent = parser_expect(parser, VUS_TOKEN_INDENT);
            if (!else_indent) return NULL;

            VusAstList *else_body = parse_statements(parser);
            if (parser->error) return NULL;

            parser_expect(parser, VUS_TOKEN_DEDENT);
            if (parser->error) return NULL;

            vus_ast_if_set_else(if_node, else_body);
        }
    }

    return (VusAstNode*)if_node;
}

/* ==================================================================
 * For 循环解析
 * ==================================================================
 *
 * 函数风格:
 *   for i in range(1, 10):\n    body       — 数值 for-range
 *   for 元素 in 列表:\n    body              — foreach
 *   循环 i 从 1 到 10:\n    body              — 中文 for-range
 *   循环 元素 在 列表:\n    body               — 中文 foreach
 */
static VusAstNode *parse_for_stmt(VusParser *parser) {
    VusToken *keyword = parser_peek(parser); /* 消耗 for/循环 */
    int line = keyword->line;
    int col = keyword->column;
    parser_advance(parser);

    /* 函数风格 / 中文风格 for 循环 */
    /* 需要区分 for-range 和 foreach */
    VusToken *var_token = parser_expect(parser, VUS_TOKEN_IDENTIFIER);
    if (!var_token) return NULL;
    char *var_name = strndup(var_token->start, var_token->length);

    VusToken *next = parser_peek(parser);
    if (!next) { free(var_name); return NULL; }

    /* 判断是 for-range 还是 foreach */

    /* 模式: identifier in range(...)  → for-range
     * 模式: identifier in expr        → foreach
     * 模式: identifier 从 expr 到 expr → for-range (中文)
     * 模式: identifier 在 expr         → foreach (中文)
     */
    int is_range = 0;
    int is_foreach = 0;

    if (next->type == VUS_TOKEN_IN) {
        /* for i in ... */
        parser_advance(parser); /* 消耗 in */

        /* 检查是否 range(...) */
        VusToken *after_in = parser_peek(parser);
        if (after_in && after_in->type == VUS_TOKEN_IDENTIFIER &&
            after_in->length == 5 && strncmp(after_in->start, "range", 5) == 0) {
            /* 检查后面是否是 ( */
            VusToken *maybe_lparen = parser_peek_next(parser);
            if (maybe_lparen && maybe_lparen->type == VUS_TOKEN_LPAREN) {
                is_range = 1;
                parser_advance(parser); /* 消耗 "range" */
                parser_advance(parser); /* 消耗 ( */
            } else {
                is_foreach = 1;
            }
        } else {
            is_foreach = 1;
        }
    } else if (next->type == VUS_TOKEN_CN_FROM) {
        /* 循环 i 从 ... */
        parser_advance(parser); /* 消耗 从 */
        is_range = 1;
    } else if (next->type == VUS_TOKEN_CN_IN) {
        /* 循环 元素 在 ... */
        parser_advance(parser); /* 消耗 在 */
        is_foreach = 1;
    } else {
        parser_set_error(parser, "无效的 for 循环语法（第 %d 行第 %d 列）", line, parser_peek(parser)->column);
        free(var_name);
        return NULL;
    }

    if (is_range) {
        /* for-range */
        VusAstNode *start = NULL;
        VusAstNode *end = NULL;

        if (keyword->type == VUS_TOKEN_FOR || keyword->type == VUS_TOKEN_CN_FOR) {
            /* range(expr, expr) 或 从 expr 到 expr */
            if (next->type == VUS_TOKEN_IN) {
                start = parse_expr(parser);
                if (!start) { free(var_name); return NULL; }

                parser_expect(parser, VUS_TOKEN_COMMA);
                if (parser->error) { free(var_name); return NULL; }

                end = parse_expr(parser);
                if (!end) { free(var_name); return NULL; }

                parser_expect(parser, VUS_TOKEN_RPAREN);
                if (parser->error) { free(var_name); return NULL; }
            } else {
                /* 从 expr 到 expr 模式 */
                start = parse_expr(parser);
                if (!start) { free(var_name); return NULL; }

                /* 期望 到 */
                parser_expect(parser, VUS_TOKEN_CN_TO);
                if (parser->error) { free(var_name); return NULL; }

                end = parse_expr(parser);
                if (!end) { free(var_name); return NULL; }
            }
        } else {
            parser_set_error(parser, "内部错误：无法识别的 for-range 风格（第 %d 行第 %d 列）", line, parser_peek(parser)->column);
            free(var_name);
            return NULL;
        }

        /* 期望冒号 */
        parser_expect(parser, VUS_TOKEN_COLON);
        if (parser->error) { free(var_name); return NULL; }

        /* 解析 body */
        parser_skip_newlines(parser);
        VusToken *indent = parser_expect(parser, VUS_TOKEN_INDENT);
        if (!indent) { free(var_name); return NULL; }

        VusAstList *body = parse_statements(parser);
        if (parser->error) { free(var_name); return NULL; }

        parser_expect(parser, VUS_TOKEN_DEDENT);
        if (parser->error) { free(var_name); return NULL; }

        VusAstForRange *node = vus_ast_for_range_new(var_name, start, end, body, line, col);
        free(var_name);
        return (VusAstNode*)node;
    }

    if (is_foreach) {
        /* foreach: 解析可迭代对象表达式 */
        VusAstNode *iterable = parse_expr(parser);
        if (!iterable) { free(var_name); return NULL; }

        /* 期望冒号 */
        parser_expect(parser, VUS_TOKEN_COLON);
        if (parser->error) { free(var_name); return NULL; }

        /* 解析 body */
        parser_skip_newlines(parser);
        VusToken *indent = parser_expect(parser, VUS_TOKEN_INDENT);
        if (!indent) { free(var_name); return NULL; }

        VusAstList *body = parse_statements(parser);
        if (parser->error) { free(var_name); return NULL; }

        parser_expect(parser, VUS_TOKEN_DEDENT);
        if (parser->error) { free(var_name); return NULL; }

        VusAstForEach *node = vus_ast_for_each_new(var_name, iterable, body, line, col);
        free(var_name);
        return (VusAstNode*)node;
    }

    free(var_name);
    return NULL;
}

/* ==================================================================
 * While 循环解析
 * ==================================================================
 *
 * 函数风格:
 *   while cond:\n    body
 *   当循环 cond:\n    body
 */
static VusAstNode *parse_while_stmt(VusParser *parser) {
    VusToken *keyword = parser_peek(parser); /* 消耗 while/当循环 */
    int line = keyword->line;
    int col = keyword->column;
    parser_advance(parser);

    /* 解析条件表达式 */
    VusAstNode *cond = parse_expr(parser);
    if (!cond) return NULL;

    /* 期望冒号 */
    parser_expect(parser, VUS_TOKEN_COLON);
    if (parser->error) return NULL;

    /* 解析 body */
    parser_skip_newlines(parser);
    VusToken *indent = parser_expect(parser, VUS_TOKEN_INDENT);
    if (!indent) return NULL;

    VusAstList *body = parse_statements(parser);
    if (parser->error) return NULL;

    parser_expect(parser, VUS_TOKEN_DEDENT);
    if (parser->error) return NULL;

    VusAstWhile *node = vus_ast_while_new(cond, body, line, col);
    return (VusAstNode*)node;
}

/* ==================================================================
 * Try 语句解析
 * ==================================================================
 *
 * 函数风格:
 *   try:\n    body except Type:\n    body
 */
static VusAstNode *parse_try_stmt(VusParser *parser) {
    VusToken *keyword = parser_peek(parser); /* 消耗 try/尝试 */
    int line = keyword->line;
    int col = keyword->column;
    parser_advance(parser);

    /* 期望冒号 */
    parser_expect(parser, VUS_TOKEN_COLON);
    if (parser->error) return NULL;

    /* 解析 try body */
    parser_skip_newlines(parser);
    VusToken *indent = parser_expect(parser, VUS_TOKEN_INDENT);
    if (!indent) return NULL;

    VusAstList *try_body = parse_statements(parser);
    if (parser->error) return NULL;

    parser_expect(parser, VUS_TOKEN_DEDENT);
    if (parser->error) return NULL;

    VusAstTry *try_node = vus_ast_try_new(try_body, line, col);
    if (!try_node) return NULL;

    /* 解析 except 子句 */
    while (1) {
        parser_skip_newlines(parser);
        VusToken *next = parser_peek(parser);
        if (!next) break;

        int is_except = 0;
        if (next->type == VUS_TOKEN_EXCEPT || next->type == VUS_TOKEN_CN_EXCEPT) {
            is_except = 1;
        }

        if (!is_except) break;

        parser_advance(parser); /* 消耗 except */

        /* 异常类型（可选） */
        char *except_type = NULL;
        VusToken *type_token = parser_peek(parser);
        if (type_token && type_token->type == VUS_TOKEN_IDENTIFIER &&
            parser_peek_next(parser) &&
            (parser_peek_next(parser)->type == VUS_TOKEN_COLON ||
             parser_peek_next(parser)->type == VUS_TOKEN_NEWLINE ||
             parser_peek_next(parser)->type == VUS_TOKEN_INDENT)) {
            /* 有类型名：except Type: */
            parser_advance(parser);
            except_type = strndup(type_token->start, type_token->length);
        } else if (type_token && type_token->type == VUS_TOKEN_LPAREN) {
            /* except (Type1, Type2): */
            parser_advance(parser); /* 消耗 ( */
            /* 取第一个类型作为代表 */
            VusToken *first_type = parser_expect(parser, VUS_TOKEN_IDENTIFIER);
            if (first_type) {
                except_type = strndup(first_type->start, first_type->length);
            }
            /* 跳过剩余参数 */
            while (parser_peek(parser) && parser_peek(parser)->type != VUS_TOKEN_RPAREN) {
                parser_advance(parser);
            }
            parser_match(parser, VUS_TOKEN_RPAREN);
        }

        /* 期望冒号 */
        parser_expect(parser, VUS_TOKEN_COLON);
        if (parser->error) { free(except_type); return NULL; }

        /* 解析 except body */
        parser_skip_newlines(parser);
        VusToken *except_indent = parser_expect(parser, VUS_TOKEN_INDENT);
        if (!except_indent) { free(except_type); return NULL; }

        VusAstList *except_body = parse_statements(parser);
        if (parser->error) { free(except_type); return NULL; }

        parser_expect(parser, VUS_TOKEN_DEDENT);
        if (parser->error) { free(except_type); return NULL; }

        vus_ast_try_add_except(try_node, except_type, except_body);
        free(except_type);
    }

    return (VusAstNode*)try_node;
}

/* ==================================================================
 * Return 语句解析
 * ================================================================== */
static VusAstNode *parse_return_stmt(VusParser *parser) {
    VusToken *keyword = parser_peek(parser); /* 消耗 return/返回 */
    int line = keyword->line;
    int col = keyword->column;
    parser_advance(parser);

    VusAstNode *value = NULL;

    /* 检查是否返回值（后面不是 NEWLINE/INDENT/DEDENT/EOF 时） */
    VusToken *next = parser_peek(parser);
    if (next && next->type != VUS_TOKEN_NEWLINE && next->type != VUS_TOKEN_INDENT &&
        next->type != VUS_TOKEN_DEDENT && next->type != VUS_TOKEN_EOF) {
        value = parse_expr(parser);
        if (!value) return NULL;
    }

    return (VusAstNode*)vus_ast_return_new(value, line, col);
}

/* ==================================================================
 * Import 语句解析
 * ================================================================== */
static VusAstNode *parse_import_stmt(VusParser *parser) {
    VusToken *keyword = parser_peek(parser); /* 消耗 import/导入 */
    int line = keyword->line;
    int col = keyword->column;
    parser_advance(parser);

    VusAstList *names = vus_ast_list_new();
    if (!names) return NULL;

    while (1) {
        VusToken *name_token = parser_expect(parser, VUS_TOKEN_IDENTIFIER);
        if (!name_token) { vus_ast_list_free(names); return NULL; }

        char *mod_name = strndup(name_token->start, name_token->length);
        VusAstNode *ident = (VusAstNode*)vus_ast_ident_new(mod_name, name_token->line, name_token->column);
        free(mod_name);
        vus_ast_list_push(names, ident);

        if (!parser_match(parser, VUS_TOKEN_COMMA)) {
            break;
        }
    }

    /* 创建 Import 节点 */
    ParserAstImport *node = (ParserAstImport*)calloc(1, sizeof(ParserAstImport));
    if (!node) { vus_ast_list_free(names); return NULL; }
    node->type = VUS_AST_IMPORT;
    node->line = line;
    node->column = col;
    node->names = names;

    return (VusAstNode*)node;
}

/* ==================================================================
 * From-Import 语句解析
 * ================================================================== */
static VusAstNode *parse_from_import_stmt(VusParser *parser) {
    VusToken *keyword = parser_peek(parser); /* 消耗 from/从 */
    int line = keyword->line;
    int col = keyword->column;
    parser_advance(parser);

    /* 模块名 */
    VusToken *mod_token = parser_expect(parser, VUS_TOKEN_IDENTIFIER);
    if (!mod_token) return NULL;
    char *module = strndup(mod_token->start, mod_token->length);

    /* 期望 import/导入 */
    VusToken *next = parser_peek(parser);
    if (!next || (next->type != VUS_TOKEN_IMPORT && next->type != VUS_TOKEN_CN_IMPORT)) {
        parser_set_error(parser, "期望 import/导入，但遇到其他 Token（第 %d 行第 %d 列）", line, next->column);
        free(module);
        return NULL;
    }
    parser_advance(parser); /* 消耗 import */

    /* 名称列表 */
    VusAstList *names = vus_ast_list_new();
    if (!names) { free(module); return NULL; }

    while (1) {
        VusToken *name_token = parser_expect(parser, VUS_TOKEN_IDENTIFIER);
        if (!name_token) { free(module); vus_ast_list_free(names); return NULL; }

        char *name = strndup(name_token->start, name_token->length);
        VusAstNode *ident = (VusAstNode*)vus_ast_ident_new(name, name_token->line, name_token->column);
        free(name);
        vus_ast_list_push(names, ident);

        if (!parser_match(parser, VUS_TOKEN_COMMA)) {
            break;
        }
    }

    /* 创建 FromImport 节点 */
    ParserAstFromImport *node = (ParserAstFromImport*)calloc(1, sizeof(ParserAstFromImport));
    if (!node) { free(module); vus_ast_list_free(names); return NULL; }
    node->type = VUS_AST_FROM_IMPORT;
    node->line = line;
    node->column = col;
    node->module = module;
    node->names = names;

    return (VusAstNode*)node;
}

/* ==================================================================
 * Break / Continue / Throw / Global 语句解析
 * ================================================================== */
static VusAstNode *parse_break_stmt(VusParser *parser) {
    VusToken *token = parser_peek(parser);
    int line = token->line, col = token->column;
    parser_advance(parser);
    return (VusAstNode*)vus_ast_break_new(line, col);
}

static VusAstNode *parse_continue_stmt(VusParser *parser) {
    VusToken *token = parser_peek(parser);
    int line = token->line, col = token->column;
    parser_advance(parser);
    /* 使用 break_new 创建 CONTINUE 节点（VusAstBreak 结构体共用） */
    /* 创建后修改 type 为 VUS_AST_CONTINUE */
    VusAstBreak *node = vus_ast_break_new(line, col);
    if (node) {
        node->type = VUS_AST_CONTINUE;
    }
    return (VusAstNode*)node;
}

static VusAstNode *parse_throw_stmt(VusParser *parser) {
    VusToken *keyword = parser_peek(parser);
    int line = keyword->line;
    int col = keyword->column;
    parser_advance(parser);

    VusAstNode *value = NULL;
    VusToken *next = parser_peek(parser);
    if (next && next->type != VUS_TOKEN_NEWLINE && next->type != VUS_TOKEN_INDENT &&
        next->type != VUS_TOKEN_DEDENT && next->type != VUS_TOKEN_EOF) {
        value = parse_expr(parser);
        if (!value) return NULL;
    }

    return (VusAstNode*)vus_ast_throw_new(value, line, col);
}

static VusAstNode *parse_global_stmt(VusParser *parser) {
    VusToken *keyword = parser_peek(parser);
    int line = keyword->line;
    int col = keyword->column;
    parser_advance(parser);

    VusToken *name_token = parser_expect(parser, VUS_TOKEN_IDENTIFIER);
    if (!name_token) return NULL;

    char *name = strndup(name_token->start, name_token->length);
    VusAstGlobalDecl *node = vus_ast_global_new(name, line, col);
    free(name);
    return (VusAstNode*)node;
}

/* ==================================================================
 * 结构体定义解析
 * ==================================================================
 *
 * 函数风格:
 *   struct 名称:\n    字段名1: 类型\n    字段名2: 类型
 *
 * 中文别名:
 *   结构 名称:\n    字段名1: 类型\n    字段名2: 类型
 */
static VusAstNode *parse_struct_def(VusParser *parser) {
    int line = parser->tokens[parser->pos].line;
    int col = parser->tokens[parser->pos].column;
    parser_advance(parser); /* 跳过 struct/结构 */

    if (parser->pos >= parser->token_count ||
        parser->tokens[parser->pos].type == VUS_TOKEN_NEWLINE ||
        parser->tokens[parser->pos].type == VUS_TOKEN_EOF) {
        parser_set_error(parser, "结构体定义需要名称");
        return NULL;
    }
    VusToken *name_tok = &parser->tokens[parser->pos];
    if (name_tok->type != VUS_TOKEN_IDENTIFIER) {
        parser_set_error(parser, "期望结构体名称，但遇到 %s（第 %d 行第 %d 列）",
                         vus_token_type_name(name_tok->type), name_tok->line, name_tok->column);
        return NULL;
    }
    char name_buf[256];
    snprintf(name_buf, sizeof(name_buf), "%.*s", (int)name_tok->length, name_tok->start);
    parser_advance(parser);

    /* 期望换行或冒号 */
    if (parser->pos < parser->token_count && parser->tokens[parser->pos].type == VUS_TOKEN_COLON) {
        parser_advance(parser);
    }

    /* 期望换行 */
    if (parser->pos < parser->token_count && parser->tokens[parser->pos].type == VUS_TOKEN_NEWLINE) {
        parser_advance(parser);
    }
    /* 期望缩进 */
    if (parser->pos >= parser->token_count || parser->tokens[parser->pos].type != VUS_TOKEN_INDENT) {
        parser_set_error(parser, "结构体体需要缩进（第 %d 行第 %d 列）", line, col);
        return NULL;
    }
    parser_advance(parser);

    VusAstList *fields = vus_ast_list_new();

    /* 解析字段（每行一个） */
    while (parser->pos < parser->token_count) {
        VusToken *tok = &parser->tokens[parser->pos];
        if (tok->type == VUS_TOKEN_DEDENT || tok->type == VUS_TOKEN_EOF) break;
        if (tok->type == VUS_TOKEN_NEWLINE) { parser_advance(parser); continue; }

        /* 字段名 */
        char field_name[256];
        snprintf(field_name, sizeof(field_name), "%.*s", (int)tok->length, tok->start);
        parser_advance(parser);

        /* 可选类型注解 */
        char *type_ann = NULL;
        if (parser->pos < parser->token_count && parser->tokens[parser->pos].type == VUS_TOKEN_COLON) {
            parser_advance(parser);
            if (parser->pos < parser->token_count && parser->tokens[parser->pos].type == VUS_TOKEN_IDENTIFIER) {
                char ann_buf[256];
                snprintf(ann_buf, sizeof(ann_buf), "%.*s",
                         (int)parser->tokens[parser->pos].length, parser->tokens[parser->pos].start);
                type_ann = strdup(ann_buf);
                parser_advance(parser);
            }
        }

        VusAstParam *param = vus_ast_param_new(field_name, type_ann, tok->line, tok->column);
        vus_ast_list_push(fields, (VusAstNode *)param);
        if (type_ann) free(type_ann);

        /* 跳过该行剩余内容 */
        while (parser->pos < parser->token_count &&
               parser->tokens[parser->pos].type != VUS_TOKEN_NEWLINE &&
               parser->tokens[parser->pos].type != VUS_TOKEN_DEDENT &&
               parser->tokens[parser->pos].type != VUS_TOKEN_EOF) {
            parser_advance(parser);
        }
    }

    /* 跳过 DEDENT */
    if (parser->pos < parser->token_count && parser->tokens[parser->pos].type == VUS_TOKEN_DEDENT) {
        parser_advance(parser);
    }

    return (VusAstNode *)vus_ast_struct_def_new(name_buf, fields, line, col);
}

/* ==================================================================
 * 赋值或表达式语句解析
 * ================================================================== */
static VusAstNode *parse_assign_or_expr(VusParser *parser) {
    VusToken *token = parser_peek(parser);
    if (!token) return NULL;

    /* 检查是否为赋值语句：identifier = expr 或 identifier : type = expr */
    if (token->type == VUS_TOKEN_IDENTIFIER) {
        VusToken *next = parser_peek_next(parser);
        if (next && (next->type == VUS_TOKEN_ASSIGN || next->type == VUS_TOKEN_COLON)) {
            /* 赋值语句 */
            VusToken *id_token = parser_peek(parser);
            char *target = strndup(id_token->start, id_token->length);
            int line = id_token->line;
            int col = id_token->column;
            parser_advance(parser);
            char *type_ann = NULL;

            if (parser_match(parser, VUS_TOKEN_COLON)) {
                /* 类型注解 */
                VusToken *type_token = parser_expect(parser, VUS_TOKEN_IDENTIFIER);
                if (!type_token) { free(target); return NULL; }
                type_ann = strndup(type_token->start, type_token->length);

                parser_expect(parser, VUS_TOKEN_ASSIGN);
                if (parser->error) { free(target); free(type_ann); return NULL; }
            } else {
                /* 无类型注解，直接消耗赋值号 */
                parser_expect(parser, VUS_TOKEN_ASSIGN);
                if (parser->error) { free(target); return NULL; }
            }

            VusAstNode *value = parse_expr(parser);
            if (!value) { free(target); free(type_ann); return NULL; }

            VusAstAssign *node;
            if (parser->in_function) {
                node = vus_ast_assign_local_new(target, type_ann, value, line, col);
            } else {
                node = vus_ast_assign_new(target, type_ann, value, line, col);
            }
            free(target);
            free(type_ann);
            return (VusAstNode*)node;
        }
    }

    /* 否则解析为表达式语句 */
    VusAstNode *expr = parse_expr(parser);
    if (!expr) return NULL;

    return (VusAstNode*)vus_ast_expr_stmt_new(expr, token->line, token->column);
}

/* ==================================================================
 * 表达式解析（递归下降，优先级从低到高）
 * ================================================================== */

static VusAstNode *parse_expr(VusParser *parser) {
    return parse_logical_or(parser);
}

/*
 * logical_or  → logical_and (("or"|"或") logical_and)*
 */
static VusAstNode *parse_logical_or(VusParser *parser) {
    VusAstNode *left = parse_logical_and(parser);
    if (!left) return NULL;

    while (1) {
        VusToken *token = parser_peek(parser);
        if (!token) break;

        const char *op = NULL;
        if (token->type == VUS_TOKEN_OR || token->type == VUS_TOKEN_CN_OR) {
            op = "or";
        }

        if (!op) break;

        parser_advance(parser);
        VusAstNode *right = parse_logical_and(parser);
        if (!right) return NULL;

        VusAstBinaryOp *node = vus_ast_binary_new(op, left, right, token->line, token->column);
        left = (VusAstNode*)node;
    }

    return left;
}

/*
 * logical_and  → bitwise_or (("and"|"和") bitwise_or)*
 */
static VusAstNode *parse_logical_and(VusParser *parser) {
    VusAstNode *left = parse_bitwise_or(parser);
    if (!left) return NULL;

    while (1) {
        VusToken *token = parser_peek(parser);
        if (!token) break;

        const char *op = NULL;
        if (token->type == VUS_TOKEN_AND || token->type == VUS_TOKEN_CN_AND) {
            op = "and";
        }

        if (!op) break;

        parser_advance(parser);
        VusAstNode *right = parse_bitwise_or(parser);
        if (!right) return NULL;

        VusAstBinaryOp *node = vus_ast_binary_new(op, left, right, token->line, token->column);
        left = (VusAstNode*)node;
    }

    return left;
}

/*
 * bitwise_or  → bitwise_xor (("|") bitwise_xor)*
 */
static VusAstNode *parse_bitwise_or(VusParser *parser) {
    VusAstNode *left = parse_bitwise_xor(parser);
    if (!left) return NULL;

    while (1) {
        VusToken *token = parser_peek(parser);
        if (!token) break;
        if (token->type != VUS_TOKEN_BIT_OR) break;

        parser_advance(parser);
        VusAstNode *right = parse_bitwise_xor(parser);
        if (!right) return NULL;

        VusAstBinaryOp *node = vus_ast_binary_new("|", left, right, token->line, token->column);
        left = (VusAstNode*)node;
    }

    return left;
}

/*
 * bitwise_xor  → bitwise_and (("^") bitwise_and)*
 */
static VusAstNode *parse_bitwise_xor(VusParser *parser) {
    VusAstNode *left = parse_bitwise_and(parser);
    if (!left) return NULL;

    while (1) {
        VusToken *token = parser_peek(parser);
        if (!token) break;
        if (token->type != VUS_TOKEN_BIT_XOR) break;

        parser_advance(parser);
        VusAstNode *right = parse_bitwise_and(parser);
        if (!right) return NULL;

        VusAstBinaryOp *node = vus_ast_binary_new("^", left, right, token->line, token->column);
        left = (VusAstNode*)node;
    }

    return left;
}

/*
 * bitwise_and  → comparison (("&") comparison)*
 */
static VusAstNode *parse_bitwise_and(VusParser *parser) {
    VusAstNode *left = parse_comparison(parser);
    if (!left) return NULL;

    while (1) {
        VusToken *token = parser_peek(parser);
        if (!token) break;
        if (token->type != VUS_TOKEN_BIT_AND) break;

        parser_advance(parser);
        VusAstNode *right = parse_comparison(parser);
        if (!right) return NULL;

        VusAstBinaryOp *node = vus_ast_binary_new("&", left, right, token->line, token->column);
        left = (VusAstNode*)node;
    }

    return left;
}

/*
 * comparison  → shift (("=="|"!="|"<"|">"|"<="|">=") shift)*
 */
static VusAstNode *parse_comparison(VusParser *parser) {
    VusAstNode *left = parse_shift(parser);
    if (!left) return NULL;

    while (1) {
        VusToken *token = parser_peek(parser);
        if (!token) break;

        const char *op = NULL;
        switch (token->type) {
            case VUS_TOKEN_EQ:  op = "=="; break;
            case VUS_TOKEN_NEQ: op = "!="; break;
            case VUS_TOKEN_LT:  op = "<";  break;
            case VUS_TOKEN_GT:  op = ">";  break;
            case VUS_TOKEN_LE:  op = "<="; break;
            case VUS_TOKEN_GE:  op = ">="; break;
            default: break;
        }

        if (!op) break;

        parser_advance(parser);
        VusAstNode *right = parse_shift(parser);
        if (!right) return NULL;

        VusAstBinaryOp *node = vus_ast_binary_new(op, left, right, token->line, token->column);
        left = (VusAstNode*)node;
    }

    return left;
}

/*
 * shift  → concat (("<<"|">>") concat)*
 */
static VusAstNode *parse_shift(VusParser *parser) {
    VusAstNode *left = parse_concat(parser);
    if (!left) return NULL;

    while (1) {
        VusToken *token = parser_peek(parser);
        if (!token) break;

        const char *op = NULL;
        if (token->type == VUS_TOKEN_SHL) {
            op = "<<";
        } else if (token->type == VUS_TOKEN_SHR) {
            op = ">>";
        }

        if (!op) break;

        parser_advance(parser);
        VusAstNode *right = parse_concat(parser);
        if (!right) return NULL;

        VusAstBinaryOp *node = vus_ast_binary_new(op, left, right, token->line, token->column);
        left = (VusAstNode*)node;
    }

    return left;
}

/*
 * concat  → additive (("..") additive)*
 */
static VusAstNode *parse_concat(VusParser *parser) {
    VusAstNode *left = parse_additive(parser);
    if (!left) return NULL;

    while (1) {
        VusToken *token = parser_peek(parser);
        if (!token || token->type != VUS_TOKEN_CONCAT) break;

        parser_advance(parser);
        VusAstNode *right = parse_additive(parser);
        if (!right) return NULL;

        VusAstBinaryOp *node = vus_ast_binary_new("..", left, right, token->line, token->column);
        left = (VusAstNode*)node;
    }

    return left;
}

/*
 * additive  → multiplicative (("+"|"-") multiplicative)*
 */
static VusAstNode *parse_additive(VusParser *parser) {
    VusAstNode *left = parse_multiplicative(parser);
    if (!left) return NULL;

    while (1) {
        VusToken *token = parser_peek(parser);
        if (!token) break;

        const char *op = NULL;
        switch (token->type) {
            case VUS_TOKEN_PLUS:  op = "+"; break;
            case VUS_TOKEN_MINUS: op = "-"; break;
            default: break;
        }

        if (!op) break;

        parser_advance(parser);
        VusAstNode *right = parse_multiplicative(parser);
        if (!right) return NULL;

        VusAstBinaryOp *node = vus_ast_binary_new(op, left, right, token->line, token->column);
        left = (VusAstNode*)node;
    }

    return left;
}

/*
 * multiplicative  → unary (("*"|"/"|"%") unary)*
 */
static VusAstNode *parse_multiplicative(VusParser *parser) {
    VusAstNode *left = parse_unary(parser);
    if (!left) return NULL;

    while (1) {
        VusToken *token = parser_peek(parser);
        if (!token) break;

        const char *op = NULL;
        switch (token->type) {
            case VUS_TOKEN_STAR:    op = "*"; break;
            case VUS_TOKEN_SLASH:   op = "/"; break;
            case VUS_TOKEN_PERCENT: op = "%"; break;
            default: break;
        }

        if (!op) break;

        parser_advance(parser);
        VusAstNode *right = parse_unary(parser);
        if (!right) return NULL;

        VusAstBinaryOp *node = vus_ast_binary_new(op, left, right, token->line, token->column);
        left = (VusAstNode*)node;
    }

    return left;
}

/*
 * unary → ("-"|"not"|"非"|"!"|"~") unary | primary
 */
static VusAstNode *parse_unary(VusParser *parser) {
    VusToken *token = parser_peek(parser);
    if (!token) return NULL;

    const char *op = NULL;
    switch (token->type) {
        case VUS_TOKEN_MINUS:    op = "-";    break;
        case VUS_TOKEN_NOT:      op = "not";  break;
        case VUS_TOKEN_CN_NOT:   op = "not";  break;
        case VUS_TOKEN_BIT_NOT:  op = "~";    break;
        default: break;
    }

    if (op) {
        parser_advance(parser);
        int line = token->line;
        int col = token->column;
        VusAstNode *operand = parse_unary(parser);
        if (!operand) return NULL;
        return (VusAstNode*)vus_ast_unary_new(op, operand, line, col);
    }

    return parse_primary(parser);
}

/*
 * primary → literal | identifier call? | "(" expr ")" | "[" list "]" | "{" dict "}"
 */
static VusAstNode *parse_primary(VusParser *parser) {
    VusToken *token = parser_peek(parser);
    if (!token) {
        parser_set_error(parser, "期望表达式，但遇到文件结尾");
        return NULL;
    }

    switch (token->type) {
        case VUS_TOKEN_IDENTIFIER: {
            parser_advance(parser);
            char *name = strndup(token->start, token->length);
            int line = token->line;
            int col = token->column;

            VusAstNode *expr = NULL;
            VusAstList *type_args = NULL;

            /* 检查泛型类型参数 <T, U> */
            if (parser_peek(parser) && parser_peek(parser)->type == VUS_TOKEN_LT) {
                /* 尝试解析泛型类型参数，如果后面不是标识符+逗号+>，则回退 */
                VusParser saved = *parser; /* 保存状态以便回退 */
                parser_advance(parser); /* skip < */
                VusAstList *ta = vus_ast_list_new();
                int valid = 1;
                while (1) {
                    VusToken *tp_tok = parser_peek(parser);
                    if (!tp_tok) { valid = 0; break; }
                    /* 接受标识符或类型关键字作为类型参数名 */
                    int is_type = (tp_tok->type == VUS_TOKEN_IDENTIFIER ||
                                   tp_tok->type == VUS_TOKEN_TYPE_INT ||
                                   tp_tok->type == VUS_TOKEN_TYPE_STR ||
                                   tp_tok->type == VUS_TOKEN_TYPE_FLOAT ||
                                   tp_tok->type == VUS_TOKEN_TYPE_BOOL ||
                                   tp_tok->type == VUS_TOKEN_TYPE_LIST ||
                                   tp_tok->type == VUS_TOKEN_TYPE_DICT);
                    if (!is_type) { valid = 0; break; }
                    parser_advance(parser);
                    char *tp_name = strndup(tp_tok->start, tp_tok->length);
                    VusAstParam *tp = vus_ast_param_new(tp_name, NULL, tp_tok->line, tp_tok->column);
                    free(tp_name);
                    vus_ast_list_push(ta, (VusAstNode *)tp);

                    VusToken *next = parser_peek(parser);
                    if (!next) { valid = 0; break; }
                    if (next->type == VUS_TOKEN_COMMA) {
                        parser_advance(parser);
                        continue;
                    } else if (next->type == VUS_TOKEN_GT) {
                        parser_advance(parser);
                        break;
                    } else {
                        valid = 0;
                        break;
                    }
                }
                if (valid && parser_peek(parser) && parser_peek(parser)->type == VUS_TOKEN_LPAREN) {
                    /* 确实是泛型函数调用 */
                    type_args = ta;
                    VusAstList *args = parse_call_args(parser);
                    if (!args) { free(name); vus_ast_list_free(type_args); return NULL; }
                    VusAstCall *node = vus_ast_call_new(name, args, type_args, line, col);
                    free(name);
                    expr = (VusAstNode*)node;
                } else {
                    /* 不是泛型调用，回退 */
                    *parser = saved;
                    vus_ast_list_free(ta);
                    type_args = NULL;
                    /* 普通函数调用？ */
                    if (parser_peek(parser) && parser_peek(parser)->type == VUS_TOKEN_LPAREN) {
                        VusAstList *args = parse_call_args(parser);
                        if (!args) { free(name); return NULL; }
                        VusAstCall *node = vus_ast_call_new(name, args, NULL, line, col);
                        free(name);
                        expr = (VusAstNode*)node;
                    } else {
                        VusAstIdentifier *node = vus_ast_ident_new(name, line, col);
                        free(name);
                        expr = (VusAstNode*)node;
                    }
                }
            } else if (parser_peek(parser) && parser_peek(parser)->type == VUS_TOKEN_LPAREN) {
                VusAstList *args = parse_call_args(parser);
                if (!args) { free(name); return NULL; }
                VusAstCall *node = vus_ast_call_new(name, args, NULL, line, col);
                free(name);
                expr = (VusAstNode*)node;
            } else {
                VusAstIdentifier *node = vus_ast_ident_new(name, line, col);
                free(name);
                expr = (VusAstNode*)node;
            }

            /* 成员访问链（点号） */
            while (parser_peek(parser) && parser_peek(parser)->type == VUS_TOKEN_DOT) {
                parser_advance(parser); /* 跳过点号 */
                if (parser_peek(parser) && parser_peek(parser)->type == VUS_TOKEN_IDENTIFIER) {
                    VusToken *mtok = parser_peek(parser);
                    char member[256];
                    snprintf(member, sizeof(member), "%.*s", (int)mtok->length, mtok->start);
                    parser_advance(parser);
                    VusAstAccess *access = vus_ast_access_new(expr, member, line, col);
                    expr = (VusAstNode*)access;
                }
            }

            /* 下标访问链（方括号） */
            while (parser_peek(parser) && parser_peek(parser)->type == VUS_TOKEN_LBRACKET) {
                parser_advance(parser); /* 跳过 [ */
                VusAstNode *idx = parse_expr(parser);
                if (!idx) return NULL;
                parser_expect(parser, VUS_TOKEN_RBRACKET);
                if (parser->error) return NULL;
                VusAstSubscript *sub = vus_ast_subscript_new(expr, idx, line, col);
                expr = (VusAstNode*)sub;
            }

            return expr;
        }

        case VUS_TOKEN_NUMBER: {
            parser_advance(parser);
            char *val = strndup(token->start, token->length);
            int is_float = 0;
            for (size_t i = 0; i < token->length; i++) {
                if (token->start[i] == '.') { is_float = 1; break; }
            }
            VusAstNumber *node = vus_ast_number_new(val, is_float, token->line, token->column);
            free(val);
            return (VusAstNode*)node;
        }

        case VUS_TOKEN_STRING: {
            parser_advance(parser);
            char *val = NULL;
            if (token->value) {
                val = strdup(token->value);
            } else {
                val = strndup(token->start, token->length);
            }
            VusAstString *node = vus_ast_string_new(val, token->line, token->column);
            free(val);
            return (VusAstNode*)node;
        }

        case VUS_TOKEN_TRUE:
        case VUS_TOKEN_CN_TRUE: {
            parser_advance(parser);
            return (VusAstNode*)vus_ast_bool_new(1, token->line, token->column);
        }

        case VUS_TOKEN_FALSE:
        case VUS_TOKEN_CN_FALSE: {
            parser_advance(parser);
            return (VusAstNode*)vus_ast_bool_new(0, token->line, token->column);
        }

        case VUS_TOKEN_NULL:
        case VUS_TOKEN_CN_NULL: {
            parser_advance(parser);
            return (VusAstNode*)vus_ast_null_new(token->line, token->column);
        }

        case VUS_TOKEN_LPAREN: {
            parser_advance(parser); /* 消耗 ( */
            VusAstNode *expr = parse_expr(parser);
            if (!expr) return NULL;
            parser_expect(parser, VUS_TOKEN_RPAREN);
            if (parser->error) return NULL;
            return expr;
        }

        case VUS_TOKEN_LBRACKET: {
            /* 列表字面量 */
            parser_advance(parser); /* 消耗 [ */
            VusAstList *items = vus_ast_list_new();
            if (!items) return NULL;

            if (parser_peek(parser) && parser_peek(parser)->type != VUS_TOKEN_RBRACKET) {
                while (1) {
                    VusAstNode *item = parse_expr(parser);
                    if (!item) { vus_ast_list_free(items); return NULL; }
                    vus_ast_list_push(items, item);
                    if (!parser_match(parser, VUS_TOKEN_COMMA)) break;
                }
            }

            parser_expect(parser, VUS_TOKEN_RBRACKET);
            if (parser->error) { vus_ast_list_free(items); return NULL; }

            /* 创建 ListLiteral 节点 — 使用 VUS_AST_LIST_LITERAL 类型 */
            /* 由于没有专门的创建函数，用 calloc 创建一个通用节点临时存储 */
            /* 使用 VusAstNode 作为基类，list 数据通过额外结构存储 */
            typedef struct {
                VusAstNodeType type;
                int            line;
                int            column;
                VusAstList    *items;
            } LocalListLiteral;

            LocalListLiteral *node = (LocalListLiteral*)calloc(1, sizeof(LocalListLiteral));
            if (!node) { vus_ast_list_free(items); return NULL; }
            node->type = VUS_AST_LIST_LITERAL;
            node->line = token->line;
            node->column = token->column;
            node->items = items;
            return (VusAstNode*)node;
        }

        case VUS_TOKEN_LBRACE: {
            /* 字典字面量 */
            parser_advance(parser); /* 消耗 { */
            VusAstList *keys = vus_ast_list_new();
            VusAstList *values = vus_ast_list_new();
            if (!keys || !values) {
                vus_ast_list_free(keys);
                vus_ast_list_free(values);
                return NULL;
            }

            if (parser_peek(parser) && parser_peek(parser)->type != VUS_TOKEN_RBRACE) {
                while (1) {
                    VusAstNode *key = parse_expr(parser);
                    if (!key) { vus_ast_list_free(keys); vus_ast_list_free(values); return NULL; }

                    parser_expect(parser, VUS_TOKEN_COLON);
                    if (parser->error) { vus_ast_list_free(keys); vus_ast_list_free(values); return NULL; }

                    VusAstNode *val = parse_expr(parser);
                    if (!val) { vus_ast_list_free(keys); vus_ast_list_free(values); return NULL; }

                    vus_ast_list_push(keys, key);
                    vus_ast_list_push(values, val);

                    if (!parser_match(parser, VUS_TOKEN_COMMA)) break;
                }
            }

            parser_expect(parser, VUS_TOKEN_RBRACE);
            if (parser->error) { vus_ast_list_free(keys); vus_ast_list_free(values); return NULL; }

            /* 创建 DictLiteral 节点 */
            typedef struct {
                VusAstNodeType type;
                int            line;
                int            column;
                VusAstList    *keys;
                VusAstList    *values;
            } LocalDictLiteral;

            LocalDictLiteral *node = (LocalDictLiteral*)calloc(1, sizeof(LocalDictLiteral));
            if (!node) { vus_ast_list_free(keys); vus_ast_list_free(values); return NULL; }
            node->type = VUS_AST_DICT_LITERAL;
            node->line = token->line;
            node->column = token->column;
            node->keys = keys;
            node->values = values;
            return (VusAstNode*)node;
        }

        /* ===== 线程/协程表达式 ===== */
        case VUS_TOKEN_CN_THREAD:
        case VUS_TOKEN_CN_JOIN_THREAD:
        case VUS_TOKEN_CN_COROUTINE:
        case VUS_TOKEN_CN_RESUME: {
            /* 这些关键字作为表达式处理：线程(func, arg)、等待线程(t)、协程(func, arg)、恢复(c) */
            VusTokenType ktype = token->type;
            int kw_line = token->line;
            int kw_col = token->column;
            parser_advance(parser); /* 消耗关键字 */

            /* 期望左括号 */
            parser_expect(parser, VUS_TOKEN_LPAREN);
            if (parser->error) return NULL;

            VusAstNode *expr_result = NULL;

            switch (ktype) {
                case VUS_TOKEN_CN_THREAD: {
                    /* 线程(func, arg) */
                    VusAstNode *func = parse_expr(parser);
                    if (!func) return NULL;
                    parser_expect(parser, VUS_TOKEN_COMMA);
                    if (parser->error) { vus_ast_node_free(func); return NULL; }
                    VusAstNode *arg = parse_expr(parser);
                    if (!arg) { vus_ast_node_free(func); return NULL; }
                    parser_expect(parser, VUS_TOKEN_RPAREN);
                    if (parser->error) { vus_ast_node_free(func); vus_ast_node_free(arg); return NULL; }
                    expr_result = (VusAstNode*)vus_ast_thread_create_new(func, arg, kw_line, kw_col);
                    break;
                }
                case VUS_TOKEN_CN_JOIN_THREAD: {
                    /* 等待线程(t) */
                    VusAstNode *thread = parse_expr(parser);
                    if (!thread) return NULL;
                    parser_expect(parser, VUS_TOKEN_RPAREN);
                    if (parser->error) { vus_ast_node_free(thread); return NULL; }
                    expr_result = (VusAstNode*)vus_ast_thread_join_new(thread, kw_line, kw_col);
                    break;
                }
                case VUS_TOKEN_CN_COROUTINE: {
                    /* 协程(func, arg) */
                    VusAstNode *func = parse_expr(parser);
                    if (!func) return NULL;
                    parser_expect(parser, VUS_TOKEN_COMMA);
                    if (parser->error) { vus_ast_node_free(func); return NULL; }
                    VusAstNode *arg = parse_expr(parser);
                    if (!arg) { vus_ast_node_free(func); return NULL; }
                    parser_expect(parser, VUS_TOKEN_RPAREN);
                    if (parser->error) { vus_ast_node_free(func); vus_ast_node_free(arg); return NULL; }
                    expr_result = (VusAstNode*)vus_ast_coro_create_new(func, arg, kw_line, kw_col);
                    break;
                }
                case VUS_TOKEN_CN_RESUME: {
                    /* 恢复(c) */
                    VusAstNode *coro = parse_expr(parser);
                    if (!coro) return NULL;
                    parser_expect(parser, VUS_TOKEN_RPAREN);
                    if (parser->error) { vus_ast_node_free(coro); return NULL; }
                    expr_result = (VusAstNode*)vus_ast_coro_resume_new(coro, kw_line, kw_col);
                    break;
                }
                default:
                    break;
            }

            return expr_result;
        }

        case VUS_TOKEN_CN_YIELD: {
            /* 让出() — 无参数表达式 */
            int kw_line = token->line;
            int kw_col = token->column;
            parser_advance(parser); /* 消耗 让出 */

            /* 期望左括号和右括号 */
            parser_expect(parser, VUS_TOKEN_LPAREN);
            if (parser->error) return NULL;
            parser_expect(parser, VUS_TOKEN_RPAREN);
            if (parser->error) return NULL;

            return (VusAstNode*)vus_ast_coro_yield_new(kw_line, kw_col);
        }

        case VUS_TOKEN_CN_THREAD_SLEEP: {
            /* 睡眠(ms) — 转换为 vus_thread_sleep(ms) */
            int kw_line = token->line;
            int kw_col = token->column;
            parser_advance(parser); /* 消耗 睡眠 */

            parser_expect(parser, VUS_TOKEN_LPAREN);
            if (parser->error) return NULL;
            VusAstNode *ms = parse_expr(parser);
            if (!ms) return NULL;
            parser_expect(parser, VUS_TOKEN_RPAREN);
            if (parser->error) { vus_ast_node_free(ms); return NULL; }

            /* 用函数调用包装：睡眠(ms) -> 调用 vus_thread_sleep */
            /* 实际上，使用函数调用并将结果丢弃 */
            VusAstList *args = vus_ast_list_new();
            vus_ast_list_push(args, ms);
            return (VusAstNode*)vus_ast_call_new("睡眠", args, NULL, kw_line, kw_col);
        }

        default:
            parser_set_error(parser, "意外的 Token: %s（第 %d 行第 %d 列）",
                             vus_token_type_name(token->type), token->line, token->column);
            return NULL;
    }
}

/*
 * 解析函数调用参数列表：已经消耗了左括号，解析到右括号
 */
static VusAstList *parse_call_args(VusParser *parser) {
    parser_advance(parser); /* 消耗 ( */
    VusAstList *args = vus_ast_list_new();
    if (!args) return NULL;

    if (parser_peek(parser) && parser_peek(parser)->type != VUS_TOKEN_RPAREN) {
        while (1) {
            VusAstNode *arg = parse_expr(parser);
            if (!arg) { vus_ast_list_free(args); return NULL; }
            vus_ast_list_push(args, arg);
            if (!parser_match(parser, VUS_TOKEN_COMMA)) break;
        }
    }

    parser_expect(parser, VUS_TOKEN_RPAREN);
    if (parser->error) { vus_ast_list_free(args); return NULL; }

    return args;
}

/* ==================================================================
 * 公开 API 实现
 * ================================================================== */

VusParser *vus_parser_new(VusToken *tokens, size_t count) {
    VusParser *parser = (VusParser*)calloc(1, sizeof(VusParser));
    if (!parser) return NULL;

    parser->tokens = tokens;
    parser->token_count = count;
    parser->pos = 0;
    parser->error = 0;
    parser->error_msg[0] = '\0';
    parser->in_function = 0;

    return parser;
}

VusAstProgram *vus_parser_parse(VusParser *parser) {
    if (!parser) return NULL;
    if (parser->error) return NULL;

    parser_skip_newlines(parser);

    VusAstList *stmts = parse_statements(parser);
    if (parser->error) {
        if (stmts) vus_ast_list_free(stmts);
        return NULL;
    }

    return vus_ast_program_new(stmts);
}

void vus_parser_free(VusParser *parser) {
    if (parser) {
        free(parser);
    }
}

const char *vus_parser_error(VusParser *parser) {
    if (!parser) return "未知错误（空解析器）";
    return parser->error_msg;
}

/* ==================================================================
 * AST 打印（调试用）
 * ================================================================== */

static void print_indent(int indent) {
    for (int i = 0; i < indent; i++) {
        printf("  ");
    }
}

static void vus_ast_print_node(VusAstNode *node, int indent) {
    if (!node) {
        print_indent(indent);
        printf("(null)\n");
        return;
    }

    switch (node->type) {
        case VUS_AST_PROGRAM: {
            VusAstProgram *prog = (VusAstProgram*)node;
            print_indent(indent);
            printf("Program\n");
            if (prog->statements) {
                for (size_t i = 0; i < prog->statements->count; i++) {
                    vus_ast_print_node(prog->statements->items[i], indent + 1);
                }
            }
            break;
        }

        case VUS_AST_FUNCTION_DEF: {
            VusAstFunctionDef *fd = (VusAstFunctionDef*)node;
            print_indent(indent);
            printf("FunctionDef: %s\n", fd->name);
            if (fd->type_params && fd->type_params->count > 0) {
                print_indent(indent + 1);
                printf("TypeParams:\n");
                for (size_t i = 0; i < fd->type_params->count; i++) {
                    vus_ast_print_node(fd->type_params->items[i], indent + 2);
                }
            }
            print_indent(indent + 1);
            printf("Params:\n");
            if (fd->params) {
                for (size_t i = 0; i < fd->params->count; i++) {
                    vus_ast_print_node(fd->params->items[i], indent + 2);
                }
            }
            print_indent(indent + 1);
            printf("Body:\n");
            if (fd->body) {
                for (size_t i = 0; i < fd->body->count; i++) {
                    vus_ast_print_node(fd->body->items[i], indent + 2);
                }
            }
            break;
        }

        case VUS_AST_PARAM: {
            VusAstParam *p = (VusAstParam*)node;
            print_indent(indent);
            printf("Param: %s", p->name);
            if (p->type_annotation) {
                printf(" : %s", p->type_annotation);
            }
            printf("\n");
            break;
        }

        case VUS_AST_PARAM_DEFAULT: {
            VusAstParamDefault *pd = (VusAstParamDefault*)node;
            print_indent(indent);
            printf("ParamDefault: %s", pd->name);
            if (pd->type_annotation) {
                printf(" : %s", pd->type_annotation);
            }
            printf(" =\n");
            vus_ast_print_node(pd->default_value, indent + 1);
            break;
        }

        case VUS_AST_IF: {
            VusAstIf *if_node = (VusAstIf*)node;
            print_indent(indent);
            printf("If\n");
            print_indent(indent + 1);
            printf("Condition:\n");
            vus_ast_print_node(if_node->condition, indent + 2);
            print_indent(indent + 1);
            printf("Then:\n");
            if (if_node->then_body) {
                for (size_t i = 0; i < if_node->then_body->count; i++) {
                    vus_ast_print_node(if_node->then_body->items[i], indent + 2);
                }
            }
            if (if_node->elif_conditions) {
                for (size_t i = 0; i < if_node->elif_conditions->count; i++) {
                    print_indent(indent + 1);
                    printf("Elif:\n");
                    vus_ast_print_node(if_node->elif_conditions->items[i], indent + 2);
                    if (if_node->elif_bodies && i < if_node->elif_bodies->count) {
                        VusAstList *body = (VusAstList*)if_node->elif_bodies->items[i];
                        if (body) {
                            for (size_t j = 0; j < body->count; j++) {
                                vus_ast_print_node(body->items[j], indent + 2);
                            }
                        }
                    }
                }
            }
            if (if_node->else_body) {
                print_indent(indent + 1);
                printf("Else:\n");
                for (size_t i = 0; i < if_node->else_body->count; i++) {
                    vus_ast_print_node(if_node->else_body->items[i], indent + 2);
                }
            }
            break;
        }

        case VUS_AST_FOR_RANGE: {
            VusAstForRange *fr = (VusAstForRange*)node;
            print_indent(indent);
            printf("ForRange: %s\n", fr->var_name);
            print_indent(indent + 1);
            printf("Start:\n");
            vus_ast_print_node(fr->start, indent + 2);
            print_indent(indent + 1);
            printf("End:\n");
            vus_ast_print_node(fr->end, indent + 2);
            print_indent(indent + 1);
            printf("Body:\n");
            if (fr->body) {
                for (size_t i = 0; i < fr->body->count; i++) {
                    vus_ast_print_node(fr->body->items[i], indent + 2);
                }
            }
            break;
        }

        case VUS_AST_FOR_EACH: {
            VusAstForEach *fe = (VusAstForEach*)node;
            print_indent(indent);
            printf("ForEach: %s\n", fe->var_name);
            print_indent(indent + 1);
            printf("Iterable:\n");
            vus_ast_print_node(fe->iterable, indent + 2);
            print_indent(indent + 1);
            printf("Body:\n");
            if (fe->body) {
                for (size_t i = 0; i < fe->body->count; i++) {
                    vus_ast_print_node(fe->body->items[i], indent + 2);
                }
            }
            break;
        }

        case VUS_AST_WHILE: {
            VusAstWhile *w = (VusAstWhile*)node;
            print_indent(indent);
            printf("While\n");
            print_indent(indent + 1);
            printf("Condition:\n");
            vus_ast_print_node(w->condition, indent + 2);
            print_indent(indent + 1);
            printf("Body:\n");
            if (w->body) {
                for (size_t i = 0; i < w->body->count; i++) {
                    vus_ast_print_node(w->body->items[i], indent + 2);
                }
            }
            break;
        }

        case VUS_AST_TRY: {
            VusAstTry *t = (VusAstTry*)node;
            print_indent(indent);
            printf("Try\n");
            print_indent(indent + 1);
            printf("Body:\n");
            if (t->try_body) {
                for (size_t i = 0; i < t->try_body->count; i++) {
                    vus_ast_print_node(t->try_body->items[i], indent + 2);
                }
            }
            if (t->except_types && t->except_bodies) {
                for (size_t i = 0; i < t->except_types->count; i++) {
                    print_indent(indent + 1);
                    VusAstIdentifier *et = (VusAstIdentifier*)t->except_types->items[i];
                    if (et) {
                        printf("Except: %s\n", et->name);
                    } else {
                        printf("Except: *\n");
                    }
                    if (i < t->except_bodies->count) {
                        VusAstList *eb = (VusAstList*)t->except_bodies->items[i];
                        if (eb) {
                            for (size_t j = 0; j < eb->count; j++) {
                                vus_ast_print_node(eb->items[j], indent + 2);
                            }
                        }
                    }
                }
            }
            break;
        }

        case VUS_AST_RETURN: {
            VusAstReturn *r = (VusAstReturn*)node;
            print_indent(indent);
            printf("Return\n");
            if (r->value) {
                vus_ast_print_node(r->value, indent + 1);
            }
            break;
        }

        case VUS_AST_BREAK: {
            print_indent(indent);
            printf("Break\n");
            break;
        }

        case VUS_AST_CONTINUE: {
            print_indent(indent);
            printf("Continue\n");
            break;
        }

        case VUS_AST_THROW: {
            VusAstThrow *t = (VusAstThrow*)node;
            print_indent(indent);
            printf("Throw\n");
            if (t->value) {
                vus_ast_print_node(t->value, indent + 1);
            }
            break;
        }

        case VUS_AST_GLOBAL_DECL: {
            VusAstGlobalDecl *g = (VusAstGlobalDecl*)node;
            print_indent(indent);
            printf("Global: %s\n", g->name);
            break;
        }

        case VUS_AST_IMPORT: {
            ParserAstImport *imp = (ParserAstImport*)node;
            print_indent(indent);
            printf("Import:\n");
            if (imp->names) {
                for (size_t i = 0; i < imp->names->count; i++) {
                    vus_ast_print_node(imp->names->items[i], indent + 1);
                }
            }
            break;
        }

        case VUS_AST_FROM_IMPORT: {
            ParserAstFromImport *fi = (ParserAstFromImport*)node;
            print_indent(indent);
            printf("FromImport: %s\n", fi->module);
            if (fi->names) {
                for (size_t i = 0; i < fi->names->count; i++) {
                    vus_ast_print_node(fi->names->items[i], indent + 1);
                }
            }
            break;
        }

        case VUS_AST_ASSIGN: {
            VusAstAssign *a = (VusAstAssign*)node;
            print_indent(indent);
            printf("Assign: %s", a->target);
            if (a->type_annotation) {
                printf(" : %s", a->type_annotation);
            }
            printf("\n");
            vus_ast_print_node(a->value, indent + 1);
            break;
        }

        case VUS_AST_EXPR_STMT: {
            VusAstExprStmt *es = (VusAstExprStmt*)node;
            print_indent(indent);
            printf("ExprStmt\n");
            vus_ast_print_node(es->expr, indent + 1);
            break;
        }

        case VUS_AST_BINARY_OP: {
            VusAstBinaryOp *b = (VusAstBinaryOp*)node;
            print_indent(indent);
            printf("BinaryOp: %s\n", b->op);
            vus_ast_print_node(b->left, indent + 1);
            vus_ast_print_node(b->right, indent + 1);
            break;
        }

        case VUS_AST_UNARY_OP: {
            VusAstUnaryOp *u = (VusAstUnaryOp*)node;
            print_indent(indent);
            printf("UnaryOp: %s\n", u->op);
            vus_ast_print_node(u->operand, indent + 1);
            break;
        }

        case VUS_AST_CALL: {
            VusAstCall *c = (VusAstCall*)node;
            print_indent(indent);
            printf("Call: %s", c->func_name);
            if (c->type_args && c->type_args->count > 0) {
                printf("<");
                for (size_t i = 0; i < c->type_args->count; i++) {
                    VusAstNode *p = c->type_args->items[i];
                    if (p->type == VUS_AST_PARAM) {
                        VusAstParam *tp = (VusAstParam *)p;
                        if (i > 0) printf(", ");
                        printf("%s", tp->name);
                    }
                }
                printf(">");
            }
            printf("\n");
            if (c->type_args) {
                for (size_t i = 0; i < c->type_args->count; i++) {
                    vus_ast_print_node(c->type_args->items[i], indent + 1);
                }
            }
            if (c->args) {
                for (size_t i = 0; i < c->args->count; i++) {
                    vus_ast_print_node(c->args->items[i], indent + 1);
                }
            }
            break;
        }

        case VUS_AST_IDENTIFIER: {
            VusAstIdentifier *id = (VusAstIdentifier*)node;
            print_indent(indent);
            printf("Identifier: %s\n", id->name);
            break;
        }

        case VUS_AST_STRING_LITERAL: {
            VusAstString *s = (VusAstString*)node;
            print_indent(indent);
            printf("String: \"%s\"\n", s->value);
            break;
        }

        case VUS_AST_NUMBER_LITERAL: {
            VusAstNumber *n = (VusAstNumber*)node;
            print_indent(indent);
            printf("Number: %s%s\n", n->value, n->is_float ? " (float)" : "");
            break;
        }

        case VUS_AST_BOOL_LITERAL: {
            VusAstBool *b = (VusAstBool*)node;
            print_indent(indent);
            printf("Bool: %s\n", b->value ? "true" : "false");
            break;
        }

        case VUS_AST_NULL_LITERAL: {
            print_indent(indent);
            printf("Null\n");
            break;
        }

        case VUS_AST_LIST_LITERAL: {
            typedef struct {
                VusAstNodeType type;
                int            line;
                int            column;
                VusAstList    *items;
            } LocalListLiteral;
            LocalListLiteral *ll = (LocalListLiteral*)node;
            print_indent(indent);
            printf("List:\n");
            if (ll->items) {
                for (size_t i = 0; i < ll->items->count; i++) {
                    vus_ast_print_node(ll->items->items[i], indent + 1);
                }
            }
            break;
        }

        case VUS_AST_DICT_LITERAL: {
            typedef struct {
                VusAstNodeType type;
                int            line;
                int            column;
                VusAstList    *keys;
                VusAstList    *values;
            } LocalDictLiteral;
            LocalDictLiteral *dl = (LocalDictLiteral*)node;
            print_indent(indent);
            printf("Dict:\n");
            if (dl->keys && dl->values) {
                size_t n = dl->keys->count < dl->values->count ? dl->keys->count : dl->values->count;
                for (size_t i = 0; i < n; i++) {
                    print_indent(indent + 1);
                    printf("Key:\n");
                    vus_ast_print_node(dl->keys->items[i], indent + 2);
                    print_indent(indent + 1);
                    printf("Value:\n");
                    vus_ast_print_node(dl->values->items[i], indent + 2);
                }
            }
            break;
        }

        case VUS_AST_STRUCT_DEF: {
            VusAstStructDef *sd = (VusAstStructDef*)node;
            print_indent(indent);
            printf("StructDef: %s\n", sd->name);
            if (sd->fields) {
                for (size_t i = 0; i < sd->fields->count; i++) {
                    vus_ast_print_node(sd->fields->items[i], indent + 1);
                }
            }
            break;
        }

        case VUS_AST_STRUCT_INSTANTIATE: {
            VusAstStructInst *si = (VusAstStructInst*)node;
            print_indent(indent);
            printf("StructInst: %s\n", si->struct_name);
            if (si->args) {
                for (size_t i = 0; i < si->args->count; i++) {
                    vus_ast_print_node(si->args->items[i], indent + 1);
                }
            }
            break;
        }

        case VUS_AST_ACCESS: {
            VusAstAccess *ac = (VusAstAccess*)node;
            print_indent(indent);
            printf("Access: %s%s\n", ac->member, ac->is_optional ? " (可选)" : "");
            vus_ast_print_node(ac->object, indent + 1);
            break;
        }

        case VUS_AST_THREAD_CREATE: {
            VusAstThreadCreate *tc = (VusAstThreadCreate*)node;
            print_indent(indent);
            printf("ThreadCreate\n");
            print_indent(indent + 1);
            printf("Func:\n");
            vus_ast_print_node(tc->func, indent + 2);
            print_indent(indent + 1);
            printf("Arg:\n");
            vus_ast_print_node(tc->arg, indent + 2);
            break;
        }

        case VUS_AST_THREAD_JOIN: {
            VusAstThreadJoin *tj = (VusAstThreadJoin*)node;
            print_indent(indent);
            printf("ThreadJoin\n");
            vus_ast_print_node(tj->thread, indent + 1);
            break;
        }

        case VUS_AST_CORO_CREATE: {
            VusAstCoroCreate *cc = (VusAstCoroCreate*)node;
            print_indent(indent);
            printf("CoroCreate\n");
            print_indent(indent + 1);
            printf("Func:\n");
            vus_ast_print_node(cc->func, indent + 2);
            print_indent(indent + 1);
            printf("Arg:\n");
            vus_ast_print_node(cc->arg, indent + 2);
            break;
        }

        case VUS_AST_CORO_RESUME: {
            VusAstCoroResume *cr = (VusAstCoroResume*)node;
            print_indent(indent);
            printf("CoroResume\n");
            vus_ast_print_node(cr->coro, indent + 1);
            break;
        }

        case VUS_AST_CORO_YIELD: {
            print_indent(indent);
            printf("CoroYield\n");
            break;
        }

        default:
            print_indent(indent);
            printf("Unknown AST node type: %d\n", node->type);
            break;
    }
}

void vus_ast_print(VusAstNode *node, int indent) {
    vus_ast_print_node(node, indent);
}