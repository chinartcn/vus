/*
 * gen_builtin_io.c — 基础 IO + 日志内置函数映射
 *
 * 处理：打印/输入/转数字/转文本/睡眠/日志_xx
 * 从 generator.c 的 gen_expr_call 中拆分而来。
 */

#define _GNU_SOURCE
#include "generator.h"
#include "gen_builtin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *gen_builtin_io(GenBuf *buf, VusAstCall *call) {
    /* 打印 */
    if (strcmp(call->func_name, "打印") == 0 || strcmp(call->func_name, "print") == 0) {
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

    /* 输入 */
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

    /* 转数字 */
    if (strcmp(call->func_name, "转数字") == 0) {
        if (call->args && call->args->count > 0) {
            char *arg = gen_expr(buf, call->args->items[0]);
            size_t sz = strlen(arg) + 64;
            char *result = (char *)malloc(sz);
            snprintf(result, sz, "vus_to_string(vus_to_int(%s, &_err))", arg);
            free(arg);
            return result;
        }
        return strdup("vus_to_string(0)");
    }

    /* 转文本 */
    if (strcmp(call->func_name, "转文本") == 0) {
        if (call->args && call->args->count > 0) {
            char *arg = gen_expr(buf, call->args->items[0]);
            size_t sz = strlen(arg) + 64;
            char *result = (char *)malloc(sz);
            snprintf(result, sz, "vus_string_new(vus_string_cstr(%s))", arg);
            free(arg);
            return result;
        }
        return strdup("vus_string_new(vus_string_cstr(vus_to_string(0)))");
    }

    /* 睡眠 */
    if (strcmp(call->func_name, "睡眠") == 0) {
        if (call->args && call->args->count > 0) {
            char *arg = gen_expr(buf, call->args->items[0]);
            size_t sz = strlen(arg) + 64;
            char *result = (char *)malloc(sz);
            snprintf(result, sz, "vus_thread_sleep(%s)", arg);
            free(arg);
            return result;
        }
        return strdup("(0)");
    }

    /* 命令行参数：命令行_参数数() / 命令行_参数(i)（自举编译器 CLI 用） */
    if (strcmp(call->func_name, "命令行_参数数") == 0 ||
        strcmp(call->func_name, "cli_argc") == 0) {
        return strdup("vus_cli_argc()");
    }
    if (strcmp(call->func_name, "命令行_参数") == 0 ||
        strcmp(call->func_name, "cli_argv") == 0) {
        if (call->args && call->args->count >= 1) {
            char *arg = gen_expr(buf, call->args->items[0]);
            char result[4096];
            /* 参数为 VusString*（数字字面量经 gen_expr 已是 vus_to_string(0)），
             * 直接透传，vus_cli_argv 内部 vus_to_int 解出下标。 */
            snprintf(result, sizeof(result), "vus_cli_argv(%s)", arg);
            free(arg);
            return strdup(result);
        }
        return strdup("vus_cli_argv(vus_to_string(0))");
    }

    /* ============= 分级日志 ============= */
    if (strcmp(call->func_name, "日志_调试") == 0) {
        if (call->args && call->args->count > 0) {
            char *arg = gen_expr(buf, call->args->items[0]);
            size_t sz = strlen(arg) + 64;
            char *result = (char *)malloc(sz);
            snprintf(result, sz, "vus_log_debug(%s)", arg);
            free(arg);
            return result;
        }
        return strdup("vus_log_debug(vus_string_new(\"\"))");
    }
    if (strcmp(call->func_name, "日志_信息") == 0) {
        if (call->args && call->args->count > 0) {
            char *arg = gen_expr(buf, call->args->items[0]);
            size_t sz = strlen(arg) + 64;
            char *result = (char *)malloc(sz);
            snprintf(result, sz, "vus_log_info(%s)", arg);
            free(arg);
            return result;
        }
        return strdup("vus_log_info(vus_string_new(\"\"))");
    }
    if (strcmp(call->func_name, "日志_警告") == 0) {
        if (call->args && call->args->count > 0) {
            char *arg = gen_expr(buf, call->args->items[0]);
            size_t sz = strlen(arg) + 64;
            char *result = (char *)malloc(sz);
            snprintf(result, sz, "vus_log_warn(%s)", arg);
            free(arg);
            return result;
        }
        return strdup("vus_log_warn(vus_string_new(\"\"))");
    }
    if (strcmp(call->func_name, "日志_错误") == 0) {
        if (call->args && call->args->count > 0) {
            char *arg = gen_expr(buf, call->args->items[0]);
            size_t sz = strlen(arg) + 64;
            char *result = (char *)malloc(sz);
            snprintf(result, sz, "vus_log_error(%s)", arg);
            free(arg);
            return result;
        }
        return strdup("vus_log_error(vus_string_new(\"\"))");
    }
    if (strcmp(call->func_name, "日志_级别") == 0) {
        if (call->args && call->args->count > 0) {
            char *arg = gen_expr(buf, call->args->items[0]);
            size_t sz = strlen(arg) + 64;
            char *result = (char *)malloc(sz);
            snprintf(result, sz, "vus_log_set_level(%s)", arg);
            free(arg);
            return result;
        }
        return strdup("vus_log_set_level(vus_string_new(\"\"))");
    }

    return NULL;
}