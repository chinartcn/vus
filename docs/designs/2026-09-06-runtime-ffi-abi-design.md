# 运行期外部桥（Runtime FFI Bridge）ABI 设计

> 版本：draft-1.0
> 日期：2026-09-06
> 状态：待评审
> 范围：VUS 程序 ↔ Python 模块 / C 运行时插件 之间的**函数互调**与**双向变量读写**完整 ABI。
> 原则：一次到位，不做残缺实现。所有能力（函数、变量、类型映射、错误、生命周期、版本）在本设计内闭环，作为后续实现的唯一依据。

## 文档结构（实现分工）

| 文档 | 定位 | 读者 |
|------|------|------|
| [2026-09-06-runtime-ffi-abi-design.md](2026-09-06-runtime-ffi-abi-design.md)（本文件） | 总纲：架构、VUS 侧语法、类型映射、运行时协议、版本、红线 | 所有实现者 |
| [2026-09-06-runtime-ffi-py-impl.md](2026-09-06-runtime-ffi-py-impl.md) | Python 域实现规格（模块约定、C-API 符号、桥桩、类型转换、生成代码、测试） | Python 侧实现 |
| [2026-09-06-runtime-ffi-c-impl.md](2026-09-06-runtime-ffi-c-impl.md) | C 域实现规格（结构体全量、dlopen 装载、宿主槽、类型转换、示例、测试） | C 侧实现 |

---

## 1. 目标与范围

### 1.1 目标能力

| # | 能力 | 说明 |
|---|------|------|
| 1 | Python 侧定义 VUS 可调用函数 | Python 模块/插件里写的函数可被 VUS 程序直接调用 |
| 2 | VUS → 外部读变量 | VUS 读取 Python/C 侧变量（含嵌套容器） |
| 3 | VUS → 外部写变量 | VUS 修改 Python/C 侧变量（含嵌套容器） |
| 4 | 外部 → VUS 读变量 | Python/C 侧读取 VUS 文件级全局变量 |
| 5 | 外部 → VUS 写变量 | Python/C 侧修改 VUS 文件级全局变量（VUS 立即可见） |
| 6 | C 侧与 Python 侧同构 | C 运行时插件提供与 Python ABI 一一对应的能力 |

### 1.2 不在此范围内

- 编译期/构建期插件（`vus_plugin`/`vus_lang`/`vusx`）不改造，保持现有构建期语义。
- `.vux` 的 `插件_运行`/`插件_运行JSON` 旧通道保留，与新桥并存，互不影响。
- 线程与并发：VUS 单线程协作式运行时，桥调用为**同步阻塞**，不引入锁（既有决策 #6 延用）。
- 序列化到磁盘/进程外通信：桥为**进程内**直接调用（Python 走 `VUS_USE_PY` 进程内嵌入；C 走 dlopen 进 vus_app 进程）。

---

## 2. 总体架构

```
┌────────────────────────────────────────────────────────────┐
│ VUS 程序（生成的 C，运行于 libvus_rt）                       │
│                                                          │
│  导入外部 (别名, {...})      → 绑定「外部域」到别名            │
│  别名.函数名(参数…)          → vus_ext_call(域, 函数, 参数)   │
│  别名.变量名               → vus_ext_get(域, 变量)          │
│  别名.变量名 = 值           → vus_ext_set(域, 变量, 值)      │
│  导出变量 (["x","y"])       → 全局导出槽表登记指针            │
│                                                            │
│                     │            │                         │
│            ┌────────▼───┐ ┌─────▼────────┐                │
│            │ C 类型转换层 │ │ Python 桥桩  │  （VUS_USE_PY） │
│            │ VusRTValue │ │  PyObject 互转│                │
│            └────────┬───┘ └─────┬────────┘                │
│                     │            │                         │
│   ┌─────────────────▼───────────────▼─────────────┐        │
│   │           运行时桥注册表 libvus_rt             │        │
│   │  ns[别名] → 域描述符 {类型, 表}                │        │
│   │  函数表: 名 → 可调用                          │        │
│   │  变量表: 名 → 槽（指针 或 get/set 回调）        │        │
│   │  全局导出槽表: 名 → 指向 VUS 全局变量的指针      │        │
│   └───────┬───────────────────────────┬──────────┘        │
│      ┌────▼─────┐               ┌─────▼──────┐             │
│      │ Python 域 │               │ C 插件域   │             │
│      │ (模块)   │               │ (.so)      │             │
│      └──────────┘               └────────────┘             │
└────────────────────────────────────────────────────────────┘
```

- **外部域（External Domain）**：一次 `导入外部` 产生的命名空间，内部维护两张表：**函数表**（名 → 可调用句柄）与**变量表**（名 → 变量槽）。
- **变量槽（Var Slot）**：二选一，模块声明时决定：
  - `指针槽`：外部侧直接持有 `void*`，指向实际的 VUS C 变量/外部内存。读写零拷贝、零转换，**导出变量路径用它**（VUS 侧零性能回归）。
  - `回调槽`：get/set 回调对。用于外部侧不暴露内存地址的场景（如模块内部属性），两侧各需一次值转换。
- **全局导出槽表（Global Export Slot Table）**：`导出变量` 语句把 VUS 文件级全局变量登记进桥的全局表（名 → 指向变量存储的指针）；**任何**已导入/后导入的域都通过 `别名.名` 访问这些槽（与域自己声明的变量表同名时，以域内声明优先）。

---

## 3. VUS 侧语法与语义

### 3.1 导入外部语句

```
导入外部 (别名, {源: <源>, 参数: {键: 值, ...}})
```

| 键 | 必填 | 类型 | 语义 |
|----|:---:|------|------|
| `别名` | 是 | 标识符 | 该外部域的绑定名；不得与 VUS 全局名/内建名/其它别名冲突；只读（不可再赋值） |
| `源` | 是 | 字符串 | Python：模块路径（如 `"myapp.mod"` 或 `"myapp.mod@插件的已安装名"`）；C：`.so` 绝对/相对路径 |
| `参数` | 否 | 字典 | 传给模块导出函数/插件 `init` 的初始化参数（键值均按 §6 类型映射转换） |

> 语法位置：`导入外部` 为顶层语句（文件级），在一个源文件中可出现多次（不同别名）；同一 `源` 可绑定多个别名（互为同一域的别名视图）。
> VUS 变量对外的共享不在这里声明，由独立语句 `导出变量`（§3.4）管理。

加载时序：**惰性**。首次真正使用（调用函数/读写变量）时才执行加载与 `init`；仅声明未使用不产生运行期开销。

### 3.2 调用外部函数

```
别名.函数名(实参…)
```

- 实参求值顺序、个数/类型检查语义同普通 VUS 调用；`别名.函数名` 不能在调用点外单独取函数值（首版限制：仅限直接调用形态 `别名.名(...)`）。
- 返回值类型映射见 §6；返回值参与 VUS 引用计数（视为新建对象，遵循 R6 收割语义）。
- 错误：函数不存在、参数个数不符、类型不可转换、调用抛异常 → 抛出 VUS 异常，类型为 `"外部错误"`，消息携带 `别名.函数名` 与域侧错误文本（§7.4）。

### 3.3 读写外部变量

```
值 = 别名.变量名          # 读
别名.变量名 = 值          # 写
别名.变量名[下标] = 值     # 对容器元素写（若变量槽支持元素级可变）
别名.变量名[下标]          # 读容器元素
```

- 读返回外部值的**拷贝**（按类型映射换算为 VUS 对象）；写为按映射换算后写入目标槽。
- 容器（list/dict）整体读写为**深拷贝语义**（避免两侧共享可变对象导致的别名混乱；性能敏感可后续加 `引用` 选项，默认拷贝）。
- 下标读写：仅当外部变量为 list/dict 时可用，行为与 VUS 内建列表/字典下标一致；数组越界/键不存在与 VUS 下标语义一致（返回空/抛错按内建既定规则）。

### 3.4 导出 VUS 变量（外部 → VUS）

```
导出变量 (["y", "x"])
```

顶层语句，参数为字符串列表（字面量列表；可为多行）。语义：

1. 这些名字必须是**已声明/已首次赋值的文件级全局变量**（编译期校验：未定义 → 报错"导出变量 y 未定义"）；
2. 编译期为每个名字生成 `vus_ext_export("y", &vus_y)` 注册进**全局导出槽表**（§7.1）；
3. 该声明对**任何**已导入/后导入的域生效——域通过 `别名.y` 访问即命中全局槽（域自身声明了同名变量表时，域内声明优先）；
4. 外部侧通过槽读写即直接作用于 `vus_y` 内存：外部写后 VUS 内 `y` 立即读到新值，VUS 内赋值后外部立即读到新值——**无缓存、零拷贝、天然双向同步**。

约束：
- 仅文件级全局变量可导出（函数内局部变量不可，因其生命周期随函数栈）。
- `导出变量` 可多次出现；重复导出同名变量为幂等（不报错）。
- 全局槽对 C 域的 `slot` 语义为：`vus_ext_export` 生成"宿主转换槽"（VUS 全局为 `VusString*`/`VusObject*` 时），C 侧读写槽触发与 VUS 之间的值转换（§6.2）；Python 域则把名字注册到模块视图，读写经桥桩 §7.2 同步。

### 3.5 Lexer/Parser 扩展

新增 token：`.`（成员访问点）。消歧规则：数字字面量中的 `.`（如 `3.14`）在词法层先行消化为数字的一部分；成员访问点**仅**出现在「标识符之后」且其后跟标识符（`别名.名`），由词法上下文（前一个 token 为标识符/右括号）判定。新增 AST 节点：`VUS_AST_MEMBER_CALL`（别名、函数名、实参表）、`VUS_AST_MEMBER_ACCESS`（别名、变量名）、`VUS_AST_MEMBER_ASSIGN`。

---

## 4. Python ABI（规范总览）

> 接口契约见下；**实现级规格（C-API 符号清单、桥桩代码、转换、生成桩、测试）见 [2026-09-06-runtime-ffi-py-impl.md](2026-09-06-runtime-ffi-py-impl.md)**。

### 4.1 被导入模块的形态

`源` 为 `"包.模块"`（`myapp.mod`）时，按标准 Python 模块查找（`sys.path` 先注入 `VUS_PLUGIN_DIR`/`VUS_HOME` 等既有点）。也可为 `"myapp.mod@插件名"`——先从已安装 `.vux` 插件目录里取该模块（插件即模块集合，新增约定：安装的 vux 包解压根目录被加入 `sys.path`，故 `"插件名.模块"` 直接可用）。

模块需满足（或继承）以下约定之一：

- **声明式**（推荐，最清晰）：

```python
# myapp/mod.py
__vus_export__ = {
    "函数": ["计算面积", "合并数据"],      # 导出给 VUS 调用的函数白名单
    "变量": ["计数", "状态", "配置"],      # 可读可写变量
    "只读":  ["版本"],                     # 只读变量
}

def 计算面积(w: float, h: float) -> float:
    return w * h

def 合并数据(a: list, b: list) -> list:
    return a + b
```

- **回退式**（未声明 `__vus_export__`）：导出 = 模块公开命名空间（名称不以 `_` 开头的可调用 → 函数表；名称不以 `_` 开头的变量 → 变量表，`__builtins__`/`__name__` 等 dunder 恒排除）。

以声明式为准时，白名单之外的符号 VUS **不可见**（调用/读写即抛"外部错误：符号未导出"）。

### 4.2 生命周期

| 阶段 | 触发 | 约定 |
|------|------|------|
| `import` | 首次使用时 | `importlib.import_module(模块路径)`；失败 → 外部错误 |
| `初始化` | import 后 | 若模块定义了 `def __vus_init__(api, 参数):`，以（API 对象, §4.3 的初始化参数字典副本）调用；返回非 0 或抛异常 → 导入失败，别名失效 |
| `调用` | 每次 `别名.函数(…)` | 直接调用模块内函数对象；异常 → 转 VUS 外部错误 |
| `清理` | 程序退出（RT 清理阶段） | 若模块定义 `def __vus_cleanup__():` 则调用；异常仅打印告警不中断 |

### 4.3 初始化参数传递

`参数` 字典按 §6 映射换算为 Python dict（普通 VUS 字典的深拷贝）传给 `__vus_init__(api, 参数)`。`api` 为桥 API 对象，首版提供：

```
api.version             # 桥 ABI 版本号（整数）
api.导出变量(name)       # 等价于导出变量 声明在运行期生效（配合动态脚本）
```

### 4.4 线程与阻塞约束

桥调用在 VUS 单线程协作式运行时内同步执行，期间协程不可切换（延用决策 #6）。Python 侧禁用 GIL 释放型长阻塞（不引入多线程）。

### 4.5 与既有 vux 通道的关系

- `插件_运行`（子进程）/`插件_运行JSON`（可进程内）旧通道**不变**，面向"命令字符串"插件；
- 新桥面向"函数/变量协议"模块；
- `.vux` 包可同时两者皆备：既有 `VuxPlugin` 子类（旧） + 模块级 `__vus_export__`（新）。安装路径统一，无冲突。

---

## 5. C ABI（规范总览）

> 接口契约见下；**实现级规格（结构体全量定义、dlopen 装载、宿主槽、类型转换、示例、测试）见 [2026-09-06-runtime-ffi-c-impl.md](2026-09-06-runtime-ffi-c-impl.md)**。

### 5.1 导出宏与模块入口

```c
/* vus_rt_bridge.h (新头文件，libvus_rt 侧) */
#if defined(_WIN32) || defined(_WIN64)
#  define VUS_RT_EXPORT __declspec(dllexport)
#else
#  define VUS_RT_EXPORT __attribute__((visibility("default")))
#endif

/* 每个运行时插件必须且仅导出一个该符号 */
VUS_RT_EXPORT void vus_rt_module_entry(VusRTModule **module);
```

### 5.2 结构体（首版即完整，字段含扩展保留位）

```c
/* ---- 值（跨界统一值类型） ---- */
typedef enum {
    VUS_RT_NIL = 0,      /* 空 */
    VUS_RT_INT,          /* i64 */
    VUS_RT_FLOAT,        /* f64 */
    VUS_RT_BOOL,         /* b */
    VUS_RT_STR,          /* s: UTF-8 NUL 结尾，桥层复制 */
    VUS_RT_LIST,         /* v.arr: VusRTValue 数组（深拷贝语义） */
    VUS_RT_DICT,         /* v.map: VusRTKv 数组（键 s + 值，深拷贝） */
} VusRTType;

typedef struct { const char *k; VusRTValue *v; } VusRTKv;

typedef struct VusRTValue {
    VusRTType t;
    union {
        long long i64;
        double    f64;
        int       b;
        const char *s;
        struct { VusRTValue *items; int n; } arr;
        struct { VusRTKv *pairs; int n; } map;
    } v;
} VusRTValue;

/* ---- 函数 ---- */
typedef struct VusRTFunc {
    const char *name;           /* VUS 侧调用名 */
    int         min_args;       /* 最少数（≤0 表示 0）*/
    int         max_args;       /* 最大数（<0 表示可变）*/
    /* call：参数 values[0..nargs-1]（按声明拷贝，插件可改不越界）；
     * out 由插件写入返回值；env->errs 写错误时返回非 0 */
    int  (*call)(VusRTValue *values, int nargs,
                 VusRTValue *out, VusRTEnv *env);
} VusRTFunc;

/* ---- 变量（二选一：slot 优先） ---- */
typedef struct VusRTVar {
    const char *name;
    VusRTType   type;           /* 声明类型；读写以此为准做严格校验 */
    int         readonly;       /* 1 = VUS 侧只读 */
    void       *slot;           /* 非 NULL：VUS 直接读写该内存（须 VusRTValue 布局）*/
    /* slot 为 NULL 时用回调 */
    int (*get)(const char *name, VusRTValue *out, VusRTEnv *env);
    int (*set)(const char *name, const VusRTValue *val, VusRTEnv *env);
} VusRTVar;

/* ---- 环境/错误 ---- */
typedef struct VusRTEnv {
    int   abi_version_current;  /* 桥 ABI 版本（1）*/
    char  errs[256];            /* 插件写错误消息；空串 = 无错误 */
} VusRTEnv;

/* ---- 模块描述符 ---- */
typedef struct VusRTModule {
    const char      *name;         /* 域类型名（调试/错误用） */
    const char      *version;      /* 插件自身版本 "x.y.z" */
    VusRTFunc       *funcs;        /* NUL name 结尾的表 */
    VusRTVar        *vars;         /* NUL name 结尾的表 */
    int  (*init)(const VusRTValue *init_params_dict, VusRTEnv *env);  /* 可 NULL */
    void (*cleanup)(void);         /* 可 NULL */
    /* 扩展保留：后续字段追加在尾部，不改变既有段偏移 */
} VusRTModule;
```

### 5.3 值约定（C 侧读写 VUS 变量）

- `VusRTValue` 布局即 C 插件与 VUS 共享的内存格式；`slot` 指针直接指向 VUS 侧 `VusRTValue*`（`vus_ext_export` 为导出的 VUS 全局变量生成同布局的宿主槽，VUS 全局变量为 `VusString*/VusObject*` 时 `export` 层自动构造包装槽：`写` 时按 §6 转换并走 `vus_var_set`，`读` 时按 §6 转换填充）。
- 所有字符串进出桥层按 UTF-8 复制，插件不得持有桥层分配的字符串所有权（`s` 指向的缓冲由桥层管理，插件调用期间有效，返回后失效）。

### 5.4 C 示例

```c
#include "vus_rt_bridge.h"
#include <string.h>

static int 计数 = 0;   /* 直接指针槽 */

static int add_impl(VusRTValue *v, int n, VusRTValue *out, VusRTEnv *e) {
    if (n != 2 || v[0].t != VUS_RT_INT || v[1].t != VUS_RT_INT) {
        snprintf(e->errs, sizeof(e->errs), "add 需要两个整数");
        return -1;
    }
    out->t = VUS_RT_INT; out->v.i64 = v[0].v.i64 + v[1].v.i64;
    return 0;
}

static VusRTValue g_count_slot; /* 桥层会按这个布局读写 */

static VusRTFunc g_funcs[] = {
    { "加法", 2, 2, add_impl }, { "", 0, 0, NULL }
};
static VusRTVar g_vars[] = {
    { "计数", VUS_RT_INT, 0, &g_count_slot, NULL, NULL },
    { "", 0, 0, NULL, NULL, NULL }
};

VUS_RT_EXPORT void vus_rt_module_entry(VusRTModule **m) {
    static VusRTModule mod = {
        .name = "c_math", .version = "1.0.0",
        .funcs = g_funcs, .vars = g_vars
    };
    *m = &mod;
}
```

VUS 侧：

```
导入外部 (m, {源: "./c_math.so"})
打印(m.加法(3, 4))   # 7
m.计数 = 10
打印(m.计数)         # 10
```

---

## 6. 类型映射（完整表）

跨界换算规则：数字严格按类型标签处理；字符串按 UTF-8 复制；容器**深拷贝**；不匹配一律抛外部错误（不截断、不隐式转换）。

### 6.1 VUS ↔ Python

| VUS | Python | 方向 | 规则 |
|-----|--------|------|------|
| 整数 | `int` | 双向 | 精确值（C `long long` ↔ `PyLong`） |
| 浮点 | `float` | 双向 | IEEE-754 double |
| 布尔 | `bool` | 双向 | — |
| 字符串 | `str` | 双向 | UTF-8；VUS 内部为 VusString，Python 双击为 str at C-API（`PyUnicode_AsUTF8`/`PyUnicode_FromString`） |
| 列表 | `list` | 双向 | 元素递归；深度限制 64（防循环引用爆炸） |
| 字典 | `dict` | 双向 | 键限字符串；值递归；深度同列表 |
| 空 | `None` | 双向 | — |
| 其它 | — | — | 不支持 → 外部错误 |

### 6.2 VUS ↔ C（VusRTValue）

| VUS | C | 规则 |
|-----|---|------|
| 整数 | `VUS_RT_INT` | `v.i64` |
| 浮点 | `VUS_RT_FLOAT` | `v.f64` |
| 布尔 | `VUS_RT_BOOL` | `v.b` |
| 字符串 | `VUS_RT_STR` | `v.s` UTF-8 |
| 列表 | `VUS_RT_LIST` | `v.arr.items/n` 递归 |
| 字典 | `VUS_RT_DICT` | `v.map.pairs` 键仍为字符串 |
| 空 | `VUS_RT_NIL` | — |

### 6.3 引用计数与所有权

- 外部函数返回值/外部变量读回 → 桥层构造新 VusString/VusObject 交给 VUS（出生 ref=1，走 R6 收割语义，无泄漏）。
- VUS → 外部参数：桥层在调用期间持有参数构造的临时转换值，调用结束释放，不改变 VUS 侧引用。
- 导出变量：VUS 全局变量内存归属 VUS 侧，外部通过指针槽读写不产生新引用。

### 6.4 容器嵌套限制与容器细节

- **深度上限 64**：跨域转换（任一方向）从顶层容器计 depth=1，递归层数 >64 → `"外部错误"「容器嵌套过深（>64）」`。
- **环形引用兜底**：Python/C 侧环形或自引容器（如 `a.append(a)`）在深度 64 处同样报"嵌套过深"，**不会**无限递归或崩——VUS 容器无环，深拷贝语义下环不可表达，逐层推进到上限即报错即正确行为。
- **大小**：不设元素数上限（内存由运行时管理，无固定缓冲）。
- **dict 键**：必须字符串；非字符串键 → `"外部错误"「字典键必须是字符串」`（Python 侧 int 键等将被拒绝，不做隐式转 str）。
- **tuple**：Python tuple 在「读回」方向视为前端只读列表（转为 VUS 列表）；「写入」方向只生成 list，不构造 tuple。

### 6.5 自定义类型（Python 类实例等）

- **默认**：非映射对象（非 None/数字/字符串/list/tuple/dict）跨域 → `"外部错误"「无法映射类型: <类型名>」`。VUS 值模型无对象实例概念，**不提供句柄语义**（不允许把外部实例持久存入 VUS 变量）。
- **可选精确转换协议**：若对象定义了 `__vus_tovalue__(self) -> <基础值>`，转换层调用它并把**返回值**按 §6.1 递归映射；返回值仍是不可映射类型 → 连环报错（消息含原类型名）。
- 反向（VUS → 外部）无自定义类型，无需协议。
- C 域只有基础类型（§5.2 枚举），无自定义类型问题。

### 6.6 空 / NULL 语义

- **对应关系**：Python `None` ⇄ `VUS 空`（运行时即 NULL 指针）⇄ C `VUS_RT_NIL`。**无需特殊包装**——NULL 指针就是 VUS 空，转换层直接透传。
- **严格区分空与空串**：`""`（非 NULL、长度 0 的 VusString）与 NULL 是两个值。转换必须保持：
  - None → NULL；`""` → 空字符串对象；
  - C `VUS_RT_NIL` → NULL；`VUS_RT_STR("")` → 空串；
  - VUS NULL → None；VUS 空串 → `""`。
- 容器内部的空元素：允许（list/dict 中可含 NULL/None/NIL 元素），语义与 VUS 内建一致。
- 空值参与 VUS 既有运算（`==`、`类型` 等）按既有内建规则，不加特殊分支。

---

## 7. 运行时协议与实现要点

### 7.1 桥注册表（libvus_rt）

- `ns` 表：`别名 → 域描述符`；`域描述符` = `{域类型(PY/C), 加载态, 函数表, 变量表}`。
- 操作：
  - `vus_ext_load(别名, 源, 参数, 导出表)` → 首次使用惰性加载；
  - `vus_ext_call(别名, 函数名, args[], n)`；
  - `vus_ext_get/set(别名, 变量名, …)`；
  - `vus_ext_export(别名, 名, 槽)`（导出变量语句生成）。
- 同一源多别名：共享域主体（加载/初始化只一次），各别名持有独立符号视图。

#### 7.1.1 符号解析与命名空间隔离（两域合并规则）

运行时表是「**别名 → 域 → 表**」的层级结构，**不做跨域平铺合并**——这是命名空间隔离的基础：

1. **别名隔离**：函数/变量一律经 `别名.名` 访问。Python 域与 C 域即使定义同名函数（如 `py.加法` 与 `c.加法`），分属不同别名，**无冲突，也不做前缀隔离**（前缀会破坏 `别名.函数()` 的简洁形态）。
2. **域内查重**：同一域内函数表/变量表重复名 → 加载时报错「域 <别名> 重复定义符号 <名>」，不静默吞掉（防插件表错误）。
3. **全局导出槽表**（`导出变量`）与域表同名：**域内声明优先**（确定性规则，无运行时警告）。
4. **全局导出槽同名**：重复 `导出变量` 同一名字 = 幂等（同一指针，不重复注册）。
5. **同源多别名**：共享域主体，别名视图各自独立但指向同一存储。
6. **冲突告警（可选增强）**：如需排查"域内变量意外遮蔽全局导出槽"的隐式使用，可在编译期对调用点给出 stderr 一行警告；**默认不开**，优先级规则即协议。

### 7.2 Python 通道（VUS_USE_PY）

复用既有进程内嵌入基建（`dlopen(libpython)` + dlsym C-API + `PyRun_String` helper 注入）。扩展部分：

- 新增/复用的 C-API 符号（对照 §6.1 双向映射）：
  - VUS→Py：`PyLong_FromLongLong`、`PyFloat_FromDouble`、`PyBool_FromLong`、`PyUnicode_FromString`、`PyList_New/Append`、`PyDict_New/SetItem`（已备 `PyObject_CallObject`、`Py_BuildValue`）；
  - Py→VUS：`PyLong_AsLongLong`、`PyFloat_AsDouble`、`PyObject_IsTrue`、`PyUnicode_AsUTF8`、`PyList_GetItem/Size`、`PyDict_Next/GetItem`（大部分已备）；
  - 符号解析追加 + 错误：`PyErr` 检查与 `PyErr_Clear` 已备。
- 加载：`_vus_bridge_load(源, 参数JSON)`（helper 内实现 `importlib.import_module` + `__vus_init__` + 采集 `__vus_export__`）；
- 调用/变量：`_vus_bridge_call(源, 函数, 参数JSON)` / `_vus_bridge_get(源, 名)` / `_vus_bridge_set(源, 名, 值JSON)`。
- **类型映射不走 JSON**：VUS→Py 由 C 侧直接构造 PyObject（递归）；Py→VUS 由 helper 返回后 C 侧直接解析。JSON 仅用于初始化`参数`（纯 dict 场景）与错误文本传输，避免复杂值失真。

### 7.3 C 通道

- `导入外部` 源以 `.so`/`.vulage`/绝对路径 结尾判定为 C 域：dlopen + dlsym(`vus_rt_module_entry`) → 注册函数/变量表 → `init(参数, env)`。
- 函数表 `name` 以空串结尾项为标记；变量表同。

### 7.4 错误语义（全部走 VUS 错误码链）

| 场景 | 错误类型 | 消息示例 |
|------|----------|----------|
| 源加载失败 | `"外部错误"` | `导入外部 失败: myapp.mod: No module named...` |
| 别名冲突/语法错 | 编译期报错 | `别名 m 与内建名冲突` |
| 函数不存在 | `"外部错误"` | `外部函数 m.加法 不存在` |
| 参数个数/类型不符 | `"外部错误"` | `m.合并数据 需要 2 个参数，实际 3` |
| 域侧抛异常 | `"外部错误"` | `m.计算面积: division by zero` |
| 类型不可映射 | `"外部错误"` | `值类型 字典 不能映射到 Python bool` |
| 只读变量写入 | `"外部错误"` | `m.版本 为只读` |

---

## 8. 版本与兼容

- 桥 ABI 版本号：`1`（`VUS_RT_BRIDGE_ABI = 1`，注入 `VusRTEnv.abi_version_current`）。
- 扩展规则：结构体字段**只增尾**、不删除不改语义；`VusRTModule` 末尾保留区承载新字段；旧插件在 `abi_version_current=1` 下按既有段偏移工作。
- Python 侧：`__vus_init__` API 对象按 `api.version` 分支；`__vus_export__` 键名语义锁定 `函数/变量/只读`。
- 文档同步：本设计定稿后，`PLUGIN_USAGE.md` 增补「运行期外部桥」章节；LSP 补全表（`src/lsp/vus_builtin.c`）登记 `导入外部` 与成员访问补全。

---

## 9. 限制与红线

- 性能红线：导出变量路径**零开销**（指针直达）；`别名.变量` 非导出访问 = 一次映射 + 深拷贝（容器随深度线性），不做 CSP 级缓存到首版。
- 深度限制：容器递归 ≤ 64 层；函数调用嵌套走 VUS 既有调用栈，不额外设限。
- 阻塞：桥调用同步阻塞，事件循环/协程等待期间不可调度。
- 平台：C 域需要 dlopen 支持（桌面/Android 构建机可用；Android APK 运行期默认禁用 C 域或按需打包）；Python 域依赖 `VUS_USE_PY` + 目标机 libpython（Android 不可用，与既有 vux 通道一致）。

---

## 10. 实施顺序（文档确认后按此推进）

> 两域并行独立实现，共享桥注册表 + 生成器 + lexer 改动为公共前置/协同点。

**公共前置（P）**
1. `include/vus/vus_rt_bridge.h`：值/函数/变量/slot/env/模块结构体（§5 全量，见 C 实现文档）与 `vus_ext_*` 接口；
2. libvus_rt 桥注册表（ns 表、全局导出槽表）与 `vus_ext_load/call/get/set/export` 框架（域类型分发的骨架）；
3. 生成器 + lexer：`导入外部`/`导出变量` 语句、`VUS_AST_MEMBER_*` 解析与代码生成（§3.5）。

**Python 域（本文档读者：Python 侧 AI）**：按 py-impl 文档实现（桥桩/转换/C-API 符号/示例/测试）。

**C 域（另一 AI）**：按 c-impl 文档实现（dlopen 装载/宿主槽/示例/测试）。

**联调（两个域完成后）**：错误/边界回归（`tests/`）+ `PLUGIN_USAGE.md` 章节 + LSP 补全表登记 `导入外部` 与成员访问补全。