# VUS 编译器 — API 参考文档

> 版本：对应 ABI v1.0.0（`VUS_ABI_VERSION_*`） 与编译器 v1.0-beta
> 说明：本文档基于项目**真实头文件**撰写，所有签名均与源码一致。仅供参考，不涉及任何 GUI 相关 API（guilite / guilibridge / gui，尚未完成）。

本参考文档覆盖四层插件体系与 C ABI、运行时库的公开接口，共分 **9 章**：

1. 概述：C ABI 与插件体系概览、头文件清单
2. 核心类型：VusConfig、VusResult
3. C ABI 接口：vus_compile_file / vus_compile_string / vus_compile_string_to_exe / vus_eval 及版本函数
4. `.vux` 功能插件接口
5. `.vulage` 语言插件接口
6. `.vusx` VUS 插件接口
7. 运行时 API 完整参考
8. 插件运行时函数
9. 附录：公共头文件目录结构与 ABI 版本说明

---

## 1. 概述

VUS 是一个「编译到 C」的中文友好多范式编程语言。编译器将 `.vus` 源码逐级降级为 C 代码，再交给 GCC/Clang 生成原生可执行文件。围绕该核心，对外暴露：

- **C ABI**（`vus_abi.h`）：稳定、可由 `extern "C"` 导出的编译接口，供 C/C++、Python(ctypes)、Ruby 等外部程序嵌入调用。
- **四层插件体系**：从源码级到编译前语法级逐层扩展语言能力。

### 1.1 四层插件体系总览

| 类型 | 扩展名 | 编写语言 | 加载时机 | 主要头文件 |
|------|--------|----------|----------|-----------|
| 源码 | `.vus` | VUS | 编译时 | （无，普通源文件） |
| VUS 插件 | `.vusx` | VUS | 编译时（自动编译 `.o` 并链接） | `vus_vusx.h` |
| 功能插件 | `.vux` | Python / C | 运行时 | `vus_plugin.h` |
| 语言插件 | `.vulage` | Python / C | 编译前预处理 | `vus_lang.h` |

```
┌──────────────────────────────────────────────────────┐
│  源码 .vus     普通 VUS 源文件，import 机制复用       │
├──────────────────────────────────────────────────────┤
│  VUS 插件 .vusx   VUS 编写，编译时自动编译+链接        │
├──────────────────────────────────────────────────────┤
│  功能插件 .vux    Python/C 运行时插件，dlopen 加载     │
├──────────────────────────────────────────────────────┤
│  语言插件 .vulage  Python/C 语法预处理插件（词法前）   │
└──────────────────────────────────────────────────────┘
```

### 1.2 头文件清单

| 文件 | 内容 | 定义对象 |
|------|------|----------|
| `include/vus/vus.h` | 核心公共接口（顶层编译流水线封装） | `VusResult`、`vus_compile_to_c`、`vus_compile_to_exe`、`vus_run` |
| `include/vus/vus_abi.h` | C ABI 接口 | ABI 版本宏与函数、`vus_compile_file`、`vus_compile_string`、`vus_compile_string_to_exe`、`vus_eval` |
| `include/vus/vus_plugin.h` | `.vux` 功能插件接口 | `VusPlugin`、`VusPluginAPI`、注册/加载/生命周期/查询 API |
| `include/vus/vus_lang.h` | `.vulage` 语言插件接口 | `VusLangPlugin`、预处理、注册/加载/生命周期 API |
| `include/vus/vus_vusx.h` | `.vusx` VUS 插件接口 | `VusVusxPlugin`、解析/编译/清理 API |
| `src/config.h`（被 `vus.h` include） | 项目配置 | `VusConfig` 结构体与配置操作函数 |
| `rt/libvus_rt.h` | 运行时库接口 | `VusString`/`VusList`/`VusDict`/`VusClosure`/`VusError`/`VusObject` 及运行时函数 |

> 注意：`vus.h` 通过 `#include "../src/config.h"` 引入 `VusConfig`，因此 `VusConfig` 的权威定义在 `src/config.h`。

---

## 2. 核心类型

### 2.1 `VusConfig`（项目/编译配置）

定义于 `src/config.h`，是编译流水线的配置载体。字段逐一说明：

```c
typedef struct {
    char  project_dir[1024];   /* 项目根目录 */
    char  name[256];           /* 项目名称 */
    char  version[64];         /* 版本号 */
    char  style[32];           /* 语法风格（默认"函数"） */
    char  language_plugin[64]; /* 语言插件名称（如"易语言"，空=使用核心语法） */
    char  vusx_deps[VUS_CONFIG_MAX_VUSX_DEPS][256]; /* vusx 依赖路径列表 */
    int   vusx_deps_count;                    /* vusx 依赖数量 */
    char  main_file[256];      /* 主文件路径 */
    char  output_mode[16];     /* "c" 或 "exe" */
    char  list_mode[16];       /* "严格" 或 "混合" */
    int   debug;               /* 0 或 1 */
    char  target_platform[32]; /* "linux-gnu" / "linux-musl" / "android" */
    char  rt_dir[1024];        /* 运行时库目录 */
    char  build_dir[1024];     /* 构建输出目录 */

    /* 编译选项 */
    char  optimization[16];    /* "速度" / "体积" / "调试" */
    char  arm_version[16];     /* "ARM64" / "ARM32" */
} VusConfig;
```

| 字段 | 大小 | 说明 |
|------|------|------|
| `project_dir` | 1024 | 项目根目录 |
| `name` | 256 | 项目名称 |
| `version` | 64 | 版本号 |
| `style` | 32 | 语法风格，默认 `"函数"` |
| `language_plugin` | 64 | 语言插件名称（如 `"易语言"`），空串表示使用核心语法 |
| `vusx_deps` | 16×256 | vusx 依赖路径列表 |
| `vusx_deps_count` | int | vusx 依赖数量 |
| `main_file` | 256 | 主文件路径 |
| `output_mode` | 16 | `"c"` 或 `"exe"` |
| `list_mode` | 16 | `"严格"` 或 `"混合"` |
| `debug` | int | 0 或 1 |
| `target_platform` | 32 | `"linux-gnu"` / `"linux-musl"` / `"android"` |
| `rt_dir` | 1024 | 运行时库目录 |
| `build_dir` | 1024 | 构建输出目录 |
| `optimization` | 16 | `"速度"` / `"体积"` / `"调试"` |
| `arm_version` | 16 | `"ARM64"` / `"ARM32"` |

相关常量：

```c
#define VUS_CONFIG_MAX_VUSX_DEPS 16  /* 最大 vusx 依赖数量 */
```

配置操作函数（`src/config.h`）：

```c
int  vus_config_load(VusConfig *config, const char *project_dir);          /* 从 project_dir/vus.json 加载 */
void vus_config_main_path(VusConfig *config, char *buf, size_t buf_size);  /* 主文件完整路径 */
void vus_config_build_path(VusConfig *config, char *buf, size_t buf_size); /* 构建目录路径 */
void vus_config_rt_header_path(VusConfig *config, char *buf, size_t buf_size); /* 运行时头文件路径 */
void vus_config_rt_source_path(VusConfig *config, char *buf, size_t buf_size); /* 运行时源文件路径 */
```

数组容量：`vusx_deps` 共 `VUS_CONFIG_MAX_VUSX_DEPS`(16) 行、每行 256 字节。

### 2.2 `VusResult`（编译结果）

定义于 `include/vus/vus.h`：

```c
typedef struct {
    int   success;
    char  error_msg[512];
    char  c_output_path[1024];   /* 生成的 C 文件路径 */
    char  exe_output_path[1024]; /* 可执行文件路径 */
} VusResult;
```

| 字段 | 大小 | 说明 |
|------|------|------|
| `success` | int | `1`=成功，`0`=失败 |
| `error_msg` | 512 | 失败时的错误描述 |
| `c_output_path` | 1024 | 生成的 C 文件路径 |
| `exe_output_path` | 1024 | 可执行文件路径 |

`vus.h` 顶层封装函数（编译流水线入口）：

| 函数 | 说明 |
|------|------|
| `VusResult vus_compile_to_c(const char *vus_file_path, VusConfig *config)` | 编译单个 `.vus` 文件，生成 C 代码 |
| `VusResult vus_compile_to_exe(const char *vus_file_path, VusConfig *config)` | 编译 `.vus` 文件并生成可执行文件 |
| `int vus_run(const char *vus_file_path, VusConfig *config)` | 编译并运行 |

---

## 3. C ABI 接口

定义于 `include/vus/vus_abi.h`。所有函数均以 `extern "C"` 导出，C++ 编译时自动兼容。

### 3.1 ABI 版本

```c
#define VUS_ABI_VERSION_MAJOR 1
#define VUS_ABI_VERSION_MINOR 0
#define VUS_ABI_VERSION_PATCH 0
```

| 宏 | 值 | 语义 |
|----|----|------|
| `VUS_ABI_VERSION_MAJOR` | 1 | 主版本号（不兼容的 ABI 变更） |
| `VUS_ABI_VERSION_MINOR` | 0 | 次版本号（向后兼容的新增） |
| `VUS_ABI_VERSION_PATCH` | 0 | 补丁版本（向后兼容的缺陷修复） |

```c
int         vus_abi_version(void);                 /* 获取 ABI 版本号（编码为 0xMMmmpp） */
const char *vus_abi_version_string(void);          /* 获取 ABI 版本字符串（如 "1.0.0"） */
```

### 3.2 编译函数

#### `vus_compile_file` — 编译 `.vus` 文件 → C 代码

```c
VusResult vus_compile_file(const char *path, VusConfig *config);
```

| 参数 | 类型 | 说明 |
|------|------|------|
| `path` | `const char *` | `.vus` 源文件路径 |
| `config` | `VusConfig *` | 编译配置（可为 NULL，此时使用默认配置） |

| 返回值 | 说明 |
|--------|------|
| `success==1` | 成功，`result.c_output_path` 为生成的 C 文件路径 |
| `success==0` | 失败，`result.error_msg` 含错误描述 |

#### `vus_compile_string` — 从源码字符串编译 → C 代码

```c
VusResult vus_compile_string(const char *source, VusConfig *config);
```

从内存中已 `'\0'` 结尾的 VUS 源码字符串编译，生成 C 代码；不涉及文件系统 I/O（除最终写入 C 文件外）。

- 写入的 C 文件路径由 `config->build_dir` 或 `config->project_dir` 决定。
- 文件名取 `source` 的前 16 个字符 sanitize 后作为名称。

| 参数 | 类型 | 说明 |
|------|------|------|
| `source` | `const char *` | VUS 源码字符串 |
| `config` | `VusConfig *` | 编译配置（可为 NULL） |

#### `vus_compile_string_to_exe` — 源码编译 + 链接为可执行文件

```c
VusResult vus_compile_string_to_exe(const char *source, VusConfig *config);
```

相当于 `vus_compile_string()` + GCC 链接。成功后 `result.exe_output_path` 为可执行文件路径。

| 参数 | 类型 | 说明 |
|------|------|------|
| `source` | `const char *` | VUS 源码 |
| `config` | `VusConfig *` | 编译配置 |

#### `vus_eval` — 表达式求值

```c
VusResult vus_eval(const char *code, VusConfig *config, char *output);
```

编译并执行 VUS 代码片段，返回求值结果字符串。实现方式：将代码包装为完整程序 → 编译 → 运行 → 捕获 stdout。

| 参数 | 类型 | 说明 |
|------|------|------|
| `code` | `const char *` | VUS 代码片段（如 `"1 + 2 * 3"`） |
| `config` | `VusConfig *` | 编译配置 |
| `output` | `char *` | 输出缓冲区（由调用方分配，**至少 4096 字节**），填写程序执行的 stdout 内容 |

注意：
- 每次调用都会启动一个子进程，开销较大。
- 输出截断为 4096 字节以内。
- `result.success` 表示编译和执行均成功。

### 3.3 使用示例

**C 语言示例**（头文件内注释提供）：

```c
#include <vus/vus_abi.h>
#include <string.h>
#include <stdio.h>

int main(void) {
    VusConfig config;
    memset(&config, 0, sizeof(config));
    strcpy(config.style, "函数");
    strcpy(config.project_dir, ".");

    VusResult res = vus_compile_file("script.vus", &config);
    if (res.success)
        printf("生成: %s\n", res.c_output_path);
    else
        fprintf(stderr, "错误: %s\n", res.error_msg);
    return 0;
}
```

**Python ctypes 示例**（头文件内注释提供）：

```python
import ctypes

class VusResult(ctypes.Structure):
    _fields_ = [
        ("success", ctypes.c_int),
        ("error_msg", ctypes.c_char * 512),
        ("c_output_path", ctypes.c_char * 1024),
        ("exe_output_path", ctypes.c_char * 1024),
    ]

class VusConfig(ctypes.Structure):
    _fields_ = [  # 篇幅起见仅声明用到的字段，需与 C 结构体布局一致
        ("project_dir", ctypes.c_char * 1024),
        ("name", ctypes.c_char * 256),
        ("version", ctypes.c_char * 64),
        ("style", ctypes.c_char * 32),
        ("language_plugin", ctypes.c_char * 64),
        # ... 其余字段按 src/config.h 顺序补全
    ]

lib = ctypes.CDLL("./libvus.so")
lib.vus_abi_version.restype = ctypes.c_int
print("ABI 版本:", hex(lib.vus_abi_version()))

lib.vus_compile_file.argtypes = [ctypes.c_char_p, ctypes.POINTER(VusConfig)]
lib.vus_compile_file.restype = VusResult

cfg = VusConfig()
cfg.style = b"函数"
cfg.project_dir = b"."
res = lib.vus_compile_file(b"script.vus", ctypes.byref(cfg))
print("成功:", res.success, "C 输出:", res.c_output_path)
```

---

## 4. `.vux` 功能插件接口

定义于 `include/vus/vus_plugin.h`。`.vux` 插件是动态共享库（`.so` / `.dll`），通过 `VusPlugin` 结构体暴露给编译器，可在编译流水线的各个阶段插入自定义行为。

### 4.1 导出宏

```c
#if defined(_WIN32) || defined(_WIN64)
#  define VUS_PLUGIN_EXPORT __declspec(dllexport)
#else
#  define VUS_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif
```

插件 `.so` 中必须且仅定义一个 `VUS_PLUGIN_EXPORT` 符号；编译器通过 `dlsym("vus_plugin_entry")` 定位入口。

### 4.2 `VusPluginAPI` — 插件可调用的编译器 API 表

插件通过此结构体访问编译器功能，无需直接链接编译器。编译器在调用 `plugin->init()` 前填充此结构体并传入。

```c
typedef struct {
    int version;                                    /* ABI 版本号（编译器的 ABI 版本，用于兼容性检查） */
    VusResult (*compile_file)(const char *path, VusConfig *config);
    VusResult (*compile_string)(const char *source, VusConfig *config);
    VusResult (*compile_string_to_exe)(const char *source, VusConfig *config);
    VusResult (*eval)(const char *code, VusConfig *config, char *output);
    const char *(*compiler_version)(void);          /* 获取编译器版本 */
} VusPluginAPI;
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `version` | int | 编译器 ABI 版本号 |
| `compile_file` | 函数指针 | 编译 `.vus` 文件 → C 代码 |
| `compile_string` | 函数指针 | 编译 VUS 源码字符串 → C 代码 |
| `compile_string_to_exe` | 函数指针 | 编译字符串 → 可执行文件 |
| `eval` | 函数指针 | 求值 VUS 表达式，返回 stdout 输出 |
| `compiler_version` | 函数指针 | 获取编译器版本 |

函数指针的签名均与第 3 章 C ABI 相应函数一致。

### 4.3 `VusPlugin` — 插件描述符

```c
typedef struct VusPlugin {
    const char *name;
    const char *version;
    int  (*init)(VusPluginAPI *api);
    int  (*run)(VusPluginAPI *api, const char *input, char **output);
    void (*cleanup)(VusPluginAPI *api);
    const char *description;      /* 可选：描述（--list-plugins 显示） */
    const char *author;           /* 可选：作者 */
} VusPlugin;
```

所有字段均为必填（除非明确指出可选，`description`、`author` 为可选，可 NULL）。

| 字段 | 类型 | 说明 |
|------|------|------|
| `name` | `const char *` | 插件名称（唯一标识符，不可为 NULL） |
| `version` | `const char *` | 插件版本号（如 `"1.0.0"`） |
| `init` | 函数指针 | 初始化回调。加载后、使用前调用；返回 0 成功，非 0 失败（编译器将卸载插件）。`api` 指针在插件整个生命周期内有效 |
| `run` | 函数指针 | 执行回调。`input` 输入数据（可 NULL），`output` 输出数据（由插件分配，调用方负责 `free`）；返回 0 成功，非 0 失败 |
| `cleanup` | 函数指针 | 清理回调。卸载前调用，释放资源；可为 NULL（无需清理） |
| `description` | `const char *` | 可选，插件描述 |
| `author` | `const char *` | 可选，插件作者 |

### 4.4 注册 / 加载 / 生命周期 / 查询 API

```c
#define VUS_MAX_PLUGINS 64   /* 最大注册插件数量 */

int  vus_register_plugin(VusPlugin *plugin);
int  vus_plugin_load(const char *path);
int  vus_plugin_init_all(void);
int  vus_plugin_run_all(const char *input, char **output);
void vus_plugin_cleanup_all(void);
VusPlugin *vus_plugin_find(const char *name);
int  vus_plugin_count(void);
void vus_plugin_list_all(void);
void vus_plugin_unload_all(void);
```

| 函数 | 语义 | 返回值 |
|------|------|--------|
| `vus_register_plugin` | 注册一个插件（加载 `.so` 时自动注册，也可手动调用） | 0 成功，-1 插件已满，-2 名称冲突 |
| `vus_plugin_load` | 从共享库加载插件（`path` 为 `.so`/`.dll` 路径） | 0 成功，-1 无法打开，-2 未找到入口符号 |
| `vus_plugin_init_all` | 遍历插件列表调用每个 `init` 回调 | 成功初始化的插件数量（失败的不计入） |
| `vus_plugin_run_all` | 对每个插件调用其 `run` 回调 | 成功运行的插件数量 |
| `vus_plugin_cleanup_all` | 逆序遍历插件列表调用每个 `cleanup` 回调 | void |
| `vus_plugin_find` | 按名称查找已注册插件 | 插件指针，未找到返回 NULL |
| `vus_plugin_count` | 获取已注册插件数量 | int |
| `vus_plugin_list_all` | 列出所有已注册插件（打印到 stdout） | void |
| `vus_plugin_unload_all` | 卸载所有插件并释放内部资源 | void |

### 4.5 C 插件编写示例

```c
#include <vus/vus_plugin.h>
#include <stdio.h>
#include <string.h>

static int my_init(VusPluginAPI *api) {
    printf("插件已加载，ABI 版本: %d\n", api->version);
    return 0;
}

static int my_run(VusPluginAPI *api, const char *input, char **output) {
    *output = strdup("插件处理结果");
    return 0;
}

static void my_cleanup(VusPluginAPI *api) { }

VusPlugin g_plugin = {
    .name    = "my_plugin",
    .version = "1.0.0",
    .init    = my_init,
    .run     = my_run,
    .cleanup = my_cleanup
};

/* 编译器加载时会自动调用此函数 */
VUS_PLUGIN_EXPORT void vus_plugin_entry(VusPlugin **plugin) {
    *plugin = &g_plugin;
}
```

编译为共享库：

```bash
gcc -shared -fPIC -o my_plugin.so my_plugin.c -I/path/to/vus/include
```

使用方法：

```c
vus_plugin_load("my_plugin.so");
vus_plugin_init_all();
vus_plugin_run_all("input", &output);
vus_plugin_cleanup_all();
```

### 4.6 Python 插件基类（`scripts/vux_plugin_entry.py`）

`VuxPlugin` 基类：所有 Python 插件应在 `__init__.py` 中继承，实现 `init()`、`run()`、`cleanup()` 三个生命周期方法，元数据由 `vux.json` 自动填充。

```python
from vux_plugin_entry import VuxPlugin

class MyPlugin(VuxPlugin):
    def init(self, api):
        print(f"ABI 版本: {api['version']}")
        return 0

    def run(self, api, input_data):
        return 0, f"处理结果: {input_data}"

    def cleanup(self, api):
        pass
```

| 方法 | 签名语义 |
|------|----------|
| `init(self, api)` | 初始化插件；返回 0 成功，非 0 失败 |
| `run(self, api, input_data)` | 执行主要功能；返回 `(code, output)` |
| `cleanup(self, api)` | 清理资源 |

同时提供：
- `VuxPluginAPI`：通过 ctypes 调用编译器 C ABI 的封装，提供 `version` 属性（0x010000 = ABI v1.0.0）及 `compile_file` / `compile_string` / `eval` / `compiler_version` 方法；优先加载 `libvus.so`，缺省时回退到 CLI 调用。
- `load_plugin(plugin_dir)`：从 `.vux` 解压目录加载插件（读取 `vux.json` 元数据 + `__init__.py`，实例化继承 `VuxPlugin` 的类）。
- `load_plugin_from_vux(vux_path)`：从 `.vux` 文件加载（解压到临时目录后调用 `load_plugin`）。

---

## 5. `.vulage` 语言插件接口

定义于 `include/vus/vus_lang.h`。语言插件（`.vulage`）负责在编译前端将不同风格的源代码解析为统一的标准 AST，在词法分析阶段之前加载：

```
加载 .vulage → 源码预处理 → 词法分析 → 语法分析 → AST → 代码生成
```

与 `.vux` 的区别：

| `.vulage`（语言插件） | `.vux`（功能插件） |
|------------------------|---------------------|
| 编译前端生效 | 运行时生效 |
| 解析前加载 | 解析后加载 |
| 影响"怎么读代码" | 影响"怎么运行代码" |
| 项目级锁定 | 可按需加载 |

### 5.1 导出宏

```c
#if defined(_WIN32) || defined(_WIN64)
#  define VUS_LANG_EXPORT __declspec(dllexport)
#else
#  define VUS_LANG_EXPORT __attribute__((visibility("default")))
#endif
```

插件 `.vulage` 中必须且仅定义一个 `VUS_LANG_EXPORT` 符号；编译器通过 `dlsym("vus_lang_entry")` 定位入口。

### 5.2 `VusLangPlugin` — 语言插件描述符

```c
typedef struct VusLangPlugin {
    const char *name;
    const char *version;
    const char *ast_version;
    int  (*preprocess)(const char *input, char **output);
    int  (*init)(void);
    void (*cleanup)(void);
    const char *description;   /* 可选 */
    const char *author;        /* 可选 */
} VusLangPlugin;
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `name` | `const char *` | 插件名称（唯一标识符，如 `"易语言"`、`"函数风格"`） |
| `version` | `const char *` | 插件版本号（如 `"1.0.0"`） |
| `ast_version` | `const char *` | 兼容的 AST 版本号（如 `"1.0"`） |
| `preprocess` | 函数指针 | 预处理：将输入源码转换为标准 VUS 函数风格语法。`input` 原始源代码、`output` 转换后代码（由插件分配，调用方负责 `free`）；返回 0 成功，非 0 失败。无需预处理时可 `*output = strdup(input)` 原样返回 |
| `init` | 函数指针 | 可选（可 NULL）。初始化回调；返回 0 成功，非 0 失败 |
| `cleanup` | 函数指针 | 可选（可 NULL）。清理回调，释放资源 |
| `description` | `const char *` | 可选，插件描述 |
| `author` | `const char *` | 可选，作者 |

> 注意：`preprocess` 的回调签名与 `ECOSYSTEM.md` 中草稿形式（`char *(*preprocess)(const char*, size_t)`）不同，**权威签名以上方头文件为准**：`int (*preprocess)(const char *input, char **output)`。

### 5.3 语言插件注册和管理

```c
#define VUS_MAX_LANG_PLUGINS 16   /* 最大注册语言插件数量 */

int  vus_lang_register(VusLangPlugin *plugin);
int  vus_lang_load(const char *path);
VusLangPlugin *vus_lang_find(const char *name);
int  vus_lang_preprocess(const char *name, const char *input, char **output);
int  vus_lang_count(void);
void vus_lang_list_all(void);
int  vus_lang_init_all(void);
void vus_lang_cleanup_all(void);
void vus_lang_unload_all(void);
```

| 函数 | 语义 | 返回值 |
|------|------|--------|
| `vus_lang_register` | 注册一个语言插件 | 0 成功，-1 插件已满，-2 名称冲突 |
| `vus_lang_load` | 从 `.vulage` 文件加载（`path` 为文件路径） | 0 成功，-1 无法打开，-2 未找到入口符号 |
| `vus_lang_find` | 按名称查找 | 插件指针，未找到返回 NULL |
| `vus_lang_preprocess` | 对输入源码应用指定语言插件的预处理；`name` 为 NULL 或未找到时直接 `strdup` 原样返回 | 0 成功，非 0 失败 |
| `vus_lang_count` | 已注册数量 | int |
| `vus_lang_list_all` | 列出所有已注册语言插件（打印到 stdout） | void |
| `vus_lang_init_all` | 初始化所有已注册语言插件 | int |
| `vus_lang_cleanup_all` | 清理所有已注册语言插件 | void |
| `vus_lang_unload_all` | 卸载所有语言插件 | void |

### 5.4 语言插件编写示例（易语言风格）

```c
#include <vus/vus_lang.h>
#include <string.h>

static int my_preprocess(const char *input, char **output) {
    *output = strdup(input);   /* 示例：不做转换 */
    return 0;
}

VusLangPlugin g_lang = {
    .name        = "my_lang",
    .version     = "1.0.0",
    .ast_version = "1.0",
    .preprocess  = my_preprocess,
    .init        = NULL,
    .cleanup     = NULL
};

VUS_LANG_EXPORT void vus_lang_entry(VusLangPlugin **plugin) {
    *plugin = &g_lang;
}
```

编译为 `.vulage`：

```bash
gcc -shared -fPIC -o my_lang.vulage my_lang.c -I/path/to/vus/include
```

在 `vus.json` 中配置：

```json
{
    "语言插件": "易语言"
}
```

---

## 6. `.vusx` VUS 插件接口

定义于 `include/vus/vus_vusx.h`。`.vusx` 插件是用 VUS 本身编写的功能扩展插件，在编译时由编译器自动解析、编译、链接到最终可执行文件中。

三种插件对比：

- `.vux` — 运行时功能插件（Python/C 编写）
- `.vulage` — 编译前语法插件（Python/C 编写）
- `.vusx` — 编译时功能插件（VUS 编写）

### 6.1 常量

```c
#define VUS_MAX_VUSX_DEPS    16   /* 最大 vusx 依赖数量 */
#define VUS_MAX_VUSX_EXPORTS 32   /* 最大导出函数数量 */
#define VUS_VUSX_NAME_LEN    64   /* 插件名称最大长度 */
#define VUS_VUSX_PATH_LEN    256  /* 路径最大长度 */
```

### 6.2 `VusVusxPlugin` — `.vusx` 插件描述符

```c
typedef struct {
    char name[VUS_VUSX_NAME_LEN];      /* 插件名称 */
    char version[32];                  /* 插件版本 */
    char dir[VUS_VUSX_PATH_LEN];       /* 插件目录路径 */
    char main_vus[VUS_VUSX_PATH_LEN];  /* main.vus 路径 */
    char c_output[VUS_VUSX_PATH_LEN];  /* 编译后的 C 文件路径 */
    char obj_output[VUS_VUSX_PATH_LEN];/* 编译后的 .o 文件路径 */
    char exports[VUS_MAX_VUSX_EXPORTS][VUS_VUSX_NAME_LEN]; /* 导出函数 */
    int  export_count;                 /* 导出函数数量 */
} VusVusxPlugin;
```

| 字段 | 说明 |
|------|------|
| `name[64]` | 插件名称 |
| `version[32]` | 插件版本 |
| `dir[256]` | 插件目录路径 |
| `main_vus[256]` | main.vus 路径 |
| `c_output[256]` | 编译后的 C 文件路径 |
| `obj_output[256]` | 编译后的 `.o` 文件路径 |
| `exports[32][64]` | 导出函数名数组 |
| `export_count` | 导出函数数量 |

### 6.3 `.vusx` 操作函数

```c
int  vus_vusx_resolve(const char *path, VusVusxPlugin *plugin);
int  vus_vusx_compile(VusVusxPlugin *plugin, VusConfig *config);
int  vus_vusx_resolve_all(VusConfig *config, VusVusxPlugin *plugins, int *count);
int  vus_vusx_compile_all(VusVusxPlugin *plugins, int count, VusConfig *config);
void vus_vusx_cleanup_all(VusVusxPlugin *plugins, int count);
```

| 函数 | 语义 | 返回值 |
|------|------|--------|
| `vus_vusx_resolve` | 解析 `.vusx` 目录，填充 `VusVusxPlugin` 结构体。`path` 为 `.vusx` 目录路径 | 0 成功，-1 目录不存在，-2 vusx.json 解析失败 |
| `vus_vusx_compile` | 编译 `.vusx` 插件：将 VUS 源码编译为 C 代码，再编译为 `.o` 文件 | 0 成功，非 0 失败 |
| `vus_vusx_resolve_all` | 从 `config` 解析所有 vusx 依赖。`plugins` 输出数组；`count` 输入为数组大小、输出为实际数量 | 0 成功，-1 超过最大数量，-2 解析失败 |
| `vus_vusx_compile_all` | 编译所有 vusx 依赖 | 0 成功，非 0 失败 |
| `vus_vusx_cleanup_all` | 清理 vusx 插件（释放临时文件） | void |

### 6.4 vusx.json 元数据

`.vusx` 目录结构：

```
my_plugin.vusx/
├── vusx.json      # 插件元数据（必需）
├── main.vus       # VUS 源码（必需）
└── 依赖.txt       # 依赖列表（可选）
```

`vusx.json` 格式：

```json
{
    "名称": "my_utils",
    "版本": "1.0.0",
    "入口": "main.vus",
    "导出": ["问候", "计算"],
    "依赖": {}
}
```

在 `vus.json` 中声明：

```json
{
    "vusx依赖": ["my_utils.vusx", "path/to/other.vusx"]
}
```

### 6.5 编译链接流程

```
读取 vus.json → 解析 vusx 依赖 → 编译 .vusx（VUS→C→.o）→ 链接到主程序可执行文件
```

即：编译时在主程序之前解析和编译，走 `VUS → C → .o` 流水线，自动链接到最终可执行文件。

对应 CLI：`vus vusx list|info|build`。

---

## 7. 运行时 API 完整参考

定义于 `rt/libvus_rt.h`，供编译器生成的 C 代码与运行时库使用。以下为逐一列出的原型。

### 7.1 类型标记常量

```c
#define TYPE_INT     1
#define TYPE_FLOAT   2
#define TYPE_STR     3
#define TYPE_BOOL    4
#define TYPE_LIST    5
#define TYPE_DICT    6
#define TYPE_MIXED   99
```

### 7.2 `VusObject` — 结构化容器（组合容器）

带类型标记的轻量容器，内部 union 持有指向 `VusList`/`VusDict`/`VusString` 的指针。不替代、不重写既有结构，用于承载插件返回的结构化数据。

```c
#define VUS_OBJECT_MAGIC 0x564F4221  /* 'VOB!'：运行时识别结构化容器的魔数 */

struct VusObject {
    int ref;    /* 引用计数，必须为第一个字段 */
    int magic;  /* VUS_OBJECT_MAGIC：供 vus_print/vus_typeof 区分 VusObject 与 VusString */
    int type;   /* TYPE_LIST / TYPE_DICT / TYPE_STR / TYPE_INT / TYPE_FLOAT / TYPE_BOOL */
    union {
        VusList*    list;
        VusDict*    dict;
        VusString*  str;
    } u;
};

static inline int vus_is_object(void* obj);
VusString* vus_object_to_string(void* obj);
```

| 函数 | 语义 |
|------|------|
| `vus_is_object(obj)` | 判断指针是否为结构化容器（`VusObject*`）。非容器（普通 `VusString*`）返回 0。依据 `VusObject` 在 `ref` 后有 magic 标记，而 `VusString` 在 `ref` 后是 `len`（小整数），不会与 `VUS_OBJECT_MAGIC` 冲突 |
| `vus_object_to_string(obj)` | 将任意值（`VusString*` 或 `VusObject*`）转为字符串表示：标量取原文，列表/字典递归序列化。纯 C 实现，不依赖嵌入式 Python |

### 7.3 引用计数通用操作

```c
void vus_ref(void* obj);
void vus_unref(void* obj);
```

### 7.4 `VusString` — 字符串

`data` 始终以 `'\0'` 结尾（便于 C 互操作），但字符串内容可能包含中间 `'\0'`。`len` 表示有效字节长度（不含结尾 `'\0'`），保证 `len <= strlen(data)`。

```c
struct VusString {
    int ref;
    int len;          /* UTF-8 字节长度 */
    char* data;       /* 只读字符缓冲区 */
};
```

| 函数 | 原型 | 说明 |
|------|------|------|
| 创建 | `VusString* vus_string_new(const char* s)` | 从 C 字符串创建 |
| 创建 | `VusString* vus_string_new_len(const char* s, int len)` | 按指定长度创建 |
| 拼接 | `VusString* vus_string_concat(VusString* a, VusString* b)` | 拼接两个字符串 |
| 切片 | `VusString* vus_string_slice(VusString* s, int start, int len)` | 取子串 |
| 长度 | `int vus_string_len(VusString* s)` | 返回长度 |
| 转 C 串 | `char* vus_string_cstr(VusString* s)` | 返回指向 `data` 的指针，调用方禁止修改或释放 |

### 7.5 `VusList` — 列表

```c
struct VusList {
    int ref;
    int len;
    int cap;
    void** items;
    int type;          /* 元素类型标记（严格模式）/ TYPE_MIXED（混合模式） */
};
```

| 函数 | 原型 | 说明 |
|------|------|------|
| 创建 | `VusList* vus_list_new(int type)` | 创建指定元素类型的列表 |
| 追加 | `void vus_list_append(VusList* list, void* item)` | 追加元素 |
| 获取 | `void* vus_list_get(VusList* list, int index)` | 取元素 |
| 删除 | `void vus_list_remove(VusList* list, int index)` | 删除元素 |
| 设置 | `void vus_list_set(VusList* list, int index, void* item)` | 覆盖元素 |
| 长度 | `int vus_list_len(VusList* list)` | 返回长度 |

### 7.6 `VusDict` — 字典

键仅支持 `VusString*`（字符串），值可为任意 VUS 对象。哈希表实现策略：链地址法（separate chaining），负载因子超过 0.75 时自动扩容。`v0.1` 不提供字典遍历接口，`v1.0` 补充。

```c
struct VusDict {
    int ref;
    void* impl;       /* 哈希表实现（运行时库内部） */
};
```

| 函数 | 原型 | 说明 |
|------|------|------|
| 创建 | `VusDict* vus_dict_new(void)` | 创建空字典 |
| 设置 | `void vus_dict_set(VusDict* dict, VusString* key, void* value)` | 写键值对 |
| 获取 | `void* vus_dict_get(VusDict* dict, VusString* key)` | 读键对应值 |
| 删除 | `void vus_dict_remove(VusDict* dict, VusString* key)` | 删除键 |
| 长度 | `int vus_dict_len(VusDict* dict)` | 返回条目数 |

### 7.7 `VusClosure` — 闭包

闭包参数约定：`args` 由调用方分配，调用结束后由调用方负责释放（`vus_unref`）。`func` 在调用期间持有 `args` 的引用，但不负责释放。

```c
struct VusClosure {
    int ref;
    void (*func)(void* env, void* args);
    void* env;
};

VusClosure* vus_closure_new(void (*func)(void*, void*), void* env);
void vus_closure_call(VusClosure* closure, void* args);
```

| 函数 | 原型 | 说明 |
|------|------|------|
| 创建 | `VusClosure* vus_closure_new(void (*func)(void*, void*), void* env)` | 创建闭包 |
| 调用 | `void vus_closure_call(VusClosure* closure, void* args)` | 调用闭包 |

### 7.8 `VusError` — 错误处理（错误码链）

注意：`VusError` **不参与引用计数**，由运行时库独立管理，用户无需调用 `vus_ref`/`vus_unref`。

```c
struct VusError {
    int code;
    int line;
    const char* func;
    const char* msg;
    VusError* next;   /* 错误链（最新错误在链头） */
};

VusError* vus_error_new(int code, const char* msg, int line, const char* func);
void vus_error_push(VusError** chain, VusError* err);
void vus_error_print(VusError* err);
void vus_error_free(VusError* err);
```

| 函数 | 原型 | 说明 |
|------|------|------|
| 创建 | `VusError* vus_error_new(int code, const char* msg, int line, const char* func)` | 创建错误节点 |
| 压栈 | `void vus_error_push(VusError** chain, VusError* err)` | 将错误压入错误链 |
| 打印 | `void vus_error_print(VusError* err)` | 打印错误链 |
| 释放 | `void vus_error_free(VusError* err)` | 释放错误链 |

### 7.9 调试支持

```c
extern int vus_debug_enabled;
void vus_debug_print(const char* msg);
```

### 7.10 分级日志（EasyLogger 集成）

首次调用任一 `日志_*` 内建函数时惰性初始化；无需手动调用 `vus_log_init`。4 级输出函数返回 `VusString*`（`"0"` 成功 / `"-1"` 失败），沿用内建函数约定。

```c
int  vus_log_init(void);                          /* 初始化 EasyLogger，幂等，失败返回 -1 */
VusString* vus_log_set_level(VusString* level);   /* 设置过滤级别（调试/信息/警告/错误） */
VusString* vus_log_debug(VusString* msg);
VusString* vus_log_info(VusString* msg);
VusString* vus_log_warn(VusString* msg);
VusString* vus_log_error(VusString* msg);
```

### 7.11 栈追踪支持

```c
#define VUS_MAX_STACK_DEPTH 256
extern int vus_stack_depth;
extern const char* vus_stack_frames[VUS_MAX_STACK_DEPTH];

void vus_stack_push(const char* func_name);
void vus_stack_pop(void);
void vus_stack_print(void);
```

### 7.12 标准库辅助函数

以下函数由编译器生成的 C 代码调用，用于标准库功能。

```c
void       vus_print(void* s);                 /* 打印输出 */
VusString* vus_input(VusString* prompt);       /* 读取输入 */
VusString* vus_add(VusString* a, VusString* b);/* 加法/字符串拼接 */
int64_t    vus_to_int(VusString* s, int* err); /* 字符串转整数 */
VusString* vus_to_string(int64_t n);           /* 整数转字符串 */
double     vus_to_float(VusString* s, int* err); /* 字符串转浮点数 */
```

| 函数 | 语义 | 约束 |
|------|------|------|
| `vus_print(s)` | 打印，`s` 可为字符串或结构化容器 | — |
| `vus_input(prompt)` | 以提示符读取一行输入 | — |
| `vus_add(a, b)` | 若两个操作数均可解析为整数则做算术加法，否则做字符串拼接 | 类型调度 |
| `vus_to_int(s, err)` | 字符串转整数；成功时返回结果并 `*err=0`，失败返回 0 且 `*err` 非 0 | 调用方应检查 `err` 并抛异常 |
| `vus_to_string(n)` | 整数转字符串 | — |
| `vus_to_float(s, err)` | 字符串转浮点数；成功返回结果并 `*err=0`，失败返回 0.0 且 `*err` 非 0 | — |

### 7.13 线程支持

```c
typedef struct VusThread VusThread;

VusThread* vus_thread_create(void* (*func)(void*), void* arg); /* 创建线程 */
void*      vus_thread_join(VusThread* thread);                 /* 等待线程结束并取返回值 */
void       vus_thread_detach(VusThread* thread);               /* 分离线程 */
```

### 7.14 线程 / 协程句柄接口（返回 `VusString*` 句柄）

以字符串句柄替代裸指针，避免指针类型转换问题。

```c
#define VUS_MAX_HANDLES 64   /* 句柄注册表槽位数 */

VusString* vus_thread_create_handle(void* (*func)(void*), void* arg);
void*      vus_thread_join_handle(VusString* handle);
VusString* vus_coro_create_handle(void (*func)(void*), void* arg);
void       vus_coro_resume_handle(VusString* handle);
```

| 函数 | 语义 |
|------|------|
| `vus_thread_create_handle` | 创建线程并返回 `VusString*` 句柄 |
| `vus_thread_join_handle` | 依据句柄等待线程返回 |
| `vus_coro_create_handle` | 创建协程并返回句柄 |
| `vus_coro_resume_handle` | 依据句柄恢复协程 |

> 内部对应全局句柄注册表 `vus_thread_handles[]` / `vus_coro_handles[]`（各 `VUS_MAX_HANDLES`=64 个槽位）。

### 7.15 异步 / 协程支持（ucontext 实现）

```c
typedef struct VusCoroutine VusCoroutine;

VusCoroutine* vus_coro_create(void (*func)(void*), void* arg);
void          vus_coro_resume(VusCoroutine* coro);
void          vus_coro_yield(void);
int           vus_coro_is_done(VusCoroutine* coro);
```

| 函数 | 语义 |
|------|------|
| `vus_coro_create` | 创建轻量级协程 |
| `vus_coro_resume` | 恢复/启动协程执行 |
| `vus_coro_yield` | 让出协程 |
| `vus_coro_is_done` | 协程是否执行完毕 |

### 7.16 JSON 与结构化值（进程内 .vux 调用相关）

```c
void*       vus_json_parse(VusString* s);    /* JSON 字符串 -> 结构化 VusObject*（组合容器），失败返回 NULL */
VusString*  vus_json_generate(void* obj);    /* 结构化 VusObject* -> JSON 字符串，失败返回空串 */
VusString*  vus_typeof(void* obj);           /* 返回结构化值的类型名（整数/浮点/字符串/布尔/列表/字典/空） */
```

---

## 8. 插件运行时函数

以下为编译器内置的插件运行时函数（`VusString*` 接口），定义于 `rt/libvus_rt.h`。共分四类，按头文件实际原型逐一列出。

### 8.1 TUI（使用 ANSI 转义码，无外部依赖）

```c
VusString* vus_plugin_tui_clear(VusString* dummy);            /* 清屏 */
VusString* vus_plugin_tui_set_color(VusString* fg, VusString* bg); /* 设置颜色 */
VusString* vus_plugin_tui_locate(VusString* row, VusString* col);  /* 定位光标 */
VusString* vus_plugin_tui_progress(VusString* current, VusString* total, VusString* width); /* 进度条 */
VusString* vus_plugin_tui_reset(VusString* dummy);            /* 重置终端 */
```

对应 VUS 内建：`tui_清屏()`、`tui_设置颜色(前景色, 背景色)`、`tui_定位(行, 列)`、`tui_进度条(当前值, 总值, 宽度)`、`tui_重置()`。

### 8.2 网络（基于 libcurl，**条件编译 `VUS_HAVE_CURL`**）

> 依赖说明：`VUS_HAVE_CURL` 定义时才链接 libcurl。编译时需 `-DVUS_HAVE_CURL -lcurl`。未定义该宏时，以下函数为空实现（见 `libvus_rt.c` 中 `#ifdef VUS_HAVE_CURL` 分支）。系统需安装 `libcurl-dev`。

```c
VusString* vus_plugin_http_get(VusString* url);               /* HTTP GET */
VusString* vus_plugin_http_post(VusString* url, VusString* data);  /* HTTP POST */
VusString* vus_plugin_http_download(VusString* url, VusString* filepath); /* 下载 */
```

对应 VUS 内建：`网络_GET(url)`、`网络_POST(url, 数据)`、`网络_下载(url, 文件路径)`。

### 8.3 插件调用（.vux 进程内/子进程）

```c
VusString* vus_plugin_run_vux(VusString* plugin, VusString* cmd);        /* 调用 .vux Python 插件（子进程） */
int        vus_py_init(void);                                            /* 进程内嵌入 Python 解释器 */
VusString* vus_plugin_run_vux_inproc(VusString* plugin, VusString* cmd);  /* 进程内调用 .vux 插件 */
void*      vus_plugin_run_vux_json(VusString* plugin, VusString* cmd);    /* 进程内调用返回结构化 VusObject* */
```

| 函数 | 说明 | 依赖/行为 |
|------|------|-----------|
| `vus_plugin_run_vux` | 调用 `.vux` Python 插件 | 子进程方式 |
| `vus_py_init` | 进程内嵌入 Python 解释器（惰性 `dlopen` libpython） | `0` 成功，`-1` 失败或未启用 `VUS_USE_PY` |
| `vus_plugin_run_vux_inproc` | 进程内调用 `.vux` 插件，返回字符串结果 | `VUS_USE_PY` 下用嵌入解释器，否则回退子进程 |
| `vus_plugin_run_vux_json` | 进程内调用 `.vux` 插件，返回结构化 `VusObject*`（列表/字典/字符串） | 失败返回 NULL |

### 8.4 文件操作（基于标准 C I/O，无外部依赖）

```c
VusString* vus_plugin_file_read(VusString* path);          /* 读取文件全部内容 */
VusString* vus_plugin_file_write(VusString* path, VusString* content);  /* 写入（覆盖写） */
VusString* vus_plugin_file_append(VusString* path, VusString* content); /* 追加 */
VusString* vus_plugin_file_exists(VusString* path);        /* 检查是否存在 */
VusString* vus_plugin_file_delete(VusString* path);        /* 删除文件 */
VusString* vus_plugin_file_list(VusString* path);          /* 列出目录内容 */
```

对应 VUS 内建：`文件_读取(路径)`、`文件_写入(路径, 内容)`、`文件_追加(路径, 内容)`、`文件_存在(路径)`、`文件_删除(路径)`、`文件_列表(路径)`。

### 8.5 日期时间（基于 `<time.h>`，无外部依赖）

```c
VusString* vus_plugin_date_now(VusString* dummy);               /* 当前时间（ISO 8601） */
VusString* vus_plugin_date_format(VusString* fmt);              /* 按格式格式化当前时间 */
VusString* vus_plugin_date_parse(VusString* str, VusString* fmt);   /* 解析日期字符串 */
VusString* vus_plugin_date_timestamp(VusString* dummy);         /* Unix 时间戳 */
VusString* vus_plugin_date_from_timestamp(VusString* ts);       /* 时间戳 → 日期字符串 */
VusString* vus_plugin_date_year(VusString* dummy);
VusString* vus_plugin_date_month(VusString* dummy);
VusString* vus_plugin_date_day(VusString* dummy);
VusString* vus_plugin_date_hour(VusString* dummy);
VusString* vus_plugin_date_minute(VusString* dummy);
VusString* vus_plugin_date_second(VusString* dummy);
```

对应 VUS 内建：`日期_现在()`、`日期_格式化(格式)`、`日期_解析(字符串, 格式)`、`日期_时间戳()`、`日期_从时间戳(时间戳)`、`日期_年/月/日/时/分/秒()`。

---

## 9. 附录

### 9.1 公共头文件目录结构

```
vus/
├── include/vus/       # 公共 API 头文件
│   ├── vus.h          # 核心类型与编译流水线入口（VusResult、vus_compile_to_c 等）
│   ├── vus_abi.h      # C ABI 接口（编译/求值/版本函数）
│   ├── vus_plugin.h   # .vux 功能插件接口（VusPlugin、VusPluginAPI、注册/加载/查询）
│   ├── vus_lang.h     # .vulage 语言插件接口（VusLangPlugin、预处理）
│   └── vus_vusx.h     # .vusx VUS 插件接口（VusVusxPlugin、解析/编译/清理）
├── src/
│   └── config.h       # VusConfig 定义（被 vus.h include）
└── rt/
    ├── libvus_rt.h    # 运行时类型与函数（VusString/List/Dict/Closure/Error/Object、标准库、线程/协程、插件运行时函数）
    └── libvus_rt.c    # 运行时实现
```

### 9.2 ABI 版本说明

ABI 版本编号规则：

| 版本段 | 语义 |
|--------|------|
| 主版本（MAJOR） | 不兼容的 ABI 变更 |
| 次版本（MINOR） | 向后兼容的新增功能 |
| 补丁版本（PATCH） | 向后兼容的缺陷修复 |

| 宏 / 函数 | 值 / 说明 |
|-----------|-----------|
| `VUS_ABI_VERSION_MAJOR` | 1 |
| `VUS_ABI_VERSION_MINOR` | 0 |
| `VUS_ABI_VERSION_PATCH` | 0 |
| `vus_abi_version()` | 返回编码为 `0xMMmmpp` 的整数（即 `0x010000`） |
| `vus_abi_version_string()` | 返回 `"1.0.0"` |

### 9.3 条件编译依赖汇总

| 宏 | 生效范围 | 说明 |
|----|----------|------|
| `VUS_HAVE_CURL` | 网络插件运行时函数（`vus_plugin_http_get/post/download`） | 需要 `-lcurl` 链接，系统安装 `libcurl-dev` |
| `VUS_USE_PY` | 进程内 .vux 嵌入调用（`vus_py_init`、`vus_plugin_run_vux_inproc`、`vus_plugin_run_vux_json`） | 惰性 `dlopen` libpython；未定义时相关函数回退到子进程方案 |

### 9.4 限制提示

- 最大注册 `.vux` 插件数量：`VUS_MAX_PLUGINS`=64。
- 最大注册语言插件数量：`VUS_MAX_LANG_PLUGINS`=16。
- `.vusx` 依赖上限：`VUS_MAX_VUSX_DEPS`=16；导出函数上限：`VUS_MAX_VUSX_EXPORTS`=32。
- 线程/协程句柄注册表：`VUS_MAX_HANDLES`=64 个槽位。
- 栈追踪最大深度：`VUS_MAX_STACK_DEPTH`=256。
- `vus_eval` 每次调用都编译 C 代码并链接可执行文件，开销较大；输出截断为 4096 字节。