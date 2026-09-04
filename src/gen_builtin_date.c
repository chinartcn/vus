/*
 * gen_builtin_date.c — 日期时间 + Termux 内置函数映射
 *
 * 处理：日期_xx/Termux_xx
 * 从 generator.c 的 gen_expr_call 中拆分而来。
 */

#define _GNU_SOURCE
#include "generator.h"
#include "gen_builtin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *gen_builtin_date(GenBuf *buf, VusAstCall *call) {
    /* ============= Termux-X11 ============= */
    if (strcmp(call->func_name, "Termux_启动X11") == 0) {
        return strdup("vus_termux_start_x11()");
    }
    if (strcmp(call->func_name, "Termux_启动GPU") == 0) {
        return strdup("vus_termux_start_gl()");
    }

    /* ============= 日期时间 ============= */
    if (strcmp(call->func_name, "日期_现在") == 0) {
        return strdup("vus_plugin_date_now(NULL)");
    }
    if (strcmp(call->func_name, "日期_时间戳") == 0) {
        return strdup("vus_plugin_date_timestamp(NULL)");
    }
    if (strcmp(call->func_name, "日期_年") == 0) {
        return strdup("vus_plugin_date_year(NULL)");
    }
    if (strcmp(call->func_name, "日期_月") == 0) {
        return strdup("vus_plugin_date_month(NULL)");
    }
    if (strcmp(call->func_name, "日期_日") == 0) {
        return strdup("vus_plugin_date_day(NULL)");
    }
    if (strcmp(call->func_name, "日期_时") == 0) {
        return strdup("vus_plugin_date_hour(NULL)");
    }
    if (strcmp(call->func_name, "日期_分") == 0) {
        return strdup("vus_plugin_date_minute(NULL)");
    }
    if (strcmp(call->func_name, "日期_秒") == 0) {
        return strdup("vus_plugin_date_second(NULL)");
    }
    if (strcmp(call->func_name, "日期_格式化") == 0) {
        if (call->args && call->args->count >= 1) {
            char *arg = gen_expr(buf, call->args->items[0]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_plugin_date_format(%s)", arg);
            free(arg);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "日期_解析") == 0) {
        if (call->args && call->args->count >= 2) {
            char *arg1 = gen_expr(buf, call->args->items[0]);
            char *arg2 = gen_expr(buf, call->args->items[1]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_plugin_date_parse(%s, %s)", arg1, arg2);
            free(arg1); free(arg2);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "日期_从时间戳") == 0) {
        if (call->args && call->args->count >= 1) {
            char *arg = gen_expr(buf, call->args->items[0]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_plugin_date_from_timestamp(%s)", arg);
            free(arg);
            return strdup(result);
        }
    }

    return NULL;
}