#define _GNU_SOURCE
#include "libvus_rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <elog.h>

// ============ 引用计数通用操作 ============

void vus_ref(void* obj) {
    if (!obj) return;
    int* ref = (int*)obj;
    (*ref)++;
}

void vus_unref(void* obj) {
    if (!obj) return;
    int* ref = (int*)obj;
    (*ref)--;
    if (*ref <= 0) {
        // 根据类型释放 - 简化实现，由调用方确保正确释放
        free(obj);
    }
}

// ============ 字符串 ============

VusString* vus_string_new(const char* s) {
    if (!s) return NULL;
    int len = strlen(s);
    return vus_string_new_len(s, len);
}

VusString* vus_string_new_len(const char* s, int len) {
    VusString* str = (VusString*)malloc(sizeof(VusString));
    if (!str) return NULL;
    str->ref = 1;
    str->len = len;
    str->data = (char*)malloc(len + 1);
    if (!str->data) {
        free(str);
        return NULL;
    }
    memcpy(str->data, s, len);
    str->data[len] = '\0';
    return str;
}

VusString* vus_string_concat(VusString* a, VusString* b) {
    if (!a && !b) return vus_string_new("");
    if (!a) return vus_string_new_len(b->data, b->len);
    if (!b) return vus_string_new_len(a->data, a->len);

    int new_len = a->len + b->len;
    char* buf = (char*)malloc(new_len + 1);
    if (!buf) return NULL;
    memcpy(buf, a->data, a->len);
    memcpy(buf + a->len, b->data, b->len);
    buf[new_len] = '\0';

    VusString* result = vus_string_new_len(buf, new_len);
    free(buf);
    return result;
}

VusString* vus_string_slice(VusString* s, int start, int len) {
    if (!s) return NULL;
    if (start < 0) start = 0;
    if (start >= s->len) return vus_string_new("");
    if (len < 0) len = 0;
    if (start + len > s->len) len = s->len - start;
    return vus_string_new_len(s->data + start, len);
}

int vus_string_len(VusString* s) {
    return s ? s->len : 0;
}

char* vus_string_cstr(VusString* s) {
    return s ? s->data : NULL;
}

// ============ 列表 ============

VusList* vus_list_new(int type) {
    VusList* list = (VusList*)malloc(sizeof(VusList));
    if (!list) return NULL;
    list->ref = 1;
    list->len = 0;
    list->cap = 4;
    list->items = (void**)malloc(sizeof(void*) * list->cap);
    list->type = type;
    return list;
}

void vus_list_append(VusList* list, void* item) {
    if (!list) return;
    if (list->len >= list->cap) {
        list->cap *= 2;
        list->items = (void**)realloc(list->items, sizeof(void*) * list->cap);
    }
    vus_ref(item);
    list->items[list->len++] = item;
}

void* vus_list_get(VusList* list, int index) {
    if (!list || index < 0 || index >= list->len) return NULL;
    return list->items[index];
}

void vus_list_remove(VusList* list, int index) {
    if (!list || index < 0 || index >= list->len) return;
    vus_unref(list->items[index]);
    for (int i = index; i < list->len - 1; i++) {
        list->items[i] = list->items[i + 1];
    }
    list->len--;
}

void vus_list_set(VusList* list, int index, void* item) {
    if (!list || index < 0 || index >= list->len) return;
    vus_ref(item);
    vus_unref(list->items[index]);
    list->items[index] = item;
}

int vus_list_len(VusList* list) {
    return list ? list->len : 0;
}

// ============ 字典 ============
// 简单哈希表实现 - 链地址法

typedef struct DictEntry {
    VusString* key;
    void* value;
    struct DictEntry* next;
} DictEntry;

struct DictImpl {
    DictEntry** buckets;
    int size;
    int count;
};

static unsigned int hash_string(VusString* key) {
    unsigned int hash = 5381;
    char* data = key->data;
    for (int i = 0; i < key->len; i++) {
        hash = ((hash << 5) + hash) + (unsigned char)data[i];
    }
    return hash;
}

static void dict_resize(VusDict* dict) {
    struct DictImpl* impl = (struct DictImpl*)dict->impl;
    int old_size = impl->size;
    DictEntry** old_buckets = impl->buckets;

    impl->size = old_size * 2;
    impl->buckets = (DictEntry**)calloc(impl->size, sizeof(DictEntry*));
    impl->count = 0;

    for (int i = 0; i < old_size; i++) {
        DictEntry* entry = old_buckets[i];
        while (entry) {
            DictEntry* next = entry->next;
            unsigned int idx = hash_string(entry->key) % impl->size;
            entry->next = impl->buckets[idx];
            impl->buckets[idx] = entry;
            impl->count++;
            entry = next;
        }
    }
    free(old_buckets);
}

VusDict* vus_dict_new(void) {
    VusDict* dict = (VusDict*)malloc(sizeof(VusDict));
    if (!dict) return NULL;
    dict->ref = 1;
    struct DictImpl* impl = (struct DictImpl*)malloc(sizeof(struct DictImpl));
    impl->size = 16;
    impl->count = 0;
    impl->buckets = (DictEntry**)calloc(impl->size, sizeof(DictEntry*));
    dict->impl = impl;
    return dict;
}

void vus_dict_set(VusDict* dict, VusString* key, void* value) {
    if (!dict || !key) return;
    struct DictImpl* impl = (struct DictImpl*)dict->impl;

    if (impl->count > impl->size * 0.75) {
        dict_resize(dict);
    }

    unsigned int idx = hash_string(key) % impl->size;

    // 查找是否已存在
    DictEntry* entry = impl->buckets[idx];
    while (entry) {
        if (vus_string_len(entry->key) == key->len &&
            memcmp(entry->key->data, key->data, key->len) == 0) {
            // 更新已有键
            vus_ref(value);
            vus_unref(entry->value);
            entry->value = value;
            return;
        }
        entry = entry->next;
    }

    // 创建新条目
    entry = (DictEntry*)malloc(sizeof(DictEntry));
    entry->key = key;
    vus_ref(key);
    entry->value = value;
    vus_ref(value);
    entry->next = impl->buckets[idx];
    impl->buckets[idx] = entry;
    impl->count++;
}

void* vus_dict_get(VusDict* dict, VusString* key) {
    if (!dict || !key) return NULL;
    struct DictImpl* impl = (struct DictImpl*)dict->impl;
    unsigned int idx = hash_string(key) % impl->size;
    DictEntry* entry = impl->buckets[idx];
    while (entry) {
        if (vus_string_len(entry->key) == key->len &&
            memcmp(entry->key->data, key->data, key->len) == 0) {
            return entry->value;
        }
        entry = entry->next;
    }
    return NULL;
}

void vus_dict_remove(VusDict* dict, VusString* key) {
    if (!dict || !key) return;
    struct DictImpl* impl = (struct DictImpl*)dict->impl;
    unsigned int idx = hash_string(key) % impl->size;
    DictEntry** prev = &impl->buckets[idx];
    DictEntry* entry = impl->buckets[idx];
    while (entry) {
        if (vus_string_len(entry->key) == key->len &&
            memcmp(entry->key->data, key->data, key->len) == 0) {
            *prev = entry->next;
            vus_unref(entry->key);
            vus_unref(entry->value);
            free(entry);
            impl->count--;
            return;
        }
        prev = &entry->next;
        entry = entry->next;
    }
}

int vus_dict_len(VusDict* dict) {
    if (!dict) return 0;
    struct DictImpl* impl = (struct DictImpl*)dict->impl;
    return impl->count;
}

// ============ 闭包 ============

VusClosure* vus_closure_new(void (*func)(void*, void*), void* env) {
    VusClosure* closure = (VusClosure*)malloc(sizeof(VusClosure));
    if (!closure) return NULL;
    closure->ref = 1;
    closure->func = func;
    closure->env = env;
    vus_ref(env);
    return closure;
}

void vus_closure_call(VusClosure* closure, void* args) {
    if (!closure || !closure->func) return;
    closure->func(closure->env, args);
}

// ============ 错误处理 ============

VusError* vus_error_new(int code, const char* msg, int line, const char* func) {
    VusError* err = (VusError*)malloc(sizeof(VusError));
    if (!err) return NULL;
    err->code = code;
    err->msg = msg ? strdup(msg) : NULL;
    err->line = line;
    err->func = func ? strdup(func) : NULL;
    err->next = NULL;
    return err;
}

void vus_error_push(VusError** chain, VusError* err) {
    if (!chain || !err) return;
    err->next = *chain;
    *chain = err;
}

void vus_error_print(VusError* err) {
    if (!err) return;
    fprintf(stderr, "E%03d %s (代码行号: %d)\n", err->code, err->msg ? err->msg : "", err->line);
    if (err->func) {
        fprintf(stderr, "    位置: %s\n", err->func);
    }
    if (err->next) {
        vus_error_print(err->next);
    }
}

void vus_error_free(VusError* err) {
    while (err) {
        VusError* next = err->next;
        free((void*)err->msg);
        free((void*)err->func);
        free(err);
        err = next;
    }
}

// ============ 调试支持 ============

int vus_debug_enabled = 0;

void vus_debug_print(const char* msg) {
    if (vus_debug_enabled) {
        fprintf(stdout, "[调试] %s\n", msg);
    }
}

// ============ 分级日志（EasyLogger 集成） ============

static int s_vus_log_inited = 0;

/* 惰性初始化 EasyLogger，幂等。成功返回 0，失败返回 -1。 */
int vus_log_init(void) {
    if (s_vus_log_inited) return 0;
    if (elog_init() != ELOG_NO_ERR) return -1;

    /* 启用格式：级别 + 标签 + 时间（VUS 运行时非源码行号，禁用 dir/func/line） */
    for (int lvl = ELOG_LVL_ASSERT; lvl <= ELOG_LVL_VERBOSE; lvl++) {
        elog_set_fmt((uint8_t)lvl, ELOG_FMT_LVL | ELOG_FMT_TAG | ELOG_FMT_TIME);
    }

    elog_start();   /* 启用输出并打印初始化成功日志 */
    s_vus_log_inited = 1;
    return 0;
}

/* 解析中文级别名称，返回对应 ELOG_LVL_*；无法识别返回 -1 */
static int vus_log_parse_level(VusString* level) {
    if (!level || !level->data) return -1;
    const char* s = level->data;
    if (strcmp(s, "调试") == 0) return ELOG_LVL_DEBUG;
    if (strcmp(s, "信息") == 0) return ELOG_LVL_INFO;
    if (strcmp(s, "警告") == 0) return ELOG_LVL_WARN;
    if (strcmp(s, "错误") == 0) return ELOG_LVL_ERROR;
    if (strcmp(s, "assert") == 0) return ELOG_LVL_ASSERT;
    if (strcmp(s, "verbose") == 0) return ELOG_LVL_VERBOSE;
    return -1;
}

/* 设置运行时过滤级别：低于该级别的日志将被过滤。 */
VusString* vus_log_set_level(VusString* level) {
    if (vus_log_init() != 0) return vus_string_new("-1");
    int lvl = vus_log_parse_level(level);
    if (lvl < 0) return vus_string_new("-1");
    elog_set_filter_lvl((uint8_t)lvl);
    return vus_string_new("0");
}

#define VUS_LOG_TAG "vus"

VusString* vus_log_debug(VusString* msg) {
    if (vus_log_init() != 0) return vus_string_new("-1");
    elog_d(VUS_LOG_TAG, "%s", msg ? vus_string_cstr(msg) : "");
    return vus_string_new("0");
}

VusString* vus_log_info(VusString* msg) {
    if (vus_log_init() != 0) return vus_string_new("-1");
    elog_i(VUS_LOG_TAG, "%s", msg ? vus_string_cstr(msg) : "");
    return vus_string_new("0");
}

VusString* vus_log_warn(VusString* msg) {
    if (vus_log_init() != 0) return vus_string_new("-1");
    elog_w(VUS_LOG_TAG, "%s", msg ? vus_string_cstr(msg) : "");
    return vus_string_new("0");
}

VusString* vus_log_error(VusString* msg) {
    if (vus_log_init() != 0) return vus_string_new("-1");
    elog_e(VUS_LOG_TAG, "%s", msg ? vus_string_cstr(msg) : "");
    return vus_string_new("0");
}

// ============ 栈追踪支持 ============

int vus_stack_depth = 0;
const char* vus_stack_frames[VUS_MAX_STACK_DEPTH];

void vus_stack_push(const char* func_name) {
    if (vus_stack_depth < VUS_MAX_STACK_DEPTH) {
        vus_stack_frames[vus_stack_depth++] = func_name;
    }
}

void vus_stack_pop(void) {
    if (vus_stack_depth > 0) {
        vus_stack_depth--;
    }
}

void vus_stack_print(void) {
    fprintf(stderr, "调用栈追踪:\n");
    for (int i = 0; i < vus_stack_depth; i++) {
        fprintf(stderr, "  [%d] %s\n", i, vus_stack_frames[i]);
    }
}

// ============ 标准库辅助函数 ============

void vus_print(void* s) {
    if (!s) return;
    if (vus_is_object(s)) {
        VusString* rep = vus_object_to_string(s);
        if (rep) { printf("%s", vus_string_cstr(rep)); vus_unref(rep); }
    } else {
        VusString* str = (VusString*)s;
        if (!str->data) return;
        printf("%s", str->data);
    }
    fflush(stdout);
}

/* 将任意值（VusString* 或 VusObject*）转为字符串表示。
 * 标量取原文；列表/字典递归序列化为可读文本。纯 C 实现，不依赖嵌入式 Python。 */
VusString* vus_object_to_string(void* obj) {
    if (!obj) return vus_string_new("");
    if (!vus_is_object(obj)) {
        VusString* s = (VusString*)obj;
        return s ? vus_string_new_len(s->data, s->len) : vus_string_new("");
    }
    VusObject* o = (VusObject*)obj;
    switch (o->type) {
        case TYPE_STR:
            return o->u.str ? vus_string_new_len(o->u.str->data, o->u.str->len) : vus_string_new("");
        case TYPE_LIST: {
            VusList* list = o->u.list;
            if (!list) return vus_string_new("[]");
            VusString* acc = vus_string_new("[");
            int n = vus_list_len(list);
            for (int i = 0; i < n; i++) {
                VusString* item = vus_object_to_string(vus_list_get(list, i));
                if (!item) item = vus_string_new("");
                if (i > 0) {
                    VusString* sep = vus_string_new(", ");
                    VusString* t = vus_string_concat(acc, sep);
                    vus_unref(acc); vus_unref(sep); acc = t;
                }
                VusString* t = vus_string_concat(acc, item);
                vus_unref(acc); vus_unref(item); acc = t;
            }
            VusString* close = vus_string_new("]");
            VusString* out = vus_string_concat(acc, close);
            vus_unref(acc); vus_unref(close);
            return out;
        }
        case TYPE_DICT:
            /* v0.1 字典无遍历接口，返回占位表示 */
            return vus_string_new("{}");
        default:
            return vus_string_new("");
    }
}

VusString* vus_input(VusString* prompt) {
    if (prompt && prompt->data) {
        printf("%s", prompt->data);
        fflush(stdout);
    }
    char buf[4096];
    if (fgets(buf, sizeof(buf), stdin) == NULL) {
        return vus_string_new("");
    }
    int len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) {
        buf[len-1] = '\0';
        len--;
    }
    return vus_string_new(buf);
}

VusString* vus_add(VusString* a, VusString* b) {
    int err_a = 0, err_b = 0;
    int64_t na = vus_to_int(a, &err_a);
    int64_t nb = vus_to_int(b, &err_b);
    if (err_a == 0 && err_b == 0) {
        /* 两个都是合法数字，做算术加法 */
        return vus_to_string(na + nb);
    }
    /* 否则做字符串拼接 */
    return vus_string_concat(a, b);
}

int64_t vus_to_int(VusString* s, int* err) {
    if (!s || !s->data) {
        if (err) *err = 1;
        return 0;
    }
    char* endptr = NULL;
    int64_t result = strtoll(s->data, &endptr, 10);
    if (err) {
        *err = (endptr == s->data || *endptr != '\0') ? 1 : 0;
    }
    return result;
}

VusString* vus_to_string(int64_t n) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%lld", (long long)n);
    return vus_string_new(buf);
}

double vus_to_float(VusString* s, int* err) {
    if (!s || !s->data) {
        if (err) *err = 1;
        return 0.0;
    }
    char* endptr = NULL;
    double result = strtod(s->data, &endptr);
    if (err) {
        *err = (endptr == s->data || *endptr != '\0') ? 1 : 0;
    }
    return result;
}

// ============ 线程实现 ============

#include <pthread.h>
#include <unistd.h>  /* usleep / useconds_t，供 vus_thread_sleep */

struct VusThread {
    pthread_t thread;
    int detached;
};

// Thread wrapper struct
typedef struct {
    void* (*func)(void*);
    void* arg;
} VusThreadTask;

static void* vus_thread_wrapper(void* arg) {
    VusThreadTask* task = (VusThreadTask*)arg;
    void* result = task->func(task->arg);
    free(task);
    return result;
}

VusThread* vus_thread_create(void* (*func)(void*), void* arg) {
    VusThread* thread = (VusThread*)malloc(sizeof(VusThread));
    if (!thread) return NULL;
    thread->detached = 0;

    VusThreadTask* task = (VusThreadTask*)malloc(sizeof(VusThreadTask));
    task->func = func;
    task->arg = arg;

    if (pthread_create(&thread->thread, NULL, vus_thread_wrapper, task) != 0) {
        free(thread);
        free(task);
        return NULL;
    }
    return thread;
}

void* vus_thread_join(VusThread* thread) {
    if (!thread || thread->detached) return NULL;
    void* result;
    pthread_join(thread->thread, &result);
    thread->detached = 1;
    return result;
}

void vus_thread_detach(VusThread* thread) {
    if (!thread || thread->detached) return;
    pthread_detach(thread->thread);
    thread->detached = 1;
}

/* 睡眠：休眠毫秒。生成器把 睡眠(ms) 映射为 vus_thread_sleep(vus_to_string(ms))。
 * 用 usleep 跨平台休眠，nanosleep 更精确但部分嵌入式环境缺失 usleep 依赖。
 * Termux / Linux / macOS 均提供 usleep。 */
void vus_thread_sleep(VusString* ms) {
    int64_t msec = vus_to_int(ms, NULL);
    if (msec <= 0) return;
    /* 分组休眠，避免超大毫秒值乘 1000 溢出 */
    int64_t remaining_us = msec * 1000;
    while (remaining_us > 0) {
        useconds_t chunk = remaining_us > 1000000 ? 1000000 : (useconds_t)remaining_us;
        usleep(chunk);
        remaining_us -= chunk;
    }
}

/* ============ 线程/协程句柄接口 ============ */
/* 使用全局句柄注册表，避免指针类型转换问题 */

static void* vus_thread_handles[VUS_MAX_HANDLES];
static int vus_thread_handle_count = 0;
static void* vus_coro_handles[VUS_MAX_HANDLES];
static int vus_coro_handle_count = 0;

VusString* vus_thread_create_handle(void* (*func)(void*), void* arg) {
    VusThread* thread = vus_thread_create(func, arg);
    if (!thread) return vus_string_new("-1");
    int idx = vus_thread_handle_count++;
    if (idx >= VUS_MAX_HANDLES) {
        vus_thread_join(thread);
        free(thread);
        return vus_string_new("-1");
    }
    vus_thread_handles[idx] = thread;
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", idx);
    return vus_string_new(buf);
}

void* vus_thread_join_handle(VusString* handle) {
    if (!handle) return NULL;
    int idx = atoi(handle->data);
    if (idx < 0 || idx >= vus_thread_handle_count || !vus_thread_handles[idx]) {
        return NULL;
    }
    VusThread* thread = (VusThread*)vus_thread_handles[idx];
    void* result = vus_thread_join(thread);
    free(thread);
    vus_thread_handles[idx] = NULL;
    return result;
}

VusString* vus_coro_create_handle(void (*func)(void*), void* arg) {
    VusCoroutine* coro = vus_coro_create(func, arg);
    if (!coro) return vus_string_new("-1");
    int idx = vus_coro_handle_count++;
    if (idx >= VUS_MAX_HANDLES) {
        free(coro);
        return vus_string_new("-1");
    }
    vus_coro_handles[idx] = coro;
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", idx);
    return vus_string_new(buf);
}

void vus_coro_resume_handle(VusString* handle) {
    if (!handle) return;
    int idx = atoi(handle->data);
    if (idx < 0 || idx >= vus_coro_handle_count || !vus_coro_handles[idx]) {
        return;
    }
    VusCoroutine* coro = (VusCoroutine*)vus_coro_handles[idx];
    vus_coro_resume(coro);
    if (vus_coro_is_done(coro)) {
        free(coro);
        vus_coro_handles[idx] = NULL;
    }
}

// ============ 协程实现 ============
// 基于 setjmp / longjmp + 少量平台特定汇编栈切换。
// 不依赖 ucontext，可在 Android / Termux（armv8l aarch64）上编译通过。

#include "vus_coro.h"

VusCoroutine* vus_coro_create(void (*func)(void*), void* arg);
void          vus_coro_resume(VusCoroutine* coro);
void          vus_coro_yield(void);
int           vus_coro_is_done(VusCoroutine* coro);

/* ============ 插件运行时函数实现 ============ */

/* ---- TUI（ANSI 转义码） ---- */

VusString* vus_plugin_tui_clear(VusString* dummy) {
    (void)dummy;
    printf("\033[2J\033[H");
    fflush(stdout);
    return vus_string_new("");
}

VusString* vus_plugin_tui_set_color(VusString* fg, VusString* bg) {
    const char* c_fg = fg ? vus_string_cstr(fg) : "37";
    const char* c_bg = bg ? vus_string_cstr(bg) : "40";
    printf("\033[38;5;%sm\033[48;5;%sm", c_fg, c_bg);
    fflush(stdout);
    return vus_string_new("");
}

VusString* vus_plugin_tui_locate(VusString* row, VusString* col) {
    const char* c_row = row ? vus_string_cstr(row) : "1";
    const char* c_col = col ? vus_string_cstr(col) : "1";
    printf("\033[%s;%sH", c_row, c_col);
    fflush(stdout);
    return vus_string_new("");
}

VusString* vus_plugin_tui_progress(VusString* current, VusString* total, VusString* width) {
    int c = current ? atoi(vus_string_cstr(current)) : 0;
    int t = total ? atoi(vus_string_cstr(total)) : 100;
    int w = width ? atoi(vus_string_cstr(width)) : 20;
    if (t <= 0) t = 1;
    if (w <= 0) w = 20;
    int pct = (c * 100) / t;
    int bar_w = (c * w) / t;
    printf("\033[?25l[");  /* hide cursor */
    for (int i = 0; i < w; i++) {
        putchar(i < bar_w ? '=' : ' ');
    }
    printf("] %d%%\r", pct);
    fflush(stdout);
    if (c >= t) {
        printf("\033[?25h\n");  /* show cursor, newline */
    }
    return vus_string_new("");
}

VusString* vus_plugin_tui_reset(VusString* dummy) {
    (void)dummy;
    printf("\033[0m");
    fflush(stdout);
    return vus_string_new("");
}

/* ---- 网络（libcurl） ---- */

#ifdef VUS_HAVE_CURL
#include <curl/curl.h>

struct vus_mem_buf {
    char* data;
    size_t size;
};

static size_t vus_curl_write_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t total = size * nmemb;
    struct vus_mem_buf* buf = (struct vus_mem_buf*)userdata;
    char* new_data = (char*)realloc(buf->data, buf->size + total + 1);
    if (!new_data) return 0;
    buf->data = new_data;
    memcpy(buf->data + buf->size, ptr, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}

static CURL* vus_curl_easy(const char* url) {
    CURL* curl = curl_easy_init();
    if (!curl) return NULL;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "VUS/1.0");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    return curl;
}
#endif

VusString* vus_plugin_http_get(VusString* url) {
#ifdef VUS_HAVE_CURL
    if (!url) return vus_string_new("");
    const char* c_url = vus_string_cstr(url);
    CURL* curl = vus_curl_easy(c_url);
    if (!curl) return vus_string_new("");
    struct vus_mem_buf buf = {NULL, 0};
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, vus_curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) {
        free(buf.data);
        return vus_string_new("");
    }
    VusString* result = vus_string_new(buf.data ? buf.data : "");
    free(buf.data);
    return result;
#else
    (void)url;
    return vus_string_new("");
#endif
}

VusString* vus_plugin_http_post(VusString* url, VusString* data) {
#ifdef VUS_HAVE_CURL
    if (!url) return vus_string_new("");
    const char* c_url = vus_string_cstr(url);
    const char* c_data = data ? vus_string_cstr(data) : "";
    CURL* curl = vus_curl_easy(c_url);
    if (!curl) return vus_string_new("");
    struct vus_mem_buf buf = {NULL, 0};
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, c_data);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(c_data));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, vus_curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) {
        free(buf.data);
        return vus_string_new("");
    }
    VusString* result = vus_string_new(buf.data ? buf.data : "");
    free(buf.data);
    return result;
#else
    (void)url; (void)data;
    return vus_string_new("");
#endif
}

VusString* vus_plugin_http_download(VusString* url, VusString* filepath) {
#ifdef VUS_HAVE_CURL
    if (!url || !filepath) return vus_string_new("-1");
    const char* c_url = vus_string_cstr(url);
    const char* c_path = vus_string_cstr(filepath);
    CURL* curl = vus_curl_easy(c_url);
    if (!curl) return vus_string_new("-1");
    FILE* fp = fopen(c_path, "wb");
    if (!fp) { curl_easy_cleanup(curl); return vus_string_new("-1"); }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NULL);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    CURLcode res = curl_easy_perform(curl);
    fclose(fp);
    curl_easy_cleanup(curl);
    return vus_string_new(res == CURLE_OK ? "0" : "-1");
#else
    (void)url; (void)filepath;
    return vus_string_new("-1");
#endif
}

/* ---- 插件调用（.vux Python 插件） ---- */

/*
 * vus_plugin_run_vux — 调用已安装的 .vux Python 插件。
 *
 * 通过子进程执行：
 *   python3 <vux_plugin_manager.py> run <插件名> "<命令>" --raw
 * 并返回插件实际输出（stdout）。
 *
 * 脚本路径查找顺序：
 *   1. 环境变量 VUS_PLUGIN_MANAGER（指向 vux_plugin_manager.py）
 *   2. 环境变量 VUS_HOME/scripts/vux_plugin_manager.py
 *   3. 当前目录 scripts/vux_plugin_manager.py
 */
VusString* vus_plugin_run_vux(VusString* plugin, VusString* cmd) {
    if (!plugin || !cmd) return vus_string_new("");
    const char* c_plugin = vus_string_cstr(plugin);
    const char* c_cmd = vus_string_cstr(cmd);

    /* 定位插件管理器脚本 */
    char manager[1024] = {0};
    const char *env_mgr = getenv("VUS_PLUGIN_MANAGER");
    if (env_mgr && env_mgr[0]) {
        snprintf(manager, sizeof(manager), "%s", env_mgr);
    } else {
        const char *home = getenv("VUS_HOME");
        if (home && home[0]) {
            snprintf(manager, sizeof(manager), "%s/scripts/vux_plugin_manager.py", home);
        } else {
            snprintf(manager, sizeof(manager), "scripts/vux_plugin_manager.py");
        }
    }

    /* 构建命令。插件名与命令参数在 manager 脚本中作为 argv 传递。
     * 参数用单引号包裹：命令内可安全携带双引号（如 --json '{"a":1}'），
     * 避免 VUS 脚本构造的 JSON/引号破坏 shell 结构。 */
    char cmdline[8192];
    int n = snprintf(cmdline, sizeof(cmdline),
                     "python3 '%s' run '%s' '%s' --raw 2>/dev/null",
                     manager, c_plugin, c_cmd);
    if (n < 0 || n >= (int)sizeof(cmdline)) {
        return vus_string_new("");
    }

    FILE *fp = popen(cmdline, "r");
    if (!fp) return vus_string_new("");

    /* 读取全部输出 */
    size_t cap = 4096, len = 0;
    char *buf = (char*)malloc(cap);
    if (!buf) { pclose(fp); return vus_string_new(""); }
    size_t r;
    while ((r = fread(buf + len, 1, cap - len - 1, fp)) > 0) {
        len += r;
        if (len + 1 >= cap) {
            cap *= 2;
            char *nb = (char*)realloc(buf, cap);
            if (!nb) { free(buf); pclose(fp); return vus_string_new(""); }
            buf = nb;
        }
    }
    int status = pclose(fp);
    buf[len] = '\0';

    /* 去掉末尾换行 */
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) {
        buf[--len] = '\0';
    }

    /* 非零退出码视为失败，返回空串 */
    if (status != 0 && len == 0) {
        VusString* empty = vus_string_new("");
        free(buf);
        return empty;
    }

    VusString* result = vus_string_new_len(buf, (int)len);
    free(buf);
    return result;
}

/* =====================================================================
 * 进程内嵌入 Python 解释器
 * ---------------------------------------------------------------------
 * 通过 dlopen 惰性加载 libpython，用 dlsym 定位符号，避免编译期对
 * libpython 的硬依赖。开 VUS_USE_PY 才启用；否则这些函数回退到
 * 子进程方案或返回空值，保证无解释器环境仍可编译运行。
 * 注意：VUS 协程为单线程协作式调度，插件调用采用同步阻塞执行，
 * 调用期间协程不可切换（决策 #6）。
 * ===================================================================== */
#ifdef VUS_USE_PY
#include <dlfcn.h>

/* PyRun_String 的起始语法模式：Py_file_input（编译完整语句）。
 * 值为 CPython 头文件 pgenheader 中定义的枚举，此处命名化避免裸魔数。 */
#define VUS_PY_FILE_INPUT 257

/* ---- libpython 符号函数指针 ---- */
static void* (*vus_py_Py_InitializeFn)(void) = NULL;
static void  (*vus_py_Py_FinalizeFn)(void) = NULL;
static void* (*vus_py_PyImport_ImportModuleFn)(const char*) = NULL;
static void* (*vus_py_PyObject_CallFunctionFn)(void*, const char*, ...) = NULL;
static void* (*vus_py_PyObject_CallMethodFn)(void*, const char*, const char*, ...) = NULL;
static void* (*vus_py_PySys_GetObjectFn)(const char*) = NULL;
static void  (*vus_py_Py_XDECREF_Fn)(void*) = NULL;
static void  (*vus_py_PyErr_PrintFn)(void) = NULL;
static void* (*vus_py_PyErr_ClearFn)(void) = NULL;
static double(*vus_py_PyFloat_AsDoubleFn)(void*) = NULL;
static int   (*vus_py_PyList_SizeFn)(void*) = NULL;
static void* (*vus_py_PyList_GetItemFn)(void*, long) = NULL;
static int   (*vus_py_PyDict_NextFn)(void*, long*, void**, void**) = NULL;
static int   (*vus_py_PyDict_SizeFn)(void*) = NULL;
static void* (*vus_py_Py_BuildValueFn)(const char*, ...) = NULL;
static int   (*vus_py_PyObject_IsTrueFn)(void*) = NULL;
static int   (*vus_py_PySequence_CheckFn)(void*) = NULL;
static int   (*vus_py_PyMapping_CheckFn)(void*) = NULL;
static void* (*vus_py_PyObject_TypeFn)(void*) = NULL;
static const char* (*vus_py_PyUnicode_AsUTF8Fn)(void*) = NULL;
static void* (*vus_py_PyUnicode_FromStringFn)(const char*) = NULL;
static void* (*vus_py_PyObject_StrFn)(void*) = NULL;
static void* (*vus_py_PyList_NewFn)(long) = NULL;
static int   (*vus_py_PyList_AppendFn)(void*, void*) = NULL;
static void* (*vus_py_PyDict_NewFn)(void) = NULL;
static int   (*vus_py_PyDict_SetItemFn)(void*, void*, void*) = NULL;
static long (*vus_py_PyLong_AsLongFn)(void*) = NULL;
static void* (*vus_py_PyRun_StringFn)(const char*, int, void*, void*) = NULL;
static void* (*vus_py_PyObject_CallObjectFn)(void*, void*) = NULL;
static void* (*vus_py_PyImport_AddModuleFn)(const char*) = NULL;
static void* (*vus_py_PyModule_GetDictFn)(void*) = NULL;
static void* (*vus_py_PyDict_GetItemStringFn)(void*, const char*) = NULL;
static void* (*vus_py_PyEval_GetBuiltinsFn)(void) = NULL;

static void* vus_py_globals = NULL;
static void* vus_py_handle = NULL;
static void* vus_py_PyUnicode_Type = NULL;   /* &PyUnicode_Type，用于实现 PyUnicode_Check 宏 */
static int   vus_py_inited  = 0;
static int   vus_py_tried   = 0;

/* 查找一条符号；缺失则整套回退 */
static int vus_py_load_symbol(const char* name, void** out) {
    void* sym = dlsym(vus_py_handle, name);
    if (!sym) return -1;
    *out = sym;
    return 0;
}

int vus_py_init(void) {
    if (vus_py_inited) return 0;
    if (vus_py_tried)  return -1;   /* 已尝试过且失败，快速返回 */

    vus_py_tried = 1;
    /* 候选 soname：优先编译期注入的匹配版本（见 Makefile -DVUS_PY_SONAME），
     * 其次无版本符号链接，再回退若干常见版本，避免硬编码单一版本。 */
#ifdef VUS_PY_SONAME
    vus_py_handle = dlopen(VUS_PY_SONAME, RTLD_NOW | RTLD_GLOBAL);
#endif
    if (!vus_py_handle) vus_py_handle = dlopen("libpython3.so", RTLD_NOW | RTLD_GLOBAL);
    if (!vus_py_handle) vus_py_handle = dlopen("libpython3.14.so", RTLD_NOW | RTLD_GLOBAL);
    if (!vus_py_handle) vus_py_handle = dlopen("libpython3.12.so", RTLD_NOW | RTLD_GLOBAL);
    if (!vus_py_handle) return -1;

#define VSYM(n, f) if (vus_py_load_symbol(n, (void**)&f) != 0) { dlclose(vus_py_handle); vus_py_handle = NULL; return -1; }
    VSYM("Py_Initialize",            vus_py_Py_InitializeFn);
    VSYM("Py_Finalize",              vus_py_Py_FinalizeFn);
    VSYM("PyImport_ImportModule",    vus_py_PyImport_ImportModuleFn);
    VSYM("PyObject_CallFunction",    vus_py_PyObject_CallFunctionFn);
    VSYM("PyObject_CallMethod",      vus_py_PyObject_CallMethodFn);
    VSYM("PySys_GetObject",          vus_py_PySys_GetObjectFn);
    VSYM("Py_DecRef",                vus_py_Py_XDECREF_Fn);
    VSYM("PyErr_Print",              vus_py_PyErr_PrintFn);
    VSYM("PyErr_Clear",              vus_py_PyErr_ClearFn);
    VSYM("PyFloat_AsDouble",         vus_py_PyFloat_AsDoubleFn);
    VSYM("PyList_Size",              vus_py_PyList_SizeFn);
    VSYM("PyList_GetItem",           vus_py_PyList_GetItemFn);
    VSYM("PyDict_Next",              vus_py_PyDict_NextFn);
    VSYM("PyDict_Size",              vus_py_PyDict_SizeFn);
    VSYM("Py_BuildValue",            vus_py_Py_BuildValueFn);
    VSYM("PyObject_IsTrue",          vus_py_PyObject_IsTrueFn);
    VSYM("PySequence_Check",         vus_py_PySequence_CheckFn);
    VSYM("PyMapping_Check",          vus_py_PyMapping_CheckFn);
    VSYM("PyObject_Type",            vus_py_PyObject_TypeFn);
    VSYM("PyUnicode_AsUTF8",         vus_py_PyUnicode_AsUTF8Fn);
    VSYM("PyUnicode_FromString",     vus_py_PyUnicode_FromStringFn);
    VSYM("PyObject_Str",             vus_py_PyObject_StrFn);
    VSYM("PyList_New",               vus_py_PyList_NewFn);
    VSYM("PyList_Append",            vus_py_PyList_AppendFn);
    VSYM("PyDict_New",               vus_py_PyDict_NewFn);
    VSYM("PyDict_SetItem",           vus_py_PyDict_SetItemFn);
    VSYM("PyLong_AsLong",            vus_py_PyLong_AsLongFn);
    VSYM("PyObject_CallObject",     vus_py_PyObject_CallObjectFn);
    VSYM("PyRun_String",            vus_py_PyRun_StringFn);
    VSYM("PyDict_GetItemString",    vus_py_PyDict_GetItemStringFn);
    VSYM("PyImport_AddModule",      vus_py_PyImport_AddModuleFn);
    VSYM("PyModule_GetDict",        vus_py_PyModule_GetDictFn);
    VSYM("PyEval_GetBuiltins",      vus_py_PyEval_GetBuiltinsFn);
#undef VSYM

    /* PyUnicode_Type 是全局变量（非函数），用 dlsym 取地址 */
    vus_py_PyUnicode_Type = dlsym(vus_py_handle, "PyUnicode_Type");
    if (!vus_py_PyUnicode_Type) { dlclose(vus_py_handle); vus_py_handle = NULL; return -1; }

    vus_py_Py_InitializeFn();
    /* 初始化模块全局命名空间：__main__ 模块的 dict，供 PyRun_String 使用 */
    vus_py_globals = vus_py_PyModule_GetDictFn(vus_py_PyImport_AddModuleFn("__main__"));
    vus_py_inited = 1;
    return 0;
}

/* ---- PyObject -> VusObject / VusString 转换 ---- */

/* 释放一个 VusObject 容器（不递归释放内部引用，交由 VUS 引用计数管理） */
static void vus_py_vus_object_free(VusObject* obj) {
    free(obj);
}

static VusObject* vus_py_to_vus_object(void* pyobj);

static VusString* vus_py_unicode_to_vus(void* pyobj) {
    const char* s = vus_py_PyUnicode_AsUTF8Fn(pyobj);
    if (!s) return vus_string_new("");
    return vus_string_new(s);
}

static VusObject* vus_py_to_vus_object(void* pyobj) {
    if (!pyobj || pyobj == (void*)0x1 || pyobj == (void*)0x0) return NULL;
    if (vus_py_PyObject_TypeFn(pyobj) == vus_py_PyUnicode_Type) {
        VusObject* o = (VusObject*)calloc(1, sizeof(VusObject));
        if (!o) return NULL;
        o->magic = VUS_OBJECT_MAGIC;
        o->type = TYPE_STR;
        o->u.str = vus_py_unicode_to_vus(pyobj);
        return o;
    }
    if (vus_py_PySequence_CheckFn(pyobj)) {
        long n = vus_py_PyList_SizeFn(pyobj);
        if (n < 0) return NULL;
        VusList* list = vus_list_new(TYPE_MIXED);
        for (long i = 0; i < n; i++) {
            void* item = vus_py_PyList_GetItemFn(pyobj, i);
            VusObject* sub = vus_py_to_vus_object(item);
            if (sub) {
                /* 展开子对象：标量存字符串，列表/字典存容器 */
                if (sub->type == TYPE_LIST || sub->type == TYPE_DICT) {
                    vus_list_append(list, sub);
                } else {
                    vus_ref(sub->u.str);
                    vus_list_append(list, sub->u.str);
                    vus_py_vus_object_free(sub);
                }
            }
        }
        VusObject* o = (VusObject*)calloc(1, sizeof(VusObject));
        if (!o) return NULL;
        o->magic = VUS_OBJECT_MAGIC;
        o->type = TYPE_LIST;
        o->u.list = list;
        return o;
    }
    if (vus_py_PyMapping_CheckFn(pyobj)) {
        VusDict* dict = vus_dict_new();
        long pos = 0;
        void *key, *value;
        while (vus_py_PyDict_NextFn(pyobj, &pos, &key, &value)) {
            VusString* k = vus_py_unicode_to_vus(key);
            VusObject* sub = vus_py_to_vus_object(value);
            if (k && sub) {
                if (sub->type == TYPE_LIST || sub->type == TYPE_DICT) {
                    vus_dict_set(dict, k, sub);
                } else {
                    vus_ref(sub->u.str);
                    vus_dict_set(dict, k, sub->u.str);
                    vus_py_vus_object_free(sub);
                }
            }
            if (k) vus_unref(k);
        }
        VusObject* o = (VusObject*)calloc(1, sizeof(VusObject));
        if (!o) return NULL;
        o->magic = VUS_OBJECT_MAGIC;
        o->type = TYPE_DICT;
        o->u.dict = dict;
        return o;
    }
    /* 数字/布尔/标量：统一转字符串 */
    void* s = vus_py_PyObject_StrFn(pyobj);
    if (s) {
        VusObject* o = (VusObject*)calloc(1, sizeof(VusObject));
        if (o) { o->magic = VUS_OBJECT_MAGIC; o->type = TYPE_STR; o->u.str = vus_py_unicode_to_vus(s); }
        vus_py_Py_XDECREF_Fn(s);
        return o;
    }
    return NULL;
}

/* ---- JSON 解析 / 生成 ---- */

void* vus_json_parse(VusString* s) {
    if (!s || vus_py_init() != 0) return NULL;
    void* json_mod = vus_py_PyImport_ImportModuleFn("json");
    if (!json_mod) return NULL;
    void* result = vus_py_PyObject_CallMethodFn(json_mod, "loads", "(s)", vus_string_cstr(s));
    if (!result) {
        vus_py_PyErr_PrintFn();
        vus_py_PyErr_ClearFn();
        return NULL;
    }
    VusObject* obj = vus_py_to_vus_object(result);
    vus_py_Py_XDECREF_Fn(result);
    vus_py_Py_XDECREF_Fn(json_mod);
    return obj;
}

/* 将 VusObject 容器序列化为 Python 对象（用于 json.dumps） */
static void* vus_py_vus_to_py(VusObject* obj) {
    if (!obj) return NULL;
    if (obj->type == TYPE_STR) {
        if (obj->u.str) return vus_py_PyUnicode_FromStringFn(vus_string_cstr(obj->u.str));
        return NULL;
    }
    if (obj->type == TYPE_LIST && obj->u.list) {
        void* py_list = vus_py_PyList_NewFn(0);
        if (!py_list) return NULL;
        for (int i = 0; i < vus_list_len(obj->u.list); i++) {
            void* item = vus_list_get(obj->u.list, i);
            void* pitem = vus_py_vus_to_py((VusObject*)item);
            if (pitem) {
                vus_py_PyList_AppendFn(py_list, pitem);
                vus_py_Py_XDECREF_Fn(pitem);
            }
        }
        return py_list;
    }
    if (obj->type == TYPE_DICT && obj->u.dict) {
        void* py_dict = vus_py_PyDict_NewFn();
        if (!py_dict) return NULL;
        /* 简化字典转换：VUS 字典遍历接口 v0.1 未提供，返回空 dict */
        return py_dict;
    }
    return NULL;
}

VusString* vus_json_generate(void* obj) {
    if (!obj) return vus_string_new("");
    if (vus_py_init() != 0) return vus_string_new("");
    void* json_mod = vus_py_PyImport_ImportModuleFn("json");
    if (!json_mod) return vus_string_new("");
    void* pyval = vus_py_vus_to_py((VusObject*)obj);
    if (!pyval) return vus_string_new("");
    void* result = vus_py_PyObject_CallMethodFn(json_mod, "dumps", "(O)", pyval);
    if (!result) {
        vus_py_PyErr_PrintFn();
        vus_py_PyErr_ClearFn();
        return vus_string_new("");
    }
    VusString* out = vus_py_unicode_to_vus(result);
    vus_py_Py_XDECREF_Fn(result);
    vus_py_Py_XDECREF_Fn(pyval);
    vus_py_Py_XDECREF_Fn(json_mod);
    return out;
}

/* ---- 进程内插件调用 ---- */

/* ---- 进程内插件调用 ---- */

/* 构造内联 Python 助手：调用插件类 run(api, input_data)，返回结构化 JSON。
 * helper 通过 exec 注入后在模块内执行；返回 (json, code)。 */
static const char* VUS_PY_PLUGIN_HELPER =
    "import json, sys, os\n"
    "def _vus_run_plugin(plugin_root, plugin_name, input_data):\n"
    "    sys.path.insert(0, plugin_root)\n"
    "    for _cand in (os.path.join(plugin_root, '..', '..', 'scripts'),\n"
    "                  os.path.join(os.getcwd(), 'scripts')):\n"
    "        _cand = os.path.abspath(_cand)\n"
    "        if os.path.isfile(os.path.join(_cand, 'vux_plugin_entry.py')) and _cand not in sys.path:\n"
    "            sys.path.insert(0, _cand)\n"
    "    try:\n"
    "        from vux_plugin_entry import VuxPluginAPI, load_plugin\n"
    "        plugin = load_plugin(os.path.join(plugin_root, plugin_name))\n"
    "        if plugin is None:\n"
    "            return json.dumps({'ok': False, 'error': 'load_plugin failed', 'data': None})\n"
    "        api = VuxPluginAPI()\n"
    "        if plugin.init(api) != 0:\n"
    "            return json.dumps({'ok': False, 'error': 'init failed', 'data': None})\n"
    "        code, out = plugin.run(api, input_data)\n"
    "        plugin.cleanup(api)\n"
    "        return json.dumps({'ok': code == 0, 'code': code, 'data': out})\n"
    "    except Exception as e:\n"
    "        return json.dumps({'ok': False, 'error': str(e), 'data': None})\n"
    "    finally:\n"
    "        for _p in list(sys.path):\n"
    "            if _p and (_p == plugin_root or _p.endswith('scripts')):\n"
    "                sys.path.remove(_p)\n";

/* ---- 进程内插件调用 ---- */

/* 定位插件根目录：优先 VUS_PLUGIN_DIR，其次 VUS_HOME，默认当前目录 */
static int vus_py_resolve_plugin_root(char* out, size_t cap) {
    const char* d = getenv("VUS_PLUGIN_DIR");
    if (d && d[0]) { snprintf(out, cap, "%s", d); return 0; }
    const char* home = getenv("VUS_HOME");
    if (home && home[0]) { snprintf(out, cap, "%s", home); return 0; }
    snprintf(out, cap, ".");
    return 0;
}

/* 进程内调用插件，返回：
 *   structured=1 -> VusObject*（解析插件返回的 JSON 结构化数据）
 *   structured=0 -> VusString*（插件返回的原始字符串）
 * 失败时 structured=0 返回空串，structured=1 返回 NULL。 */

/* 从字典中取指定键的 PyObject*（借用引用） */
static void* vus_py_dict_get_str(void* dict, const char* key) {
    return vus_py_PyDict_GetItemStringFn(dict, key);
}

/* 将 PyObject*（Unicode）转为 UTF-8 C 字符串（借用引用） */
static const char* vus_py_unicode_to_cstr(void* obj) {
    return vus_py_PyUnicode_AsUTF8Fn(obj);
}

static void* vus_py_plugin_run_obj(VusString* plugin, VusString* cmd, int structured) {
    if (!plugin || !cmd) return structured ? NULL : vus_string_new("");
    if (vus_py_init() != 0) {
        return structured ? NULL : vus_string_new("");
    }

    char root[2048];
    vus_py_resolve_plugin_root(root, sizeof(root));
    const char* pname = vus_string_cstr(plugin);
    const char* input = vus_string_cstr(cmd);

    /* 注入助手源码到内建命名空间（start=VUS_PY_FILE_INPUT，编译完整语句） */
    if (vus_py_PyRun_StringFn(VUS_PY_PLUGIN_HELPER, VUS_PY_FILE_INPUT, vus_py_globals, vus_py_globals) == NULL) {
        vus_py_PyErr_PrintFn();
        vus_py_PyErr_ClearFn();
        return structured ? NULL : vus_string_new("");
    }

    /* 调用 _vus_run_plugin(root, pname, input) */
    void* args = vus_py_Py_BuildValueFn("(sss)", root, pname, input);
    if (!args) return structured ? NULL : vus_string_new("");
    void* result = vus_py_PyObject_CallObjectFn(vus_py_dict_get_str(vus_py_globals, "_vus_run_plugin"), args);
    vus_py_Py_XDECREF_Fn(args);
    if (!result) {
        vus_py_PyErr_PrintFn();
        vus_py_PyErr_ClearFn();
        return structured ? NULL : vus_string_new("");
    }

    /* result 是 JSON 字符串 */
    const char* js = vus_py_unicode_to_cstr(result);
    if (!js) { vus_py_Py_XDECREF_Fn(result); return structured ? NULL : vus_string_new(""); }

    if (structured) {
        /* 解析 JSON 为 VusObject；临时串用完即释放，避免泄漏 */
        VusString* tmp = vus_string_new(js);
        VusObject* obj = (VusObject*)vus_json_parse(tmp);
        vus_unref(tmp);
        vus_py_Py_XDECREF_Fn(result);
        return obj;
    } else {
        VusString* s = vus_string_new(js);
        vus_py_Py_XDECREF_Fn(result);
        return s;
    }
}

VusString* vus_plugin_run_vux_inproc(VusString* plugin, VusString* cmd) {
#ifdef VUS_USE_PY
    return (VusString*)vus_py_plugin_run_obj(plugin, cmd, 0);
#else
    return vus_plugin_run_vux(plugin, cmd);
#endif
}

void* vus_plugin_run_vux_json(VusString* plugin, VusString* cmd) {
#ifdef VUS_USE_PY
    return vus_py_plugin_run_obj(plugin, cmd, 1);
#else
    return NULL;
#endif
}

VusString* vus_typeof(void* obj) {
    if (!obj) return vus_string_new("空");
    /* 非结构化容器（普通 VusString*）视为字符串 */
    if (!vus_is_object(obj)) return vus_string_new("字符串");
    VusObject* o = (VusObject*)obj;
    switch (o->type) {
        case TYPE_INT:    return vus_string_new("整数");
        case TYPE_FLOAT:  return vus_string_new("浮点");
        case TYPE_STR:    return vus_string_new("字符串");
        case TYPE_BOOL:   return vus_string_new("布尔");
        case TYPE_LIST:   return vus_string_new("列表");
        case TYPE_DICT:   return vus_string_new("字典");
        default:          return vus_string_new("空");
    }
}

#else /* !VUS_USE_PY：降级实现 */

int vus_py_init(void) { return -1; }

VusString* vus_plugin_run_vux_inproc(VusString* plugin, VusString* cmd) {
    return vus_plugin_run_vux(plugin, cmd);
}

void* vus_plugin_run_vux_json(VusString* plugin, VusString* cmd) {
    (void)plugin; (void)cmd;
    return NULL;
}

void* vus_json_parse(VusString* s) { (void)s; return NULL; }
VusString* vus_json_generate(void* obj) { (void)obj; return vus_string_new(""); }
VusString* vus_typeof(void* obj) { (void)obj; return vus_string_new("空"); }

#endif /* VUS_USE_PY */

/* ---- 文件操作 ---- */

#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

VusString* vus_plugin_file_read(VusString* path) {
    if (!path) return vus_string_new("");
    const char* c_path = vus_string_cstr(path);
    FILE* fp = fopen(c_path, "rb");
    if (!fp) return vus_string_new("");
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return vus_string_new(""); }
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return vus_string_new(""); }
    rewind(fp);
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return vus_string_new(""); }
    size_t nread = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if ((long)nread != sz) { free(buf); return vus_string_new(""); }
    buf[sz] = '\0';
    VusString* result = vus_string_new_len(buf, (int)sz);
    free(buf);
    return result;
}

VusString* vus_plugin_file_write(VusString* path, VusString* content) {
    if (!path || !content) return vus_string_new("-1");
    const char* c_path = vus_string_cstr(path);
    const char* c_data = vus_string_cstr(content);
    FILE* fp = fopen(c_path, "wb");
    if (!fp) return vus_string_new("-1");
    size_t len = strlen(c_data);
    size_t written = fwrite(c_data, 1, len, fp);
    fclose(fp);
    return vus_string_new(written == len ? "0" : "-1");
}

VusString* vus_plugin_file_append(VusString* path, VusString* content) {
    if (!path || !content) return vus_string_new("-1");
    const char* c_path = vus_string_cstr(path);
    const char* c_data = vus_string_cstr(content);
    FILE* fp = fopen(c_path, "ab");
    if (!fp) return vus_string_new("-1");
    size_t len = strlen(c_data);
    size_t written = fwrite(c_data, 1, len, fp);
    fclose(fp);
    return vus_string_new(written == len ? "0" : "-1");
}

VusString* vus_plugin_file_exists(VusString* path) {
    if (!path) return vus_string_new("0");
    const char* c_path = vus_string_cstr(path);
    struct stat st;
    return vus_string_new(stat(c_path, &st) == 0 ? "1" : "0");
}

VusString* vus_plugin_file_delete(VusString* path) {
    if (!path) return vus_string_new("-1");
    const char* c_path = vus_string_cstr(path);
    return vus_string_new(remove(c_path) == 0 ? "0" : "-1");
}

VusString* vus_plugin_file_list(VusString* path) {
    if (!path) return vus_string_new("");
    const char* c_path = vus_string_cstr(path);
    DIR* dir = opendir(c_path);
    if (!dir) return vus_string_new("");
    size_t total = 0;
    struct dirent* entry;
    int count = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        total += strlen(entry->d_name) + 1;
        count++;
    }
    if (count == 0) { closedir(dir); return vus_string_new(""); }
    rewinddir(dir);
    char* list = (char*)malloc(total + 1);
    if (!list) { closedir(dir); return vus_string_new(""); }
    char* ptr = list;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        size_t len = strlen(entry->d_name);
        memcpy(ptr, entry->d_name, len);
        ptr += len;
        *ptr++ = '\n';
    }
    *ptr = '\0';
    closedir(dir);
    VusString* result = vus_string_new(list);
    free(list);
    return result;
}

/* ---- 日期时间 ---- */

#include <time.h>

VusString* vus_plugin_date_now(VusString* dummy) {
    (void)dummy;
    time_t t = time(NULL);
    struct tm* tm_info = localtime(&t);
    if (!tm_info) return vus_string_new("");
    char buf[20];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_info);
    return vus_string_new(buf);
}

VusString* vus_plugin_date_format(VusString* fmt) {
    if (!fmt) return vus_string_new("");
    const char* c_fmt = vus_string_cstr(fmt);
    time_t t = time(NULL);
    struct tm* tm_info = localtime(&t);
    if (!tm_info) return vus_string_new("");
    char buf[256];
    size_t ret = strftime(buf, sizeof(buf), c_fmt, tm_info);
    if (ret == 0) return vus_string_new("");
    return vus_string_new(buf);
}

VusString* vus_plugin_date_parse(VusString* str, VusString* fmt) {
    if (!str || !fmt) return vus_string_new("");
    const char* c_str = vus_string_cstr(str);
    const char* c_fmt = vus_string_cstr(fmt);
    struct tm tm_val;
    memset(&tm_val, 0, sizeof(tm_val));
    const char* ret = strptime(c_str, c_fmt, &tm_val);
    if (!ret) return vus_string_new("");
    time_t t = mktime(&tm_val);
    if (t == (time_t)-1) return vus_string_new("");
    struct tm* norm = localtime(&t);
    if (!norm) return vus_string_new("");
    char buf[20];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", norm);
    return vus_string_new(buf);
}

VusString* vus_plugin_date_timestamp(VusString* dummy) {
    (void)dummy;
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)time(NULL));
    return vus_string_new(buf);
}

VusString* vus_plugin_date_from_timestamp(VusString* ts) {
    if (!ts) return vus_string_new("");
    long long t_val = atoll(vus_string_cstr(ts));
    time_t t = (time_t)t_val;
    struct tm* tm_info = localtime(&t);
    if (!tm_info) return vus_string_new("");
    char buf[20];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_info);
    return vus_string_new(buf);
}

static struct tm* vus_get_tm(void) {
    time_t t = time(NULL);
    return localtime(&t);
}

VusString* vus_plugin_date_year(VusString* dummy) {
    (void)dummy;
    struct tm* tm_info = vus_get_tm();
    if (!tm_info) return vus_string_new("0");
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", tm_info->tm_year + 1900);
    return vus_string_new(buf);
}

VusString* vus_plugin_date_month(VusString* dummy) {
    (void)dummy;
    struct tm* tm_info = vus_get_tm();
    if (!tm_info) return vus_string_new("0");
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", tm_info->tm_mon + 1);
    return vus_string_new(buf);
}

VusString* vus_plugin_date_day(VusString* dummy) {
    (void)dummy;
    struct tm* tm_info = vus_get_tm();
    if (!tm_info) return vus_string_new("0");
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", tm_info->tm_mday);
    return vus_string_new(buf);
}

VusString* vus_plugin_date_hour(VusString* dummy) {
    (void)dummy;
    struct tm* tm_info = vus_get_tm();
    if (!tm_info) return vus_string_new("0");
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", tm_info->tm_hour);
    return vus_string_new(buf);
}

VusString* vus_plugin_date_minute(VusString* dummy) {
    (void)dummy;
    struct tm* tm_info = vus_get_tm();
    if (!tm_info) return vus_string_new("0");
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", tm_info->tm_min);
    return vus_string_new(buf);
}

VusString* vus_plugin_date_second(VusString* dummy) {
    (void)dummy;
    struct tm* tm_info = vus_get_tm();
    if (!tm_info) return vus_string_new("0");
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", tm_info->tm_sec);
    return vus_string_new(buf);
}