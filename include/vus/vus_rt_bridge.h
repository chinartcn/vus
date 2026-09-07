/*
 * vus_rt_bridge.h — Runtime FFI Bridge 公共头文件（运行期外部桥）
 *
 * VUS 程序 ↔ Python 模块 / C 运行时插件 的函数互调与双向变量读写。
 * 本文档对应 docs/designs/2026-09-06-runtime-ffi-abi-design.md（总纲 §5）
 * 与 2026-09-06-runtime-ffi-c-impl.md（c-impl §2）。
 *
 * 协作边界（总纲 §10）：本文件由 Python 侧实现维护并提交仓库；
 * C 域实现者拉取仓库后取用，不得改动既有结构体段（扩展只加尾）。
 */
#ifndef VUS_VUS_RT_BRIDGE_H
#define VUS_VUS_RT_BRIDGE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 桥 ABI 版本（总纲 §8） */
#define VUS_RT_BRIDGE_ABI 1

/* 导出宏：插件编译时导出一个 vus_rt_module_entry 符号 */
#if defined(_WIN32) || defined(_WIN64)
#  define VUS_RT_EXPORT __declspec(dllexport)
#else
#  define VUS_RT_EXPORT __attribute__((visibility("default")))
#endif

/* ── 值（跨界统一中介类型） ── */
typedef enum {
    VUS_RT_NIL = 0,   /* 空（对应 VUS 空 / Python None） */
    VUS_RT_INT,
    VUS_RT_FLOAT,
    VUS_RT_BOOL,
    VUS_RT_STR,       /* UTF-8，调用期间有效，桥层复制 */
    VUS_RT_LIST,      /* 深拷贝语义 */
    VUS_RT_DICT,      /* 键必须为字符串 */
} VusRTType;

typedef struct VusRTValue VusRTValue;
typedef struct VusRTKv   VusRTKv;

struct VusRTKv {
    const char *k;
    VusRTValue *v;
};

struct VusRTValue {
    VusRTType t;
    union {
        long long i64;
        double    f64;
        int       b;
        const char *s;
        struct { VusRTValue *items; int n; } arr;
        struct { VusRTKv    *pairs; int n; } map;
    } v;
};

/* ── 环境/错误 ── */
typedef struct VusRTEnv {
    int     abi_version_current;   /* 恒 VUS_RT_BRIDGE_ABI */
    char    errs[256];             /* 插件写错误消息；空串=无错 */
} VusRTEnv;

/* ── 函数 ── */
typedef struct VusRTFunc {
    const char *name;              /* VUS 侧调用名；空串=表尾哨兵 */
    int  min_args;
    int  max_args;                 /* <0 表示可变 */
    int (*call)(VusRTValue *values, int nargs, VusRTValue *out, VusRTEnv *env);
} VusRTFunc;

/* ── 变量（slot 优先；否则 get/set 回调） ── */
typedef struct VusRTVar {
    const char *name;              /* 空串=表尾哨兵 */
    VusRTType   type;              /* 声明类型；读写严格校验 */
    int         readonly;          /* 1 = VUS 侧只读 */
    void       *slot;              /* 非 NULL：直接读写该内存（VusRTValue 布局） */
    int (*get)(const char *name, VusRTValue *out, VusRTEnv *env);
    int (*set)(const char *name, const VusRTValue *val, VusRTEnv *env);
} VusRTVar;

/* ── 模块描述符 ── */
typedef struct VusRTModule {
    const char      *name;         /* 域类型名（调试/错误用） */
    const char      *version;      /* 插件自身版本 "x.y.z" */
    VusRTFunc       *funcs;        /* 空 name 结尾；可为空表 */
    VusRTVar        *vars;         /* 空 name 结尾；可为空表 */
    int (*init)(const VusRTValue *init_params_dict, VusRTEnv *env); /* 可 NULL */
    void (*cleanup)(void);         /* 可 NULL */
    /* 扩展保留字段追加于此尾 */
} VusRTModule;

/* 模块入口（插件侧必须且仅定义一个该符号） */
typedef void (*VusRTModuleEntryFn)(VusRTModule **module);

#ifdef __cplusplus
}
#endif

#endif /* VUS_VUS_RT_BRIDGE_H */