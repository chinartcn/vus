# VUS 项目生态说明

> 版本：v1.0-beta  
> 最后更新：2026-08-06

---

## 一、概述

VUS 是一个面向 Linux、Android Termux、嵌入式 ARM 设备的中文友好多范式编译型强类型编程语言。其生态围绕"编译到 C"这一核心设计展开，形成了从编译器内核到插件体系、从运行时库到构建工具链的完整技术栈。

```
┌─────────────────────────────────────────────────────────┐
│                    用户层（VUS 源码）                      │
│  main.vus  │  项目配置 vus.json  │  测试用例  │  插件      │
└──────────────────────┬──────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────┐
│                   编译器内核（src/）                       │
│  词法分析 → 语法分析 → AST → C 代码生成                  │
│  API 层：C ABI / 插件系统 / 语言插件 / VUSX 插件         │
└──────────────────────┬──────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────┐
│                   编译后端                               │
│  VUS → C 代码  →  GCC/Clang  →  原生可执行文件           │
│                       ↘
│                    Android NDK  →  APK 项目              │
└──────────────────────┬──────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────┐
│                   运行时支撑层                             │
│  libvus_rt（引用计数/字符串/列表/字典/闭包/错误处理）     │
│  libcurl（可选）│  POSIX 线程 │ ANSI TUI（无外部依赖）    │
└─────────────────────────────────────────────────────────┘
```

---

## 二、核心组件

### 2.1 编译器（`src/`）

编译器采用经典三段式架构，将 VUS 源码逐级降级为 C 代码：

| 组件 | 文件 | 职责 |
|------|------|------|
| 入口调度 | `main.c` | CLI 参数解析、子命令分发（run/build/test/init/vux/vusx/lang） |
| 词法分析器 | `lexer.c` / `lexer.h` | UTF-8 中文标识符支持，93 种 Token 类型，缩进敏感 INDENT/DEDENT，双语法体系 |
| Token 定义 | `token.c` / `token.h` | Token 类型枚举、字符串化、调试输出 |
| 语法分析器 | `parser.c` / `parser.h` | 递归下降解析，12 种语句，8 级表达式优先级，双语法支持 |
| 抽象语法树 | `ast.c` / `ast.h` | 27 种 AST 节点类型，创建/遍历/销毁 |
| 代码生成器 | `generator.c` / `generator.h` | 中文标识符 sanitize、引用计数插入、类型调度、泛型函数调用 |
| 配置加载 | `config.c` / `config.h` | 项目配置 vus.json 加载与解析 |

### 2.2 运行时库（`rt/`）

| 文件 | 职责 |
|------|------|
| `libvus_rt.h` | 运行时类型定义（VusString、VusList、VusDict、VusClosure、VusError、VusPlugin 等） |
| `libvus_rt.c` | 运行时实现（字符串操作、智能加法、列表/字典操作、闭包、错误处理、线程/协程句柄注册表、25+ 插件运行时函数） |

运行时库提供的能力：

- **VusString** — 引用计数字符串类型
- **VusList** — 动态数组（创建、追加、获取、长度）
- **VusDict** — 字典（创建、设置、获取、包含检查）
- **VusClosure** — 闭包支持
- **VusError** — 错误处理
- **标准库函数** — `vus_print`、`vus_input`、`vus_to_int`、`vus_to_string`、`vus_type_of`
- **线程/协程句柄注册表** — 各 64 个槽位
- **插件运行时函数** — TUI（ANSI 转义码）、网络（libcurl）、文件 I/O、日期时间

### 2.3 公共 API 头文件（`include/vus/`）

| 文件 | 内容 |
|------|------|
| `vus.h` | 核心类型 VusConfig、VusResult 定义 |
| `vus_abi.h` | C ABI 接口：vus_compile_file()、vus_compile_string()、vus_eval() 等 |
| `vus_plugin.h` | .vux 功能插件系统接口：VusPlugin 结构体、VusPluginAPI 编译器 API 表 |
| `vus_lang.h` | .vulage 语言插件系统接口：VusLangPlugin 结构体 |
| `vus_vusx.h` | .vusx VUS 插件系统接口：VusVusxPlugin 结构体 |

---

## 三、四层插件体系

VUS 定义了四层插件体系，从源码级到编译器预处理级，逐层扩展语言能力：

```
┌──────────────────────────────────────────────────────┐
│  第四层：源码 .vus                                    │
│  普通 VUS 源文件，通过 import 机制复用                  │
├──────────────────────────────────────────────────────┤
│  第三层：VUS 插件 .vusx                               │
│  用 VUS 编写的插件，编译时自动编译为 .o 并链接到主程序   │
│  包含 vusx.json 元数据描述                             │
├──────────────────────────────────────────────────────┤
│  第二层：功能插件 .vux                                │
│  用 Python 或 C 编写的运行时插件，通过 dlopen 加载      │
│  可访问编译器 API（compile_file、eval 等）              │
├──────────────────────────────────────────────────────┤
│  第一层：语言插件 .vulage                             │
│  用 Python 或 C 编写的语法预处理插件                   │
│  在词法分析之前转换源码，实现语法风格扩展               │
│  示例：易语言风格插件（.功能 → 定义 等）               │
└──────────────────────────────────────────────────────┘
```

### 3.1 源码层（.vus）

标准 VUS 源文件，支持函数风格和易语言风格两种语法体系。

### 3.2 VUS 插件（.vusx）

用 VUS 自身编写的插件，含 `vusx.json` 元数据描述文件。编译时自动走 VUS → C → .o 流水线，链接到主程序。

**CLI 命令**：`vus vusx list|info|build`

### 3.3 功能插件（.vux）

用 Python 或 C 编写的运行时插件，通过 `vus_plugin_load()` 加载 `.so` 共享库。

**插件结构**：
```c
typedef struct VusPlugin {
    const char *name;         // 插件名称
    const char *version;      // 版本号
    int  (*init)(VusPluginAPI *api);     // 初始化
    int  (*run)(VusPluginAPI *api, const char *input, char **output);
    void (*cleanup)(VusPluginAPI *api);  // 清理
    const char *description;  // 描述
    const char *author;       // 作者
} VusPlugin;
```

**编译器 API 表**（运行时注入）：
```c
typedef struct VusPluginAPI {
    int version;
    VusResult (*compile_file)(const char *, VusConfig *);
    VusResult (*compile_string)(const char *, VusConfig *);
    VusResult (*compile_string_to_exe)(const char *, VusConfig *);
    VusResult (*eval)(const char *, VusConfig *, char *);
    const char *(*compiler_version)(void);
} VusPluginAPI;
```

**CLI 命令**：`vus vux install|build|info|list|run`

### 3.4 语言插件（.vulage）

在词法分析之前对源码进行预处理，实现语法风格转换。例如将易语言风格的 `.功能` 转换为标准 VUS 的 `定义`。

**插件结构**：
```c
typedef struct VusLangPlugin {
    const char *name;
    const char *version;
    char *(*preprocess)(const char *source, size_t len);
    int  (*init)(void);
    void (*cleanup)(void);
} VusLangPlugin;
```

**CLI 命令**：`vus lang list|load|info`

---

## 四、插件运行时函数（v1.0-beta）

编译器内置了 25+ 个插件运行时函数，分为四类：

### 4.1 终端 UI（TUI）— 基于 ANSI 转义码，无外部依赖

| 函数 | 说明 |
|------|------|
| `tui_清屏()` | 清空终端屏幕 |
| `tui_重置()` | 重置终端属性 |
| `tui_设置颜色(前景色, 背景色)` | 设置终端颜色（ANSI 标准色） |
| `tui_定位(行, 列)` | 移动光标到指定位置 |
| `tui_进度条(当前值, 总值, 宽度)` | 显示进度条 |

### 4.2 网络 — 基于 libcurl（可选，需系统安装 libcurl-dev）

| 函数 | 说明 |
|------|------|
| `网络_GET(url)` | HTTP GET 请求 |
| `网络_POST(url, 数据)` | HTTP POST 请求 |
| `网络_下载(url, 文件路径)` | 下载文件到本地 |

### 4.3 文件操作 — 基于标准 C I/O，无外部依赖

| 函数 | 说明 |
|------|------|
| `文件_读取(路径)` | 读取文件全部内容 |
| `文件_写入(路径, 内容)` | 写入文件（覆盖写） |
| `文件_追加(路径, 内容)` | 追加到文件末尾 |
| `文件_存在(路径)` | 检查文件是否存在 |
| `文件_删除(路径)` | 删除文件 |
| `文件_列表(路径)` | 列出目录内容 |

### 4.4 日期时间 — 基于 `<time.h>`，无外部依赖

| 函数 | 说明 |
|------|------|
| `日期_现在()` | 返回 ISO 8601 格式当前时间 |
| `日期_时间戳()` | 返回 Unix 时间戳 |
| `日期_从时间戳(时间戳)` | 时间戳 → 日期字符串 |
| `日期_格式化(格式)` | 按格式格式化当前时间 |
| `日期_解析(字符串, 格式)` | 解析日期字符串 |
| `日期_年/月/日/时/分/秒()` | 获取当前时间各部分 |

### 4.5 Meilisearch 搜索插件（.vux 功能插件）

VUS 插件系统第一个正式功能插件，用 Python 封装 Meilisearch 全文搜索引擎，提供中文子命令接口。

**安装**：
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

源码位于 `plugins/func/meilisearch/`，单元测试位于 `plugins/func/meilisearch/tests/`。

---

## 五、构建系统

### 5.1 Makefile（`Makefile`）

主构建系统，支持：

| 目标 | 说明 |
|------|------|
| `make` / `make all` | 完整编译（编译器 + 运行时库） |
| `make clean` | 清理构建产物 |
| `make install` | 安装到系统 |
| `make libvus_rt.a` | 仅编译运行时库 |

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

自动生成内容：

- **JNI 桥接代码** — 自动转换点号为下划线
- **`main()` 替换** — 自动替换为 `vus_main()` 避免冲突
- **运行时库嵌入** — 自动复制 `libvus_rt.h`/`libvus_rt.c` 到 jni 目录
- **Android.mk** — 含 libvus_rt.c 和 `-DVUS_HAVE_CURL`
- **AndroidManifest.xml** — 基础清单文件
- **Application.mk** — NDK 编译配置

---

## 七、测试体系

### 7.1 测试用例（`tests/`）

覆盖以下功能模块：

| 测试文件 | 覆盖功能 |
|----------|---------|
| test_hello.vus | 基本输出 |
| test_variables.vus | 变量声明与赋值 |
| test_arithmetic.vus | 四则运算 |
| test_control.vus | 条件分支 |
| test_functions.vus | 函数定义与调用 |
| test_comparison.vus | 比较运算符 |
| test_string.vus | 字符串拼接与比较 |
| test_while_count.vus | 当循环 |
| test_factorial.vus | 递归阶乘 |
| test_fibonacci.vus | 递归斐波那契 |
| test_nested_control.vus | 嵌套循环 |
| test_comprehensive.vus | 综合功能测试 |
| test_demo.vus | 10 模块综合演示 |
| test_generic.vus / test_generic_call.vus | 泛型函数调用 |
| test_struct_chain.vus | 结构体链式访问 |
| test_thread_coro.vus | 线程与协程 |
| test_plugins.vus | 插件运行时函数 |

### 7.2 运行方式

```bash
./vus run tests/test_hello.vus       # 单个测试
bash tests/run_tests.sh              # 批量运行
```

---

## 八、CLI 命令参考

### 8.1 编译与运行

| 命令 | 说明 |
|------|------|
| `vus run <文件>` | 编译并运行 VUS 程序 |
| `vus run --debug <文件>` | 调试模式运行（含栈追踪和调试输出） |
| `vus build --c-only <文件>` | 仅编译为 C 代码 |
| `vus build --exe <文件>` | 编译为可执行文件 |
| `vus build --apk <文件>` | 编译为 Android APK 项目 |

### 8.2 项目管理

| 命令 | 说明 |
|------|------|
| `vus init` | 交互式项目初始化 |
| `vus test` | 自动执行测试用例 |

### 8.3 插件管理

| 命令 | 说明 |
|------|------|
| `vus lang list \| load \| info` | 语言插件管理（.vulage） |
| `vus vux install \| build \| info \| list \| run` | 功能插件管理（.vux） |
| `vus vusx list \| info \| build` | VUS 插件管理（.vusx） |

### 8.4 其他

| 命令 | 说明 |
|------|------|
| `vus --help` | 显示帮助信息 |
| `vus --version` | 显示版本信息 |

---

## 九、插件管理脚本（`scripts/`）

| 文件 | 职责 |
|------|------|
| `vux_plugin_manager.py` | .vux 插件的安装、构建、列表、信息查询管理 |
| `vux_plugin_entry.py` | 插件基类定义，供 Python 编写的插件继承 |

---

## 十、目录结构

```
vus/
├── src/                    # 编译器源码
│   ├── main.c              # CLI 入口和公共 API
│   ├── lexer.c/h           # 词法分析器
│   ├── parser.c/h          # 语法分析器
│   ├── token.c/h           # Token 类型
│   ├── ast.c/h             # 抽象语法树
│   ├── generator.c/h       # 代码生成器
│   ├── config.c/h          # 配置加载
│   ├── vus_abi.c           # C ABI 实现
│   ├── vus_plugin.c        # .vux 插件系统
│   ├── vus_lang.c/h        # .vulage 语言插件系统
│   ├── vus_vusx.c/h        # .vusx 插件系统
│   └── vus_apk.c/h         # APK 打包
├── rt/                     # 运行时库
│   ├── libvus_rt.c         # 运行时实现
│   └── libvus_rt.h         # 运行时类型定义
├── include/vus/            # 公共 API 头文件
│   ├── vus.h               # 核心类型
│   ├── vus_abi.h           # C ABI 接口
│   ├── vus_plugin.h        # 插件系统接口
│   ├── vus_lang.h          # 语言插件接口
│   └── vus_vusx.h          # VUS 插件接口
├── plugins/                # 插件目录
│   ├── lang/               # 语言插件（.vulage）
│   │   └── 易语言/         # 易语言语法风格插件
│   └── func/               # 功能插件（.vux）
│       └── 示例/           # 插件开发示例
├── tests/                  # 测试用例（18 个 .vus 文件）
├── scripts/                # 工具脚本
│   ├── build_release.sh    # 预编译包构建
│   ├── vux_plugin_manager.py
│   └── vux_plugin_entry.py
├── examples/               # 示例程序
├── docs/                   # 文档
│   ├── TUTORIAL.md         # 从零开始教程
│   ├── STATUS.md           # 状态报告（功能清单、测试状态、已知 Bug）
│   ├── COMPILER_GUIDELINES.md  # 编译器开发指南
│   └── ECOSYSTEM.md        # 本文件
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
  ├── POSIX 线程（pthread）— 必需（线程/协程支持）
  ├── libcurl（可选）— 网络插件运行时函数
  ├── GCC / Clang — 编译后端（将生成的 C 代码编译为可执行文件）
  └── Android NDK（可选）— APK 打包
```

**无外部依赖的组件**：词法分析器、语法分析器、AST、代码生成器、TUI 函数、文件操作函数、日期时间函数、运行时核心（VusString/VusList/VusDict/VusClosure/VusError）。

---

## 十二、版本演进路线

| 版本 | 核心交付 | 状态 |
|------|---------|:----:|
| v0.1 | 基础语言 + 插件体系 | ✅ 完成 |
| v0.2 | 调试体验优化 + 预编译包 + 安装脚本 | ✅ 完成 |
| v1.0-alpha | 泛型 + 结构体 + 多线程 + 异步 + APK 打包 | ✅ 完成 |
| v1.0-beta | 语言核心稳定 + APK 修复优化 + TUI/网络/文件/日期 插件 | ✅ 完成 |
| v1.0 | 正式版 + 加密/数据库/易语言风格 | 🚀 未来 |
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