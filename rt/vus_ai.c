/*
 * vus_ai.c — VUS AI 聚合模块（多服务商对话调用）
 *
 * 统一封装国内常用 AI 服务商的 OpenAI 兼容接口：
 *   AI_api池(配置JSON)      —— 批量导入 服务名→令牌 池（支持 "API_xxx" 前导名）
 *   AI_open兼容(服务名, 地址) —— 为某服务绑定 OpenAI 兼容端点（覆盖内置默认）
 *   AI_请求(名称, 模型, 消息)  —— 发起对话，返回回答文本；两参时消息缺省"你好"
 *   AI_服务列表()            —— 返回当前已注册的服务名（逗号分隔）
 *   AI_移除(服务名)          —— 从池中删除某服务
 *
 * 名称约定：注册/调用时 "API_" 前缀会被剥除，故 AI_api池({API_glm:"..."}) 后
 * 既可用 "API_glm" 也可用 "glm" 发起请求。未命中内置默认端点且未显式绑定
 * AI_open兼容 时返回可读错误，绝不盲目发网络请求。
 *
 * HTTP 通道：桌面复用系统 curl 命令（-H Authorization + JSON body），
 * 与其它 网络_* 桌面回退一致，不引入额外运行时依赖。
 */

#include "libvus_rt.h"
#include "yyjson/yyjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- 内置默认端点（均为 OpenAI 兼容，可拼 /chat/completions 与 /models） ---- */
typedef struct { const char *name; const char *base; } AiEndpoint;

/* name 支持官方名与中文别名（别名同样参与 /models 查询与调用解析） */
static const AiEndpoint s_defaults[] = {
    { "glm",         "https://open.bigmodel.cn/api/paas/v4" },
    { "智谱",        "https://open.bigmodel.cn/api/paas/v4" },
    { "qwen",        "https://dashscope.aliyuncs.com/compatible-mode/v1" },
    { "千问",        "https://dashscope.aliyuncs.com/compatible-mode/v1" },
    { "通义",        "https://dashscope.aliyuncs.com/compatible-mode/v1" },
    { "doubao",      "https://ark.cn-beijing.volces.com/api/v3" },
    { "豆包",        "https://ark.cn-beijing.volces.com/api/v3" },
    { "deepseek",    "https://api.deepseek.com/v1" },
    { "深度求索",    "https://api.deepseek.com/v1" },
    { "kimi",        "https://api.moonshot.cn/v1" },
    { "moonshot",    "https://api.moonshot.cn/v1" },
    { "月之暗面",    "https://api.moonshot.cn/v1" },
    { "siliconflow", "https://api.siliconflow.cn/v1" },
    { "硅基流动",    "https://api.siliconflow.cn/v1" },
    { "spark",       "https://spark-api-open.xf-yun.com/v1" },
    { "讯飞",        "https://spark-api-open.xf-yun.com/v1" },
    { "讯飞星火",    "https://spark-api-open.xf-yun.com/v1" },
    { "hunyuan",     "https://api.hunyuan.cloud.tencent.com/v1" },
    { "混元",        "https://api.hunyuan.cloud.tencent.com/v1" },
    { NULL, NULL }
};

#define AI_MAX_SRV  32
#define AI_NAME_LEN 64
#define AI_TOKEN_LEN 512
#define AI_BASE_LEN  512

typedef struct {
    char name[AI_NAME_LEN];   /* 规范化名（已剥 API_ 前缀） */
    char token[AI_TOKEN_LEN];
    char base[AI_BASE_LEN];   /* 空 = 用内置默认（若有） */
    int  used;
} AiSrv;

static AiSrv s_srv[AI_MAX_SRV];

/* ---- 内部辅助 ---- */

/* 剥离 "API_"（大小写不敏感）前缀，返回规范化名拷贝（静态缓冲不可跨调用保存） */
static const char *ai_norm(const char *name) {
    static char b[AI_NAME_LEN];
    snprintf(b, sizeof(b), "%s", name ? name : "");
    if (strncmp(b, "API_", 4) == 0) memmove(b, b + 4, strlen(b + 4) + 1);
    return b;
}

static int ai_find(const char *name) {
    for (int i = 0; i < AI_MAX_SRV; i++)
        if (s_srv[i].used && strcmp(s_srv[i].name, name) == 0) return i;
    return -1;
}

/* 找到或创建服务槽（按规范化名） */
static int ai_slot(const char *name) {
    const char *n = ai_norm(name);
    if (!n[0]) return -1;
    int idx = ai_find(n);
    if (idx >= 0) return idx;
    for (int i = 0; i < AI_MAX_SRV; i++) {
        if (!s_srv[i].used) {
            s_srv[i].used = 1;
            snprintf(s_srv[i].name, sizeof(s_srv[i].name), "%s", n);
            s_srv[i].token[0] = '\0';
            s_srv[i].base[0] = '\0';
            return i;
        }
    }
    return -1;
}

static const char *ai_default_base(const char *name) {
    for (int i = 0; s_defaults[i].name; i++)
        if (strcmp(s_defaults[i].name, name) == 0) return s_defaults[i].base;
    return NULL;
}

/* shell 单引号转义（vus_sh_squote 同款，独立实现避免跨文件依赖 static） */
static char *ai_sh_squote(const char *s) {
    if (!s) return NULL;
    size_t need = 3;
    for (const char *q = s; *q; q++) need += (*q == '\'' ? 4 : 1);
    char *out = (char *)malloc(need);
    if (!out) return NULL;
    char *p = out;
    *p++ = '\'';
    for (const char *q = s; *q; q++) {
        if (*q == '\'') { memcpy(p, "'\\''", 4); p += 4; }
        else *p++ = *q;
    }
    *p++ = '\'';
    *p = '\0';
    return out;
}

/* 返回失败 JSON 串（约定 {"错误": ...}），供错误路径统一返回 */
static VusString *ai_err(const char *msg) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "{\"错误\":\"%s\"}", msg);
    return vus_string_new(buf);
}

/* ---- 对外 API ---- */

/* AI_api池(配置JSON)：导入 {服务名: 令牌} 池；无参返回当前池 JSON（token 脱敏前 4 位） */
VusString *vus_ai_pool(VusString *config) {
    if (!config || vus_string_len(config) == 0) {
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        if (!doc) return vus_string_new("{}");
        yyjson_mut_val *root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);
        for (int i = 0; i < AI_MAX_SRV; i++) {
            if (!s_srv[i].used) continue;
            size_t tok = strlen(s_srv[i].token);
            char masked[16];
            size_t show = tok < 4 ? tok : 4;
            snprintf(masked, sizeof(masked), "%.*s", (int)show, s_srv[i].token);
            char disp[24];
            snprintf(disp, sizeof(disp), "%s%s", masked, tok > 4 ? "*" : "");
            char val[AI_BASE_LEN + 32];
            if (s_srv[i].base[0]) {
                snprintf(val, sizeof(val), "%s (端点:已配置)", disp);
            } else {
                snprintf(val, sizeof(val), "%s", disp);
            }
            yyjson_mut_obj_add_strcpy(doc, root, s_srv[i].name, val);
        }
        const char *out = yyjson_mut_write(doc, 0, NULL);
        if (!out) { yyjson_mut_doc_free(doc); return vus_string_new("{}"); }
        VusString *ret = vus_string_new(out);
        yyjson_mut_doc_free(doc);
        return ret;
    }

    const char *json = vus_string_cstr(config);
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    if (!doc) return ai_err("AI_api池: 配置不是合法 JSON 对象");
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return ai_err("AI_api池: 配置须为 {服务名: 令牌} 对象");
    }
    yyjson_val *k, *v;
    size_t ai_i, ai_n;
    yyjson_obj_foreach(root, ai_i, ai_n, k, v) {
        const char *key = yyjson_get_str(k);
        if (!key || !yyjson_is_str(v)) continue;
        int slot = ai_slot(key);
        if (slot < 0) continue;
        snprintf(s_srv[slot].token, sizeof(s_srv[slot].token), "%s", yyjson_get_str(v));
    }
    yyjson_doc_free(doc);
    return vus_string_new("1");
}

/* AI_open兼容(服务名, 地址)：为某服务绑定 OpenAI 兼容端点 */
VusString *vus_ai_open_compat(VusString *name, VusString *base) {
    if (!name || vus_string_len(name) == 0) return ai_err("AI_open兼容: 缺少服务名");
    const char *b = base ? vus_string_cstr(base) : "";
    if (!b[0]) return ai_err("AI_open兼容: 缺少地址");
    int slot = ai_slot(vus_string_cstr(name));
    if (slot < 0) return ai_err("AI_open兼容: 服务数超限");
    snprintf(s_srv[slot].base, sizeof(s_srv[slot].base), "%s", b);
    return vus_string_new("1");
}

/* AI_服务列表()：逗号分隔的已注册服务名 */
VusString *vus_ai_list(VusString *dummy) {
    (void)dummy;
    char out[AI_MAX_SRV * (AI_NAME_LEN + 2)];
    out[0] = '\0';
    size_t off = 0;
    int first = 1;
    for (int i = 0; i < AI_MAX_SRV; i++) {
        if (!s_srv[i].used) continue;
        if (!first) out[off++] = ',';
        first = 0;
        int n = snprintf(out + off, sizeof(out) - off, "%s", s_srv[i].name);
        if (n <= 0) break;
        off += (size_t)n;
    }
    return vus_string_new(out);
}

/* AI_移除(服务名)：从池中删除 */
VusString *vus_ai_del(VusString *name) {
    if (!name || vus_string_len(name) == 0) return ai_err("AI_移除: 缺少服务名");
    int idx = ai_find(ai_norm(vus_string_cstr(name)));
    if (idx < 0) return vus_string_new("0");
    memset(&s_srv[idx], 0, sizeof(s_srv[idx]));
    return vus_string_new("1");
}

/* AI_模型列表(服务名)：通过网络请求（GET {base}/models）查询内置服务商可用模型。
 * 仅内置服务商支持（官方名/中文别名均可）；需该服务已配置令牌。
 * 返回 JSON 数组 ["m1","m2",...]；接口异常返回 {"错误": ...}。 */
VusString *vus_ai_models(VusString *name) {
    if (!name || vus_string_len(name) == 0) return ai_err("AI_模型列表: 缺少服务名");
    int idx = ai_find(ai_norm(vus_string_cstr(name)));
    if (idx < 0) {
        char b[AI_NAME_LEN + 96];
        snprintf(b, sizeof(b), "AI_模型列表: 服务 \"%s\" 未配置", vus_string_cstr(name));
        return ai_err(b);
    }
    AiSrv *s = &s_srv[idx];
    if (!ai_default_base(s->name)) {
        char b[AI_NAME_LEN + 108];
        snprintf(b, sizeof(b), "AI_模型列表: \"%s\" 非内置服务商，不支持查询", s->name);
        return ai_err(b);
    }
    if (!s->token[0]) {
        char b[AI_NAME_LEN + 96];
        snprintf(b, sizeof(b), "AI_模型列表: \"%s\" 缺少令牌", s->name);
        return ai_err(b);
    }
    const char *base = s->base[0] ? s->base : ai_default_base(s->name);
    char url[512 + 32];
    snprintf(url, sizeof(url), "%s/models", base);
    char hdr[AI_TOKEN_LEN + 32];
    snprintf(hdr, sizeof(hdr), "Authorization: Bearer %s", s->token);
    char *qu = ai_sh_squote(url);
    char *qh = ai_sh_squote(hdr);
    if (!qu || !qh) { free(qu); free(qh); return ai_err("AI_模型列表: 内存不足"); }
    char cmd[10240];
    int n = snprintf(cmd, sizeof(cmd),
        "curl -s -L -m 30 -H %s %s 2>/dev/null", qh, qu);
    free(qu); free(qh);
    if (n <= 0 || n >= (int)sizeof(cmd)) return ai_err("AI_模型列表: 命令过长");

    VusString *out = vus_plugin_shell_exec(vus_string_new(cmd));
    if (!out || vus_string_len(out) == 0) {
        return ai_err("AI_模型列表: 网络请求无响应（检查网络/令牌/端点）");
    }
    const char *resp = vus_string_cstr(out);
    yyjson_doc *doc = yyjson_read(resp, strlen(resp), 0);
    if (!doc) return ai_err("AI_模型列表: 响应非 JSON");
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *data = yyjson_obj_get(root, "data");
    if (!yyjson_is_arr(data)) {
        yyjson_val *errv = yyjson_obj_get(root, "error");
        const char *emsg = NULL;
        if (yyjson_is_obj(errv)) {
            yyjson_val *m = yyjson_obj_get(errv, "message");
            if (yyjson_is_str(m)) emsg = yyjson_get_str(m);
        }
        char b[640];
        snprintf(b, sizeof(b), "AI_模型列表: 服务端未返回模型（%s）",
                 emsg ? emsg : "令牌无效或接口不支持 /models");
        yyjson_doc_free(doc);
        return ai_err(b);
    }

    /* 组装 JSON 数组 ["id1","id2",...]（用 mut 文档 + strcpy 保证值安全拷贝） */
    yyjson_mut_doc *mdoc = yyjson_mut_doc_new(NULL);
    if (!mdoc) { yyjson_doc_free(doc); return ai_err("AI_模型列表: 内存不足"); }
    yyjson_mut_val *arr = yyjson_mut_arr(mdoc);
    yyjson_mut_doc_set_root(mdoc, arr);
    size_t cap = yyjson_arr_size(data);
    if (cap > 256) cap = 256;   /* 防超长响应撑爆输出 */
    int added = 0;
    for (size_t i = 0; i < cap; i++) {
        yyjson_val *item = yyjson_arr_get(data, i);
        yyjson_val *idv = yyjson_is_obj(item) ? yyjson_obj_get(item, "id") : NULL;
        if (yyjson_is_str(idv)) {
            yyjson_mut_arr_add_strcpy(mdoc, arr, yyjson_get_str(idv));
            added++;
        }
    }
    yyjson_doc_free(doc);
    if (added == 0) {
        yyjson_mut_doc_free(mdoc);
        return ai_err("AI_模型列表: 服务端模型 data 为空");
    }
    const char *out2 = yyjson_mut_write(mdoc, 0, NULL);
    if (!out2) { yyjson_mut_doc_free(mdoc); return ai_err("AI_模型列表: 序列化失败"); }
    VusString *ret = vus_string_new(out2);
    yyjson_mut_doc_free(mdoc);
    return ret;
}

/* ===========================================================================
 * AI 工作流（后台协程 + 进度回调）
 * ---------------------------------------------------------------------------
 * 约定：用户用 `定义 AI_工作流_xxx(参数):` 编写工作流体，其内用 AI_返回(值)
 * 汇报进度、用 AI_完成返回() 宣告结束（工作流体应以 AI_完成返回 收尾）。
 *   AI_启动工作流(槽名, 函数, 参数) —— 创建协程后台执行；每个 AI_返回 /
 *     AI_完成返回 都会让出，等待外部 AI_推进(槽名) 推进一步。
 *   AI_回调(槽名)        读该工作流「最近一次 AI_返回」的值（未结束也可读）
 *   AI_清空回调(槽名)    把回调值复位为空串
 *   AI_回调_完成(槽名)   "1"/"0"，是否已调用过 AI_完成返回
 *   AI_工作流状态(槽名)  三态："不存在" / "进行中" / "完成"（重新启动后刷新）
 * 说明：VUS 协程为单线程协作式，"后台"指与主流程交错推进；同名槽启动即刷新状态。
 * =========================================================================== */
#define AI_MAX_WF 16
#define WF_ALIVE 1
#define WF_DONE  2

typedef struct {
    char         slot[AI_NAME_LEN];
    VusString   *handle;    /* 协程句柄字符串（VusString*） */
    VusString   *value;     /* 最新 AI_返回 值 */
    int          state;     /* 0=未用 1=进行中 2=完成 */
    struct { void (*func)(void*); void* arg; } task; /* 常驻任务（防栈悬垂） */
} AiWf;

static AiWf   s_wf[AI_MAX_WF];
static AiWf  *s_cur = NULL; /* 当前正在运行的协程绑定的工作流（单线程协作安全） */

static AiWf *ai_wf_find(const char *slot) {
    for (int i = 0; i < AI_MAX_WF; i++)
        if (s_wf[i].state != 0 && strcmp(s_wf[i].slot, slot) == 0) return &s_wf[i];
    return NULL;
}

static AiWf *ai_wf_slot(const char *slot) {
    AiWf *w = ai_wf_find(slot);
    if (w) return w;
    for (int i = 0; i < AI_MAX_WF; i++) {
        if (s_wf[i].state == 0) {
            w = &s_wf[i];
            memset(w, 0, sizeof(*w));
            snprintf(w->slot, sizeof(w->slot), "%s", slot);
            w->value = vus_string_new("");
            return w;
        }
    }
    return NULL;
}

/* 协程入口：task 与生成代码 _VusThreadTask 同布局 {func, arg}，解包执行用户函数。
 * 用户函数签名 void fn(VusString** args)：args[0] 返回值槽、args[1] 传入参数。
 * 调用形态与生成代码 _vus_thread_run 完全一致（func 以 void(*)(void*) 传参）。 */
static void vus_ai_wf_run(void *arg) {
    AiWf *w = (AiWf*)((char*)arg - (long)(unsigned long)((char*)&((AiWf*)0)->task));
    s_cur = w;
    if (w->task.func) {
        VusString *_vus_args[2] = {NULL, (VusString*)w->task.arg};
        w->task.func((void*)_vus_args);
        /* 工作流自然结束但未显式 AI_返回：若函数有返回值则以返回值补记 */
        if (_vus_args[0] && (!w->value || vus_string_len(w->value) == 0)) {
            vus_unref(w->value);
            w->value = vus_string_new(vus_string_cstr(_vus_args[0]));
        }
    }
    s_cur = NULL;
    /* 未调用 AI_完成返回 即结束：状态保持进行中，由下次启动覆盖（文档约定） */
}

/* AI_启动工作流(槽名, 函数, 参数)：创建协程后台执行，驱动到首个让步点 */
VusString *vus_ai_wf_start(VusString *slot, VusString *fn, VusString *arg) {
    if (!slot || vus_string_len(slot) == 0) return ai_err("AI_启动工作流: 缺少槽名");
    if (!fn) return ai_err("AI_启动工作流: 缺少函数");
    AiWf *w = ai_wf_slot(vus_string_cstr(slot));
    if (!w) return ai_err("AI_启动工作流: 槽位已满");
    /* 覆盖同名旧工作流：释放旧句柄与值，状态刷新为进行中 */
    if (w->handle) { vus_unref(w->handle); w->handle = NULL; }
    if (w->value)  { vus_unref(w->value);  w->value = vus_string_new(""); }
    w->state = WF_ALIVE;
    w->task.func = (void (*)(void*))fn;
    w->task.arg  = (void*)arg;
    VusString *h = vus_coro_create_handle((void (*)(void*))vus_ai_wf_run, (void*)&w->task);
    if (!h || strcmp(vus_string_cstr(h), "-1") == 0) {
        (void)h;
        w->state = 0;   /* 启动失败即不存在 */
        return ai_err("AI_启动工作流: 协程创建失败");
    }
    w->handle = vus_string_new(vus_string_cstr(h));
    vus_unref(h);
    vus_coro_resume_handle(w->handle);   /* 驱动到首个让步点 */
    if (w->state == 0) w->state = WF_ALIVE; /* 已在入口绑定 */
    return vus_string_new("1");
}

/* AI_返回(值)：汇报最新进度（工作流内调用；不在活动工作流内时忽略返回 "0"） */
VusString *vus_ai_wf_return(VusString *v) {
    if (!s_cur || s_cur->state != WF_ALIVE) return vus_string_new("0");
    const char *txt = v ? vus_string_cstr(v) : "";
    VusString *nv = vus_string_new(txt);
    vus_unref(s_cur->value);
    s_cur->value = nv;
    vus_coro_yield();   /* 让出，等待 AI_推进 继续 */
    return vus_string_new("1");
}

/* AI_完成返回()：标记完成（工作流内调用；非活动上下文安全 no-op） */
VusString *vus_ai_wf_done(void) {
    if (s_cur && s_cur->state == WF_ALIVE) {
        s_cur->state = WF_DONE;
        vus_coro_yield();
        return vus_string_new("1");
    }
    return vus_string_new("0");
}

/* AI_回调(槽名)：读取最近一次 AI_返回 的值（不要求完成） */
VusString *vus_ai_wf_value(VusString *slot) {
    if (!slot || vus_string_len(slot) == 0) return vus_string_new("");
    AiWf *w = ai_wf_find(vus_string_cstr(slot));
    return w && w->value ? vus_string_new(vus_string_cstr(w->value)) : vus_string_new("");
}

/* AI_清空回调(槽名)：把回调值复位为空串 */
VusString *vus_ai_wf_clear(VusString *slot) {
    if (!slot || vus_string_len(slot) == 0) return vus_string_new("0");
    AiWf *w = ai_wf_find(vus_string_cstr(slot));
    if (!w || !w->value) return vus_string_new("0");
    vus_unref(w->value);
    w->value = vus_string_new("");
    return vus_string_new("1");
}

/* AI_回调_完成(槽名)：是否完成 */
VusString *vus_ai_wf_done_q(VusString *slot) {
    if (!slot || vus_string_len(slot) == 0) return vus_string_new("0");
    AiWf *w = ai_wf_find(vus_string_cstr(slot));
    return vus_string_new(w && w->state == WF_DONE ? "1" : "0");
}

/* AI_工作流状态(槽名)：不存在 / 进行中 / 完成 */
VusString *vus_ai_wf_status(VusString *slot) {
    if (!slot || vus_string_len(slot) == 0) return vus_string_new("不存在");
    AiWf *w = ai_wf_find(vus_string_cstr(slot));
    if (!w) return vus_string_new("不存在");
    if (w->state == WF_DONE) return vus_string_new("完成");
    return vus_string_new("进行中");
}

/* AI_推进(槽名)：把工作流推进一步（越过一个让步点），结束后返回 "0" */
VusString *vus_ai_wf_step(VusString *slot) {
    if (!slot || vus_string_len(slot) == 0) return vus_string_new("0");
    AiWf *w = ai_wf_find(vus_string_cstr(slot));
    if (!w || w->state == WF_DONE) return vus_string_new("0");
    s_cur = w;
    vus_coro_resume_handle(w->handle);
    s_cur = NULL;
    return vus_string_new("1");
}

/* ===========================================================================
 * AI 图像生成（OpenAI 兼容 images/generations：智谱 glm-image / cogview 等）
 * ---------------------------------------------------------------------------
 * AI_文生图(服务商, 模型, 提示词[, 尺寸])   → POST {base}/images/generations
 * AI_图生图(服务商, 模型, 图片路径, 提示词[, 强度])
 *     → 读图片→Base64 → 同端点带 image 字段（智谱 glm-image 系支持图生图）
 * 成功返回 data[0].url；接口异常返回 {"错误": ...}（可 JSON 解析）。
 * =========================================================================== */

/* Base64 编码（标准表；用于图生图图片内嵌） */
static char *ai_b64(const unsigned char *src, size_t len) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (len == 0) return strdup("");
    size_t olen = 4 * ((len + 2) / 3);
    char *out = (char*)malloc(olen + 1);
    if (!out) return NULL;
    size_t i = 0, o = 0;
    while (i + 2 < len) {
        uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i+1] << 8) | src[i+2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = tbl[(v >> 6) & 63];
        out[o++] = tbl[v & 63];
        i += 3;
    }
    if (i + 1 == len) {
        uint32_t v = (uint32_t)src[i] << 16;
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = '='; out[o++] = '=';
    } else if (i + 2 == len) {
        uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i+1] << 8);
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = tbl[(v >> 6) & 63];
        out[o++] = '=';
    }
    out[o] = '\0';
    return out;
}

/* 读图片文件（≤20MB，含空保护） */
static char *ai_read_file(const char *path, size_t *out_len) {
    *out_len = 0;
    if (!path || !path[0]) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 20 * 1024 * 1024) { fclose(f); return NULL; }
    char *buf = (char*)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    *out_len = rd;
    return buf;
}

/* 图像生成公共实现：
 * is_edit=0 文生图（body: model/prompt[/size]）；is_edit=1 图生图（+image[]/strength） */
static VusString *ai_image_call(VusString *name, VusString *model, const char *prompt,
                                const char *size, int is_edit, const char *img_path,
                                const char *strength) {
    if (!name || vus_string_len(name) == 0) return ai_err("AI_图像: 缺少服务名");
    const char *mmodel = model && vus_string_len(model) > 0 ? vus_string_cstr(model) : "";
    if (!mmodel[0]) return ai_err("AI_图像: 缺少模型名");
    if (!prompt || !prompt[0]) return ai_err("AI_图像: 缺少提示词");

    int idx = ai_find(ai_norm(vus_string_cstr(name)));
    if (idx < 0) {
        char b[AI_NAME_LEN + 96];
        snprintf(b, sizeof(b), "AI_图像: 服务 \"%s\" 未配置，先 AI_api池 导入令牌", vus_string_cstr(name));
        return ai_err(b);
    }
    AiSrv *s = &s_srv[idx];
    if (!s->token[0]) {
        char b[AI_NAME_LEN + 96];
        snprintf(b, sizeof(b), "AI_图像: \"%s\" 缺少令牌", s->name);
        return ai_err(b);
    }
    const char *base = s->base[0] ? s->base : ai_default_base(s->name);
    if (!base) return ai_err("AI_图像: 无可用端点，用 AI_open兼容 绑定");

    /* body */
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return ai_err("AI_图像: 内存不足");
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "model", mmodel);
    yyjson_mut_obj_add_strcpy(doc, root, "prompt", prompt);
    if (size && size[0]) yyjson_mut_obj_add_strcpy(doc, root, "size", size);
    /* 图生图：读文件 → Base64 → image 字段（智谱 glm-image 兼容） */
    if (is_edit) {
        size_t flen = 0;
        char *fdata = ai_read_file(img_path, &flen);
        if (!fdata || flen == 0) {
            if (fdata) free(fdata);
            yyjson_mut_doc_free(doc);
            return ai_err("AI_图生图: 读取图片失败（文件不存在或超 20MB）");
        }
        char *b64 = ai_b64((const unsigned char*)fdata, flen);
        free(fdata);
        if (!b64) { yyjson_mut_doc_free(doc); return ai_err("AI_图生图: 内存不足"); }
        char datauri[128 + 4 * (flen / 3) + 8];
        snprintf(datauri, sizeof(datauri), "data:image/jpeg;base64,%s", b64);
        free(b64);
        yyjson_mut_obj_add_strcpy(doc, root, "image", datauri);
        if (strength && strength[0]) {
            /* 允许 0.0~1.0 强度（文本形式传入原样透传数值） */
            yyjson_mut_obj_add_strcpy(doc, root, "strength", strength);
        }
    }
    const char *body = yyjson_mut_write(doc, 0, NULL);
    if (!body) { yyjson_mut_doc_free(doc); return ai_err("AI_图像: JSON 构造失败"); }

    char url[512 + 32];
    snprintf(url, sizeof(url), "%s/images/generations", base);
    char hdr[AI_TOKEN_LEN + 32];
    snprintf(hdr, sizeof(hdr), "Authorization: Bearer %s", s->token);
    char *qu = ai_sh_squote(url);
    char *qh = ai_sh_squote(hdr);
    char *qd = ai_sh_squote(body);
    if (!qu || !qh || !qd) {
        free(qu); free(qh); free(qd);
        yyjson_mut_doc_free(doc);
        return ai_err("AI_图像: 内存不足");
    }
    /* 生图耗时较长，超时给足；body 可能很大（图生图 Base64），动态分配命令缓冲 */
    static const long curl_timeout = 180;
    size_t cmd_need = strlen(qu) + strlen(qh) + strlen(qd) + 256;
    char *cmd = (char*)malloc(cmd_need);
    if (!cmd) {
        free(qu); free(qh); free(qd);
        yyjson_mut_doc_free(doc);
        return ai_err("AI_图像: 内存不足");
    }
    int n = snprintf(cmd, cmd_need,
        "curl -s -L -m %ld -H 'Content-Type: application/json' "
        "-H %s -d %s %s 2>/dev/null",
        curl_timeout, qh, qd, qu);
    free(qu); free(qh); free(qd);
    yyjson_mut_doc_free(doc);
    if (n <= 0 || (size_t)n >= cmd_need) {
        free(cmd);
        return ai_err("AI_图像: 命令过长");
    }

    VusString *out = vus_plugin_shell_exec(vus_string_new(cmd));
    free(cmd);
    if (!out || vus_string_len(out) == 0) {
        return ai_err("AI_图像: 网络请求无响应（检查网络/令牌/端点）");
    }
    const char *resp = vus_string_cstr(out);
    yyjson_doc *jdoc = yyjson_read(resp, strlen(resp), 0);
    if (!jdoc) {
        char b[384];
        snprintf(b, sizeof(b), "AI_图像: 响应非 JSON（前 120 字：%s）",
                 resp[0] ? resp : "(空)");
        return ai_err(b);
    }
    yyjson_val *jroot = yyjson_doc_get_root(jdoc);
    yyjson_val *data = yyjson_obj_get(jroot, "data");
    const char *img_url = NULL;
    if (yyjson_is_arr(data) && yyjson_arr_size(data) > 0) {
        yyjson_val *d0 = yyjson_arr_get(data, 0);
        yyjson_val *u = yyjson_obj_get(d0, "url");
        if (yyjson_is_str(u)) img_url = yyjson_get_str(u);
    }
    if (!img_url) {
        yyjson_val *errv = yyjson_obj_get(jroot, "error");
        const char *emsg = NULL;
        if (yyjson_is_obj(errv)) {
            yyjson_val *m = yyjson_obj_get(errv, "message");
            if (yyjson_is_str(m)) emsg = yyjson_get_str(m);
        }
        char b[640];
        snprintf(b, sizeof(b), "AI_图像: 服务端未返回图片 URL（%s）",
                 emsg ? emsg : "仅返回 Base64 或额度不足/不可用");
        yyjson_doc_free(jdoc);
        return ai_err(b);
    }
    VusString *ret = vus_string_new(img_url);
    yyjson_doc_free(jdoc);
    return ret;
}

/* AI_文生图(服务商, 模型, 提示词[, 尺寸]) */
VusString *vus_ai_image_gen(VusString *name, VusString *model, VusString *prompt,
                            VusString *size) {
    return ai_image_call(name, model, prompt ? vus_string_cstr(prompt) : "",
                         size ? vus_string_cstr(size) : "", 0, NULL, NULL);
}

/* AI_图生图(服务商, 模型, 图片路径, 提示词[, 强度]) */
VusString *vus_ai_image_edit(VusString *name, VusString *model, VusString *imgpath,
                             VusString *prompt, VusString *strength) {
    return ai_image_call(name, model, prompt ? vus_string_cstr(prompt) : "",
                         NULL, 1, imgpath ? vus_string_cstr(imgpath) : "",
                         strength ? vus_string_cstr(strength) : NULL);
}

VusString *vus_ai_chat(VusString *name, VusString *model, VusString *message) {
    if (!name || vus_string_len(name) == 0) return ai_err("AI_请求: 缺少服务名");
    const char *mname = vus_string_cstr(name);
    const char *mmodel = model && vus_string_len(model) > 0 ? vus_string_cstr(model) : "";
    if (!mmodel[0]) return ai_err("AI_请求: 缺少模型名");

    int idx = ai_find(ai_norm(mname));
    if (idx < 0) {
        char b[AI_NAME_LEN + 96];
        snprintf(b, sizeof(b), "AI_请求: 服务 \"%s\" 未配置，先 AI_api池 导入令牌", mname);
        return ai_err(b);
    }
    AiSrv *s = &s_srv[idx];
    if (!s->token[0]) {
        char b[AI_NAME_LEN + 96];
        snprintf(b, sizeof(b), "AI_请求: \"%s\" 缺少令牌", s->name);
        return ai_err(b);
    }
    const char *base = s->base[0] ? s->base : ai_default_base(s->name);
    if (!base) {
        char b[AI_NAME_LEN + 128];
        snprintf(b, sizeof(b),
                 "AI_请求: \"%s\" 无可用端点，用 AI_open兼容(\"url\", \"%s\") 绑定",
                 s->name, s->name);
        return ai_err(b);
    }
    if (strstr(base, "http://") != base && strstr(base, "https://") != base) {
        return ai_err("AI_open兼容: 地址须以 http:// 或 https:// 开头");
    }

    const char *msg = message && vus_string_len(message) > 0 ? vus_string_cstr(message) : "你好";

    /* 构造请求体 JSON */
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return ai_err("AI_请求: 内存不足");
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "model", mmodel);
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    yyjson_mut_val *m0 = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, m0, "role", "user");
    yyjson_mut_obj_add_strcpy(doc, m0, "content", msg);
    yyjson_mut_arr_append(arr, m0);
    yyjson_mut_obj_add_val(doc, root, "messages", arr);
    const char *body = yyjson_mut_write(doc, 0, NULL);
    if (!body) {
        yyjson_mut_doc_free(doc);
        return ai_err("AI_请求: JSON 构造失败");
    }

    /* 构造 curl 命令行：Authorization Bearer + JSON body
     * 注意 header 值需整体单引号包裹（否则带引号的 token 会原样发出导致 401）。 */
    char url[512 + 64];
    snprintf(url, sizeof(url), "%s/chat/completions", base);
    char hdr[AI_TOKEN_LEN + 32];
    snprintf(hdr, sizeof(hdr), "Authorization: Bearer %s", s->token);
    char *qu = ai_sh_squote(url);
    char *qh = ai_sh_squote(hdr);
    char *qd = ai_sh_squote(body);
    if (!qu || !qh || !qd) {
        free(qu); free(qh); free(qd);
        yyjson_mut_doc_free(doc);
        return ai_err("AI_请求: 内存不足");
    }
    char cmd[16384];
    int n = snprintf(cmd, sizeof(cmd),
        "curl -s -L -m 60 -H 'Content-Type: application/json' "
        "-H %s -d %s %s 2>/dev/null",
        qh, qd, qu);
    free(qu); free(qh); free(qd);
    yyjson_mut_doc_free(doc);
    if (n <= 0 || n >= (int)sizeof(cmd)) return ai_err("AI_请求: 命令过长");

    VusString *out = vus_plugin_shell_exec(vus_string_new(cmd));
    if (!out || vus_string_len(out) == 0) {
        return ai_err("AI_请求: 网络请求无响应（检查网络/令牌/端点）");
    }
    const char *resp = vus_string_cstr(out);

    /* 解析 choices[0].message.content */
    yyjson_doc *jdoc = yyjson_read(resp, strlen(resp), 0);
    if (!jdoc) {
        char b[256];
        snprintf(b, sizeof(b), "AI_请求: 响应非 JSON（前 120 字：%s）",
                 resp[0] ? resp : "(空)");
        return ai_err(b);
    }
    yyjson_val *jroot = yyjson_doc_get_root(jdoc);
    yyjson_val *choices = yyjson_obj_get(jroot, "choices");
    const char *content = NULL;
    if (yyjson_is_arr(choices) && yyjson_arr_size(choices) > 0) {
        yyjson_val *c0 = yyjson_arr_get(choices, 0);
        yyjson_val *msgv = yyjson_obj_get(c0, "message");
        yyjson_val *cv = yyjson_obj_get(msgv, "content");
        if (yyjson_is_str(cv)) content = yyjson_get_str(cv);
    }
    if (!content) {
        yyjson_val *errv = yyjson_obj_get(jroot, "error");
        const char *errmsg = NULL;
        if (yyjson_is_obj(errv)) {
            yyjson_val *ev = yyjson_obj_get(errv, "message");
            if (yyjson_is_str(ev)) errmsg = yyjson_get_str(ev);
        }
        char b[512];
        snprintf(b, sizeof(b), "AI_请求: 服务端无内容（%s）",
                 errmsg ? errmsg : "可能令牌无效或模型名错误");
        yyjson_doc_free(jdoc);
        return ai_err(b);
    }
    VusString *ret = vus_string_new(content);
    yyjson_doc_free(jdoc);
    return ret;
}