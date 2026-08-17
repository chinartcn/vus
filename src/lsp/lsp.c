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

#include "lsp.h"
#include "vus_builtin.h"
#include "yyjson/yyjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  /* strncasecmp */

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
            strncpy(words[nw], tok, 127);
            words[nw][127] = '\0';
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
                    strncpy(names[*count], words[1], 127);
                    names[*count][127] = '\0';
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
                    strncpy(names[*count], words[0], 127);
                    names[*count][127] = '\0';
                    (*count)++;
                }
            }
        }
        line++;
    }
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

/* ============ 补全项追加 ============ */

static void append_builtin_item(yyjson_mut_doc *doc, yyjson_mut_val *arr,
                                const VusBuiltin *b, const char *typed,
                                int detailed) {
    yyjson_mut_val *it = yyjson_mut_obj(doc);
    obj_add_str(doc, it, "label", b->name);
    /* kind: 函数 = 3 */
    yyjson_mut_obj_add_int(doc, it, "kind", 3);

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
    /* kind: 函数=3，变量=6 */
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
    /* 命令/事件：kind = 9 (Event) */
    yyjson_mut_obj_add_int(doc, it, "kind", 9);
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

/* ============ 普通补全：内置函数 + 已定义符号 ============ */
static void handle_normal(yyjson_mut_doc *doc, yyjson_mut_val *items,
                          const char *text, const char *token) {
    const VusBuiltin *tb = vus_builtin_table();
    for (int i = 0; i < vus_builtin_count(); i++) {
        if (token[0] == '\0' || strncmp(tb[i].name, token, strlen(token)) == 0)
            append_builtin_item(doc, items, &tb[i], token, 0);
    }

    /* 已定义函数 / 变量（普通补全辅助） */
    char defs[64][128];
    int ndef = 0;
    collect_defined(text, defs, &ndef, 64);
    for (int i = 0; i < ndef; i++) {
        if (token[0] == '\0' || strncmp(defs[i], token, strlen(token)) == 0) {
            /* kind 6 = Variable（约定为变量；此处定义为函数/变量通用项） */
            append_defined_item(doc, items, defs[i], token, 6);
        }
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

    const char *text = req_document_text(params);
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
        default:           handle_normal(doc, items, text, token); break;
    }

    yyjson_mut_obj_add_val(doc, res, "isIncomplete", yyjson_mut_false(doc));
    yyjson_mut_obj_add_val(doc, res, "items", items);
    yyjson_mut_obj_add_val(doc, root, "result", res);
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
        const char *text = req_document_text(params);
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

        const VusBuiltin *b = vus_builtin_find(token);
        if (b) {
            char md[1024];
            build_markdown(b, md, sizeof(md));
            result = yyjson_mut_obj(doc);
            yyjson_mut_val *content = yyjson_mut_obj(doc);
            obj_add_str(doc, content, "kind", "markdown");
            obj_add_str(doc, content, "value", md);
            yyjson_mut_obj_add_val(doc, result, "contents", content);
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
               strcmp(method, "textDocument/didClose") == 0 ||
               strcmp(method, "textDocument/didSave") == 0) {
        /* 文档同步通知：当前实现无需维护缓冲，忽略即可 */
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