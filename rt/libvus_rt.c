#define _GNU_SOURCE
#include "libvus_rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>

/* dlopen/dlsym/dlclose：FFI Bridge C 域装载（无条件编译；Python 域嵌入段也复用） */
#include <dlfcn.h>

#include <elog.h>

/* 运行期外部桥公共头（FFI Bridge：VUS ↔ Python / C 插件） */
#include "../include/vus/vus_rt_bridge.h"

/* yyjson：纯 C JSON 解析/生成库（rt/yyjson/，MIT 许可） */
#include "yyjson/yyjson.h"

// ============ 引用计数通用操作 ============
// B4：ref 首字段用原子增减（__atomic 内置），使多线程对同一对象的
// vus_ref/vus_unref 不再构成数据竞争。归零判定用 fetch_sub 旧值==1；
// 对象自身内存生命周期（归零释放、池驻留借用）由各对象归属方与契约约束。*/

void vus_ref(void* obj) {
    if (!obj) return;
    __atomic_fetch_add((int*)obj, 1, __ATOMIC_RELAXED);
}

/* --- 递归释放（容器内部成员）---
 * 引用计数归零时，除释放自身外，还需释放其持有的内部成员，避免泄漏：
 *  - 字符串(VusString*)：单块分配，直接 free；
 *  - 容器(VusObject*)：按其类型递归释放内部列表/字典的每个成员，
 *    元素可再为标量或容器，逐层向下。
 * 环/过深引用用固定释放链(rel_chain)防无限递归与重复释放。 */
static void vus_object_release(VusObject* o);

/* --- 全局共享状态并发保护（B4） ---
 * g_lit_pool / g_numstr_pool / g_intern_keys / g_rel_chain 是进程级共享可变
 * 结构，多线程（协程/线程内建）下并发读写构成数据竞争。用一把进程级递归互斥
 * 锁保护上述池与释放链：递归锁保证 vus_object_release → vus_unref(元素) 的
 * 重入安全（同一线程重复加锁不死锁）。引用计数增减（ref--）若跨线程共享同一
 * 对象仍需外部同步（对象级语义，见 libvus_rt.h 契约说明）。 */
#include <pthread.h>
static pthread_mutex_t        g_pool_lock;
static pthread_once_t         g_pool_once = PTHREAD_ONCE_INIT;

static void vus_pool_lock_init(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_pool_lock, &attr);
    pthread_mutexattr_destroy(&attr);
}
#define VUS_POOL_LOCK()   (pthread_once(&g_pool_once, vus_pool_lock_init), \
                           pthread_mutex_lock(&g_pool_lock))
#define VUS_POOL_UNLOCK() (pthread_mutex_unlock(&g_pool_lock))

static void *g_rel_chain[256];
static int   g_rel_depth = 0;
static int rel_contains(void *p) {
    for (int i = 0; i < g_rel_depth; i++) if (g_rel_chain[i] == p) return 1;
    return 0;
}

void vus_unref(void* obj) {
    if (!obj) return;
    /* 原子归零判定：仅最后一个引用（旧值==1）触发释放 */
    if (__atomic_fetch_sub((int*)obj, 1, __ATOMIC_ACQ_REL) != 1) return;
    if (vus_is_object(obj)) {
        VUS_POOL_LOCK();               /* 保护释放链 */
        vus_object_release((VusObject*)obj);
        VUS_POOL_UNLOCK();
        return;
    }
    /* B1：脚本结构体实例（C 结构体，ref 后为 VUS_STRUCT_MAGIC）——归零释放必须
     * 先通过其 _release 钩子递归 vus_unref 各字段引用，再 free 主体；原先
     * 一律按非容器单块 free，字段持有的引用永久泄漏（与已修复 UAF 同源） */
    if (((VusStructHeader*)obj)->magic == VUS_STRUCT_MAGIC) {
        VusStructHeader *h = (VusStructHeader*)obj;
        if (h->release) {
            VUS_POOL_LOCK();
            h->release(obj);
            VUS_POOL_UNLOCK();
            return;
        }
        /* 无钩子（异常形态）：退化为仅释放外壳 */
    }
    /* 非容器：VusString 标量（单块分配），直接释放 */
    free(obj);
}

/* 变量赋值热路径：*slot = v（v/旧值均可 NULL）。生成器把原先
 * { _tmp = v; vus_ref(_tmp); vus_unref(*slot); *slot = _tmp; } 收敛成一行。
 * 值类型用 void*：普通赋值传 VusString*，列表/字典字面量传装箱后的 VusObject*，
 * 避免生成代码出现 -Wincompatible-pointer-types（反馈四.1）。 */
void vus_var_set(VusString** slot, void* v) {
    if (!slot) return;
    /* R5：同指针短路——循环内反复赋同一变量/常量时跳过 ref/unref 抖动
     * （同对象 ref+1 再 unref-1 净零，跳过语义完全等价）。 */
    if ((void*)*slot == v) return;
    if (v) vus_ref(v);
    if (*slot) vus_unref(*slot);
    *slot = (VusString*)v;
}

// ============ 字符串 ============

VusString* vus_string_new(const char* s) {
    if (!s) return NULL;
    int len = strlen(s);
    return vus_string_new_len(s, len);
}

VusString* vus_string_new_len(const char* s, int len) {
    /* 单块分配：头 + 负载一次 malloc（data 指向块尾），省一次 malloc 与释放 */
    VusString* str = (VusString*)malloc(sizeof(VusString) + (size_t)len + 1);
    if (!str) return NULL;
    str->ref = 1;
    str->len = len;
    str->data = (char*)(str + 1);
    if (s && len > 0) memcpy(str->data, s, (size_t)len);
    str->data[len] = '\0';
    return str;
}

/* ============ 字符串常量字面量池 ============
 * 生成器把源码字符串字面量替换为 vus_literal("...")：同内容返回同一常驻实例，
 * 省去高频路径每次 malloc+复制。VusString 内容不可变（无原地修改 API），
 * 池持保底引用，借出不增减引用计数、调用方不得释放/修改内容。单线程使用。 */
#define VUS_LIT_SLOTS 256
static VusString *g_lit_pool[VUS_LIT_SLOTS];

static unsigned vus_literal_hash(const char *s, int len) {
    unsigned h = 5381;
    for (int i = 0; i < len; i++) h = h * 33 + (unsigned char)s[i];
    return h;
}

VusString* vus_literal(const char* s) {
    if (!s) return NULL;
    int len = (int)strlen(s);
    int slot = (int)(vus_literal_hash(s, len) % VUS_LIT_SLOTS);
    VUS_POOL_LOCK();                 /* B4：池槽位并发保护 */
    VusString* v = g_lit_pool[slot];
    if (v && v->len == len && memcmp(v->data, s, (size_t)len) == 0) {
        VUS_POOL_UNLOCK();
        return v;
    }
    VusString* nv = vus_string_new_len(s, len);   /* ref=1 由池持有 */
    if (!nv) { VUS_POOL_UNLOCK(); return NULL; }
    /* 换出时不 unref 旧项：借出的指针可能已被 static 缓存/容器持久化，
     * 池换出若递减其引用会叠加到提前 free，导致悬垂（GUI 测试段错误）。
     * 改为永驻：旧项失链交给进程退出回收，借用恒有效。VUS 短生命周期
     * 程序里本文件字面量数量有限，代价可忽略。 */
    g_lit_pool[slot] = nv;
    VUS_POOL_UNLOCK();
    return nv;
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
    VUS_POOL_LOCK();                 /* B4：池槽位并发保护 */
    VusString* v = g_intern_keys[slot];
    if (v && v->len == len && memcmp(v->data, s, (size_t)len) == 0) {
        vus_ref(v);                      /* 本次借用 */
        VUS_POOL_UNLOCK();
        return v;
    }
    VusString* nv = vus_string_new_len(s, len);   /* ref=1 归缓存持有 */
    if (!nv) { VUS_POOL_UNLOCK(); return vus_string_new(""); }
    VusString* old = g_intern_keys[slot];
    if (old && __atomic_load_n(&old->ref, __ATOMIC_RELAXED) != 1) {
        /* B4：旧驻留仍有借用（跨线程共享中）——不换出（避免 free 借用中的
         * 实例），本次新实例仅借出放弃驻留；借用归还后 ref 回 1，下次再命中。 */
        VUS_POOL_UNLOCK();
        return nv;
    }
    if (old) {                          /* 换出旧驻留，归还其缓存引用 */
        g_intern_keys[slot] = NULL;
        vus_unref(old);
    }
    g_intern_keys[slot] = nv;
    vus_ref(nv);                         /* 本次借用（调用方归还） */
    VUS_POOL_UNLOCK();
    return nv;
}

/* 分配指定长度、data 未初始化的新串（ref=1；调用方自行填充 data）。 */
static VusString* vus_string_new_raw(int len) {
    VusString* s = (VusString*)malloc(sizeof(VusString) + (size_t)len + 1);
    if (!s) return NULL;
    s->ref = 1;
    s->len = len;
    s->data = (char*)(s + 1);
    return s;
}

VusString* vus_string_concat(VusString* a, VusString* b) {
    if (!a && !b) return vus_string_new("");
    if (!a || !a->data) return vus_string_new_len(b ? b->data : "", b ? b->len : 0);
    if (!b || !b->data) return vus_string_new_len(a->data, a->len);

    /* 直接写入新串 data，免去中间缓冲 + 二次复制 */
    VusString* result = vus_string_new_raw(a->len + b->len);
    if (!result) return NULL;
    memcpy(result->data, a->data, a->len);
    memcpy(result->data + a->len, b->data, b->len);
    result->data[a->len + b->len] = '\0';
    return result;
}

/* R3：多段拼接一次分配。n 段即一次 malloc（vus_string_new_raw）+ n 次 memcpy，
 * 替换 vus_string_concat 嵌套链（K 段 = K 次 malloc + 2K 次 memcpy）。
 * NULL 段按空串处理（与 vus_string_concat 的 NULL 容错一致）。 */
VusString* vus_string_concat_n(VusString** parts, int n) {
    int total = 0;
    for (int i = 0; i < n; i++) {
        if (parts[i] && parts[i]->data) total += parts[i]->len;
    }
    VusString* result = vus_string_new_raw(total);
    if (!result) return NULL;
    int pos = 0;
    for (int i = 0; i < n; i++) {
        if (parts[i] && parts[i]->data) {
            if (parts[i]->len > 0) memcpy(result->data + pos, parts[i]->data, (size_t)parts[i]->len);
            pos += parts[i]->len;
        }
    }
    result->data[pos] = '\0';
    result->len = pos;
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

/* 列表图片段：字面量求值创建 VusObject(TYPE_LIST) 装箱（与生成器旧内联模板
 * 完全等价：ref 初值 0 → 首次 vus_ref 后为 1，vus_unref 到 0 即释放）。
 * 供生成器把多行内联简化为一行，减少生成 C 体积。 */
VusObject* vus_object_list(void) {
    VusObject* o = (VusObject*)calloc(1, sizeof(VusObject));
    if (!o) return NULL;
    o->magic = VUS_OBJECT_MAGIC;
    o->type = TYPE_LIST;
    o->u.list = vus_list_new(TYPE_MIXED);
    return o;
}

VusObject* vus_object_dict(void) {
    VusObject* o = (VusObject*)calloc(1, sizeof(VusObject));
    if (!o) return NULL;
    o->magic = VUS_OBJECT_MAGIC;
    o->type = TYPE_DICT;
    o->u.dict = vus_dict_new();
    return o;
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

/* 取结构化字典的全部键值对列表（VusObject* TYPE_LIST）。
 * 每个元素为一个「[键, 值]」双元素列表（VusObject* TYPE_LIST）：
 * 键/值按 vus_list_append 引用计数规则各自持有新引用，pair 又由 out 持有。
 * 供「字典_项」内建配合「循环 对 在 字典_项(字典)」遍历键和值。 */
VusObject* vus_dict_items(void* obj) {
    VusObject* out = vus_object_list();
    if (!out) return NULL;
    if (!obj || !vus_is_object(obj)) return out;
    VusObject* o = (VusObject*)obj;
    if (o->type != TYPE_DICT || !o->u.dict) return out;
    struct DictImpl* impl = (struct DictImpl*)o->u.dict->impl;
    for (int i = 0; i < impl->size; i++) {
        DictEntry* e = impl->buckets[i];
        while (e) {
            VusObject* pair = vus_object_list();
            vus_list_append(pair->u.list, e->key);
            vus_list_append(pair->u.list, e->value);
            vus_list_append(out->u.list, pair);
            e = e->next;
        }
    }
    return out;
}

// ============ 容器递归释放（配合 vus_unref 归零路径） ============

/* 释放列表内部：逐元素按引用计数 vus_unref（元素可为标量或容器），
 * 再释放 items 数组与列表头。元素为共享引用时不归零释放，由计数保护。 */
static void vus_release_list(VusList* l) {
    if (!l) return;
    if (l->items) {
        for (int i = 0; i < l->len; i++)
            if (l->items[i]) vus_unref(l->items[i]);
        free(l->items);
    }
    free(l);
}

/* 释放字典内部：遍历桶内全部条目，释放键与值（按引用计数），再释放桶/impl/字典头。 */
static void vus_release_dict(VusDict* d) {
    if (!d) return;
    struct DictImpl* impl = (struct DictImpl*)d->impl;
    if (impl) {
        for (int i = 0; i < impl->size; i++) {
            DictEntry* e = impl->buckets[i];
            while (e) {
                DictEntry* next = e->next;
                if (e->key)   vus_unref(e->key);
                if (e->value) vus_unref(e->value);
                free(e);
                e = next;
            }
        }
        free(impl->buckets);
        free(impl);
    }
    free(d);
}

/* 递归释放 VusObject 容器。释放链防环/防过深（自身已在链上则跳过，
 * 避免无限递归与重复释放）；内层列表/字典为对象专属成员，直接整树回收。 */
static void vus_object_release(VusObject* o) {
    if (!vus_is_object(o)) return;
    if (g_rel_depth >= 256 || rel_contains(o)) return;
    g_rel_chain[g_rel_depth++] = o;
    if (o->type == TYPE_LIST && o->u.list) {
        vus_release_list(o->u.list);
    } else if (o->type == TYPE_DICT && o->u.dict) {
        vus_release_dict(o->u.dict);
    } else if (o->type == TYPE_STR && o->u.str) {
        vus_unref(o->u.str);   /* 持有串按引用计数回收（可共享/驻留） */
    }
    g_rel_depth--;
    free(o);
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

VusError* vus_error_new_typed(int code, const char* type, const char* msg, int line, const char* func) {
    VusError* err = (VusError*)malloc(sizeof(VusError));
    if (!err) return NULL;
    err->code = code;
    err->type = (type && type[0]) ? strdup(type) : strdup("错误");
    err->msg = msg ? strdup(msg) : NULL;
    err->line = line;
    err->func = func ? strdup(func) : NULL;
    err->next = NULL;
    return err;
}

VusError* vus_error_new(int code, const char* msg, int line, const char* func) {
    return vus_error_new_typed(code, NULL, msg, line, func);
}

/* except 类型匹配：name 与 err->type 相等，或与 err->msg 相等（旧「消息即类型」用法）即命中 */
int vus_error_matches(VusError* err, const char* name) {
    if (!err || !name || !name[0]) return 0;
    const char* t = err->type;
    const char* m = err->msg;
    return (t && t[0] && strcmp(t, name) == 0) ||
           (m && m[0] && strcmp(m, name) == 0);
}

void vus_error_push(VusError** chain, VusError* err) {
    if (!chain || !err) return;
    err->next = *chain;
    *chain = err;
}

void vus_error_print(VusError* err) {
    if (!err) return;
    fprintf(stderr, "E%03d[%s] %s (代码行号: %d)\n", err->code,
            err->type ? err->type : "错误", err->msg ? err->msg : "", err->line);
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
        free((void*)err->type);
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
            /* R4：一次性增长缓冲拼装，替换逐元素 new 分隔符 + concat（≈2n 次 malloc）。
             * 最后包一次 VusString（单次 memcpy），整体远低于原 2n 次分配。 */
            size_t cap = 16, len = 0;
            char* buf = (char*)malloc(cap);
            if (!buf) return vus_string_new("[]");
            buf[len++] = '[';
            int n = vus_list_len(list);
            for (int i = 0; i < n; i++) {
                if (i > 0) {
                    static const char sep[] = ", ";
                    if (len + 2 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
                    memcpy(buf + len, sep, 2); len += 2;
                }
                VusString* item = vus_object_to_string(vus_list_get(list, i));
                if (!item) item = vus_string_new("");
                if (len + (size_t)item->len + 1 >= cap) {
                    while (len + (size_t)item->len + 1 >= cap) cap *= 2;
                    buf = (char*)realloc(buf, cap);
                }
                memcpy(buf + len, item->data, (size_t)item->len);
                len += (size_t)item->len;
                vus_unref(item);
            }
            if (len + 1 >= cap) { buf = (char*)realloc(buf, cap + 1); }
            buf[len++] = ']';
            buf[len] = '\0';
            VusString* out = vus_string_new_len(buf, (int)len);
            free(buf);
            return out;
        }
        case TYPE_DICT: {
            struct DictImpl* impl = o->u.dict ? (struct DictImpl*)o->u.dict->impl : NULL;
            /* R4：字典真实序列化 {k: v, ...}（修复原 "{}" 占位语义缺失） */
            if (!impl || impl->count == 0) return vus_string_new("{}");
            size_t cap = 16, len = 0;
            char* buf = (char*)malloc(cap);
            if (!buf) return vus_string_new("{}");
            buf[len++] = '{';
            int first = 1;
            for (int bi = 0; bi < impl->size; bi++) {
                DictEntry* e = impl->buckets[bi];
                while (e) {
                    if (!first) {
                        static const char sep[] = ", ";
                        if (len + 2 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
                        memcpy(buf + len, sep, 2); len += 2;
                    }
                    first = 0;
                    if (len + (size_t)e->key->len + 3 >= cap) {
                        while (len + (size_t)e->key->len + 3 >= cap) cap *= 2;
                        buf = (char*)realloc(buf, cap);
                    }
                    memcpy(buf + len, e->key->data, (size_t)e->key->len); len += (size_t)e->key->len;
                    static const char kvsep[] = ": ";
                    memcpy(buf + len, kvsep, 2); len += 2;
                    VusString* vs = vus_object_to_string(e->value);
                    if (!vs) vs = vus_string_new("");
                    if (len + (size_t)vs->len + 1 >= cap) {
                        while (len + (size_t)vs->len + 1 >= cap) cap *= 2;
                        buf = (char*)realloc(buf, cap);
                    }
                    memcpy(buf + len, vs->data, (size_t)vs->len); len += (size_t)vs->len;
                    vus_unref(vs);
                    e = e->next;
                }
            }
            if (len + 1 >= cap) { buf = (char*)realloc(buf, cap + 1); }
            buf[len++] = '}';
            buf[len] = '\0';
            VusString* out = vus_string_new_len(buf, (int)len);
            free(buf);
            return out;
        }
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

/* 兼容结构化值（JSON_查询/JSON_解析 返回的 VusObject）在比较/转数字场景的自动解包：
 * 标量(字符串)直接返回内部文本；列表/字典序列化为 JSON 文本（owned=1，调用方 vus_unref）。
 * 直接传 VusString* 时原样返回（owned=0），避免把 VusObject 当 VusString 解引用野指针。 */
static VusString *vus_value_unwrap(void *v, int *owned);

VusString* vus_add(VusString* a, VusString* b) {
    int oa = 0, ob = 0;
    VusString *ta = vus_value_unwrap(a, &oa);
    VusString *tb = vus_value_unwrap(b, &ob);

    /* 快速路径（同 vus_compare/vus_to_int 的首字符短路）：两参首字符都不是
     * 数字/正负号/空白 → 必非数字 → 直接字符串拼接，省两次 strtoll。
     * 文本拼接是最常见场景（日志/插值消息），收益立竿见影。 */
    if (ta && tb && ta->data && tb->data) {
        char ca = ta->data[0], cb = tb->data[0];
        int na = (ca >= '0' && ca <= '9') || ca == '+' || ca == '-' ||
                 ca == ' ' || ca == '\t' || ca == '\n' || ca == '\v' || ca == '\f' || ca == '\r';
        int nb = (cb >= '0' && cb <= '9') || cb == '+' || cb == '-' ||
                 cb == ' ' || cb == '\t' || cb == '\n' || cb == '\v' || cb == '\f' || cb == '\r';
        if (!na && !nb) {
            VusString *r = vus_string_concat(ta, tb);
            if (oa) vus_unref(ta);
            if (ob) vus_unref(tb);
            return r;
        }
    }

    int err_a = 0, err_b = 0;
    int64_t na = vus_to_int(ta, &err_a);
    int64_t nb = vus_to_int(tb, &err_b);
    if (err_a == 0 && err_b == 0) {
        /* 两个都是合法数字，做算术加法 */
        VusString *r = vus_to_string(na + nb);
        if (oa) vus_unref(ta);
        if (ob) vus_unref(tb);
        return r;
    }
    /* 否则做字符串拼接 */
    VusString *r = vus_string_concat(ta, tb);
    if (oa) vus_unref(ta);
    if (ob) vus_unref(tb);
    return r;
}

/* 兼容结构化值（JSON_查询/JSON_解析 返回的 VusObject）在比较/转数字场景的自动解包：
 * 标量(字符串)直接返回内部文本；列表/字典序列化为 JSON 文本（owned=1，调用方 vus_unref）。
 * 直接传 VusString* 时原样返回（owned=0），避免把 VusObject 当 VusString 解引用野指针。 */
static VusString *vus_value_unwrap(void *v, int *owned) {
    if (owned) *owned = 0;
    if (!v || !vus_is_object(v)) {
        /* B3：脚本结构体实例不是 VusString，直接按串读 ref 后的 data 会读到
         * 字段区野指针（offset 8 是 _release 或首个字段指针）。magic 判定后
         * 返回 NULL，由调用方置 err（转数字/浮点失败），不再越界解引用。 */
        if (v && ((VusStructHeader *)v)->magic == VUS_STRUCT_MAGIC) return NULL;
        return (VusString *)v;
    }
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

/* 整数 → 字符串驻留缓存（同 vus_string_intern 借用语义）：热循环里 _i/计数器
 * 的 vus_to_string(N) 命中缓存实例，免每次 malloc+复制。VusString 内容不可变。
 * 返回 ref+1 借用（调用方 vus_unref 归还）；缓存自身持保底引用，换出时释放。 */
#define VUS_NUMSTR_SLOTS 128
static VusString *g_numstr_pool[VUS_NUMSTR_SLOTS];

VusString* vus_to_string(int64_t n) {
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%lld", (long long)n);
    if (len < 0) len = 0;
    if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;

    unsigned slot = ((unsigned)n * 2654435761u) % VUS_NUMSTR_SLOTS;
    VUS_POOL_LOCK();                 /* B4：数字驻留池并发保护 */
    VusString *cached = g_numstr_pool[slot];
    if (cached && cached->len == len && memcmp(cached->data, buf, (size_t)len) == 0) {
        vus_ref(cached);               /* 本次借用 */
        VUS_POOL_UNLOCK();
        return cached;
    }
    VusString *nv = vus_string_new_len(buf, len);
    if (!nv) { VUS_POOL_UNLOCK(); return NULL; }
    VusString *old = g_numstr_pool[slot];
    if (old && __atomic_load_n(&old->ref, __ATOMIC_RELAXED) != 1) {
        /* B4：旧驻留仍有借用——不换出（避免 free 借用中的实例），
         * 新实例仅借出放弃驻留；借用归还后 ref 回 1，下次同槽再命中/置换。 */
        VUS_POOL_UNLOCK();
        return nv;
    }
    if (old) {                     /* 换出旧驻留 */
        g_numstr_pool[slot] = NULL;
        vus_unref(old);
    }
    g_numstr_pool[slot] = nv;          /* 缓存保底引用 */
    vus_ref(nv);                       /* 本次借用（调用方 unref 归还） */
    VUS_POOL_UNLOCK();
    return nv;
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
    if (!s) {
        if (err) *err = 1;
        return 0.0;
    }
    /* B3：与 vus_to_int 对称做容器解包——列表/字典序列化为 JSON 后不可转浮点
     * （置 err）；结构体实例经 vus_value_unwrap 判 magic 返回 NULL（置 err），
     * 不再按 VusString 直读 data 造成越界解引用。 */
    int owned = 0;
    VusString *tmp = vus_value_unwrap(s, &owned);
    if (owned) {
        if (err) *err = 1;
        vus_unref(tmp);
        return 0.0;
    }
    s = tmp;
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

/* pthread.h 已在顶部（B4 全局池锁）包含；usleep 供 vus_thread_sleep */
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
void vus_coro_store_result(void* result);
VusCoroutine* vus_coro_current(void);
void* vus_coro_take_result(VusCoroutine* coro);

/* 真 await：驱动协程到完成（可能需多次 resume 越过让出点），并返回其结果。
 * 完成后的协程会被释放并清空句柄槽。 */
VusString* vus_coro_await_handle(VusString* handle) {
    if (!handle) return NULL;
    int idx = atoi(handle->data);
    if (idx < 0 || idx >= vus_coro_handle_count || !vus_coro_handles[idx]) {
        return NULL;
    }
    VusCoroutine* coro = (VusCoroutine*)vus_coro_handles[idx];
    /* 驱动直到完成 */
    int guard = 0;
    while (!vus_coro_is_done(coro) && guard < 1000000) {
        vus_coro_resume(coro);
        guard++;
    }
    void* res = vus_coro_take_result(coro);
    VusString* out = NULL;
    if (res) out = (VusString*)res;
    /* 复用现成资源回收：释放协程并清槽 */
    vus_coro_free(coro);
    vus_coro_handles[idx] = NULL;
    if (out) vus_ref(out);
    else out = vus_string_new("");
    return out;
}

/* ============ 插件运行时函数实现 ============ */

/* ---- TUI（ANSI 转义码） ---- */

VusString* vus_plugin_tui_clear(VusString* dummy) {
    (void)dummy;
    printf("\033[2J\033[H");
    fflush(stdout);
    /* 画布联动：清空帧 + 失效差分基准（下次刷新全量输出） */
    vus_tui_canvas_resize(0, 0);
    vus_tui_canvas_clear();
    return vus_string_new("");
}

VusString* vus_plugin_tui_set_color(VusString* fg, VusString* bg) {
    int f = fg ? atoi(vus_string_cstr(fg)) : -1;
    int b = bg ? atoi(vus_string_cstr(bg)) : -1;
    vus_tui_color_clamp(&f, &b);
    printf("\033[38;5;%dm\033[48;5;%dm", f >= 0 ? f : 37, b >= 0 ? b : 40);
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

/* ---- 网络回退辅助 ---- */

/* shell 单引号转义：`'` → `'\''`，整体用单引号包裹，返回 malloc 缓冲（调用方 free）。
 * 用于把 URL / 数据 / 文件路径安全嵌入 curl 命令行，避免引号、&、$ 等破坏 shell 结构。 */
static char *vus_sh_squote(const char *s) {
    if (!s) return NULL;
    size_t need = 3; /* 首尾两枚单引号 + NUL */
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

/* 网络回退到系统 curl 命令构造：无 libcurl 开发库时仍可发起请求。
 * 返回完整命令行（调用方 vus_plugin_shell_exec 执行），失败返回 NULL。 */
static char *vus_curl_cli(const char *url, const char *data, const char *out_path,
                          const char *upload_path, long timeout_s) {
    char *qu = vus_sh_squote(url);
    if (!qu) return NULL;
    char cmd[8192];
    int n;
    if (upload_path) {
        char *qp = vus_sh_squote(upload_path);
        if (!qp) { free(qu); return NULL; }
        n = snprintf(cmd, sizeof(cmd), "curl -s -L -m %ld -F 'file=@%s' %s 2>/dev/null",
                     timeout_s, qp, qu);
        free(qp);
    } else if (out_path) {
        char *qo = vus_sh_squote(out_path);
        if (!qo) { free(qu); return NULL; }
        n = snprintf(cmd, sizeof(cmd), "curl -s -L -m %ld -o %s %s 2>/dev/null",
                     timeout_s, qo, qu);
        free(qo);
    } else if (data) {
        char *qd = vus_sh_squote(data);
        if (!qd) { free(qu); return NULL; }
        n = snprintf(cmd, sizeof(cmd), "curl -s -L -m %ld -d %s %s 2>/dev/null",
                     timeout_s, qd, qu);
        free(qd);
    } else {
        n = snprintf(cmd, sizeof(cmd), "curl -s -L -m %ld %s 2>/dev/null", timeout_s, qu);
    }
    free(qu);
    if (n <= 0 || n >= (int)sizeof(cmd)) return NULL;
    return strdup(cmd);
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
    /* 无 libcurl 开发库：回退系统 curl 命令（GET），失败返回空串 */
    if (!url) return vus_string_new("");
    char *cli = vus_curl_cli(vus_string_cstr(url), NULL, NULL, NULL, 30);
    if (!cli) return vus_string_new("");
    VusString *out = vus_plugin_shell_exec(vus_string_new(cli));
    free(cli);
    return out;
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
    /* 无 libcurl 开发库：回退系统 curl 命令（POST），失败返回空串 */
    if (!url) return vus_string_new("");
    char *cli = vus_curl_cli(vus_string_cstr(url),
                             (data && vus_string_len(data) > 0) ? vus_string_cstr(data) : "",
                             NULL, NULL, 30);
    if (!cli) return vus_string_new("");
    VusString *out = vus_plugin_shell_exec(vus_string_new(cli));
    free(cli);
    return out;
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
    /* 无 libcurl 开发库：回退系统 curl 命令（-o 写文件），成功返回 "0"，失败 "-1" */
    if (!url || !filepath) return vus_string_new("-1");
    char *cli = vus_curl_cli(vus_string_cstr(url), NULL, vus_string_cstr(filepath), NULL, 60);
    if (!cli) return vus_string_new("-1");
    VusString *out = vus_plugin_shell_exec(vus_string_new(cli));
    free(cli);
    vus_unref(out);
    /* 以目标文件是否生成判定成败 */
    if (access(vus_string_cstr(filepath), F_OK) != 0) return vus_string_new("-1");
    return vus_string_new("0");
#endif
}

/* ---- JSON 字符串转义（"" 与 \ 前加 \）返回 malloc（调用方 free）---- */
static char *vus_json_escape(const char *s) {
    if (!s) return NULL;
    size_t n = 0;
    for (const char *q = s; *q; q++) if (*q == '"' || *q == '\\') n++;
    char *out = (char *)malloc(strlen(s) + n + 1);
    if (!out) return NULL;
    char *p = out;
    for (const char *q = s; *q; q++) { if (*q == '"' || *q == '\\') *p++ = '\\'; *p++ = *q; }
    *p = '\0';
    return out;
}

/* ---- 通用网络请求（覆盖反馈「认证/超时/重试」：自定义请求头如 token、超时秒、重试次数）----
 * 网络_请求(方式, 地址, 头JSON, 数据, 超时秒, 重试次数)
 * APK：走 Java 平台桥 http.request（headers/timeout/retry 全支持）；
 * 桌面：回退 curl（GET/POST，headers 仅 APK 生效）。 */
VusString* vus_plugin_http_request(VusString* method, VusString* url,
                                   VusString* headers_json, VusString* body,
                                   VusString* timeout_s, VusString* retry_s) {
    const char *m = (method && vus_string_len(method) > 0) ? vus_string_cstr(method) : "GET";
    const char *u = url ? vus_string_cstr(url) : "";
    if (!u[0]) return vus_string_new("");
    const char *h = (headers_json && vus_string_len(headers_json) > 0) ? vus_string_cstr(headers_json) : NULL;
    const char *d = (body && vus_string_len(body) > 0) ? vus_string_cstr(body) : NULL;
    int to = timeout_s ? atoi(vus_string_cstr(timeout_s)) : 30;
    if (to <= 0) to = 30;
    int rt = retry_s ? atoi(vus_string_cstr(retry_s)) : 0;
    if (rt < 0) rt = 0;
    if (rt > 10) rt = 10;

    if (g_java_cb) {
        char *eu = vus_json_escape(u);
        char *ed = vus_json_escape(d ? d : "");
        char *aj = NULL;
        if (eu && ed) {
            int n = asprintf(&aj,
                "{\"method\":\"%s\",\"url\":\"%s\",\"headers\":%s,\"data\":\"%s\",\"timeout\":%d,\"retry\":%d}",
                m, eu, h ? h : "{}", ed, to, rt);
            if (n < 0) { free(aj); aj = NULL; }
        }
        free(eu); free(ed);
        if (aj) {
            VusString *jr = vus_java_rpc("http.request", aj);
            free(aj);
            if (jr) return jr;
        }
        return vus_string_new("");
    }
    /* 桌面回退：系统 curl 命令（headers 忽略），POST 用 -d，否则 GET；应用超时秒 */
    {
        if (!u[0]) return vus_string_new("");
        const char *data = (strcmp(m, "POST") == 0 || strcmp(m, "post") == 0) ? d : NULL;
        char *cli = vus_curl_cli(u, data, NULL, NULL, to);
        if (!cli) return vus_string_new("");
        VusString *out = vus_plugin_shell_exec(vus_string_new(cli));
        free(cli);
        return out;
    }
}

/* ---- 文件上传（multipart/form-data，反馈「文件上传」）----
 * 文件_上传(地址, 本地文件, 字段JSON, 头JSON)
 * APK：走 Java 平台桥 http.upload（multipart 手写，支持附加字段+自定义头）；
 * 桌面：回退 curl -F file=@path（仅文件，无附加字段），返回服务器响应文本。 */
VusString* vus_plugin_http_upload(VusString* url, VusString* path,
                                  VusString* fields_json, VusString* headers_json) {
    (void)headers_json;   /* 桌面回退 curl 不支持自定义头；APK 由 Java 桥 http.upload 处理 */
    if (!url || !path) return vus_string_new("0");
    const char *u = vus_string_cstr(url);
    const char *p = vus_string_cstr(path);
    const char *f = (fields_json && vus_string_len(fields_json) > 0) ? vus_string_cstr(fields_json) : NULL;

    if (g_java_cb) {
        char *eu = vus_json_escape(u);
        char *ep = vus_json_escape(p);
        if (!eu || !ep) { free(eu); free(ep); return vus_string_new("0"); }
        size_t cap = strlen(eu) + strlen(ep) + (f ? strlen(f) : 0) + 160;
        char *aj = (char *)malloc(cap);
        if (!aj) { free(eu); free(ep); return vus_string_new("0"); }
        snprintf(aj, cap, "{\"url\":\"%s\",\"path\":\"%s\",\"fields\":%s,\"headers\":%s}",
                 eu, ep, f ? f : "{}", "{}");
        free(eu); free(ep);
        /* Java 侧返回 data="1" 或 "0" */
        VusString *jr = vus_java_rpc("http.upload", aj);
        free(aj);
        if (jr) return jr;
        return vus_string_new("0");
    }
    /* 桌面回退: curl -F file=@本地路径（单引号转义防注入；返回服务器响应文本，失败空串） */
    {
        char *cli = vus_curl_cli(u, NULL, NULL, p, 60);
        if (!cli) return vus_string_new("0");
        VusString *out = vus_plugin_shell_exec(vus_string_new(cli));
        free(cli);
        return out;
    }
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

/* 热更_应用(更新清单URL)（仅 APK）：转 Java 平台桥 hotupdate.apply，
 * 内部走 UpdateManager.applyUpdate（拉清单→校验→下载→原子提交→回滚防护）。
 * 返回 data：0=已应用(实时层生效, .so 重启生效) 1=无更新 -1=宿主过低 -2=失败。 */
VusString* vus_plugin_hotupdate_apply(VusString *url) {
    if (!url || !g_java_cb) return vus_string_new("-2");
    const char *u = vus_string_cstr(url);
    if (!u || !u[0]) return vus_string_new("-2");
    char *aj = vus_java_json_2("url", u, "path", "");
    if (!aj) return vus_string_new("-2");
    VusString *jr = vus_java_rpc("hotupdate.apply", aj);
    free(aj);
    if (jr) return jr;
    return vus_string_new("-2");
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

/* 函数一等公民：把裸函数指针装箱为 VusObject(TYPE_FUNC)。
 * ref 初值 0，与 vus_object_list/dict 约定一致（首次 vus_ref 后为 1）。 */
VusObject* vus_object_func(void (*fn)(void*)) {
    VusObject* o = (VusObject*)calloc(1, sizeof(VusObject));
    if (!o) return NULL;
    o->magic = VUS_OBJECT_MAGIC;
    o->type = TYPE_FUNC;
    o->u.fn = fn;
    return o;
}

/* 调用已装箱的函数值：o 为 TYPE_FUNC 的 VusObject，args 为 VusString* 数组
 * （槽0=返回值，槽1..N=参数），与用户函数 _args 约定一致。返回 args[0]。 */
VusString* vus_object_func_call(VusObject* o, VusString** args) {
    if (!o || o->type != TYPE_FUNC || !o->u.fn) return NULL;
    o->u.fn(args);
    return args[0];
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

/* JSON_解析：解析 JSON 字符串为结构化对象。
 * 幂等（P7）：输入已是列表/字典（VusObject* TYPE_LIST/TYPE_DICT）时 ref+1 原样返回，
 * 使旧写法 JSON_解析(文本_分割(...)) 在文本_分割 直接返回列表后依旧可用。 */
void* vus_json_parse(void* s) {
    if (!s) return NULL;
    if (vus_is_object(s)) {
        VusObject* o = (VusObject*)s;
        if (o->type == TYPE_LIST || o->type == TYPE_DICT) { vus_ref(o); return o; }
        return NULL;
    }
    /* 非容器：按 VusString* 的 JSON 文本解析 */
    yyjson_doc* doc = yyjson_read(vus_string_cstr((VusString*)s), (size_t)vus_string_len((VusString*)s), 0);
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
    /* 兼容容器输入（JSON_解析 的结果）：先序列化为 JSON 文本再查询 */
    VusString* owned = NULL;
    if (vus_is_object(json)) {
        VusObject* jo = (VusObject*)json;
        if (jo->type == TYPE_LIST || jo->type == TYPE_DICT) {
            owned = vus_json_generate(json);
            json = owned;
        }
    }
    if (!json) return NULL;
    yyjson_doc* doc = yyjson_read(vus_string_cstr(json), (size_t)vus_string_len(json), 0);
    if (!doc) { if (owned) vus_unref(owned); return NULL; }
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
    if (owned) vus_unref(owned);
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

/* 插件 JSON 子进程回退（无进程内 python 或进程内失败时共用）：
 * 调用 vux_plugin_manager 取插件原始输出，按 JSON 解析为结构化对象；
 * 输出为空或非 JSON → NULL。 */
static void* vus_plugin_run_vux_json_fallback(VusString* plugin, VusString* cmd) {
    VusString *raw = vus_plugin_run_vux(plugin, cmd);
    if (!raw || vus_string_len(raw) == 0) {
        if (raw) vus_unref(raw);
        return NULL;
    }
    void *obj = vus_json_parse(raw);
    vus_unref(raw);
    return obj;
}

/* ---- 进程内嵌入 Python ---- */
#ifdef VUS_USE_PY

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
static void  (*vus_py_PyErr_FetchFn)(void**, void**, void**) = NULL;
/* Py*_Check 宏在 CPython 中多为宏/内联，不导出符号；改为「对象类型指针相等」判定，
 * 需 dlsym 类型全局变量：PyBool_Type/PyLong_Type/PyFloat_Type/PyList_Type/
 * PyTuple_Type/PyDict_Type（PyUnicode_Type 既有）。bool 是独立类型，type 相等
 * 判定天然区分 bool 与 int。 */
static void* (*vus_py_PyLong_FromLongLongFn)(long long) = NULL;
static void* (*vus_py_PyFloat_FromDoubleFn)(double) = NULL;
static void* (*vus_py_PyBool_FromLongFn)(long) = NULL;
static void* (*vus_py_PyObject_GetAttrStringFn)(void*, const char*) = NULL;
static int   (*vus_py_PyObject_SetAttrStringFn)(void*, const char*, void*) = NULL;
static void* (*vus_py_PyTuple_NewFn)(long) = NULL;
static int   (*vus_py_PyTuple_SetItemFn)(void*, long, void*) = NULL;
static int   (*vus_py_PyErr_OccurredFn)(void) = NULL;
static int   (*vus_py_PyCallable_CheckFn)(void*) = NULL;
static long  (*vus_py_PyTuple_SizeFn)(void*) = NULL;
static void* (*vus_py_PyTuple_GetItemFn)(void*, long) = NULL;
static void* vus_py_PyBool_Type  = NULL;   /* &PyBool_Type（dlsym 类型全局变量） */
static void* vus_py_PyLong_Type  = NULL;
static void* vus_py_PyFloat_Type = NULL;
static void* vus_py_PyList_Type  = NULL;
static void* vus_py_PyTuple_Type = NULL;
static void* vus_py_PyDict_Type  = NULL;
static void* vus_py_Py_None = NULL;        /* 宏 Py_None 指向 _Py_None 全局变量（dlsym 取址后解引用） */

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
    VSYM("PyLong_FromLongLong",     vus_py_PyLong_FromLongLongFn);
    VSYM("PyFloat_FromDouble",      vus_py_PyFloat_FromDoubleFn);
    VSYM("PyBool_FromLong",         vus_py_PyBool_FromLongFn);
    VSYM("PyObject_GetAttrString",  vus_py_PyObject_GetAttrStringFn);
    VSYM("PyObject_SetAttrString",  vus_py_PyObject_SetAttrStringFn);
    VSYM("PyErr_Fetch",             vus_py_PyErr_FetchFn);
    VSYM("PyTuple_New",             vus_py_PyTuple_NewFn);
    VSYM("PyTuple_SetItem",         vus_py_PyTuple_SetItemFn);
    VSYM("PyErr_Occurred",          vus_py_PyErr_OccurredFn);
    VSYM("PyCallable_Check",        vus_py_PyCallable_CheckFn);
    VSYM("PyTuple_Size",            vus_py_PyTuple_SizeFn);
    VSYM("PyTuple_GetItem",         vus_py_PyTuple_GetItemFn);
#undef VSYM

    /* 类型全局变量（Py*_Check 宏不导出符号，用对象类型指针相等判定） */
#define VTSYM(_n, _slot) do { _slot = dlsym(vus_py_handle, _n); if (!_slot) { dlclose(vus_py_handle); vus_py_handle = NULL; return -1; } } while (0)
    VTSYM("PyBool_Type",  vus_py_PyBool_Type);
    VTSYM("PyLong_Type",  vus_py_PyLong_Type);
    VTSYM("PyFloat_Type", vus_py_PyFloat_Type);
    VTSYM("PyList_Type",  vus_py_PyList_Type);
    VTSYM("PyTuple_Type", vus_py_PyTuple_Type);
    VTSYM("PyDict_Type",  vus_py_PyDict_Type);
#undef VTSYM

    /* PyUnicode_Type 是全局变量（非函数），用 dlsym 取地址 */
    vus_py_PyUnicode_Type = dlsym(vus_py_handle, "PyUnicode_Type");
    if (!vus_py_PyUnicode_Type) { dlclose(vus_py_handle); vus_py_handle = NULL; return -1; }
    /* Py_None：版本差异兼容。
     * - 3.12+（含 3.14）：导出 _Py_NoneStruct（对象实体），宏 Py_None = &_Py_NoneStruct；
     * - 更老版本：导出 _Py_None（PyObject* 指针变量），需解引用。
     * 均借用引用，不释放。 */
    {
        void *pn = dlsym(vus_py_handle, "_Py_NoneStruct");
        if (!pn) {
            void **pOld = (void **)dlsym(vus_py_handle, "_Py_None");
            if (!pOld) { dlclose(vus_py_handle); vus_py_handle = NULL; return -1; }
            pn = *pOld;
        }
        vus_py_Py_None = pn;
    }

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
    /* 进程内嵌入 python 可用且返回非空 → 用进程内结果；
     * dlopen 失败 / 返回空 → 回退子进程 vux_plugin_manager，能力不降级 */
    if (plugin && cmd && vus_py_init() == 0) {
        VusString *r = (VusString*)vus_py_plugin_run_obj(plugin, cmd, 0);
        if (r && vus_string_len(r) > 0) return r;
        if (r) vus_unref(r);
    }
    return vus_plugin_run_vux(plugin, cmd);
#else
    return vus_plugin_run_vux(plugin, cmd);
#endif
}

void* vus_plugin_run_vux_json(VusString* plugin, VusString* cmd) {
#ifdef VUS_USE_PY
    /* 进程内嵌入可用且成功解析出结构化结果 → 直接返回；
     * 失败（dlopen 失败 / 插件输出非 JSON）→ 回退子进程再试 */
    if (plugin && cmd && vus_py_init() == 0) {
        void *obj = vus_py_plugin_run_obj(plugin, cmd, 1);
        if (obj) return obj;
    }
#endif
    /* 无进程内 python（或失败）：回退子进程取原始输出，按 JSON 解析 */
    return vus_plugin_run_vux_json_fallback(plugin, cmd);
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
    /* 无进程内 python：回退子进程并解析 JSON（不再返回 NULL） */
    return vus_plugin_run_vux_json_fallback(plugin, cmd);
}

VusString* vus_typeof(void* obj) { (void)obj; return vus_string_new("空"); }

#endif /* VUS_USE_PY */

/* =====================================================================
 * 运行期外部桥（Runtime FFI Bridge）
 * ----------------------------------------------------------------------
 * VUS ↔ Python 模块 / C 运行时插件：函数互调 + 双向变量读写。
 * 设计：docs/designs/2026-09-06-runtime-ffi-abi-design.md（总纲）
 *       docs/designs/2026-09-06-runtime-ffi-py-impl.md / -c-impl.md
 * 协作边界（总纲 §10）：公共件（本块 + bridge.h）由 Python 侧实现维护。
 * ===================================================================== */

#define VUS_EXT_MAX_NS       32   /* 外部别名上限 */
#define VUS_EXT_MAX_EXPORTS  64   /* 全局导出变量上限 */
#define VUS_EXT_DEPTH_LIMIT  64   /* 容器嵌套深度上限 */

typedef enum { VUS_EXT_DOMAIN_NONE = 0, VUS_EXT_DOMAIN_PY, VUS_EXT_DOMAIN_C } VusExtDomainType;

/* 域描述符（桥注册表项） */
typedef struct {
    char             ns[128];        /* 别名 */
    VusExtDomainType type;
    int              loaded;
    char            *src;            /* strdup 持有 */
    /* Python 域（VUS_USE_PY） */
    void            *py_mod;         /* PyObject* 模块（new-ref） */
    char           **py_funcs;       /* 白名单；NULL 结尾 */
    char           **py_vars;
    char           **py_ros;
    /* C 域（c-impl 接入后填充） */
    VusRTModule     *c_mod;
    void            *c_handle;
    /* 惰性声明参数（导入外部 声明时深拷贝保有，首次使用时传递） */
    VusRTValue       params;
    int              has_params;
} VusExtDomain;

static VusExtDomain s_ext_ns[VUS_EXT_MAX_NS];
static int          s_ext_n_ns = 0;

/* 全局导出槽表：导出变量 (["x","y"]) → &vus_x */
typedef struct {
    char        *name;
    VusString  **ptr;
} VusExtExport;
static VusExtExport s_ext_exports[VUS_EXT_MAX_EXPORTS];
static int          s_ext_n_exports = 0;

/* 转换深度计数（单线程协作式运行时，无并发） */
static int s_ext_conv_depth = 0;

/* 最近一次桥错误消息（生成代码经 vus_ext_last_error() 取用） */
static char s_ext_err[256];

const char *vus_ext_last_error(void) { return s_ext_err; }

static void ext_set_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_ext_err, sizeof(s_ext_err), fmt, ap);
    va_end(ap);
}

static int ext_errv(VusRTEnv *env, const char *fmt, ...) {
    if (env) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(env->errs, sizeof(env->errs), fmt, ap);
        va_end(ap);
    } else {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(s_ext_err, sizeof(s_ext_err), fmt, ap);
        va_end(ap);
    }
    return -1;
}

static VusExtDomain *ext_find_ns(const char *ns) {
    if (!ns) return NULL;
    for (int i = 0; i < s_ext_n_ns; i++)
        if (strcmp(s_ext_ns[i].ns, ns) == 0) return &s_ext_ns[i];
    return NULL;
}

static VusExtDomain *ext_create_ns(const char *ns) {
    if (s_ext_n_ns >= VUS_EXT_MAX_NS) return NULL;
    VusExtDomain *d = &s_ext_ns[s_ext_n_ns++];
    memset(d, 0, sizeof(*d));
    snprintf(d->ns, sizeof(d->ns), "%s", ns);
    return d;
}

/* ---- 全局导出槽 ---- */

int vus_ext_export(const char *name, VusString **ptr) {
    if (!name || !ptr) { ext_set_err("导出变量参数无效"); return -1; }
    for (int i = 0; i < s_ext_n_exports; i++) {
        if (strcmp(s_ext_exports[i].name, name) == 0) {
            s_ext_exports[i].ptr = ptr;   /* 幂等：更新指针 */
            return 0;
        }
    }
    if (s_ext_n_exports >= VUS_EXT_MAX_EXPORTS) {
        ext_set_err("全局导出变量超过上限 %d", VUS_EXT_MAX_EXPORTS);
        return -1;
    }
    s_ext_exports[s_ext_n_exports].name = strdup(name);
    s_ext_exports[s_ext_n_exports].ptr  = ptr;
    s_ext_n_exports++;
    return 0;
}

static VusString **ext_find_export(const char *name) {
    for (int i = 0; i < s_ext_n_exports; i++)
        if (strcmp(s_ext_exports[i].name, name) == 0) return s_ext_exports[i].ptr;
    return NULL;
}

/* ---- 类型转换：VUS 值 ↔ VusRTValue（无条件编译） ---- */

void vus_rtval_free(VusRTValue *v);   /* 前向声明（递归释放 VusRTValue 堆内存） */

/* 文本数值判定：十进制整数 / 浮点 / 纯文本。
 * 前导零（如 "007"）按纯文本保留，避免数值精度歧义。 */
static int ext_text_kind(const char *s, long long *pi, double *pf) {
    if (!s || !*s) return VUS_RT_STR;
    const char *p = s;
    if (*p == '-' || *p == '+') p++;
    if (!*p) return VUS_RT_STR;
    if (*p == '0' && p[1] && p[1] >= '0' && p[1] <= '9') return VUS_RT_STR;
    int digits = 1;
    for (const char *q = p; *q; q++) {
        if (*q < '0' || *q > '9') { digits = 0; break; }
    }
    if (digits) {
        if (pi) *pi = strtoll(s, NULL, 10);
        return VUS_RT_INT;
    }
    errno = 0;
    char *end = NULL;
    double d = strtod(s, &end);
    if (errno == 0 && end && *end == '\0' && end != s) {
        if (pf) *pf = d;
        return VUS_RT_FLOAT;
    }
    return VUS_RT_STR;
}

/* VUS 值（VusString* / VusObject*，NULL=空）→ VusRTValue（深拷贝） */
int vus_val_to_rt(void *vus_val, VusRTValue *out, VusRTEnv *env) {
    memset(out, 0, sizeof(*out));
    if (s_ext_conv_depth >= VUS_EXT_DEPTH_LIMIT)
        return ext_errv(env, "容器嵌套过深（>%d）", VUS_EXT_DEPTH_LIMIT);
    if (!vus_val) { out->t = VUS_RT_NIL; return 0; }

    s_ext_conv_depth++;
    int rc = 0;
    do {
        if (!vus_is_object(vus_val)) {
            VusString *sv = (VusString *)vus_val;
            const char *cs = vus_string_cstr(sv);
            if (strcmp(cs, "true") == 0)  { out->t = VUS_RT_BOOL; out->v.b = 1; break; }
            if (strcmp(cs, "false") == 0) { out->t = VUS_RT_BOOL; out->v.b = 0; break; }
            long long i64 = 0; double f64 = 0;
            VusRTType k = ext_text_kind(cs, &i64, &f64);
            if (k == VUS_RT_INT)    { out->t = VUS_RT_INT; out->v.i64 = i64; break; }
            if (k == VUS_RT_FLOAT)  { out->t = VUS_RT_FLOAT; out->v.f64 = f64; break; }
            out->t = VUS_RT_STR;
            out->v.s = strdup(cs);
            break;
        }
        VusObject *o = (VusObject *)vus_val;
        switch (o->type) {
            case TYPE_STR:
                out->t = VUS_RT_STR;
                out->v.s = strdup(vus_string_cstr(o->u.str));
                break;
            case TYPE_LIST: {
                VusList *l = vus_list_unwrap(o);
                int n = vus_list_len(l);
                VusRTValue *items = (VusRTValue *)calloc((size_t)n, sizeof(VusRTValue));
                if (!items) { rc = ext_errv(env, "内存不足"); break; }
                for (int i = 0; i < n; i++) {
                    if (vus_val_to_rt(vus_list_get(l, i), &items[i], env) != 0) {
                        for (int j = 0; j < i; j++) {
                            if (items[j].t == VUS_RT_STR) free((void *)items[j].v.s);
                            if (items[j].t == VUS_RT_LIST) { for (int k = 0; k < items[j].v.arr.n; k++) vus_rtval_free(&items[j].v.arr.items[k]); free(items[j].v.arr.items); }
                            if (items[j].t == VUS_RT_DICT) { for (int k = 0; k < items[j].v.map.n; k++) { free((void *)items[j].v.map.pairs[k].k); vus_rtval_free(items[j].v.map.pairs[k].v); free(items[j].v.map.pairs[k].v); } free(items[j].v.map.pairs); }
                        }
                        free(items);
                        rc = -1;
                        break;
                    }
                }
                if (rc == 0) { out->t = VUS_RT_LIST; out->v.arr.items = items; out->v.arr.n = n; }
                break;
            }
            case TYPE_DICT: {
                VusList *keys = vus_dict_keys_of(o);
                int n = vus_list_len(keys);
                VusRTKv *pairs = (VusRTKv *)calloc((size_t)n, sizeof(VusRTKv));
                if (!pairs) { rc = ext_errv(env, "内存不足"); vus_unref(keys); break; }
                int nn = 0;
                for (int i = 0; i < n; i++) {
                    VusString *k = (VusString *)vus_list_get(keys, i);
                    pairs[nn].k = strdup(vus_string_cstr(k));
                    pairs[nn].v = (VusRTValue *)calloc(1, sizeof(VusRTValue));
                    void *vv = vus_dict_get(vus_dict_unwrap(o), k);
                    if (vus_val_to_rt(vv, pairs[nn].v, env) != 0) {
                        free((void *)pairs[nn].k);
                        free(pairs[nn].v);
                        for (int j = 0; j < nn; j++) {
                            free((void *)pairs[j].k);
                            vus_rtval_free(pairs[j].v);
                            free(pairs[j].v);
                        }
                        free(pairs);
                        vus_unref(keys);
                        rc = -1;
                        break;
                    }
                    nn++;
                }
                if (rc == 0) { out->t = VUS_RT_DICT; out->v.map.pairs = pairs; out->v.map.n = nn; }
                vus_unref(keys);
                break;
            }
            default:
                rc = ext_errv(env, "无法映射类型（VUS 对象 type=%d）", o->type);
                break;
        }
    } while (0);
    s_ext_conv_depth--;
    if (rc != 0 && env) strncpy(s_ext_err, env->errs, sizeof(s_ext_err) - 1);
    return rc;
}

/* 递归释放一个 VusRTValue 的堆内存（s/arr/map） */
void vus_rtval_free(VusRTValue *v) {
    if (!v) return;
    if (v->t == VUS_RT_STR) { free((void *)v->v.s); v->v.s = NULL; }
    else if (v->t == VUS_RT_LIST) {
        for (int i = 0; i < v->v.arr.n; i++) vus_rtval_free(&v->v.arr.items[i]);
        free(v->v.arr.items); v->v.arr.items = NULL;
    } else if (v->t == VUS_RT_DICT) {
        for (int i = 0; i < v->v.map.n; i++) {
            free((void *)v->v.map.pairs[i].k);
            vus_rtval_free(v->v.map.pairs[i].v);
            free(v->v.map.pairs[i].v);
        }
        free(v->v.map.pairs); v->v.map.pairs = NULL;
    }
}

/* 将 VusRTValue 构造为 VUS 值。
 * 返回约定（与生成器 R6 一致）：标量/字符串 vus_string_new 出生 ref=1；
 * 容器返回 ref=0（宿主精确转让，调用点 vus_var_set +1 即唯一持有，不需 unref）；
 * NIL/空 返回 NULL。调用点对标量需 vus_unref 归还出生引用。 */
int vus_rt_to_val(const VusRTValue *in, void **out_vus, VusRTEnv *env) {
    *out_vus = NULL;
    if (!in) return 0;
    if (s_ext_conv_depth >= VUS_EXT_DEPTH_LIMIT)
        return ext_errv(env, "容器嵌套过深（>%d）", VUS_EXT_DEPTH_LIMIT);
    s_ext_conv_depth++;
    int rc = 0;
    do {
        switch (in->t) {
            case VUS_RT_NIL: break;
            case VUS_RT_INT: {
                char nb[32];
                snprintf(nb, sizeof(nb), "%lld", in->v.i64);
                *out_vus = vus_string_new(nb);
                break;
            }
            case VUS_RT_FLOAT: {
                char nb[48];
                snprintf(nb, sizeof(nb), "%.15g", in->v.f64);
                *out_vus = vus_string_new(nb);
                break;
            }
            case VUS_RT_BOOL:
                *out_vus = vus_string_new(in->v.b ? "true" : "false");
                break;
            case VUS_RT_STR:
                *out_vus = in->v.s ? vus_string_new(in->v.s) : NULL;
                break;
            case VUS_RT_LIST: {
                VusObject *o = vus_object_list();
                VusList *l = o->u.list;
                for (int i = 0; i < in->v.arr.n; i++) {
                    void *e = NULL;
                    if (vus_rt_to_val(&in->v.arr.items[i], &e, env) != 0) {
                        vus_unref(o);
                        rc = -1;
                        goto out;
                    }
                    if (e) { vus_list_append(l, e); vus_unref(e); }
                }
                *out_vus = o;
                break;
            }
            case VUS_RT_DICT: {
                VusObject *o = vus_object_dict();
                VusDict *d = o->u.dict;
                for (int i = 0; i < in->v.map.n; i++) {
                    if (!in->v.map.pairs[i].k || !in->v.map.pairs[i].v) continue;
                    void *e = NULL;
                    if (vus_rt_to_val(in->v.map.pairs[i].v, &e, env) != 0) {
                        vus_unref(o);
                        rc = -1;
                        goto out;
                    }
                    if (!e) { rc = ext_errv(env, "字典不能包含空值（%s）", in->v.map.pairs[i].k); vus_unref(o); goto out; }
                    VusString *k = vus_string_new(in->v.map.pairs[i].k);
                    vus_dict_set(d, k, e);
                    vus_unref(k);
                    vus_unref(e);
                }
                *out_vus = o;
                break;
            }
            default:
                rc = ext_errv(env, "无法映射类型（VusRTValue t=%d）", (int)in->t);
                break;
        }
    } while (0);
out:
    s_ext_conv_depth--;
    if (rc != 0 && env) strncpy(s_ext_err, env->errs, sizeof(s_ext_err) - 1);
    return rc;
}

/* 赋值 helper：vus_var_set 然后按出生引用规则归还（容器 ref=0 不归还） */
static void ext_var_assign(VusString **slot, void *v) {
    vus_var_set(slot, v);
    if (v && !vus_is_container(v)) vus_unref(v);
}

/* ---- 域加载/分发（无 USE_PY 时 Python 域报未启用；C 域暂桩） ---- */

#ifdef VUS_USE_PY
int vus_py_ext_load(const char *ns, const char *src, const VusRTValue *params);
int vus_py_ext_call(const char *ns, const char *fname, const VusRTValue *args, int nargs, VusRTValue *out);
int vus_py_ext_get(const char *ns, const char *vname, VusRTValue *out);
int vus_py_ext_set(const char *ns, const char *vname, const VusRTValue *val);
#else
#define vus_py_ext_load NULL
#define vus_py_ext_call NULL
#define vus_py_ext_get  NULL
#define vus_py_ext_set  NULL
#endif

/* C 域（c-impl 接入点；实现在 rt/vus_rt_c_impl.c，此处仅前向声明） */
int vus_c_ext_load(const char *ns, const char *src, const VusRTValue *params);
int vus_c_ext_call(const char *ns, const char *fname, const VusRTValue *args, int nargs, VusRTValue *out);
int vus_c_ext_get(const char *ns, const char *vname, VusRTValue *out);
int vus_c_ext_set(const char *ns, const char *vname, const VusRTValue *val);

/* C 域接入访问器（c-impl 填充/读取域描述符；尾部扩展，不改既有字段布局） */

/* 写入最近桥错误文本（与 vus_ext_last_error() 配对；C 域加载/调用失败时上报） */
void vus_ext_seterr(const char *fmt, ...) {
    if (!fmt) { s_ext_err[0] = '\0'; return; }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_ext_err, sizeof(s_ext_err), fmt, ap);
    va_end(ap);
}

/* 加载成功后登记 C 域模块描述符与 dlopen 句柄（幂等：已登记同模块直接成功） */
int vus_ext_c_register(const char *ns, VusRTModule *mod, void *handle) {
    VusExtDomain *d = ns ? ext_find_ns(ns) : NULL;
    if (!d) { ext_set_err("外部域 %s 未导入", ns ? ns : "(空)"); return -1; }
    d->type = VUS_EXT_DOMAIN_C;
    d->c_mod = mod;
    d->c_handle = handle;
    d->loaded = 1;
    return 0;
}

/* 按别名取 C 域模块描述符（未加载/非 C 域返回 NULL） */
VusRTModule *vus_ext_c_module(const char *ns) {
    VusExtDomain *d = ns ? ext_find_ns(ns) : NULL;
    if (!d || d->type != VUS_EXT_DOMAIN_C) return NULL;
    return d->c_mod;
}

/* 按别名取 C 域 dlopen 句柄（清理用） */
void *vus_ext_c_handle(const char *ns) {
    VusExtDomain *d = ns ? ext_find_ns(ns) : NULL;
    if (!d || d->type != VUS_EXT_DOMAIN_C) return NULL;
    return d->c_handle;
}

/* 域内变量可见性（py 白名单 / c 变量表） */
static int ext_domain_has_var(const VusExtDomain *d, const char *vname) {
    if (d->type == VUS_EXT_DOMAIN_PY) {
        for (int i = 0; d->py_vars && d->py_vars[i]; i++)
            if (strcmp(d->py_vars[i], vname) == 0) return 1;
        for (int i = 0; d->py_ros && d->py_ros[i]; i++)
            if (strcmp(d->py_ros[i], vname) == 0) return 2;   /* 2 = 只读 */
        return 0;
    }
    if (d->type == VUS_EXT_DOMAIN_C && d->c_mod && d->c_mod->vars) {
        for (int i = 0; d->c_mod->vars[i].name[0]; i++)
            if (strcmp(d->c_mod->vars[i].name, vname) == 0)
                return d->c_mod->vars[i].readonly ? 2 : 1;
    }
    return 0;
}

/* ---- 惰性加载支撑：声明式注册 + 首次使用 ensure ----
 * 生成代码入口统一走 vus_ext_*_v（值级、自带错误挂载）；vus_ext_load 保留为
 * 兼容/宿主直调入口（等价：声明 + 立即加载）。 */

static int ext_dom_set_type(VusExtDomain *d, const char *src) {
    size_t sl = strlen(src);
    if ((sl >= 3 && strcmp(src + sl - 3, ".so") == 0) ||
        (sl >= 7 && strcmp(src + sl - 7, ".vulage") == 0)) {
        d->type = VUS_EXT_DOMAIN_C;
        return 0;
    }
    d->type = VUS_EXT_DOMAIN_PY;
    return 0;
}

/* 递归深拷贝 VusRTValue（str/list/dict），配合 vus_rtval_free 释放 */
static void ext_rtval_deepcopy(const VusRTValue *s, VusRTValue *d) {
    memset(d, 0, sizeof(*d));
    if (!s) return;
    d->t = s->t;
    switch (s->t) {
    case VUS_RT_STR:
        d->v.s = s->v.s ? strdup(s->v.s) : NULL;
        break;
    case VUS_RT_LIST: {
        d->v.arr.n = s->v.arr.n;
        d->v.arr.items = s->v.arr.n ? (VusRTValue *)calloc((size_t)s->v.arr.n, sizeof(VusRTValue)) : NULL;
        for (int i = 0; i < s->v.arr.n; i++)
            ext_rtval_deepcopy(&s->v.arr.items[i], &d->v.arr.items[i]);
        break;
    }
    case VUS_RT_DICT: {
        d->v.map.n = s->v.map.n;
        d->v.map.pairs = s->v.map.n ? (VusRTKv *)calloc((size_t)s->v.map.n, sizeof(VusRTKv)) : NULL;
        for (int i = 0; i < s->v.map.n; i++) {
            d->v.map.pairs[i].k = s->v.map.pairs[i].k ? strdup(s->v.map.pairs[i].k) : NULL;
            d->v.map.pairs[i].v = (VusRTValue *)calloc(1, sizeof(VusRTValue));
            if (s->v.map.pairs[i].v)
                ext_rtval_deepcopy(s->v.map.pairs[i].v, d->v.map.pairs[i].v);
        }
        break;
    }
    default:
        break;
    }
}

/* 首次使用触发加载（惰性语义核心）：声明后首次 调用/读写 才真正加载并 init */
static int ext_ensure_loaded(VusExtDomain *d) {
    if (!d) return -1;
    if (d->loaded) return 0;
    const VusRTValue *p = d->has_params ? &d->params : NULL;
    int rc;
    if (d->type == VUS_EXT_DOMAIN_C) {
        rc = vus_c_ext_load(d->ns, d->src, p);
    } else {
#ifdef VUS_USE_PY
        rc = vus_py_ext_load(d->ns, d->src, p);
#else
        ext_set_err("本构建未启用 Python 域（需 VUS_USE_PY + libpython）");
        rc = -1;
#endif
    }
    if (rc == 0) d->loaded = 1;
    return rc;
}

/* 声明式注册（惰性）：别名→源+参数 登记但**不加载**。幂等（同源重复成功）；
 * 异源/别名冲突报错。params_vus 为 VUS 字典值（VusObject*）或 NULL。 */
int vus_ext_declare_v(const char *ns, const char *src, void *params_vus) {
    if (!ns || !ns[0] || !src || !src[0]) { ext_set_err("导入外部 参数无效"); return -1; }
    VusExtDomain *d = ext_find_ns(ns);
    if (d) {
        /* 幂等：同源重复导入成功；异源报错（别名冲突） */
        if (strcmp(d->src, src) == 0) return 0;
        ext_set_err("别名 %s 已绑定到 %s，不能重复导入 %s", ns, d->src, src);
        return -1;
    }
    d = ext_create_ns(ns);
    if (!d) { ext_set_err("外部别名超过上限 %d", VUS_EXT_MAX_NS); return -1; }
    d->src = strdup(src);
    ext_dom_set_type(d, src);
    /* 参数深拷贝保有（供首次加载时传予域 init） */
    if (params_vus) {
        if (vus_val_to_rt(params_vus, &d->params, NULL) != 0) {
            ext_set_err("导入外部 参数无法转换（需为字典）");
            free(d->src);
            d->src = NULL;
            return -1;
        }
        d->has_params = 1;
    }
    return 0;
}

int vus_ext_load(const char *ns, const char *src, const VusRTValue *params) {
    if (!ns || !ns[0] || !src || !src[0]) { ext_set_err("导入外部 参数无效"); return -1; }
    VusExtDomain *d = ext_find_ns(ns);
    if (!d) {
        d = ext_create_ns(ns);
        if (!d) { ext_set_err("外部别名超过上限 %d", VUS_EXT_MAX_NS); return -1; }
        d->src = strdup(src);
        ext_dom_set_type(d, src);
    } else if (strcmp(d->src, src) != 0) {
        ext_set_err("别名 %s 已绑定到 %s，不能重复导入 %s", ns, d->src, src);
        return -1;
    }
    if (params && !d->has_params) {
        ext_rtval_deepcopy(params, &d->params);
        d->has_params = 1;
    }
    return ext_ensure_loaded(d);
}

int vus_ext_call(const char *ns, const char *fname, const VusRTValue *args, int nargs, VusRTValue *out) {
    VusExtDomain *d = ext_find_ns(ns);
    if (!d) { ext_set_err("外部域 %s 未导入", ns ? ns : "(空)"); return -1; }
    if (ext_ensure_loaded(d) != 0) return -1;
    memset(out, 0, sizeof(*out));
    if (d->type == VUS_EXT_DOMAIN_PY) {
#ifdef VUS_USE_PY
        return vus_py_ext_call(ns, fname, args, nargs, out);
#else
        ext_set_err("本构建未启用 Python 域"); return -1;
#endif
    }
    return vus_c_ext_call(ns, fname, args, nargs, out);
}

int vus_ext_get(const char *ns, const char *vname, VusRTValue *out) {
    VusExtDomain *d = ext_find_ns(ns);
    if (!d) { ext_set_err("外部域 %s 未导入", ns ? ns : "(空)"); return -1; }
    if (ext_ensure_loaded(d) != 0) return -1;
    memset(out, 0, sizeof(*out));
    int rw = ext_domain_has_var(d, vname);
    if (rw) {
        if (d->type == VUS_EXT_DOMAIN_PY) {
#ifdef VUS_USE_PY
            return vus_py_ext_get(ns, vname, out);
#else
            ext_set_err("本构建未启用 Python 域"); return -1;
#endif
        }
        return vus_c_ext_get(ns, vname, out);
    }
    /* 域内无 → 全局导出槽 */
    VusString **slot = ext_find_export(vname);
    if (!slot) { ext_set_err("<%s>.%s 不存在", ns, vname); return -1; }
    return vus_val_to_rt(*slot, out, NULL);
}

int vus_ext_set(const char *ns, const char *vname, const VusRTValue *val) {
    VusExtDomain *d = ext_find_ns(ns);
    if (!d) { ext_set_err("外部域 %s 未导入", ns ? ns : "(空)"); return -1; }
    if (ext_ensure_loaded(d) != 0) return -1;
    int rw = ext_domain_has_var(d, vname);
    if (rw == 2) { ext_set_err("%s.%s 为只读", ns, vname); return -1; }
    if (rw == 1) {
        if (d->type == VUS_EXT_DOMAIN_PY) {
#ifdef VUS_USE_PY
            return vus_py_ext_set(ns, vname, val);
#else
            ext_set_err("本构建未启用 Python 域"); return -1;
#endif
        }
        return vus_c_ext_set(ns, vname, val);
    }
    /* 域内无 → 全局导出槽（只读性与类型在 VUS 侧无声明约束，直读直写） */
    VusString **slot = ext_find_export(vname);
    if (!slot) { ext_set_err("<%s>.%s 不存在", ns, vname); return -1; }
    void *v = NULL;
    if (vus_rt_to_val(val, &v, NULL) != 0) return -1;
    ext_var_assign(slot, v);
    return 0;
}

/* ---- 生成代码入口（vus_ext_*_v）：值级互操作，失败挂载 VusError（类型"外部错误"） ---- */

static int ext_fail_v(VusError **err_out) {
    if (err_out) {
        *err_out = vus_error_new_typed(1, "外部错误",
                                       vus_ext_last_error()[0] ? vus_ext_last_error() : "外部域调用失败",
                                       0, 0);
    }
    return -1;
}

int vus_ext_call_v(const char *ns, const char *fname, void *const *args, int nargs,
                   void **out_vus, VusError **err_out) {
    if (out_vus) *out_vus = NULL;
    if (err_out) *err_out = NULL;
    VusExtDomain *d = ext_find_ns(ns);
    if (!d) { ext_set_err("外部域 %s 未导入", ns ? ns : "(空)"); return ext_fail_v(err_out); }
    if (ext_ensure_loaded(d) != 0) return ext_fail_v(err_out);

    VusRTValue *rtargs = NULL;
    if (nargs > 0) {
        rtargs = (VusRTValue *)calloc((size_t)nargs, sizeof(VusRTValue));
        if (!rtargs) { ext_set_err("内存不足"); return ext_fail_v(err_out); }
        for (int i = 0; i < nargs; i++) {
            if (vus_val_to_rt(args[i], &rtargs[i], NULL) != 0) {
                for (int j = 0; j < i; j++) vus_rtval_free(&rtargs[j]);
                free(rtargs);
                ext_set_err("实参 #%d 无法转换", i + 1);
                return ext_fail_v(err_out);
            }
        }
    }
    VusRTValue out = {0};
    int rc = vus_ext_call(ns, fname, rtargs, nargs, &out);
    for (int i = 0; i < nargs; i++) vus_rtval_free(&rtargs[i]);
    free(rtargs);
    if (rc != 0) return ext_fail_v(err_out);
    void *v = NULL;
    if (vus_rt_to_val(&out, &v, NULL) != 0) {
        vus_rtval_free(&out);
        if (!vus_ext_last_error()[0]) ext_set_err("返回值无法转换");
        return ext_fail_v(err_out);
    }
    vus_rtval_free(&out);
    if (out_vus) *out_vus = v;
    return 0;
}

int vus_ext_get_v(const char *ns, const char *vname, void **out_vus, VusError **err_out) {
    if (out_vus) *out_vus = NULL;
    if (err_out) *err_out = NULL;
    VusExtDomain *d = ext_find_ns(ns);
    if (!d) { ext_set_err("外部域 %s 未导入", ns ? ns : "(空)"); return ext_fail_v(err_out); }
    if (ext_ensure_loaded(d) != 0) return ext_fail_v(err_out);
    VusRTValue out = {0};
    if (vus_ext_get(ns, vname, &out) != 0) return ext_fail_v(err_out);
    void *v = NULL;
    if (vus_rt_to_val(&out, &v, NULL) != 0) {
        vus_rtval_free(&out);
        if (!vus_ext_last_error()[0]) ext_set_err("变量值无法转换");
        return ext_fail_v(err_out);
    }
    vus_rtval_free(&out);
    if (out_vus) *out_vus = v;
    return 0;
}

int vus_ext_set_v(const char *ns, const char *vname, void *val_vus, VusError **err_out) {
    if (err_out) *err_out = NULL;
    VusExtDomain *d = ext_find_ns(ns);
    if (!d) { ext_set_err("外部域 %s 未导入", ns ? ns : "(空)"); return ext_fail_v(err_out); }
    if (ext_ensure_loaded(d) != 0) return ext_fail_v(err_out);
    VusRTValue rv;
    if (vus_val_to_rt(val_vus, &rv, NULL) != 0) return ext_fail_v(err_out);
    int rc = vus_ext_set(ns, vname, &rv);
    vus_rtval_free(&rv);
    if (rc != 0) return ext_fail_v(err_out);
    return 0;
}

int vus_ext_export_v(const char *name, void **ptr) {
    return vus_ext_export(name, (VusString **)ptr);
}

void vus_ext_shutdown_all(void) {
#ifdef VUS_USE_PY
    for (int i = 0; i < s_ext_n_ns; i++) {
        VusExtDomain *d = &s_ext_ns[i];
        if (d->type == VUS_EXT_DOMAIN_PY && d->py_mod) {
            /* __vus_cleanup__（若模块定义） */
            void *cf = vus_py_PyObject_GetAttrStringFn(d->py_mod, "__vus_cleanup__");
            if (cf && !vus_py_PyErr_OccurredFn()) {
                (void)vus_py_PyObject_CallObjectFn(cf, NULL);
                if (vus_py_PyErr_OccurredFn()) { vus_py_PyErr_PrintFn(); vus_py_PyErr_ClearFn(); }
            }
            if (cf) vus_py_Py_XDECREF_Fn(cf);
            vus_py_Py_XDECREF_Fn(d->py_mod);
            d->py_mod = NULL;
        }
    }
#endif
    for (int i = 0; i < s_ext_n_ns; i++) {
        VusExtDomain *d = &s_ext_ns[i];
        char **arrs[3] = { d->py_funcs, d->py_vars, d->py_ros };
        for (int a = 0; a < 3; a++)
            for (int j = 0; arrs[a] && arrs[a][j]; j++) free(arrs[a][j]);
        for (int a = 0; a < 3; a++) free(arrs[a]);
        if (d->src) { free(d->src); d->src = NULL; }
        if (d->has_params) { vus_rtval_free(&d->params); d->has_params = 0; }
        /* C 域：cleanup 回调 + dlclose（资源/共存清单 #12） */
        if (d->type == VUS_EXT_DOMAIN_C && d->c_mod) {
            if (d->c_mod->cleanup) d->c_mod->cleanup();
            if (d->c_handle) { dlclose(d->c_handle); d->c_handle = NULL; }
            d->c_mod = NULL;
        }
    }
    s_ext_n_ns = 0;
    for (int i = 0; i < s_ext_n_exports; i++) {
        free(s_ext_exports[i].name);
        s_ext_exports[i].name = NULL;
    }
    s_ext_n_exports = 0;
}

#ifdef VUS_USE_PY
/* =====================================================================
 * Python 域实现（在既有 vus_py_* 嵌入通道之上）
 * ===================================================================== */

/* 对象类型指针相等判定（替代 Py*_Check 宏；朴素对象相等，子类视为自定义类型） */
static int vus_py_is_type(void *o, void *t) {
    return o && t && vus_py_PyObject_TypeFn(o) == t;
}

/* VusRTValue → PyObject*（桥层转换，深拷贝） */
static void *vus_py_rt_to_obj(const VusRTValue *in, VusRTEnv *env) {
    if (s_ext_conv_depth >= VUS_EXT_DEPTH_LIMIT) {
        ext_errv(env, "容器嵌套过深（>%d）", VUS_EXT_DEPTH_LIMIT); return NULL;
    }
    if (!in) return NULL;
    s_ext_conv_depth++;
    void *o = NULL;
    switch (in->t) {
        case VUS_RT_NIL:   o = vus_py_Py_None; break;
        case VUS_RT_INT:   o = vus_py_PyLong_FromLongLongFn(in->v.i64); break;
        case VUS_RT_FLOAT: o = vus_py_PyFloat_FromDoubleFn(in->v.f64); break;
        case VUS_RT_BOOL:  o = vus_py_PyBool_FromLongFn(in->v.b); break;
        case VUS_RT_STR:
            o = in->v.s ? vus_py_PyUnicode_FromStringFn(in->v.s) : vus_py_Py_None;
            break;
        case VUS_RT_LIST: {
            /* PyList_New(n) 在 3.12+ 语义为「创建长度 n 的列表」而非「预分配容量」，
             * 直接 New(n)+Append 会多出 n 个空槽（ob_size 翻倍）。统一 New(0)+Append。 */
            o = vus_py_PyList_NewFn(0);
            if (!o) break;
            for (int i = 0; i < in->v.arr.n; i++) {
                void *e = vus_py_rt_to_obj(&in->v.arr.items[i], env);
                if (!e) { vus_py_Py_XDECREF_Fn(o); o = NULL; break; }
                if (vus_py_PyList_AppendFn(o, e) != 0) { vus_py_Py_XDECREF_Fn(e); vus_py_Py_XDECREF_Fn(o); o = NULL; break; }
                vus_py_Py_XDECREF_Fn(e);
            }
            break;
        }
        case VUS_RT_DICT: {
            o = vus_py_PyDict_NewFn();
            if (!o) break;
            for (int i = 0; i < in->v.map.n; i++) {
                void *v = vus_py_rt_to_obj(in->v.map.pairs[i].v, env);
                if (!v) { vus_py_Py_XDECREF_Fn(o); o = NULL; break; }
                void *k = vus_py_PyUnicode_FromStringFn(in->v.map.pairs[i].k);
                if (vus_py_PyDict_SetItemFn(o, k, v) != 0) { vus_py_Py_XDECREF_Fn(k); vus_py_Py_XDECREF_Fn(v); vus_py_Py_XDECREF_Fn(o); o = NULL; break; }
                vus_py_Py_XDECREF_Fn(k);
                vus_py_Py_XDECREF_Fn(v);
            }
            break;
        }
        default:
            ext_errv(env, "无法映射类型（VusRTValue t=%d）", (int)in->t);
            break;
    }
    s_ext_conv_depth--;
    return o;
}

/* 提取 PyErr 当前异常为 "类型: 消息" 文本 */
static void vus_py_err_text(char *buf, size_t cap) {
    buf[0] = '\0';
    if (!vus_py_PyErr_FetchFn || !vus_py_PyErr_OccurredFn) return;
    void *t = NULL, *v = NULL, *tb = NULL;
    vus_py_PyErr_FetchFn(&t, &v, &tb);
    if (t) {
        void *ts = vus_py_PyObject_StrFn(t);
        const char *tc = ts ? vus_py_PyUnicode_AsUTF8Fn(ts) : NULL;
        snprintf(buf, cap, "%s", tc ? tc : "异常");
        if (ts) vus_py_Py_XDECREF_Fn(ts);
    }
    if (v) {
        void *vs = vus_py_PyObject_StrFn(v);
        const char *vc = vs ? vus_py_PyUnicode_AsUTF8Fn(vs) : NULL;
        if (vc && vc[0]) {
            size_t l = strlen(buf);
            snprintf(buf + l, cap - l, ": %s", vc);
        }
        if (vs) vus_py_Py_XDECREF_Fn(vs);
    }
    if (t) vus_py_Py_XDECREF_Fn(t);
    if (v) vus_py_Py_XDECREF_Fn(v);
    if (tb) vus_py_Py_XDECREF_Fn(tb);
}

/* PyObject* → VusRTValue（深拷贝；None→NIL；list/tuple→LIST；dict 键限 str；
 * 自定义类型走 __vus_tovalue__ 协议，未实现则报错） */
static int vus_py_obj_to_rt(void *obj, VusRTValue *out, VusRTEnv *env) {
    memset(out, 0, sizeof(*out));
    if (s_ext_conv_depth >= VUS_EXT_DEPTH_LIMIT)
        return ext_errv(env, "容器嵌套过深（>%d）", VUS_EXT_DEPTH_LIMIT);
    if (!obj || obj == vus_py_Py_None) { out->t = VUS_RT_NIL; return 0; }
    s_ext_conv_depth++;
    int rc = 0;
    if (vus_py_is_type(obj, vus_py_PyBool_Type)) {
        out->t = VUS_RT_BOOL;
        out->v.b = vus_py_PyObject_IsTrueFn(obj) ? 1 : 0;
    } else if (vus_py_is_type(obj, vus_py_PyLong_Type)) {
        long v = vus_py_PyLong_AsLongFn(obj);
        if (vus_py_PyErr_OccurredFn()) { vus_py_PyErr_ClearFn(); rc = ext_errv(env, "整数超出范围"); }
        else { out->t = VUS_RT_INT; out->v.i64 = (long long)v; }
    } else if (vus_py_is_type(obj, vus_py_PyFloat_Type)) {
        out->t = VUS_RT_FLOAT;
        out->v.f64 = vus_py_PyFloat_AsDoubleFn(obj);
    } else if (vus_py_is_type(obj, vus_py_PyUnicode_Type)) {
        const char *cs = vus_py_PyUnicode_AsUTF8Fn(obj);
        if (!cs) { rc = ext_errv(env, "字符串解码失败"); }
        else { out->t = VUS_RT_STR; out->v.s = strdup(cs); }
    } else if (vus_py_is_type(obj, vus_py_PyList_Type) || vus_py_is_type(obj, vus_py_PyTuple_Type)) {
        int is_list = vus_py_is_type(obj, vus_py_PyList_Type);
        long n = is_list ? vus_py_PyList_SizeFn(obj) : vus_py_PyTuple_SizeFn(obj);
        VusRTValue *items = (VusRTValue *)calloc(n > 0 ? (size_t)n : 1, sizeof(VusRTValue));
        if (!items) { rc = ext_errv(env, "内存不足"); }
        else {
            for (long i = 0; i < n; i++) {
                void *e = is_list ? vus_py_PyList_GetItemFn(obj, i)
                                  : vus_py_PyTuple_GetItemFn(obj, i);
                if (vus_py_obj_to_rt(e, &items[i], env) != 0) {
                    for (long j = 0; j < i; j++) vus_rtval_free(&items[j]);
                    free(items);
                    rc = -1;
                    break;
                }
            }
            if (rc == 0) { out->t = VUS_RT_LIST; out->v.arr.items = items; out->v.arr.n = (int)n; }
        }
    } else if (vus_py_is_type(obj, vus_py_PyDict_Type)) {
        long n = vus_py_PyDict_SizeFn(obj);
        VusRTKv *pairs = (VusRTKv *)calloc(n > 0 ? (size_t)n : 1, sizeof(VusRTKv));
        if (!pairs) { rc = ext_errv(env, "内存不足"); }
        else {
            long pos = 0, nn = 0;
            void *k = NULL, *v = NULL;
            while (vus_py_PyDict_NextFn(obj, &pos, &k, &v)) {
                if (!vus_py_is_type(k, vus_py_PyUnicode_Type)) { rc = ext_errv(env, "字典键必须是字符串"); break; }
                const char *ks = vus_py_PyUnicode_AsUTF8Fn(k);
                pairs[nn].k = strdup(ks);
                pairs[nn].v = (VusRTValue *)calloc(1, sizeof(VusRTValue));
                if (vus_py_obj_to_rt(v, pairs[nn].v, env) != 0) {
                    for (long j = 0; j <= nn; j++) {
                        if (pairs[j].k) free((void *)pairs[j].k);
                        if (pairs[j].v) { vus_rtval_free(pairs[j].v); free(pairs[j].v); }
                    }
                    free(pairs);
                    rc = -1;
                    break;
                }
                nn++;
            }
            if (rc == 0) { out->t = VUS_RT_DICT; out->v.map.pairs = pairs; out->v.map.n = (int)nn; }
        }
    } else {
        /* 自定义类型：__vus_tovalue__ 协议 */
        void *fv = vus_py_PyObject_GetAttrStringFn(obj, "__vus_tovalue__");
        if (fv && vus_py_PyErr_OccurredFn()) vus_py_PyErr_ClearFn();
        if (fv) {
            void *r = vus_py_PyObject_CallObjectFn(fv, NULL);
            if (r) {
                rc = vus_py_obj_to_rt(r, out, env);
                vus_py_Py_XDECREF_Fn(r);
            } else {
                char eb[192]; vus_py_err_text(eb, sizeof(eb));
                vus_py_PyErr_ClearFn();
                rc = ext_errv(env, "__vus_tovalue__ 调用失败: %s", eb);
            }
            vus_py_Py_XDECREF_Fn(fv);
        } else {
            void *ts = vus_py_PyObject_StrFn(vus_py_PyObject_TypeFn(obj));
            const char *tn = ts ? vus_py_PyUnicode_AsUTF8Fn(ts) : "未知";
            if (ts) vus_py_Py_XDECREF_Fn(ts);
            rc = ext_errv(env, "无法映射类型: %s", tn ? tn : "未知");
        }
    }
    s_ext_conv_depth--;
    if (rc != 0 && env) strncpy(s_ext_err, env->errs, sizeof(s_ext_err) - 1);
    return rc;
}

/* sys.path 预置注入（不重复） */
static void vus_py_ext_ensure_path(const char *dir) {
    if (!dir || !dir[0]) return;
    void *path = vus_py_PySys_GetObjectFn("path");
    if (!path || !vus_py_is_type(path, vus_py_PyList_Type)) return;
    long n = vus_py_PyList_SizeFn(path);
    for (long i = 0; i < n; i++) {
        void *it = vus_py_PyList_GetItemFn(path, i);
        if (it && vus_py_is_type(it, vus_py_PyUnicode_Type)) {
            const char *cs = vus_py_PyUnicode_AsUTF8Fn(it);
            if (cs && strcmp(cs, dir) == 0) return;
        }
    }
    void *u = vus_py_PyUnicode_FromStringFn(dir);
    if (u) { vus_py_PyList_AppendFn(path, u); vus_py_Py_XDECREF_Fn(u); }
}

/* Python str 列表（如 __vus_export__ 的 函数/变量 值）→ char** 数组（NULL 结尾）。失败返回 NULL */
static char **vus_py_strlist_from(void *obj) {
    if (!obj || !vus_py_is_type(obj, vus_py_PyList_Type)) return NULL;
    long n = vus_py_PyList_SizeFn(obj);
    char **arr = (char **)calloc((size_t)n + 1, sizeof(char *));
    if (!arr) return NULL;
    long m = 0;
    for (long i = 0; i < n; i++) {
        void *it = vus_py_PyList_GetItemFn(obj, i);
        if (it && vus_py_is_type(it, vus_py_PyUnicode_Type)) {
            const char *cs = vus_py_PyUnicode_AsUTF8Fn(it);
            arr[m++] = cs ? strdup(cs) : NULL;
        } else {
            for (long j = 0; j < m; j++) free(arr[j]);
            free(arr);
            return NULL;
        }
    }
    arr[m] = NULL;
    return arr;
}

/* 缺省回退采集：模块公开命名空间（跳过下划线开头、桥保留名、可调用对象） */
static void vus_py_export_fallback(void *dict, char ***pu_funcs, char ***pu_vars, char ***pu_ros) {
    (void)pu_ros; /* 默认无可写/只读区分，全为可读写变量 */
    long pos = 0;
    void *k = NULL, *v = NULL;
    char **funcs = NULL, **vars = NULL;
    int nf = 0, nv = 0, cf = 0, cv = 0;
    while (vus_py_PyDict_NextFn(dict, &pos, &k, &v)) {
        if (!vus_py_is_type(k, vus_py_PyUnicode_Type)) continue;
        const char *ks = vus_py_PyUnicode_AsUTF8Fn(k);
        if (!ks || !ks[0] || ks[0] == '_') continue;               /* 跳过 dunder/私有 */
        if (strncmp(ks, "__vus_", 6) == 0) continue;               /* 桥保留名 */
        if (vus_py_PyCallable_CheckFn(v)) {                         /* 可调用 → 函数表 */
            funcs = funcs ? funcs : (char **)calloc(64, sizeof(char *));
            if (nf < 63) funcs[nf++] = strdup(ks);
        } else if (vus_py_PySequence_CheckFn(v) || vus_py_PyMapping_CheckFn(v) ||
                   vus_py_is_type(v, vus_py_PyLong_Type) || vus_py_is_type(v, vus_py_PyFloat_Type) ||
                   vus_py_is_type(v, vus_py_PyBool_Type) || vus_py_is_type(v, vus_py_PyUnicode_Type) || v == vus_py_Py_None) {
            vars = vars ? vars : (char **)calloc(64, sizeof(char *));
            if (nv < 63) vars[nv++] = strdup(ks);
        }
    }
    funcs = funcs ? funcs : (char **)calloc(1, sizeof(char *));
    vars  = vars  ? vars  : (char **)calloc(1, sizeof(char *));
    *pu_funcs = funcs; *pu_vars = vars; *pu_ros = NULL;
    (void)cf; (void)cv;
}

int vus_py_ext_load(const char *ns, const char *src, const VusRTValue *params) {
    VusExtDomain *d = ext_find_ns(ns);
    if (!d) { ext_set_err("域 %s 不存在", ns); return -1; }
    if (vus_py_init() != 0) { ext_set_err("Python 域不可用（无 libpython 或未启用 VUS_USE_PY）"); return -1; }

    /* sys.path 注入（插件根/安装目录/脚本目录） */
    const char *pd = getenv("VUS_PLUGIN_DIR");
    if (pd && pd[0]) vus_py_ext_ensure_path(pd);
    const char *home = getenv("VUS_HOME");
    if (home && home[0]) vus_py_ext_ensure_path(home);
    const char *cdir = getenv("PWD");
    if (cdir && cdir[0]) vus_py_ext_ensure_path(cdir);

    void *mod = vus_py_PyImport_ImportModuleFn(src);
    if (!mod) {
        char eb[192]; vus_py_err_text(eb, sizeof(eb));
        vus_py_PyErr_ClearFn();
        ext_set_err("导入外部 失败: %s: %s", src, eb);
        return -1;
    }

    /* __vus_init__(api_dict, 参数) */
    void *initerr = NULL;
    void *initfn = vus_py_PyObject_GetAttrStringFn(mod, "__vus_init__");
    if (initfn && !vus_py_PyErr_OccurredFn()) {
        void *api = vus_py_PyDict_NewFn();
        if (api) {
            void *vk = vus_py_PyUnicode_FromStringFn("version");
            void *vv = vus_py_PyLong_FromLongLongFn(VUS_RT_BRIDGE_ABI);
            vus_py_PyDict_SetItemFn(api, vk, vv);
            vus_py_Py_XDECREF_Fn(vk); vus_py_Py_XDECREF_Fn(vv);
            void *nk = vus_py_PyUnicode_FromStringFn("别名");
            void *nv = vus_py_PyUnicode_FromStringFn(ns);
            vus_py_PyDict_SetItemFn(api, nk, nv);
            vus_py_Py_XDECREF_Fn(nk); vus_py_Py_XDECREF_Fn(nv);
            void *sk = vus_py_PyUnicode_FromStringFn("源");
            void *sv = vus_py_PyUnicode_FromStringFn(src);
            vus_py_PyDict_SetItemFn(api, sk, sv);
            vus_py_Py_XDECREF_Fn(sk); vus_py_Py_XDECREF_Fn(sv);
            VusRTEnv env = { VUS_RT_BRIDGE_ABI, {0} };
            void *pobj = params ? vus_py_rt_to_obj(params, &env) : vus_py_PyDict_NewFn();
            if (!pobj) { ext_set_err("初始化参数无法映射: %s", env.errs); vus_py_Py_XDECREF_Fn(api); vus_py_Py_XDECREF_Fn(mod); return -1; }
            void *r = vus_py_PyObject_CallObjectFn(initfn,
                        vus_py_Py_BuildValueFn("(OO)", api, pobj));
            if (!r) {
                char eb[192]; vus_py_err_text(eb, sizeof(eb));
                vus_py_PyErr_ClearFn();
                initerr = strdup(eb);
            } else {
                long code = vus_py_is_type(r, vus_py_PyLong_Type) ? vus_py_PyLong_AsLongFn(r) : 0;
                if (vus_py_PyErr_OccurredFn()) vus_py_PyErr_ClearFn();
                if (code != 0) {
                    char eb[192]; snprintf(eb, sizeof(eb), "初始化返回码 %ld", code);
                    initerr = strdup(eb);
                }
                vus_py_Py_XDECREF_Fn(r);
            }
            vus_py_Py_XDECREF_Fn(pobj);
            vus_py_Py_XDECREF_Fn(api);
        }
    } else if (initfn && vus_py_PyErr_OccurredFn()) {
        vus_py_PyErr_ClearFn();
    }
    if (initfn) vus_py_Py_XDECREF_Fn(initfn);
    if (initerr) {
        ext_set_err("%s 初始化失败: %s", src, initerr);
        free(initerr);
        vus_py_Py_XDECREF_Fn(mod);
        return -1;
    }

    /* 导出清单采集：__vus_export__（声明式）或公开命名空间（回退） */
    char **funcs = NULL, **vars = NULL, **ros = NULL;
    void *ex = vus_py_PyObject_GetAttrStringFn(mod, "__vus_export__");
    if (ex && !vus_py_PyErr_OccurredFn()) {
        if (!vus_py_is_type(ex, vus_py_PyDict_Type)) {
            ext_set_err("%s: __vus_export__ 必须是字典", src);
            vus_py_Py_XDECREF_Fn(ex); vus_py_Py_XDECREF_Fn(mod);
            return -1;
        }
        funcs = vus_py_strlist_from(vus_py_PyDict_GetItemStringFn(ex, "函数"));
        vars  = vus_py_strlist_from(vus_py_PyDict_GetItemStringFn(ex, "变量"));
        ros   = vus_py_strlist_from(vus_py_PyDict_GetItemStringFn(ex, "只读"));
        /* 校验：白名单里模块属性必须存在；函数必须可调用 */
        char errb[256] = {0};
        for (int i = 0; funcs && funcs[i] && !errb[0]; i++) {
            void *a = vus_py_PyObject_GetAttrStringFn(mod, funcs[i]);
            if (vus_py_PyErr_OccurredFn()) { vus_py_PyErr_ClearFn(); snprintf(errb, sizeof(errb), "函数 %s 不存在", funcs[i]); }
            else if (!vus_py_PyCallable_CheckFn(a)) { snprintf(errb, sizeof(errb), "%s 不可调用", funcs[i]); }
            if (a) vus_py_Py_XDECREF_Fn(a);
        }
        for (int i = 0; i < 2 && vars && vars[i] && !errb[0]; i++) {}
        if (!errb[0]) {
            for (int i = 0; vars && vars[i] && !errb[0]; i++) {
                void *a = vus_py_PyObject_GetAttrStringFn(mod, vars[i]);
                if (vus_py_PyErr_OccurredFn()) { vus_py_PyErr_ClearFn(); snprintf(errb, sizeof(errb), "变量 %s 不存在", vars[i]); }
                if (a) vus_py_Py_XDECREF_Fn(a);
            }
        }
        if (!errb[0]) {
            for (int i = 0; ros && ros[i] && !errb[0]; i++) {
                void *a = vus_py_PyObject_GetAttrStringFn(mod, ros[i]);
                if (vus_py_PyErr_OccurredFn()) { vus_py_PyErr_ClearFn(); snprintf(errb, sizeof(errb), "只读变量 %s 不存在", ros[i]); }
                if (a) vus_py_Py_XDECREF_Fn(a);
            }
        }
        if (errb[0]) {
            ext_set_err("%s: __vus_export__ 校验失败: %s", src, errb);
            vus_py_Py_XDECREF_Fn(ex); vus_py_Py_XDECREF_Fn(mod);
            return -1;
        }
    } else {
        if (ex && vus_py_PyErr_OccurredFn()) vus_py_PyErr_ClearFn();
        void *mdc = vus_py_PyModule_GetDictFn(mod);
        vus_py_export_fallback(mdc, &funcs, &vars, &ros);
    }
    if (ex) vus_py_Py_XDECREF_Fn(ex);

    d->py_mod   = mod;   /* new-ref 持有 */
    d->py_funcs = funcs ? funcs : (char **)calloc(1, sizeof(char *));
    d->py_vars  = vars  ? vars  : (char **)calloc(1, sizeof(char *));
    d->py_ros   = ros   ? ros   : (char **)calloc(1, sizeof(char *));
    return 0;
}

static VusExtDomain *ext_ns_loaded(const char *ns) {
    VusExtDomain *d = ext_find_ns(ns);
    if (!d || !d->loaded || d->type != VUS_EXT_DOMAIN_PY || !d->py_mod) {
        ext_set_err("外部域 %s 未加载", ns ? ns : "(空)");
        return NULL;
    }
    return d;
}

static int ext_py_func_exists(const VusExtDomain *d, const char *fname) {
    for (int i = 0; d->py_funcs && d->py_funcs[i]; i++)
        if (strcmp(d->py_funcs[i], fname) == 0) return 1;
    return 0;
}

int vus_py_ext_call(const char *ns, const char *fname, const VusRTValue *args, int nargs, VusRTValue *out) {
    VusExtDomain *d = ext_ns_loaded(ns);
    if (!d) return -1;
    memset(out, 0, sizeof(*out));
    if (!ext_py_func_exists(d, fname)) {
        ext_set_err("外部函数 %s.%s 不存在（或未导出）", ns, fname);
        return -1;
    }
    void *fn = vus_py_PyObject_GetAttrStringFn(d->py_mod, fname);
    if (!fn || vus_py_PyErr_OccurredFn()) {
        if (vus_py_PyErr_OccurredFn()) vus_py_PyErr_ClearFn();
        ext_set_err("外部函数 %s.%s 不存在", ns, fname);
        return -1;
    }
    void *tup = vus_py_PyTuple_NewFn(nargs);
    for (int i = 0; i < nargs; i++) {
        VusRTEnv env = { VUS_RT_BRIDGE_ABI, {0} };
        void *o = vus_py_rt_to_obj(&args[i], &env);
        if (!o) {
            ext_set_err("参数 %d 无法映射: %s", i + 1, env.errs[0] ? env.errs : "未知");
            if (tup) vus_py_Py_XDECREF_Fn(tup);
            vus_py_Py_XDECREF_Fn(fn);
            return -1;
        }
        vus_py_PyTuple_SetItemFn(tup, i, o);   /* 窃取引用 */
    }
    void *r = vus_py_PyObject_CallObjectFn(fn, tup);
    if (tup) vus_py_Py_XDECREF_Fn(tup);
    vus_py_Py_XDECREF_Fn(fn);
    if (!r) {
        char eb[192]; vus_py_err_text(eb, sizeof(eb));
        vus_py_PyErr_ClearFn();
        ext_set_err("%s.%s: %s", ns, fname, eb);
        return -1;
    }
    VusRTEnv env = { VUS_RT_BRIDGE_ABI, {0} };
    int rc = vus_py_obj_to_rt(r, out, &env);
    vus_py_Py_XDECREF_Fn(r);
    return rc;
}

int vus_py_ext_get(const char *ns, const char *vname, VusRTValue *out) {
    VusExtDomain *d = ext_ns_loaded(ns);
    if (!d) return -1;
    memset(out, 0, sizeof(*out));
    void *o = vus_py_PyObject_GetAttrStringFn(d->py_mod, vname);
    if (!o || vus_py_PyErr_OccurredFn()) {
        if (vus_py_PyErr_OccurredFn()) vus_py_PyErr_ClearFn();
        ext_set_err("<%s>.%s 不存在", ns, vname);
        return -1;
    }
    VusRTEnv env = { VUS_RT_BRIDGE_ABI, {0} };
    int rc = vus_py_obj_to_rt(o, out, &env);
    vus_py_Py_XDECREF_Fn(o);
    return rc;
}

int vus_py_ext_set(const char *ns, const char *vname, const VusRTValue *val) {
    VusExtDomain *d = ext_ns_loaded(ns);
    if (!d) return -1;
    VusRTEnv env = { VUS_RT_BRIDGE_ABI, {0} };
    void *o = vus_py_rt_to_obj(val, &env);
    if (!o) { ext_set_err("值无法映射: %s", env.errs[0] ? env.errs : "未知"); return -1; }
    int is_none = (o == vus_py_Py_None);
    if (vus_py_PyObject_SetAttrStringFn(d->py_mod, vname, o) != 0) {
        char eb[192]; vus_py_err_text(eb, sizeof(eb));
        vus_py_PyErr_ClearFn();
        if (!is_none) vus_py_Py_XDECREF_Fn(o);
        ext_set_err("<%s>.%s 写入失败: %s", ns, vname, eb);
        return -1;
    }
    if (!is_none) vus_py_Py_XDECREF_Fn(o);
    return 0;
}
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

/* ---- Android 轻量能力（APK：Java 平台桥；桌面：无害降级） ----
 * 振动/剪贴板/设备信息/Toast 均为纯 Java 实现（VuaBridge.callJava），
 * 桌面侧无对应硬件/服务，按"空操作成功 / 空值"降级，脚本无需写平台分支。 */

VusString* vus_plugin_vibrate(VusString* ms) {
    if (ms && vus_string_cstr(ms)) {
        char *aj = vus_java_json_kv("ms", vus_string_cstr(ms));
        VusString *jr = aj ? vus_java_rpc("vibrate", aj) : NULL;
        free(aj);
        if (jr) return jr;
    }
    return vus_string_new("0");   /* 桌面无振动硬件语义：空操作成功 */
}

VusString* vus_plugin_clipboard_read(void) {
    VusString *jr = vus_java_rpc("clipboard.read", "{}");
    if (jr) return jr;
    return vus_string_new("");    /* 桌面无系统剪贴板：返回空串 */
}

VusString* vus_plugin_clipboard_write(VusString* text) {
    if (text && vus_string_cstr(text)) {
        char *aj = vus_java_json_kv("text", vus_string_cstr(text));
        VusString *jr = aj ? vus_java_rpc("clipboard.write", aj) : NULL;
        free(aj);
        if (jr) return jr;
    }
    return vus_string_new("0");
}

VusString* vus_plugin_device_info(void) {
    VusString *jr = vus_java_rpc("device.info", "{}");
    if (jr) return jr;
    return vus_string_new("{\"品牌\":\"desktop\",\"型号\":\"\",\"系统版本\":\"\",\"SDK\":0}");
}

VusString* vus_plugin_toast(VusString* text, VusString* is_long) {
    if (text && vus_string_cstr(text)) {
        const char *tl = (is_long && vus_string_cstr(is_long) && vus_string_cstr(is_long)[0] == '1') ? "1" : "0";
        char *aj = vus_java_json_2("text", vus_string_cstr(text), "long", tl);
        VusString *jr = aj ? vus_java_rpc("toast", aj) : NULL;
        free(aj);
        if (jr) return jr;
    }
    return vus_string_new("0");
}

VusString* vus_plugin_share_text(VusString* text) {
    if (text && vus_string_cstr(text)) {
        char *aj = vus_java_json_kv("text", vus_string_cstr(text));
        VusString *jr = aj ? vus_java_rpc("share.text", aj) : NULL;
        free(aj);
        if (jr) return jr;
    }
    return vus_string_new("0");   /* 桌面无分享面板：空操作成功 */
}

VusString* vus_plugin_battery_status(void) {
    VusString *jr = vus_java_rpc("battery.status", "{}");
    if (jr) return jr;
    return vus_string_new("{\"电量\":-1,\"充电中\":false}");   /* 桌面无电源管理：电量未知 */
}

VusString* vus_plugin_screen_keepon(VusString* flag) {
    const char *f = (flag && vus_string_cstr(flag)) ? vus_string_cstr(flag) : "0";
    char *aj = vus_java_json_kv("flag", f);
    VusString *jr = aj ? vus_java_rpc("screen.keepon", aj) : NULL;
    free(aj);
    if (jr) return jr;
    return vus_string_new("0");
}

VusString* vus_plugin_network_type(void) {
    VusString *jr = vus_java_rpc("network.type", "{}");
    if (jr) return jr;
    return vus_string_new("none");
}

VusString* vus_plugin_notify_send(VusString* title, VusString* body) {
    if (title && vus_string_cstr(title)) {
        const char *b = (body && vus_string_cstr(body)) ? vus_string_cstr(body) : "";
        char *aj = vus_java_json_2("title", vus_string_cstr(title), "body", b);
        VusString *jr = aj ? vus_java_rpc("notify.send", aj) : NULL;
        free(aj);
        if (jr) return jr;
    }
    return vus_string_new("0");
}

/* ---- 主题（APK：Java 平台桥切换并重建；桌面：无害降级） ---- */

VusString* vus_plugin_theme_set(VusString* name) {
    if (name && vus_string_cstr(name)) {
        char *aj = vus_java_json_kv("name", vus_string_cstr(name));
        VusString *jr = aj ? vus_java_rpc("theme.set", aj) : NULL;
        free(aj);
        if (jr) return jr;
    }
    return vus_string_new("0");   /* 桌面 GUI 主题不受脚本控制：空操作成功 */
}

VusString* vus_plugin_theme_get(void) {
    VusString *jr = vus_java_rpc("theme.get", "{}");
    if (jr) return jr;
    return vus_string_new("浅色");
}

VusString* vus_plugin_theme_primary(VusString* color) {
    if (color && vus_string_cstr(color)) {
        char *aj = vus_java_json_kv("color", vus_string_cstr(color));
        VusString *jr = aj ? vus_java_rpc("theme.primary", aj) : NULL;
        free(aj);
        if (jr) return jr;
    }
    return vus_string_new("0");
}

/* ---- 媒体播放（APK：Java 平台桥 MediaPlayer/VideoView；桌面：无害降级） ----
 * 音乐_播放(源, 循环, 音量) / 音乐_停止 / 音乐_暂停 / 音乐_继续 /
 * 音乐_跳转(秒) / 音乐_状态() / 视频_播放(源)。
 * 桌面侧无系统媒体框架，调用时按"空操作成功/固定状态"降级，脚本无需分支。 */

VusString* vus_plugin_media_play(VusString* src, VusString* loop, VusString* volume) {
    const char *s = (src && vus_string_cstr(src)) ? vus_string_cstr(src) : "";
    if (!s[0]) return vus_string_new("0");
    const char *lp = (loop && vus_string_cstr(loop)) ? vus_string_cstr(loop) : "0";
    const char *vol = (volume && vus_string_cstr(volume)) ? vus_string_cstr(volume) : "1";
    char *aj = (char *)malloc(strlen(s) * 2 + strlen(lp) + strlen(vol) + 32);
    if (!aj) return vus_string_new("0");
    char *p = aj;
    p += sprintf(p, "{\"src\":\"");
    for (size_t i = 0; i < strlen(s); i++) {
        char c = s[i];
        if (c == '"' || c == '\\') *p++ = '\\';
        *p++ = c;
    }
    p += sprintf(p, "\",\"loop\":\"%s\",\"volume\":\"%s\"}", lp, vol);
    VusString *jr = vus_java_rpc("media.play", aj);
    free(aj);
    return jr ? jr : vus_string_new("0");
}

VusString* vus_plugin_media_stop(void) {
    VusString *jr = vus_java_rpc("media.stop", "{}");
    return jr ? jr : vus_string_new("0");
}

VusString* vus_plugin_media_pause(void) {
    VusString *jr = vus_java_rpc("media.pause", "{}");
    return jr ? jr : vus_string_new("0");
}

VusString* vus_plugin_media_resume(void) {
    VusString *jr = vus_java_rpc("media.resume", "{}");
    return jr ? jr : vus_string_new("0");
}

VusString* vus_plugin_media_seek(VusString* pos) {
    if (pos && vus_string_cstr(pos)) {
        char *aj = vus_java_json_kv("pos", vus_string_cstr(pos));
        VusString *jr = aj ? vus_java_rpc("media.seek", aj) : NULL;
        free(aj);
        if (jr) return jr;
    }
    return vus_string_new("0");
}

VusString* vus_plugin_media_status(void) {
    VusString *jr = vus_java_rpc("media.status", "{}");
    if (jr) return jr;
    /* 桌面降级：无播放器，返回统一空闲状态 */
    return vus_string_new("{\"state\":\"idle\",\"src\":\"\",\"loop\":false,\"err\":\"\",\"duration\":0,\"position\":0}");
}

VusString* vus_plugin_video_play(VusString* src) {
    if (src && vus_string_cstr(src)) {
        char *aj = vus_java_json_kv("src", vus_string_cstr(src));
        VusString *jr = aj ? vus_java_rpc("video.play", aj) : NULL;
        free(aj);
        if (jr) return jr;
    }
    return vus_string_new("0");
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

/* ---- 文本分割：按分隔符直接拆成列表（P7：免去「JSON 数组字符串 → JSON_解析」中间层） ----
 * 返回 VusObject*（TYPE_LIST，ref=1），元素为 VusString*（每段，含末尾空段）。
 * 空文本返回空列表；分隔符为空时默认按换行 \n。旧写法
 *  JSON_解析(文本_分割(...)) 依旧可用：vus_json_parse 对列表输入幂等返回。 */
void* vus_plugin_text_split(VusString* text, VusString* sep) {
    if (!text) return NULL;
    const char* s = vus_string_cstr(text);
    size_t slen = strlen(s);
    const char* sp = (sep && vus_string_cstr(sep)) ? vus_string_cstr(sep) : "\n";
    size_t splen = strlen(sp);
    if (splen == 0) splen = 1;

    VusObject* o = (VusObject*)calloc(1, sizeof(VusObject));
    if (!o) return NULL;
    o->ref = 1; o->magic = VUS_OBJECT_MAGIC; o->type = TYPE_LIST;
    o->u.list = vus_list_new(TYPE_MIXED);
    VusList* list = o->u.list;

    const char* start = s;
    const char* p = s;
    const char* end = s + slen;
    while (p <= end) {
        size_t remain = (size_t)(end - p);
        if (splen <= remain && memcmp(p, sp, splen) == 0) {
            vus_list_append(list, vus_string_new_len(start, (int)(p - start)));
            p += splen;
            start = p;
            continue;
        }
        /* 到达末尾：追加最后一段（含末尾空段处理） */
        if (p == end) {
            vus_list_append(list, vus_string_new_len(start, (int)(p - start)));
            break;
        }
        p++;
    }
    return o;
}

/* ---- 文本_行：按换行 \n 直接拆成列表（一步到位） ---- */
void* vus_plugin_text_lines(VusString* text) {
    return vus_plugin_text_split(text, NULL);
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

/* ============ 旧式标准库辅助函数（设计文档 §10.1 核心库接线） ============
 * 供生成器把旧式名称（长度/替换/取随机数/断言 等）映射到现代实现。
 * 语义均与对应的 文本_* 系列、列表_* 系列、字典_* 系列、文件_* 系列、
 * 日期_* 系列新式函数一致。 */
#include <limits.h>
#include <time.h>

static int vus_rt_seeded = 0;   /* 取随机数：进程内只播种一次 */

/* 长度：列表/字典返回元素个数，其余（含字符串）返回 UTF-8 字节长度 */
VusString* vus_length(void* obj) {
    if (!obj) return vus_to_string(0);
    if (vus_is_object(obj)) {
        VusObject* o = (VusObject*)obj;
        switch (o->type) {
            case TYPE_LIST:  return vus_to_string(vus_list_len(o->u.list));
            case TYPE_DICT:  return vus_to_string(vus_dict_len(o->u.dict));
            case TYPE_STR:   return vus_to_string(o->u.str ? o->u.str->len : 0);
            default:         return vus_to_string(0);
        }
    }
    return vus_to_string(((VusString*)obj)->len);
}

/* 替换：把 text 中所有 old_s 出现替换为 rep，返回新字符串（不修改原串）。
 * old_s 为空时原样返回 text 的副本。 */
VusString* vus_string_replace(VusString* text, VusString* old_s, VusString* rep) {
    if (!text) return vus_string_new("");
    const char* hay   = text->data ? text->data : "";
    int hlen          = text->len;
    int nlen          = (old_s && old_s->data) ? old_s->len : 0;
    const char* repl  = (rep && rep->data) ? rep->data : "";
    size_t rlen       = strlen(repl);
    if (nlen <= 0) return vus_string_new_len(hay, hlen);

    size_t count = 0;
    for (int i = 0; i + nlen <= hlen; ) {
        if (memcmp(hay + i, old_s->data, (size_t)nlen) == 0) { count++; i += nlen; }
        else i++;
    }
    if (count == 0) return vus_string_new_len(hay, hlen);
    size_t total = (size_t)hlen + count * (rlen > (size_t)nlen ? rlen - (size_t)nlen : 0);
    if (total > (size_t)INT_MAX) return vus_string_new_len(hay, hlen);   /* 溢出防护：回退原文 */
    char* out = (char*)malloc(total + 1);
    if (!out) return vus_string_new_len(hay, hlen);
    size_t p = 0;
    for (int i = 0; i < hlen; ) {
        if (i + nlen <= hlen && memcmp(hay + i, old_s->data, (size_t)nlen) == 0) {
            memcpy(out + p, repl, rlen); p += rlen; i += nlen;
        } else {
            out[p++] = hay[i++];
        }
    }
    out[p] = '\0';
    VusString* r = vus_string_new_len(out, (int)p);
    free(out);
    return r;
}

/* 取随机数：[最小值, 最大值] 闭区间随机整数（过程内只播种一次） */
VusString* vus_random_int(VusString* min_s, VusString* max_s) {
    int err = 0;
    long long lo = min_s ? vus_to_int(min_s, &err) : 0;
    long long hi = max_s ? vus_to_int(max_s, &err) : lo + 1;
    if (hi < lo) { long long t = lo; lo = hi; hi = t; }
    if (!vus_rt_seeded) {
        srand((unsigned)time(NULL) ^ (unsigned)(uintptr_t)(void*)min_s);
        vus_rt_seeded = 1;
    }
    if (hi <= lo) return vus_to_string(lo);
    return vus_to_string(lo + (rand() % (hi - lo + 1)));
}

/* 断言失败：打印消息并以退出码 1 终止进程（不触发 尝试/捕获 异常链） */
void vus_assert_fail(VusString* msg) {
    fprintf(stderr, "断言失败: %s\n", (msg && msg->data) ? msg->data : "");
    exit(1);
}