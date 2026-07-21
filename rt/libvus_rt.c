#include "libvus_rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    err->msg = msg;
    err->line = line;
    err->func = func;
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

void vus_print(VusString* s) {
    if (!s || !s->data) return;
    printf("%s", s->data);
    fflush(stdout);
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

// ============ 协程实现 ============
// 使用 ucontext 实现轻量级协程

#include <ucontext.h>

// 协程状态
typedef enum {
    CORO_READY,
    CORO_RUNNING,
    CORO_YIELDED,
    CORO_DONE
} CoroState;

struct VusCoroutine {
    CoroState state;
    void (*func)(void*);
    void* arg;
    ucontext_t ctx;
    char* stack;
};

// 主上下文（协程调度器的上下文）
static ucontext_t vus_coro_main_ctx;
static VusCoroutine* current_coro = NULL;

// 协程入口
static void coro_entry(void) {
    if (current_coro && current_coro->func) {
        current_coro->func(current_coro->arg);
    }
    if (current_coro) {
        current_coro->state = CORO_DONE;
    }
    // uc_link 设置为 &vus_coro_main_ctx，函数返回后自动切回
}

VusCoroutine* vus_coro_create(void (*func)(void*), void* arg) {
    VusCoroutine* coro = (VusCoroutine*)malloc(sizeof(VusCoroutine));
    if (!coro) return NULL;
    coro->state = CORO_READY;
    coro->func = func;
    coro->arg = arg;
    coro->stack = NULL;
    return coro;
}

void vus_coro_resume(VusCoroutine* coro) {
    if (!coro || coro->state == CORO_DONE) return;

    VusCoroutine* prev = current_coro;
    current_coro = coro;

    if (coro->state == CORO_READY) {
        coro->state = CORO_RUNNING;
        coro->stack = (char*)malloc(65536);
        if (!coro->stack) { current_coro = prev; return; }

        getcontext(&coro->ctx);
        coro->ctx.uc_stack.ss_sp = coro->stack;
        coro->ctx.uc_stack.ss_size = 65536;
        coro->ctx.uc_stack.ss_flags = 0;
        coro->ctx.uc_link = &vus_coro_main_ctx;

        makecontext(&coro->ctx, coro_entry, 0);
        swapcontext(&vus_coro_main_ctx, &coro->ctx);
    } else if (coro->state == CORO_YIELDED) {
        coro->state = CORO_RUNNING;
        swapcontext(&vus_coro_main_ctx, &coro->ctx);
    }

    if (coro->state == CORO_DONE) {
        free(coro->stack);
        coro->stack = NULL;
    }

    current_coro = prev;
}

void vus_coro_yield(void) {
    if (current_coro) {
        current_coro->state = CORO_YIELDED;
        swapcontext(&current_coro->ctx, &vus_coro_main_ctx);
    }
}

int vus_coro_is_done(VusCoroutine* coro) {
    return !coro || coro->state == CORO_DONE;
}