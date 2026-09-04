/*
 * gen_builtin_net.c — 网络 + 文件操作内置函数映射
 *
 * 处理：网络_xx/文件_xx/命令_执行/文本_分割
 * 从 generator.c 的 gen_expr_call 中拆分而来。
 */

#define _GNU_SOURCE
#include "generator.h"
#include "gen_builtin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *gen_builtin_net_file(GenBuf *buf, VusAstCall *call) {
    /* ============= 网络 ============= */
    if (strcmp(call->func_name, "网络_GET") == 0) {
        if (call->args && call->args->count >= 1) {
            char *url = gen_expr(buf, call->args->items[0]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_plugin_http_get(%s)", url);
            free(url);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "网络_POST") == 0) {
        if (call->args && call->args->count >= 2) {
            char *url = gen_expr(buf, call->args->items[0]);
            char *data = gen_expr(buf, call->args->items[1]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_plugin_http_post(%s, %s)", url, data);
            free(url); free(data);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "网络_下载") == 0) {
        if (call->args && call->args->count >= 2) {
            char *url = gen_expr(buf, call->args->items[0]);
            char *path = gen_expr(buf, call->args->items[1]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_plugin_http_download(%s, %s)", url, path);
            free(url); free(path);
            return strdup(result);
        }
    }

    /* ============= 文件操作 ============= */
    if (strcmp(call->func_name, "文件_读取") == 0) {
        if (call->args && call->args->count >= 1) {
            char *arg = gen_expr(buf, call->args->items[0]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_plugin_file_read(%s)", arg);
            free(arg);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "文件_写入") == 0) {
        if (call->args && call->args->count >= 2) {
            char *arg1 = gen_expr(buf, call->args->items[0]);
            char *arg2 = gen_expr(buf, call->args->items[1]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_plugin_file_write(%s, %s)", arg1, arg2);
            free(arg1); free(arg2);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "文件_追加") == 0) {
        if (call->args && call->args->count >= 2) {
            char *arg1 = gen_expr(buf, call->args->items[0]);
            char *arg2 = gen_expr(buf, call->args->items[1]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_plugin_file_append(%s, %s)", arg1, arg2);
            free(arg1); free(arg2);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "文件_存在") == 0) {
        if (call->args && call->args->count >= 1) {
            char *arg = gen_expr(buf, call->args->items[0]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_plugin_file_exists(%s)", arg);
            free(arg);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "文件_删除") == 0) {
        if (call->args && call->args->count >= 1) {
            char *arg = gen_expr(buf, call->args->items[0]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_plugin_file_delete(%s)", arg);
            free(arg);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "文件_列表") == 0) {
        if (call->args && call->args->count >= 1) {
            char *arg = gen_expr(buf, call->args->items[0]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_plugin_file_list(%s)", arg);
            free(arg);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "文件_是目录") == 0) {
        if (call->args && call->args->count >= 1) {
            char *arg = gen_expr(buf, call->args->items[0]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_plugin_file_isdir(%s)", arg);
            free(arg);
            return strdup(result);
        }
    }

    /* shell 命令执行 */
    if (strcmp(call->func_name, "命令_执行") == 0) {
        if (call->args && call->args->count >= 1) {
            char *arg = gen_expr(buf, call->args->items[0]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_plugin_shell_exec(%s)", arg);
            free(arg);
            return strdup(result);
        }
    }

    /* 文本分割 */
    if (strcmp(call->func_name, "文本_分割") == 0) {
        if (call->args && call->args->count >= 2) {
            char *a = gen_expr(buf, call->args->items[0]);
            char *b = gen_expr(buf, call->args->items[1]);
            char result[8192];
            snprintf(result, sizeof(result), "vus_plugin_text_split(%s, %s)", a, b);
            free(a); free(b);
            return strdup(result);
        }
        return strdup("vus_string_new(\"[]\")");
    }

    return NULL;
}