/*
 * config.c — VUS 项目配置加载实现
 *
 * 从 vus.json 加载项目配置，提供配置字段访问和路径构建函数。
 * 内置简易 JSON 解析器，无外部依赖。
 */

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* ============ 简易 JSON 解析器 ============ */

/* 解析上下文 */
typedef struct {
    const char *json;
    size_t      pos;
    size_t      len;
    int         error;
} JsonCtx;

/* 跳过空白字符 */
static void json_skip_ws(JsonCtx *ctx)
{
    while (ctx->pos < ctx->len) {
        unsigned char c = (unsigned char)ctx->json[ctx->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ctx->pos++;
        } else {
            break;
        }
    }
}

/* 查看当前字符 */
static int json_peek(JsonCtx *ctx)
{
    json_skip_ws(ctx);
    if (ctx->pos >= ctx->len) return EOF;
    return (unsigned char)ctx->json[ctx->pos];
}

/* 消费一个字符 */
static int json_next(JsonCtx *ctx)
{
    json_skip_ws(ctx);
    if (ctx->pos >= ctx->len) return EOF;
    return (unsigned char)ctx->json[ctx->pos++];
}

/* 检查是否匹配字符串 */
static int json_match(JsonCtx *ctx, const char *s)
{
    size_t saved = ctx->pos;
    json_skip_ws(ctx);
    size_t slen = strlen(s);
    if (ctx->pos + slen > ctx->len) return 0;
    if (strncmp(ctx->json + ctx->pos, s, slen) == 0) {
        ctx->pos += slen;
        return 1;
    }
    ctx->pos = saved;
    return 0;
}

/* 解析 JSON 字符串 "..." 并复制到 dst，返回实际长度 */
static size_t json_parse_string(JsonCtx *ctx, char *dst, size_t dst_size)
{
    int c = json_next(ctx);
    if (c != '"') {
        ctx->error = 1;
        return 0;
    }

    size_t i = 0;
    while (ctx->pos < ctx->len) {
        c = (unsigned char)ctx->json[ctx->pos++];
        if (c == '"') {
            break;
        }
        if (c == '\\' && ctx->pos < ctx->len) {
            /* 简单的转义处理 */
            unsigned char esc = (unsigned char)ctx->json[ctx->pos++];
            if (esc == '"' || esc == '\\' || esc == '/') {
                if (i + 1 < dst_size) dst[i++] = (char)esc;
            } else if (esc == 'n') {
                if (i + 1 < dst_size) dst[i++] = '\n';
            } else if (esc == 't') {
                if (i + 1 < dst_size) dst[i++] = '\t';
            } else if (esc == 'r') {
                if (i + 1 < dst_size) dst[i++] = '\r';
            } else {
                /* 其他转义：原样保留反斜杠和字符 */
                if (i + 1 < dst_size) dst[i++] = '\\';
                if (i + 1 < dst_size) dst[i++] = (char)esc;
            }
        } else {
            if (i + 1 < dst_size) dst[i++] = (char)c;
        }
    }
    if (i < dst_size) dst[i] = '\0';
    return i;
}

/* 解析 JSON 布尔值或 null */
static int json_parse_bool(JsonCtx *ctx, int *out_val)
{
    if (json_match(ctx, "true")) {
        *out_val = 1;
        return 1;
    }
    if (json_match(ctx, "false")) {
        *out_val = 0;
        return 1;
    }
    return 0;
}

/* 跳过整个 JSON 值（用于跳出不关心的字段） */
static void json_skip_value(JsonCtx *ctx)
{
    int c = json_peek(ctx);
    if (c == '"') {
        char buf[2];
        json_parse_string(ctx, buf, sizeof(buf));
    } else if (c == '{') {
        json_next(ctx); /* 跳过 { */
        int first = 1;
        while (json_peek(ctx) != '}' && ctx->pos < ctx->len && !ctx->error) {
            if (!first) json_next(ctx); /* 跳过逗号 */
            first = 0;
            /* 跳过 key */
            json_skip_value(ctx);
            /* 跳过冒号 */
            json_next(ctx);
            /* 跳过 value */
            json_skip_value(ctx);
        }
        json_next(ctx); /* 跳过 } */
    } else if (c == '[') {
        json_next(ctx); /* 跳过 [ */
        int first = 1;
        while (json_peek(ctx) != ']' && ctx->pos < ctx->len && !ctx->error) {
            if (!first) json_next(ctx); /* 跳过逗号 */
            first = 0;
            json_skip_value(ctx);
        }
        json_next(ctx); /* 跳过 ] */
    } else {
        /* 跳过数字或关键字 */
        while (ctx->pos < ctx->len) {
            c = (unsigned char)ctx->json[ctx->pos];
            if (c == ',' || c == '}' || c == ']' || c == ' ' ||
                c == '\t' || c == '\n' || c == '\r') break;
            ctx->pos++;
        }
    }
}

/* 在对象中查找指定 key，并解析其字符串值 */
static int json_read_string_field(JsonCtx *ctx, const char *key,
                                   char *dst, size_t dst_size)
{
    /* 期望当前处于对象起始位置 { */
    if (json_peek(ctx) != '{') return 0;
    json_next(ctx); /* 跳过 { */

    while (json_peek(ctx) != '}' && ctx->pos < ctx->len && !ctx->error) {
        /* 解析 key */
        char field_key[256];
        if (json_peek(ctx) != '"') {
            json_skip_value(ctx);
            continue;
        }
        json_parse_string(ctx, field_key, sizeof(field_key));

        /* 跳过冒号 */
        json_next(ctx);

        if (strcmp(field_key, key) == 0) {
            /* 匹配到目标 key，解析值 */
            if (json_peek(ctx) == '"') {
                json_parse_string(ctx, dst, dst_size);
                return 1;
            }
            return 0;
        }

        /* 跳过值 */
        json_skip_value(ctx);

        /* 跳过逗号 */
        if (json_peek(ctx) == ',') json_next(ctx);
    }
    return 0;
}

/* ============ 配置加载 ============ */

int vus_config_load(VusConfig *config, const char *project_dir)
{
    if (!config || !project_dir) return -1;

    /* 清空并设置默认值 */
    memset(config, 0, sizeof(VusConfig));

    strncpy(config->project_dir, project_dir, sizeof(config->project_dir) - 1);
    strncpy(config->style, "函数", sizeof(config->style) - 1);
    strncpy(config->main_file, "main.vus", sizeof(config->main_file) - 1);
    strncpy(config->output_mode, "c", sizeof(config->output_mode) - 1);
    strncpy(config->list_mode, "严格", sizeof(config->list_mode) - 1);
    config->debug = 0;
    strncpy(config->target_platform, "linux-gnu", sizeof(config->target_platform) - 1);
    strncpy(config->optimization, "速度", sizeof(config->optimization) - 1);
    strncpy(config->arm_version, "ARM64", sizeof(config->arm_version) - 1);
    strncpy(config->rt_dir, "rt", sizeof(config->rt_dir) - 1);
    strncpy(config->build_dir, "构建", sizeof(config->build_dir) - 1);

    /* 构建 vus.json 路径 */
    char json_path[2048];
    int n = snprintf(json_path, sizeof(json_path), "%s/vus.json", project_dir);
    if (n < 0 || (size_t)n >= sizeof(json_path)) return -1;

    /* 读取文件 */
    FILE *fp = fopen(json_path, "r");
    if (!fp) {
        /* 文件不存在，使用默认配置 */
        return 0;
    }

    /* 获取文件大小 */
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    if (fsize <= 0) {
        fclose(fp);
        return 0;
    }
    fseek(fp, 0, SEEK_SET);

    /* 分配缓冲区并读取 */
    char *content = (char *)malloc((size_t)fsize + 1);
    if (!content) {
        fclose(fp);
        return -1;
    }
    size_t read_size = fread(content, 1, (size_t)fsize, fp);
    fclose(fp);
    content[read_size] = '\0';

    /* 初始化解析上下文 */
    JsonCtx ctx;
    ctx.json   = content;
    ctx.pos    = 0;
    ctx.len    = read_size;
    ctx.error  = 0;

    /* 解析顶层对象 */
    /* 跳过可能的前导空白 */
    json_skip_ws(&ctx);
    if (json_peek(&ctx) != '{') {
        free(content);
        return -1;
    }
    json_next(&ctx); /* 跳过 { */

    /* 遍历所有字段 */
    while (json_peek(&ctx) != '}' && ctx.pos < ctx.len && !ctx.error) {
        char field_key[256];
        if (json_peek(&ctx) != '"') {
            json_skip_value(&ctx);
            if (json_peek(&ctx) == ',') json_next(&ctx);
            continue;
        }
        json_parse_string(&ctx, field_key, sizeof(field_key));

        /* 跳过冒号 */
        json_next(&ctx);

        if (strcmp(field_key, "name") == 0) {
            json_parse_string(&ctx, config->name, sizeof(config->name));
        } else if (strcmp(field_key, "version") == 0) {
            json_parse_string(&ctx, config->version, sizeof(config->version));
        } else if (strcmp(field_key, "风格") == 0) {
            json_parse_string(&ctx, config->style, sizeof(config->style));
        } else if (strcmp(field_key, "主文件") == 0) {
            json_parse_string(&ctx, config->main_file, sizeof(config->main_file));
        } else if (strcmp(field_key, "输出模式") == 0) {
            json_parse_string(&ctx, config->output_mode, sizeof(config->output_mode));
        } else if (strcmp(field_key, "列表模式") == 0) {
            json_parse_string(&ctx, config->list_mode, sizeof(config->list_mode));
        } else if (strcmp(field_key, "调试") == 0) {
            int bool_val;
            if (json_parse_bool(&ctx, &bool_val)) {
                config->debug = bool_val;
            } else {
                /* 尝试数字 */
                char num_buf[16];
                size_t ni = 0;
                while (ctx.pos < ctx.len && ni < sizeof(num_buf) - 1) {
                    int c = (unsigned char)ctx.json[ctx.pos];
                    if (c == '0' || c == '1') {
                        num_buf[ni++] = (char)c;
                        ctx.pos++;
                    } else {
                        break;
                    }
                }
                num_buf[ni] = '\0';
                if (ni > 0) config->debug = atoi(num_buf);
            }
        } else if (strcmp(field_key, "目标平台") == 0) {
            json_parse_string(&ctx, config->target_platform, sizeof(config->target_platform));
        } else if (strcmp(field_key, "编译选项") == 0) {
            /* 嵌套对象 */
            if (json_peek(&ctx) == '{') {
                /* 保存当前 pos，创建子上下文 */
                JsonCtx sub_ctx = ctx;
                json_read_string_field(&sub_ctx, "优化",
                                       config->optimization,
                                       sizeof(config->optimization));
                JsonCtx sub_ctx2 = ctx;
                json_read_string_field(&sub_ctx2, "ARM版本",
                                       config->arm_version,
                                       sizeof(config->arm_version));
                /* 跳过整个对象 */
                json_skip_value(&ctx);
            } else {
                json_skip_value(&ctx);
            }
        } else {
            /* 不关心的字段，跳过 */
            json_skip_value(&ctx);
        }

        /* 跳过逗号 */
        if (json_peek(&ctx) == ',') json_next(&ctx);
    }

    free(content);
    return 0;
}

/* ============ 路径构建函数 ============ */

void vus_config_main_path(VusConfig *config, char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, "%s/%s",
             config->project_dir, config->main_file);
}

void vus_config_build_path(VusConfig *config, char *buf, size_t buf_size)
{
    if (config->build_dir[0] != '\0') {
        snprintf(buf, buf_size, "%s/%s",
                 config->project_dir, config->build_dir);
    } else {
        snprintf(buf, buf_size, "%s/构建", config->project_dir);
    }
}

void vus_config_rt_header_path(VusConfig *config, char *buf, size_t buf_size)
{
    if (config->rt_dir[0] != '\0') {
        snprintf(buf, buf_size, "%s/libvus_rt.h", config->rt_dir);
    } else {
        /* 默认相对于项目目录下的 rt/ */
        snprintf(buf, buf_size, "%s/rt/libvus_rt.h", config->project_dir);
    }
}

void vus_config_rt_source_path(VusConfig *config, char *buf, size_t buf_size)
{
    if (config->rt_dir[0] != '\0') {
        snprintf(buf, buf_size, "%s/libvus_rt.c", config->rt_dir);
    } else {
        /* 默认相对于项目目录下的 rt/ */
        snprintf(buf, buf_size, "%s/rt/libvus_rt.c", config->project_dir);
    }
}