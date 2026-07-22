# VUS 编译器 v0.1 — 状态报告

## 一、已实现的功能

### 词法分析器 (Lexer) — 稳定
- UTF-8 中文标识符完整支持
- 93 种 Token 类型（含中文关键字、运算符、字面量）
- 缩进敏感的 INDENT/DEDENT 处理
- 多行注释 `#//` 和行尾注释 `#`
- 字符串字面量支持转义符（`\n`、`\t`、`\"`、`\\`、`\xHH`、`\uHHHH`）
- 双语法体系：函数风格（`定义`）和易语言风格（`.版本`）

### 语法分析器 (Parser) — 稳定
- 12 种语句解析函数：
  - 表达式语句、赋值语句、变量声明
  - `如果`/`否则如果`/`否则` 条件分支
  - `循环…从…到…` 范围循环
  - `当循环` 条件循环
  - `定义` 函数定义
  - `返回` 语句
  - `打印()`、`输入()`、`转数字()`、`转文本()` 内置函数
- 8 级表达式优先级（从低到高）：
  - 逻辑或 → 逻辑与 → 比较 → 拼接(`..`) → 加减 → 乘除 → 一元 → 基础
- 递归下降解析，支持中缀、前缀、后缀表达式

### 抽象语法树 (AST) — 稳定
- 27 种 AST 节点类型
- 程序、语句、表达式、函数定义、函数调用、字面量等

### 代码生成器 (Generator) — 稳定
- 完整的中文标识符 → C 标识符 sanitize 转换
- 引用计数自动内存管理（`vus_ref`/`vus_unref`）
- 函数调用使用 GNU C 语句表达式 `({...})` 处理返回值
- 智能 `vus_add` 类型调度（字符串拼接 vs 算术加法）
- 完整类型转换：`vus_to_int`/`vus_to_string`
- 浮点数支持
- 泛型函数调用支持（`函数名<类型>(参数)` 语法）
- 30+ 个内置插件运行时函数处理器（TUI/网络/文件/日期）

### 运行时库 (Runtime) — 稳定
- `VusString` 引用计数字符串类型
- 字符串操作：`vus_string_new`、`vus_string_concat`、`vus_string_cmp` 等
- 数字转换：`vus_to_int`、`vus_to_string`
- 智能加法：`vus_add`（自动识别字符串/数字）
- 列表操作：`VusList`（创建、追加、获取、长度）
- 字典操作：`VusDict`（创建、设置、获取、包含检查）
- 闭包支持：`VusClosure`
- 错误处理：`VusError`
- 标准库函数：`vus_print`、`vus_input`、`vus_to_int`、`vus_to_string`、`vus_type_of`
- 线程/协程句柄注册表：`vus_thread_handles[]`、`vus_coro_handles[]`（各 64 个槽位）
- 25+ 个插件运行时函数实现（TUI ANSI 转义码、libcurl 网络请求、文件 I/O、日期时间）

### CLI 界面 — 稳定
- `vus run <文件>` — 编译并运行 VUS 程序
- `vus run --debug <文件>` — 调试模式运行（含栈追踪和调试输出）
- `vus build --c-only <文件>` — 仅编译为 C 代码
- `vus build --exe <文件>` — 编译为可执行文件
- `vus build --apk <文件> [--ndk-path <路径>]` — 编译为 Android APK 项目
- `vus init` — 交互式项目初始化
- `vus test` — 运行测试用例
- `vus lang list|load|info` — 语言插件管理（.vulage）
- `vus vux install|build|info|list|run` — 功能插件管理（.vux）
- `vus vusx list|info|build` — VUS 插件管理（.vusx）
- `vus --help` — 显示帮助信息
- `vus --version` — 显示版本信息

### C ABI 接口 (v0.1) — 新增
- `vus_compile_file()` — 编译 .vus 文件 → C 代码
- `vus_compile_string()` — 从源码字符串编译 → C 代码
- `vus_compile_string_to_exe()` — 从源码编译并链接 → 可执行文件
- `vus_eval()` — 编译并执行代码片段，捕获 stdout 输出
- `extern "C"` 兼容 C++ 调用
- 完整的 `VusResult` 错误报告

### 插件系统 (v0.1) — 新增

**四层插件体系**：

| 类型 | 扩展名 | 编写语言 | 加载时机 | 状态 |
|------|--------|----------|----------|:----:|
| 源码 | `.vus` | VUS | 编译时 | ✅ 稳定 |
| VUS 插件 | `.vusx` | VUS | 编译时（自动编译+链接） | ✅ 新增 |
| 功能插件 | `.vux` | Python/C | 运行时 | ✅ 新增 |
| 语言插件 | `.vulage` | Python/C | 编译前预处理 | ✅ 新增 |

**`.vux` 运行时插件**：
- `VusPlugin` 结构体（name、version、init、run、cleanup）
- `VusPluginAPI` 编译器 API 表（compile_file、compile_string、eval 等）
- `vus_register_plugin()` — 注册插件
- `vus_plugin_load()` — 从 .so 共享库加载插件
- 生命周期管理：`vus_plugin_init_all`、`vus_plugin_run_all`、`vus_plugin_cleanup_all`
- 查询与列表：`vus_plugin_find`、`vus_plugin_count`、`vus_plugin_list_all`
- Linux .so 和 Windows .dll 兼容导出宏

**`.vulage` 语言插件**：
- `VusLangPlugin` 结构体（name、version、preprocess、init、cleanup）
- dlopen 动态加载共享库
- 编译前预处理：在词法分析之前转换源码
- 易语言语法插件示例（plugins/lang/易语言/）

**`.vusx` VUS 插件**：
- `VusVusxPlugin` 结构体（name、version、dir、main_vus、exports）
- 编译时自动解析 vusx.json 元数据
- VUS → C → .o 编译流水线
- 自动链接到主程序可执行文件
- CLI 命令：`vus vusx list|info|build`

### APK 打包 (v1.0-alpha) — 新增
- `vus build --apk <文件> [--ndk-path <路径>] [--app-name <名称>] [--output <目录>]`
- 自动生成 Android JNI 桥接代码（JNI 函数名自动转换点号 → 下划线）
- 自动替换 `main()` 为 `vus_main()` 避免冲突
- 自动复制运行时库 `libvus_rt.h`/`libvus_rt.c` 到 jni 目录
- 自动生成 `Android.mk`（含 libvus_rt.c 和 `-DVUS_HAVE_CURL`）
- 自动生成 `AndroidManifest.xml` 和 `Application.mk`

### 线程与协程 (v1.0-alpha) — 新增
- 关键字：`线程`、`等待线程`、`协程`、`恢复`、`让出`、`睡眠`
- 基于 POSIX 线程（pthread）的并发支持
- 基于生成器的协作式协程支持
- 全局句柄注册表管理线程/协程生命周期
- 线程/协程函数可作为表达式嵌入任何位置

### 插件运行时函数 (v1.0-beta) — 新增

**终端 UI（TUI）函数**（基于 ANSI 转义码，无外部依赖）：
| 函数 | 说明 |
|------|------|
| `tui_清屏()` | 清空终端屏幕 |
| `tui_重置()` | 重置终端属性 |
| `tui_设置颜色(前景色, 背景色)` | 设置终端颜色（ANSI 标准色） |
| `tui_定位(行, 列)` | 移动光标到指定位置 |
| `tui_进度条(当前值, 总值, 宽度)` | 显示进度条 |

**网络函数**（基于 libcurl，需 `-DVUS_HAVE_CURL -lcurl`）：
| 函数 | 说明 |
|------|------|
| `网络_GET(url)` | HTTP GET 请求 |
| `网络_POST(url, 数据)` | HTTP POST 请求 |
| `网络_下载(url, 文件路径)` | 下载文件到本地 |

**文件操作函数**（基于标准 C I/O，无外部依赖）：
| 函数 | 说明 |
|------|------|
| `文件_读取(路径)` | 读取文件全部内容 |
| `文件_写入(路径, 内容)` | 写入文件（覆盖写） |
| `文件_追加(路径, 内容)` | 追加到文件末尾 |
| `文件_存在(路径)` | 检查文件是否存在 |
| `文件_删除(路径)` | 删除文件 |
| `文件_列表(路径)` | 列出目录内容 |

**日期时间函数**（基于 `<time.h>`，无外部依赖）：
| 函数 | 说明 |
|------|------|
| `日期_现在()` | 返回 ISO 8601 格式当前时间 |
| `日期_时间戳()` | 返回 Unix 时间戳 |
| `日期_从时间戳(时间戳)` | 时间戳 → 日期字符串 |
| `日期_格式化(格式)` | 按格式格式化当前时间 |
| `日期_解析(字符串, 格式)` | 解析日期字符串 |
| `日期_年/月/日/时/分/秒()` | 获取当前时间各部分 |

---

## 二、测试状态

| 测试文件 | 覆盖功能 | 状态 |
|----------|---------|:----:|
| test_hello.vus | 基本输出 | ✅ 稳定 |
| test_variables.vus | 变量声明与赋值 | ✅ 稳定 |
| test_arithmetic.vus | 四则运算 | ✅ 稳定 |
| test_control.vus | 条件分支 | ✅ 稳定 |
| test_functions.vus | 函数定义与调用 | ✅ 稳定 |
| test_comparison.vus | 比较运算符 | ✅ 稳定 |
| test_string.vus | 字符串拼接与比较 | ✅ 稳定 |
| test_while_count.vus | 当循环 | ✅ 稳定 |
| test_factorial.vus | 递归阶乘 | ✅ 稳定 |
| test_fibonacci.vus | 递归斐波那契 | ✅ 稳定 |
| test_nested_control.vus | 嵌套循环（乘法表） | ✅ 稳定 |
| test_comprehensive.vus | 综合功能测试 | ✅ 稳定 |
| test_demo.vus | 10 模块综合演示 | ✅ 稳定 |
| test_generic.vus | 泛型函数调用 | ✅ 稳定 |
| test_generic_call.vus | 泛型调用语法 | ✅ 稳定 |
| test_struct_chain.vus | 结构体链式访问 | ✅ 稳定 |
| test_thread_coro.vus | 线程与协程 | ✅ 稳定 |
| test_plugins.vus | 插件运行时函数 | ✅ 稳定 |
| examples/hello.vus | 完整示例程序 | ✅ 稳定 |

### 测试中 / 待开发

| 功能 | 状态 | 说明 |
|------|:----:|------|
| 列表/字典操作语法层 | 🔄 待办 | 运行时库已实现，VUS 语法层绑定 |
| 错误处理语法层 | 🔄 待办 | `VusError` 运行时已实现，语法层 |
| 闭包/高阶函数 | 🔄 待办 | 运行时 `VusClosure` 已实现 |
| 位运算符 | 🔄 待办 | `&`、`|`、`^`、`~`、`<<`、`>>` |
| `!` 逻辑取反 | 🔄 待办 | 一元运算符 |
| 函数内部局部变量 | 🔄 待办 | 当前不支持函数内赋值声明 |
| Python 桥接 | 📋 规划 | 通过 C ABI + ctypes 调用 |
| 包管理/模块导入 | 📋 规划 | vus.json 项目配置 |

---

## 三、已知 Bug 和限制

### Bug
| # | 描述 | 状态 | 优先级 |
|---|------|:----:|:------:|
| 1 | 字符串 `\n` 在 `echo` 创建测试文件时被 shell 解释，非编译器 Bug | 已确认 | 低 |
| 2 | 用户环境报告"期望 缩进，但遇到 冒号"错误，开发环境无法复现 | 待排查 | 中 |
| 3 | 函数内部局部变量（赋值）不声明，仅支持全局变量 | 已知限制 | 中 |

### 限制
- **函数内部局部变量**：函数体内的赋值语句使用全局变量声明，不支持函数作用域局部变量。变通方法：将变量移到顶层作用域。
- **布尔返回**：`返回 0` 返回字符串 `"0"`，`如果` 条件检查 `"true"` 字符串。需使用 `返回 0 == 0` 获得 `"true"` 值。
- **单文件编译**：当前只支持单文件编译，不支持多文件项目和模块导入。
- **无类型检查**：VUS 是动态类型语言，所有值均为 `VusString*`，无编译时类型检查。
- **内存管理**：引用计数自动管理，但存在少量内存泄漏（临时表达式结果未释放）。
- **`vus_eval` 性能**：每次调用都会编译 C 代码并链接为可执行文件，开销较大。
- **最大插件数量**：硬限制为 64 个。

---

## 四、API 和接口

### 4.1 C ABI 接口 (`include/vus/vus_abi.h`)

```c
// ABI 版本
int         vus_abi_version(void);
const char *vus_abi_version_string(void);

// 编译 .vus 文件 → C 代码
VusResult vus_compile_file(const char *path, VusConfig *config);

// 从源码字符串编译 → C 代码
VusResult vus_compile_string(const char *source, VusConfig *config);

// 从源码编译并链接 → 可执行文件
VusResult vus_compile_string_to_exe(const char *source, VusConfig *config);

// 求值表达式，返回 stdout 输出
VusResult vus_eval(const char *code, VusConfig *config, char *output);
```

### 4.2 插件接口 (`include/vus/vus_plugin.h`)

```c
// 插件描述符
typedef struct VusPlugin {
    const char *name;         // 插件名称
    const char *version;      // 版本号
    int  (*init)(VusPluginAPI *api);     // 初始化
    int  (*run)(VusPluginAPI *api, const char *input, char **output);
    void (*cleanup)(VusPluginAPI *api);  // 清理
    const char *description;  // 描述（可选）
    const char *author;       // 作者（可选）
} VusPlugin;

// 编译器 API 表（运行时填充）
typedef struct VusPluginAPI {
    int version;
    VusResult (*compile_file)(const char *, VusConfig *);
    VusResult (*compile_string)(const char *, VusConfig *);
    VusResult (*compile_string_to_exe)(const char *, VusConfig *);
    VusResult (*eval)(const char *, VusConfig *, char *);
    const char *(*compiler_version)(void);
} VusPluginAPI;
```

### 4.3 内部编译器 API (`vus.h`)

```c
typedef struct VusConfig {
    char style[32];        // 编码风格："函数" / "易语言"
    char project_dir[1024];// 项目目录
    char rt_dir[1024];     // 运行时库目录
    char build_dir[1024];  // 构建输出目录
    char optimization[32]; // 优化级别
    // ... 其他配置字段
} VusConfig;

typedef struct VusResult {
    int   success;         // 1=成功, 0=失败
    char  c_output_path[1024];   // 生成的 C 文件路径
    char  exe_output_path[1024]; // 可执行文件路径
    char  error_msg[512];        // 错误消息
} VusResult;
```

### 4.4 Python 调用示例

```python
import ctypes
import subprocess

# 编译 VUS 文件
def compile_vus(path):
    cmd = ["./vus", "compile", path]
    result = subprocess.run(cmd, capture_output=True, text=True)
    return result.returncode == 0

# 通过 C ABI 调用（需要 libvus.so）
lib = ctypes.CDLL("./vus")
lib.vus_abi_version.restype = ctypes.c_int
print(f"ABI 版本: {lib.vus_abi_version()}")
```

---

## 五、构建和测试方法

### 5.1 编译

```bash
# 完整编译（编译器 + 运行时库）
make

# 清理并重新编译
make clean && make

# 仅编译运行时库
make libvus_rt.a
```

### 5.2 运行测试

```bash
# 运行单个测试
./vus run tests/test_hello.vus

# 运行所有测试
./vus run tests/test_demo.vus

# 批量运行测试脚本
bash tests/run_tests.sh
```

### 5.3 安装

```bash
# 一键安装
bash install.sh

# 手动安装
make install PREFIX=/usr/local
```

### 5.4 编写和运行 VUS 程序

```bash
# 创建 VUS 源文件
echo '打印("Hello, World!\n")' > hello.vus

# 编译并运行
./vus run hello.vus

# 仅编译为 C 代码
./vus compile hello.vus

# 编译为可执行文件
./vus build hello.vus
./构建/hello
```

### 5.5 项目结构

```
vus/
├── include/vus/       # 公共 API 头文件
│   ├── vus.h          # 核心类型和配置
│   ├── vus_abi.h      # C ABI 接口
│   ├── vus_plugin.h   # 插件系统接口（.vux）
│   ├── vus_lang.h     # 语言插件接口（.vulage）
│   └── vus_vusx.h     # VUS 插件接口（.vusx）
├── src/               # 编译器源码
│   ├── main.c         # CLI 入口
│   ├── lexer.c/h      # 词法分析器
│   ├── parser.c/h     # 语法分析器
│   ├── token.c/h      # Token 类型定义
│   ├── ast.c/h        # 抽象语法树
│   ├── generator.c/h  # 代码生成器
│   ├── config.c/h     # 配置加载
│   ├── vus_abi.c      # C ABI 实现
│   ├── vus_plugin.c   # .vux 插件系统实现
│   ├── vus_lang.c/h   # .vulage 语言插件系统实现
│   └── vus_vusx.c/h   # .vusx 插件系统实现
├── rt/                # 运行时库
│   ├── libvus_rt.h    # 运行时类型定义
│   └── libvus_rt.c    # 运行时实现
├── tests/             # 测试用例
│   ├── test_*.vus     # 功能测试
│   └── run_tests.sh   # 测试运行脚本
├── scripts/           # 工具脚本
│   ├── vux_plugin_manager.py  # 插件管理
│   └── vux_plugin_entry.py    # 插件基类
├── plugins/           # 插件目录
│   ├── lang/          # 语言插件（.vulage）
│   │   └── 易语言/    # 示例语言插件
│   └── func/          # 功能插件（.vux）
├── examples/          # 示例程序
├── docs/              # 文档
├── Makefile           # 构建系统
└── install.sh         # 安装脚本
```

---

## 六、版本历史

| 版本 | 日期 | 变更 |
|------|------|------|
| v0.1 | 2026-07 | 初始版本：词法分析、语法分析、代码生成、C ABI、插件系统 |
| v0.2 | 2026-07 | 调试体验优化 + 预编译包 + 安装脚本 |
| v1.0-alpha | 2026-07 | 泛型 + 结构体 + 线程/协程 + APK 打包 |
| v1.0-beta | 2026-07 | 语言核心稳定 + APK 修复优化 + TUI/网络/文件/日期 插件运行时函数 |