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
    buf->cap = 16384;
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

/* ============ 结构体类型跟踪 ============ */

typedef struct GenStructField {
    char *name;
    char *type_ann;
} GenStructField;

typedef struct GenStructInfo {
    char           *name;
    GenStructField *fields;
    size_t          num_fields;
    struct GenStructInfo *next;
} GenStructInfo;

/* 添加结构体定义到类型表 */
static GenStructInfo *gen_struct_add(GenStructInfo *list, const char *name,
                                     GenStructField *fields, size_t num_fields) {
    GenStructInfo *info = (GenStructInfo *)calloc(1, sizeof(GenStructInfo));
    if (!info) return list;
    info->name = strdup(name);
    info->fields = fields;
    info->num_fields = num_fields;
    info->next = list;
    return info;
}

/* 查找包含指定字段名的结构体名称 */
static const char *gen_find_struct_by_field(GenStructInfo *list, const char *field_name) {
    for (GenStructInfo *si = list; si; si = si->next) {
        for (size_t i = 0; i < si->num_fields; i++) {
            if (strcmp(si->fields[i].name, field_name) == 0) {
                return si->name;
            }
        }
    }
    return NULL;
}

/* 查找结构体字段的类型注解 */
static const char *gen_find_field_type(GenStructInfo *list, const char *struct_name, const char *field_name) {
    for (GenStructInfo *si = list; si; si = si->next) {
        if (strcmp(si->name, struct_name) == 0) {
            for (size_t i = 0; i < si->num_fields; i++) {
                if (strcmp(si->fields[i].name, field_name) == 0) {
                    return si->fields[i].type_ann;
                }
            }
        }
    }
    return NULL;
}

/* 检查指定名称的结构体是否存在 */
static int gen_struct_exists(GenStructInfo *list, const char *name) {
    for (GenStructInfo *si = list; si; si = si->next) {
        if (strcmp(si->name, name) == 0) return 1;
    }
    return 0;
}

/* 释放结构体类型表 */
static void gen_struct_free(GenStructInfo *list) {
    while (list) {
        GenStructInfo *next = list->next;
        free(list->name);
        for (size_t i = 0; i < list->num_fields; i++) {
            free(list->fields[i].name);
            free(list->fields[i].type_ann);
        }
        free(list->fields);
        free(list);
        list = next;
    }
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
static GenStructInfo *s_gen_structs = NULL; /* 结构体类型表，用于成员访问 */

static char *gen_expr_access(GenBuf *buf, VusAstAccess *access) {
    /* 生成成员访问表达式
     * 对于 p.name，生成 ((vus_struct_StructName*)vus_p)->vus_name
     * 通过检查 s_gen_structs 列表来尝试匹配字段。
     * 支持链式访问如 r.p1.x，通过递归解析中间类型。
     */
    char *obj = gen_expr(buf, access->object);
    char san_member[256];
    gen_sanitize_name(access->member, san_member, sizeof(san_member));

    /* 尝试确定用于类型转换的结构体名称 */
    const char *struct_name = NULL;

    VusAstNode *obj_node = access->object;
    if (obj_node && obj_node->type == VUS_AST_IDENTIFIER) {
        /* 简单标识符访问：通过字段名匹配结构体 */
        struct_name = gen_find_struct_by_field(s_gen_structs, access->member);
    } else if (obj_node && obj_node->type == VUS_AST_ACCESS) {
        /* 链式访问：解析中间表达式返回的结构体类型 */
        VusAstAccess *inner = (VusAstAccess *)obj_node;
        /* 确定内层访问的对象所属的结构体 */
        const char *inner_struct = NULL;
        if (inner->object && inner->object->type == VUS_AST_IDENTIFIER) {
            inner_struct = gen_find_struct_by_field(s_gen_structs, inner->member);
        }
        /* 如果找到了内层结构体，查找字段的类型注解 */
        if (inner_struct) {
            const char *member_type = gen_find_field_type(s_gen_structs, inner_struct, inner->member);
            if (member_type && gen_struct_exists(s_gen_structs, member_type)) {
                struct_name = member_type;
            }
        }
        /* 如果仍未解析，尝试直接匹配字段名 */
        if (!struct_name) {
            struct_name = gen_find_struct_by_field(s_gen_structs, access->member);
        }
    }

    if (struct_name) {
        char san_struct[256];
        gen_sanitize_name(struct_name, san_struct, sizeof(san_struct));
        /* obj 在模板中被使用两次，需要更大的缓冲区 */
        size_t sz = strlen(obj) * 2 + strlen(san_struct) + strlen(san_member) + 256;
        char *result = (char *)malloc(sz);
        snprintf(result, sz,
            "({if(!%s){VusString* _null_str=vus_string_new(\"\");_null_str;}((vus_struct_%s*)%s)->vus_%s;})",
            obj, san_struct, obj, san_member);
        free(obj);
        return result;
    }

    /* 兜底：生成通用访问表达式（使用成员名作为类型名） */
    size_t sz = strlen(obj) * 2 + strlen(san_member) + strlen(san_member) + 256;
    char *result = (char *)malloc(sz);
    snprintf(result, sz,
        "({if(!%s){VusString* _null_str=vus_string_new(\"\");_null_str;}((vus_struct_%s*)%s)->vus_%s;})",
        obj, san_member, obj, san_member);
    free(obj);
    return result;
}

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
        size_t sz = strlen(left) + strlen(right) + 64;
        result = (char *)malloc(sz);
        snprintf(result, sz, "vus_add(%s, %s)", left, right);
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
    } else if (strcmp(un->op, "~") == 0) {
        size_t sz = strlen(operand) + 128;
        result = (char *)malloc(sz);
        snprintf(result, sz,
            "vus_to_string(~vus_to_int(%s, &_err))",
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

    /* ============= TUI 插件内置函数 ============= */
    if (strcmp(call->func_name, "tui_清屏") == 0) {
        return strdup("vus_plugin_tui_clear(NULL)");
    }
    if (strcmp(call->func_name, "tui_重置") == 0) {
        return strdup("vus_plugin_tui_reset(NULL)");
    }
    if (strcmp(call->func_name, "tui_设置颜色") == 0) {
        if (call->args && call->args->count >= 2) {
            char *fg = gen_expr(buf, call->args->items[0]);
            char *bg = gen_expr(buf, call->args->items[1]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_plugin_tui_set_color(%s, %s)", fg, bg);
            free(fg); free(bg);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "tui_定位") == 0) {
        if (call->args && call->args->count >= 2) {
            char *row = gen_expr(buf, call->args->items[0]);
            char *col = gen_expr(buf, call->args->items[1]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_plugin_tui_locate(%s, %s)", row, col);
            free(row); free(col);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "tui_进度条") == 0) {
        if (call->args && call->args->count >= 3) {
            char *cur = gen_expr(buf, call->args->items[0]);
            char *tot = gen_expr(buf, call->args->items[1]);
            char *wid = gen_expr(buf, call->args->items[2]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_plugin_tui_progress(%s, %s, %s)", cur, tot, wid);
            free(cur); free(tot); free(wid);
            return strdup(result);
        }
    }

    /* ============= 网络插件内置函数 ============= */
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

    /* ============= 文件操作内置函数 ============= */
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

    /* ============= 日期时间内置函数 ============= */
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

    /* 如果有泛型类型参数，添加注释 */
    if (call->type_args && call->type_args->count > 0) {
        pos += snprintf(args_buf + pos, sizeof(args_buf) - pos, "/* <");
        for (size_t i = 0; i < call->type_args->count; i++) {
            VusAstNode *pnode = call->type_args->items[i];
            if (pnode->type == VUS_AST_PARAM) {
                VusAstParam *tp = (VusAstParam *)pnode;
                if (i > 0) pos += snprintf(args_buf + pos, sizeof(args_buf) - pos, ", ");
                pos += snprintf(args_buf + pos, sizeof(args_buf) - pos, "%s", tp->name);
            }
        }
        pos += snprintf(args_buf + pos, sizeof(args_buf) - pos, "> */");
    }

    pos += snprintf(args_buf + pos, sizeof(args_buf) - pos, "({VusString* _vus_args[%zu];_vus_args[0]=NULL;", nargs + 1);
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
        case VUS_AST_ACCESS:
            return gen_expr_access(buf, (VusAstAccess *)node);
        case VUS_AST_STRUCT_INSTANTIATE: {
            VusAstStructInst *si = (VusAstStructInst *)node;
            char san[256];
            gen_sanitize_name(si->struct_name, san, sizeof(san));
            size_t nargs = si->args ? si->args->count : 0;
            char **arg_exprs = NULL;
            if (nargs > 0) {
                arg_exprs = (char **)calloc(nargs, sizeof(char *));
                for (size_t i = 0; i < nargs; i++) {
                    arg_exprs[i] = gen_expr(buf, si->args->items[i]);
                }
            }
            char args_buf[4096] = {0};
            size_t pos = 0;
            pos += snprintf(args_buf + pos, sizeof(args_buf) - pos,
                "({VusString* _vus_args[%zu];_vus_args[0]=NULL;", nargs + 1);
            for (size_t i = 0; i < nargs; i++) {
                pos += snprintf(args_buf + pos, sizeof(args_buf) - pos,
                    "_vus_args[%zu]=%s;", i + 1, arg_exprs[i]);
            }
            pos += snprintf(args_buf + pos, sizeof(args_buf) - pos,
                "vus_%s(_vus_args);_vus_args[0];})", san);
            for (size_t i = 0; i < nargs; i++) free(arg_exprs[i]);
            free(arg_exprs);
            return strdup(args_buf);
        }

        /* ============= 线程/协程表达式 ============= */
        case VUS_AST_THREAD_CREATE: {
            VusAstThreadCreate *tc = (VusAstThreadCreate *)node;
            char *func = gen_expr(buf, tc->func);
            char *arg = gen_expr(buf, tc->arg);
            char result[4096];
            snprintf(result, sizeof(result),
                "({_VusThreadTask _task={(void(*)(void*))%s, (void*)%s};"
                "vus_thread_create_handle(_vus_thread_run, &_task);})",
                func, arg);
            free(func);
            free(arg);
            return strdup(result);
        }
        case VUS_AST_THREAD_JOIN: {
            VusAstThreadJoin *tj = (VusAstThreadJoin *)node;
            char *thread = gen_expr(buf, tj->thread);
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_thread_join_handle(%s)", thread);
            free(thread);
            return strdup(result);
        }
        case VUS_AST_CORO_CREATE: {
            VusAstCoroCreate *cc = (VusAstCoroCreate *)node;
            char *func = gen_expr(buf, cc->func);
            char *arg = gen_expr(buf, cc->arg);
            char result[4096];
            snprintf(result, sizeof(result),
                "({_VusThreadTask _task={(void(*)(void*))%s, (void*)%s};"
                "vus_coro_create_handle((void(*)(void*))_vus_thread_run, &_task);})",
                func, arg);
            free(func);
            free(arg);
            return strdup(result);
        }
        case VUS_AST_CORO_RESUME: {
            VusAstCoroResume *cr = (VusAstCoroResume *)node;
            char *coro = gen_expr(buf, cr->coro);
            char result[1024];
            snprintf(result, sizeof(result),
                "(vus_coro_resume_handle(%s), NULL)", coro);
            free(coro);
            return strdup(result);
        }
        case VUS_AST_CORO_YIELD: {
            return strdup("(vus_coro_yield(), NULL)");
        }
        case VUS_AST_SUBSCRIPT: {
            VusAstSubscript *sub = (VusAstSubscript *)node;
            char *obj = gen_expr(buf, sub->object);
            char *idx = gen_expr(buf, sub->index);
            /* 生成 vus_list_get / vus_list_set 调用 */
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_list_get((VusList*)(%s), vus_to_int(%s, &_err))",
                obj, idx);
            free(obj);
            free(idx);
            return strdup(result);
        }
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

    if (assign->is_local) {
        /* 局部变量：直接赋值，变量已在函数顶部声明 */
        gen_emit_linef(buf, "{ VusString* _tmp = %s;", val);
        gen_emit_linef(buf, "vus_ref(_tmp);");
        gen_emit_linef(buf, "vus_unref(vus_%s);", san);
        gen_emit_linef(buf, "vus_%s = _tmp; }", san);
    } else {
        /* 全局变量 */
        gen_emit_linef(buf, "{ VusString* _tmp = %s;", val);
        gen_emit_linef(buf, "vus_ref(_tmp);");
        gen_emit_linef(buf, "vus_unref(vus_%s);", san);
        gen_emit_linef(buf, "vus_%s = _tmp; }", san);
    }

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
    gen_emit_linef(buf, "for (int64_t _i = _start; _i <= _end; _i++) {");
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
        gen_emit_line(buf, "vus_stack_pop();");
        gen_emit_line(buf, "return;");
        free(val);
    } else {
        gen_emit_line(buf, "vus_stack_pop();");
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
    /* try-catch 实现：使用 setjmp/longjmp 风格，保存/恢复错误状态 */
    gen_emit_line(buf, "{");
    buf->indent++;
    gen_emit_line(buf, "VusError* _saved_err = _vus_err;");
    gen_emit_line(buf, "_vus_err = NULL;");
    gen_emit_line(buf, "int _vus_caught = 0;");

    /* try 块 — 用 do-while(0) 包裹，使 throw 能用 break 跳出 */
    gen_emit_line(buf, "/* try block */");
    gen_emit_line(buf, "do {");
    buf->indent++;
    if (try_stmt->try_body) {
        for (size_t i = 0; i < try_stmt->try_body->count; i++) {
            gen_statement(buf, try_stmt->try_body->items[i]);
        }
    }
    buf->indent--;
    gen_emit_line(buf, "} while(0);");

    /* except 块 */
    if (try_stmt->except_bodies && try_stmt->except_bodies->count > 0) {
        gen_emit_line(buf, "if (_vus_err && !_vus_caught) {");
        buf->indent++;
        /* 遍历所有 except 子句 */
        for (size_t i = 0; i < try_stmt->except_bodies->count; i++) {
            VusAstIdentifier *et = (VusAstIdentifier *)try_stmt->except_types->items[i];
            VusAstList *body = (VusAstList *)try_stmt->except_bodies->items[i];
            if (!body) continue;

            if (et && et->name && et->name[0]) {
                /* 带类型匹配的 except */
                if (i == 0) {
                    gen_emit_linef(buf, "if (strcmp(_vus_err->msg, \"%s\") == 0) {", et->name);
                } else {
                    gen_emit_linef(buf, "} else if (strcmp(_vus_err->msg, \"%s\") == 0) {", et->name);
                }
            } else {
                /* 通配 except（无类型或空类型名） */
                if (i == 0) {
                    gen_emit_line(buf, "{");
                } else {
                    gen_emit_line(buf, "} else {");
                }
            }
            buf->indent++;
            gen_emit_line(buf, "vus_error_print(_vus_err);");
            gen_emit_line(buf, "vus_error_free(_vus_err);");
            gen_emit_line(buf, "_vus_err = NULL;");
            gen_emit_line(buf, "_vus_caught = 1;");
            for (size_t j = 0; j < body->count; j++) {
                gen_statement(buf, body->items[j]);
            }
            gen_emit_line(buf, "}");
            buf->indent--;
        }
        buf->indent--;
        gen_emit_line(buf, "}");
        /* 如果所有 except 都不匹配，恢复错误 */
        gen_emit_line(buf, "if (!_vus_caught && _vus_err) {");
        buf->indent++;
        gen_emit_line(buf, "/* 未捕获的异常，恢复错误状态 */");
        gen_emit_line(buf, "vus_error_push(&_saved_err, _vus_err);");
        gen_emit_line(buf, "_vus_err = _saved_err;");
        buf->indent--;
        gen_emit_line(buf, "}");
    }

    /* 恢复原始错误状态（如果没有新的错误） */
    gen_emit_line(buf, "if (!_vus_err) {");
    buf->indent++;
    gen_emit_line(buf, "_vus_err = _saved_err;");
    buf->indent--;
    gen_emit_line(buf, "}");

    buf->indent--;
    gen_emit_line(buf, "}");
}

static void gen_stmt_throw(GenBuf *buf, VusAstThrow *thr) {
    char *val = gen_expr(buf, thr->value);
    gen_emit_linef(buf, "_vus_err = vus_error_new(1, vus_string_cstr(%s), __LINE__, __func__);", val);
    gen_emit_linef(buf, "vus_unref(%s);", val);
    gen_emit_line(buf, "break;");
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
        case VUS_AST_STRUCT_DEF:
            /* 结构体定义在顶层生成 C 类型，语句级别跳过 */
            break;
        default:
            break;
    }
}

/* ============ 函数定义生成 ============ */

/* 递归扫描 AST 节点，收集局部变量名 */
static void gen_collect_locals(VusAstNode *node, VusAstList *locals) {
    if (!node) return;
    if (node->type == VUS_AST_ASSIGN) {
        VusAstAssign *assign = (VusAstAssign *)node;
        if (assign->is_local) {
            /* 检查是否已收集 */
            for (size_t i = 0; i < locals->count; i++) {
                VusAstIdentifier *id = (VusAstIdentifier *)locals->items[i];
                if (strcmp(id->name, assign->target) == 0) return;
            }
            VusAstIdentifier *id = vus_ast_ident_new(assign->target, 0, 0);
            vus_ast_list_push(locals, (VusAstNode *)id);
        }
    } else if (node->type == VUS_AST_IF) {
        VusAstIf *ifn = (VusAstIf *)node;
        if (ifn->then_body) {
            for (size_t i = 0; i < ifn->then_body->count; i++)
                gen_collect_locals(ifn->then_body->items[i], locals);
        }
        if (ifn->elif_bodies) {
            for (size_t i = 0; i < ifn->elif_bodies->count; i++) {
                VusAstList *body = (VusAstList *)ifn->elif_bodies->items[i];
                if (body) {
                    for (size_t j = 0; j < body->count; j++)
                        gen_collect_locals(body->items[j], locals);
                }
            }
        }
        if (ifn->else_body) {
            for (size_t i = 0; i < ifn->else_body->count; i++)
                gen_collect_locals(ifn->else_body->items[i], locals);
        }
    } else if (node->type == VUS_AST_FOR_RANGE) {
        VusAstForRange *fr = (VusAstForRange *)node;
        if (fr->body) {
            for (size_t i = 0; i < fr->body->count; i++)
                gen_collect_locals(fr->body->items[i], locals);
        }
    } else if (node->type == VUS_AST_FOR_EACH) {
        VusAstForEach *fe = (VusAstForEach *)node;
        if (fe->body) {
            for (size_t i = 0; i < fe->body->count; i++)
                gen_collect_locals(fe->body->items[i], locals);
        }
    } else if (node->type == VUS_AST_WHILE) {
        VusAstWhile *wl = (VusAstWhile *)node;
        if (wl->body) {
            for (size_t i = 0; i < wl->body->count; i++)
                gen_collect_locals(wl->body->items[i], locals);
        }
    } else if (node->type == VUS_AST_TRY) {
        VusAstTry *tryn = (VusAstTry *)node;
        if (tryn->try_body) {
            for (size_t i = 0; i < tryn->try_body->count; i++)
                gen_collect_locals(tryn->try_body->items[i], locals);
        }
        if (tryn->except_bodies) {
            for (size_t i = 0; i < tryn->except_bodies->count; i++) {
                VusAstList *body = (VusAstList *)tryn->except_bodies->items[i];
                if (body) {
                    for (size_t j = 0; j < body->count; j++)
                        gen_collect_locals(body->items[j], locals);
                }
            }
        }
    }
}

static void gen_function(GenBuf *buf, VusAstFunctionDef *func) {
    char san[256];
    gen_sanitize_name(func->name, san, sizeof(san));

    /* 函数注释 */
    gen_emit_linef(buf, "/* VUS function: %s */", func->name);

    /* 泛型类型参数注释 */
    if (func->type_params && func->type_params->count > 0) {
        char tp_buf[512] = {0};
        size_t pos = 0;
        pos += snprintf(tp_buf + pos, sizeof(tp_buf) - pos, "/* generic type params: ");
        for (size_t i = 0; i < func->type_params->count; i++) {
            VusAstNode *pnode = func->type_params->items[i];
            if (pnode->type == VUS_AST_PARAM) {
                VusAstParam *tp = (VusAstParam *)pnode;
                if (i > 0) pos += snprintf(tp_buf + pos, sizeof(tp_buf) - pos, ", ");
                pos += snprintf(tp_buf + pos, sizeof(tp_buf) - pos, "%s", tp->name);
            }
        }
        snprintf(tp_buf + pos, sizeof(tp_buf) - pos, " */");
        gen_emit_line(buf, tp_buf);
    }

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
    gen_emit_line(buf, "VusError* _vus_err = NULL;");

    /* 扫描函数体中的局部变量，在函数顶部声明 */
    if (func->body) {
        VusAstList *locals = vus_ast_list_new();
        for (size_t i = 0; i < func->body->count; i++) {
            gen_collect_locals(func->body->items[i], locals);
        }
        for (size_t i = 0; i < locals->count; i++) {
            VusAstIdentifier *id = (VusAstIdentifier *)locals->items[i];
            char lsan[256];
            gen_sanitize_name(id->name, lsan, sizeof(lsan));
            gen_emit_linef(buf, "VusString* vus_%s = NULL;", lsan);
        }
        vus_ast_list_free(locals);
    }

    /* 栈追踪：记录函数调用 */
    gen_emit_linef(buf, "vus_stack_push(\"%s\");", func->name);

    /* 函数体 */
    if (func->body) {
        for (size_t i = 0; i < func->body->count; i++) {
            gen_statement(buf, func->body->items[i]);
        }
    }

    /* 栈追踪：函数退出 */
    gen_emit_line(buf, "vus_stack_pop();");

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

/* ============ 结构体代码生成 ============ */

/* 生成 C 结构体类型定义 */
static void gen_struct_type_def(GenBuf *buf, VusAstStructDef *sd) {
    char san[256];
    gen_sanitize_name(sd->name, san, sizeof(san));

    gen_emit_linef(buf, "/* VUS struct: %s */", sd->name);
    gen_emit_linef(buf, "typedef struct vus_struct_%s {", san);
    buf->indent++;
    gen_emit_linef(buf, "int ref;  /* 引用计数 */");

    if (sd->fields) {
        for (size_t i = 0; i < sd->fields->count; i++) {
            VusAstNode *fnode = sd->fields->items[i];
            if (fnode->type == VUS_AST_PARAM) {
                VusAstParam *p = (VusAstParam *)fnode;
                char fsan[256];
                gen_sanitize_name(p->name, fsan, sizeof(fsan));
                gen_emit_linef(buf, "VusString* vus_%s;", fsan);
            }
        }
    }

    buf->indent--;
    gen_emit_linef(buf, "} vus_struct_%s;\n", san);
}

/* 生成结构体构造函数 */
static void gen_struct_constructor(GenBuf *buf, VusAstStructDef *sd) {
    char san[256];
    gen_sanitize_name(sd->name, san, sizeof(san));

    gen_emit_linef(buf, "/* VUS struct constructor: %s */", sd->name);
    gen_emit_linef(buf, "void vus_%s(void* _args) {", san);
    buf->indent++;

    gen_emit_line(buf, "VusString** _vus_params = (VusString**)_args;");
    gen_emit_linef(buf, "vus_struct_%s* _obj = (vus_struct_%s*)calloc(1, sizeof(vus_struct_%s));",
                   san, san, san);

    if (sd->fields) {
        for (size_t i = 0; i < sd->fields->count; i++) {
            VusAstNode *fnode = sd->fields->items[i];
            if (fnode->type == VUS_AST_PARAM) {
                VusAstParam *p = (VusAstParam *)fnode;
                char fsan[256];
                gen_sanitize_name(p->name, fsan, sizeof(fsan));
                gen_emit_linef(buf, "if (_vus_params[%zu]) {", i + 1);
                buf->indent++;
                gen_emit_linef(buf, "_obj->vus_%s = _vus_params[%zu];", fsan, i + 1);
                gen_emit_linef(buf, "vus_ref(_obj->vus_%s);", fsan);
                buf->indent--;
                gen_emit_line(buf, "}");
            }
        }
    }

    gen_emit_line(buf, "_vus_params[0] = (VusString*)_obj;");
    buf->indent--;
    gen_emit_line(buf, "}\n");
}

/* ============ 主函数生成 ============ */

static void gen_main_function(GenBuf *buf, VusAstProgram *program, int debug) {
    gen_emit_line(buf, "int main(void) {");
    buf->indent++;

    if (debug) {
        gen_emit_line(buf, "vus_debug_enabled = 1;");
    }

    gen_emit_line(buf, "int _err = 0;");
    gen_emit_line(buf, "VusError* _vus_err = NULL;");

    /* 遍历所有顶层语句（跳过函数定义和结构体定义，它们已单独生成） */
    if (program->statements) {
        for (size_t i = 0; i < program->statements->count; i++) {
            VusAstNode *node = program->statements->items[i];
            if (node->type != VUS_AST_FUNCTION_DEF && node->type != VUS_AST_STRUCT_DEF) {
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

    /* 线程/协程运行时辅助 */
    gen_emit(buf, "/* 线程/协程运行时辅助 */\n");
    gen_emit(buf, "typedef struct { void (*func)(void*); void* arg; } _VusThreadTask;\n");
    gen_emit(buf, "static void* _vus_thread_run(void* _arg) {\n");
    gen_emit(buf, "    _VusThreadTask* _task = (_VusThreadTask*)_arg;\n");
    gen_emit(buf, "    VusString* _vus_args[2] = {NULL, (VusString*)_task->arg};\n");
    gen_emit(buf, "    _task->func(_vus_args);\n");
    gen_emit(buf, "    return _vus_args[0];\n");
    gen_emit(buf, "}\n\n");

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

    /* 收集结构体定义并生成 C 结构体类型 */
    s_gen_structs = NULL;
    if (program->statements) {
        for (size_t i = 0; i < program->statements->count; i++) {
            VusAstNode *node = program->statements->items[i];
            if (node->type == VUS_AST_STRUCT_DEF) {
                VusAstStructDef *sd = (VusAstStructDef *)node;

                /* 构建字段信息 */
                size_t nf = sd->fields ? sd->fields->count : 0;
                GenStructField *fields = NULL;
                if (nf > 0) {
                    fields = (GenStructField *)calloc(nf, sizeof(GenStructField));
                    for (size_t j = 0; j < nf; j++) {
                        VusAstNode *fnode = sd->fields->items[j];
                        if (fnode->type == VUS_AST_PARAM) {
                            VusAstParam *p = (VusAstParam *)fnode;
                            fields[j].name = strdup(p->name);
                            fields[j].type_ann = p->type_annotation ? strdup(p->type_annotation) : NULL;
                        }
                    }
                }
                s_gen_structs = gen_struct_add(s_gen_structs, sd->name, fields, nf);

                /* 生成 C 结构体类型定义 */
                gen_struct_type_def(buf, sd);
            }
        }
    }

    /* 生成结构体构造函数 */
    if (program->statements) {
        for (size_t i = 0; i < program->statements->count; i++) {
            VusAstNode *node = program->statements->items[i];
            if (node->type == VUS_AST_STRUCT_DEF) {
                gen_struct_constructor(buf, (VusAstStructDef *)node);
            }
        }
    }

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
    gen_main_function(buf, program, config->debug);

    char *result = strdup(buf->data);
    free(buf->data);
    free(buf);

    /* 清理结构体类型表 */
    gen_struct_free(s_gen_structs);
    s_gen_structs = NULL;

    return result;
}

int vus_compile_c(const char *c_source_path, const char *output_path,
                  VusConfig *config, char *error_msg, size_t error_size,
                  const char *extra_objects) {
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

    /* 将 rt_dir 解析为绝对路径（相对于 project_dir） */
    char abs_rt_dir[2048];
    if (config->rt_dir[0] == '/') {
        /* 已经是绝对路径 */
        strncpy(abs_rt_dir, config->rt_dir, sizeof(abs_rt_dir) - 1);
        abs_rt_dir[sizeof(abs_rt_dir) - 1] = '\0';
    } else if (config->project_dir[0]) {
        snprintf(abs_rt_dir, sizeof(abs_rt_dir), "%s/%s", config->project_dir, config->rt_dir);
    } else {
        strncpy(abs_rt_dir, config->rt_dir, sizeof(abs_rt_dir) - 1);
        abs_rt_dir[sizeof(abs_rt_dir) - 1] = '\0';
    }

    /* 确定 rt 源文件路径（libvus_rt.c + vus_coro.c 一起编译） */
    char rt_source[2048];
    char rt_coro[2048];
    if (config->rt_dir[0]) {
        snprintf(rt_source, sizeof(rt_source), "%s/libvus_rt.c", abs_rt_dir);
        snprintf(rt_coro,   sizeof(rt_coro),   "%s/vus_coro.c", abs_rt_dir);
    } else {
        rt_source[0] = '\0';
        rt_coro[0] = '\0';
    }

    /* 构建 GCC 命令 */
    char cmd[8192];
    int n;

    /* 检测系统是否安装了 libcurl 开发头文件 */
    int has_curl = 0;
    FILE *curl_check = popen("curl-config --version >/dev/null 2>&1", "r");
    if (curl_check) {
        has_curl = (pclose(curl_check) == 0);
    }

    const char *curl_def = has_curl ? "-DVUS_HAVE_CURL" : "";
    const char *curl_lib = has_curl ? "-lcurl" : "";

    if (extra_objects && extra_objects[0]) {
        n = snprintf(cmd, sizeof(cmd),
            "gcc %s -g %s -I\"%s\" \"%s\" \"%s\" \"%s\" %s -o \"%s\" -lm -lpthread %s 2>&1",
            opt_level,
            curl_def,
            abs_rt_dir,
            c_source_path,
            rt_source,
            rt_coro,
            extra_objects,
            output_path,
            curl_lib);
    } else {
        n = snprintf(cmd, sizeof(cmd),
            "gcc %s -g %s -I\"%s\" \"%s\" \"%s\" \"%s\" -o \"%s\" -lm -lpthread %s 2>&1",
            opt_level,
            curl_def,
            abs_rt_dir,
            c_source_path,
            rt_source,
            rt_coro,
            output_path,
            curl_lib);
    }

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