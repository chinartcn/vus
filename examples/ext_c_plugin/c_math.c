/*
 * c_math.c — FFI Bridge C 域示例插件（接入点 c-impl §8）
 *
 * 构建：gcc -shared -fPIC -I<include> -o c_math.so c_math.c
 * 只依赖 vus_rt_bridge.h（不链接 libvus），导出唯一入口 vus_rt_module_entry。
 * 演示三种变量机制：指针槽（计数）、回调槽（比例）、只读（版本）；
 * 函数含定参（加法）、可变参数（求和）与自定义类型校验失败路径。
 */
#include "vus_rt_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- 指针槽（slot 优先：桥层按 VusRTValue 布局直接读写该内存） ---- */
static VusRTValue g_slot_count = { .t = VUS_RT_INT, .v = { .i64 = 0 } };

/* ---- 回调槽（slot 为 NULL 时走 get/set 回调） ---- */
static int   g_ratio = 3;                 /* 插件侧私有存储，桥层不直接触碰 */

/* ---- 只读变量（readonly=1，VUS 侧写入被拒绝） ---- */
static const char *g_version = "1.0.0";

/* ---- 函数：加法（定参 2） ---- */
static int fn_add(VusRTValue *v, int n, VusRTValue *out, VusRTEnv *e) {
    if (n != 2 || v[0].t != VUS_RT_INT || v[1].t != VUS_RT_INT) {
        snprintf(e->errs, sizeof(e->errs), "add 需要两个整数");
        return -1;
    }
    out->t = VUS_RT_INT;
    out->v.i64 = v[0].v.i64 + v[1].v.i64;
    return 0;
}

/* ---- 函数：浮点乘法 ---- */
static int fn_mul(VusRTValue *v, int n, VusRTValue *out, VusRTEnv *e) {
    if (n != 2 || v[0].t != VUS_RT_FLOAT || v[1].t != VUS_RT_FLOAT) {
        snprintf(e->errs, sizeof(e->errs), "mul 需要两个浮点");
        return -1;
    }
    out->t = VUS_RT_FLOAT;
    out->v.f64 = v[0].v.f64 * v[1].v.f64;
    return 0;
}

/* ---- 函数：字符串拼接（str 参数复制语义） ---- */
static int fn_join(VusRTValue *v, int n, VusRTValue *out, VusRTEnv *e) {
    if (n != 2 || v[0].t != VUS_RT_STR || v[1].t != VUS_RT_STR) {
        snprintf(e->errs, sizeof(e->errs), "join 需要两个字符串");
        return -1;
    }
    size_t l0 = v[0].v.s ? strlen(v[0].v.s) : 0;
    size_t l1 = v[1].v.s ? strlen(v[1].v.s) : 0;
    char *buf = (char *)malloc(l0 + l1 + 1);
    if (!buf) { snprintf(e->errs, sizeof(e->errs), "内存不足"); return -1; }
    if (l0) memcpy(buf, v[0].v.s, l0);
    if (l1) memcpy(buf + l0, v[1].v.s, l1);
    buf[l0 + l1] = '\0';
    out->t = VUS_RT_STR;
    out->v.s = buf;      /* 桥层复制后释放；插件不持有所有权 */
    return 0;
}

/* ---- 函数：可变参数求和（max_args = -1） ---- */
static int fn_sum(VusRTValue *v, int n, VusRTValue *out, VusRTEnv *e) {
    long long acc = 0;
    for (int i = 0; i < n; i++) {
        if (v[i].t != VUS_RT_INT) {
            snprintf(e->errs, sizeof(e->errs), "sum 参数 #%d 需要整数", i + 1);
            return -1;
        }
        acc += v[i].v.i64;
    }
    out->t = VUS_RT_INT;
    out->v.i64 = acc;
    return 0;
}

/* ---- 回调槽 get/set ---- */
static int var_get_ratio(const char *name, VusRTValue *out, VusRTEnv *e) {
    (void)name; (void)e;
    out->t = VUS_RT_INT;
    out->v.i64 = g_ratio;
    return 0;
}
static int var_set_ratio(const char *name, const VusRTValue *val, VusRTEnv *e) {
    (void)name;
    if (val->t != VUS_RT_INT) {
        snprintf(e->errs, sizeof(e->errs), "比例需整数");
        return -1;
    }
    g_ratio = (int)val->v.i64;
    return 0;
}

/* ---- 只读变量 get 回调（readonly=1 拦截 VUS 侧写入；读仍须可读） ---- */
static int var_get_version(const char *name, VusRTValue *out, VusRTEnv *e) {
    (void)name; (void)e;
    out->t = VUS_RT_STR;
    out->v.s = g_version;
    return 0;
}

/* ---- init：可从导入外部 参数 dict 读基准（测试 参数 传递） ---- */
static int mod_init(const VusRTValue *params, VusRTEnv *e) {
    (void)e;
    if (params && params->t == VUS_RT_DICT) {
        for (int i = 0; i < params->v.map.n; i++) {
            if (params->v.map.pairs[i].k &&
                strcmp(params->v.map.pairs[i].k, "基准") == 0 &&
                params->v.map.pairs[i].v &&
                params->v.map.pairs[i].v->t == VUS_RT_INT) {
                g_slot_count.v.i64 = params->v.map.pairs[i].v->v.i64;
                g_ratio = (int)params->v.map.pairs[i].v->v.i64;
            }
        }
    }
    return 0;
}

/* ---- 清理 ---- */
static void mod_cleanup(void) {
    g_slot_count.v.i64 = -1;
}

static VusRTFunc g_funcs[] = {
    { "加法", 2, 2, fn_add },
    { "乘法", 2, 2, fn_mul },
    { "拼接", 2, 2, fn_join },
    { "求和", 0, -1, fn_sum },
    { "", 0, 0, NULL }                                /* 哨兵 */
};

static VusRTVar g_vars[] = {
    { "计数", VUS_RT_INT, 0, &g_slot_count, NULL, NULL },            /* 指针槽 */
    { "比例", VUS_RT_INT, 0, NULL, var_get_ratio, var_set_ratio },   /* 回调槽 */
    { "版本", VUS_RT_STR, 1, NULL, var_get_version, NULL },          /* 只读（读回调） */
    { "", 0, 0, NULL, NULL, NULL }                                   /* 哨兵 */
};

VUS_RT_EXPORT void vus_rt_module_entry(VusRTModule **m) {
    static VusRTModule mod = {
        .name = "c_math",
        .version = "1.0.0",
        .funcs = g_funcs,
        .vars = g_vars,
        .init = mod_init,
        .cleanup = mod_cleanup
    };
    *m = &mod;
}