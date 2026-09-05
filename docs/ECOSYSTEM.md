> 文档版本：v1.0_apk（APK 功能时代）
> 最后更新时间：2026-09-04


# VUS 项目生态说明

> 版本：v3.0.20260904150204（正式版）  
> 最后更新：2026-09-04

---

## 一、概述

VUS 是一个面向 Linux、Android Termux、嵌入式 ARM 设备的中文友好**动态类型**多范式编译型编程语言（类型注解解析记录、不强制检查）。其生态围绕"编译到 C"这一核心设计展开，形成了从编译器内核到插件体系、从运行时库到构建工具链、从 GUI 双机制到体感音游的完整技术栈。

```
┌─────────────────────────────────────────────────────────┐
│                    用户层（VUS 源码）                      │
│  main.vus  │ 项目配置 vus.json  │ 测试用例  │ 插件         │
│  .vua 界面定义（Android 组件流）│ .vaz 扩展包            │
└──────────────────────┬──────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────┐
│                   编译器内核（src/）                       │
│  词法分析 → 语法分析 → AST → C 代码生成（含生成代码优化） │
│  API 层：C ABI / 插件系统 / 语言插件 / VUSX 插件 / APK     │
│  LSP（src/lsp/）│ 谱面生成 vus_chart │ vaz 展开 vus_vaz   │
└──────────────────────┬──────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────┐
│                   编译后端                               │
│  VUS → C 代码  →  GCC/Clang  →  原生可执行文件           │
│                       ↘
│                    Android NDK  →  APK 项目（VUA 壳）     │
└──────────────────────┬──────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────┐
│                   运行时支撑层                             │
│  libvus_rt（引用计数/字符串/列表/字典/闭包/错误处理）      │
│  线程（pthread）│ 协程（setjmp/longjmp 轻量实现）          │
│  EasyLogger（分级日志）│ yyjson（JSON）│ ANSI TUI          │
│  GuiLite（图形_* 画布流）│ vua.c（界面_* 组件流）          │
│  vus_xyz（体感音游：mpv + termux-sensor）                 │
│  libcurl（可选）│ libpython（可选，VUS_USE_PY）            │
└─────────────────────────────────────────────────────────┘
```

---

## 二、核心组件

### 2.1 编译器（`src/`）

编译器采用经典三段式架构，将 VUS 源码逐级降级为 C 代码：

| 组件 | 文件 | 职责 |
|------|------|------|
| 入口调度 | `main.c` | CLI 参数解析、子命令分发（run/build/test/init/update/lang/vux/vusx/vaz/chart/lsp） |
| 词法分析器 | `lexer.c` / `lexer.h` | UTF-8 中文标识符支持，90 余种 Token 类型，缩进敏感 INDENT/DEDENT，双语法体系 |
| Token 定义 | `token.c` / `token.h` | Token 类型枚举、字符串化、关键字查找（中英文别名合并到统一 Token） |
| 语法分析器 | `parser.c` / `parser.h` | 递归下降解析，覆盖函数/结构体/条件/循环/异常/导入/返回/全局声明等语句，8 级表达式优先级 |
| 抽象语法树 | `ast.c` / `ast.h` | 30 余种 AST 节点类型，创建/遍历/销毁 |
| 代码生成器 | `generator.c` / `generator.h` | 中文标识符 sanitize（`_XXXX` 编码）、引用计数插入、类型调度（`vus_add`）、GNU 语句表达式、泛型函数调用、结构体/线程/协程；**生成代码优化**（特征扫描按需产变量、`vus_var_set` 赋值热路径、整数字面量池化、列表/字典 helper 收敛、循环模板收敛、`omit_main` 库式编译） |
| 配置加载 | `config.c` / `config.h` | 项目配置 vus.json 加载与解析（内建简易 JSON 解析器），`VusConfig` 权威定义（含 `omit_main`） |
| C ABI | `vus_abi.c` | `compile_source` 编译流水线核心、编译/求值接口实现 |
| 插件系统 | `vus_plugin.c` | .vux 功能插件（dlopen 加载、注册/生命周期/查询） |
| 语言插件 | `vus_lang.c` / `vus_lang.h` | .vulage 语言插件（编译前预处理） |
| VUSX 插件 | `vus_vusx.c` / `vus_vusx.h` | .vusx 编译期插件（元数据解析、编译为 .o 并追加链接；`omit_main` 库式编译） |
| APK 打包 | `vus_apk.c` / `vus_apk.h` | VUS → Android APK 项目（JNI 桥接、NDK 检测、嵌入 VUA 壳） |
| VAZ 扩展包 | `vus_vaz.c` / `vus_vaz.h` | 展开 `.vaz` 控件模板 + 逻辑库（`vus vaz expand`） |
| 谱面生成 | `vus_chart.c` / `vus_chart.h` | 从音频生成体感音游谱面 JSON（`vus chart`，BPM/节拍估计） |
| 语言服务器 | `lsp/`（`lsp.c`、`vus_builtin.c` 等） | JSON-RPC 补全服务（可集成 ACode / GUI Designer） |

### 2.2 运行时库（`rt/`）

| 文件 | 职责 |
|------|------|
| `libvus_rt.h` | 运行时类型定义（VusString、VusList、VusDict、VusClosure、VusError、VusObject 等）与全部运行时函数原型 |
| `libvus_rt.c` | 运行时实现（引用计数、智能加法、列表/字典、闭包、错误链、线程/协程句柄注册表、分级日志、TUI/网络/文件/日期插件运行时函数；**性能优化**：字符串常量池 `vus_literal`、拼接免二重复制 `vus_string_concat`、`VusString` 头+负载单块 malloc、`vus_to_string` 整数值驻留缓存、`vus_var_set` 统一赋值） |
| `vus_coro.c` / `vus_coro.h` | 轻量级协程（基于 setjmp/longjmp + 平台特定汇编手工切换栈，独立 128KB 栈，可在 Android/Bionic/Termux 编译） |
| `elog_port.c` + `easylogger/` | EasyLogger 嵌入式日志库静态集成（分级日志） |
| `yyjson/` | 纯 C JSON 解析/生成库（默认内置，支撑 `JSON_*`） |
| `guilite_bridge.c/h` + `guilite_wrapper.cpp` | GuiLite 画布流桥接（`图形_*` 内建函数 → C++ 包装） |
| `guilite_platform.c` + `guilite_gles.c` | GuiLite 平台层（X11 / headless / 可选 GLES 加速） |
| `guiLite/` | GuiLite UI 框架头文件（MIT） |
| `gifdec/`、`nanosvg/` | GIF / SVG 解码（`图形_图片`、`图形_动画_*`）；PNG 走 libpng |
| `vua.c` / `vua.h` | VUA 组件流运行时（`.vua` 解析/校验/渲染树/事件派发/屏栈） |
| `vus_xyz.c` | 体感音游运行时（mpv 音频后端 + termux-sensor 传感器后端 + 单调时钟） |
| `vus_rt_shim.c` | 运行时 shim（编译期聚合辅助） |

运行时库提供的能力：

- **VusString** — 引用计数字符串类型（UTF-8，`data` 带 `\0` 结尾）
- **VusList** — 动态数组（创建、追加、获取、删除、设置、长度）
- **VusDict** — 字典（创建、设置、获取、删除、长度，链地址法哈希 + 自动扩容；v1.0-beta 暂无字典遍历接口）
- **VusObject** — 带类型标记的结构化容器（魔数 `'VOB!'`），承载列表/字典结构化数据
- **VusClosure** — 闭包支持（C 层回调）
- **VusError** — 错误链处理（不参与引用计数）
- **标准库辅助** — `vus_print`、`vus_input`、`vus_to_int`、`vus_to_string`、`vus_typeof` 等
- **线程/协程句柄注册表** — 各 64 个槽位（`VUS_MAX_HANDLES`）
- **分级日志** — EasyLogger 集成（`日志_调试/信息/警告/错误/级别`）
- **JSON** — yyjson 内嵌（`JSON_解析`/`JSON_生成`/`JSON_查询`、`对象文本`、`字典_键`）
- **插件运行时函数** — TUI（ANSI 转义码）、网络（libcurl，`VUS_HAVE_CURL`）、文件 I/O、日期时间、命令执行、文本/列表/字典、音频/传感器/时钟
- **GUI 画布流** — GuiLite 桥接（`图形_*`：绘制/控件/图片/动画/滚动/页面/主题/事件）
- **GUI 组件流** — VUA（`界面_*`：`.vua` 解析/校验/渲染树/事件派发/多屏导航）
- **体感音游** — `vus_xyz`（mpv 音频 + termux-sensor 传感器 + 单调时钟）

### 2.3 公共 API 头文件（`include/vus/`）

| 文件 | 内容 |
|------|------|
| `vus.h` | 核心类型 VusConfig、VusResult 及顶层编译流水线封装（`vus_compile_to_c`/`vus_compile_to_exe`/`vus_run`） |
| `vus_abi.h` | C ABI 接口：vus_compile_file()、vus_compile_string()、vus_compile_string_to_exe()、vus_eval()、ABI 版本函数 |
| `vus_plugin.h` | .vux 功能插件系统接口：VusPlugin 结构体、VusPluginAPI 编译器 API 表 |
| `vus_lang.h` | .vulage 语言插件系统接口：VusLangPlugin 结构体 |
| `vus_vusx.h` | .vusx VUS 插件系统接口：VusVusxPlugin 结构体 |

> 注意：`VusConfig` 的权威定义位于 `src/config.h`（`vus.h` 通过 `#include "../src/config.h"` 引入）。更完整的接口细节见《API_REFERENCE.md》。

---

## 三、四层插件体系

VUS 定义了四层插件体系，从源码级到编译器预处理级，逐层扩展语言能力：

```
┌──────────────────────────────────────────────────────┐
│  源码 .vus     普通 VUS 源文件，通过 import 机制复用    │
├──────────────────────────────────────────────────────┤
│  VUS 插件 .vusx  用 VUS 编写，编译时自动编译为 .o 并链接 │
│                  含 vusx.json 元数据描述               │
├──────────────────────────────────────────────────────┤
│  功能插件 .vux   用 Python 或 C 编写的运行时插件        │
│                  通过 dlopen 加载，可访问编译器 API     │
├──────────────────────────────────────────────────────┤
│  语言插件 .vulage 用 Python 或 C 编写的语法预处理插件    │
│                  在词法分析之前转换源码                 │
│                  示例：易语言风格插件（.功能 → 定义）   │
└──────────────────────────────────────────────────────┘
```

| 类型 | 扩展名 | 编写语言 | 加载时机 | 主要头文件 |
|------|--------|----------|----------|-----------|
| 源码 | `.vus` | VUS | 编译时 | （无） |
| VUS 插件 | `.vusx` | VUS | 编译时（自动编译 .o 并链接） | `vus_vusx.h` |
| 功能插件 | `.vux` | Python / C | 运行时 | `vus_plugin.h` |
| 语言插件 | `.vulage` | Python / C | 编译前预处理 | `vus_lang.h` |

### 3.1 源码层（.vus）

标准 VUS 源文件，支持函数风格（英文 + 中文别名）与易语言风格（经语言插件预处理）两种书写体系。核心语法始终是函数风格，易语言风格是「编译前预处理」层而非核心语法层。

### 3.2 VUS 插件（.vusx）

用 VUS 自身编写的插件，含 `vusx.json` 元数据描述文件（`名称`/`版本`/`入口`/`导出`/`依赖`），目录结构为 `my_plugin.vusx/{vusx.json, main.vus, 依赖.txt}`。编译时自动走 VUS → C → .o 流水线，把依赖的 `.o` 追加到 GCC 命令，链接到主程序。通过 `vus.json` 的 `vusx依赖` 数组声明。

**CLI 命令**：`vus vusx list|info <路径>|build <路径>`

### 3.3 功能插件（.vux）

用 Python 或 C 编写的运行时插件，通过 `vus_plugin_load()` 加载 `.so`/`.dll` 共享库。共享库必须且仅导出一个 `VUS_PLUGIN_EXPORT void vus_plugin_entry(VusPlugin **plugin)` 入口符号（编译器用 `dlsym("vus_plugin_entry")` 定位）。

**插件结构**：
```c
typedef struct VusPlugin {
    const char *name;                                  // 插件名称（必填）
    const char *version;                               // 版本号（必填）
    int  (*init)(VusPluginAPI *api);                   // 初始化（返回 0 成功）
    int  (*run)(VusPluginAPI *api, const char *input, char **output); // 执行回调
    void (*cleanup)(VusPluginAPI *api);                // 清理（可 NULL）
    const char *description;                           // 描述（可选）
    const char *author;                                // 作者（可选）
} VusPlugin;
```

**编译器 API 表**（运行时注入，`init` 前由编译器填充）：
```c
typedef struct VusPluginAPI {
    int version;                                    // 编译器 ABI 版本（兼容性检查）
    VusResult (*compile_file)(const char *, VusConfig *);
    VusResult (*compile_string)(const char *, VusConfig *);
    VusResult (*compile_string_to_exe)(const char *, VusConfig *);
    VusResult (*eval)(const char *, VusConfig *, char *);
    const char *(*compiler_version)(void);
} VusPluginAPI;
```

相关 API：`vus_register_plugin`、`vus_plugin_load`、`vus_plugin_init_all`、`vus_plugin_run_all`、`vus_plugin_cleanup_all`、`vus_plugin_find`、`vus_plugin_count`、`vus_plugin_list_all`、`vus_plugin_unload_all`（最大 `VUS_MAX_PLUGINS`=64）。Python 插件继承 `scripts/vux_plugin_entry.py` 中的 `VuxPlugin` 基类，实现 `init`/`run`/`cleanup` 三个生命周期方法。

**CLI 命令**：`vus vux install <源>|build [目录]|info <插件>|list|run <插件>`

### 3.4 语言插件（.vulage）

在词法分析之前对源码进行预处理，将不同风格的源代码转换为统一的标准 VUS 函数风格，实现语法风格扩展。例如将易语言风格的 `.功能` 转换为标准 VUS 的 `定义`。加载机制与 .vux 类似，`VUS_LANG_EXPORT void vus_lang_entry(VusLangPlugin **plugin)` 唯一导出符号，配置文件在 `vus.json` 中 `"语言插件": "易语言"`。

**插件结构**（权威签名见 `include/vus/vus_lang.h`）：
```c
typedef struct VusLangPlugin {
    const char *name;                                     // 插件名称（如"易语言"）
    const char *version;                                  // 版本号
    const char *ast_version;                              // 兼容 AST 版本号
    int  (*preprocess)(const char *input, char **output); // 预处理（统一签名）
    int  (*init)(void);                                   // 可选，可 NULL
    void (*cleanup)(void);                                // 可选，可 NULL
    const char *description;                              // 可选
    const char *author;                                   // 可选
} VusLangPlugin;
```

> ⚠️ **signature 更正**：`.vulage` 语言插件的 `preprocess` 回调**统一签名为** `int (*preprocess)(const char *input, char **output)`（返回 0 成功，输出由插件分配、调用方释放）。此前的草稿形式 `char *(*preprocess)(const char*, size_t)` 已作废，以头文件为准。

**CLI 命令**：`vus lang list|load <文件>|info <名称>`

---

## 四、插件运行时函数（v1.0-beta）

编译器内置的插件运行时函数由代码生成器按函数名映射（`generator.c` 的 `gen_expr_call`）。以下四组为 **v1.0-beta 已实现**的插件运行时函数。

### 4.1 终端 UI（TUI）— 基于 ANSI 转义码，无外部依赖

| 函数 | 运行时 | 说明 |
|------|--------|------|
| `tui_清屏()` | `vus_plugin_tui_clear` | 清空终端屏幕 |
| `tui_重置()` | `vus_plugin_tui_reset` | 重置终端属性 |
| `tui_设置颜色(前景色, 背景色)` | `vus_plugin_tui_set_color` | 设置终端颜色（ANSI 标准色） |
| `tui_定位(行, 列)` | `vus_plugin_tui_locate` | 移动光标到指定位置 |
| `tui_进度条(当前值, 总值, 宽度)` | `vus_plugin_tui_progress` | 显示进度条 |

### 4.2 网络 — 基于 libcurl（可选）

> **依赖标注**：依赖编译期宏 `VUS_HAVE_CURL` 及 `-lcurl`（系统需安装 `libcurl-dev`）。未定义 `VUS_HAVE_CURL` 时，以下函数为空实现（返回错误消息，不发送请求）。

| 函数 | 运行时 | 说明 |
|------|--------|------|
| `网络_GET(url)` | `vus_plugin_http_get` | HTTP GET 请求 |
| `网络_POST(url, 数据)` | `vus_plugin_http_post` | HTTP POST 请求 |
| `网络_请求(方式, 地址, 头JSON, 数据, 超时秒, 重试次数)` | `vus_plugin_http_request` | 通用请求：自定义请求头（token 认证）、超时、重试；APK 走 Java 桥，桌面回退 curl |
| `文件_上传(地址, 本地文件, 字段JSON, 头JSON)` | `vus_plugin_http_upload` | multipart 文件上传 + 附加字段/自定义头（APK 专属；桌面回退 `curl -F` 仅文件） |
| `网络_下载(url, 文件路径)` | `vus_plugin_http_download` | 下载文件到本地 |

### 4.3 文件操作 — 基于标准 C I/O，无外部依赖

| 函数 | 运行时 | 返回 |
|------|--------|------|
| `文件_读取(路径)` | `vus_plugin_file_read` | 文件内容字符串 |
| `文件_写入(路径, 内容)` | `vus_plugin_file_write` | 成功 `"true"`（覆盖写） |
| `文件_追加(路径, 内容)` | `vus_plugin_file_append` | 成功 `"true"` |
| `文件_存在(路径)` | `vus_plugin_file_exists` | `"true"` / `"false"` |
| `文件_删除(路径)` | `vus_plugin_file_delete` | 成功 `"true"` |
| `文件_列表(路径)` | `vus_plugin_file_list` | 每行一个文件名 |

### 4.4 日期时间 — 基于 `<time.h>`，无外部依赖

| 函数 | 运行时 | 说明 |
|------|--------|------|
| `日期_现在()` | `vus_plugin_date_now` | ISO 8601 格式当前时间 |
| `日期_时间戳()` | `vus_plugin_date_timestamp` | Unix 时间戳 |
| `日期_从时间戳(时间戳)` | `vus_plugin_date_from_timestamp` | 时间戳 → 日期字符串 |
| `日期_格式化(格式)` | `vus_plugin_date_format` | 按格式格式化当前时间 |
| `日期_解析(字符串, 格式)` | `vus_plugin_date_parse` | 解析日期字符串 |
| `日期_年/月/日/时/分/秒()` | `vus_plugin_date_year/month/day/hour/minute/second` | 获取当前时间各部分 |

### 4.5 插件 / JSON 内置函数

> **依赖标注**：`JSON_解析/生成/查询` 基于内嵌 **yyjson**（纯 C）**默认可用**，无需 Python；嵌入式（进程内）插件调用与 `typeof` 在编译期定义 `VUS_USE_PY`（`python3-config` 可用时由 Makefile 自动注入）时经 libpython 生效，未定义时 `typeof` 降级为恒返回 `"空"`、进程内调用回退子进程方案。

| 函数 | 运行时 | 说明 |
|------|--------|------|
| `插件_运行(插件, 命令)` | `vus_plugin_run_vux` | 子进程运行 .vux Python 插件 |
| `插件_运行JSON(插件, 命令)` | `vus_plugin_run_vux_json` / `vus_plugin_run_vux_inproc` | 子进程/进程内调用，返回 JSON 结构化结果 |
| `JSON_解析(字符串)` | `vus_json_parse` | JSON → 结构化 `VusObject*`（yyjson）|
| `JSON_生成(对象)` | `vus_json_generate` | `VusObject*` → JSON 字符串（yyjson）|
| `JSON_查询(json, 路径)` | `vus_json_query` | 按 `a.b[0]` 路径取字段（yyjson）|
| `对象文本(值)` | `vus_object_to_string` | 结构化对象安全转文本 |
| `字典_键(字典)` | `vus_dict_keys` | 返回字典键列表 |

### 4.6 分级日志内置函数（EasyLogger 集成）

首次调用任一 `日志_*` 惰性初始化，4 级输出返回 `"0"`（成功）/`"-1"`（失败）。

| 函数 | 运行时 | 说明 |
|------|--------|------|
| `日志_调试/信息/警告/错误(消息)` | `vus_log_debug/info/warn/error` | 各分级输出 |
| `日志_级别(级别)` | `vus_log_set_level` | 设置过滤级别（调试/信息/警告/错误） |

### 4.7 旧式标准库（设计文档 §10.1，已接线）

设计文档 §10.1 的核心库旧式名称**已接线**（映射到现代实现，可直接调用）：

- 字符串：`长度`、`拼接`、`分割`、`替换`、`取子串`（对应 `文本_长度/子串/分割`、`..` 拼接等）
- 数字：`取整`、`取随机数`
- 列表：`创建列表`、`添加元素`、`取元素`、`删除元素`、`列表长度`、`遍历列表`
- 字典：`创建字典`、`字典设值`、`字典取值`、`字典删除`、`字典长度`
- 文件：`读取文件`、`写入文件`、`追加文件`、`删除文件`、`文件是否存在`
- 时间/调试：`当前时间`、`调试输出`、`退出`、`断言`
- 说明：**异步 `等待`** 为协程保留语义（`等待(协程句柄)` await），旧文档中的 `等待(毫秒)` 睡眠请用 `睡眠`。

> 结论：**当前真正可用的内置函数** = 核心内置（打印/输入/转数字/转文本/类型）+ 日志 + tui_/网络_/文件_/日期_/文本_/列表_/字典_ 组 + 插件/JSON（JSON 基于 yyjson 默认可用）+ **GUI**（`图形_*` GuiLite 画布流、`界面_*` VUA 组件流）+ 旧式标准库名称。

### 4.8 GUI 内置函数（已实现，两套机制）

| 机制 | 平台 | 函数族 | 文档 |
|------|------|--------|------|
| **画布交互流** | Termux / Linux X11 | `图形_*`（绘制/控件/图片/动画/滚动/页面/事件，GuiLite）| [API_REFERENCE.md 第 1 章](API_REFERENCE.md) |
| **组件解析流** | Android APK | `界面_*`（`.vua` 多屏导航 + 事件绑定，Java View 渲染）| [VUA_REFERENCE.md](VUA_REFERENCE.md) / [VUA_RENDER_TREE.md](VUA_RENDER_TREE.md) |

### 4.9 Meilisearch 搜索插件（.vux 功能插件）

VUS 插件系统的功能插件，用 Python 封装 Meilisearch 全文搜索引擎，提供中文子命令接口（服务健康/索引管理/文档 CRUD/全文搜索/同义词/设置管理）。

**安装/运行**：
```bash
vus vux install meilisearch        # 从仓库安装
python3 scripts/vux_plugin_manager.py run meilisearch "状态"
```

**中文子命令**：

| 命令 | 说明 |
|------|------|
| `状态` / `健康` / `统计` | 服务健康与统计信息 |
| `索引 列表` | 列出所有索引 |
| `索引 创建 <名> [--主键 xxx]` | 创建索引 |
| `索引 删除 <名>` | 删除索引 |
| `索引 设置 <名> [--筛选属性 a,b] [--分面数量 n]` | 读取/更新索引设置 |
| `文档 添加 <索引> --文件 path \| --json '[...]'` | 添加文档 |
| `文档 更新 <索引> ...` | 更新文档 |
| `文档 删除 <索引> <id>` | 删除文档 |
| `文档 获取 <索引> <id>` | 获取文档 |
| `搜索 <关键词> --索引 <名> [--筛选] [--排序] [--高亮] [--分面] [--限制 n]` | 企业级全文搜索 |
| `同义词 设置 <索引> --词 '组1;组2'` | 设置同义词组 |
| `同义词 获取 <索引>` | 获取同义词组 |
| `设置 获取\|更新 <索引> [--筛选属性 a,b]` | 设置管理 |

**连接配置**（优先级递减）：
1. 环境变量 `VUS_MEILI_HOST` / `VUS_MEILI_API_KEY`
2. 配置文件 `~/.vus/plugins/meilisearch/config.json`（键：`host` / `api_key`）
3. 默认 `http://localhost:7700`

源码位于 `plugins/func/meilisearch/`（含 `tests/`），并提供本地部署扩展插件 `plugins/func/meilisearch_localdeployment/` 及预打包产物 `Meilisearch搜索-1.0.0.vux`、`Meilisearch本地部署拓展-1.0.0.vux`（见根目录）。示例见 `examples/meili_search.vus`、`examples/meili_localdeploy_demo.vus`。

---

## 五、构建系统

### 5.1 Makefile（`Makefile`）

主构建系统（GNU Make），主要目标：

| 目标 | 说明 |
|------|------|
| `make` / `make all` | 完整编译（`vus` 编译器 + `libvus_rt.a` 运行时库） |
| `make vus` | 链接编译器二进制（`-lm -ldl`） |
| `make libvus_rt.a` | 仅编译运行时库（含 `vus_coro.o`、EasyLogger 对象） |
| `make test` / `make run-tests` | 运行测试（`./vus test` / `bash tests/run_tests.sh`） |
| `make run` / `build-c` / `build-exe` | 便捷运行 / 仅生成 C / 生成可执行文件 |
| `make install` / `uninstall` | 安装到 `/usr/local/bin/vus` + 共享脚本/头文件 |
| `make clean` | 清理构建产物 |
| `make format` | 用 `clang-format` 格式化 `src/`、`rt/` 下的 C/H |

**编译宏**：CFLAGS 为 `-Wall -Wextra -g -O2 -std=c11`（GCC 语句表达式等扩展以 GNU C11 级别可用）；`python3-config` 可用时定义 `-DVUS_USE_PY` 并注入 `-DVUS_PY_SONAME`（启用嵌入式 Python），不可用则仅编译子进程方案。协程模块单独编译以避免 inline asm 与 C11 冲突。

### 5.2 预编译包构建（`scripts/build_release.sh`）

用于生成分架构的预编译发布包：

- 自动检测本地架构或交叉编译指定架构
- 编译 VUS 编译器 + 运行时库
- 打包为 `.tar.gz` 压缩包
- 生成 MD5 和 SHA256 校验文件

### 5.3 安装脚本（`install.sh`）

一键安装脚本，支持：

- 自动检测系统架构（x86_64 / aarch64 / armv7l）
- 尝试下载预编译包并校验 MD5
- 校验失败自动降级到源码编译
- 自动配置 PATH 环境变量

---

## 六、APK 打包（`src/vus_apk.c`）

VUS 支持将程序编译为 Android APK 项目：

```
vus build --apk main.vus [--ndk-path <路径>] [--app-name <名称>] [--output <目录>]
```

流程要点：

- **NDK 检测** — 依序探测显式路径 → `ANDROID_NDK_HOME` → `ANDROID_HOME` 下常见 NDK 版本目录 → 默认路径；未找到时仅生成项目结构并提示手动构建。
- **包名** — 取反向域名 `com.vus.<应用名>` 并小写化。
- **`main()` 替换** — 自动替换为 `vus_main()` 避免冲突。
- **运行时单一真源（P5）** — `rt/` 是唯一真源，APK 构建时由 `vus_apk.c` 现场拷贝 `libvus_rt.c`/`vua.c`/`yyjson` 到生成工程的 jni 目录，仓库不再留存第二份拷贝（历史两副本分叉过）。
- **JNI 桥自动生成（P3）** — `vus build --apk` 调用 `scripts/gen_jni_bridge.py`，从 Java `native` 声明生成 `jni_bridge.c`：符号名随实际包名自动对齐（包名怎么变都不再需要手改），Java 声明与 C 实现一一对应、缺失即构建报错；`python3` 缺失时回退模板替换。
- **完整构建链（P4）** — 一次产完的多 ABI 完整链见参考工程 `examples/vua-android/scripts/build_apk.sh`（NDK clang → javac → d8 → aapt → zip → zipalign → apksigner）：
  - **JDK 探测与降级**：优先 JDK8；JDK9+ 自动加 `--release 8`（d8/build-tools 31 只认老 class 文件，实测 javac 25 编出的 class 69 会直接报 `Unsupported class file major version`）。
  - **多 ABI 一次产出**：`--abi all`（默认）同时编 `arm64-v8a` + `armeabi-v7a` + `x86_64`，全部打进同一个 APK 的 `lib/<abi>/`。
  - **JNI 符号核对**：.so 构建后自动 `nm -D | grep Java_*` 与 Java 声明逐一比对，缺符号即中止（消灭"包名/方法名改了就运行期崩溃"）。
- **构建脚本** — 生成 `Android.mk`（含 rt 源文件与 `-DVUS_HAVE_CURL`）、`Application.mk`（`APP_ABI=arm64-v8a armeabi-v7a x86_64`）、`AndroidManifest.xml`。
- **自动构建** — 有 NDK 时调用 `ndk-build` 实际构建，否则提示手动构建。

> **参考工程**：`examples/vua-android/`（VUA 组件流最小 APK）自带 `scripts/build_apk.sh`，可直接 `scripts/build_apk.sh --abi all` 复现完整 APK 构建，是自建工程的蓝本。

---

## 七、测试体系

### 7.1 测试用例（`tests/`）

覆盖以下功能模块（`.vus` 用例）：

| 测试文件（节选） | 覆盖功能 |
|----------|---------|
| test_hello.vus | 基本输出 |
| test_variables.vus | 变量声明与赋值 |
| test_arithmetic.vus | 算术运算 |
| test_control.vus / test_elif.vus | 条件分支 / 多路选择 |
| test_functions.vus | 函数定义与调用 |
| test_comparison.vus / test_logical.vus / test_not.vus | 比较 / 逻辑 / 取反 |
| test_string.vus / test_concat.vus | 字符串拼接与比较 |
| test_while_count.vus / test_nested_control.vus | 当循环 / 嵌套循环 |
| test_factorial.vus / test_fibonacci.vus / test_recursion.vus | 递归 |
| test_bitwise.vus / test_modulo.vus / test_negative.vus | 位运算 / 取余 / 负数 |
| test_literal.vus / test_cast.vus / test_type_annot.vus | 字面量 / 类型转换 / 类型注解 |
| test_break_continue.vus / test_global.vus / test_local_vars.vus | 跳转 / 全局变量 / 局部变量 |
| test_exception.vus / test_error.vus | 异常处理 |
| test_comprehensive.vus / test_demo.vus | 综合功能测试 |
| test_generic.vus / test_generic_call.vus | 泛型函数调用 |
| test_struct_basic.vus / test_struct_chain.vus / test_subscript.vus | 结构体 / 链式访问 / 下标 |
| test_thread_coro.vus | 线程与协程 |
| test_plugin_run_json.vus / test_plugin_inproc.c | 插件运行 / JSON |
| test_plugins.vus | 插件运行时函数 |
| test_logger.vus | 分级日志 |
| test_import.vus | 导入机制 |
| test_gui.vus / test_gui_btn.vus / test_gui_ctrl(adv).vus | GUI 画布流（控件/事件，headless 可测） |
| test_gui_shape_adv.vus / test_gui_png.vus / test_gui_font.vus / test_gui_media.vus | GUI 绘制原语 / 图片 / 字体 / 媒体动画 |
| test_gui_pages.vus / test_gui_pages_adv.vus / test_pages_ext.vus | GUI 多页面导航 |
| test_gui_md_line_scroll.vus | Markdown 渲染 / 滚动容器 |
| test_xyz_basic.vus / test_vus_chart.vus | 体感音游运行时 / 谱面生成 |
| test_vus_abi.vus / test_vus_lang.vus / test_vus_plugin.vus | C ABI / 语言插件 / 功能插件 |
| vua_smoke.c | VUA 渲染树 / 严格校验 / 按 ID 触发（`gcc -I rt rt/vua.c rt/yyjson/yyjson.c tests/vua_smoke.c`） |

错误路径用例位于 `tests/error_tests/`（缺冒号、未闭合字符串/列表、泛型未闭合、结构体无名、括号不匹配等），运行脚本 `tests/run_error_tests.sh`。

### 7.2 运行方式

```bash
./vus run tests/test_hello.vus       # 单个测试
bash tests/run_tests.sh              # 批量运行
make test                            # 调用 vus test
```

---

## 八、CLI 命令参考

本节依据 `src/main.c` 真实实现整理。

### 8.1 编译与运行

| 命令 | 说明 |
|------|------|
| `vus run <文件>` | 编译并运行 VUS 程序 |
| `vus run --debug <文件>` | 调试模式运行（开启 `vus_debug_enabled`，含栈追踪和调试输出） |
| `vus build --c-only <文件>` | 仅编译为 C 代码 |
| `vus build --exe <文件>` | 编译为可执行文件 |
| `vus build --apk <文件> [--ndk-path <路径>] [--app-name <名称>] [--output <目录>]` | 编译为 Android APK 项目 |

### 8.2 项目管理

| 命令 | 说明 |
|------|------|
| `vus init [--force]` | 交互式项目初始化（`--force` 强制重建配置） |
| `vus test` | 自动执行测试用例 |
| `vus update` | 自动更新编译器（检测 Git 安装则 `git pull` + `make`，否则下载预编译包） |

### 8.3 插件管理

| 命令 | 说明 |
|------|------|
| `vus lang list` | 列出已安装语言插件（.vulage） |
| `vus lang load <文件>` | 加载语言插件 |
| `vus lang info <名称>` | 查看语言插件信息 |
| `vus vux install <源>` | 安装 .vux 功能插件 |
| `vus vux build [目录]` | 打包 .vux 插件 |
| `vus vux info <插件>` | 查看插件信息 |
| `vus vux list` | 列出已安装插件 |
| `vus vux run <插件>` | 运行插件 |
| `vus vusx list` | 列出项目中配置的 vusx 依赖 |
| `vus vusx info <路径>` | 查看 vusx 插件信息 |
| `vus vusx build <路径>` | 编译 vusx 插件（库式，`omit_main`） |
| `vus vaz expand <页面目录> -v <包.vaz|目录>` | 展开 `.vaz` 扩展包（控件模板 + 逻辑库） |

### 8.4 其他

| 命令 | 说明 |
|------|------|
| `vus chart <音频> [-o 输出文件]` | 从音频生成体感音游谱面 JSON（chart 格式，估算 BPM/节拍） |
| `vus lsp` | 启动语言服务器（JSON-RPC 补全，可集成 ACode / GUI Designer） |
| `vus --help` / `vus -h` | 显示帮助信息 |
| `vus --version` / `vus -v` | 显示版本信息（含 ABI 版本） |

---

## 九、插件管理脚本（`scripts/`）

| 文件 | 职责 |
|------|------|
| `vux_plugin_manager.py` | .vux 插件的安装、构建、列表、信息查询、运行管理 |
| `vux_plugin_entry.py` | 插件基类 `VuxPlugin` 定义，供 Python 编写的插件继承 |
| `build_release.sh` | 分架构预编译发布包构建 |
| `index_vus_docs.py` | 文档索引生成（配合 docs/search.html 提供文档检索） |

---

## 十、目录结构

```
vus/
├── src/                    # 编译器源码
│   ├── main.c              # CLI 入口（命令分发：run/build/test/init/update/lang/vux/vusx/vaz/chart/lsp）
│   ├── lexer.c/h           # 词法分析器
│   ├── parser.c/h          # 语法分析器
│   ├── token.c/h           # Token 类型（90 余种）
│   ├── ast.c/h             # 抽象语法树（30 余种节点）
│   ├── generator.c/h       # 代码生成器（含生成代码优化 + GUI/界面_* 映射）
│   ├── config.c/h          # 配置加载 + VusConfig 定义（含 omit_main）
│   ├── vus_abi.c           # C ABI 实现 + 编译流水线核心
│   ├── vus_plugin.c        # .vux 插件系统
│   ├── vus_lang.c/h        # .vulage 语言插件系统
│   ├── vus_vusx.c/h        # .vusx 插件系统（库式编译）
│   ├── vus_apk.c/h         # APK 打包（嵌入 VUA 壳）
│   ├── vus_vaz.c/h         # .vaz 扩展包展开
│   ├── vus_chart.c/h       # 体感音游谱面生成
│   └── lsp/                # 语言服务器（lsp.c、vus_builtin.c/h 内置函数元数据表）
├── rt/                     # 运行时库
│   ├── libvus_rt.c/h       # 运行时实现 + 类型定义（含性能优化路径）
│   ├── vus_coro.c/h        # 协程实现
│   ├── vus_rt_shim.c       # 运行时 shim
│   ├── elog_port.c         # EasyLogger 移植层
│   ├── easylogger/         # EasyLogger 日志库（inc/ + src/）
│   ├── yyjson/             # JSON 解析/生成库（内置）
│   ├── guilite/            # GuiLite UI 框架头文件
│   ├── guilite_bridge.c/h  # 图形_* → GuiLite C 桥接
│   ├── guilite_wrapper.cpp # GuiLite C++ 包装
│   ├── guilite_platform.c  # X11 / headless 平台层
│   ├── guilite_gles.c/h    # GLES 加速平台层（可选）
│   ├── gifdec/  + nanosvg/ # GIF / SVG 解码
│   ├── vua.c/h             # VUA 组件流运行时（渲染树/事件/屏栈）
│   └── vus_xyz.c           # 体感音游运行时（mpv + termux-sensor）
├── include/vus/            # 公共 API 头文件
│   ├── vus.h               # 核心类型与顶层编译封装
│   ├── vus_abi.h           # C ABI 接口
│   ├── vus_plugin.h        # .vux 功能插件接口
│   ├── vus_lang.h          # .vulage 语言插件接口
│   └── vus_vusx.h          # .vusx VUS 插件接口
├── plugins/                # 插件目录
│   ├── lang/易语言/        # 语言插件（.vulage，易语言风格）
│   ├── 易语言/             # 语言插件打包形态（vux.json + __init__.py）
│   └── func/               # 功能插件（.vux）
│       ├── 示例/           # 插件开发示例
│       ├── meilisearch/    # Meilisearch 搜索插件（含 tests/）
│       └── meilisearch_localdeployment/  # Meilisearch 本地部署扩展
├── tests/                  # 测试用例（.vus + error_tests/ + vua_smoke.c + 运行脚本）
├── testdata/               # 测试数据（.vua 界面样本、vua_controls.json、vaz 样本等）
├── .bench/                 # 性能基准（big.vus 大文件、hot.vus 热循环）
├── scripts/                # 工具脚本
│   ├── build_release.sh    # 预编译包构建
│   ├── build_lsp_android.sh
│   ├── vux_plugin_manager.py
│   ├── vux_plugin_entry.py
│   └── index_vus_docs.py
├── examples/               # 示例程序（GUI 示例 gui_*.vus、体感音游 xyz_game.vus/solace_game.vus、vua-android/ APK 壳工程、gui-designer/ 可视化设计器、acode-vus-lsp-plugin/、plugins/）
├── docs/                   # 文档
│   ├── LANGUAGE_REFERENCE.md    # 语言参考手册（已实现 vs 未实现权威标注）
│   ├── API_REFERENCE.md         # 内置函数参考（图形_*/界面_* 全量）
│   ├── VUA_REFERENCE.md         # VUA 界面定义规范（Android 组件流）
│   ├── VUA_RENDER_TREE.md       # VUA 渲染树格式（native → Java）
│   ├── ARCHITECTURE.md          # 架构与模块实现
│   ├── PROJECT_BRIEFING.md      # 项目简介
│   ├── STATUS.md                # 状态报告（功能清单、测试状态、已知 Bug）
│   ├── PERFORMANCE.md            # 性能优化专项记录（生成代码/编译/运行时）
│   ├── PLUGIN_USAGE.md           # 插件系统使用指南（vux/vusx/.vulage/vaz）
│   ├── TUTORIAL.md              # 从零开始教程
│   ├── COMPILER_GUIDELINES.md   # 编译器开发指南
│   ├── ECOSYSTEM.md             # 本文件（生态全景）
│   ├── designs/                 # 设计文档
│   ├── plans/                   # 实施计划
│   ├── plugins/                 # 插件说明
│   └── search.html              # 文档检索入口
├── Makefile                # 构建系统
├── install.sh              # 一键安装脚本
├── 设计文档.md             # 语言设计文档
└── README.md               # 项目简介
```

---

## 十一、技术依赖关系

```
VUS 编译器（C11）
  ├── 标准 C 库（glibc / musl）— 必需
  ├── POSIX 线程（pthread）— 必需（线程支持）
  ├── EasyLogger — 运行时内置（rt/easylogger/），分级日志
  ├── yyjson — 运行时内置（rt/yyjson/），JSON（默认可用）
  ├── GuiLite — 运行时内置（rt/guilite/ + 桥接/平台层），图形_* 画布流
  ├── libpng + FreeType — 图形_背景图/图形_图片(.png)/图形_字体_加载（GUI 链接期）
  ├── mpv — 音频后端（vus_xyz，运行时 spawn；不可用时音频安全返回 0）
  ├── termux-sensor — 传感器后端（Android Termux，体感音游）
  ├── libcurl（可选，VUS_HAVE_CURL）— 网络插件运行时函数
  ├── libpython（可选，VUS_USE_PY）— 进程内嵌入 Python（插件进程内调用、typeof）
  ├── GCC / Clang — 编译后端（将生成的 C 代码编译为可执行文件）
  └── Android NDK（可选）— APK 打包（含 VUA 壳）
```

**无外部依赖的组件**：词法分析器、语法分析器、AST、代码生成器、配置加载、TUI 函数、文件操作函数、日期时间函数、运行时核心（VusString/VusList/VusDict/VusClosure/VusError/VusObject）、协程（setjmp/longjmp + 汇编，不依赖 ucontext）、EasyLogger、yyjson（JSON）、VUA（`.vua` 解析/渲染树）。

**条件编译宏**：

| 宏 | 生效范围 |
|----|----------|
| `VUS_HAVE_CURL` | 网络插件运行时函数（需 `-lcurl`） |
| `VUS_USE_PY` | 进程内嵌入 Python（`vus_py_init`、`vus_plugin_run_vux_inproc`、`vus_typeof` 完整实现） |

（宏默认由 Makefile 依据环境自动注入；JSON 与 GUI 不需要宏即可用。）

---

## 十二、版本演进路线

| 版本 | 核心交付 | 状态 |
|------|---------|:----:|
| v0.1 | 基础语言 + 插件体系 + C ABI | ✅ 完成 |
| v0.2 | 调试体验优化 + 预编译包 + 安装脚本 | ✅ 完成 |
| v1.0-alpha | 泛型 + 结构体 + 多线程/协程 + APK 打包 | ✅ 完成 |
| v1.0-beta | 语言核心稳定 + APK 修复优化 + TUI/网络/文件/日期/日志 插件 + Meilisearch 插件 + EasyLogger + **GUI 双机制（GuiLite 画布流 + VUA 组件流）+ 体感音游 + 生成代码/运行时性能优化 + 插件系统修复（vusx omit_main、JSON 死循环）+ JSON(yyjson) 默认可用** | ✅ 完成 |
| v1.0 | 正式版 + 加密/数据库 | 🚀 未来 |
| v7.0 | 包管理器仓库（`vus install` 生态） | 🚀 未来 |
| v8.0 | 编译器自举（VUS 编译自身） | 🚀 未来 |

---

## 十三、社区与资源

| 渠道 | 用途 |
|------|------|
| 百度贴吧 · VUS语言吧 | 讨论交流、问题反馈、社区活动 |
| Gitee Issues | Bug 报告、功能请求 |
| 邮箱：rtcn_0523@qq.com | 私下联系 |

**开源协议**：MIT