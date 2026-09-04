/*
 * vus_rt_shim.c — VUS 运行时精简实现（Android APK 测试用）
 *
 * 只实现 APK 测试程序实际用到的 libvus_rt 子集：
 *   引用计数(vus_ref/vus_unref) / 字符串 / 字典 / 闭包 / 调用栈。
 * 跳过 elog、协程、curl、GuiLite 等依赖，使 .so 可干净地交叉编译。
 * 结构体布局照 libvus_rt.h，仅做最简实现（不做内容深度引用计数）。
 */

#include "libvus_rt.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---------- 引用计数 ---------- */
void vus_ref(void *obj) {
    if (!obj) return;
    int *r = (int *)obj;
    (*r)++;
}

void vus_unref(void *obj) {
    if (!obj) return;
    if (vus_is_object(obj)) {
        /* 结构化对象：略（测试不深用），仅减计数 */
        int *r = (int *)obj;
        if (--(*r) <= 0) free(obj);
        return;
    }
    int *r = (int *)obj;
    if (--(*r) <= 0) {
        free(obj); /* 单块分配：头 + data 一次 malloc，data 指向块内 */
    }
}

/* ---------- 字符串 ---------- */
VusString *vus_string_new(const char *s) {
    if (!s) s = "";
    size_t l = strlen(s);
    VusString *st = (VusString *)malloc(sizeof(VusString) + l + 1);
    if (!st) return NULL;
    st->ref = 1;
    st->len = (int)l;
    st->data = (char *)(st + 1);
    memcpy(st->data, s, l + 1);
    return st;
}

VusString *vus_string_new_len(const char *s, int len) {
    if (!s) return vus_string_new("");
    return vus_string_new(s); /* 简化：按 C 字符串 */
}

int vus_string_len(VusString *s) { return s ? s->len : 0; }
char *vus_string_cstr(VusString *s) { return s ? (char *)s->data : (char *)""; }

VusString *vus_string_concat(VusString *a, VusString *b) {
    if (!a) return vus_string_new(b ? b->data : "");
    if (!b) return vus_string_new(a->data);
    size_t la = a->len, lb = b->len;
    char *buf = (char *)malloc(la + lb + 1);
    if (!buf) return NULL;
    memcpy(buf, a->data, la);
    memcpy(buf + la, b->data, lb);
    buf[la + lb] = '\0';
    VusString *r = vus_string_new(buf);
    free(buf);
    return r;
}

/* ---------- 列表（简化，够测试用） ---------- */
VusList *vus_list_new(int type) {
    VusList *l = (VusList *)calloc(1, sizeof(VusList));
    if (l) { l->ref = 1; l->type = type; }
    return l;
}
void vus_list_append(VusList *list, void *item) {
    if (!list) return;
    if (list->len >= list->cap) {
        list->cap = list->cap ? list->cap * 2 : 4;
        list->items = (void **)realloc(list->items, (size_t)list->cap * sizeof(void *));
    }
    list->items[list->len++] = item;
}
void *vus_list_get(VusList *list, int index) {
    return (list && index >= 0 && index < list->len) ? list->items[index] : NULL;
}
int vus_list_len(VusList *list) { return list ? list->len : 0; }

/* ---------- 字典（链式，键为字符串副本） ---------- */
typedef struct { char *key; void *val; } VusKV;
typedef struct { VusKV *kv; int n; int cap; } VusImpl;

VusDict *vus_dict_new(void) {
    VusDict *d = (VusDict *)calloc(1, sizeof(VusDict));
    if (d) { d->ref = 1; d->impl = calloc(1, sizeof(VusImpl)); }
    return d;
}

static VusImpl *VUSI(VusDict *d) { return (VusImpl *)d->impl; }

void vus_dict_set(VusDict *dict, VusString *key, void *value) {
    if (!dict || !key) return;
    VusImpl *t = VUSI(dict);
    for (int i = 0; i < t->n; i++) {
        if (strcmp(t->kv[i].key, key->data) == 0) { t->kv[i].val = value; return; }
    }
    if (t->n >= t->cap) {
        t->cap = t->cap ? t->cap * 2 : 8;
        t->kv = (VusKV *)realloc(t->kv, (size_t)t->cap * sizeof(VusKV));
    }
    t->kv[t->n].key = (char *)malloc(strlen(key->data) + 1);
    if (t->kv[t->n].key) strcpy(t->kv[t->n].key, key->data);
    t->kv[t->n].val = value;
    t->n++;
}

void *vus_dict_get(VusDict *dict, VusString *key) {
    if (!dict || !key) return NULL;
    VusImpl *t = VUSI(dict);
    for (int i = 0; i < t->n; i++) {
        if (strcmp(t->kv[i].key, key->data) == 0) return t->kv[i].val;
    }
    return NULL;
}

void vus_dict_remove(VusDict *dict, VusString *key) {
    if (!dict || !key) return;
    VusImpl *t = VUSI(dict);
    for (int i = 0; i < t->n; i++) {
        if (strcmp(t->kv[i].key, key->data) == 0) {
            free(t->kv[i].key);
            t->kv[i] = t->kv[t->n - 1];
            t->n--;
            return;
        }
    }
}

int vus_dict_len(VusDict *dict) { return dict ? VUSI(dict)->n : 0; }
VusList *vus_dict_keys(VusDict *dict) { return vus_list_new(TYPE_STR); }

/* ---------- 闭包 ----------
 * struct VusClosure 已在 libvus_rt.h 定义（ref/func/env），此处不再重复 */

VusClosure *vus_closure_new(void (*func)(void *, void *), void *env) {
    VusClosure *c = (VusClosure *)calloc(1, sizeof(VusClosure));
    if (c) { c->ref = 1; c->func = func; c->env = env; }
    return c;
}

void vus_closure_call(VusClosure *closure, void *args) {
    if (closure && closure->func) closure->func(closure->env, args);
}

/* ---------- 调用栈（vus_stack_push/pop，调试用，仅占位） ---------- */
void vus_stack_push(const char *name) { (void)name; }
void vus_stack_pop(void) { }

/* ---------- 结构化对象（测试不深用） ---------- */
VusString *vus_object_to_string(void *obj) { return vus_string_new("{}"); }