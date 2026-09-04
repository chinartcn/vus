/*
 * gen_builtin_data.c — 数据结构内置函数映射（文本/列表/字典）
 *
 * 处理：文本_xx/列表_xx/字典_xx
 * 从 generator.c 的 gen_expr_call 中拆分而来。
 */

#define _GNU_SOURCE
#include "generator.h"
#include "gen_builtin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *gen_builtin_data(GenBuf *buf, VusAstCall *call) {
    /* ============= 文本操作 ============= */
    if (strcmp(call->func_name, "文本_长度") == 0) {
        if (call->args && call->args->count >= 1) {
            char *a = gen_expr(buf, call->args->items[0]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_to_string(vus_string_len(%s))", a);
            free(a);
            return strdup(result);
        }
        return strdup("vus_to_string(0)");
    }
    if (strcmp(call->func_name, "文本_取字符") == 0) {
        if (call->args && call->args->count >= 2) {
            char *a = gen_expr(buf, call->args->items[0]);
            char *b = gen_expr(buf, call->args->items[1]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_string_slice(%s, (int)vus_to_int(%s, &_err), 1)", a, b);
            free(a); free(b);
            return strdup(result);
        }
        return strdup("vus_string_new(\"\")");
    }
    if (strcmp(call->func_name, "文本_子串") == 0) {
        if (call->args && call->args->count >= 3) {
            char *a = gen_expr(buf, call->args->items[0]);
            char *b = gen_expr(buf, call->args->items[1]);
            char *c = gen_expr(buf, call->args->items[2]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_string_slice(%s, (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err))",
                a, b, c);
            free(a); free(b); free(c);
            return strdup(result);
        }
        return strdup("vus_string_new(\"\")");
    }

    /* ============= 列表操作 ============= */
    if (strcmp(call->func_name, "列表_追加") == 0) {
        if (call->args && call->args->count >= 2) {
            char *a = gen_expr(buf, call->args->items[0]);
            char *b = gen_expr(buf, call->args->items[1]);
            char result[4096];
            snprintf(result, sizeof(result),
                "({ vus_list_append(vus_list_unwrap((void*)(%s)), (void*)(%s)); %s; })", a, b, a);
            free(a); free(b);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "列表_取") == 0) {
        if (call->args && call->args->count >= 2) {
            char *a = gen_expr(buf, call->args->items[0]);
            char *b = gen_expr(buf, call->args->items[1]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_list_get(vus_list_unwrap((void*)(%s)), (int)vus_to_int(%s, &_err))", a, b);
            free(a); free(b);
            return strdup(result);
        }
        return strdup("vus_string_new(\"\")");
    }
    if (strcmp(call->func_name, "列表_长度") == 0) {
        if (call->args && call->args->count >= 1) {
            char *a = gen_expr(buf, call->args->items[0]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_to_string(vus_list_len(vus_list_unwrap((void*)(%s))))", a);
            free(a);
            return strdup(result);
        }
        return strdup("vus_to_string(0)");
    }

    /* ============= 字典操作 ============= */
    if (strcmp(call->func_name, "字典_设值") == 0) {
        if (call->args && call->args->count >= 3) {
            char *a = gen_expr(buf, call->args->items[0]);
            char *b = gen_expr(buf, call->args->items[1]);
            char *c = gen_expr(buf, call->args->items[2]);
            char result[4096];
            snprintf(result, sizeof(result),
                "({ vus_dict_set(vus_dict_unwrap((void*)(%s)), (VusString*)(%s), (void*)(%s)); %s; })",
                a, b, c, a);
            free(a); free(b); free(c);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "字典_取值") == 0) {
        if (call->args && call->args->count >= 2) {
            char *a = gen_expr(buf, call->args->items[0]);
            char *b = gen_expr(buf, call->args->items[1]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_dict_get(vus_dict_unwrap((void*)(%s)), (VusString*)(%s))", a, b);
            free(a); free(b);
            return strdup(result);
        }
        return strdup("vus_string_new(\"\")");
    }

    return NULL;
}