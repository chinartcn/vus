#define _GNU_SOURCE
#include "libvus_rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <elog.h>

/* yyjson：纯 C JSON 解析/生成库（rt/yyjson/，MIT 许可） */
#include "yyjson/yyjson.h"

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

/* ============ 字符串驻留（进程级小缓存） ============
 * 用于高频重复的键名/常量（如 界面_设置("计数", v) 的变量名），避免每次调用
 * 都 malloc+复制。语义：与 vus_string_new 同，返回 ref+1 的借用（调用方 vus_unref
 * 归还）；内容相同时返回缓存实例，缓存本身持有一份引用保证驻留存活，换出时释放。
 * 注：单线程（VUA 主线程）使用，不做锁。 */
#define VUS_INTERN_SLOTS 64
static VusString *g_intern_keys[VUS_INTERN_SLOTS];

static unsigned vus_intern_hash(const char *s, int len) {
    unsigned h = 5381;
    for (int i = 0; i < len; i++) h = h * 33 + (unsigned char)s[i];
    return h;
}

VusString* vus_string_intern(const char* s) {
    if (!s || !s[0]) return vus_string_new("");
    int len = (int)strlen(s);
    int slot = (int)(vus_intern_hash(s, len) % VUS_INTERN_SLOTS);
    VusString* v = g_intern_keys[slot];
    if (v && v->len == len && memcmp(v->data, s, (size_t)len) == 0) {
        vus_ref(v);                      /* 本次借用 */
        return v;
    }
    VusString* nv = vus_string_new_len(s, len);   /* ref=1 归缓存持有 */
    if (!nv) return vus_string_new("");
    if (g_intern_keys[slot]) {           /* 换出旧驻留，归还其缓存引用 */
        VusString* old = g_intern_keys[slot];
        g_intern_keys[slot] = NULL;
        vus_unref(old);
    }
    g_intern_keys[slot] = nv;
    vus_ref(nv);                         /* 本次借用（调用方归还） */
    return nv;
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

/* 从 VusObject 中解包列表/字典。字面量创建的列表/字典是 VusObject 包裹的。 */
VusList* vus_list_unwrap(void* obj) {
    if (vus_is_object(obj)) return ((VusObject*)obj)->u.list;
    return (VusList*)obj;
}
VusDict* vus_dict_unwrap(void* obj) {
    if (vus_is_object(obj)) return ((VusObject*)obj)->u.dict;
    return (VusDict*)obj;
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

/* 返回字典所有键（VusString*）构成的列表，元素为键副本，调用方负责 vus_unref。 */
VusList* vus_dict_keys(VusDict* dict) {
    VusList* keys = vus_list_new(TYPE_STR);
    if (!dict) return keys;
    struct DictImpl* impl = (struct DictImpl*)dict->impl;
    for (int i = 0; i < impl->size; i++) {
        DictEntry* entry = impl->buckets[i];
        while (entry) {
            vus_list_append(keys, vus_string_new_len(entry->key->data, entry->key->len));
            entry = entry->next;
        }
    }
    return keys;
}

/* 取结构化字典的键列表。脚本中字典均为 VusObject*（TYPE_DICT），
 * 直接传裸 VusDict* 不被接受（防误用）。 */
VusList* vus_dict_keys_of(void* obj) {
    if (!obj || !vus_is_object(obj)) return vus_list_new(TYPE_STR);
    VusObject* o = (VusObject*)obj;
    if (o->type == TYPE_DICT && o->u.dict) return vus_dict_keys(o->u.dict);
    return vus_list_new(TYPE_STR);
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

/* 兼容结构化值（JSON_查询/JSON_解析 返回的 VusObject）在比较/转数字场景的自动解包：
 * 标量(字符串)直接返回内部文本；列表/字典序列化为 JSON 文本（owned=1，调用方 vus_unref）。
 * 直接传 VusString* 时原样返回（owned=0），避免把 VusObject 当 VusString 解引用野指针。 */
static VusString *vus_value_unwrap(void *v, int *owned) {
    if (owned) *owned = 0;
    if (!v || !vus_is_object(v)) return (VusString *)v;
    VusObject *o = (VusObject *)v;
    if (o->type == TYPE_LIST || o->type == TYPE_DICT) {
        VusString *s = vus_json_generate(v);
        if (s && owned) *owned = 1;
        return s;
    }
    return o->u.str;
}

int64_t vus_to_int(VusString* s, int* err) {
    if (!s) {
        if (err) *err = 1;
        return 0;
    }
    int owned = 0;
    VusString *tmp = vus_value_unwrap(s, &owned);
    if (owned) {
        /* 容器（列表/字典）无法转数字 */
        if (err) *err = 1;
        vus_unref(tmp);
        return 0;
    }
    s = tmp;
    if (!s || !s->data) {
        if (err) *err = 1;
        return 0;
    }
    /* 快速路径：strtoll 只会从数字/正负号/空白（含 \t\n\v\f\r）开头完整解析；
     * 其余首字符（中文/普通文本常见）必然失败，直接短路，避免高频比较时两次 strtoll。 */
    char c0 = s->data[0];
    if (!((c0 >= '0' && c0 <= '9') || c0 == '+' || c0 == '-' ||
          c0 == ' ' || c0 == '\t' || c0 == '\n' || c0 == '\v' || c0 == '\f' || c0 == '\r')) {
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

// vus_compare：比较两个字符串。若两者都能解析为整数则按数值比较，
// 否则按字典序（strcmp）比较。返回 -1 / 0 / 1，供 == != < > <= >= 使用。
// 避免旧实现把非数字字符串都转成 0 导致 "abc" == "xyz" 被误判为真。
// 相等短路：strcmp==0 即字面相同（数值也必等），跳过两次 strtoll 数字解析。
int vus_compare(VusString* a, VusString* b) {
    int oa = 0, ob = 0;
    VusString *ta = vus_value_unwrap(a, &oa);
    VusString *tb = vus_value_unwrap(b, &ob);
    const char* ca = ta ? vus_string_cstr(ta) : "";
    const char* cb = tb ? vus_string_cstr(tb) : "";
    int r = strcmp(ca, cb);
    if (r != 0) {
        /* 字面不等但数值可能相等（如 "5" vs "05"）才需数字解析 */
        int err_a = 0, err_b = 0;
        int64_t na = vus_to_int(ta, &err_a);
        int64_t nb = vus_to_int(tb, &err_b);
        if (err_a == 0 && err_b == 0) {
            r = (na > nb) - (na < nb);
        }
    }
    if (oa) vus_unref(ta);
    if (ob) vus_unref(tb);
    return r;
}

double vus_to_float(VusString* s, int* err) {
    if (!s || !s->data) {
        if (err) *err = 1;
        return 0.0;
    }
    /* 快速路径：同 vus_to_int，非数字起始（中文/普通文本）直接短路 strtod */
    char c0 = s->data[0];
    if (!((c0 >= '0' && c0 <= '9') || c0 == '+' || c0 == '-' ||
          c0 == ' ' || c0 == '\t' || c0 == '\n' || c0 == '\v' || c0 == '\f' || c0 == '\r')) {
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

/* ============ 命令行参数支持（自举编译器 CLI 用） ============ */
static int s_cli_argc = 0;
static char** s_cli_argv = NULL;

void vus_cli_init(int argc, char** argv) {
    s_cli_argc = argc;
    s_cli_argv = argv;
}

VusString* vus_cli_argc(void) {
    return vus_to_string(s_cli_argc);
}

VusString* vus_cli_argv(VusString* index) {
    int i = (int)vus_to_int(index, NULL);
    if (i < 0 || i >= s_cli_argc || !s_cli_argv) return vus_to_string(0);
    return vus_string_new(s_cli_argv[i] ? s_cli_argv[i] : "");
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

/* ============ Java 平台能力桥（网络/文件等由 Java 暴露、VUS 调用） ============
 * APK 环境：jni_bridge 在 JNI_OnLoad 通过 vus_set_java_callback 注册回调；
 * VUS 的 网络_* / 文件_* 内建在 APK 内优先走 Java 实现（用户架构约定）。
 * 桌面/纯 native 环境未注册回调时，各内建自动回退到本文件的内建实现（stdio/curl）。
 * RPC 协议：入参 args 为 JSON 字符串；返回值 {"ok":1,"data":"..."} / {"ok":0,"err":"..."}。 */

static void (*g_java_cb)(const char *api, const char *args, char **out) = NULL;

void vus_set_java_callback(void (*fn)(const char *api, const char *args, char **out)) {
    g_java_cb = fn;
}

/* 单键值 JSON 参数构造（值做 JSON 字符串转义），返回 malloc 缓冲（调用方 free）。 */
static char *vus_java_json_kv(const char *key, const char *val) {
    if (!key) return NULL;
    if (!val) val = "";
    size_t klen = strlen(key), vlen = strlen(val), cap = klen + vlen + 16;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    char *p = out;
    *p++ = '{'; *p++ = '"';
    memcpy(p, key, klen); p += klen;
    *p++ = '"'; *p++ = ':'; *p++ = '"';
    for (size_t i = 0; i < vlen; i++) {
        char c = val[i];
        if (c == '"' || c == '\\') { *p++ = '\\'; }
        *p++ = c;
    }
    *p++ = '"'; *p++ = '}'; *p = '\0';
    return out;
}

/* 双键值 JSON 参数构造（{"k1":"v1","k2":"v2"}），值做 JSON 转义，返回 malloc 缓冲。 */
static char *vus_java_json_2(const char *k1, const char *v1, const char *k2, const char *v2) {
    if (!k1 || !k2) return NULL;
    if (!v1) v1 = "";
    if (!v2) v2 = "";
    size_t cap = strlen(k1) + strlen(v1) * 2 + strlen(k2) + strlen(v2) * 2 + 32;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    char *p = out;
    *p++ = '{'; *p++ = '"'; memcpy(p, k1, strlen(k1)); p += strlen(k1); *p++ = '"'; *p++ = ':'; *p++ = '"';
    for (size_t i = 0; i < strlen(v1); i++) { char c = v1[i]; if (c == '"' || c == '\\') *p++ = '\\'; *p++ = c; }
    *p++ = '"'; *p++ = ','; *p++ = '"'; memcpy(p, k2, strlen(k2)); p += strlen(k2); *p++ = '"'; *p++ = ':'; *p++ = '"';
    for (size_t i = 0; i < strlen(v2); i++) { char c = v2[i]; if (c == '"' || c == '\\') *p++ = '\\'; *p++ = c; }
    *p++ = '"'; *p++ = '}'; *p = '\0';
    return out;
}

/* 调用 Java 接口并解析返回 JSON：成功返回 data（新 VusString，调用方管理），失败返回 NULL。 */
static VusString *vus_java_rpc(const char *api, const char *args_json) {
    if (!g_java_cb || !api || !args_json) return NULL;
    char *out = NULL;
    g_java_cb(api, args_json, &out);
    if (!out) return NULL;
    VusString *resp = vus_string_new(out);
    free(out);
    if (!resp) return NULL;
    VusString *ret = NULL;
    yyjson_doc *doc = yyjson_read(vus_string_cstr(resp), (size_t)vus_string_len(resp), 0);
    if (doc) {
        yyjson_val *root = yyjson_doc_get_root(doc);
        if (root && yyjson_is_obj(root)) {
            yyjson_val *ok = yyjson_obj_get(root, "ok");
            if (ok && yyjson_is_bool(ok) && yyjson_get_bool(ok)) {
                yyjson_val *d = yyjson_obj_get(root, "data");
                if (d && yyjson_is_str(d)) ret = vus_string_new(yyjson_get_str(d));
            }
        }
        yyjson_doc_free(doc);
    }
    vus_unref(resp);
    return ret;
}

VusString* vus_plugin_http_get(VusString* url) {
    /* APK：Java 平台层实现；桌面回退 curl */
    if (url) {
        char *aj = vus_java_json_kv("url", vus_string_cstr(url));
        VusString *jr = aj ? vus_java_rpc("http.get", aj) : NULL;
        free(aj);
        if (jr) return jr;
    }
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
    /* APK：Java 平台层实现；桌面回退 curl */
    if (url) {
        char *aj = vus_java_json_2("url", vus_string_cstr(url), "data", data ? vus_string_cstr(data) : "");
        VusString *jr = aj ? vus_java_rpc("http.post", aj) : NULL;
        free(aj);
        if (jr) return jr;
    }
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
    /* APK：Java 平台层实现；桌面回退 curl */
    if (url && filepath) {
        char *aj = vus_java_json_2("url", vus_string_cstr(url), "path", vus_string_cstr(filepath));
        VusString *jr = aj ? vus_java_rpc("http.download", aj) : NULL;
        free(aj);
        if (jr) return jr;   /* Java 返回 data="1" 成功 / "0" 失败 */
    }
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

/* 拓展_调用（DEX 逻辑拓展，仅 APK）：把调用转给 Java 平台桥 ext.* 命名空间，
 * 原样返回 Java 响应 JSON 串（VUS 用 JSON_查询 取 ok/data/err）。
 * 桌面/纯 native 未注册 Java 回调时返回空串（DEX 拓展为 APK 独有能力，不回退）。 */
VusString* vus_plugin_ext_call(VusString* plugin_op, VusString* args) {
    if (!plugin_op || !g_java_cb) return vus_string_new("");
    const char *op = vus_string_cstr(plugin_op);
    if (!op || !op[0]) return vus_string_new("");
    char api[512];
    int n = snprintf(api, sizeof(api), "ext.%s", op);
    if (n <= 0 || n >= (int)sizeof(api)) return vus_string_new("");
    const char *astr = (args && vus_string_len(args) > 0) ? vus_string_cstr(args) : "{}";
    char *out = NULL;
    g_java_cb(api, astr, &out);
    if (!out) return vus_string_new("");
    VusString *resp = vus_string_new(out);
    free(out);
    return resp ? resp : vus_string_new("");
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
 * JSON 解析 / 生成 / 查询（基于 yyjson 纯 C 库，不依赖 Python）
 * ---------------------------------------------------------------------
 * 标量与容器约定与插件结构化数据一致：
 *   标量（字符串/数字/布尔/空）-> TYPE_STR 的 VusObject，值存字符串；
 *   对象/数组 -> TYPE_DICT / TYPE_LIST 容器。
 * ===================================================================== */

static VusObject* vus_json_scalar_wrap(VusString* s) {
    VusObject* o = (VusObject*)calloc(1, sizeof(VusObject));
    if (!o) return NULL;
    o->ref = 1;
    o->magic = VUS_OBJECT_MAGIC;
    o->type = TYPE_STR;
    o->u.str = s;
    return o;
}

static VusString* vus_json_number_to_string(double d) {
    char buf[64];
    if (d == (double)(long long)d) {
        snprintf(buf, sizeof(buf), "%lld", (long long)d);
    } else {
        snprintf(buf, sizeof(buf), "%.17g", d);
    }
    return vus_string_new(buf);
}

/* JSON 值 -> VusObject（标量转字符串；对象/数组转容器） */
static VusObject* vus_json_val_to_object(yyjson_val* val) {
    if (!val || yyjson_is_null(val)) {
        return vus_json_scalar_wrap(vus_string_new(""));
    }
    if (yyjson_is_str(val)) {
        return vus_json_scalar_wrap(vus_string_new(yyjson_get_str(val)));
    }
    if (yyjson_is_int(val)) {
        return vus_json_scalar_wrap(vus_json_number_to_string((double)yyjson_get_sint(val)));
    }
    if (yyjson_is_real(val)) {
        return vus_json_scalar_wrap(vus_json_number_to_string(yyjson_get_real(val)));
    }
    if (yyjson_is_bool(val)) {
        return vus_json_scalar_wrap(vus_string_new(yyjson_get_bool(val) ? "真" : "假"));
    }
    if (yyjson_is_obj(val)) {
        VusDict* dict = vus_dict_new();
        yyjson_obj_iter iter;
        yyjson_obj_iter_init(val, &iter);
        yyjson_val* key;
        while ((key = yyjson_obj_iter_next(&iter))) {
            yyjson_val* v = yyjson_obj_iter_get_val(key);
            VusObject* sub = vus_json_val_to_object(v);
            const char* ks = yyjson_get_str(key);
            VusString* k = vus_string_new(ks);
            if (sub) {
                if (sub->type == TYPE_LIST || sub->type == TYPE_DICT) {
                    vus_dict_set(dict, k, sub);
                } else {
                    vus_ref(sub->u.str);
                    vus_dict_set(dict, k, sub->u.str);
                    free(sub);
                }
            }
            vus_unref(k);
        }
        VusObject* o = (VusObject*)calloc(1, sizeof(VusObject));
        if (!o) return NULL;
        o->ref = 1; o->magic = VUS_OBJECT_MAGIC; o->type = TYPE_DICT; o->u.dict = dict;
        return o;
    }
    if (yyjson_is_arr(val)) {
        VusList* list = vus_list_new(TYPE_MIXED);
        yyjson_val* v;
        size_t idx, max = yyjson_arr_size(val);
        yyjson_arr_foreach(val, idx, max, v) {
            VusObject* sub = vus_json_val_to_object(v);
            if (sub) {
                if (sub->type == TYPE_LIST || sub->type == TYPE_DICT) {
                    vus_list_append(list, sub);
                } else {
                    vus_ref(sub->u.str);
                    vus_list_append(list, sub->u.str);
                    free(sub);
                }
            }
        }
        VusObject* o = (VusObject*)calloc(1, sizeof(VusObject));
        if (!o) return NULL;
        o->ref = 1; o->magic = VUS_OBJECT_MAGIC; o->type = TYPE_LIST; o->u.list = list;
        return o;
    }
    return NULL;
}

/* 将 UTF-8 字节串转为可在 JSON 中安全嵌入的字符串字面量（escape 由 yyjson 内部处理） */
static yyjson_mut_val* vus_json_mut_str(yyjson_mut_doc* doc, const char* s, int len) {
    if (!s) return yyjson_mut_null(doc);
    return yyjson_mut_strncpy(doc, s, (size_t)len);
}

/* VUS 对象 -> yyjson mut 值。标量（VusString*）直接作为字符串；容器递归展开。 */
static yyjson_mut_val* vus_json_vus_to_mut(yyjson_mut_doc* doc, void* obj) {
    if (!vus_is_object(obj)) {
        VusString* s = (VusString*)obj;
        return vus_json_mut_str(doc, s ? vus_string_cstr(s) : "", s ? vus_string_len(s) : 0);
    }
    VusObject* o = (VusObject*)obj;
    switch (o->type) {
        case TYPE_STR:
            return vus_json_mut_str(doc, vus_string_cstr(o->u.str), vus_string_len(o->u.str));
        case TYPE_LIST: {
            yyjson_mut_val* arr = yyjson_mut_arr(doc);
            VusList* list = o->u.list;
            if (list) {
                for (int i = 0; i < list->len; i++) {
                    yyjson_mut_arr_append(arr, vus_json_vus_to_mut(doc, list->items[i]));
                }
            }
            return arr;
        }
        case TYPE_DICT: {
            yyjson_mut_val* mv = yyjson_mut_obj(doc);
            VusDict* dict = o->u.dict;
            if (dict) {
                VusList* keys = vus_dict_keys(dict);
                for (int i = 0; i < keys->len; i++) {
                    VusString* k = (VusString*)keys->items[i];
                    void* val = vus_dict_get(dict, k);
                    yyjson_mut_val* mval = vus_json_vus_to_mut(doc, val);
                    yyjson_mut_val* mkey = yyjson_mut_strncpy(doc, k->data, (size_t)k->len);
                    yyjson_mut_obj_add(mv, mkey, mval);
                }
                vus_unref(keys);
            }
            return mv;
        }
        default:
            return yyjson_mut_null(doc);
    }
}

/* JSON_解析：解析 JSON 字符串为结构化对象 */
void* vus_json_parse(VusString* s) {
    if (!s) return NULL;
    yyjson_doc* doc = yyjson_read(vus_string_cstr(s), (size_t)vus_string_len(s), 0);
    if (!doc) return NULL;
    yyjson_val* root = yyjson_doc_get_root(doc);
    VusObject* o = root ? vus_json_val_to_object(root) : NULL;
    yyjson_doc_free(doc);
    return o;
}

/* JSON_生成：将结构化对象序列化为 JSON 字符串 */
VusString* vus_json_generate(void* obj) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);
    if (!doc) return vus_string_new("");
    yyjson_mut_val* root = vus_json_vus_to_mut(doc, obj);
    yyjson_mut_doc_set_root(doc, root);
    size_t len = 0;
    char* json = yyjson_mut_write(doc, 0, &len);
    yyjson_mut_doc_free(doc);
    if (!json) return vus_string_new("");
    VusString* out = vus_string_new_len(json, (int)len);
    free(json);
    return out;
}

/* 沿 JSON 取值后递归导航。读取一段：字段名或 [索引]。返回解析后的新 p，或 NULL。 */
void* vus_json_query(VusString* json, VusString* path) {
    if (!json || !path) return NULL;
    yyjson_doc* doc = yyjson_read(vus_string_cstr(json), (size_t)vus_string_len(json), 0);
    if (!doc) return NULL;
    yyjson_val* cur = yyjson_doc_get_root(doc);
    const char* p = vus_string_cstr(path);
    while (cur && *p) {
        if (*p == '.') { p++; continue; }
        if (*p == '[') {
            p++;
            long idx = strtol(p, (char**)&p, 10);
            if (*p == ']') p++;
            cur = (yyjson_is_arr(cur)) ? yyjson_arr_get(cur, (size_t)idx) : NULL;
        } else {
            char name[256];
            size_t n = 0;
            while (*p && *p != '.' && *p != '[' && n < sizeof(name) - 1) name[n++] = *p++;
            name[n] = '\0';
            cur = (yyjson_is_obj(cur)) ? yyjson_obj_get(cur, name) : NULL;
        }
    }
    VusObject* o = cur ? vus_json_val_to_object(cur) : NULL;
    yyjson_doc_free(doc);
    return o;
}

/* =====================================================================
 * Termux-X11 一键启动（Termux_* 内建）
 * ---------------------------------------------------------------------
 * 让 GUILT/GLES 脚本免去每次手动敲启动命令与环境变量：
 *   Termux_启动X11()：启动 termux-x11 :0 后台并设置 DISPLAY=:0
 *   Termux_启动GPU()：灌入 zink(virgl) 会用到的 MESA/GALLIUM 环境变量，
 *                     并启动 virgl_test_server(GLES, 无窗口 surface)
 * 在非 Termux 环境调用无害：命令不存在静默失败，仅 setenv 生效。
 * ===================================================================== */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
#endif
int vus_termux_start_x11(void)
{
    if (!getenv("DISPLAY") || !getenv("DISPLAY")[0])
    {
        system("termux-x11 :0 >/dev/null 2>&1 &");
        system("sleep 1");
        setenv("DISPLAY", ":0", 1);
    }
    return getenv("DISPLAY") ? 1 : 0;
}

int vus_termux_start_gl(void)
{
    setenv("MESA_NO_ERROR", "1", 1);
    setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 1);
    setenv("MESA_GLES_VERSION_OVERRIDE", "3.2", 1);
    setenv("GALLIUM_DRIVER", "zink", 1);
    setenv("ZINK_DESCRIPTORS", "lazy", 1);
    /* 4.3COMPAT 在 virgl 后台服务器(GLES)由 MESA_GLES_VERSION_OVERRIDE 兜底 */
    setenv("MESA_GL_VERSION_OVERRIDE", "4.3", 1);
    system("virgl_test_server --use-egl-surfaceless --use-gles >/dev/null 2>&1 &");
    return 1;
}
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

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

VusString* vus_typeof(void* obj) { (void)obj; return vus_string_new("空"); }

#endif /* VUS_USE_PY */

/* ---- 文件操作 ---- */

#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

VusString* vus_plugin_file_read(VusString* path) {
    /* APK：Java 平台层实现；桌面回退 stdio */
    if (path) {
        char *aj = vus_java_json_kv("path", vus_string_cstr(path));
        VusString *jr = aj ? vus_java_rpc("file.read", aj) : NULL;
        free(aj);
        if (jr) return jr;
    }
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
    /* APK：Java 平台层实现；桌面回退 stdio */
    if (path && content) {
        char *aj = vus_java_json_2("path", vus_string_cstr(path), "content", vus_string_cstr(content));
        VusString *jr = aj ? vus_java_rpc("file.write", aj) : NULL;
        free(aj);
        if (jr) return jr;
    }
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
    /* APK：Java 平台层实现；桌面回退 stdio */
    if (path && content) {
        char *aj = vus_java_json_2("path", vus_string_cstr(path), "content", vus_string_cstr(content));
        VusString *jr = aj ? vus_java_rpc("file.append", aj) : NULL;
        free(aj);
        if (jr) return jr;
    }
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
    /* APK：Java 平台层实现；桌面回退 stdio */
    if (path) {
        char *aj = vus_java_json_kv("path", vus_string_cstr(path));
        VusString *jr = aj ? vus_java_rpc("file.exists", aj) : NULL;
        free(aj);
        if (jr) return jr;
    }
    if (!path) return vus_string_new("0");
    const char* c_path = vus_string_cstr(path);
    struct stat st;
    return vus_string_new(stat(c_path, &st) == 0 ? "1" : "0");
}

VusString* vus_plugin_file_delete(VusString* path) {
    /* APK：Java 平台层实现；桌面回退 stdio */
    if (path) {
        char *aj = vus_java_json_kv("path", vus_string_cstr(path));
        VusString *jr = aj ? vus_java_rpc("file.delete", aj) : NULL;
        free(aj);
        if (jr) return jr;
    }
    if (!path) return vus_string_new("-1");
    const char* c_path = vus_string_cstr(path);
    return vus_string_new(remove(c_path) == 0 ? "0" : "-1");
}

VusString* vus_plugin_file_list(VusString* path) {
    /* APK：Java 平台层实现；桌面回退 stdio */
    if (path) {
        char *aj = vus_java_json_kv("path", vus_string_cstr(path));
        VusString *jr = aj ? vus_java_rpc("file.list", aj) : NULL;
        free(aj);
        if (jr) return jr;
    }
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

/* ---- 判断路径是否为目录（供"文件管理器"区分文件与目录） ---- */
#include <sys/stat.h>
VusString* vus_plugin_file_isdir(VusString* path) {
    /* APK：Java 平台层实现；桌面回退 stat */
    if (path) {
        char *aj = vus_java_json_kv("path", vus_string_cstr(path));
        VusString *jr = aj ? vus_java_rpc("file.isdir", aj) : NULL;
        free(aj);
        if (jr) return jr;
    }
    struct stat st;
    if (path && vus_string_cstr(path) && stat(vus_string_cstr(path), &st) == 0 && S_ISDIR(st.st_mode))
        return vus_string_new("true");
    return vus_string_new("false");
}

/* ---- shell 命令执行（供"终端"使用） ---- */
VusString* vus_plugin_shell_exec(VusString* cmd) {
    if (!cmd) return vus_string_new("");
    const char* c_cmd = vus_string_cstr(cmd);
    /* 合并 stderr 到 stdout，便于终端看到错误信息 */
    size_t clen = strlen(c_cmd);
    char* full = (char*)malloc(clen + 8);
    if (!full) return vus_string_new("");
    memcpy(full, c_cmd, clen);
    memcpy(full + clen, " 2>&1", 6);
    full[clen + 6] = '\0';

    FILE* fp = popen(full, "r");
    free(full);
    if (!fp) return vus_string_new("（命令执行失败：无法启动 shell）");

    const size_t cap = 65536;          /* 输出上限，防失控 */
    char* acc = (char*)malloc(cap);
    if (!acc) { pclose(fp); return vus_string_new(""); }
    size_t len = 0;
    char buf[4096];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (len + r >= cap) break;
        memcpy(acc + len, buf, r);
        len += r;
    }
    pclose(fp);
    acc[len] = '\0';
    VusString* result = vus_string_new_len(acc, (int)len);
    free(acc);
    return result;
}

/* ---- 文本分割：按分隔符拆成 JSON 数组字符串（供文件管理器逐行渲染目录） ---- */
VusString* vus_plugin_text_split(VusString* text, VusString* sep) {
    if (!text) return vus_string_new("[]");
    const char* s = vus_string_cstr(text);
    size_t slen = strlen(s);
    const char* sp = (sep && vus_string_cstr(sep)) ? vus_string_cstr(sep) : "\n";
    size_t splen = strlen(sp);
    if (splen == 0) splen = 1;

    yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val* arr = yyjson_mut_arr(doc);
    yyjson_mut_doc_set_root(doc, arr);

    const char* start = s;
    const char* p = s;
    const char* end = s + slen;
    while (p <= end) {
        size_t remain = (size_t)(end - p);
        if (splen <= remain && memcmp(p, sp, splen) == 0) {
            yyjson_mut_arr_append(arr, yyjson_mut_strncpy(doc, start, (size_t)(p - start)));
            p += splen;
            start = p;
            continue;
        }
        /* 到达末尾：追加最后一段（含末尾空段处理） */
        if (p == end) {
            yyjson_mut_arr_append(arr, yyjson_mut_strncpy(doc, start, (size_t)(p - start)));
            break;
        }
        p++;
    }
    size_t outlen = 0;
    char* out = yyjson_mut_write(doc, 0, &outlen);
    yyjson_mut_doc_free(doc);
    VusString* result = out ? vus_string_new_len(out, (int)outlen) : vus_string_new("[]");
    if (out) free(out);
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