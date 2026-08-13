#define _GNU_SOURCE
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