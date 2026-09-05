/*
 * lsp.c — VUS 语言服务器（LSP）核心实现
 *
 * - JSON-RPC 2.0 over LSP 标准传输（Content-Length 分帧，stdin/stdout）
 * - 三层补全：
 *     1) 普通补全：按光标前 token 前缀匹配内置函数 / 已定义函数 / 变量
 *     2) 详细补全（`.:` 前缀）：返回完整签名 + 参数说明 + 示例（detail/documentation）
 *     3) 命令补全（`..:` 前缀）与 workspace/executeCommand：开始/结束/设置/索引/帮助
 * - 额外支持 textDocument/hover：鼠标悬停返回函数签名。
 * - shutdown 返回 null、exit 优雅退出；stdin EOF 时也优雅退出。
 */

/* 启用 strdup 等 POSIX 声明（C11 下默认不暴露，需在含 string.h 前定义） */
#define _POSIX_C_SOURCE 200809L

#include "lsp.h"
#include "vus_builtin.h"
#include "yyjson/yyjson.h"
#include "lexer.h"
#include "token.h"
#include "vua_lint.h"   /* .vua 校验闭环：发布诊断（复用 vua.c 严格校验+渲染树归一） */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  /* strncasecmp */

/* LSP CompletionItemKind（LSP 协议标准值） */
#define LSP_KIND_FUNCTION  3
#define LSP_KIND_VARIABLE  6
#define LSP_KIND_EVENT     9

/* LSP SymbolKind（LSP 协议标准值） */
#define LSP_SYM_FUNCTION  12
#define LSP_SYM_VARIABLE  13

/* 已定义的命令表（`..:` 命令） */
static const char *const g_commands[] = { "开始", "结束", "设置", "索引", "帮助" };

/* 命令数量 */
static const int g_commands_count =
    (int)(sizeof(g_commands) / sizeof(g_commands[0]));

/* 进程退出开关：收到 exit 或在主循环读到 EOF 时置位 */
static int g_exit = 0;

/* LSP 是否完成初始化 */
static int g_initialized = 0;

/* VUS 关键字黑名单（普通补全中不作为“已定义符号”收集） */
static const char *const g_keywords[] = {
    "函数", "变量", "如果", "否则", "则", "结束", "对于", "循环", "到",
    "导入", "返回", "真", "假", "空", "并且", "或者", "不是", "共", "步",
    NULL
};

/* ============ 文档缓冲：按 uri 维护最近打开/修改的文档文本 ============ */

#define VUS_LSP_MAX_DOCS 64     /* 最多同时维护的文档数 */
#define VUS_LSP_MAX_SYMS 256    /* 单个文档收集的符号数上限 */

/* 单个已打开文档 */
typedef struct {
    char  uri[512];      /* 去掉 file:// 前缀后的 key */
    char *text;          /* 文档全部文本（动态分配） */
} VusDocBuf;

/* 打开的文档数组 */
static VusDocBuf g_docs[VUS_LSP_MAX_DOCS];
static int g_docs_count = 0;

/* 去掉 uri 的 file:// 前缀，得到缓冲的 key（无非前缀则原样返回字符串） */
static const char *uri_key(const char *uri) {
    if (!uri) return "";
    if (strncmp(uri, "file://", 7) == 0) return uri + 7;
    return uri;
}

/* 按 uri 查找缓冲文本；找不到返回 NULL */
static const char *doc_find_text(const char *uri) {
    if (!uri) return NULL;
    const char *key = uri_key(uri);
    for (int i = 0; i < g_docs_count; i++) {
        if (strcmp(g_docs[i].uri, key) == 0) return g_docs[i].text;
    }
    return NULL;
}

/* 更新/新增文档缓冲（text 会被复制） */
static void doc_update(const char *uri, const char *text) {
    if (!uri || !text) return;
    const char *key = uri_key(uri);
    for (int i = 0; i < g_docs_count; i++) {
        if (strcmp(g_docs[i].uri, key) == 0) {
            char *nb = strdup(text);
            if (!nb) return;
            free(g_docs[i].text);
            g_docs[i].text = nb;
            return;
        }
    }
    /* 新增；若已满则忽略（保持现有文档） */
    if (g_docs_count >= VUS_LSP_MAX_DOCS) return;
    strncpy(g_docs[g_docs_count].uri, key, sizeof(g_docs[g_docs_count].uri) - 1);
    g_docs[g_docs_count].uri[sizeof(g_docs[g_docs_count].uri) - 1] = '\0';
    g_docs[g_docs_count].text = strdup(text);
    if (g_docs[g_docs_count].text) g_docs_count++;
}

/* 关闭文档：移除缓冲占据的槽位 */
static void doc_close(const char *uri) {
    if (!uri) return;
    const char *key = uri_key(uri);
    for (int i = 0; i < g_docs_count; i++) {
        if (strcmp(g_docs[i].uri, key) == 0) {
            free(g_docs[i].text);
            /* 用末尾元素回填，保持数组紧凑 */
            if (g_docs_count > 1) {
                g_docs[i] = g_docs[g_docs_count - 1];
            }
            g_docs_count--;
            return;
        }
    }
}

/* ============ 符号表：用词法分析器精确收集的文档符号 ============ */

/* 符号内部类别（用于换算 LSP / CompletionItemKind） */
enum { VUS_SYM_VARIABLE = 0, VUS_SYM_FUNCTION = 1 };

/* 单个带位置的符号（行/列为 0 起步，供 documentSymbol 使用） */
typedef struct {
    char name[128];
    int  kind;         /* VUS_SYM_VARIABLE / VUS_SYM_FUNCTION */
    int  line;         /* 0 起步 */
    int  column;       /* 0 起步 */
    int  end_line;     /* 0 起步 */
    int  end_column;   /* 0 起步 */
} VusSymbol;

/* ============ 工具：字节是否为标识符字符（含 UTF-8 多字节与下划线） ============ */
static int is_ident_byte(unsigned char c) {
    return (c >= 0x80) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* ============ LSP 传输层：读一行（不含换行），EOF 返回 -1 ============ */
static int lsp_read_line(char *buf, size_t size) {
    size_t i = 0;
    int c;
    while (i + 1 < size) {
        c = getchar();
        if (c == EOF) return -1;
        if (c == '\n') { buf[i] = '\0'; return (int)i; }
        buf[i++] = (char)c;
    }
    buf[i] = '\0';
    return (int)i;
}

/* 读取并解析一条 LSP 消息体；成功返回 1，EOF 返回 0，异常返回 -1 */
static int lsp_read_message(char **body, size_t *length) {
    int content_length = -1;

    /* 读取头区域直到空行 */
    while (1) {
        char line[256];
        int n = lsp_read_line(line, sizeof(line));
        if (n < 0) {
            /* 读到 EOF：若已有部分头但没有完整消息则视为干净退出 */
            return 0;
        }
        if (line[0] == '\r' || line[0] == '\0') break; /* 空行，头结束 */
        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            content_length = atoi(line + 15);
        }
    }

    if (content_length <= 0) return -1;

    char *buf = (char *)malloc((size_t)content_length + 1);
    if (!buf) return -1;

    size_t got = 0;
    while (got < (size_t)content_length) {
        size_t n = fread(buf + got, 1, (size_t)content_length - got, stdin);
        if (n == 0) {
            free(buf);
            return -1; /* 不足一个完整消息 */
        }
        got += n;
    }
    buf[content_length] = '\0';
    *body = buf;
    *length = (size_t)content_length;
    return 1;
}

/* 发送一条 JSON-RPC 消息（Content-Length 分帧到 stdout） */
static void lsp_send_json(const char *json) {
    printf("Content-Length: %d\r\n\r\n%s", (int)strlen(json), json);
    fflush(stdout);
}

/* 向对象添加字符串字段。
 * yyjson_mut_obj_add_str() 只是“包装”传入指针而不复制，若传入的是
 * 函数内栈上缓冲区（如整理的 insertText/documentation）将在序列化时失效，
 * 造成 use-after-return 使 yyjson_mut_write 返回 NULL。这里统一用
 * yyjson_mut_strncpy() 复制后再加入，保证字段内容在整个 doc 生命周期内有效。 */
static void obj_add_str(yyjson_mut_doc *doc, yyjson_mut_val *obj,
                        const char *key, const char *val) {
    yyjson_mut_val *v = yyjson_mut_strncpy(doc, val ? val : "", (size_t)strlen(val ? val : ""));
    yyjson_mut_obj_add_val(doc, obj, key, v);
}

/* ============ 请求参数读取 ============ */

/* 从参数中读取文本内容（textDocument.text），可能为 NULL */
static const char *req_document_text(yyjson_val *params) {
    yyjson_val *td = params ? yyjson_obj_get(params, "textDocument") : NULL;
    if (!td) return NULL;
    yyjson_val *txt = yyjson_obj_get(td, "text");
    if (txt && yyjson_is_str(txt)) return yyjson_get_str(txt);
    return NULL;
}

static int req_position_line(yyjson_val *params) {
    yyjson_val *pos = params ? yyjson_obj_get(params, "position") : NULL;
    yyjson_val *v = pos ? yyjson_obj_get(pos, "line") : NULL;
    return (v && yyjson_is_int(v)) ? (int)yyjson_get_int(v) : 0;
}

static int req_position_character(yyjson_val *params) {
    yyjson_val *pos = params ? yyjson_obj_get(params, "position") : NULL;
    yyjson_val *v = pos ? yyjson_obj_get(pos, "character") : NULL;
    return (v && yyjson_is_int(v)) ? (int)yyjson_get_int(v) : 0;
}

/* 取出参数指向的行（按行号），写到 buf；找不到返回 NULL */
static const char *extract_line(const char *text, int line, char *buf, size_t size) {
    if (!text) return NULL;
    int cur = 0;
    const char *start = text;
    while (*text) {
        if (*text == '\n') {
            if (cur == line) {
                size_t len = (size_t)(text - start);
                if (len >= size) len = size - 1;
                memcpy(buf, start, len);
                buf[len] = '\0';
                return buf;
            }
            cur++;
            start = text + 1;
        } else if (*text == '\r') {
            if (cur == line) {
                size_t len = (size_t)(text - start);
                if (len >= size) len = size - 1;
                memcpy(buf, start, len);
                buf[len] = '\0';
                return buf;
            }
        }
        text++;
    }
    if (cur == line) {
        size_t len = (size_t)(text - start);
        if (len >= size) len = size - 1;
        memcpy(buf, start, len);
        buf[len] = '\0';
        return buf;
    }
    return NULL;
}

/* 取光标所在行的“光标前文本”（按字节截断到 character） */
static void get_line_prefix(const char *text, int line, int character,
                            char *out, size_t size) {
    char linebuf[4096];
    if (!extract_line(text, line, linebuf, sizeof(linebuf))) {
        out[0] = '\0';
        return;
    }
    int n = (int)strlen(linebuf);
    if (character < n) n = character;
    if (n < 0) n = 0;
    if ((size_t)n >= size) n = (int)size - 1;
    memcpy(out, linebuf, (size_t)n);
    out[n] = '\0';
}

/* ============ 补全模式判定 ============ */

enum { MODE_NORMAL = 0, MODE_DETAIL, MODE_COMMAND };

/* 由光标前文本判定补全模式，并把光标前的“目标 token”写入 token */
static int classify_mode(const char *prefix, char *token, size_t token_size) {
    token[0] = '\0';
    if (!prefix) return MODE_NORMAL;

    /* 命令模式：以 `..:` 开头 */
    if (prefix[0] == '.' && prefix[1] == '.' && prefix[2] == ':') {
        /* 取 `..:` 之后的全部文本，命令名为末尾标识符 token */
        const char *rest = prefix + 3;
        /* 找到末尾标识符起点 */
        const char *p = rest + strlen(rest) - 1;
        while (p >= rest && (*p == ' ' || *p == '\t')) p--;
        const char *end = p + 1;
        while (p >= rest && is_ident_byte((unsigned char)*p)) p--;
        p++;
        size_t len = (size_t)(end - p);
        if (len > 0) {
            if (len >= token_size) len = token_size - 1;
            memcpy(token, p, len);
            token[len] = '\0';
        }
        return MODE_COMMAND;
    }

    /* 详细模式：以 `.:` 开头 */
    if (prefix[0] == '.' && prefix[1] == ':') {
        const char *rest = prefix + 2;
        if (strlen(rest) >= token_size) return MODE_DETAIL; /* 过长，置空 token */
        strcpy(token, rest);
        return MODE_DETAIL;
    }

    /* 普通模式：末尾标识符 token（含中文/下划线） */
    const char *p = prefix + strlen(prefix) - 1;
    while (p >= prefix && is_ident_byte((unsigned char)*p)) p--;
    p++;
    size_t len = (size_t)(prefix + strlen(prefix) - p);
    if (len >= token_size) len = token_size - 1;
    memcpy(token, p, len);
    token[len] = '\0';
    return MODE_NORMAL;
}

/* ============ 已定义符号收集（普通补全用） ============ */

static int name_is_keyword(const char *name) {
    if (vus_builtin_find(name)) return 1;
    for (int i = 0; g_keywords[i]; i++) {
        if (strcmp(g_keywords[i], name) == 0) return 1;
    }
    return 0;
}

/* 收集文档中“已定义”的函数名与变量名。逐行按分隔符切词启发式识别。 */
static void collect_defined(const char *text, char names[][128],
                            int *count, int max_count) {
    char linebuf[4096];
    int line = 0;
    while (text && text[0]) {
        if (!extract_line(text, line, linebuf, sizeof(linebuf))) break;

        /* 按分隔符切词 */
        char words[64][128];
        int nw = 0;
        char tmp[4096];
        strcpy(tmp, linebuf);
        char *tok = strtok(tmp, " \t(),;:=\"'+-*/<>[]{}");
        while (tok && nw < 64) {
            snprintf(words[nw], sizeof(words[nw]), "%s", tok);
            nw++;
            tok = strtok(NULL, " \t(),;:=\"'+-*/<>[]{}");
        }

        /* 识别：`函数 名` / `变量 名` 定义 */
        if (nw >= 2 && (strcmp(words[0], "函数") == 0 || strcmp(words[0], "变量") == 0)) {
            if (!name_is_keyword(words[1]) && *count < max_count) {
                int dup = 0;
                for (int k = 0; k < *count; k++)
                    if (strcmp(names[k], words[1]) == 0) { dup = 1; break; }
                if (!dup) {
                    snprintf(names[*count], sizeof(names[*count]), "%s", words[1]);
                    (*count)++;
                }
            }
        } else if (nw >= 3 && strcmp(words[1], "=") == 0) {
            /* 赋值：`名 = ...` 视为变量定义 */
            if (!name_is_keyword(words[0]) && *count < max_count) {
                int dup = 0;
                for (int k = 0; k < *count; k++)
                    if (strcmp(names[k], words[0]) == 0) { dup = 1; break; }
                if (!dup) {
                    snprintf(names[*count], sizeof(names[*count]), "%s", words[0]);
                    (*count)++;
                }
            }
        }
        line++;
    }
}

/* 向符号表追加一个符号；重名（已存在同名）时跳过，返回是否真正加入 */
static void symbol_add(VusSymbol *out, int *count, int max,
                       const char *name, size_t nlen, int kind,
                       int line, int column) {
    if (*count >= max) return;
    /* 去重：同名符号只保留第一个 */
    for (int i = 0; i < *count; i++) {
        if (strlen(out[i].name) == nlen &&
            strncmp(out[i].name, name, nlen) == 0) return;
    }
    if (nlen >= sizeof(out[*count].name)) nlen = sizeof(out[*count].name) - 1;
    memcpy(out[*count].name, name, nlen);
    out[*count].name[nlen] = '\0';
    out[*count].kind = kind;
    out[*count].line = line;
    out[*count].column = column;
    /* 结束位置：名称覆盖范围（该名所在行、起始列起算长度），行同为起始行 */
    out[*count].end_line = line;
    out[*count].end_column = column + (int)nlen;
    (*count)++;
}

/* 仅含名称、无位置的符号（用于失败回退） */
static void symbol_add_name(VusSymbol *out, int *count, int max,
                            const char *name, int kind) {
    symbol_add(out, count, max, name, strlen(name), kind, 0, 0);
}

/*
 * 用 VUS 词法分析器对文档精确收集符号。成功返回 1；tokenize 失败（lexer
 * 报错）或为空时返回 0，交由调用方回退启发式，保证不崩溃。
 * 函数定义：`定义 名` / `def 名`（关键字 DEF/CN_DEF 之后紧跟标识符）→ Function。
 * 变量定义：`名 = 表达式` 且名是标识符，排除属性访问（对象.名）与下标（名[）。
 */
static int collect_symbols_lexer(const char *text, VusSymbol *out,
                                 int *count, int max) {
    *count = 0;
    if (!text || text[0] == '\0') return 0; /* 空文档不收集 */

    size_t slen = strlen(text);
    size_t ntok = 0;
    VusLexer *lx = vus_lexer_new(text, slen);
    if (!lx) return 0;
    VusToken *toks = vus_lexer_tokenize(lx, &ntok);
    if (lx->error || !toks || ntok == 0) {
        /* tokenize 失败：回退（调用方负责启发式） */
        if (toks) vus_lexer_free_tokens(toks, ntok);
        vus_lexer_free(lx);
        return 0;
    }

    for (size_t i = 0; i < ntok; i++) {
        VusTokenType ty = toks[i].type;

        /* 函数定义：`定义 名` / `def 名`，紧跟标识符 */
        if ((ty == VUS_TOKEN_CN_DEF || ty == VUS_TOKEN_DEF) &&
            i + 1 < ntok && toks[i + 1].type == VUS_TOKEN_IDENTIFIER) {
            VusToken fn = toks[i + 1];
            symbol_add(out, count, max, fn.start, fn.length,
                       VUS_SYM_FUNCTION, fn.line - 1, fn.column - 1);
            i++; /* 跳过函数名，加快遍历 */
            continue;
        }

        /* 变量定义/赋值：标识符紧跟赋值号 `=` */
        if (ty == VUS_TOKEN_IDENTIFIER && i + 1 < ntok &&
            toks[i + 1].type == VUS_TOKEN_ASSIGN) {
            /* 排除属性访问：前面是点号，如 `对象.名 = ...` */
            if (i > 0 && toks[i - 1].type == VUS_TOKEN_DOT) continue;
            /* 标识符 token 已保证不是关键字（关键字会被识别为专门类型）；
               下标（名[）因为赋值号不紧随标识符，也自然被排除。 */
            symbol_add(out, count, max, toks[i].start, toks[i].length,
                       VUS_SYM_VARIABLE, toks[i].line - 1, toks[i].column - 1);
            continue;
        }
    }

    vus_lexer_free_tokens(toks, ntok);
    vus_lexer_free(lx);
    return 1; /* tokenize 成功（即便未收集到符号） */
}

/* 回退：用原有启发式收集符号（一律视为变量、无位置） */
static void collect_symbols_fallback(const char *text, VusSymbol *out,
                                     int *count, int max) {
    char names[64][128];
    int n = 0;
    collect_defined(text, names, &n, 64);
    for (int i = 0; i < n; i++)
        symbol_add_name(out, count, max, names[i], VUS_SYM_VARIABLE);
}

/*
 * 符号收集主入口：优先取 uri 对应缓冲文本，否则用传入文本；
 * 先用词法分析器精确分类，失败时回退启发式。
 */
static void collect_symbols(const char *text, const char *uri,
                            VusSymbol *out, int *count, int max) {
    const char *src = NULL;
    if (uri) src = doc_find_text(uri);   /* 优先已打开缓冲 */
    if (!src) src = text;                /* 退而请求内文本 */
    if (!collect_symbols_lexer(src, out, count, max))
        collect_symbols_fallback(src, out, count, max);
}

/* 从请求参数取 textDocument.uri；无则返回 NULL */
static const char *req_document_uri(yyjson_val *params) {
    yyjson_val *td = params ? yyjson_obj_get(params, "textDocument") : NULL;
    if (!td) return NULL;
    yyjson_val *u = yyjson_obj_get(td, "uri");
    if (u && yyjson_is_str(u)) return yyjson_get_str(u);
    return NULL;
}

/* 解析当前文档文本：优先 uri 对应缓冲，其次请求内 textDocument.text */
static const char *resolve_text(yyjson_val *params, const char *uri) {
    if (uri) {
        const char *t = doc_find_text(uri);
        if (t) return t;
    }
    return req_document_text(params);
}

/* ============ 构造 Markdown 文档（doc + 示例） ============ */
static void build_markdown(const VusBuiltin *b, char *out, size_t size) {
    snprintf(out, size, "%s\n\n**示例：**\n```vus\n%s\n```", b->doc, b->example);
}

/* ============ 响应构造：请求 id 克隆 ============ */

static yyjson_mut_doc *new_response_doc(yyjson_val *req) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    obj_add_str(doc, root, "jsonrpc", "2.0");

    /* 复制请求的 id（若存在） */
    yyjson_val *idv = req ? yyjson_obj_get(req, "id") : NULL;
    if (idv) {
        yyjson_mut_val *mid = NULL;
        if (yyjson_is_str(idv)) mid = yyjson_mut_str(doc, yyjson_get_str(idv));
        else if (yyjson_is_int(idv)) mid = yyjson_mut_int(doc, (int64_t)yyjson_get_int(idv));
        else if (yyjson_is_real(idv)) mid = yyjson_mut_real(doc, yyjson_get_real(idv));
        else mid = yyjson_mut_null(doc);
        yyjson_mut_obj_add_val(doc, root, "id", mid);
    }
    return doc;
}

static void send_doc(yyjson_mut_doc *doc) {
    size_t len = 0;
    const char *json = yyjson_mut_write(doc, 0, &len);
    lsp_send_json(json ? json : "{}");
    free((void *)json);
    yyjson_mut_doc_free(doc);
}

/* ============ .vua 校验闭环（publishDiagnostics） ============ */

static int uri_is_vua(const char *uri) {
    if (!uri) return 0;
    size_t n = strlen(uri);
    return n >= 4 && strcmp(uri + n - 4, ".vua") == 0;
}

/* 构造 publishDiagnostics 通知并发送（diags 内为空数组则清空诊断） */
static void send_publish_diagnostics(const char *uri, int have_err,
                                     const VuaError *errs) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    obj_add_str(doc, root, "jsonrpc", "2.0");
    obj_add_str(doc, root, "method", "textDocument/publishDiagnostics");
    yyjson_mut_val *params = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, root, "params", params);
    obj_add_str(doc, params, "uri", uri);
    yyjson_mut_val *diags = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, params, "diagnostics", diags);

    const VuaError *e = errs;
    for (int n = 0; have_err && e && n < 32; e = e->next, n++) {
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_arr_append(diags, item);
        yyjson_mut_val *range = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_val(doc, item, "range", range);
        int line = e->line > 0 ? e->line - 1 : 0;   /* VuaError.line 为 1 起步；0=未知 */
        yyjson_mut_val *start = yyjson_mut_obj(doc);
        yyjson_mut_val *end = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_val(doc, range, "start", start);
        yyjson_mut_obj_add_val(doc, range, "end", end);
        yyjson_mut_obj_add_int(doc, start, "line", line);
        yyjson_mut_obj_add_int(doc, start, "character", 0);
        yyjson_mut_obj_add_int(doc, end, "line", line);
        yyjson_mut_obj_add_int(doc, end, "character", 1);
        yyjson_mut_obj_add_int(doc, item, "severity", 1);   /* 1 = Error */
        obj_add_str(doc, item, "source", "vua-lint");
        yyjson_mut_obj_add_int(doc, item, "code", e->code);
        obj_add_str(doc, item, "message", e->msg);
    }
    send_doc(doc);
}

/* 校验 .vua 文本并发布诊断（didOpen/didChange/didSave 后调用） */
static void publish_vua_diagnostics(const char *uri, const char *text) {
    if (!uri_is_vua(uri)) return;
    vua_lint_ensure_catalog(NULL, NULL, 0);   /* 进程级一次，尽力加载控件表 */
    VuaError err;
    memset(&err, 0, sizeof(err));
    int rc = vua_lint_text(text ? text : "", uri, &err, NULL);  /* 0=通过, -1=失败 */
    send_publish_diagnostics(uri, rc != 0, rc == 0 ? NULL : &err);
}

/* 关闭 .vua 文档时清空诊断 */
static void clear_vua_diagnostics(const char *uri) {
    if (!uri_is_vua(uri)) return;
    send_publish_diagnostics(uri, 0, NULL);
}

/* ============ 补全项追加 ============ */

static void append_builtin_item(yyjson_mut_doc *doc, yyjson_mut_val *arr,
                                const VusBuiltin *b, const char *typed,
                                int detailed) {
    yyjson_mut_val *it = yyjson_mut_obj(doc);
    obj_add_str(doc, it, "label", b->name);
    yyjson_mut_obj_add_int(doc, it, "kind", LSP_KIND_FUNCTION);

    if (detailed) {
        /* 详细补全：complete = 签名；documentation = 说明 + 示例 */
        obj_add_str(doc, it, "detail", b->signature);
        char md[1024];
        build_markdown(b, md, sizeof(md));
        obj_add_str(doc, it, "documentation", md);
    }

    /* insertText：去除已输入前缀后的剩余部分，便于就地补全 */
    if (typed && typed[0]) {
        int tlen = (int)strlen(typed);
        int nlen = (int)strlen(b->name);
        if (tlen <= nlen) {
            char rest[256];
            snprintf(rest, sizeof(rest), "%s", b->name + tlen);
            obj_add_str(doc, it, "insertText", rest);
        }
    }
    yyjson_mut_arr_append(arr, it);
}

static void append_defined_item(yyjson_mut_doc *doc, yyjson_mut_val *arr,
                                const char *name, const char *typed,
                                int kind) {
    yyjson_mut_val *it = yyjson_mut_obj(doc);
    obj_add_str(doc, it, "label", name);
    yyjson_mut_obj_add_int(doc, it, "kind", kind);
    if (typed && typed[0]) {
        int tlen = (int)strlen(typed);
        int nlen = (int)strlen(name);
        if (tlen <= nlen) {
            char rest[256];
            snprintf(rest, sizeof(rest), "%s", name + tlen);
            obj_add_str(doc, it, "insertText", rest);
        }
    }
    yyjson_mut_arr_append(arr, it);
}

static void append_command_item(yyjson_mut_doc *doc, yyjson_mut_val *arr,
                                const char *command, const char *typed) {
    yyjson_mut_val *it = yyjson_mut_obj(doc);
    obj_add_str(doc, it, "label", command);
    yyjson_mut_obj_add_int(doc, it, "kind", LSP_KIND_EVENT);
    obj_add_str(doc, it, "detail", "VUS 命令: 执行");

    /* 由服务端计算 diff：去掉已输入前置 `..:执行 ` / token 部分 */
    if (typed && typed[0]) {
        int tlen = (int)strlen(typed);
        int nlen = (int)strlen(command);
        if (tlen <= nlen) {
            char rest[256];
            snprintf(rest, sizeof(rest), "%s", command + tlen);
            obj_add_str(doc, it, "insertText", rest);
        }
    }
    yyjson_mut_arr_append(arr, it);
}

/* ============ 普通补全：内置函数 + 文档符号（去重，内置优先） ============ */
static void handle_normal(yyjson_mut_doc *doc, yyjson_mut_val *items,
                          const char *text, const char *uri,
                          const char *token) {
    const VusBuiltin *tb = vus_builtin_table();
    for (int i = 0; i < vus_builtin_count(); i++) {
        if (token[0] == '\0' || strncmp(tb[i].name, token, strlen(token)) == 0)
            append_builtin_item(doc, items, &tb[i], token, 0);
    }

    /* 文档符号：函数 kind=3、变量 kind=6；同名时内置优先，故符号跳过 */
    VusSymbol syms[VUS_LSP_MAX_SYMS];
    int ns = 0;
    collect_symbols(text, uri, syms, &ns, VUS_LSP_MAX_SYMS);
    for (int i = 0; i < ns; i++) {
        if (token[0] != '\0' &&
            strncmp(syms[i].name, token, strlen(token)) != 0) continue;
        /* 与内置同名：跳过（内置优先） */
        if (vus_builtin_find(syms[i].name)) continue;
        int ckind = (syms[i].kind == VUS_SYM_FUNCTION) ? LSP_KIND_FUNCTION : LSP_KIND_VARIABLE;
        append_defined_item(doc, items, syms[i].name, token, ckind);
    }
}

/* ============ 详细补全（`.:`）：完整签名 + 说明 + 示例 ============ */
static void handle_detail(yyjson_mut_doc *doc, yyjson_mut_val *items,
                          const char *token) {
    const VusBuiltin *b = vus_builtin_find(token);
    if (b) {
        /* `.:` 后是完全匹配：详细展示该函数并列出全部候选 */
        append_builtin_item(doc, items, b, token, 1);
        /* 已展示精确项后，补充其余候选（供选择） */
        const VusBuiltin *tb = vus_builtin_table();
        for (int i = 0; i < vus_builtin_count(); i++) {
            if (&tb[i] != b && strncmp(tb[i].name, token, strlen(token)) == 0)
                append_builtin_item(doc, items, &tb[i], token, 0);
        }
        return;
    }

    /* 无精确匹配：按前缀列出候选（带签名 detail）；token 为空则列出全部 */
    const VusBuiltin *tb = vus_builtin_table();
    for (int i = 0; i < vus_builtin_count(); i++) {
        if (token[0] == '\0' || strncmp(tb[i].name, token, strlen(token)) == 0)
            append_builtin_item(doc, items, &tb[i], token, 1);
    }
}

/* ============ 命令补全（`..:`） + workspace/executeCommand ============ */
static void handle_command(yyjson_mut_doc *doc, yyjson_mut_val *items,
                           const char *token) {
    for (int i = 0; i < g_commands_count; i++) {
        if (token[0] == '\0' || strncmp(g_commands[i], token, strlen(token)) == 0)
            append_command_item(doc, items, g_commands[i], token);
    }
}

/* ============ 各方法处理器 ============ */

static void handle_initialize(yyjson_val *req) {
    g_initialized = 1;
    yyjson_mut_doc *doc = new_response_doc(req);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    yyjson_mut_val *res = yyjson_mut_obj(doc);

    yyjson_mut_val *caps = yyjson_mut_obj(doc);
    yyjson_mut_val *cp = yyjson_mut_obj(doc);
    yyjson_mut_val *trig = yyjson_mut_arr(doc);
    yyjson_mut_arr_append(trig, yyjson_mut_str(doc, "图形"));
    yyjson_mut_arr_append(trig, yyjson_mut_str(doc, "."));
    yyjson_mut_obj_add_val(doc, cp, "triggerCharacters", trig);
    yyjson_mut_obj_add_val(doc, caps, "completionProvider", cp);

    yyjson_mut_val *ecp = yyjson_mut_obj(doc);
    yyjson_mut_val *cmds = yyjson_mut_arr(doc);
    yyjson_mut_arr_append(cmds, yyjson_mut_str(doc, "vus.executeCommand"));
    yyjson_mut_obj_add_val(doc, ecp, "commands", cmds);
    yyjson_mut_obj_add_val(doc, caps, "executeCommandProvider", ecp);
    yyjson_mut_obj_add_val(doc, caps, "documentSymbolProvider", yyjson_mut_true(doc));

    yyjson_mut_obj_add_val(doc, res, "capabilities", caps);

    yyjson_mut_val *info = yyjson_mut_obj(doc);
    obj_add_str(doc, info, "name", "vus-lsp");
    obj_add_str(doc, info, "version", "0.1.0");
    yyjson_mut_obj_add_val(doc, res, "serverInfo", info);

    yyjson_mut_obj_add_val(doc, root, "result", res);
    send_doc(doc);
}

static void handle_completion(yyjson_val *req) {
    yyjson_val *params = yyjson_obj_get(req, "params");
    if (!params || !yyjson_is_obj(params)) {
        yyjson_mut_doc *doc = new_response_doc(req);
        yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
        yyjson_mut_val *res = yyjson_mut_obj(doc);
        yyjson_mut_val *items = yyjson_mut_arr(doc);
        yyjson_mut_obj_add_val(doc, res, "isIncomplete", yyjson_mut_false(doc));
        yyjson_mut_obj_add_val(doc, res, "items", items);
        yyjson_mut_obj_add_val(doc, root, "result", res);
        send_doc(doc);
        return;
    }

    const char *uri = req_document_uri(params);
    const char *text = resolve_text(params, uri); /* 优先取缓冲 */
    int line = req_position_line(params);
    int character = req_position_character(params);

    char prefix[4096];
    get_line_prefix(text, line, character, prefix, sizeof(prefix));

    char token[256];
    int mode = classify_mode(prefix, token, sizeof(token));

    yyjson_mut_doc *doc = new_response_doc(req);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    yyjson_mut_val *res = yyjson_mut_obj(doc);
    yyjson_mut_val *items = yyjson_mut_arr(doc);

    switch (mode) {
        case MODE_COMMAND: handle_command(doc, items, token); break;
        case MODE_DETAIL:  handle_detail(doc, items, token);  break;
        default:           handle_normal(doc, items, text, uri, token); break;
    }

    yyjson_mut_obj_add_val(doc, res, "isIncomplete", yyjson_mut_false(doc));
    yyjson_mut_obj_add_val(doc, res, "items", items);
    yyjson_mut_obj_add_val(doc, root, "result", res);
    send_doc(doc);
}

/* ============ documentSymbol：返回文档符号列表（带范围） ============ */
static void handle_document_symbol(yyjson_val *req) {
    yyjson_val *params = yyjson_obj_get(req, "params");
    const char *uri = req_document_uri(params);
    const char *text = req_document_text(params);

    VusSymbol syms[VUS_LSP_MAX_SYMS];
    int ns = 0;
    collect_symbols(text, uri, syms, &ns, VUS_LSP_MAX_SYMS);

    yyjson_mut_doc *doc = new_response_doc(req);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    yyjson_mut_val *arr = yyjson_mut_arr(doc);

    for (int i = 0; i < ns; i++) {
        yyjson_mut_val *it = yyjson_mut_obj(doc);
        obj_add_str(doc, it, "name", syms[i].name);
        yyjson_mut_obj_add_int(doc, it, "kind",
                               syms[i].kind == VUS_SYM_FUNCTION ? LSP_SYM_FUNCTION : LSP_SYM_VARIABLE);

        yyjson_mut_val *rg = yyjson_mut_obj(doc);
        yyjson_mut_val *st = yyjson_mut_obj(doc);
        yyjson_mut_val *en = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_int(doc, st, "line", syms[i].line);
        yyjson_mut_obj_add_int(doc, st, "character", syms[i].column);
        yyjson_mut_obj_add_int(doc, en, "line", syms[i].end_line);
        yyjson_mut_obj_add_int(doc, en, "character", syms[i].end_column);
        yyjson_mut_obj_add_val(doc, rg, "start", st);
        yyjson_mut_obj_add_val(doc, rg, "end", en);
        yyjson_mut_obj_add_val(doc, it, "range", rg);
        yyjson_mut_arr_append(arr, it);
    }

    yyjson_mut_obj_add_val(doc, root, "result", arr);
    send_doc(doc);
}

static void handle_execute_command(yyjson_val *req) {
    yyjson_val *params = yyjson_obj_get(req, "params");
    const char *command = NULL;
    if (params) {
        yyjson_val *cv = yyjson_obj_get(params, "command");
        if (cv && yyjson_is_str(cv)) command = yyjson_get_str(cv);
    }
    if (!command) command = "(未知)";

    /* 打印执行意图到 stdout（最小实现） */
    printf("[vus-lsp] 执行命令: %s\n", command);
    fflush(stdout);

    yyjson_mut_doc *doc = new_response_doc(req);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    yyjson_mut_obj_add_val(doc, root, "result", yyjson_mut_null(doc));
    send_doc(doc);
}

static void handle_hover(yyjson_val *req) {
    yyjson_val *params = yyjson_obj_get(req, "params");
    yyjson_mut_doc *doc = new_response_doc(req);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);

    yyjson_mut_val *result = yyjson_mut_null(doc);
    if (params) {
        const char *uri = req_document_uri(params);
        const char *text = resolve_text(params, uri);
        int line = req_position_line(params);
        int character = req_position_character(params);

        char prefix[4096];
        get_line_prefix(text, line, character, prefix, sizeof(prefix));

        /* 取光标处（光标前）的标识符 token */
        char token[256];
        token[0] = '\0';
        const char *p = prefix + strlen(prefix) - 1;
        while (p >= prefix && is_ident_byte((unsigned char)*p)) p--;
        p++;
        size_t len = (size_t)(prefix + strlen(prefix) - p);
        if (len > 0 && len < sizeof(token)) {
            memcpy(token, p, len);
            token[len] = '\0';
        }

        char md[1024];
        const VusBuiltin *b = vus_builtin_find(token);
        if (b) {
            build_markdown(b, md, sizeof(md));
            result = yyjson_mut_obj(doc);
            yyjson_mut_val *content = yyjson_mut_obj(doc);
            obj_add_str(doc, content, "kind", "markdown");
            obj_add_str(doc, content, "value", md);
            yyjson_mut_obj_add_val(doc, result, "contents", content);
        } else if (token[0]) {
            /* 内置未命中：查文档符号表，函数命中时返回其签名（名称+类别） */
            VusSymbol syms[VUS_LSP_MAX_SYMS];
            int ns = 0;
            collect_symbols(text, uri, syms, &ns, VUS_LSP_MAX_SYMS);
            for (int i = 0; i < ns; i++) {
                if (strcmp(syms[i].name, token) == 0) {
                    snprintf(md, sizeof(md),
                             "%s `%s`：用户定义的%s",
                             syms[i].kind == VUS_SYM_FUNCTION ? "函数" : "变量",
                             syms[i].name,
                             syms[i].kind == VUS_SYM_FUNCTION ? "函数" : "变量");
                    result = yyjson_mut_obj(doc);
                    yyjson_mut_val *content = yyjson_mut_obj(doc);
                    obj_add_str(doc, content, "kind", "markdown");
                    obj_add_str(doc, content, "value", md);
                    yyjson_mut_obj_add_val(doc, result, "contents", content);
                    break;
                }
            }
        }
    }

    yyjson_mut_obj_add_val(doc, root, "result", result);
    send_doc(doc);
}

/* ============ 主循环与分发 ============ */

static void process_request(yyjson_val *req) {
    yyjson_val *mv = yyjson_obj_get(req, "method");
    if (!mv || !yyjson_is_str(mv)) return;
    const char *method = yyjson_get_str(mv);

    if (strcmp(method, "initialize") == 0) {
        handle_initialize(req);
    } else if (strcmp(method, "textDocument/completion") == 0) {
        handle_completion(req);
    } else if (strcmp(method, "textDocument/documentSymbol") == 0) {
        handle_document_symbol(req);
    } else if (strcmp(method, "workspace/executeCommand") == 0) {
        handle_execute_command(req);
    } else if (strcmp(method, "textDocument/hover") == 0) {
        handle_hover(req);
    } else if (strcmp(method, "shutdown") == 0) {
        /* 返回 null；之后 client 会发 exit */
        yyjson_mut_doc *doc = new_response_doc(req);
        yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
        yyjson_mut_obj_add_val(doc, root, "result", yyjson_mut_null(doc));
        send_doc(doc);
    } else if (strcmp(method, "exit") == 0) {
        g_exit = 1; /* 通知：无响应 */
    } else if (strcmp(method, "textDocument/didOpen") == 0 ||
               strcmp(method, "textDocument/didChange") == 0 ||
               strcmp(method, "textDocument/didSave") == 0) {
        /* 文档同步通知：维护 uri 缓冲，供补全 / documentSymbol 取文本 */
        yyjson_val *params = yyjson_obj_get(req, "params");
        const char *uri = req_document_uri(params);
        if (!uri) return;
        if (strcmp(method, "textDocument/didChange") == 0) {
            /* 全量替换：取 contentChanges[0].text（改动总是全量时最稳）；
               若该结果是增量（带 range 且无 text），暂不合并，保持原文档。 */
            const char *text = NULL;
            yyjson_val *cc = params ? yyjson_obj_get(params, "contentChanges") : NULL;
            if (cc && yyjson_is_arr(cc)) {
                for (size_t i = 0; i < (size_t)yyjson_arr_size(cc); i++) {
                    yyjson_val *ch = yyjson_arr_get(cc, i);
                    yyjson_val *tx = ch ? yyjson_obj_get(ch, "text") : NULL;
                    if (tx && yyjson_is_str(tx)) { text = yyjson_get_str(tx); break; }
                }
            }
            if (text) doc_update(uri, text);
        } else {
            /* didOpen / didSave：直接用请求内 textDocument.text 刷新 */
            const char *text = req_document_text(params);
            if (text) doc_update(uri, text);
        }
        /* .vua 校验闭环：文档打开/变更/保存后发布诊断 */
        publish_vua_diagnostics(uri, doc_find_text(uri));
    } else if (strcmp(method, "textDocument/didClose") == 0) {
        /* 关闭文档：释放缓冲并清空 .vua 诊断 */
        yyjson_val *params = yyjson_obj_get(req, "params");
        const char *uri = req_document_uri(params);
        clear_vua_diagnostics(uri);
        doc_close(uri);
    } else if (strcmp(method, "$/cancelRequest") == 0) {
        /* 忽略取消请求 */
    } else {
        /* 未知方法：若有 id 则返回错误，否则忽略（通知） */
        if (yyjson_obj_get(req, "id")) {
            yyjson_mut_doc *doc = new_response_doc(req);
            yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
            yyjson_mut_val *err = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_int(doc, err, "code", -32601);
            obj_add_str(doc, err, "message", "Method not found");
            yyjson_mut_obj_add_val(doc, root, "error", err);
            send_doc(doc);
        }
    }
}

/* LSP 服务器主入口 */
int vus_lsp_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    /* 设置 stdin/stdout 为二进制模式（行缓冲），保证 Content-Length 分帧正确 */
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    while (!g_exit) {
        char *body = NULL;
        size_t length = 0;
        int rc = lsp_read_message(&body, &length);
        if (rc == 0) break;        /* stdin EOF，优雅退出 */
        if (rc < 0) break;         /* 协议损坏，退出 */

        yyjson_doc *jdoc = yyjson_read(body, length, 0);
        free(body);
        if (!jdoc) continue;

        yyjson_val *root = yyjson_doc_get_root(jdoc);
        if (root && yyjson_is_obj(root)) process_request(root);

        yyjson_doc_free(jdoc);
    }

    return 0;
}