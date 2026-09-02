/*
 * vua_smoke.c — VUA 运行时冒烟测试（不依赖完整 libvus_rt）
 *
 * 仅 stub 出 vua.c 真正用到的几个 Vus* 函数，隔离编译：
 *   rt/vua.c + rt/yyjson/yyjson.c + tests/vua_smoke.c + (-I rt)
 *
 * 用 gcc 编译运行：
 *   gcc -I rt -Wall -O0 rt/vua.c rt/yyjson/yyjson.c tests/vua_smoke.c -o /tmp/vua_smoke
 *   /tmp/vua_smoke
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../rt/libvus_rt.h"
#include "../rt/vua.h"

/* ================= 极简 libvus_rt stubs（仅覆盖 vua.c 所用） ================= */

void vus_ref(void *obj) { if (obj) ((int *)obj)[0]++; }

void vus_unref(void *obj) {
    if (!obj) return;
    int *r = (int *)obj;
    if (--(*r) <= 0) free(obj);
}

VusString *vus_string_new(const char *s) {
    if (!s) s = "";
    size_t l = strlen(s);
    VusString *st = (VusString *)malloc(sizeof(VusString));
    st->ref = 1; st->len = (int)l;
    st->data = (char *)malloc(l + 1);
    memcpy(st->data, s, l + 1);
    return st;
}

typedef struct { char *key; void *val; } KV;
typedef struct { KV *kv; int n; int cap; } TImpl;

VusDict *vus_dict_new(void) {
    VusDict *d = (VusDict *)malloc(sizeof(VusDict));
    d->ref = 1; d->impl = calloc(1, sizeof(TImpl));
    return d;
}

static TImpl *DI(VusDict *d) { return (TImpl *)d->impl; }

void vus_dict_set(VusDict *d, VusString *key, void *val) {
    if (!d || !key) return;
    TImpl *t = DI(d);
    for (int i = 0; i < t->n; i++)
        if (strcmp(t->kv[i].key, key->data) == 0) { t->kv[i].val = val; return; }
    if (t->n >= t->cap) { t->cap = t->cap ? t->cap * 2 : 4; t->kv = (KV *)realloc(t->kv, (size_t)t->cap * sizeof(KV)); }
    t->kv[t->n].key = strdup(key->data);
    t->kv[t->n].val = val;
    t->n++;
}

void *vus_dict_get(VusDict *d, VusString *key) {
    if (!d || !key) return NULL;
    TImpl *t = DI(d);
    for (int i = 0; i < t->n; i++)
        if (strcmp(t->kv[i].key, key->data) == 0) return t->kv[i].val;
    return NULL;
}

void vus_dict_remove(VusDict *d, VusString *key) {
    if (!d || !key) return;
    TImpl *t = DI(d);
    for (int i = 0; i < t->n; i++)
        if (strcmp(t->kv[i].key, key->data) == 0) {
            free(t->kv[i].key);
            memmove(&t->kv[i], &t->kv[i + 1], (size_t)(t->n - i - 1) * sizeof(KV));
            t->n--; return;
        }
}

void vus_closure_call(VusClosure *c, void *args) {
    if (c && c->func) c->func(c->env, args);
}

/* D 段用：按ID触发后应被调到的 handler */
static const char *g_triggered = NULL;
static void my_handler(void *env, void *args) { (void)env; (void)args; g_triggered = "保存"; }

/* ================= 断言 ================= */

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { if (cond) { pass++; } else { fail++; printf("  FAIL: %s\n", msg); } } while (0)

int main(void) {
    VuaError err;
    memset(&err, 0, sizeof(err));

    printf("== 预加载词典（覆盖示例属性） ==\n");
    vua_dict_load(
        "{\"词典\":{\"内容\":\"value\",\"文字\":\"label\",\"标题\":\"title\","
        "\"标签\":\"label\",\"提示\":\"placeholder\"}}", &err);
    CHECK(err.code == 0, "字典加载");

    printf("== A: 基本解析 + 渲染树 ==\n");
    const char *vua1 = "{"
        "\"type\":\"界面\",\"标题\":\"主\",\"子组件\":["
        "{\"type\":\"文本\",\"内容\":\"你好\"},"
        "{\"type\":\"按钮\",\"id\":\"b1\",\"文字\":\"保存\",\"点击\":{\"事件名\":\"保存\",\"回调变量\":[\"金额\"]}},"
        "{\"type\":\"输入框\",\"id\":\"in1\",\"变量\":\"金额\",\"标签\":\"金额\",\"提示\":\"金额\"}"
        "]}";
    VuaScreen *s = vua_screen_load(vua1, &err);
    CHECK(s != NULL, "加载成功");
    if (s) {
        const char *rt = vua_screen_dump_rendertree(s);
        printf("渲染树:\n%s\n", rt ? rt : "(null)");
        CHECK(rt != NULL, "渲染树非空");
        if (rt) {
            CHECK(strstr(rt, "\"children\":") != 0, "子组件已归一为 children");
            CHECK(strstr(rt, "\"event\":") != 0, "事件被归一为 event");
            CHECK(strstr(rt, "\"eventIndex\":") != 0, "顶层有 eventIndex");
            CHECK(strstr(rt, "\"variable\":\"金额\"") != 0, "输入框带 variable");
            CHECK(strstr(rt, "\"collect\":[\"金额\"]") != 0, "回调变量 collect");
        }
        vua_screen_free(s);
    }

    printf("== B: 未知字段应拒绝 ==\n");
    err.code = 0;
    const char *vua3 = "{\"type\":\"界面\",\"乱码\":\"x\"}";
    VuaScreen *s3 = vua_screen_load(vua3, &err);
    CHECK(s3 == NULL, "未知字段导致加载失败");
    CHECK(err.code == VUA_ERR_UNKNOWN_FIELD, "错误码为 UNKNOWN_FIELD");
    if (s3) vua_screen_free(s3);

    printf("== D: 按ID触发 → 事件派发到 handler ==\n");
    {
        err.code = 0;
        VuaScreen *sd = vua_screen_load(vua1, &err);
        CHECK(sd != NULL, "按ID触发: 加载成功");
        if (sd) {
            VusClosure c; memset(&c, 0, sizeof(c)); c.ref = 1; c.func = my_handler; c.env = NULL;
            CHECK(vua_on(sd, "保存", &c) == 0, "按ID触发: 绑定事件");
            /* 置一个变量，验证 collect 读取路径 */
            VusString *k = vus_string_new("金额");
            VusString *val = vus_string_new("1280");
            vua_state_set(sd, k, val);
            vua_trigger_by_id(sd, "b1", NULL);
            CHECK(g_triggered != NULL, "按ID触发: handler 被调用");
            vua_screen_free(sd);
        }
    }

    printf("== C: 未知控件类型应拒绝（加载控件表后）==\n");
    err.code = 0;
    vua_control_table_load("{\"控件表\":{\"界面\":{\"字段\":{}}}}", &err);
    CHECK(err.code == 0, "控件表加载");
    const char *vua4 = "{\"type\":\"界面\",\"子组件\":[{\"type\":\"不存在控件\"}]}";
    VuaScreen *s4 = vua_screen_load(vua4, &err);
    CHECK(s4 == NULL, "未知类型导致加载失败");
    CHECK(err.code == VUA_ERR_UNKNOWN_TYPE, "错误码为 UNKNOWN_TYPE");
    if (s4) vua_screen_free(s4);

    vua_rt_shutdown();
    printf("\n== %d 通过, %d 失败 ==\n", pass, fail);
    return fail ? 1 : 0;
}