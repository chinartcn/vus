/*
 * vus_rt_c_impl.c — Runtime FFI Bridge：C 域实现
 *
 * 文档：docs/designs/2026-09-06-runtime-ffi-c-impl.md（c-impl）
 * 协作边界（c-impl §0）：本文件只实现 C 域四个接入函数：
 *   vus_c_ext_load / vus_c_ext_call / vus_c_ext_get / vus_c_ext_set
 * 桥注册表公共部分、vus_ext_* 接口、生成器/lexer 由公共前置提供；
 * 域描述符（c_mod/c_handle）经访问器 vus_ext_c_register/module/handle 读写。
 *
 * 能力（对照 c-impl §4）：
 *   - dlopen + dlsym("vus_rt_module_entry") 装载，域内查重（funcs 内/vars 内/交叉）
 *   - init(参数 dict, env) 初始化（可 NULL）；失败 dlclose 不登记
 *   - 函数调用：min/max 参数校验 → 插件 call → out 交桥层转换
 *   - 变量读写：slot 优先（直接指针读写 VusRTValue 布局）/ 回调（get/set 对）；
 *     严格类型校验（readonly + type），全局导出槽由公共分发（本域不重复）
 */
#define _GNU_SOURCE      /* strdup（C11 下非标准函数，需 feature 宏） */
#include "libvus_rt.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* vus_rtval_free（libvus_rt.c 定义，递归释放 VusRTValue 堆内存；C 域 slot 写前释放旧值） */
void vus_rtval_free(VusRTValue *v);

/* ---- 内部工具 ---- */

/* VusRTType → 类型名（错误消息用） */
static const char *c_rt_type_name(VusRTType t) {
    switch (t) {
        case VUS_RT_NIL:   return "空";
        case VUS_RT_INT:   return "整数";
        case VUS_RT_FLOAT: return "浮点";
        case VUS_RT_BOOL:  return "布尔";
        case VUS_RT_STR:   return "字符串";
        case VUS_RT_LIST:  return "列表";
        case VUS_RT_DICT:  return "字典";
    }
    return "未知";
}

/* 递归深拷贝 VusRTValue（str/list/dict）；out 须为已清零内存。
 * slot 读/写、call 参数副本共用；配合 vus_rtval_free 释放。 */
static void c_rtval_deepcopy(const VusRTValue *s, VusRTValue *d) {
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
                c_rtval_deepcopy(&s->v.arr.items[i], &d->v.arr.items[i]);
            break;
        }
        case VUS_RT_DICT: {
            d->v.map.n = s->v.map.n;
            d->v.map.pairs = s->v.map.n ? (VusRTKv *)calloc((size_t)s->v.map.n, sizeof(VusRTKv)) : NULL;
            for (int i = 0; i < s->v.map.n; i++) {
                d->v.map.pairs[i].k = s->v.map.pairs[i].k ? strdup(s->v.map.pairs[i].k) : NULL;
                d->v.map.pairs[i].v = (VusRTValue *)calloc(1, sizeof(VusRTValue));
                if (s->v.map.pairs[i].v)
                    c_rtval_deepcopy(s->v.map.pairs[i].v, d->v.map.pairs[i].v);
            }
            break;
        }
        default:
            /* 标量（INT/FLOAT/BOOL/NIL）：值复制——NOT 跳过，否则 slot 读回恒为 0 */
            d->v = s->v;
            break;
    }
}

/* 递归规范化 STR：公共分发层（vus_ext_get_v/call_v）返回后对 out 调
 * vus_rtval_free（free s）。插件 get/call 写出的 s 可能指向静态/调用期临时内存，
 * 这里统一替换为 strdup 副本，交给公共层释放——与 Python 域（obj_to_rt strdup）对齐。 */
static void c_rtval_strs_dup(const VusRTValue *s, VusRTValue *d) {
    memset(d, 0, sizeof(*d));
    if (!s) return;
    if (s->t == VUS_RT_STR) {
        d->t = VUS_RT_STR;
        d->v.s = s->v.s ? strdup(s->v.s) : NULL;
        return;
    }
    c_rtval_deepcopy(s, d);   /* 递归复制标量/容器（容器内 STR 亦 strdup） */
}
static VusRTFunc *c_find_func(VusRTFunc *table, const char *name) {
    if (!table || !name) return NULL;
    for (int i = 0; table[i].name && table[i].name[0]; i++) {
        if (strcmp(table[i].name, name) == 0) return &table[i];
    }
    return NULL;
}

/* 在变量表（空 name 哨兵结尾）线性查找；返回 NULL = 未找到 */
static VusRTVar *c_find_var(VusRTVar *table, const char *name) {
    if (!table || !name) return NULL;
    for (int i = 0; table[i].name && table[i].name[0]; i++) {
        if (strcmp(table[i].name, name) == 0) return &table[i];
    }
    return NULL;
}

/* 域内查重（c-impl §4.2 第 3 条）：funcs 内重复、vars 内重复、函数与变量同名交叉。
 * 任一类重复 → 错误并返回 -1，不静默吞掉。 */
static int c_check_dup(VusRTFunc *funcs, VusRTVar *vars) {
    if (funcs) {
        for (int i = 0; funcs[i].name && funcs[i].name[0]; i++)
            for (int j = i + 1; funcs[j].name && funcs[j].name[0]; j++)
                if (strcmp(funcs[i].name, funcs[j].name) == 0) {
                    vus_ext_seterr("域内重复定义符号 %s（函数表）", funcs[i].name);
                    return -1;
                }
    }
    if (vars) {
        for (int i = 0; vars[i].name && vars[i].name[0]; i++)
            for (int j = i + 1; vars[j].name && vars[j].name[0]; j++)
                if (strcmp(vars[i].name, vars[j].name) == 0) {
                    vus_ext_seterr("域内重复定义符号 %s（变量表）", vars[i].name);
                    return -1;
                }
    }
    if (funcs && vars) {
        for (int i = 0; funcs[i].name && funcs[i].name[0]; i++)
            for (int j = 0; vars[j].name && vars[j].name[0]; j++)
                if (strcmp(funcs[i].name, vars[j].name) == 0) {
                    vus_ext_seterr("域内重复定义符号 %s（函数与变量同名）", funcs[i].name);
                    return -1;
                }
    }
    return 0;
}

/* ---- 模块入口 ---- */

/* 装载：dlopen 源 → 入口符号 → 查重 → init → 登记。
 * 文档 c-impl §4.2；init 失败 dlclose 且不登记（域不置 loaded）。 */
int vus_c_ext_load(const char *ns, const char *src, const VusRTValue *params) {
    if (!ns || !ns[0] || !src || !src[0]) { vus_ext_seterr("导入外部 参数无效"); return -1; }
    if (vus_ext_c_module(ns)) return 0;                 /* 幂等：已加载直接成功 */

    void *handle = dlopen(src, RTLD_NOW | RTLD_LOCAL);
    if (!handle) { vus_ext_seterr("导入外部 失败: %s: %s", src, dlerror()); return -1; }

    VusRTModuleEntryFn entry = (VusRTModuleEntryFn)dlsym(handle, "vus_rt_module_entry");
    if (!entry) {
        vus_ext_seterr("缺少 vus_rt_module_entry");
        dlclose(handle);
        return -1;
    }
    VusRTModule *mod = NULL;
    entry(&mod);
    if (!mod) {
        vus_ext_seterr("%s: 模块入口未返回模块描述符", src);
        dlclose(handle);
        return -1;
    }

    if (c_check_dup(mod->funcs, mod->vars) != 0) {
        dlclose(handle);
        return -1;
    }

    VusRTEnv env;
    memset(&env, 0, sizeof(env));
    env.abi_version_current = VUS_RT_BRIDGE_ABI;
    if (mod->init) {
        if (mod->init(params, &env) != 0) {
            vus_ext_seterr("%s init: %s",
                           mod->name ? mod->name : src,
                           env.errs[0] ? env.errs : "初始化失败");
            dlclose(handle);
            return -1;
        }
    }

    if (vus_ext_c_register(ns, mod, handle) != 0) {     /* 登记失败（别名未声明等） */
        dlclose(handle);
        return -1;
    }
    return 0;
}

/* 函数调用：查表 → min/max 校验 → 插件 call。
 * 文档 c-impl §4.3；values 由桥层构造（插件可改不越界），out 由插件写，
 * 成功归桥层按 §6.3 转 VUS 值。 */
int vus_c_ext_call(const char *ns, const char *fname, const VusRTValue *args, int nargs, VusRTValue *out) {
    VusRTModule *mod = vus_ext_c_module(ns);
    if (!mod) { vus_ext_seterr("外部域 %s 未加载", ns ? ns : "(空)"); return -1; }
    VusRTFunc *f = c_find_func(mod->funcs, fname);
    if (!f) { vus_ext_seterr("外部函数 %s.%s 不存在", ns, fname); return -1; }
    int min = f->min_args > 0 ? f->min_args : 0;
    if (nargs < min || (f->max_args >= 0 && nargs > f->max_args)) {
        if (f->max_args < 0)
            vus_ext_seterr("%s.%s 需要至少 %d 个参数，实际 %d", ns, fname, min, nargs);
        else
            vus_ext_seterr("%s.%s 需要 %d..%d 个参数，实际 %d", ns, fname, min, f->max_args, nargs);
        return -1;
    }
    VusRTEnv env;
    memset(&env, 0, sizeof(env));
    env.abi_version_current = VUS_RT_BRIDGE_ABI;
    VusRTValue raw = {0};
    if (f->call((VusRTValue *)args, nargs, &raw, &env) != 0) {
        vus_ext_seterr("%s.%s: %s", ns, fname, env.errs[0] ? env.errs : "调用失败");
        return -1;
    }
    /* 插件 out 的内存归插件（调用期间有效）；桥层 strdup 副本交给公共层释放 */
    c_rtval_strs_dup(&raw, out);
    return 0;
}

/* 变量读：slot 优先（深拷贝出，out 归桥层释放）；否则 get 回调。 */
int vus_c_ext_get(const char *ns, const char *vname, VusRTValue *out) {
    VusRTModule *mod = vus_ext_c_module(ns);
    if (!mod) { vus_ext_seterr("外部域 %s 未加载", ns ? ns : "(空)"); return -1; }
    VusRTVar *v = c_find_var(mod->vars, vname);
    if (!v) { vus_ext_seterr("<%s>.%s 不存在", ns, vname); return -1; }
    if (v->slot) {
        c_rtval_deepcopy((VusRTValue *)v->slot, out);
        return 0;
    }
    if (!v->get) { vus_ext_seterr("<%s>.%s 无读取方式", ns, vname); return -1; }
    VusRTEnv env;
    memset(&env, 0, sizeof(env));
    env.abi_version_current = VUS_RT_BRIDGE_ABI;
    VusRTValue raw = {0};
    if (v->get(vname, &raw, &env) != 0) {
        vus_ext_seterr("<%s>.%s: %s", ns, vname, env.errs[0] ? env.errs : "读取失败");
        return -1;
    }
    c_rtval_strs_dup(&raw, out);
    return 0;
}

/* 变量写：readonly 校验 → slot 直接写（先释放旧值再深拷贝，类型严格校验）；
 * 否则 set 回调。 */
int vus_c_ext_set(const char *ns, const char *vname, const VusRTValue *val) {
    VusRTModule *mod = vus_ext_c_module(ns);
    if (!mod) { vus_ext_seterr("外部域 %s 未加载", ns ? ns : "(空)"); return -1; }
    VusRTVar *v = c_find_var(mod->vars, vname);
    if (!v) { vus_ext_seterr("<%s>.%s 不存在", ns, vname); return -1; }
    if (v->readonly) { vus_ext_seterr("<%s>.%s 为只读", ns, vname); return -1; }
    if (v->slot) {
        if (val->t != v->type) {
            vus_ext_seterr("<%s>.%s 类型不匹配: 期望 %s 实际 %s",
                           ns, vname, c_rt_type_name(v->type), c_rt_type_name(val->t));
            return -1;
        }
        vus_rtval_free((VusRTValue *)v->slot);
        c_rtval_deepcopy(val, (VusRTValue *)v->slot);
        return 0;
    }
    if (!v->set) { vus_ext_seterr("<%s>.%s 无写入方式", ns, vname); return -1; }
    VusRTEnv env;
    memset(&env, 0, sizeof(env));
    env.abi_version_current = VUS_RT_BRIDGE_ABI;
    if (v->set(vname, val, &env) != 0) {
        vus_ext_seterr("<%s>.%s: %s", ns, vname, env.errs[0] ? env.errs : "写入失败");
        return -1;
    }
    return 0;
}