# Runtime FFI Bridge — Python 域实现规格（py-impl）

> 版本：impl-1.0
> 日期：2026-09-06
> 依赖总纲：[2026-09-06-runtime-ffi-abi-design.md](2026-09-06-runtime-ffi-abi-design.md)
> 本文件实现责任：**Python 域全部能力**（函数调用、变量读写、导出变量同步、类型转换、生命周期、错误、示例、测试）。
> 公共前置（桥注册表骨架、`vus_ext_*` 接口、生成器/lexer 成员访问）由共享协同点提供，签名以本文 §6 为准。
> 前提：`VUS_USE_PY` 编译定义（复用既有 `vus_py_init` 嵌入通道）；无 `VUS_USE_PY` 时所有桥调用抛 `"外部错误"`「本构建未启用 Python 域」。

---

## 1. 交付物清单

| # | 交付物 | 位置 |
|---|--------|------|
| 1 | Python 域实现（C，编入 libvus_rt） | `rt/libvus_rt.c`（或拆 `rt/libvus_rt_pyext.c` 随 Makefile 并入） |
| 2 | 示例模块 | `examples/ext_py/samplemath.py` |
| 3 | 回归测试 | `tests/test_ext_py.vus` + `tests/test_ext_py_vars.vus` |
| 4 | 文档章节 | `docs/PLUGIN_USAGE.md` 运行期外部桥·Python 域 |

---

## 2. 模块约定（被导入侧契约）

### 2.1 模块形态

`源 = "包.模块"` 或 `"插件名.模块"`（vux 安装包根目录注入 `sys.path`）。加载顺序：

1. 把既有搜索路径加入 `sys.path`（不重复）：`VUS_PLUGIN_DIR` → `VUS_HOME` → `VUS_HOME/scripts`（复用 `vus_py_resolve_plugin_root` 的取值顺序）；
2. `importlib.import_module(源)`；
3. 若模块定义 `def __vus_init__(api, 参数)`：以 (api, 参数字典的 Python 深拷贝副本) 调用；返回非 0 或抛异常 → 导入失败；
4. 采集导出清单（§2.2）；
5. 登记模块对象（持有 new-ref）与导出清单到桥注册表。

### 2.2 导出清单 `__vus_export__`

```python
__vus_export__ = {
    "函数": ["计算面积", ...],   # 可调用白名单
    "变量": ["计数", "配置"],     # 可读可写
    "只读":  ["版本"],            # 只读
}
```

规则（缺省回退与校验都要完整实现）：

- **缺省**：无 `__vus_export__` → 回退「公开命名空间」：遍历模块 `__dict__`，跳过 `_` 开头与 dunder（`__builtins__`/`__name__`/`__file__`/`__package__` 等）及 `__vus_*`；`callable(v)` → 函数表；否则 → 变量表（可写）。
- **声明式**：键必须是上述三键之一（多余键 → 报错 `__vus_export__ 键不合法: <键>`）；`函数` 中不可调用项 → 报错；`变量`/`只读` 名在模块中不存在 → 报错；`函数` 与 `变量`/`只读` 同名 → 报错；**同一键内重复名 → 报错**（对应总纲 §7.1.1 域内查重）。
- **可见性**：白名单之外的符号 VUS 不可见（访问即抛 `"外部错误"`「符号 X 未导出」）；只读变量写入 → `"外部错误"`「X 为只读」。

### 2.3 api 对象（传给 `__vus_init__`）

Python 侧收到的是一个普通 dict（实现最简单且完整，后续需要再升级为类）：

```python
api = {"version": 1, "别名": <别名>, "源": <源>}
```

`__vus_init__(api, 参数)` 可读写 api dict；返回非 0 → 导失败；不返回（None）视为 0 成功。

### 2.4 模块级函数/变量 → VUS 访问形式

| Python 侧 | VUS 侧 |
|-----------|--------|
| `def 计算面积(w,h)` | `别名.计算面积(w, h)` |
| `计数 = 0`（导出为 变量） | `v = 别名.计数`；`别名.计数 = 5` |
| `版本 = "1.0"`（导出为 只读） | `v = 别名.版本`（写 → 错） |

---

## 3. 运行时接线（libvus_rt 侧，C）

### 3.1 新增 dlsym 符号（对照既有集合）

既有（`vus_py_init` 已备，见 libvus_rt.c §VUS_USE_PY 区）：

```
Py_Initialize, Py_Finalize, PyImport_ImportModule, PyObject_CallFunction,
PyObject_CallMethod, PySys_GetObject, Py_DecRef, PyErr_Print, PyErr_Clear,
PyFloat_AsDouble, PyList_Size, PyList_GetItem, PyDict_Next, PyDict_Size,
Py_BuildValue, PyObject_IsTrue, PySequence_Check, PyMapping_Check,
PyObject_Type, PyUnicode_AsUTF8, PyUnicode_FromString, PyObject_Str,
PyList_New, PyList_Append, PyDict_New, PyDict_SetItem, PyLong_AsLong,
PyRun_String, PyObject_CallObject, PyImport_AddModule, PyModule_GetDict,
PyDict_GetItemString, PyEval_GetBuiltins, PyObject_CallObject
```

追加符号（VSYM 同名机制）：

| 符号 | 函数指针名 | 用途 |
|------|-----------|------|
| `PyLong_FromLongLong` | `vus_py_PyLong_FromLongLongFn` | VUS 整数 → PyLong |
| `PyFloat_FromDouble` | `vus_py_PyFloat_FromDoubleFn` | VUS 浮点 → PyFloat |
| `PyBool_FromLong` | `vus_py_PyBool_FromLongFn` | VUS 布尔 → PyBool |
| `PyObject_GetAttrString` | `vus_py_PyObject_GetAttrStringFn` | 取模块属性（函数/变量/只读） |
| `PyObject_SetAttrString` | `vus_py_PyObject_SetAttrStringFn` | 写模块属性（含只读检查：失败即只读语义） |
| `PyErr_Fetch` | `vus_py_PyErr_FetchFn` | 取异常 (type,value,tb) 文本 |
| `Py_NONE_PTR` 全局变量 | `vus_py_Py_None` | 空 值（dlsym 取地址 `&Py_None`） |
| `PyBool_Check` | `vus_py_PyBool_CheckFn`* | bool 判别（先于 PyLong）——bool 是 int 子类 |
| `PyLong_Check` | `vus_py_PyLong_CheckFn`* | int 判别 |
| `PyFloat_Check` | `vus_py_PyFloat_CheckFn`* | float 判别 |
| `PyUnicode_Check` | `vus_py_PyUnicode_CheckFn` | str 判别 |
| `PyList_Check` | `vus_py_PyList_CheckFn` | list 判别 |
| `PyTuple_Check` / `PyTuple_Size` / `PyTuple_GetItem` | … | tuple 视为 list 读 |
| `PyDict_Check` | `vus_py_PyDict_CheckFn` | dict 判别 |
| `PyDict_GetItem` | `vus_py_PyDict_GetItemFn` | dict 取键（字符串键） |

> 判别宏均以函数指针 + 类型检查实现；若某符号缺失（旧版 libpython），该类型判别视为不可用并在用到时报 `"外部错误"` 而非崩溃。

### 3.2 Python 域描述（C 结构）

```c
typedef struct {
    const char *别名;          /* 桥注册表主键 */
    const char *源;            /* 模块路径 */
    void       *mod;           /* Python 模块对象（new-ref，持有至清理）*/
    int         inited;        /* 是否完成加载+初始化 */
    /* 导出清单（字符串数组，NUL 结尾项为哨兵） */
    const char **funcs;        /* callable 白名单（或公开名过滤结果） */
    const char **vars;         /* 可读写变量 */
    const char **ros;          /* 只读变量 */
} VusPyDomain;
```

### 3.3 加载流程 `vus_py_ext_load(别名, 源, 参数PyDict*)`

1. 已 inited → 直接成功（幂等）；
2. `sys.path` 补注入（§2.1）；
3. `mod = vus_py_PyImport_ImportModuleFn(源)`；NULL → `PyErr_Fetch` 提取文本 → 错误 `"导入外部 失败: <源>: <err>"`；
4. `__vus_init__`：`PyObject_GetAttrString(mod, "__vus_init__")` 非空且可调用 → 构造 `Py_BuildValue("(OO)", api_dict, 参数dict)` 调 `PyObject_CallObject`；异常 → `PyErr_Fetch` 文本 → 错误 `"<源> 初始化失败: <err>"`；返回对象 `PyLong_AsLong` 非 0 → 同上错误；
5. 采集导出清单（§2.2 规则；用 `PyObject_GetAttrString(mod, "__vus_export__")` 取声明，缺失则遍历 `__dict__`）——函数表同时把 `NameError`（符号不存在）转为错误；
6. 成功置 `inited=1`；失败路径 `Py_DECREF` 已持有引用，域不登记。

### 3.4 函数调用 `vus_py_ext_call(域, 函数名, 参数VusRTValue数组, n, 返回VusRTValue*, 错误缓冲)`

```
1. 域未加载 → 先按 3.3 加载（惰性）；
2. 校验 函数名 ∈ 函数表 → 否则 错误「外部函数 <别名>.<名> 不存在」；
3. fn = PyObject_GetAttrString(mod, 函数名)（borrow）；不可调用 → 错误「<名> 不可调用」；
4. args = PyTuple_New(n)；逐参数 vus_rt_to_py(§4.2) 构造 PyObject 并 PyTuple_SetItem；
5. ret = PyObject_CallObject(fn, args)；
   - NULL → PyErr_Fetch → 错误「<别名>.<名>: <type>: <value>」；
6. 返回：vus_py_to_rt(§4.1) 把 ret 转 VusRTValue；Py_DECREF(ret)；
   无返回值（None）→ VUS_RT_NIL。
```

### 3.5 变量读写 `vus_py_ext_get/set(域, 变量名, …)`

- **域内变量**（模块属性）：
  - `get`：校验 ∈ vars∪ros → `obj = PyObject_GetAttrString(mod, 名)` → `vus_py_to_rt`；
  - `set`：校验 ∈ vars（∈ros → 错误「X 为只读」）→ 值 `vus_rt_to_py` → `PyObject_SetAttrString(mod, 名, val)`；失败（e.g. frozen）→ 错误；
- **全局导出槽命中**（变量名不在域表、在全局导出槽表 `g_ext_export`）：转读 VUS 全局变量（§3.6）。

### 3.6 全局导出槽（VUS → Python 可见）

- 槽描述（桥注册表公共部分，本域使用）：

```c
typedef struct { const char *name; VusString **ptr; /* 指向 vus_x */ } VusExtSlot;
```

- `get`：取 `*ptr` 当前对象 → `vus_val_to_rt`（VUS 值 → VusRTValue，§4.3）→ `vus_rt_to_py`，一次转换进出；
- `set`：`vus_py_to_rt` → 在 VUS 侧用 `vus_var_set(ptr, 转换后对象)` 完成赋值（引用计数走 `vus_var_set`，天然防泄漏/防悬垂），随后外部读 `*ptr` 即新值。
- 深度/容器：全局导出槽允许 list/dict（深拷贝换算，见 §4）。

### 3.7 清理

RT 清理阶段：对每个 Python 域，若模块有 `__vus_cleanup__` 且可调用 → 无条件调用（异常仅打印告警）；`Py_DECREF(mod)`；释放清单字符串。`Py_Finalize` 复用既有通道收尾。

---

## 4. 类型转换（完整实现规格）

统一中介类型 = `VusRTValue`（定义在公共头，见 c-impl 文档 §2）。本域实现三组转换：

```
VusRTValue ──vus_rt_to_py──▶ PyObject*
VusRTValue ◀──vus_py_to_rt── PyObject*
VUS 值(对象指针) ──vus_val_to_rt──▶ VusRTValue   （供导出槽读；VUS 值 → 中介）
VUS 值 ◀──vus_rt_to_val── VusRTValue             （供导出槽写；中介 → vus_var_set 输入）
```

### 4.1 PyObject* → VusRTValue（`vus_py_to_rt`）

| PyObject* 判型（顺序） | → VusRTValue |
|---|---|
| `PyBool_Check` | `VUS_RT_BOOL`（真/假） |
| `PyLong_Check` | `VUS_RT_INT` `v.i64 = PyLong_AsLong`（溢出 → 错误「整数超出范围」） |
| `PyFloat_Check` | `VUS_RT_FLOAT` `v.f64 = PyFloat_AsDouble` |
| `PyUnicode_Check` | `VUS_RT_STR` `v.s = PyUnicode_AsUTF8`（桥层复制；NULL → 错误） |
| `PyList_Check`/`PyTuple_Check` | `VUS_RT_LIST`：`PyList_Size`/`PyTuple_Size` n；逐项递归（深度≤64） |
| `PyDict_Check` | `VUS_RT_DICT`：`PyDict_Next` 全量；键必须为 str（否则错误「字典键必须是字符串」）；值递归 |
| 等于 `Py_None` | `VUS_RT_NIL` |
| 其它（自定义类实例） | 见 §4.5 自定义类型协议；未实现协议 → 错误「无法映射类型: <类型名>」 |

> 顺序即细节：`PyBool_Check` 必须先于 `PyLong_Check`（bool 是 int 子类）；环形/自引用容器由深度 64 兜底报「容器嵌套过深」（总纲 §6.4）。

### 4.2 VusRTValue → PyObject*（`vus_rt_to_py`）

| VusRTValue | → PyObject* |
|---|---|
| `VUS_RT_NIL` | `Py_None`（借用，不增引用） |
| `VUS_RT_INT` | `PyLong_FromLongLong`（NULL → 转换失败） |
| `VUS_RT_FLOAT` | `PyFloat_FromDouble` |
| `VUS_RT_BOOL` | `PyBool_FromLong` |
| `VUS_RT_STR` | `PyUnicode_FromString` |
| `VUS_RT_LIST` | `PyList_New(n)` + 逐项递归 `PyList_Append` |
| `VUS_RT_DICT` | `PyDict_New` + `PyDict_SetItem(PyUnicode_FromString(k), 值)` 递归 |

分配失败/递归异常 → 抛 `"外部错误"`；所有新建 PyObject 用完 `Py_DECREF`（深拷贝语义，互不共享）。

### 4.3 VUS 值 ↔ VusRTValue（`vus_val_to_rt` / `vus_rt_to_val`）

VUS 值模型：`VusString*`（普通字符串）或 `VusObject*`（`TYPE_INT/FLOAT/BOOL/STR/LIST/DICT`，magic 校验 `vus_is_object`）。

| VUS 值 | → VusRTValue |
|---|---|
| 整数对象 | `VUS_RT_INT` |
| 浮点对象 | `VUS_RT_FLOAT` |
| 布尔对象 | `VUS_RT_BOOL` |
| `VusString*` / 字符串对象 | `VUS_RT_STR`（按 UTF-8 数据） |
| 列表对象 | `VUS_RT_LIST` 逐项递归 |
| 字典对象 | `VUS_RT_DICT` 逐键递归 |
| 空对象 | `VUS_RT_NIL` |

反向 `vus_rt_to_val`：按类型构造 `VusString*`/`VusObject*`（整数/浮点/布尔/字符串/列表/字典），空→`NULL` 语义（即 VUS 空），返回出生 ref=1 对象由桥调用点按既有收割语义接管。

### 4.4 公共规则（所有转换共用）

- 深度限制 64：递归入口计数，超限 → `"外部错误"「容器嵌套过深（>64）」`（环形/自引用容器同样由它兜底，总纲 §6.4）；
- 错误统一进 `VusRTEnv.errs`（256 字节缓冲，空串=无错），桥调用点转 VUS `"外部错误"`；
- 字符串总是复制（UTF-8），无所有权转移；
- 数值不做隐式转换（int 给 float 参数 → 错误）；
- dict 键必须字符串（int 键 → `"外部错误"「字典键必须是字符串」`）；
- tuple 读回为 VUS 列表；写入方向不生成 tuple。

### 4.5 自定义类型协议（Python 类实例）

- **默认**：非映射对象跨域 → `"外部错误"「无法映射类型: <类型名>」`（类型名 = `PyObject_Str(PyObject_Type(o))` 的短名，如 `SampleClass`）。
- **可选协议 `__vus_tovalue__`**：转换层遇到非映射对象时先查 `PyObject_GetAttrString(o, "__vus_tovalue__")`；存在且可调用 → 调用得返回值 → **递归**走 §4.1 判型（同一深度计数）映射；返回值仍不可映射 → 连环报错并含原类型名。协议实现示例：

```python
class 三维点:
    def __init__(self, x, y, z):
        self.x, self.y, self.z = x, y, z
    def __vus_tovalue__(self):
        return {"x": self.x, "y": self.y, "z": self.z}   # → VUS 字典
```

- **无句柄语义**：不允许把实例本身存入 VUS 变量/容器（只允许转换后的基础值）；VUS → Python 方向无自定义类型（VUS 只有基础值），不提供 `__vus_fromvalue__`。
- 只读/白名单校验：协议方法与 `__vus_export__` 同层（`__vus_*` 开头的 dunder 恒不参与导出回退枚举）。

### 4.6 空 / NULL 语义（Python 域）

- `None` → `VUS_RT_NIL` → VUS 收到 `空`（NULL 指针），**无包装**；
- 区分：`""` → `VUS_RT_STR`（空串，非空）；VUS NULL → None；VUS 空串 → `""`；
- 容器内空元素允许；`vus_py_to_rt` 的 `Py_None` 判断用借用引用 `Py_None`（dlsym `&Py_None`），不 `Py_DECREF`；
- Python 函数返回空（无 return 或 return None）→ `vus_py_to_rt` 产出 `VUS_RT_NIL`，VUS 侧得到 `空`。

---

## 5. 生命周期时序图（一次 `导入外部` 到退出）

```
VUS 首次使用 → vus_ext_load(别名, "myapp.mod", 参数)
   ├─ vus_py_init()（首次，dlopen libpython + dlsym 全部符号；失败 → 外部错误「未启用/无 libpython」）
   ├─ import_module("myapp.mod")
   ├─ __vus_init__(api, 参数)  （可无）
   ├─ 采集 __vus_export__
   └─ 注册域（持有 mod 引用）

…运行中：别名.函数 / 别名.变量 读写 / 导出槽读写（惰性加载已就绪）…

退出 → vus_ext_shutdown_all()
   ├─ 每域 __vus_cleanup__（可无，异常仅告警）
   ├─ Py_DECREF(mod)
   └─ Py_Finalize
```

---

## 6. 与桥注册表/生成器的接线签名（公共契约，自本文起生效）

```c
/* 桥注册表（公共部分，见总纲 §7.1） */
int   vus_ext_load(const char *ns, const char *src, const VusRTValue *params);
int   vus_ext_call(const char *ns, const char *fname,
                   const VusRTValue *args, int nargs, VusRTValue *out);
int   vus_ext_get (const char *ns, const char *vname, VusRTValue *out);
int   vus_ext_set (const char *ns, const char *vname, const VusRTValue *val);
int   vus_ext_export(const char *name, VusString **ptr);   /* 全局导出槽；幂等 */
void  vus_ext_shutdown_all(void);

/* Python 域内部实现接口（本文件 §3 对应以下函数） */
int vus_py_ext_load(const char *ns, const char *src, const VusRTValue *params);
int vus_py_ext_call(const char *ns, const char *fname,
                    const VusRTValue *args, int nargs, VusRTValue *out);
int vus_py_ext_get (const char *ns, const char *vname, VusRTValue *out);
int vus_py_ext_set (const char *ns, const char *vname, const VusRTValue *val);
```

生成器产出（示例）：

```c
/* 导出变量 (["计数"])  →  顶层 */
vus_ext_export("计数", &vus_计数);

/* 导入外部 (m, {源:"myapp.mod"})  →  首次使用时惰性加载：
 * 生成对 vus_ext_load 的调用（第一次运行时成立即加载，简单实现可逐调用判空；
 * 优先级：导入语句位置生成一次调用 + 运行时内部幂等使能“惰性”语义）。 */
vus_ext_load("m", "myapp.mod", NULL);   /* 参数经 vus_object_dict 构造后传 */

/* m.计算面积(3, 4)  → */
VusRTValue _xv[2] = {{.t=VUS_RT_INT,.v.i64=3},{.t=VUS_RT_INT,.v.i64=4}};
VusRTValue _xo; int _xe = vus_ext_call("m", "计算面积", _xv, 2, &_xo);
/* _xe!=0 → 抛 VUS 外部错误（走既有错误码链）; 成功后 _xo 按 §4.3 转 VUS 值 */
```

> 生成 C 的**最终形态由公共前置实现校准**，本文件仅在语义与伪代码层面将其钉死；`vus_ext_*` 签名即公共契约。

---

## 7. 示例（`examples/ext_py/`）

`samplemath.py`：

```python
# -*- coding: utf-8 -*-
"""运行期外部桥示例：Python 域模块。"""
__vus_export__ = {
    "函数": ["计算面积", "合并数据"],
    "变量": ["计数", "配置"],
    "只读": ["版本"],
}

版本 = "1.0.0"
计数 = 0
配置 = {"倍率": 2, "标签": "示例"}

def __vus_init__(api, 参数):
    # 参数: {"基准": 10}
    global 计数
    计数 = 参数.get("基准", 0)
    return 0

def 计算面积(宽, 高):
    global 计数
    计数 += 1
    return 宽 * 高

def 合并数据(a, b):
    return a + b
```

`test_ext_py.vus`（节选，完整见测试清单）：

```
导入外部 (m, {源: "ext_py.samplemath", 参数: {基准: 10}})
导出变量 (["计数2"])
计数2 = 100
断言(m.计算面积(3, 4) == 12, "面积")
m.配置.标签 = "新标签"            # 整体深拷贝后写回？→ 见下「深拷贝语义」注
断言(m.版本 == "1.0.0", "只读")
```

> 注：容器整体读写语义 = 深拷贝。`m.配置.标签 = …` 属于「元素级写（下标）」——列表/字典元素级写仅当变量槽支持元素级变更（本版：Python 变量**不支持**元素下标写，`m.配置.标签` 会编译为成员访问错误；请使用「读取后修改再整体写回」模式）。示例改：`配置 = m.配置  // 修改 // m.配置 = 配置`。总纲 §3.3 的下标读写仅适用于 `别名.变量[下标]` 形式，且仅当目标为 list/dict 时可用（Python 域首版支持 `[k]` 元素读写，元素写 = 读整体→改→写整体，等价语义；`[.attr]` 属性点号二次访问不支持）。

---

## 8. 测试清单（必须全绿才验收）

**正向**
1. `导入外部` → 函数调用（int/float/str 参数与返回值、bool、None 返回）；
2. list/dict 双向：传参、返回值、嵌套（<64 层）；
3. 变量读写：`别名.计数` 读/写，写回后同模块函数可读；
4. 只读变量写 → 报 `"外部错误"`；
5. 导出变量双向：VUS 改 `计数2` → Python 读到；Python 改 → VUS 读到（同函数内连续断言）；
6. 多别名同源共享 / 惰性：仅导入未使用不报错；
7. `__vus_init__` 参数生效、`__vus_cleanup__` 被调用（打印断言）。
8. 缺省回退导出：无 `__vus_export__` 模块公开名可调用。

**负向**
9. 模块不存在 → `"外部错误"` 消息含源路径；
10. 未导出函数 → `"外部错误"`；未导出变量 → `"外部错误"`；
11. 参数个数/类型不符 → 转换错误（int 传 float 位）；
12. Python 内部异常（除零）→ 错误含 `type: value`；
13. 容器深度 >64 → 错误；
14. 无 `VUS_USE_PY` 构建下调用 → `"外部错误"「未启用」`；
15. 重复 `导入外部` 同名别名 → 编译期报错。

**资源**
16. 反复调用无泄漏（配合 R6 收割路径；运行 valgrind/ASAN 分支检查引用计数稳定）。

---

## 9. 验收标准

1. `tests/test_ext_py.vus`、`tests/test_ext_py_vars.vus` 全绿；
2. 上述负向用例逐步命中预期错误信息；
3. 与 c-impl 域同表共存（桥注册表两个域各一条），互不干扰；
4. 构建：`make` 通过；`make VUS_USE_PY=1`（或既有启用开关）下示例与测试均通过；
5. 与总纲 §7.4、§8、§9 无冲突。