# Runtime FFI Bridge — C 域实现规格（c-impl）

> 版本：impl-1.0
> 日期：2026-09-06
> 依赖总纲：[2026-09-06-runtime-ffi-abi-design.md](2026-09-06-runtime-ffi-abi-design.md)
> 协作文档（并行）：[2026-09-06-runtime-ffi-py-impl.md](2026-09-06-runtime-ffi-py-impl.md)
> 本文件实现责任：**C 域全部能力**（`vus_rt_bridge.h` 结构体全量、dlopen 装载、函数/变量表注册、宿主转换槽、类型转换、示例 `.so`、测试）。
> 公共前置（桥注册表骨架、`vus_ext_*` 接口、生成器/lexer 成员访问）由共享协同点提供；本域只填 `vus_c_ext_*` 与公共头文件。

## 0. 开工前必读（协作边界，防双写）

> ⚠️ 重要：**公共前置由 Python 侧实现唯一负责**（总纲 §10.1），可能在你开工时**尚未推送**。届时请按 §10.3 提交时序操作：先 `git pull` 最新 `master`，等 Python 侧 `ffi: 公共前置 + Python 域` 提交落地后，再以仓库中的 `include/vus/vus_rt_bridge.h` 与注册表分发点为准开始 C 域开发。

**你（C 侧）只写：**
1. `rt/vus_rt_c_impl.c`：实现 `vus_c_ext_load / vus_c_ext_call / vus_c_ext_get / vus_c_ext_set` 四个域内函数（§4）；
2. `examples/ext_c_plugin/c_math.c` + `examples/ext_c_plugin/vus.json`（§8 示例）；
3. `tests/test_ext_c.vus`（§9 测试清单）；
4. `docs/PLUGIN_USAGE.md` C 域章节。

**你不碰：** `include/vus/vus_rt_bridge.h`（已在仓库）、libvus_rt 桥注册表公共部分、lexer/parser/generator 的任何成员访问支持、`vus_ext_*` 公共接口。发现公共件缺陷 → 在 commit 说明后按总纲 §8 尾部扩展规则合入，不修改既有段偏移。

**域类型判定（注册表分发，见总纲 §10.2）：** `vus_ext_load` 按「源一串以 .so/.vulage 结尾或指向可加载文件」路由到你的 `vus_c_ext_load`；其余路由到 Python 域。

---

## 1. 交付物清单

| # | 交付物 | 位置 |
|---|--------|------|
| 1 | 公共头文件 `vus_rt_bridge.h`（§2 全量结构体 + §3 编译期标记） | `include/vus/vus_rt_bridge.h` |
| 2 | C 域装载与转换实现（编入 libvus_rt） | `rt/vus_rt_c_impl.c`（`rt/` 下新文件，Makefile 并入） |
| 3 | 示例插件 | `examples/ext_c_plugin/c_math.c` + `examples/ext_c_plugin/vus.json` |
| 4 | 回归测试 | `tests/test_ext_c.vus` |
| 5 | 文档章节 | `docs/PLUGIN_USAGE.md` 运行期外部桥·C 域 |

---

## 2. 公共头文件（`include/vus/vus_rt_bridge.h`，全量定义）

> 本文件即 C 域与桥、以及 Python 域共用数值中介的唯一类型来源；**旧字段永不删除/改造语义，扩展只加尾**。

```c
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

/* ── 值 ── */
typedef enum {
    VUS_RT_NIL = 0,
    VUS_RT_INT,
    VUS_RT_FLOAT,
    VUS_RT_BOOL,
    VUS_RT_STR,
    VUS_RT_LIST,
    VUS_RT_DICT,
} VusRTType;

typedef struct VusRTValue VusRTValue;
typedef struct VusRTKv   VusRTKv;

struct VusRTKv { const char *k; VusRTValue *v; };

struct VusRTValue {
    VusRTType t;
    union {
        long long i64;
        double    f64;
        int       b;
        const char *s;                       /* UTF-8，调用期间有效，桥层复制 */
        struct { VusRTValue *items; int n; } arr;
        struct { VusRTKv *pairs; int n; } map;
    } v;
};

/* ── 环境/错误 ── */
typedef struct VusRTEnv {
    int   abi_version_current;   /* 恒 VUS_RT_BRIDGE_ABI（1） */
    char  errs[256];             /* 插件写错误消息；空串=无错 */
} VusRTEnv;

/* ── 函数 ── */
typedef struct VusRTFunc {
    const char *name;            /* VUS 侧调用名；空串=NUL 哨兵 */
    int  min_args;               /* ≤0 表示 0 */
    int  max_args;               /* <0 表示可变 */
    int (*call)(VusRTValue *values, int nargs,
                VusRTValue *out, VusRTEnv *env);
} VusRTFunc;

/* ── 变量（slot 优先；否则回调） ── */
typedef struct VusRTVar {
    const char *name;            /* VUS 侧变量名；空串=NUL 哨兵 */
    VusRTType   type;            /* 声明类型；读写严格校验 */
    int         readonly;        /* 1 = VUS 侧只读 */
    void       *slot;            /* 非 NULL：VUS 直接读写该内存（VusRTValue 布局） */
    int (*get)(const char *name, VusRTValue *out, VusRTEnv *env);
    int (*set)(const char *name, const VusRTValue *val, VusRTEnv *env);
} VusRTVar;

/* ── 模块描述符 ── */
typedef struct VusRTModule {
    const char *name;
    const char *version;
    VusRTFunc  *funcs;           /* 空 name 结尾 */
    VusRTVar   *vars;            /* 空 name 结尾 */
    int (*init)(const VusRTValue *init_params_dict, VusRTEnv *env); /* 可 NULL */
    void (*cleanup)(void);       /* 可 NULL */
    /* 扩展保留字段追加于此尾 */
} VusRTModule;

/* ── 模块入口（插件侧必须且仅定义） ── */
typedef void (*VusRTModuleEntryFn)(VusRTModule **module);

#ifdef __cplusplus
}
#endif
#endif /* VUS_VUS_RT_BRIDGE_H */
```

---

## 3. 编译期标记与构建约定（插件侧）

- 插件编译：`gcc -shared -fPIC -I<include> -o my_plugin.so my_plugin.c`；
- 相同源码可用 `-DVUS_RT_BRIDGE_ABI` 参与自己版本控制，但运行期以 `VusRTEnv.abi_version_current` 为准；
- 插件**不链接 libvus**（纯头文件接口），只依赖 `vus_rt_bridge.h`；
- 平台注意：Android 上 `.so` 需用 `-llog` 勿链 GLIBC 专属；APK 运行期默认不加载 C 域（总纲 §9 红线），构建机/桌面可用。

---

## 4. C 域装载与接线（`rt/vus_rt_c_impl.c`）

### 4.1 域描述（桥注册表内）

```c
typedef struct {
    const char *别名;            /* ns 主键 */
    VusRTModule  *mod;           /* dlopen 后模块描述符 */
    void        *handle;         /* dlopen 句柄 */
    int          loaded;         /* init 成功 */
} VusCDomain;
```

### 4.2 `vus_c_ext_load(别名, 源, 参数VusRTValue*)`

1. `dlopen(源, RTLD_NOW | RTLD_LOCAL)`；失败 → 错误「导入外部 失败: <源>: <dlerror>」；
2. `dlsym(handle, "vus_rt_module_entry")`；缺失 → `dlclose` + 错误「缺少 vus_rt_module_entry」；
3. 调用入口拿到 `VusRTModule*`；校验 `funcs/vars` 表存在（可为空表=空哨兵）；**域内查重**：`funcs` 表内 `name` 重复、`vars` 表内 `name` 重复、函数与变量同名交叉重复 → 报错「域 <模块名> 重复定义符号 <名>」（对应总纲 §7.1.1 第 2 条），不静默吞掉；
4. 若 `mod->init` 非 NULL：构造 `VUS_RT_DICT` 参数值（参数 JSON→VusRTValue，见 §6）→ 调 `init(&dict, &env)`；返回非 0 或 `env.errs[0]` 非空 → 错误并 `dlclose`；
5. 登记域；成功后才置 `loaded=1`（幂等）。

### 4.3 `vus_c_ext_call(ns, 函数名, args[], n, out)`

1. 未加载 → 先 `load`；
2. 在 `funcs` 表线性/哈希查找 `name`；缺 → 错误「外部函数 <别名>.<名> 不存在」；
3. 校验 `min_args ≤ n ≤ max_args`（`<0` 可变）→ 错误带预期；
4. 逐参数类型校验：按 `key_args` 无声明时按值 `t` 与插件内部约定（插件自行校验）；桥层只做值 copy 与类型标签保全；
5. `func->call(args, n, out, &env)`；返回非 0 或 `env.errs[0]` → 错误；成功 → `out` 按 §6.3 转 VUS 值。

### 4.4 `vus_c_ext_get/set(别名, 变量名, …)`

1. 未加载 → 先 `load`；
2. 在 `vars` 表查找；缺 → 错误「<别名>.<名> 不存在」；
3. **slot 优先**：`slot` 非 NULL → 直接把 `(VusRTValue*)slot` 读出/写入（写前校验 `readonly`、`type`）；
4. slot 为 NULL → 调 `get/set` 回调；校验 `readonly`（set 时）、`type`；
5. 类型校验严格：传入 `VusRTValue.t` 与声明 `type` 不一致 → 错误「类型不匹配: 期望 <type> 实际 <type>」；
6. **全局导出槽命中**（变量名不在域 vars、在全局导出槽表）：转读 VUS 全局变量（§5）。

---

## 5. 宿主转换槽（VUS 全局导出变量 ↔ C）

VUS 全局变量是 `VusString*`/`VusObject*` 指针，不能直接当 `VusRTValue` 的 slot 内存。因此 `导出变量 ([...])` 对 C 域走「宿主转换槽」：

```c
typedef struct {
    const char *name;
    VusString **ptr;              /* &vus_x */
} VusHostSlot;                    /* 公共注册表项，本域合成 VusRTVar */
```

桥注册表维护 `g_ext_export`（name → VusHostSlot）。C 域访问路径：

- **读**：`slide = vus_val_to_rt(*ptr)`（VUS 值 → VusRTValue，§6.3）→ 装入 `out`；
- **写**：校验 `type` 后 `vus_rt_to_val` 得 VUS 值（出生 ref=1）→ `vus_var_set(ptr, 新值)`（复用既有引用管理，天然防泄漏/悬垂）→ 外部后续读 `*ptr` 即新值；
- 与 slot 语义等价：C 侧感知不到差异（都经过一个转换为宿主内存），但**不允许**把 `&vus_x` 直接当 slot 用（类型不匹配，防误改）。

> 对本域约定：`导出变量` 导出的名字在桥注册表登记为 VusHostSlot；域获取变量命中它时按上面读/写两条路径处理（当作「通知型 slot」）。

---

## 6. 类型转换（完整规格，`rt/vus_rt_c_impl.c`）

### 6.1 值 ↔ VUS 值（用于 call 出参入参、host slot 读写）

| VusRTType | VUS 值语义（`vus_val_to_rt`） | `vus_rt_to_val` 构造 |
|---|---|---|
| `VUS_RT_INT` | `VusObject*` TYPE_INT → `v.i64` | 整数对象 |
| `VUS_RT_FLOAT` | TYPE_FLOAT → `v.f64` | 浮点对象 |
| `VUS_RT_BOOL` | TYPE_BOOL → `v.b` | 布尔对象 |
| `VUS_RT_STR` | `VusString*`/TYPE_STR → `v.s`（UTF-8 复制） | `vus_string_new`/字符串对象 |
| `VUS_RT_LIST` | LIST → 逐项递归 | `vus_object_list` 递归构造 |
| `VUS_RT_DICT` | DICT → 逐键递归 | `vus_object_dict` 递归构造 |
| `VUS_RT_NIL` | 空对象 | NULL（VUS 空）|

- 递归深度 ≤ 64，超限 → 错误「容器嵌套过深（>64）」；环形/自引用容器由该上限兜底（总纲 §6.4）。
- 字符串复制无所有权转移；写回走 `vus_var_set`（见 §5）。
- dict 键必须字符串（`VUS_RT_DICT` 的 `k` 恒为字符串，插件侧不提供非串键接口）。
- **空（NULL）语义**（总纲 §6.6）：`VUS_RT_NIL` ⇄ VUS `空`（NULL 指针）⇄ `None`，无需包装；严格区分空与空串：`VUS_RT_NIL` → NULL，`VUS_RT_STR("")` → 空串对象；`vus_val_to_rt(NULL)` 产出 `VUS_RT_NIL`。容器内允许空元素。
- 转换函数签名（内部）：

```c
int vus_val_to_rt(void *vus_val, VusRTValue *out, VusRTEnv *env);
int vus_rt_to_val(const VusRTValue *in, void **out_vus, VusRTEnv *env);
```

### 6.2 参数（VusRTValue）与 JSON/源 转换

`导入外部` 的 `参数` 键值来自 VUS 字典 → 桥公共部分已统一为 `VusRTValue`（`VUS_RT_DICT`）。本域只需把该 dict 原样传 `init`，无需二次转换。

### 6.3 call 返回值约定

插件在 `out` 写 `VusRTValue`（可 `VUS_RT_NIL`）；桥层返回后由公共件调用 `vus_rt_to_val` 构造 VUS 对象（出生 ref=1，走 R6 收割）或 NULL（空）。插件不得在 `call` 内访问 `values` 之外的生命期对象。

---

## 7. 错误语义（对照总纲 §7.4）

| C 域场景 | 错误消息（`env.errs`） → VUS `"外部错误"` |
|---|---|
| dlopen 失败 | `导入外部 失败: <源>: <dlerror>` |
| 缺少入口符号 | `缺少 vus_rt_module_entry` |
| init 失败 | `<模块名> init: <env.errs>` |
| 函数不存在 | `外部函数 <别名>.<名> 不存在` |
| 参数个数 | `<别名>.<名> 需要 <min..max> 个参数，实际 <n>` |
| 变量不存在 | `<别名>.<名> 不存在` |
| 类型不匹配 | `类型不匹配: 期望 <type> 实际 <type>` |
| 只读写 | `<别名>.<名> 为只读` |
| 转换失败/深度 | `容器嵌套过深（>64）` 等 |

错误统一：桥调用点把 `env.errs` 带 `类型=外部错误` 抛入 VUS 错误码链。

---

## 8. 示例（`examples/ext_c_plugin/`）

`c_math.c`：

```c
#include "vus_rt_bridge.h"
#include <string.h>

static VusRTValue g_slot; /* 指针槽内存，桥层直接读写 */
static int counter;

static int add(VusRTValue *v, int n, VusRTValue *out, VusRTEnv *e) {
    if (n != 2 || v[0].t != VUS_RT_INT || v[1].t != VUS_RT_INT) {
        snprintf(e->errs, sizeof(e->errs), "add 需要两个整数"); return -1;
    }
    out->t = VUS_RT_INT; out->v.i64 = v[0].v.i64 + v[1].v.i64;
    return 0;
}
static int get_count(const char *n, VusRTValue *o, VusRTEnv *e) {
    o->t = VUS_RT_INT; o->v.i64 = counter; return 0;
}
static int set_count(const char *n, const VusRTValue *v, VusRTEnv *e) {
    if (v->t != VUS_RT_INT) { snprintf(e->errs, sizeof(e->errs), "计数需整数"); return -1; }
    counter = (int)v->v.i64; return 0;
}
static int mod_init(const VusRTValue *p, VusRTEnv *e) { return 0; }

static VusRTFunc g_funcs[] = {
    { "加法", 2, 2, add }, { "", 0, 0, NULL }
};
static VusRTVar g_vars[] = {
    { "计数", VUS_RT_INT, 0, &g_slot, NULL, NULL },
    { "", 0, 0, NULL, NULL, NULL }
};
VUS_RT_EXPORT void vus_rt_module_entry(VusRTModule **m) {
    static VusRTModule mod = {
        .name = "c_math", .version = "1.0.0", .funcs = g_funcs, .vars = g_vars,
        .init = mod_init, .cleanup = NULL
    };
    *m = &mod;
}
```

`test_ext_c.vus`：

```
导入外部 (cm, {源: "./examples/ext_c_plugin/c_math.so"})
导出变量 (["我的计数"])
我的计数 = 100
断言(cm.加法(3, 4) == 7, "加法")
cm.计数 = 42
断言(cm.计数 == 42, "指针槽读写")
我的计数 = 200          # VUS 侧改 → C 经宿主槽读到
打印(cm.我的计数)        # 200
```

---

## 9. 测试清单（必须全绿才验收）

**正向**
1. `导入外部` + 函数调用（int/float/str/list/dict 参数与返回、None 返回）；
2. 指针槽变量读写、回调 get/set 变量读写、只读变量；
3. 导出变量（宿主槽）双向同步：VUS 改→C 读；C 改→VUS 读（连续断言）；
4. 多参数 + 可变参数（max_args<0）；
5. 同一域重复 `导入外部`（幂等）。

**负向**
6. `.so` 不存在 → `"外部错误"` 含 dlerror；
7. 缺 `vus_rt_module_entry` → 错误；
8. init 返回非 0 → 错误；
9. 函数/变量不存在、参数个数不符、类型不匹配、只读写 → 各自预期消息；
10. 深度 >64 容器 → 错误。

**资源/共存**
11. 与 Python 域同表共存不互相干扰；
12. `vus_ext_shutdown_all` 释放 handle、不悬垂；重建/重导正常。

---

## 10. 验收标准

1. `tests/test_ext_c.vus` 全绿；负向用例消息逐条命中；
2. 指针槽 / 回调槽 / 宿主转换槽三种变量机制均验证；
3. `make`、示例 `.so` 构建与运行通过；
4. 与 py-impl 共用 `vus_rt_bridge.h` 无冲突（公共件裁决唯一类型源）；
5. 与总纲 §7.4、§8、§9 无冲突。