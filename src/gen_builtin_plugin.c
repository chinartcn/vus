/*
 * gen_builtin_plugin.c — 插件 + JSON 内置函数映射
 *
 * 处理：插件_xx/JSON_xx/对象文本/typeof/字典_键
 * 从 generator.c 的 gen_expr_call 中拆分而来。
 */

#define _GNU_SOURCE
#include "generator.h"
#include "gen_builtin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *gen_builtin_plugin(GenBuf *buf, VusAstCall *call) {
    /* ============= 插件调用 ============= */
    if (strcmp(call->func_name, "插件_运行") == 0) {
        if (call->args && call->args->count >= 2) {
            char *plugin = gen_expr(buf, call->args->items[0]);
            char *cmd = gen_expr(buf, call->args->items[1]);
            char result[4096];
#ifdef VUS_USE_PY
            snprintf(result, sizeof(result), "vus_plugin_run_vux_inproc(%s, %s)", plugin, cmd);
#else
            snprintf(result, sizeof(result), "vus_plugin_run_vux(%s, %s)", plugin, cmd);
#endif
            free(plugin); free(cmd);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "插件_运行JSON") == 0) {
        if (call->args && call->args->count >= 2) {
            char *plugin = gen_expr(buf, call->args->items[0]);
            char *cmd = gen_expr(buf, call->args->items[1]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_plugin_run_vux_json(%s, %s)", plugin, cmd);
            free(plugin); free(cmd);
            return strdup(result);
        }
    }

    /* ============= JSON ============= */
    if (strcmp(call->func_name, "JSON_解析") == 0) {
        if (call->args && call->args->count >= 1) {
            char *arg = gen_expr(buf, call->args->items[0]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_json_parse(%s)", arg);
            free(arg);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "JSON_生成") == 0) {
        if (call->args && call->args->count >= 1) {
            char *arg = gen_expr(buf, call->args->items[0]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_json_generate(%s)", arg);
            free(arg);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "JSON_查询") == 0) {
        if (call->args && call->args->count >= 2) {
            char *json = gen_expr(buf, call->args->items[0]);
            char *p = gen_expr(buf, call->args->items[1]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_json_query(%s, %s)", json, p);
            free(json); free(p);
            return strdup(result);
        }
    }

    /* 对象文本 */
    if (strcmp(call->func_name, "对象文本") == 0) {
        if (call->args && call->args->count >= 1) {
            char *arg = gen_expr(buf, call->args->items[0]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_object_to_string(%s)", arg);
            free(arg);
            return strdup(result);
        }
    }

    /* 字典_键 */
    if (strcmp(call->func_name, "字典_键") == 0) {
        if (call->args && call->args->count >= 1) {
            char *arg = gen_expr(buf, call->args->items[0]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_dict_keys_of(%s)", arg);
            free(arg);
            return strdup(result);
        }
    }

    /* typeof */
    if (strcmp(call->func_name, "typeof") == 0 || strcmp(call->func_name, "类型") == 0) {
        if (call->args && call->args->count >= 1) {
            char *arg = gen_expr(buf, call->args->items[0]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_typeof(%s)", arg);
            free(arg);
            return strdup(result);
        }
    }

    return NULL;
}