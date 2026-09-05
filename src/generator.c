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

/* 当前是否正在生成 main 函数体。用于让顶层「返回」生成 `return 0;`，
 * 避免 int main 中出现无返回值的 `return;` 在严格编译下报错。 */
static int s_gen_in_main = 0;

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
static int g_uses_gui = 0; /* 是否在代码中使用了 GuiLite 图形内建函数（决定链接参数） */
static int g_uses_png = 0; /* 是否使用 图形_背景图（决定追加 -lpng -lz 链接库） */
static int g_uses_freetype = 0; /* 是否使用 图形_字体_加载（决定追加 -lfreetype 链接库） */

/* ============ VUA（Android 组件流）界面内建：登记/闭包包装 ============
 * 界面_绑定 需把一个 .vus 用户函数包装成 VusClosure。闭包包装函数（静态、文件作用域）
 * 收集到 g_vua_premain，在主函数之前统一喷出。用一个计数器保证包装函数名唯一。 */
static GenBuf *g_vua_premain = NULL;     /* 收集 界面_绑定 的静态包装函数 */
static GenBuf *g_vua_fwd = NULL;         /* 收集包装函数的前向声明（放 include 后） */
static int     g_vua_bind_count = 0;     /* 包装函数序号，用于唯一命名 */
static int     g_vua_op_count = 0;       /* 界面_* 内建语句表达式临时变量序号（唯一命名） */
static int     g_uses_vua = 0;           /* 用户代码是否用了 界面_*（决定 include vua.h）*/
static VusAstProgram *g_vua_prog = NULL; /* 当前生成中的 AST 根（界面_绑定 查事件函数参数用） */

/* 顶层全局变量名集合：parser 在函数内对任何赋值一律标记 is_local=1，
 * 若不把「顶层已声明的名字」排除出局部收集，函数内对全局变量的赋值会在函数
 * 顶部生成同名局部声明（VusString* vus_x = NULL;），遮蔽文件级全局，
 * 导致函数内（含 界面_绑定 事件函数）读写全局变量静默失效。
 * 收集这些名字后，gen_collect_locals 对全局名跳过 → 函数内直接引用文件级符号。 */
static char *s_global_names[512];
static int    s_global_count = 0;

static int gen_is_global_name(const char *name) {
    if (!name) return 0;
    for (int i = 0; i < s_global_count; i++)
        if (s_global_names[i] && strcmp(s_global_names[i], name) == 0) return 1;
    return 0;
}

/* ---- 顶层文件级变量收集（递归进入顶层控制流子块）----
 * 修复反馈 1.1：此前只扫描顶层「平坦」赋值，如果/循环/异常块内的赋值不生成
 * 全局声明，C 代码报 undeclared。现递归遍历顶层 if/elif/else、for、while、try
 * 的子块，把其中所有赋值目标/显式全局声明一并：登记全局名（函数内不收集为
 * 局部，避免遮蔽）+ 生成 VusString* 全局声明（去重）。函数定义体不进入。 */
static void gen_globals_walk_list(VusAstList *list, GenBuf *gl);

static void gen_globals_declare_name(const char *name, GenBuf *gl) {
    if (!name) return;
    if (!gen_is_global_name(name) && s_global_count < 512)
        s_global_names[s_global_count++] = (char *)name;
    if (!gl) return;
    char san[256];
    gen_sanitize_name(name, san, sizeof(san));
    char decl[320];
    snprintf(decl, sizeof(decl), "VusString* vus_%s = NULL;\n", san);
    if (!strstr(gl->data, decl)) gen_emit(gl, decl);
}

static void gen_globals_walk_stmt(VusAstNode *node, GenBuf *gl) {
    if (!node) return;
    switch (node->type) {
    case VUS_AST_ASSIGN:
        gen_globals_declare_name(((VusAstAssign *)node)->target, gl);
        break;
    case VUS_AST_MULTI_ASSIGN: {
        VusAstMultiAssign *ma = (VusAstMultiAssign *)node;
        if (ma->targets)
            for (size_t i = 0; i < ma->targets->count; i++)
                gen_globals_declare_name(((VusAstIdentifier *)ma->targets->items[i])->name, gl);
        break;
    }
    case VUS_AST_GLOBAL_DECL:
        gen_globals_declare_name(((VusAstGlobalDecl *)node)->name, gl);
        break;
    case VUS_AST_IF: {
        VusAstIf *s = (VusAstIf *)node;
        gen_globals_walk_list(s->then_body, gl);
        if (s->elif_bodies)
            for (size_t i = 0; i < s->elif_bodies->count; i++)
                gen_globals_walk_list((VusAstList *)s->elif_bodies->items[i], gl);
        gen_globals_walk_list(s->else_body, gl);
        break;
    }
    case VUS_AST_FOR_RANGE:
        gen_globals_walk_list(((VusAstForRange *)node)->body, gl);
        break;
    case VUS_AST_FOR_EACH:
        gen_globals_walk_list(((VusAstForEach *)node)->body, gl);
        break;
    case VUS_AST_WHILE:
        gen_globals_walk_list(((VusAstWhile *)node)->body, gl);
        break;
    case VUS_AST_TRY: {
        VusAstTry *t = (VusAstTry *)node;
        gen_globals_walk_list(t->try_body, gl);
        if (t->except_bodies)
            for (size_t i = 0; i < t->except_bodies->count; i++)
                gen_globals_walk_list((VusAstList *)t->except_bodies->items[i], gl);
        break;
    }
    default:
        break;   /* 函数定义/结构体定义/表达式等不进文件级变量收集 */
    }
}

static void gen_globals_walk_list(VusAstList *list, GenBuf *gl) {
    if (!list) return;
    for (size_t i = 0; i < list->count; i++) {
        VusAstNode *node = list->items[i];
        if (node) gen_globals_walk_stmt(node, gl);
    }
}

/* 按函数名在 AST 根里查函数定义；未找到返回 NULL。 */
static VusAstFunctionDef *gen_vua_find_func(const char *name) {
    if (!g_vua_prog || !name || !g_vua_prog->statements) return NULL;
    for (size_t i = 0; i < g_vua_prog->statements->count; i++) {
        VusAstNode *node = g_vua_prog->statements->items[i];
        if (node->type == VUS_AST_FUNCTION_DEF) {
            VusAstFunctionDef *fn = (VusAstFunctionDef *)node;
            if (fn->name && strcmp(fn->name, name) == 0) return fn;
        }
    }
    return NULL;
}

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

/* gen_binary_concat: 字符串拼接 (..) */
static char *gen_binary_concat(const char *left, const char *right) {
    size_t sz = strlen(left) + strlen(right) + 64;
    char *r = (char *)malloc(sz);
    snprintf(r, sz, "vus_string_concat(%s, %s)", left, right);
    return r;
}

/* 判断 AST 节点是否为「纯十进制整数常量」：是则填值返回 1（供编译期折叠，避免
 * 每步都走 vus_to_string(N) → vus_to_int() 的字符串往返）。 */
static int node_int_literal(VusAstNode *node, int64_t *out) {
    if (node && node->type == VUS_AST_NUMBER_LITERAL) {
        VusAstNumber *n = (VusAstNumber *)node;
        if (!n->is_float && n->value && n->value[0]) {
            char *end = NULL;
            long long v = strtoll(n->value, &end, 10);
            if (end && *end == '\0') { *out = (int64_t)v; return 1; }
        }
    }
    return 0;
}

/* gen_cond: 把 if/elif/while 的条件 AST 直接编译成 C 布尔表达式（int 语境），
 * 替代原先「比较转成 "true"/"false" 字符串、再 strcmp=="true"」的三层包装。
 * 语义等价：VUS 布尔值即字符串 "true"（真）/其他（假）。
 *  - 比较/逻辑/取反 → 紧凑 C 运算符（短路语义与原先一致的，两段均无副作用）
 *  - 其余表达式值 → (strcmp(vus_string_cstr(EXPR), "true") == 0)
 * 调用方 free 返回值。 */
static char *gen_cond(GenBuf *buf, VusAstNode *node) {
    if (!node) return strdup("0");
    if (node->type == VUS_AST_BINARY_OP) {
        VusAstBinaryOp *b = (VusAstBinaryOp *)node;
        const char *op = b->op;
        if (strcmp(op, "and") == 0 || strcmp(op, "和") == 0 ||
            strcmp(op, "or") == 0 || strcmp(op, "或") == 0) {
            char *l = gen_cond(buf, b->left);
            char *r = gen_cond(buf, b->right);
            const char *cop = (strcmp(op, "or") == 0 || strcmp(op, "或") == 0) ? "||" : "&&";
            size_t sz = strlen(l) + strlen(r) + 32;
            char *res = (char *)malloc(sz);
            snprintf(res, sz, "(%s %s %s)", l, cop, r);
            free(l); free(r);
            return res;
        }
        if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
            strcmp(op, "<") == 0 || strcmp(op, ">") == 0 ||
            strcmp(op, "<=") == 0 || strcmp(op, ">=") == 0) {
            /* 双常量比较：编译期求值 */
            int64_t lv, rv;
            if (node_int_literal(b->left, &lv) && node_int_literal(b->right, &rv)) {
                int truth = 0;
                if (strcmp(op, "==") == 0) truth = (lv == rv);
                else if (strcmp(op, "!=") == 0) truth = (lv != rv);
                else if (strcmp(op, "<") == 0) truth = (lv < rv);
                else if (strcmp(op, ">") == 0) truth = (lv > rv);
                else if (strcmp(op, "<=") == 0) truth = (lv <= rv);
                else if (strcmp(op, ">=") == 0) truth = (lv >= rv);
                return strdup(truth ? "1" : "0");
            }
            char *l = gen_expr(buf, b->left);
            char *r = gen_expr(buf, b->right);
            size_t sz = strlen(l) + strlen(r) + 64;
            char *res = (char *)malloc(sz);
            snprintf(res, sz, "(vus_compare(%s, %s) %s 0)", l, r, op);
            free(l); free(r);
            return res;
        }
        /* 其余（算术等）按值表达式回退 */
    }
    if (node->type == VUS_AST_UNARY_OP) {
        VusAstUnaryOp *u = (VusAstUnaryOp *)node;
        if (strcmp(u->op, "not") == 0 || strcmp(u->op, "非") == 0) {
            char *inner = gen_cond(buf, u->operand);
            size_t sz = strlen(inner) + 16;
            char *res = (char *)malloc(sz);
            snprintf(res, sz, "(!%s)", inner);
            free(inner);
            return res;
        }
    }
    /* 值表达式：保持原有「字符串为 "true" 才为真」语义 */
    char *e = gen_expr(buf, node);
    size_t sz = strlen(e) + 64;
    char *res = (char *)malloc(sz);
    snprintf(res, sz, "(strcmp(vus_string_cstr(%s), \"true\") == 0)", e);
    free(e);
    return res;
}

/* gen_binary_compare: 比较运算符 (==, !=, <, >, <=, >=) → "true"/"false"
 * 使用 vus_compare 智能比较：两者均为数字时按数值比较，否则按字典序。
 * 修复了旧实现用 vus_to_int 把非数字字符串都转 0 导致 "abc"=="xyz" 误判为真的缺陷。
 */
static char *gen_binary_compare(const char *left, const char *right, const char *op) {
    size_t sz = strlen(left) + strlen(right) + 128;
    char *r = (char *)malloc(sz);
    snprintf(r, sz,
        "vus_literal((vus_compare(%s, %s) %s 0) ? \"true\" : \"false\")",
        left, right, op);
    return r;
}

/* gen_binary_arith: 算术/位运算符 (+, -, *, /, %, &, |, ^, <<, >>) → 数字字符串 */
static char *gen_binary_arith(const char *left, const char *right, const char *op) {
    size_t sz = strlen(left) + strlen(right) + 128;
    char *r = (char *)malloc(sz);
    snprintf(r, sz,
        "vus_to_string(vus_to_int(%s, &_err) %s vus_to_int(%s, &_err))",
        left, op, right);
    return r;
}

/* gen_binary_add: 特殊加法 (vus_add 支持字符串/数字混合) */
static char *gen_binary_add(const char *left, const char *right) {
    size_t sz = strlen(left) + strlen(right) + 64;
    char *r = (char *)malloc(sz);
    snprintf(r, sz, "vus_add(%s, %s)", left, right);
    return r;
}

/* gen_binary_logical: 逻辑运算符 (and/和, or/或) → "true"/"false" */
static char *gen_binary_logical(const char *left, const char *right, const char *op) {
    size_t sz = strlen(left) + strlen(right) + 128;
    char *r = (char *)malloc(sz);
    snprintf(r, sz,
        "vus_literal((strcmp(vus_string_cstr(%s), \"true\") == 0 %s strcmp(vus_string_cstr(%s), \"true\") == 0) ? \"true\" : \"false\")",
        left, op, right);
    return r;
}

/* 整数常量折叠（编译期求值）：
 *  - 双纯整数常量：+ - * / % & | ^ << >> 与 .. 直接算出发送结果，
 *    避免原先每步 vus_to_string(N) → vus_to_int() 的字符串往返；
 *  - 单整数常量 + 纯算术（- * / % & | ^ << >>）：常量侧代以裸数字参与运算
 *    （vus_to_int 两侧同级，语义等价）；"+"（vus_add 支持字符串拼接）不折叠。
 * 无可折叠时返回 NULL，走原生成路径。 */
static char *gen_binary_cfold(GenBuf *buf, VusAstBinaryOp *bin) {
    int64_t lv = 0, rv = 0;
    int li = node_int_literal(bin->left, &lv);
    int ri = node_int_literal(bin->right, &rv);
    const char *op = bin->op;

    if (li && ri) {
        if (strcmp(op, "..") == 0) {
            char lstr[64], rstr[64], ele[128], ere[128];
            snprintf(lstr, sizeof(lstr), "%lld", (long long)lv);
            snprintf(rstr, sizeof(rstr), "%lld", (long long)rv);
            gen_string_escape(lstr, ele, sizeof(ele));
            gen_string_escape(rstr, ere, sizeof(ere));
            size_t sz = strlen(ele) + strlen(ere) + 64;
            char *res = (char *)malloc(sz);
            /* 双常量连接结果是唯一值 → 驻留池（与 vus_literal 字符串语义一致） */
            snprintf(res, sz, "vus_literal(\"%s%s\")", ele, ere);
            return res;
        }
        if (strcmp(op, "+") == 0) {
            char *res = (char *)malloc(64);
            snprintf(res, 64, "vus_literal(\"%lld\")", (long long)(lv + rv));
            return res;
        }
        if (strcmp(op, "-") == 0 || strcmp(op, "*") == 0 || strcmp(op, "/") == 0 ||
            strcmp(op, "%") == 0 || strcmp(op, "&") == 0 || strcmp(op, "|") == 0 ||
            strcmp(op, "^") == 0 || strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0) {
            int64_t r;
            if (strcmp(op, "-") == 0) r = lv - rv;
            else if (strcmp(op, "*") == 0) r = lv * rv;
            else if (strcmp(op, "/") == 0) r = lv / rv;
            else if (strcmp(op, "%") == 0) r = lv % rv;
            else if (strcmp(op, "&") == 0) r = lv & rv;
            else if (strcmp(op, "|") == 0) r = lv | rv;
            else if (strcmp(op, "^") == 0) r = lv ^ rv;
            else if (strcmp(op, "<<") == 0) r = lv << rv;
            else r = lv >> rv;
            char *res = (char *)malloc(64);
            /* 双常量纯算术 → 宿总值进字面量池 */
            snprintf(res, 64, "vus_literal(\"%lld\")", (long long)r);
            return res;
        }
    }

    /* 单常量 + 纯算术：常量侧用裸 int64 直接参与 C 运算 */
    if (li != ri && (strcmp(op, "-") == 0 || strcmp(op, "*") == 0 || strcmp(op, "/") == 0 ||
                     strcmp(op, "%") == 0 || strcmp(op, "&") == 0 || strcmp(op, "|") == 0 ||
                     strcmp(op, "^") == 0 || strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0)) {
        char *res = NULL;
        if (li) {
            char *right = gen_expr(buf, bin->right);
            size_t sz = strlen(right) + 64;
            res = (char *)malloc(sz);
            snprintf(res, sz, "vus_to_string(%lld %s vus_to_int(%s, &_err))",
                     (long long)lv, op, right);
            free(right);
        } else {
            char *left = gen_expr(buf, bin->left);
            size_t sz = strlen(left) + 64;
            res = (char *)malloc(sz);
            snprintf(res, sz, "vus_to_string(vus_to_int(%s, &_err) %s %lld)",
                     left, op, (long long)rv);
            free(left);
        }
        return res;
    }
    return NULL;
}

/* 递归求值「纯整数常量表达式」：节点或其子树全部由整数常量 + 纯算术语义构成
 * 时直接算出 int64 结果（含 '+'：vus_add 对两侧数字做整数加法，语义一致）。
 * 任一环节出现变量/调用/字符串等非常量即返回 0。 */
static int eval_const_int(VusAstNode *node, int64_t *out) {
    if (!node) return 0;
    switch (node->type) {
        case VUS_AST_NUMBER_LITERAL: {
            VusAstNumber *n = (VusAstNumber *)node;
            if (n->is_float || !n->value) return 0;
            char *end = NULL;
            long long v = strtoll(n->value, &end, 10);
            if (!end || *end != '\0') return 0;
            *out = (int64_t)v;
            return 1;
        }
        case VUS_AST_BINARY_OP: {
            VusAstBinaryOp *b = (VusAstBinaryOp *)node;
            const char *op = b->op;
            if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0 || strcmp(op, "*") == 0 ||
                strcmp(op, "/") == 0 || strcmp(op, "%") == 0 || strcmp(op, "&") == 0 ||
                strcmp(op, "|") == 0 || strcmp(op, "^") == 0 ||
                strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0) {
                int64_t lv, rv;
                if (!eval_const_int(b->left, &lv) || !eval_const_int(b->right, &rv)) return 0;
                if (strcmp(op, "+") == 0) *out = lv + rv;
                else if (strcmp(op, "-") == 0) *out = lv - rv;
                else if (strcmp(op, "*") == 0) *out = lv * rv;
                else if (strcmp(op, "/") == 0) *out = lv / rv;
                else if (strcmp(op, "%") == 0) *out = lv % rv;
                else if (strcmp(op, "&") == 0) *out = lv & rv;
                else if (strcmp(op, "|") == 0) *out = lv | rv;
                else if (strcmp(op, "^") == 0) *out = lv ^ rv;
                else if (strcmp(op, "<<") == 0) *out = lv << rv;
                else if (strcmp(op, ">>") == 0) *out = lv >> rv;
                return 1;
            }
            return 0;
        }
        default:
            return 0;
    }
}

/* 判断 AST 节点是否为字符串字面量（供「字符串 + 字符串」编译期警告，P7）：
 * 动态类型下只有字面量能静态判定，与反馈场景「"7" + "5" 悄悄变加法」一致。 */
static int node_is_string_literal(VusAstNode *node) {
    return node && node->type == VUS_AST_STRING_LITERAL;
}

static char *gen_expr_binary(GenBuf *buf, VusAstBinaryOp *bin) {
    /* 整棵子树为纯整数常量表达式 → 编译期算出最终值，直接进字面量池
     * （vus_literal("N") 与 vus_to_string(N) 输出一致，免运行时分配） */
    int64_t cval;
    if (eval_const_int((VusAstNode *)bin, &cval)) {
        char *res = (char *)malloc(64);
        if (res) snprintf(res, 64, "vus_literal(\"%lld\")", (long long)cval);
        return res;
    }

    char *folded = gen_binary_cfold(buf, bin);
    if (folded) return folded;

    /* P7 (1.3)：`字符串 + 字符串` 编译期警告 —— `+` 是算术加法，`..` 才是拼接；
     * "7" + "5" 会静默得到 12，新用户想拼字符串时会踩坑。 */
    if (strcmp(bin->op, "+") == 0 &&
        node_is_string_literal(bin->left) && node_is_string_literal(bin->right)) {
        fprintf(stderr, "警告: 第 %d 行: 字符串 + 字符串会被当作算术加法（如 \"7\"+\"5\" → 12）；"
                        "拼接字符串请用 ..\n", bin->line);
    }

    char *left = gen_expr(buf, bin->left);
    char *right = gen_expr(buf, bin->right);
    char *result = NULL;

    if (strcmp(bin->op, "..") == 0) {
        result = gen_binary_concat(left, right);
    } else if (strcmp(bin->op, "==") == 0) {
        result = gen_binary_compare(left, right, "==");
    } else if (strcmp(bin->op, "!=") == 0) {
        result = gen_binary_compare(left, right, "!=");
    } else if (strcmp(bin->op, "<") == 0) {
        result = gen_binary_compare(left, right, "<");
    } else if (strcmp(bin->op, ">") == 0) {
        result = gen_binary_compare(left, right, ">");
    } else if (strcmp(bin->op, "<=") == 0) {
        result = gen_binary_compare(left, right, "<=");
    } else if (strcmp(bin->op, ">=") == 0) {
        result = gen_binary_compare(left, right, ">=");
    } else if (strcmp(bin->op, "+") == 0) {
        result = gen_binary_add(left, right);
    } else if (strcmp(bin->op, "-") == 0) {
        result = gen_binary_arith(left, right, "-");
    } else if (strcmp(bin->op, "*") == 0) {
        result = gen_binary_arith(left, right, "*");
    } else if (strcmp(bin->op, "/") == 0) {
        result = gen_binary_arith(left, right, "/");
    } else if (strcmp(bin->op, "%") == 0) {
        result = gen_binary_arith(left, right, "%");
    } else if (strcmp(bin->op, "and") == 0 || strcmp(bin->op, "和") == 0) {
        result = gen_binary_logical(left, right, "&&");
    } else if (strcmp(bin->op, "or") == 0 || strcmp(bin->op, "或") == 0) {
        result = gen_binary_logical(left, right, "||");
    } else if (strcmp(bin->op, "&") == 0) {
        result = gen_binary_arith(left, right, "&");
    } else if (strcmp(bin->op, "|") == 0) {
        result = gen_binary_arith(left, right, "|");
    } else if (strcmp(bin->op, "^") == 0) {
        result = gen_binary_arith(left, right, "^");
    } else if (strcmp(bin->op, "<<") == 0) {
        result = gen_binary_arith(left, right, "<<");
    } else if (strcmp(bin->op, ">>") == 0) {
        result = gen_binary_arith(left, right, ">>");
    } else {
        /* 兜底：未知运算符 */
        result = gen_binary_concat(left, right);
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
            "vus_literal((strcmp(vus_string_cstr(%s), \"true\") == 0) ? \"false\" : \"true\")",
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

    /* ============= 函数一等公民 =============
     * 函数值(函数名)  → 返回装箱的 VusObject(TYPE_FUNC)，作为可传递/可调用的函数值。
     * 调用(函数值, 实参...)  → 解包函数值并调用，返回其返回值。 */
    if (strcmp(call->func_name, "函数值") == 0) {
        if (call->args && call->args->count == 1 &&
            call->args->items[0]->type == VUS_AST_IDENTIFIER) {
            VusAstIdentifier *fid = (VusAstIdentifier *)call->args->items[0];
            char fs[256];
            gen_sanitize_name(fid->name, fs, sizeof(fs));
            char *r = (char *)malloc(strlen(fs) + 128);
            snprintf(r, strlen(fs) + 128,
                "({VusObject* _fv = vus_object_func((void(*)(void*))vus_%s); (VusString*)_fv;})", fs);
            return r;
        }
        return strdup("vus_string_new(\"\")");
    }
    if (strcmp(call->func_name, "调用") == 0 || strcmp(call->func_name, "调用函数") == 0) {
        /* 调用(函数值, 实参...) ：函数值是 args[0]，实参 args[1..] */
        size_t nargs = call->args ? call->args->count : 0;
        if (nargs < 1) return strdup("vus_string_new(\"\")");
        char *fval = gen_expr(buf, call->args->items[0]);
        char **arg_exprs = NULL;
        size_t nparams = nargs - 1;
        if (nparams > 0) {
            arg_exprs = (char **)calloc(nparams, sizeof(char *));
            for (size_t i = 0; i < nparams; i++)
                arg_exprs[i] = gen_expr(buf, call->args->items[i + 1]);
        }
        char args_buf[4096] = {0};
        size_t pos = 0;
        pos += snprintf(args_buf + pos, sizeof(args_buf) - pos,
            "({VusString* _vus_args[%zu];_vus_args[0]=NULL;", nparams + 1);
        for (size_t i = 0; i < nparams; i++)
            pos += snprintf(args_buf + pos, sizeof(args_buf) - pos,
                "_vus_args[%zu]=%s;", i + 1, arg_exprs[i]);
        pos += snprintf(args_buf + pos, sizeof(args_buf) - pos,
            "VusObject* _fvx = (VusObject*)(%s); vus_object_func_call(_fvx, _vus_args); _vus_args[0];})", fval);
        for (size_t i = 0; i < nparams; i++) free(arg_exprs[i]);
        if (arg_exprs) free(arg_exprs);
        free(fval);
        return strdup(args_buf);
    }

    /* ============= VUA 界面内建（Android 组件流，rt/vua.c） =============
     * 多屏导航 + 事件绑定。驱动全局 VuaSession（vua_global_session）。
     * 界面_显示(路径)  读 .vua 压栈成为当前屏；
     * 界面_返回() / 界面_返回至(名)  弹栈导航；
     * 界面_绑定(事件名, 处理函数)  把 .vus 函数登记为当前屏的事件 handler。 */
    if (strcmp(call->func_name, "界面_显示") == 0) {
        g_uses_vua = 1;
        if (call->args && call->args->count >= 1) {
            char *p = gen_expr(buf, call->args->items[0]);
            char *r = (char *)malloc(strlen(p) + 128);
            snprintf(r, strlen(p) + 128,
                "(void)vua_session_show(vua_global_session(NULL), vus_string_cstr(%s), NULL)", p);
            free(p);
            return r;
        }
        return strdup("(void)vua_session_show(vua_global_session(NULL), \"\", NULL)");
    }
    if (strcmp(call->func_name, "界面_返回") == 0) {
        g_uses_vua = 1;
        return strdup("(void)vua_session_back(vua_global_session(NULL))");
    }
    if (strcmp(call->func_name, "界面_返回至") == 0) {
        g_uses_vua = 1;
        if (call->args && call->args->count >= 1) {
            char *n = gen_expr(buf, call->args->items[0]);
            char *r = (char *)malloc(strlen(n) + 128);
            snprintf(r, strlen(n) + 128,
                "(void)vua_session_back_to(vua_global_session(NULL), vus_string_cstr(%s))", n);
            free(n);
            return r;
        }
        return strdup("(void)vua_session_back_to(vua_global_session(NULL), \"\")");
    }
    if (strcmp(call->func_name, "界面_显示_JSON") == 0) {
        g_uses_vua = 1;
        /* 动态渲染树：把 .vua 格式 JSON 字符串直接上屏（vua_show_json 压栈并通知重绘） */
        if (call->args && call->args->count >= 1) {
            char *js = gen_expr(buf, call->args->items[0]);
            size_t sz = strlen(js) + 96;
            char *r = (char *)malloc(sz);
            snprintf(r, sz, "(void)vua_show_json(vua_global_session(NULL), vus_string_cstr(%s), NULL)", js);
            free(js);
            return r;
        }
        return strdup("(void)-1; /* 界面_显示_JSON: 参数不足 */");
    }
    if (strcmp(call->func_name, "界面_绑定") == 0) {
        g_uses_vua = 1;
        /* 参数：args[0]=事件名字符串，args[1]=处理函数标识符 */
        if (call->args && call->args->count >= 2 &&
            call->args->items[1]->type == VUS_AST_IDENTIFIER) {
            char *ev = gen_expr(buf, call->args->items[0]);
            VusAstIdentifier *handler = (VusAstIdentifier *)call->args->items[1];
            char hs[256];
            gen_sanitize_name(handler->name, hs, sizeof(hs));
            int id = g_vua_bind_count++;
            /* 静态包装：调用处生成 VusClosure 并登记到当前屏事件表 */
            if (!g_vua_premain) g_vua_premain = gen_buf_new();
            if (!g_vua_fwd) g_vua_fwd = gen_buf_new();
            /* 前向声明：包装函数可能在用户 handler 函数体内被引用，须先声明 */
            gen_emit_linef(g_vua_fwd, "static void _vua_ck_%d(void*, void*);", id);
            /* 闭包参数数组：槽0为返回值，槽1..N 为事件函数参数。
             * 旧实现固定 _va[1]（仅返回值槽），事件函数读 _va[1..N] 越界访问栈垃圾，
             * vus_ref(垃圾指针) 直接段错误（真机日志 vus_dict_set+316 / fault addr 0x8）。
             * 事件触发时 _args 为 VusDict*（键=值 或 控件收集变量），按参数名匹配填充；
             * 若 dict 恰好一个键值对且参数个数≥2（如"星级=1"字面量），参数1=键、参数2=值。 */
            size_t nparams = 0;
            VusAstFunctionDef *hfunc = gen_vua_find_func(handler->name);
            if (hfunc && hfunc->params) nparams = hfunc->params->count;
            gen_emit_linef(g_vua_premain, "static void _vua_ck_%d(void* _env, void* _args){", id);
            gen_emit_linef(g_vua_premain, "    (void)_env;");
            gen_emit_linef(g_vua_premain, "    VusString* _va[%zu]={NULL};", nparams + 1);
            if (nparams > 0) {
                gen_emit_line(g_vua_premain, "    if (_args) {");
                gen_emit_line(g_vua_premain, "        VusDict* _vd = (VusDict*)_args;");
                /* 1) 参数名精确匹配：事件 dict 键等于函数参数名时按值填充 */
                if (hfunc && hfunc->params) {
                    for (size_t i = 0; i < nparams && i < hfunc->params->count; i++) {
                        VusAstNode *pn = hfunc->params->items[i];
                        const char *pname = NULL;
                        if (pn->type == VUS_AST_PARAM) pname = ((VusAstParam *)pn)->name;
                        else if (pn->type == VUS_AST_PARAM_DEFAULT) pname = ((VusAstParamDefault *)pn)->name;
                        if (!pname || !pname[0]) continue;
                        char pb[512];
                        snprintf(pb, sizeof(pb),
                            "        { VusString* _pk_%d = vus_string_new(\"%s\"); void* _pv_%d = vus_dict_get(_vd, _pk_%d); if (_pv_%d && !_va[%zu]) { _va[%zu] = (VusString*)_pv_%d; vus_ref(_va[%zu]); } vus_unref(_pk_%d); }",
                            (int)i, pname, (int)i, (int)i, (int)i, i + 1, i + 1, (int)i, i + 1, (int)i);
                        gen_emit(g_vua_premain, pb);
                        gen_emit(g_vua_premain, "\n");
                    }
                }
                /* 2) 键=值 字面量回退：dict 恰一个键值对且参数≥2 时，参数1=键、参数2=值
                 *   （用于星级评分等"控件变量=值"回调：界面_设置(变量, 值) 存为 状态[键]=值）。 */
                if (nparams >= 2) {
                    gen_emit_line(g_vua_premain,
                        "        { VusList* _kl = vus_dict_keys(_vd); if (_kl && _kl->len == 1) {");
                    gen_emit_line(g_vua_premain,
                        "            VusString* _kk = _kl->items[0]; if (_kk) {");
                    gen_emit_line(g_vua_premain,
                        "                void* _vv = vus_dict_get(_vd, _kk);");
                    gen_emit_line(g_vua_premain,
                        "                if (_vv && !_va[1]) _va[1] = vus_string_new(vus_string_cstr(_kk));");
                    gen_emit_line(g_vua_premain,
                        "                if (_vv && !_va[2]) _va[2] = (VusString*)_vv, vus_ref(_va[2]);");
                    gen_emit_line(g_vua_premain, "            }");
                    gen_emit_line(g_vua_premain, "            vus_unref((void*)_kl); }");
                    gen_emit_line(g_vua_premain, "        }");
                    gen_emit_line(g_vua_premain, "    }");
                } else {
                    gen_emit_line(g_vua_premain, "    }");
                }
            }
            /* P6: 事件参数匹配告警——回调变量/字面量里没找到形参名时打印 stderr（进
             * logcat），避免"界面无反应"/参数为 NULL 的静默失败。仅在事件携带参数
             * （_args 非空）时检查，纯点击无参事件不告警。 */
            if (hfunc && hfunc->params && nparams > 0) {
                for (size_t i = 0; i < nparams && i < hfunc->params->count; i++) {
                    VusAstNode *pn = hfunc->params->items[i];
                    const char *pname = NULL;
                    if (pn->type == VUS_AST_PARAM) pname = ((VusAstParam *)pn)->name;
                    else if (pn->type == VUS_AST_PARAM_DEFAULT) pname = ((VusAstParamDefault *)pn)->name;
                    if (!pname || !pname[0]) continue;
                    char wbuf[640];
                    snprintf(wbuf, sizeof(wbuf),
                        "    if (_args && !_va[%zu]) fprintf(stderr, \"[vua] 事件参数未匹配: 形参 '%s' 未从回调变量/字面量获取到值（检查 .vua 事件键与函数形参名）\\n\");",
                        i + 1, pname);
                    gen_emit_line(g_vua_premain, wbuf);
                }
            }
            gen_emit_linef(g_vua_premain, "    vus_%s(_va);", hs);
            gen_emit_linef(g_vua_premain, "}");
            size_t sz = strlen(ev) + 256;
            char *r = (char *)malloc(sz);
            snprintf(r, sz,
                "({ VusClosure* _vc_%d = vus_closure_new(_vua_ck_%d, NULL); vua_on(vua_session_current(vua_global_session(NULL)), vus_string_cstr(%s), _vc_%d); _vc_%d; })",
                id, id, ev, id, id);
            free(ev);
            return r;
        }
        return strdup("(void)0; /* 界面_绑定: 参数不足 */");
    }
    if (strcmp(call->func_name, "界面_设置") == 0) {
        g_uses_vua = 1;
        /* 界面_设置(变量名, 值)：写当前屏状态，供渲染树变量回填（逻辑页核心）。
         * 变量名为字符串字面量时走 cstr 驻留路径（vua_state_set_cstr），
         * 避免高频循环里每次重建 VusString 键；否则按原样透传。 */
        if (call->args && call->args->count >= 2) {
            char *val = gen_expr(buf, call->args->items[1]);
            char *r;
            if (call->args->items[0]->type == VUS_AST_STRING_LITERAL) {
                char escaped[4096];
                gen_string_escape(((VusAstString *)call->args->items[0])->value, escaped, sizeof(escaped));
                size_t sz = strlen(escaped) + strlen(val) + 192;
                r = (char *)malloc(sz);
                snprintf(r, sz,
                    "(void)vua_state_set_cstr(vua_session_current(vua_global_session(NULL)), \"%s\", %s)",
                    escaped, val);
            } else {
                char *name = gen_expr(buf, call->args->items[0]);
                size_t sz = strlen(name) + strlen(val) + 192;
                r = (char *)malloc(sz);
                snprintf(r, sz,
                    "(void)vua_state_set(vua_session_current(vua_global_session(NULL)), %s, %s)",
                    name, val);
                free(name);
            }
            free(val);
            return r;
        }
        return strdup("(void)0; /* 界面_设置: 参数不足 */");
    }
    if (strcmp(call->func_name, "界面_取") == 0) {
        g_uses_vua = 1;
        /* 界面_取(变量名)：读当前屏状态，返回新 VusString*（缺失给空串，可安全 unref）。
         * 字面量变量名走 cstr 驻留路径。 */
        if (call->args && call->args->count >= 1) {
            char *r;
            if (call->args->items[0]->type == VUS_AST_STRING_LITERAL) {
                char escaped[4096];
                gen_string_escape(((VusAstString *)call->args->items[0])->value, escaped, sizeof(escaped));
                size_t sz = strlen(escaped) + 192;
                r = (char *)malloc(sz);
                snprintf(r, sz,
                    "vua_state_get_or_empty_cstr(vua_session_current(vua_global_session(NULL)), \"%s\")",
                    escaped);
            } else {
                char *name = gen_expr(buf, call->args->items[0]);
                size_t sz = strlen(name) + 192;
                r = (char *)malloc(sz);
                snprintf(r, sz,
                    "vua_state_get_or_empty(vua_session_current(vua_global_session(NULL)), %s)",
                    name);
                free(name);
            }
            return r;
        }
        return strdup("vua_state_get_or_empty(vua_session_current(vua_global_session(NULL)), vus_string_new(\"\"))");
    }
    if (strcmp(call->func_name, "界面_触发") == 0) {
        g_uses_vua = 1;
        /* 界面_触发(事件名[, 变量JSON])：程序化派发事件（绕过 Java 触摸回传）。
         * 变量 JSON 经 vua_dict_from_json 转成 VusDict 交给处理闭包，用后 unref；
         * 未传变量 JSON 时不携带参数（vars=NULL）。 */
        if (call->args && call->args->count >= 1) {
            char *ev = gen_expr(buf, call->args->items[0]);
            if (call->args->count >= 2) {
                char *vars = gen_expr(buf, call->args->items[1]);
                int id = g_vua_op_count++;
                size_t sz = strlen(ev) + strlen(vars) + 256;
                char *r = (char *)malloc(sz);
                snprintf(r, sz,
                    "({ VusDict* _vdt_%d = vua_dict_from_json(vus_string_cstr(%s), NULL); if (_vdt_%d) { vua_trigger_event(vua_session_current(vua_global_session(NULL)), vus_string_cstr(%s), _vdt_%d); vus_unref((void*)_vdt_%d); } })",
                    id, vars, id, ev, id, id);
                free(ev);
                free(vars);
                return r;
            }
            size_t sz = strlen(ev) + 160;
            char *r = (char *)malloc(sz);
            snprintf(r, sz,
                "(void)vua_trigger_event(vua_session_current(vua_global_session(NULL)), vus_string_cstr(%s), NULL)", ev);
            free(ev);
            return r;
        }
        return strdup("(void)0; /* 界面_触发: 参数不足 */");
    }
    if (strcmp(call->func_name, "界面_按ID触发") == 0) {
        g_uses_vua = 1;
        /* 界面_按ID触发(控件id[, 变量JSON])：按渲染树节点 id 解析事件后派发。 */
        if (call->args && call->args->count >= 1) {
            char *nid = gen_expr(buf, call->args->items[0]);
            if (call->args->count >= 2) {
                char *vars = gen_expr(buf, call->args->items[1]);
                int id = g_vua_op_count++;
                size_t sz = strlen(nid) + strlen(vars) + 256;
                char *r = (char *)malloc(sz);
                snprintf(r, sz,
                    "({ VusDict* _vdd_%d = vua_dict_from_json(vus_string_cstr(%s), NULL); if (_vdd_%d) { vua_trigger_by_id(vua_session_current(vua_global_session(NULL)), vus_string_cstr(%s), _vdd_%d); vus_unref((void*)_vdd_%d); } })",
                    id, vars, id, nid, id, id);
                free(nid);
                free(vars);
                return r;
            }
            size_t sz = strlen(nid) + 160;
            char *r = (char *)malloc(sz);
            snprintf(r, sz,
                "(void)vua_trigger_by_id(vua_session_current(vua_global_session(NULL)), vus_string_cstr(%s), NULL)", nid);
            free(nid);
            return r;
        }
        return strdup("(void)0; /* 界面_按ID触发: 参数不足 */");
    }
    if (strcmp(call->func_name, "界面_解绑") == 0) {
        g_uses_vua = 1;
        /* 界面_解绑(事件名)：从会话级事件表移除已登记的处理函数（未登记则无操作）。 */
        if (call->args && call->args->count >= 1) {
            char *ev = gen_expr(buf, call->args->items[0]);
            size_t sz = strlen(ev) + 160;
            char *r = (char *)malloc(sz);
            snprintf(r, sz,
                "(void)vua_off(vua_session_current(vua_global_session(NULL)), vus_string_cstr(%s))", ev);
            free(ev);
            return r;
        }
        return strdup("(void)0; /* 界面_解绑: 参数不足 */");
    }
    if (strcmp(call->func_name, "界面_全局置") == 0) {
        g_uses_vua = 1;
        /* 界面_全局置(键, 值)：写会话级全局变量（跨屏共享）。
         * 字面量键走字符串驻留路径，避免高频重建 VusString。 */
        if (call->args && call->args->count >= 2) {
            char *val = gen_expr(buf, call->args->items[1]);
            char *r;
            if (call->args->items[0]->type == VUS_AST_STRING_LITERAL) {
                char escaped[4096];
                gen_string_escape(((VusAstString *)call->args->items[0])->value, escaped, sizeof(escaped));
                int id = g_vua_op_count++;
                size_t sz = strlen(escaped) + strlen(val) + 192;
                r = (char *)malloc(sz);
                snprintf(r, sz,
                    "({ VusString* _vkg_%d = vus_string_intern(\"%s\"); vua_session_global_set(vua_global_session(NULL), _vkg_%d, %s); vus_unref(_vkg_%d); })",
                    id, escaped, id, val, id);
            } else {
                char *key = gen_expr(buf, call->args->items[0]);
                size_t sz = strlen(key) + strlen(val) + 192;
                r = (char *)malloc(sz);
                snprintf(r, sz,
                    "(void)vua_session_global_set(vua_global_session(NULL), %s, %s)", key, val);
                free(key);
            }
            free(val);
            return r;
        }
        return strdup("(void)0; /* 界面_全局置: 参数不足 */");
    }
    if (strcmp(call->func_name, "界面_全局取") == 0) {
        g_uses_vua = 1;
        /* 界面_全局取(键)：读会话级全局变量；缺失返回空串。
         * 返回新 VusString（拷贝），可安全 unref。 */
        if (call->args && call->args->count >= 1) {
            int id = g_vua_op_count++;
            char *r;
            if (call->args->items[0]->type == VUS_AST_STRING_LITERAL) {
                char escaped[4096];
                gen_string_escape(((VusAstString *)call->args->items[0])->value, escaped, sizeof(escaped));
                size_t sz = strlen(escaped) + 256;
                r = (char *)malloc(sz);
                snprintf(r, sz,
                    "({ VusString* _vkg_%d = vus_string_intern(\"%s\"); void* _vgg_%d = vua_session_global_get(vua_global_session(NULL), _vkg_%d); VusString* _vgv_%d = _vgg_%d ? vus_string_new(vus_string_cstr((VusString*)_vgg_%d)) : vus_string_new(\"\"); vus_unref(_vkg_%d); _vgv_%d; })",
                    id, escaped, id, id, id, id, id, id, id);
            } else {
                char *key = gen_expr(buf, call->args->items[0]);
                size_t sz = strlen(key) + 256;
                r = (char *)malloc(sz);
                snprintf(r, sz,
                    "({ void* _vgg_%d = vua_session_global_get(vua_global_session(NULL), %s); VusString* _vgv_%d = _vgg_%d ? vus_string_new(vus_string_cstr((VusString*)_vgg_%d)) : vus_string_new(\"\"); _vgv_%d; })",
                    id, key, id, id, id, id);
                free(key);
            }
            return r;
        }
        return strdup("({ VusString* _vgv = vus_string_new(\"\"); _vgv; })");
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
            /* 包装为 VusString*，保证在赋值/打印/算术等所有语境下
             * 类型一致；算术场合 vus_to_int 会再次解出数值，语义不变。 */
            snprintf(result, sz, "vus_to_string(vus_to_int(%s, &_err))", arg);
            free(arg);
            return result;
        }
        return strdup("vus_to_string(0)");
    }
    if (strcmp(call->func_name, "转文本") == 0) {
        if (call->args && call->args->count > 0) {
            char *arg = gen_expr(buf, call->args->items[0]);
            /* VUS 标量统一为 VusString*：直接转发入参表达式。
             * VusString 语义不可变（运行时无原地修改 API），共享实例与副本等价，
             * 省去 vus_string_new(vus_string_cstr(arg)) 的整串复制。
             * （对象/容器请用 对象文本 显式转换，与旧行为一致。） */
            return arg;
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

    /* ============= 分级日志内置函数（EasyLogger） ============= */
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
        return strdup("vus_log_set_level(vus_string_new(\"信息\"))");
    }

    /* ============= GuiLite 图形内置函数（vus_gui_*） ============= */
    /* VUS 参数均为 VusString*：整数参数用 vus_to_int 转 native int，
       字符串参数用 vus_string_cstr 取 C 字符串，颜色为 0xRRGGBB 整数。 */
    if (strcmp(call->func_name, "图形_初始化") == 0) {
        if (call->args && call->args->count >= 3) {
            char *w = gen_expr(buf, call->args->items[0]);
            char *h = gen_expr(buf, call->args->items[1]);
            char *t = gen_expr(buf, call->args->items[2]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_init((int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), vus_string_cstr(%s))",
                w, h, t);
            free(w); free(h); free(t);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    /* ============= VUS XYZ 体感音游内建（rt/vus_xyz.c） ============= */
    if (strcmp(call->func_name, "传感器_读") == 0) {
        if (call->args && call->args->count >= 1) {
            char *a = gen_expr(buf, call->args->items[0]);
            char result[512];
            snprintf(result, sizeof(result), "vus_sensor_read(vus_string_cstr(%s))", a);
            free(a);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "时钟") == 0) {
        return strdup("vus_clock_ms()");
    }
    if (strcmp(call->func_name, "音频_打开") == 0) {
        if (call->args && call->args->count >= 1) {
            char *a = gen_expr(buf, call->args->items[0]);
            char result[512];
            snprintf(result, sizeof(result), "vus_audio_open(vus_string_cstr(%s))", a);
            free(a);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "音频_播放") == 0) {
        return strdup("vus_audio_play()");
    }
    if (strcmp(call->func_name, "音频_暂停") == 0) {
        return strdup("vus_audio_pause()");
    }
    if (strcmp(call->func_name, "音频_续") == 0) {
        return strdup("vus_audio_resume()");
    }
    if (strcmp(call->func_name, "音频_跳转") == 0) {
        if (call->args && call->args->count >= 1) {
            char *a = gen_expr(buf, call->args->items[0]);
            char result[512];
            snprintf(result, sizeof(result), "vus_audio_seek((int)vus_to_int(%s, &_err))", a);
            free(a);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "音频_进度") == 0) {
        return strdup("vus_audio_position()");
    }
    if (strcmp(call->func_name, "音频_时长") == 0) {
        return strdup("vus_audio_duration()");
    }
    if (strcmp(call->func_name, "图形_画点") == 0) {
        if (call->args && call->args->count >= 3) {
            char *x = gen_expr(buf, call->args->items[0]);
            char *y = gen_expr(buf, call->args->items[1]);
            char *c = gen_expr(buf, call->args->items[2]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_draw_pixel((int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (unsigned int)vus_to_int(%s, &_err))",
                x, y, c);
            free(x); free(y); free(c);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_画线") == 0) {
        if (call->args && call->args->count >= 5) {
            char *x1 = gen_expr(buf, call->args->items[0]);
            char *y1 = gen_expr(buf, call->args->items[1]);
            char *x2 = gen_expr(buf, call->args->items[2]);
            char *y2 = gen_expr(buf, call->args->items[3]);
            char *c  = gen_expr(buf, call->args->items[4]);
            /* 后 3 参可省略：线宽默认 1、虚线默认 0、箭头默认 0。
             * 缺省值用与 gen_expr 同构的 VusString* 表达式（vus_to_string(N)），
             * 避免 (int) 强转指针产生垃圾值。 */
            char *wd  = (call->args->count > 5) ? gen_expr(buf, call->args->items[5]) : strdup("vus_to_string(1)");
            char *ds  = (call->args->count > 6) ? gen_expr(buf, call->args->items[6]) : strdup("vus_to_string(0)");
            char *ar  = (call->args->count > 7) ? gen_expr(buf, call->args->items[7]) : strdup("vus_to_string(0)");
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_draw_line_ex((int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (unsigned int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err))",
                x1, y1, x2, y2, c, wd, ds, ar);
            free(x1); free(y1); free(x2); free(y2); free(c);
            free(wd); free(ds); free(ar);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_矩形") == 0) {
        if (call->args && call->args->count >= 5) {
            char *x = gen_expr(buf, call->args->items[0]);
            char *y = gen_expr(buf, call->args->items[1]);
            char *wd = gen_expr(buf, call->args->items[2]);
            char *ht = gen_expr(buf, call->args->items[3]);
            char *c  = gen_expr(buf, call->args->items[4]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_draw_rect((int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (unsigned int)vus_to_int(%s, &_err))",
                x, y, wd, ht, c);
            free(x); free(y); free(wd); free(ht); free(c);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_填充") == 0) {
        if (call->args && call->args->count >= 5) {
            char *x = gen_expr(buf, call->args->items[0]);
            char *y = gen_expr(buf, call->args->items[1]);
            char *wd = gen_expr(buf, call->args->items[2]);
            char *ht = gen_expr(buf, call->args->items[3]);
            char *c  = gen_expr(buf, call->args->items[4]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_fill_rect((int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (unsigned int)vus_to_int(%s, &_err))",
                x, y, wd, ht, c);
            free(x); free(y); free(wd); free(ht); free(c);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_文字") == 0) {
        if (call->args && call->args->count >= 4) {
            char *x = gen_expr(buf, call->args->items[0]);
            char *y = gen_expr(buf, call->args->items[1]);
            char *txt = gen_expr(buf, call->args->items[2]);
            char *c  = gen_expr(buf, call->args->items[3]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_draw_text((int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), vus_string_cstr(%s), (unsigned int)vus_to_int(%s, &_err))",
                x, y, txt, c);
            free(x); free(y); free(txt); free(c);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_刷新") == 0) {
        g_uses_gui = 1;
        return strdup("vus_gui_redraw()");
    }
    if (strcmp(call->func_name, "图形_保持") == 0) {
        g_uses_gui = 1;
        return strdup("vus_gui_run()");
    }
    /* 图形_MD(x, y, 宽度, 文本)：Markdown 最小集渲染，按宽度折行，返回占用行数整数串。 */
    if (strcmp(call->func_name, "图形_MD") == 0) {
        if (call->args && call->args->count >= 4) {
            char *x = gen_expr(buf, call->args->items[0]);
            char *y = gen_expr(buf, call->args->items[1]);
            char *w = gen_expr(buf, call->args->items[2]);
            char *txt = gen_expr(buf, call->args->items[3]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_md((int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), vus_string_cstr(%s))",
                x, y, w, txt);
            free(x); free(y); free(w); free(txt);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    /* 图形_滚动容器(name, x, y, w, h, content_h)：登记可视区 + 滚动范围。 */
    if (strcmp(call->func_name, "图形_滚动容器") == 0) {
        if (call->args && call->args->count >= 6) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char *x  = gen_expr(buf, call->args->items[1]);
            char *y  = gen_expr(buf, call->args->items[2]);
            char *w  = gen_expr(buf, call->args->items[3]);
            char *h  = gen_expr(buf, call->args->items[4]);
            char *ch = gen_expr(buf, call->args->items[5]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_scroll_begin(vus_string_cstr(%s), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err))",
                nm, x, y, w, h, ch);
            free(nm); free(x); free(y); free(w); free(h); free(ch);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    /* 图形_滚动容器滚(name, dy)：令容器偏移增减 dy，返回新偏移整数串。 */
    if (strcmp(call->func_name, "图形_滚动容器滚") == 0) {
        if (call->args && call->args->count >= 2) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char *dy = gen_expr(buf, call->args->items[1]);
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_scroll_delta(vus_string_cstr(%s), (int)vus_to_int(%s, &_err))",
                nm, dy);
            free(nm); free(dy);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    /* 图形_滚动容器偏移(name)：返回当前偏移整数串。 */
    if (strcmp(call->func_name, "图形_滚动容器偏移") == 0) {
        if (call->args && call->args->count >= 1) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_scroll_offset(vus_string_cstr(%s))", nm);
            free(nm);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    /* 图形_背景图(x, y, 宽, 高, PNG路径)：解码 PNG 拉伸铺作控件/区域背景。 */
    if (strcmp(call->func_name, "图形_背景图") == 0) {
        if (call->args && call->args->count >= 5) {
            char *x = gen_expr(buf, call->args->items[0]);
            char *y = gen_expr(buf, call->args->items[1]);
            char *w = gen_expr(buf, call->args->items[2]);
            char *h = gen_expr(buf, call->args->items[3]);
            char *path = gen_expr(buf, call->args->items[4]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_draw_png((int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), vus_string_cstr(%s))",
                x, y, w, h, path);
            free(x); free(y); free(w); free(h); free(path);
            g_uses_gui = 1;
            g_uses_png = 1;
            return strdup(result);
        }
    }

    /* 图形_字体_加载(路径, 字号)：加载外部 TTF/OTF 到全局活动字体。 */
    if (strcmp(call->func_name, "图形_字体_加载") == 0) {
        if (call->args && call->args->count >= 2) {
            char *path = gen_expr(buf, call->args->items[0]);
            char *sz   = gen_expr(buf, call->args->items[1]);
            char result[2048];
            snprintf(result, sizeof(result),
                "vus_gui_font(vus_string_cstr(%s), (int)vus_to_int(%s, &_err))",
                path, sz);
            free(path); free(sz);
            g_uses_gui = 1;
            g_uses_freetype = 1;
            return strdup(result);
        }
    }
    /* 图形_字体已加载()：查询外部字体是否已加载。 */
    if (strcmp(call->func_name, "图形_字体已加载") == 0) {
        g_uses_gui = 1;
        g_uses_freetype = 1;
        return strdup("vus_gui_font_loaded()");
    }

    /* ===== 阶段E：多页导航（页面栈） =====
     * 页面概念：脚本对每页定义约定式函数 `页_<名>()`，由桥接层 dlsym 反查并
     * 调用。配合语言 导入 可接入外部 .vus 页面。 */
    if (strcmp(call->func_name, "图形_页面_打开") == 0) {
        if (call->args && call->args->count >= 1) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char result[2048];
            snprintf(result, sizeof(result), "vus_gui_page_open(vus_string_cstr(%s))", nm);
            free(nm);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_页面_返回") == 0) {
        g_uses_gui = 1;
        return strdup("vus_gui_page_back()");
    }
    if (strcmp(call->func_name, "图形_页面_当前") == 0) {
        g_uses_gui = 1;
        return strdup("vus_gui_page_current()");
    }
    if (strcmp(call->func_name, "图形_页面_绘制") == 0) {
        g_uses_gui = 1;
        return strdup("vus_gui_page_draw()");
    }

    /* ===== 阶段2：控件与轮询交互 =====
     * 图形_按钮(名,x,y,宽,高,文本)：创建并绘制一个按钮，登记命中矩形。
     * 图形_取事件()：非阻塞处理 X 事件队列，更新点击坐标。
     * 图形_按钮点击(名)：命中检测，返回 "true"/"false"，可直接作 如果 条件。 */
    if (strcmp(call->func_name, "图形_按钮") == 0) {
        if (call->args && call->args->count >= 6) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char *x  = gen_expr(buf, call->args->items[1]);
            char *y  = gen_expr(buf, call->args->items[2]);
            char *wd = gen_expr(buf, call->args->items[3]);
            char *ht = gen_expr(buf, call->args->items[4]);
            char *tx = gen_expr(buf, call->args->items[5]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_button(vus_string_cstr(%s), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), vus_string_cstr(%s))",
                nm, x, y, wd, ht, tx);
            free(nm); free(x); free(y); free(wd); free(ht); free(tx);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_取事件") == 0) {
        g_uses_gui = 1;
        return strdup("vus_gui_poll()");
    }

    /* ===== 阶段4：X11 多输入 =====
     * 键盘：图形_按键() 图形_按键码()，鼠标：图形_鼠标x/y() 图形_鼠标位置()
     * 滚轮：图形_滚轮()，按键：图形_键按下(n)，悬停：图形_悬停(名) */
    if (strcmp(call->func_name, "图形_按键") == 0) {
        g_uses_gui = 1;
        return strdup("vus_gui_last_key()");
    }
    if (strcmp(call->func_name, "图形_按键码") == 0) {
        g_uses_gui = 1;
        return strdup("vus_gui_last_keycode()");
    }
    if (strcmp(call->func_name, "图形_鼠标位置") == 0) {
        g_uses_gui = 1;
        return strdup("vus_gui_mouse_pos()");
    }
    if (strcmp(call->func_name, "图形_鼠标x") == 0) {
        g_uses_gui = 1;
        return strdup("vus_gui_mouse_x()");
    }
    if (strcmp(call->func_name, "图形_鼠标y") == 0) {
        g_uses_gui = 1;
        return strdup("vus_gui_mouse_y()");
    }
    if (strcmp(call->func_name, "图形_滚轮") == 0) {
        g_uses_gui = 1;
        return strdup("vus_gui_wheel()");
    }
    if (strcmp(call->func_name, "图形_键按下") == 0) {
        if (call->args && call->args->count >= 1) {
            char *b = gen_expr(buf, call->args->items[0]);
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_button_pressed((int)vus_to_int(%s, &_err))", b);
            free(b);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_悬停") == 0) {
        if (call->args && call->args->count >= 1) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_hover(vus_string_cstr(%s))", nm);
            free(nm);
            g_uses_gui = 1;
            return strdup(result);
        }
    }

    /* ===== 阶段B：样式/主题模板 =====
     * 图形_主题(背景, 边框, 高亮, 正文, 文字)：设置全局主题色。 */
    if (strcmp(call->func_name, "图形_主题") == 0) {
        int n = call->args ? call->args->count : 0;
        char* a[5] = { 0 };
        for (int i = 0; i < 5 && i < n; i++)
            a[i] = gen_expr(buf, call->args->items[i]);
        /* 整数颜色参数是 VusString*（如 vus_to_string(0x…)），须用 vus_to_int
           转成 native int 传给 vus_gui_set_theme(int,…)；缺省 -1 保持通道不变。 */
        char t[5][128] = { {0} };
        for (int i = 0; i < 5; i++)
        {
            if (a[i])
                snprintf(t[i], sizeof(t[i]), "(int)vus_to_int(%s, &_err)", a[i]);
            else
                snprintf(t[i], sizeof(t[i]), "-1");
        }
        char result[4096];
        snprintf(result, sizeof(result),
            "vus_gui_set_theme(%s, %s, %s, %s, %s)",
            t[0], t[1], t[2], t[3], t[4]);
        for (int i = 0; i < 5; i++) if (a[i]) free(a[i]);
        g_uses_gui = 1;
        return strdup(result);
    }

    /* ===== 阶段C：控件组合模板 =====
     * 图形_卡片(名,x,y,宽,高,标题) / 图形_面板(...) /
     * 图形_表单行(名,标签,x,y,宽,文本) / 图形_行点击(名) / 图形_圆环(名,x,y,半径,比例,颜色) */
    if (strcmp(call->func_name, "图形_卡片") == 0 ||
        strcmp(call->func_name, "图形_面板") == 0) {
        if (call->args && call->args->count >= 6) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char *x  = gen_expr(buf, call->args->items[1]);
            char *y  = gen_expr(buf, call->args->items[2]);
            char *wd = gen_expr(buf, call->args->items[3]);
            char *ht = gen_expr(buf, call->args->items[4]);
            char *ti = gen_expr(buf, call->args->items[5]);
            char result[4096];
            const char* fn = (strcmp(call->func_name, "图形_卡片") == 0) ? "vus_gui_card" : "vus_gui_panel";
            snprintf(result, sizeof(result),
                "%s(vus_string_cstr(%s), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), vus_string_cstr(%s))",
                fn, nm, x, y, wd, ht, ti);
            free(nm); free(x); free(y); free(wd); free(ht); free(ti);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_表单行") == 0) {
        if (call->args && call->args->count >= 6) {
            char *nm  = gen_expr(buf, call->args->items[0]);
            char *lb  = gen_expr(buf, call->args->items[1]);
            char *x   = gen_expr(buf, call->args->items[2]);
            char *y   = gen_expr(buf, call->args->items[3]);
            char *wd  = gen_expr(buf, call->args->items[4]);
            char *tx  = gen_expr(buf, call->args->items[5]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_form_row(vus_string_cstr(%s), vus_string_cstr(%s), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), vus_string_cstr(%s))",
                nm, lb, x, y, wd, tx);
            free(nm); free(lb); free(x); free(y); free(wd); free(tx);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_行点击") == 0) {
        if (call->args && call->args->count >= 1) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_row_clicked(vus_string_cstr(%s))", nm);
            free(nm);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_圆环") == 0) {
        if (call->args && call->args->count >= 5) {
            char *nm  = gen_expr(buf, call->args->items[0]);
            char *x   = gen_expr(buf, call->args->items[1]);
            char *y   = gen_expr(buf, call->args->items[2]);
            char *r   = gen_expr(buf, call->args->items[3]);
            char *pct = gen_expr(buf, call->args->items[4]);
            char *col = (call->args->count >= 6) ? gen_expr(buf, call->args->items[5]) : NULL;
            char result[4096];
            if (col) {
                snprintf(result, sizeof(result),
                    "vus_gui_ring(vus_string_cstr(%s), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err))",
                    nm, x, y, r, pct, col);
            } else {
                snprintf(result, sizeof(result),
                    "vus_gui_ring(vus_string_cstr(%s), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), -1)",
                    nm, x, y, r, pct);
            }
            free(nm); free(x); free(y); free(r); free(pct); if (col) free(col);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_按钮点击") == 0 ||
        strcmp(call->func_name, "按钮被点击") == 0) {
        /* 图形_按钮点击(名) / 按钮被点击(名)：自然语义别名，排在控件判定后面。
         * 返回 "true"/"false"，可直接作 如果 条件。 */
        if (call->args && call->args->count >= 1) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_button_clicked(vus_string_cstr(%s))", nm);
            free(nm);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_模拟点击") == 0) {
        /* 图形_模拟点击(x,y)：注入一次点击坐标，headless 测试命中路径用。 */
        if (call->args && call->args->count >= 2) {
            char *x = gen_expr(buf, call->args->items[0]);
            char *y = gen_expr(buf, call->args->items[1]);
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_sim_click((int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err))",
                x, y);
            free(x); free(y);
            g_uses_gui = 1;
            return strdup(result);
        }
    }

    /* ===== 阶段3：控件库内建函数 =====
     * 图形_标签/图形_文本框/图形_复选框/图形_进度/图形_列表/图形_列表行/
     * 图形_列表选中/图形_列表行点击/图形_画布/图形_画布命中/图形_画布点 */
    if (strcmp(call->func_name, "图形_标签") == 0) {
        if (call->args && call->args->count >= 5) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char *x  = gen_expr(buf, call->args->items[1]);
            char *y  = gen_expr(buf, call->args->items[2]);
            char *tx = gen_expr(buf, call->args->items[3]);
            char *c  = gen_expr(buf, call->args->items[4]);
            char result[2048];
            snprintf(result, sizeof(result),
                "vus_gui_label(vus_string_cstr(%s), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), vus_string_cstr(%s), (unsigned int)vus_to_int(%s, &_err))",
                nm, x, y, tx, c);
            free(nm); free(x); free(y); free(tx); free(c);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_文本框") == 0) {
        if (call->args && call->args->count >= 6) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char *x  = gen_expr(buf, call->args->items[1]);
            char *y  = gen_expr(buf, call->args->items[2]);
            char *wd = gen_expr(buf, call->args->items[3]);
            char *ht = gen_expr(buf, call->args->items[4]);
            char *tx = gen_expr(buf, call->args->items[5]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_textbox(vus_string_cstr(%s), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), vus_string_cstr(%s))",
                nm, x, y, wd, ht, tx);
            free(nm); free(x); free(y); free(wd); free(ht); free(tx);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_复选框") == 0) {
        if (call->args && call->args->count >= 5) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char *x  = gen_expr(buf, call->args->items[1]);
            char *y  = gen_expr(buf, call->args->items[2]);
            char *tx = gen_expr(buf, call->args->items[3]);
            char *ck = gen_expr(buf, call->args->items[4]);
            char result[2048];
            snprintf(result, sizeof(result),
                "vus_gui_checkbox(vus_string_cstr(%s), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), vus_string_cstr(%s), (int)vus_to_int(%s, &_err))",
                nm, x, y, tx, ck);
            free(nm); free(x); free(y); free(tx); free(ck);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_进度") == 0) {
        if (call->args && call->args->count >= 6) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char *x  = gen_expr(buf, call->args->items[1]);
            char *y  = gen_expr(buf, call->args->items[2]);
            char *wd = gen_expr(buf, call->args->items[3]);
            char *ht = gen_expr(buf, call->args->items[4]);
            char *vl = gen_expr(buf, call->args->items[5]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_progress(vus_string_cstr(%s), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err))",
                nm, x, y, wd, ht, vl);
            free(nm); free(x); free(y); free(wd); free(ht); free(vl);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_列表") == 0) {
        if (call->args && call->args->count >= 6) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char *x  = gen_expr(buf, call->args->items[1]);
            char *y  = gen_expr(buf, call->args->items[2]);
            char *wd = gen_expr(buf, call->args->items[3]);
            char *ht = gen_expr(buf, call->args->items[4]);
            char *rh = gen_expr(buf, call->args->items[5]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_list(vus_string_cstr(%s), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err))",
                nm, x, y, wd, ht, rh);
            free(nm); free(x); free(y); free(wd); free(ht); free(rh);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_列表行") == 0) {
        if (call->args && call->args->count >= 3) {
            char *nm   = gen_expr(buf, call->args->items[0]);
            char *line = gen_expr(buf, call->args->items[1]);
            char *tx   = gen_expr(buf, call->args->items[2]);
            char result[2048];
            snprintf(result, sizeof(result),
                "vus_gui_list_row(vus_string_cstr(%s), (int)vus_to_int(%s, &_err), vus_string_cstr(%s))",
                nm, line, tx);
            free(nm); free(line); free(tx);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_列表选中") == 0) {
        if (call->args && call->args->count >= 1) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_list_selected(vus_string_cstr(%s))", nm);
            free(nm);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_列表行点击") == 0) {
        if (call->args && call->args->count >= 2) {
            char *nm   = gen_expr(buf, call->args->items[0]);
            char *line = gen_expr(buf, call->args->items[1]);
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_list_row_clicked(vus_string_cstr(%s), (int)vus_to_int(%s, &_err))",
                nm, line);
            free(nm); free(line);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_画布") == 0) {
        if (call->args && call->args->count >= 5) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char *x  = gen_expr(buf, call->args->items[1]);
            char *y  = gen_expr(buf, call->args->items[2]);
            char *wd = gen_expr(buf, call->args->items[3]);
            char *ht = gen_expr(buf, call->args->items[4]);
            char result[2048];
            snprintf(result, sizeof(result),
                "vus_gui_canvas(vus_string_cstr(%s), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err))",
                nm, x, y, wd, ht);
            free(nm); free(x); free(y); free(wd); free(ht);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_画布命中") == 0) {
        if (call->args && call->args->count >= 1) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_canvas_hit(vus_string_cstr(%s))", nm);
            free(nm);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_画布点") == 0) {
        if (call->args && call->args->count >= 1) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_canvas_pos(vus_string_cstr(%s))", nm);
            free(nm);
            g_uses_gui = 1;
            return strdup(result);
        }
    }

    /* ===== 阶段G：图片 API + GIF 动画 ===== */
    if (strcmp(call->func_name, "图形_图片") == 0) {
        if (call->args && call->args->count >= 5) {
            char *x = gen_expr(buf, call->args->items[0]);
            char *y = gen_expr(buf, call->args->items[1]);
            char *wd = gen_expr(buf, call->args->items[2]);
            char *ht = gen_expr(buf, call->args->items[3]);
            char *path = gen_expr(buf, call->args->items[4]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_draw_image((int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), vus_string_cstr(%s))",
                x, y, wd, ht, path);
            free(x); free(y); free(wd); free(ht); free(path);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_动画_打开") == 0) {
        if (call->args && call->args->count >= 2) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char *path = gen_expr(buf, call->args->items[1]);
            char result[2048];
            snprintf(result, sizeof(result),
                "vus_gui_anim_open(vus_string_cstr(%s), vus_string_cstr(%s))", nm, path);
            free(nm); free(path);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_动画_下一步") == 0) {
        if (call->args && call->args->count >= 3) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char *x = gen_expr(buf, call->args->items[1]);
            char *y = gen_expr(buf, call->args->items[2]);
            char result[2048];
            snprintf(result, sizeof(result),
                "vus_gui_anim_next(vus_string_cstr(%s), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err))",
                nm, x, y);
            free(nm); free(x); free(y);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_动画_帧数") == 0) {
        if (call->args && call->args->count >= 1) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_anim_frames(vus_string_cstr(%s))", nm);
            free(nm);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_动画_关闭") == 0) {
        if (call->args && call->args->count >= 1) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_anim_close(vus_string_cstr(%s))", nm);
            free(nm);
            g_uses_gui = 1;
            return strdup(result);
        }
    }

    /* ===== 阶段H：高级交互控件（滑块/开关/微调/单选） ===== */
    if (strcmp(call->func_name, "图形_滑块") == 0) {
        if (call->args && call->args->count >= 5) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char *x = gen_expr(buf, call->args->items[1]);
            char *y = gen_expr(buf, call->args->items[2]);
            char *wd = gen_expr(buf, call->args->items[3]);
            char *vl = gen_expr(buf, call->args->items[4]);
            /* min/max 缺省 0/100 */
            char *mn = (call->args->count > 5) ? gen_expr(buf, call->args->items[5]) : strdup("vus_to_string(0)");
            char *mx = (call->args->count > 6) ? gen_expr(buf, call->args->items[6]) : strdup("vus_to_string(100)");
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_slider(vus_string_cstr(%s), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err))",
                nm, x, y, wd, vl, mn, mx);
            free(nm); free(x); free(y); free(wd); free(vl); free(mn); free(mx);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_滑块值") == 0) {
        if (call->args && call->args->count >= 1) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_slider_value(vus_string_cstr(%s))", nm);
            free(nm);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_开关") == 0) {
        if (call->args && call->args->count >= 3) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char *x = gen_expr(buf, call->args->items[1]);
            char *y = gen_expr(buf, call->args->items[2]);
            /* 初始状态缺省 0（关） */
            char *st = (call->args->count > 3) ? gen_expr(buf, call->args->items[3]) : strdup("vus_to_string(0)");
            char result[2048];
            snprintf(result, sizeof(result),
                "vus_gui_switch(vus_string_cstr(%s), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err))",
                nm, x, y, st);
            free(nm); free(x); free(y); free(st);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_开关值") == 0) {
        if (call->args && call->args->count >= 1) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_switch_value(vus_string_cstr(%s))", nm);
            free(nm);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_微调") == 0) {
        if (call->args && call->args->count >= 4) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char *x = gen_expr(buf, call->args->items[1]);
            char *y = gen_expr(buf, call->args->items[2]);
            char *vl = gen_expr(buf, call->args->items[3]);
            /* 步长缺省 1 */
            char *st = (call->args->count > 4) ? gen_expr(buf, call->args->items[4]) : strdup("vus_to_string(1)");
            char result[2048];
            snprintf(result, sizeof(result),
                "vus_gui_spin(vus_string_cstr(%s), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err))",
                nm, x, y, vl, st);
            free(nm); free(x); free(y); free(vl); free(st);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_微调值") == 0) {
        if (call->args && call->args->count >= 1) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_spin_value(vus_string_cstr(%s))", nm);
            free(nm);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_单选") == 0) {
        if (call->args && call->args->count >= 6) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char *x = gen_expr(buf, call->args->items[1]);
            char *y = gen_expr(buf, call->args->items[2]);
            char *ih = gen_expr(buf, call->args->items[3]);
            char *opts = gen_expr(buf, call->args->items[4]);
            char *sl = gen_expr(buf, call->args->items[5]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_radio(vus_string_cstr(%s), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), vus_string_cstr(%s), (int)vus_to_int(%s, &_err))",
                nm, x, y, ih, opts, sl);
            free(nm); free(x); free(y); free(ih); free(opts); free(sl);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_单选值") == 0) {
        if (call->args && call->args->count >= 1) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_radio_value(vus_string_cstr(%s))", nm);
            free(nm);
            g_uses_gui = 1;
            return strdup(result);
        }
    }

    /* ===== 阶段I：高级外观 ===== */
    if (strcmp(call->func_name, "图形_圆角矩形") == 0) {
        if (call->args && call->args->count >= 6) {
            char *x = gen_expr(buf, call->args->items[0]);
            char *y = gen_expr(buf, call->args->items[1]);
            char *wd = gen_expr(buf, call->args->items[2]);
            char *ht = gen_expr(buf, call->args->items[3]);
            char *rd = gen_expr(buf, call->args->items[4]);
            char *c = gen_expr(buf, call->args->items[5]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_round_rect((int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (unsigned int)vus_to_int(%s, &_err))",
                x, y, wd, ht, rd, c);
            free(x); free(y); free(wd); free(ht); free(rd); free(c);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_圆角填充") == 0) {
        if (call->args && call->args->count >= 6) {
            char *x = gen_expr(buf, call->args->items[0]);
            char *y = gen_expr(buf, call->args->items[1]);
            char *wd = gen_expr(buf, call->args->items[2]);
            char *ht = gen_expr(buf, call->args->items[3]);
            char *rd = gen_expr(buf, call->args->items[4]);
            char *c = gen_expr(buf, call->args->items[5]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_round_fill((int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (unsigned int)vus_to_int(%s, &_err))",
                x, y, wd, ht, rd, c);
            free(x); free(y); free(wd); free(ht); free(rd); free(c);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_画圆") == 0) {
        if (call->args && call->args->count >= 4) {
            char *cx = gen_expr(buf, call->args->items[0]);
            char *cy = gen_expr(buf, call->args->items[1]);
            char *r = gen_expr(buf, call->args->items[2]);
            char *c = gen_expr(buf, call->args->items[3]);
            char result[2048];
            snprintf(result, sizeof(result),
                "vus_gui_draw_circle((int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (unsigned int)vus_to_int(%s, &_err))",
                cx, cy, r, c);
            free(cx); free(cy); free(r); free(c);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_填充圆") == 0) {
        if (call->args && call->args->count >= 4) {
            char *cx = gen_expr(buf, call->args->items[0]);
            char *cy = gen_expr(buf, call->args->items[1]);
            char *r = gen_expr(buf, call->args->items[2]);
            char *c = gen_expr(buf, call->args->items[3]);
            char result[2048];
            snprintf(result, sizeof(result),
                "vus_gui_fill_circle((int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (unsigned int)vus_to_int(%s, &_err))",
                cx, cy, r, c);
            free(cx); free(cy); free(r); free(c);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_圆弧") == 0) {
        if (call->args && call->args->count >= 6) {
            char *cx = gen_expr(buf, call->args->items[0]);
            char *cy = gen_expr(buf, call->args->items[1]);
            char *r = gen_expr(buf, call->args->items[2]);
            char *sd = gen_expr(buf, call->args->items[3]);
            char *sw = gen_expr(buf, call->args->items[4]);
            char *c = gen_expr(buf, call->args->items[5]);
            char result[2048];
            snprintf(result, sizeof(result),
                "vus_gui_draw_arc((int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (unsigned int)vus_to_int(%s, &_err))",
                cx, cy, r, sd, sw, c);
            free(cx); free(cy); free(r); free(sd); free(sw); free(c);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_外观") == 0) {
        if (call->args && call->args->count >= 1) {
            char *rd = gen_expr(buf, call->args->items[0]);
            char *aa = (call->args->count > 1) ? gen_expr(buf, call->args->items[1]) : strdup("vus_to_string(0)");
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_appearance((int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err))",
                rd, aa);
            free(rd); free(aa);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
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

    /* 网络_请求(方式, 地址, 头JSON, 数据, 超时秒, 重试次数)：认证头/超时/重试（缺省参数用默认值） */
    if (strcmp(call->func_name, "网络_请求") == 0) {
        const char *dft[6] = {"vus_string_new(\"GET\")", "vus_string_new(\"\")",
                              "vus_string_new(\"\")", "vus_string_new(\"\")",
                              "vus_string_new(\"30\")", "vus_string_new(\"0\")"};
        char *parts[6] = {NULL, NULL, NULL, NULL, NULL, NULL};
        if (call->args) {
            for (size_t i = 0; i < call->args->count && i < 6; i++)
                parts[i] = gen_expr(buf, call->args->items[i]);
        }
        char result[6144];
        snprintf(result, sizeof(result),
            "vus_plugin_http_request(%s, %s, %s, %s, %s, %s)",
            parts[0] ? parts[0] : dft[0],
            parts[1] ? parts[1] : dft[1],
            parts[2] ? parts[2] : dft[2],
            parts[3] ? parts[3] : dft[3],
            parts[4] ? parts[4] : dft[4],
            parts[5] ? parts[5] : dft[5]);
        for (size_t i = 0; i < 6; i++) free(parts[i]);
        return strdup(result);
    }

    /* 文件_上传(地址, 本地文件, 字段JSON, 头JSON)：multipart，APK 走 Java 平台桥 */
    if (strcmp(call->func_name, "文件_上传") == 0) {
        const char *dft2[4] = {"vus_string_new(\"\")", "vus_string_new(\"\")",
                               "vus_string_new(\"\")", "vus_string_new(\"\")"};
        char *parts[4] = {NULL, NULL, NULL, NULL};
        if (call->args) {
            for (size_t i = 0; i < call->args->count && i < 4; i++)
                parts[i] = gen_expr(buf, call->args->items[i]);
        }
        char result[4096];
        snprintf(result, sizeof(result),
            "vus_plugin_http_upload(%s, %s, %s, %s)",
            parts[0] ? parts[0] : dft2[0],
            parts[1] ? parts[1] : dft2[1],
            parts[2] ? parts[2] : dft2[2],
            parts[3] ? parts[3] : dft2[3]);
        for (size_t i = 0; i < 4; i++) free(parts[i]);
        return strdup(result);
    }

    /* ============= DEX 逻辑拓展（仅 APK） ============= */
    if (strcmp(call->func_name, "拓展_调用") == 0) {
        if (call->args && call->args->count >= 1) {
            char *op = gen_expr(buf, call->args->items[0]);
            char result[4096];
            if (call->args->count >= 2) {
                char *args = gen_expr(buf, call->args->items[1]);
                snprintf(result, sizeof(result), "vus_plugin_ext_call(%s, %s)", op, args);
                free(args);
            } else {
                snprintf(result, sizeof(result), "vus_plugin_ext_call(%s, NULL)", op);
            }
            free(op);
            return strdup(result);
        }
    }

    /* ============= 热更协议（仅 APK）：应用含新 .so/.vua/.dex 的更新包 ============= */
    if (strcmp(call->func_name, "热更_应用") == 0) {
        if (call->args && call->args->count >= 1) {
            char *url = gen_expr(buf, call->args->items[0]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_plugin_hotupdate_apply(%s)", url);
            free(url);
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
    if (strcmp(call->func_name, "文件_是目录") == 0) {
        if (call->args && call->args->count >= 1) {
            char *arg = gen_expr(buf, call->args->items[0]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_plugin_file_isdir(%s)", arg);
            free(arg);
            return strdup(result);
        }
    }

    /* shell 命令执行：popen 捕获输出，供"终端"应用使用 */
    if (strcmp(call->func_name, "命令_执行") == 0) {
        if (call->args && call->args->count >= 1) {
            char *arg = gen_expr(buf, call->args->items[0]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_plugin_shell_exec(%s)", arg);
            free(arg);
            return strdup(result);
        }
    }

    /* 文本分割：按分隔符直接拆成列表（P7：返回值已是列表，免去 JSON_解析 中间层）。
     * 旧写法 JSON_解析(文本_分割(...)) 仍可用（vus_json_parse 幂等）。 */
    if (strcmp(call->func_name, "文本_分割") == 0) {
        if (call->args && call->args->count >= 2) {
            char *a = gen_expr(buf, call->args->items[0]);
            char *b = gen_expr(buf, call->args->items[1]);
            char result[8192];
            snprintf(result, sizeof(result), "vus_plugin_text_split(%s, %s)", a, b);
            free(a); free(b);
            return strdup(result);
        }
        return strdup("vus_plugin_text_split(vus_string_new(\"\"), NULL)");
    }

    /* 文本_行：按换行 \n 拆成列表（一步到位，P7） */
    if (strcmp(call->func_name, "文本_行") == 0) {
        if (call->args && call->args->count >= 1) {
            char *a = gen_expr(buf, call->args->items[0]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_plugin_text_lines(%s)", a);
            free(a);
            return strdup(result);
        }
        return strdup("vus_plugin_text_lines(vus_string_new(\"\"))");
    }

    /* ============= 插件调用内置函数 ============= */
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
            /* 路径按字符串字面量/表达式传递 */
            char *p = gen_expr(buf, call->args->items[1]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_json_query(%s, %s)", json, p);
            free(json); free(p);
            return strdup(result);
        }
    }
    /* 对象文本(值)：把任意值(含 JSON_查询/JSON_解析 返回的结构化对象)安全转为
     * 文本 VusString。对象无法直接参与 + 拼接或转数字，先转文本再操作。 */
    if (strcmp(call->func_name, "对象文本") == 0) {
        if (call->args && call->args->count >= 1) {
            char *arg = gen_expr(buf, call->args->items[0]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_object_to_string(%s)", arg);
            free(arg);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "字典_键") == 0) {
        if (call->args && call->args->count >= 1) {
            char *arg = gen_expr(buf, call->args->items[0]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_dict_keys_of(%s)", arg);
            free(arg);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "typeof") == 0 || strcmp(call->func_name, "类型") == 0) {
        if (call->args && call->args->count >= 1) {
            char *arg = gen_expr(buf, call->args->items[0]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_typeof(%s)", arg);
            free(arg);
            return strdup(result);
        }
    }

    /* ============= Termux-X11 内建（免手敲 termux-x11/zink 启动命令） ============= */
    if (strcmp(call->func_name, "Termux_启动X11") == 0) {
        return strdup("vus_termux_start_x11()");
    }
    if (strcmp(call->func_name, "Termux_启动GPU") == 0) {
        return strdup("vus_termux_start_gl()");
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
    /* 字典_项(字典)：返回所有键值对组成的列表，每个元素是「[键, 值]」双元素列表。
     * 可配合「循环 对 在 字典_项(字典)」同时遍历键与值。 */
    if (strcmp(call->func_name, "字典_项") == 0) {
        if (call->args && call->args->count >= 1) {
            char *a = gen_expr(buf, call->args->items[0]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_dict_items(%s)", a);
            free(a);
            return strdup(result);
        }
        return strdup("vus_dict_items(vus_dict_create())");
    }

    /* ============= 旧式标准库接线（设计文档 §10.1 核心库） =============
     * 把旧式名称映射到现代实现/运行时辅助函数，语义与
     * 文本_*、列表_*、字典_*、文件_*、日期_* 新式版本一致。
     * 注：「等待」为协程保留词（VUS_TOKEN_CN_AWAIT），不做等待(毫秒) 接线。 */

    /* —— 字符串 —— */
    if (strcmp(call->func_name, "长度") == 0) {
        if (call->args && call->args->count >= 1) {
            char *a = gen_expr(buf, call->args->items[0]);
            char result[8192];
            snprintf(result, sizeof(result), "vus_length((void*)(%s))", a);
            free(a);
            return strdup(result);
        }
        return strdup("vus_to_string(0)");
    }
    if (strcmp(call->func_name, "拼接") == 0) {
        if (call->args && call->args->count >= 2) {
            char *a = gen_expr(buf, call->args->items[0]);
            char *b = gen_expr(buf, call->args->items[1]);
            char result[8192];
            snprintf(result, sizeof(result),
                "vus_string_concat((VusString*)(%s), (VusString*)(%s))", a, b);
            free(a); free(b);
            return strdup(result);
        }
        return strdup("vus_string_new(\"\")");
    }
    if (strcmp(call->func_name, "分割") == 0) {
        if (call->args && call->args->count >= 2) {
            char *a = gen_expr(buf, call->args->items[0]);
            char *b = gen_expr(buf, call->args->items[1]);
            char result[8192];
            snprintf(result, sizeof(result), "vus_plugin_text_split(%s, %s)", a, b);
            free(a); free(b);
            return strdup(result);
        }
        return strdup("vus_plugin_text_split(vus_string_new(\"\"), NULL)");
    }
    if (strcmp(call->func_name, "替换") == 0) {
        if (call->args && call->args->count >= 3) {
            char *a = gen_expr(buf, call->args->items[0]);
            char *b = gen_expr(buf, call->args->items[1]);
            char *c = gen_expr(buf, call->args->items[2]);
            char result[8192];
            snprintf(result, sizeof(result),
                "vus_string_replace((VusString*)(%s), (VusString*)(%s), (VusString*)(%s))",
                a, b, c);
            free(a); free(b); free(c);
            return strdup(result);
        }
        return strdup("vus_string_new(\"\")");
    }
    if (strcmp(call->func_name, "取子串") == 0) {
        if (call->args && call->args->count >= 3) {
            char *a = gen_expr(buf, call->args->items[0]);
            char *b = gen_expr(buf, call->args->items[1]);
            char *c = gen_expr(buf, call->args->items[2]);
            char result[8192];
            snprintf(result, sizeof(result),
                "vus_string_slice(%s, (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err))",
                a, b, c);
            free(a); free(b); free(c);
            return strdup(result);
        }
        return strdup("vus_string_new(\"\")");
    }

    /* —— 数字 —— */
    if (strcmp(call->func_name, "取整") == 0) {
        if (call->args && call->args->count >= 1) {
            char *a = gen_expr(buf, call->args->items[0]);
            char result[8192];
            snprintf(result, sizeof(result), "vus_to_string(vus_to_int(%s, &_err))", a);
            free(a);
            return strdup(result);
        }
        return strdup("vus_to_string(0)");
    }
    if (strcmp(call->func_name, "取随机数") == 0) {
        if (call->args && call->args->count >= 2) {
            char *a = gen_expr(buf, call->args->items[0]);
            char *b = gen_expr(buf, call->args->items[1]);
            char result[8192];
            snprintf(result, sizeof(result), "vus_random_int(%s, %s)", a, b);
            free(a); free(b);
            return strdup(result);
        }
        return strdup("vus_to_string(0)");
    }

    /* —— 列表 —— */
    if (strcmp(call->func_name, "创建列表") == 0) {
        char *tmp = strdup("({VusObject* _o = vus_object_list();");
        size_t n = call->args ? call->args->count : 0;
        for (size_t i = 0; i < n; i++) {
            char *e = gen_expr(buf, call->args->items[i]);
            char *tmp2 = NULL;
            if (asprintf(&tmp2, "vus_list_append(_o->u.list, (void*)(%s));", e) < 0) tmp2 = NULL;
            char *nb = (char *)realloc(tmp, strlen(tmp) + strlen(tmp2 ? tmp2 : "") + 1);
            if (nb) { strcat(nb, tmp2 ? tmp2 : ""); tmp = nb; }
            free(tmp2); free(e);
        }
        char *tail = "_o;})";
        char *nb2 = (char *)realloc(tmp, strlen(tmp) + strlen(tail) + 1);
        if (nb2) { strcat(nb2, tail); tmp = nb2; }
        return tmp;
    }
    if (strcmp(call->func_name, "添加元素") == 0) {
        if (call->args && call->args->count >= 2) {
            char *a = gen_expr(buf, call->args->items[0]);
            char *b = gen_expr(buf, call->args->items[1]);
            char result[8192];
            snprintf(result, sizeof(result),
                "({ vus_list_append(vus_list_unwrap((void*)(%s)), (void*)(%s)); %s; })",
                a, b, a);
            free(a); free(b);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "取元素") == 0) {
        if (call->args && call->args->count >= 2) {
            char *a = gen_expr(buf, call->args->items[0]);
            char *b = gen_expr(buf, call->args->items[1]);
            char result[8192];
            snprintf(result, sizeof(result),
                "vus_list_get(vus_list_unwrap((void*)(%s)), (int)vus_to_int(%s, &_err))", a, b);
            free(a); free(b);
            return strdup(result);
        }
        return strdup("vus_string_new(\"\")");
    }
    if (strcmp(call->func_name, "删除元素") == 0) {
        if (call->args && call->args->count >= 2) {
            char *a = gen_expr(buf, call->args->items[0]);
            char *b = gen_expr(buf, call->args->items[1]);
            char result[8192];
            snprintf(result, sizeof(result),
                "({ vus_list_remove(vus_list_unwrap((void*)(%s)), (int)vus_to_int(%s, &_err)); %s; })",
                a, b, a);
            free(a); free(b);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "列表长度") == 0) {
        if (call->args && call->args->count >= 1) {
            char *a = gen_expr(buf, call->args->items[0]);
            char result[8192];
            snprintf(result, sizeof(result),
                "vus_to_string(vus_list_len(vus_list_unwrap((void*)(%s))))", a);
            free(a);
            return strdup(result);
        }
        return strdup("vus_to_string(0)");
    }
    if (strcmp(call->func_name, "遍历列表") == 0) {
        if (call->args && call->args->count >= 2) {
            char *lst = gen_expr(buf, call->args->items[0]);
            char *cb = NULL;
            VusAstNode *fn = call->args->items[1];
            if (fn->type == VUS_AST_IDENTIFIER) {
                /* 直接写函数名：包装为其一等公民函数值 */
                char fs[256];
                gen_sanitize_name(((VusAstIdentifier *)fn)->name, fs, sizeof(fs));
                if (asprintf(&cb, "vus_object_func((void(*)(void*))vus_%s)", fs) < 0) cb = NULL;
            } else {
                cb = gen_expr(buf, fn);   /* 函数值(...) 表达式的函数值 */
            }
            char result[16384];
            snprintf(result, sizeof(result),
                "({ VusObject* _fv = %s;"
                " VusList* _l = vus_list_unwrap((void*)(%s));"
                " for (int _i = 0; _i < (int)_l->len; _i++) {"
                " VusString* _vargs[2]; _vargs[0] = NULL;"
                " _vargs[1] = (VusString*)vus_list_get(_l, _i);"
                " vus_object_func_call(_fv, _vargs);"
                " if (_vargs[0]) vus_unref(_vargs[0]);"
                " } (void)0; })", cb ? cb : "NULL", lst);
            free(lst);
            if (cb) free(cb);
            return strdup(result);
        }
    }

    /* —— 字典 —— */
    if (strcmp(call->func_name, "创建字典") == 0) {
        return strdup("vus_object_dict()");
    }
    if (strcmp(call->func_name, "字典设值") == 0) {
        if (call->args && call->args->count >= 3) {
            char *a = gen_expr(buf, call->args->items[0]);
            char *b = gen_expr(buf, call->args->items[1]);
            char *c = gen_expr(buf, call->args->items[2]);
            char result[8192];
            snprintf(result, sizeof(result),
                "({ vus_dict_set(vus_dict_unwrap((void*)(%s)), (VusString*)(%s), (void*)(%s)); %s; })",
                a, b, c, a);
            free(a); free(b); free(c);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "字典取值") == 0) {
        if (call->args && call->args->count >= 2) {
            char *a = gen_expr(buf, call->args->items[0]);
            char *b = gen_expr(buf, call->args->items[1]);
            char result[8192];
            snprintf(result, sizeof(result),
                "vus_dict_get(vus_dict_unwrap((void*)(%s)), (VusString*)(%s))", a, b);
            free(a); free(b);
            return strdup(result);
        }
        return strdup("vus_string_new(\"\")");
    }
    if (strcmp(call->func_name, "字典删除") == 0) {
        if (call->args && call->args->count >= 2) {
            char *a = gen_expr(buf, call->args->items[0]);
            char *b = gen_expr(buf, call->args->items[1]);
            char result[8192];
            snprintf(result, sizeof(result),
                "({ vus_dict_remove(vus_dict_unwrap((void*)(%s)), (VusString*)(%s)); %s; })",
                a, b, a);
            free(a); free(b);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "字典长度") == 0) {
        if (call->args && call->args->count >= 1) {
            char *a = gen_expr(buf, call->args->items[0]);
            char result[8192];
            snprintf(result, sizeof(result),
                "vus_to_string(vus_dict_len(vus_dict_unwrap((void*)(%s))))", a);
            free(a);
            return strdup(result);
        }
        return strdup("vus_to_string(0)");
    }

    /* —— 文件 —— */
    if (strcmp(call->func_name, "读取文件") == 0) {
        if (call->args && call->args->count >= 1) {
            char *arg = gen_expr(buf, call->args->items[0]);
            char result[8192];
            snprintf(result, sizeof(result), "vus_plugin_file_read(%s)", arg);
            free(arg);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "写入文件") == 0) {
        if (call->args && call->args->count >= 2) {
            char *arg1 = gen_expr(buf, call->args->items[0]);
            char *arg2 = gen_expr(buf, call->args->items[1]);
            char result[8192];
            snprintf(result, sizeof(result), "vus_plugin_file_write(%s, %s)", arg1, arg2);
            free(arg1); free(arg2);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "追加文件") == 0) {
        if (call->args && call->args->count >= 2) {
            char *arg1 = gen_expr(buf, call->args->items[0]);
            char *arg2 = gen_expr(buf, call->args->items[1]);
            char result[8192];
            snprintf(result, sizeof(result), "vus_plugin_file_append(%s, %s)", arg1, arg2);
            free(arg1); free(arg2);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "删除文件") == 0) {
        if (call->args && call->args->count >= 1) {
            char *arg = gen_expr(buf, call->args->items[0]);
            char result[8192];
            snprintf(result, sizeof(result), "vus_plugin_file_delete(%s)", arg);
            free(arg);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "文件是否存在") == 0) {
        if (call->args && call->args->count >= 1) {
            char *arg = gen_expr(buf, call->args->items[0]);
            char result[8192];
            snprintf(result, sizeof(result), "vus_plugin_file_exists(%s)", arg);
            free(arg);
            return strdup(result);
        }
    }

    /* —— 时间与调试 —— */
    if (strcmp(call->func_name, "当前时间") == 0) {
        return strdup("vus_plugin_date_now(NULL)");
    }
    if (strcmp(call->func_name, "调试输出") == 0) {
        if (call->args && call->args->count >= 1) {
            char *arg = gen_expr(buf, call->args->items[0]);
            char result[8192];
            snprintf(result, sizeof(result), "vus_debug_print(vus_string_cstr(%s))", arg);
            free(arg);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "退出") == 0) {
        if (call->args && call->args->count >= 1) {
            char *arg = gen_expr(buf, call->args->items[0]);
            char result[8192];
            snprintf(result, sizeof(result),
                "({ exit((int)vus_to_int(%s, &_err)); vus_literal(\"\"); })", arg);
            free(arg);
            return strdup(result);
        }
        return strdup("({ exit(0); vus_literal(\"\"); })");
    }
    if (strcmp(call->func_name, "断言") == 0) {
        if (call->args && call->args->count >= 1) {
            char *cond = gen_cond(buf, call->args->items[0]);
            char *msg = NULL;
            if (call->args->count >= 2) msg = gen_expr(buf, call->args->items[1]);
            char result[16384];
            snprintf(result, sizeof(result),
                "({ if (!(%s)) vus_assert_fail(%s); (void)0; })",
                cond, msg ? msg : "NULL");
            free(cond);
            if (msg) free(msg);
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
    /* 不可变字面量 → 常驻池（避免每次调用 malloc+复制） */
    snprintf(result, sz, "vus_literal(\"%s\")", escaped);
    return result;
}

static char *gen_expr_number(GenBuf *buf, VusAstNumber *num) {
    (void)buf;
    if (num->is_float) {
        /* 浮点数作为字符串处理（内容不可变 → 驻留池） */
        size_t sz = strlen(num->value) + 64;
        char *result = (char *)malloc(sz);
        snprintf(result, sz, "vus_literal(\"%s\")", num->value);
        return result;
    }
    /* 整数常量归一化为十进制后进字面量池：vus_to_string(0x0F) 输出
     * "15"，故不能用原文（"0x0F"）；先按前缀解析再 snprintf，语义与
     * vus_to_string(N) 完全一致，且免去每次运行时的字符串分配。 */
    if (num->value && num->value[0]) {
        char *end = NULL;
        long long v;
        if (strncmp(num->value, "0x", 2) == 0 || strncmp(num->value, "0X", 2) == 0)
            v = strtoll(num->value + 2, &end, 16);
        else if (strncmp(num->value, "0b", 2) == 0 || strncmp(num->value, "0B", 2) == 0)
            v = strtoll(num->value + 2, &end, 2);
        else
            v = strtoll(num->value, &end, 10);
        if (end && *end == '\0') {
            char *result = (char *)malloc(64);
            snprintf(result, 64, "vus_literal(\"%lld\")", (long long)v);
            return result;
        }
    }
    size_t sz = strlen(num->value) + 64;
    char *result = (char *)malloc(sz);
    snprintf(result, sz, "vus_to_string(%s)", num->value);
    return result;
}

static char *gen_expr_bool(GenBuf *buf, VusAstBool *b) {
    (void)buf;
    if (b->value) {
        return strdup("vus_literal(\"true\")");
    }
    return strdup("vus_literal(\"false\")");
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
        case VUS_AST_AWAIT: {
            VusAstAwait *aw = (VusAstAwait *)node;
            char *coro = gen_expr(buf, aw->coro);
            char result[1024];
            snprintf(result, sizeof(result), "vus_coro_await_handle(%s)", coro);
            free(coro);
            return strdup(result);
        }
        case VUS_AST_SUBSCRIPT: {
            VusAstSubscript *sub = (VusAstSubscript *)node;
            char *obj = gen_expr(buf, sub->object);
            char *idx = gen_expr(buf, sub->index);
            /* 用 vus_list_unwrap 解包：兼容 VusObject*(TYPE_LIST) 装箱与裸 VusList*。
             * 若直接强转 (VusList*)，对装箱列表会把 magic/type 当 len/items 读，
             * 下标访问越界。与「列表_取」内建同一解包风格。 */
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_list_get(vus_list_unwrap((void*)(%s)), vus_to_int(%s, &_err))",
                obj, idx);
            free(obj);
            free(idx);
            return strdup(result);
        }
        case VUS_AST_LIST_LITERAL: {
            VusAstListLiteral *ll = (VusAstListLiteral *)node;
            /* 用 GCC statement 表达式构造 VusObject（装箱）并逐项 append，
             * 使列表字面量与 VUS 字符串值模型隔离，可被 vus_print/typeof 安全识别。
             * 装箱由 vus_object_list() 一行完成（与旧多行内联等价）。 */
            char *tmp = strdup("({VusObject* _o = vus_object_list();");
            if (ll->items) {
                for (size_t i = 0; i < ll->items->count; i++) {
                    char *e = gen_expr(buf, ll->items->items[i]);
                    char *tmp2;
                    if (asprintf(&tmp2, "vus_list_append(_o->u.list, (void*)(%s));", e) < 0) { tmp2 = NULL; }
                    char *nb = (char*)realloc(tmp, strlen(tmp) + strlen(tmp2?tmp2:"") + 1);
                    if (nb) { strcat(nb, tmp2?tmp2:""); tmp = nb; }
                    free(tmp2); free(e);
                }
            }
            char *tail = "_o;})";
            char *nb2 = (char*)realloc(tmp, strlen(tmp) + strlen(tail) + 1);
            if (nb2) { strcat(nb2, tail); tmp = nb2; }
            return tmp;
        }
        case VUS_AST_DICT_LITERAL: {
            VusAstDictLiteral *dl = (VusAstDictLiteral *)node;
            char *tmp = strdup("({VusObject* _o = vus_object_dict();");
            if (dl->keys && dl->values) {
                for (size_t i = 0; i < dl->keys->count; i++) {
                    char *k = gen_expr(buf, dl->keys->items[i]);
                    char *v = gen_expr(buf, dl->values->items[i]);
                    char *tmp2;
                    if (asprintf(&tmp2, "vus_dict_set(_o->u.dict, (VusString*)(%s), (void*)(%s));", k, v) < 0) { tmp2 = NULL; }
                    char *nb = (char*)realloc(tmp, strlen(tmp) + strlen(tmp2?tmp2:"") + 1);
                    if (nb) { strcat(nb, tmp2?tmp2:""); tmp = nb; }
                    free(tmp2); free(k); free(v);
                }
            }
            char *tail = "_o;})";
            char *nb2 = (char*)realloc(tmp, strlen(tmp) + strlen(tail) + 1);
            if (nb2) { strcat(nb2, tail); tmp = nb2; }
            return tmp;
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

    /* 局部/全局变量统一走 vus_var_set 引用计数热路径（语义：ref 新值 + unref 旧值） */
    gen_emit_linef(buf, "vus_var_set(&vus_%s, %s);", san, val);

    free(val);
}

static void gen_stmt_expr(GenBuf *buf, VusAstExprStmt *stmt) {
    char *expr = gen_expr(buf, stmt->expr);
    gen_emit_linef(buf, "%s;", expr);
    free(expr);
}

static void gen_stmt_if(GenBuf *buf, VusAstIf *if_stmt) {
    char *cond = gen_cond(buf, if_stmt->condition);

    gen_emit_linef(buf, "if (%s) {", cond);
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
            char *econd = gen_cond(buf, if_stmt->elif_conditions->items[i]);
            gen_emit_linef(buf, "else if (%s) {", econd);
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
    /* 只构造一次循环值：vus_var_set 完成新旧引用交接，_tmp 归还 */
    gen_emit_linef(buf, "{ VusString* _tmp = vus_to_string(_i); vus_var_set(&vus_%s, _tmp); vus_unref(_tmp); }", san);

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
    /* 先对可迭代对象求值一次，再按容器类型分派：
     *  - 字典容器(VusObject* TYPE_DICT)：vus_dict_keys_of 取键列表遍历（键），
     *    返回的键列表为本函数新建，循环结束须 vus_unref 归还；
     *  - 列表容器(TYPE_LIST) / 裸 VusList*：沿用既有列表遍历（零额外开销）。 */
    gen_emit_linef(buf, "void* _it = (void*)(%s);", iter);
    gen_emit_linef(buf,
        "VusList* _kl = (vus_is_object(_it) && ((VusObject*)_it)->type == TYPE_DICT) ? vus_dict_keys_of(_it) : NULL;");
    gen_emit_linef(buf,
        "VusList* _list = _kl ? _kl : (vus_is_object(_it) ? ((VusObject*)_it)->u.list : (VusList*)_it);");
    gen_emit_linef(buf, "for (int _i = 0; _i < vus_list_len(_list); _i++) {");
    buf->indent++;
    /* 取一次元素：vus_var_set 交接新旧引用 */
    gen_emit_linef(buf, "{ VusString* _tmp = (VusString*)vus_list_get(_list, _i); vus_var_set(&vus_%s, _tmp); }", san);

    if (fe->body) {
        for (size_t i = 0; i < fe->body->count; i++) {
            gen_statement(buf, fe->body->items[i]);
        }
    }

    buf->indent--;
    gen_emit_line(buf, "}");
    /* 字典分支：归还 keys_of 新建的键列表（含其键副本引用） */
    gen_emit_line(buf, "if (_kl) vus_unref((void*)_kl);");
    buf->indent--;
    gen_emit_line(buf, "}");

    free(iter);
}

static void gen_stmt_while(GenBuf *buf, VusAstWhile *wl) {
    char *cond = gen_cond(buf, wl->condition);

    gen_emit_linef(buf, "while (%s) {", cond);
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
    const char *ret_stmt = s_gen_in_main ? "return 0;" : "return;";
    if (ret->value) {
        char *val = gen_expr(buf, ret->value);
        /* 返回槽位赋值（args[0]）与普通变量一样走 vus_var_set 引用计数热路径 */
        gen_emit_linef(buf, "vus_var_set(&((VusString**)_args)[0], %s);", val);
        gen_emit_line(buf, "vus_stack_pop();");
        gen_emit_line(buf, ret_stmt);
        free(val);
    } else {
        gen_emit_line(buf, "vus_stack_pop();");
        gen_emit_line(buf, ret_stmt);
    }
}

/* 多返回值：返回 a, b, c → 打包成列表对象放入 _vus_args[0]。
 * 调用侧多目标赋值（VUS_AST_MULTI_ASSIGN）再从列表拆包。 */
static void gen_stmt_return_multi(GenBuf *buf, VusAstReturnMulti *ret) {
    const char *ret_stmt = s_gen_in_main ? "return 0;" : "return;";
    /* 构造 VusObject(TYPE_LIST) 并 append 每个返回值 */
    char *tmp = strdup("({VusObject* _o = vus_object_list();");
    if (!tmp) return;
    if (ret->values) {
        for (size_t i = 0; i < ret->values->count; i++) {
            char *e = gen_expr(buf, ret->values->items[i]);
            char *n2 = NULL;
            if (asprintf(&n2, "vus_list_append(_o->u.list, (void*)(%s));", e) < 0) { n2 = NULL; }
            free(e);
            if (!n2) { continue; }
            char *n3 = NULL;
            if (asprintf(&n3, "%s%s", tmp, n2) < 0) { n3 = NULL; }
            free(n2);
            char *old = tmp;
            if (n3) tmp = n3; else tmp = NULL;
            free(old);
            if (!tmp) { return; }
        }
    }
    {
        char *tail = NULL;
        if (asprintf(&tail, "%s(VusString*)_o;})", tmp) < 0) { tail = NULL; }
        free(tmp);
        if (!tail) { return; }
        gen_emit_linef(buf, "vus_var_set(&((VusString**)_args)[0], %s);", tail);
        free(tail);
    }
    gen_emit_line(buf, "vus_stack_pop();");
    gen_emit_line(buf, ret_stmt);
}

/* 多目标赋值：a, b = 函数() → 右侧求值得到列表，逐项拆包赋值。 */
static void gen_stmt_multi_assign(GenBuf *buf, VusAstMultiAssign *assign) {
    char *val = gen_expr(buf, assign->value);
    int n = assign->targets ? (int)assign->targets->count : 0;
    /* 用一个临时变量持有函数返回的列表对象，避免多次求值调用 */
    gen_emit_line(buf, "{");
    buf->indent++;
    gen_emit_linef(buf, "VusObject* _mret = (vus_is_object((void*)(%s))) ? (VusObject*)(%s) : NULL;", val, val);
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            VusAstIdentifier *idn = (VusAstIdentifier*)assign->targets->items[i];
            char san[256];
            gen_sanitize_name(idn->name, san, sizeof(san));
            gen_emit_linef(buf, "VusString* _mval%d = (_mret && _mret->type==TYPE_LIST && _mret->u.list && vus_list_len(_mret->u.list) > %d) ? (VusString*)vus_list_get(_mret->u.list, %d) : NULL;",
                i, i, i);
            gen_emit_linef(buf, "vus_ref(_mval%d);", i);
            gen_emit_linef(buf, "vus_var_set(&vus_%s, _mval%d);", san, i);
            gen_emit_linef(buf, "vus_unref(_mval%d);", i);
        }
    }
    gen_emit_line(buf, "vus_unref(_mret);");
    buf->indent--;
    gen_emit_line(buf, "}");
    free(val);
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
    /* 字面量来自驻留池（借用，不增计数），unref 会误释放池实例；仅释放新分配值 */
    if (!(strncmp(val, "vus_literal(", 12) == 0 && val[strlen(val) - 1] == ')')) {
        gen_emit_linef(buf, "vus_unref(%s);", val);
    }
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
        case VUS_AST_RETURN_MULTI:
            gen_stmt_return_multi(buf, (VusAstReturnMulti *)node);
            break;
        case VUS_AST_MULTI_ASSIGN:
            gen_stmt_multi_assign(buf, (VusAstMultiAssign *)node);
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

/* 判断某个变量名是否为函数参数（需在函数顶部单独声明，局部变量收集时须排除，
 * 否则会与参数声明产生重复声明，导致 C 编译错误 "redefinition"。） */
static int gen_is_param_name(VusAstFunctionDef *func, const char *name) {
    if (!func || !func->params || !name) return 0;
    for (size_t i = 0; i < func->params->count; i++) {
        VusAstNode *pnode = func->params->items[i];
        if (pnode->type == VUS_AST_PARAM) {
            VusAstParam *p = (VusAstParam *)pnode;
            if (p->name && strcmp(p->name, name) == 0) return 1;
        } else if (pnode->type == VUS_AST_PARAM_DEFAULT) {
            VusAstParamDefault *p = (VusAstParamDefault *)pnode;
            if (p->name && strcmp(p->name, name) == 0) return 1;
        }
    }
    return 0;
}

/* 递归扫描 AST 节点，收集局部变量名（排除函数参数） */
static void gen_collect_locals(VusAstFunctionDef *func, VusAstNode *node, VusAstList *locals) {
    if (!node) return;
    if (node->type == VUS_AST_ASSIGN) {
        VusAstAssign *assign = (VusAstAssign *)node;
        if (assign->is_local) {
            /* 参数名已在函数顶部声明，跳过，避免重复声明 */
            if (gen_is_param_name(func, assign->target)) return;
            /* 顶层全局变量：不收集为局部，否则函数内局部声明遮蔽文件级全局，
             * 导致函数内（含事件函数）读写全局变量静默失效 */
            if (gen_is_global_name(assign->target)) return;
            /* 检查是否已收集 */
            for (size_t i = 0; i < locals->count; i++) {
                VusAstIdentifier *id = (VusAstIdentifier *)locals->items[i];
                if (strcmp(id->name, assign->target) == 0) return;
            }
            VusAstIdentifier *id = vus_ast_ident_new(assign->target, 0, 0);
            vus_ast_list_push(locals, (VusAstNode *)id);
        }
    } else if (node->type == VUS_AST_MULTI_ASSIGN) {
        VusAstMultiAssign *ma = (VusAstMultiAssign *)node;
        if (ma->is_local && ma->targets) {
            for (size_t i = 0; i < ma->targets->count; i++) {
                VusAstIdentifier *idn = (VusAstIdentifier *)ma->targets->items[i];
                if (gen_is_param_name(func, idn->name) || gen_is_global_name(idn->name)) continue;
                int found = 0;
                for (size_t j = 0; j < locals->count; j++) {
                    VusAstIdentifier *id = (VusAstIdentifier *)locals->items[j];
                    if (strcmp(id->name, idn->name) == 0) { found = 1; break; }
                }
                if (!found) vus_ast_list_push(locals, (VusAstNode *)vus_ast_ident_new(idn->name, 0, 0));
            }
        }
    } else if (node->type == VUS_AST_IF) {
        VusAstIf *ifn = (VusAstIf *)node;
        if (ifn->then_body) {
            for (size_t i = 0; i < ifn->then_body->count; i++)
                gen_collect_locals(func, ifn->then_body->items[i], locals);
        }
        if (ifn->elif_bodies) {
            for (size_t i = 0; i < ifn->elif_bodies->count; i++) {
                VusAstList *body = (VusAstList *)ifn->elif_bodies->items[i];
                if (body) {
                    for (size_t j = 0; j < body->count; j++)
                        gen_collect_locals(func, body->items[j], locals);
                }
            }
        }
        if (ifn->else_body) {
            for (size_t i = 0; i < ifn->else_body->count; i++)
                gen_collect_locals(func, ifn->else_body->items[i], locals);
        }
    } else if (node->type == VUS_AST_FOR_RANGE) {
        VusAstForRange *fr = (VusAstForRange *)node;
        if (fr->body) {
            for (size_t i = 0; i < fr->body->count; i++)
                gen_collect_locals(func, fr->body->items[i], locals);
        }
    } else if (node->type == VUS_AST_FOR_EACH) {
        VusAstForEach *fe = (VusAstForEach *)node;
        if (fe->body) {
            for (size_t i = 0; i < fe->body->count; i++)
                gen_collect_locals(func, fe->body->items[i], locals);
        }
    } else if (node->type == VUS_AST_WHILE) {
        VusAstWhile *wl = (VusAstWhile *)node;
        if (wl->body) {
            for (size_t i = 0; i < wl->body->count; i++)
                gen_collect_locals(func, wl->body->items[i], locals);
        }
    } else if (node->type == VUS_AST_TRY) {
        VusAstTry *tryn = (VusAstTry *)node;
        if (tryn->try_body) {
            for (size_t i = 0; i < tryn->try_body->count; i++)
                gen_collect_locals(func, tryn->try_body->items[i], locals);
        }
        if (tryn->except_bodies) {
            for (size_t i = 0; i < tryn->except_bodies->count; i++) {
                VusAstList *body = (VusAstList *)tryn->except_bodies->items[i];
                if (body) {
                    for (size_t j = 0; j < body->count; j++)
                        gen_collect_locals(func, body->items[j], locals);
                }
            }
        }
    }
}

/* ============ 函数体特征扫描 ============
 * 预处理函数体：是否含「带值返回」（决定 _vus_result + 尾部搬移模板）、
 * 是否含 尝试/排除（决定 _vus_err 声明）。无对应特征即省略模板，减小生成体积。 */
static void gen_scan_block(VusAstList *body, int *has_ret, int *has_try);

static void gen_scan_node(VusAstNode *node, int *has_ret, int *has_try) {
    if (!node) return;
    switch (node->type) {
        case VUS_AST_RETURN:
            if (((VusAstReturn *)node)->value) *has_ret = 1;
            break;
        case VUS_AST_TRY: {
            VusAstTry *t = (VusAstTry *)node;
            *has_try = 1;
            gen_scan_block(t->try_body, has_ret, has_try);
            if (t->except_bodies)
                for (size_t i = 0; i < t->except_bodies->count; i++) {
                    VusAstList *b = (VusAstList *)t->except_bodies->items[i];
                    if (b) gen_scan_block(b, has_ret, has_try);
                }
            break;
        }
        case VUS_AST_IF: {
            VusAstIf *f = (VusAstIf *)node;
            gen_scan_block(f->then_body, has_ret, has_try);
            if (f->elif_bodies)
                for (size_t i = 0; i < f->elif_bodies->count; i++) {
                    VusAstList *b = (VusAstList *)f->elif_bodies->items[i];
                    if (b) gen_scan_block(b, has_ret, has_try);
                }
            gen_scan_block(f->else_body, has_ret, has_try);
            break;
        }
        case VUS_AST_WHILE: {
            VusAstWhile *w = (VusAstWhile *)node;
            gen_scan_block(w->body, has_ret, has_try);
            break;
        }
        case VUS_AST_FOR_RANGE: {
            VusAstForRange *fr = (VusAstForRange *)node;
            gen_scan_block(fr->body, has_ret, has_try);
            break;
        }
        case VUS_AST_FOR_EACH: {
            VusAstForEach *fe = (VusAstForEach *)node;
            gen_scan_block(fe->body, has_ret, has_try);
            break;
        }
        default:
            break;
    }
}

static void gen_scan_block(VusAstList *body, int *has_ret, int *has_try) {
    if (!body) return;
    for (size_t i = 0; i < body->count; i++)
        gen_scan_node(body->items[i], has_ret, has_try);
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

    /* 返回值变量：仅在函数含「带值返回」时生成（含 if/循环/尝试子块） */
    int has_ret = 0, has_try = 0;
    gen_scan_block(func->body, &has_ret, &has_try);
    if (has_ret) gen_emit_line(buf, "VusString* _vus_result = NULL;");
    gen_emit_line(buf, "int _err = 0;");
    if (has_try) gen_emit_line(buf, "VusError* _vus_err = NULL;");

    /* 扫描函数体中的局部变量，在函数顶部声明 */
    if (func->body) {
        VusAstList *locals = vus_ast_list_new();
        for (size_t i = 0; i < func->body->count; i++) {
            gen_collect_locals(func, func->body->items[i], locals);
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

    /* 设置返回值（在 args[0] 中）——仅当函数可能产生返回值；
     * vus_var_set 容忍 _vus_result 为 NULL（不操作），免除 if 包装 */
    if (has_ret) {
        gen_emit_line(buf, "vus_var_set(&((VusString**)_args)[0], _vus_result);");
    }

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

    s_gen_in_main = 1;

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

    s_gen_in_main = 0;
}

/* ============ 公开 API 实现 ============ */

char *vus_generate_c(VusAstProgram *program, VusConfig *config) {
    if (!program || !config) return NULL;

    g_vua_prog = program; /* 供 界面_绑定 闭包生成时按名查事件函数定义 */

    g_uses_gui = 0; /* 每次生成前重置 GUI 使用标记 */
    g_uses_vua = 0; /* 每次生成前重置 VUA 使用标记 */
    g_vua_bind_count = 0;
    g_vua_op_count = 0;
    s_global_count = 0; /* 每次生成前重置全局变量名集合 */
    if (g_vua_premain) { free(g_vua_premain->data); free(g_vua_premain); g_vua_premain = NULL; }
    if (g_vua_fwd) { free(g_vua_fwd->data); free(g_vua_fwd); g_vua_fwd = NULL; }

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
        gen_emit(buf, "#include \"libvus_rt.h\"\n");
        /* 仅当用到 图形_*（GuiLite X11）时才引入图形桥接头，避免 APK 无该头难编译 */
        if (g_uses_gui) gen_emit(buf, "#include \"guilite_bridge.h\"\n");
        gen_emit(buf, "\n");
    }

    /* 线程/协程运行时辅助 */
    gen_emit(buf, "/* 线程/协程运行时辅助 */\n");
    gen_emit(buf, "typedef struct { void (*func)(void*); void* arg; } _VusThreadTask;\n");
    gen_emit(buf, "static void* _vus_thread_run(void* _arg) {\n");
    gen_emit(buf, "    _VusThreadTask* _task = (_VusThreadTask*)_arg;\n");
    gen_emit(buf, "    VusString* _vus_args[2] = {NULL, (VusString*)_task->arg};\n");
    gen_emit(buf, "    _task->func(_vus_args);\n");
    gen_emit(buf, "    vus_coro_store_result(_vus_args[0]);\n");
    gen_emit(buf, "    return _vus_args[0];\n");
    gen_emit(buf, "}\n\n");

    /* 全局变量声明（去重，递归含顶层控制流块内赋值，修复反馈 1.1） */
    if (program->statements) {
        GenBuf gl;
        memset(&gl, 0, sizeof(gl));
        gl.cap = 4096;
        gl.data = (char *)malloc(gl.cap);
        gl.data[0] = '\0';
        gen_globals_walk_list(program->statements, &gl);
        gen_emit(buf, gl.data);
        free(gl.data);
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

    /* 主函数（vusx 插件等库式编译时跳过，避免与宿主程序 main 冲突） */
    if (!config->omit_main)
        gen_main_function(buf, program, config->debug);

    char *result = strdup(buf->data);

    /* 收集起来的 VUA 闭包包装函数（须在 main 前定义）与 vua.h 头、前向声明，统一拼装。
     * 因为 g_uses_vua / g_vua_premain 是生成 main 时才确定，故在生成完成后拼。 */
    if (g_uses_vua) {
        size_t total = strlen(result) + 64 + 64;
        if (g_vua_premain && g_vua_premain->data) total += strlen(g_vua_premain->data);
        if (g_vua_fwd && g_vua_fwd->data) total += strlen(g_vua_fwd->data);
        char *assembled = (char *)malloc(total + 1);
        if (!assembled) {
            free(result);
        } else {
            /* 在 int main(void) { 前插入包装函数 */
            const char *mk = "int main(void) {";
            char *at = strstr(result, mk);
            size_t head = at ? (size_t)(at - result) : strlen(result);
            size_t pos = 0;
            if (at) {
                /* 前向声明放 include 之后（结果最前面是标准 include 区） */
                snprintf(assembled + pos, total - pos, "#include \"vua.h\"\n\n");
                pos += strlen(assembled + pos);
                if (g_vua_fwd && g_vua_fwd->data) {
                    snprintf(assembled + pos, total - pos, "%s\n", g_vua_fwd->data);
                    pos += strlen(assembled + pos);
                }
                strncpy(assembled + pos, result, head); pos += head;
                if (g_vua_premain && g_vua_premain->data) {
                    snprintf(assembled + pos, total - pos, "\n%s\n", g_vua_premain->data);
                    pos += strlen(assembled + pos);
                }
                strcpy(assembled + pos, result + head);
            } else {
                /* 兜底：没找到主函数（异常），仅前置 include */
                snprintf(assembled + pos, total - pos, "#include \"vua.h\"\n\n%s", result);
            }
            free(result);
            result = assembled;
        }
    }

    free(buf->data);
    free(buf);

    /* 释放 VUA 绑定包装缓冲（其内容已并入 result，此处仅回收内存） */
    if (g_vua_premain) { free(g_vua_premain->data); free(g_vua_premain); g_vua_premain = NULL; }
    if (g_vua_fwd) { free(g_vua_fwd->data); free(g_vua_fwd); g_vua_fwd = NULL; }

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

    /* 确定优化级别：优先环境变量 VUS_OPT 显式覆盖（如 VUS_OPT=-O0 / -O1），
       用于在低性能设备（如 Termux）上加速编译大型生成 C；否则按配置。 */
    const char *opt_level = "-O2";
    const char *env_opt = getenv("VUS_OPT");
    if (env_opt && env_opt[0]) {
        opt_level = env_opt;
    } else if (config->optimization[0]) {
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

    /* EasyLogger 日志库：头文件路径 + 需要一并编译的源码（随 libvus_rt.c 链接） */
    char elog_inc[1024];
    char elog_src[2048];
    if (config->rt_dir[0]) {
        snprintf(elog_inc, sizeof(elog_inc), "-I\"%s/easylogger/inc\"", abs_rt_dir);
        snprintf(elog_src, sizeof(elog_src),
                 "\"%s/easylogger/src/elog.c\" \"%s/easylogger/src/elog_utils.c\" \"%s/elog_port.c\"",
                 abs_rt_dir, abs_rt_dir, abs_rt_dir);
    } else {
        elog_inc[0] = '\0';
        elog_src[0] = '\0';
    }

    /* 构建 GCC 命令 */
    char cmd[8192];
    int n;

    /* 优先使用预编译静态库（make 产物 build/libvus_rt.a），用户代码仅编译链接，
       避免每次运行都重编整个运行时（含 yyjson/EasyLogger/GuiLite），大幅提速。
       仅当静态库存在时启用；否则回退到旧的全源码编译路径。 */
    char static_rt_lib[2048];
    int use_static_rt = 0;
    snprintf(static_rt_lib, sizeof(static_rt_lib), "%s/../build/libvus_rt.a", abs_rt_dir);
    if (config->rt_dir[0]) {
        FILE *lib_check = fopen(static_rt_lib, "r");
        if (lib_check) { use_static_rt = 1; fclose(lib_check); }
    }

    /* 检测系统是否安装了 libcurl 开发头文件 */
    int has_curl = 0;
    FILE *curl_check = popen("curl-config --version >/dev/null 2>&1", "r");
    if (curl_check) {
        has_curl = (pclose(curl_check) == 0);
    }

    const char *curl_def = has_curl ? "-DVUS_HAVE_CURL" : "";
    const char *curl_lib = has_curl ? "-lcurl" : "";

    /* 检测 libpython 开发环境（可选）：存在则启用进程内嵌入 */
    int has_py = 0;
    char py_inc[1024] = {0};
    char py_ld[2048] = {0};
    FILE *py_check = popen("python3-config --includes 2>/dev/null", "r");
    if (py_check) {
        if (fgets(py_inc, sizeof(py_inc), py_check)) {
            /* 去掉末尾换行 */
            size_t pl = strlen(py_inc);
            while (pl > 0 && (py_inc[pl-1] == '\n' || py_inc[pl-1] == ' ')) py_inc[--pl] = '\0';
            if (py_inc[0]) has_py = 1;
        }
        pclose(py_check);
    }
    if (has_py) {
        FILE *py_ldf = popen("python3-config --ldflags 2>/dev/null", "r");
        if (py_ldf) {
            if (fgets(py_ld, sizeof(py_ld), py_ldf)) {
                size_t pl = strlen(py_ld);
                while (pl > 0 && (py_ld[pl-1] == '\n' || py_ld[pl-1] == ' ')) py_ld[--pl] = '\0';
            }
            pclose(py_ldf);
        }
    }
    const char *py_def = has_py ? "-DVUS_USE_PY" : "";
    const char *py_inc_str = has_py ? py_inc : "";
    const char *py_ld_str = has_py ? py_ld : "";

    /* GuiLite 图形库：仅当源码使用了 图形_* 内建函数时，才编译 bridge/platform/wrapper
       并追加 -lstdc++（GuiLite 为 C++）与 -lX11（X11 后端可用时）。 */
    char gui_src[2048];
    gui_src[0] = '\0';
    const char *gui_def = "";
    /* gui_lib 需要被 strcat 追加 " -lpng -lz"（图形_背景图），必须用可写的固定缓冲，
       不能用指向字符串字面量的 const char*（strcat 越界写只读页 → SIGSEGV）。 */
    char gui_lib[128] = "";
    char xft_inc[512] = "";
    if (config->rt_dir[0] && g_uses_gui) {
        snprintf(gui_src, sizeof(gui_src),
                 "\"%s/guilite_bridge.c\" \"%s/guilite_platform.c\" \"%s/guilite_wrapper.cpp\" \"%s/gifdec/gifdec.c\"",
                 abs_rt_dir, abs_rt_dir, abs_rt_dir, abs_rt_dir);
        int has_x11 = 0;
        /* X11 开发库检测：同时支持标准路径与 Termux $PREFIX 路径（Termux 的
           X11 头在 $PREFIX/include/X11/Xlib.h，非 /usr/include）。 */
        FILE *x11_check = popen(
            "test -f /usr/include/X11/Xlib.h -o -f \"${PREFIX:-/usr}/include/X11/Xlib.h\" "
            "-o -f \"$TERMUX_PREFIX/include/X11/Xlib.h\" && echo yes || echo no", "r");
        if (x11_check) {
            char line[16] = {0};
            if (fgets(line, sizeof(line), x11_check)) {
                has_x11 = (strncmp(line, "yes", 3) == 0);
            }
            pclose(x11_check);
        }
        /* Xft 开发库检测：用于按 UTF-8/Unicode 叠加中英文文本（X11 核心字体
           不含中文字形）。头路径同样兼容 Termux $PREFIX。 */
        int has_xft = 0;
        FILE *xft_check = popen(
            "test -f /usr/include/X11/Xft/Xft.h -o -f \"${PREFIX:-/usr}/include/X11/Xft/Xft.h\" "
            "-o -f \"$TERMUX_PREFIX/include/X11/Xft/Xft.h\" && echo yes || echo no", "r");
        if (xft_check) {
            char line[16] = {0};
            if (fgets(line, sizeof(line), xft_check)) {
                has_xft = (strncmp(line, "yes", 3) == 0);
            }
            pclose(xft_check);
        }
        /* 捕获 FreeType 头路径（Xft.h 与 guilite_bridge.c 的 ft2build.h 都依赖它）。
           Termux 与 PC 的 freetype 目录不同，统一用 pkg-config 解析。凡使用 GUI 均需，
           因为 guilite_bridge.o/源会无条件 include <ft2build.h>。 */
        {
            FILE *ftc = popen("pkg-config --cflags freetype2 2>/dev/null", "r");
            if (ftc) {
                if (fgets(xft_inc, sizeof(xft_inc), ftc)) {
                    size_t xl = strlen(xft_inc);
                    while (xl > 0 && (xft_inc[xl-1] == '\n' || xft_inc[xl-1] == ' ')) xft_inc[--xl] = '\0';
                }
                pclose(ftc);
            }
        }
        gui_def = has_x11 ? "-DVUS_GUI_X11" : "";
        /* -rdynamic：把主程序全局符号导出到 dynsym，dlsym 才能反查用户脚本
           定义的 事件_点击 等回调函数；-ldl 提供 dlsym（Termux bionic 必需）。
           Xft 用于 UTF-8 中文叠加，-lXft -lfontconfig 链入渲染库。 */
        if (has_x11) {
            snprintf(gui_lib, sizeof(gui_lib), "-lX11 -rdynamic -ldl");
            if (has_xft) { snprintf(gui_lib, sizeof(gui_lib), "-lX11 -lXft -lfontconfig -rdynamic -ldl"); }
        } else {
            snprintf(gui_lib, sizeof(gui_lib), "-rdynamic -ldl");
        }
        /* 图形_背景图 依赖 libpng（及其 zlib 依赖）。注意：静态库 libvus_rt.a 中的
           guilite_bridge.o 始终引用 png_* 符号（vus_gui_draw_png），因此凡使用 GUI
           的用例都必须追加 -lpng -lz，否则链接器会报 png 符号缺失。
           guilite_bridge.o 同样引用 FreeType 符号（vus_gui_font 的 FT_* 调用），
           故凡使用 GUI 也一律追加 -lfreetype。 */
        if (g_uses_gui) { strcat(gui_lib, " -lpng -lz -lfreetype"); }
    }

    if (use_static_rt) {
        /* ---- 静态库路径（推荐）：仅编译用户 C，运行时从 build/libvus_rt.a 链接 ----
           libvus_rt.a 已在 make 时以匹配的 PY/GUI flags 编译，故无需再拼 py_inc/gui 源。
           仅 GUI 用例需要追加 X11/Xft/-rdynamic/-ldl 与 -lstdc++（链接 .a 内的 C++ 包装）。 */
        if (g_uses_gui) {
            /* VUS_GUI_GLES 运行时环境变量：额外链接 EGL/GLES，供 libvus_rt.a
               内的 guilite_gles.o 解析其依赖（免去运行时 dlopen 硬编码）。
               仅需判断环境变量：GLES 必然属于 X11 GUI 路径。 */
            const char *gles_lib = " ";
            if (getenv("VUS_GUI_GLES") && getenv("VUS_GUI_GLES")[0]) {
                gles_lib = " -lEGL -lGLESv2";
            }
            n = snprintf(cmd, sizeof(cmd),
                "gcc %s -I\"%s\" %s \"%s\" %s \"%s\" -o \"%s\" -lm -lpthread -lstdc++ %s %s %s %s 2>&1",
                opt_level,
                abs_rt_dir,
                xft_inc,
                c_source_path,
                extra_objects && extra_objects[0] ? extra_objects : "",
                static_rt_lib,
                output_path,
                curl_lib,
                gui_lib,
                gui_def,
                gles_lib);
        } else {
            n = snprintf(cmd, sizeof(cmd),
                "gcc %s -I\"%s\" \"%s\" %s \"%s\" -o \"%s\" -lm -lpthread %s %s 2>&1",
                opt_level,
                abs_rt_dir,
                c_source_path,
                extra_objects && extra_objects[0] ? extra_objects : "",
                static_rt_lib,
                output_path,
                curl_lib,
                gui_lib);
        }
    } else if (extra_objects && extra_objects[0]) {
        n = snprintf(cmd, sizeof(cmd),
            "gcc %s -g %s %s %s -I\"%s\" %s %s \"%s\" \"%s\" \"%s\" %s %s %s -o \"%s\" -lm -lpthread %s %s -lstdc++ %s %s 2>&1",
            opt_level,
            curl_def,
            py_def,
            py_inc_str,
            abs_rt_dir,
            elog_inc,
            xft_inc,
            c_source_path,
            rt_source,
            rt_coro,
            elog_src,
            gui_src,
            extra_objects,
            output_path,
            curl_lib,
            py_ld_str,
            gui_def,
            gui_lib);
    } else {
        n = snprintf(cmd, sizeof(cmd),
            "gcc %s -g %s %s %s -I\"%s\" %s %s \"%s\" \"%s\" \"%s\" %s %s -o \"%s\" -lm -lpthread %s %s -lstdc++ %s %s 2>&1",
            opt_level,
            curl_def,
            py_def,
            py_inc_str,
            abs_rt_dir,
            elog_inc,
            xft_inc,
            c_source_path,
            rt_source,
            rt_coro,
            elog_src,
            gui_src,
            output_path,
            curl_lib,
            py_ld_str,
            gui_def,
            gui_lib);
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
    char err_buf[16384] = {0};
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