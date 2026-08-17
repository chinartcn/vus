# VUS 编译器 v1.0-beta — 状态报告

> 本文反映 `/workspace/vus` 当前真实代码状态，权威依据为 `docs/LANGUAGE_REFERENCE.md`、`docs/ARCHITECTURE.md` 与 `docs/API_REFERENCE.md`。
> 说明：本文**不涉及 GUI**（`图形_*`、guilite、guilibridge、x11 等显示相关层仍处于实验/开发中，尚未完成，不做为已实现功能列出）。

## 一、已实现的功能

### 词法分析器 (Lexer) — 稳定
- UTF-8 中文标识符完整支持（ASCII、下划线、CJK 汉字及扩展区、全角字母数字，`vus_is_ident_start` / `vus_is_ident_continue`）
- 93+ 种 Token 类型（含英文函数风格与中文别名关键字、类型关键字、运算符、线程/协程关键字）
- 缩进敏感 INDENT/DEDENT 处理（空格=1 级、Tab=4 级，空行/纯注释不影响缩进栈）
- 行注释 `#` 与 `//`（`#//` 表连续多行注释）
- 字符串字面量转义：`\n` `\r` `\t` `\"` `\\` `\xHH` `\uHHHH`；不支持八进制转义
- 数字字面量：十进制整数/浮点、十六进制 `0x`、二进制 `0b`
- 单套编译器内核支持英文 + 中文函数风格；易语言风格（`.版本`/`.功能`）经 `.vulage` 语言插件在**词法前预处理**实现（核心词法器不认识点前缀关键字）

### 语法分析器 (Parser) — 稳定
- 递归下降解析，12 个语法层（可归纳为 8 组优先级）：逻辑或 → 逻辑与 → 位运算 → 比较 → 移位 → 拼接 → 加减乘除取模 → 一元 → 原子
- 语句解析：表达式语句、赋值（顶层=全局 / 函数内=局部）、`如果/否则如果/否则`、`循环…从…到`、`循环…在…`/`for … in`（foreach）、`当循环`、`定义/def` 函数（含参数默认值、泛型参数）、`返回`、`跳出/继续`、`全局` 声明、`导入/从…导入`、`尝试/捕获/抛出` 异常、`struct/结构` 结构体
- 一元运算符支持：`-`、`非`/`not`、`~`（`!` 逻辑取反**未实现**）
- 线程/协程关键字（中文函数式表达式）：`线程`、`等待线程`、`协程`、`恢复`、`让出`、`睡眠`

### 抽象语法树 (AST) — 稳定
- tagged union 设计，约 40 种节点类型（LANGUAGE_REFERENCE 列出完整分类）
- 覆盖：程序、函数定义、结构体定义、控制流、赋值/表达式语句、异常、导入、线程/协程、泛型（`type_params`/`type_args`）
- `VusAstAssign` 带 `is_local` 标记区分函数内局部变量与全局变量

### 代码生成器 (Generator) — 稳定
- 中文标识符 → C 标识符 sanitize（`_6253_5370` 风格，统一 `vus_` 前缀）
- 引用计数自动插入（赋值/循环变量/参数传递/返回处 `vus_ref`/`vus_unref`）
- 函数调用使用 GNU C 语句表达式 `({...})`，`_vus_args[0]` 作返回值槽位
- 智能 `vus_add` 类型调度（两操作数均可解析为整数 → 算术加法；否则字符串拼接）
- 完整类型转换：`vus_to_int`/`vus_to_string`/`vus_to_float`
- 泛型函数调用：语法形态可用，但类型实参**仅注入为注释，无真实实例化/类型检查**（占位实现）
- 结构体类型定义与构造函数自动生成（`gen_struct_type_def`/`gen_struct_constructor`）
- 函数内局部变量在函数顶部统一声明收集（`gen_collect_locals`），排除参数名避免重定义
- 内建函数 + 日志 + TUI/网络/文件/日期 等插件运行时函数映射

### 运行时库 (Runtime) — 稳定
- `VusString` 引用计数字符串（`data` 以 `\0` 结尾、`len` 为 UTF-8 字节长度）
- `vus_ref` / `vus_unref` 引用计数（归零仅 `free`，不递归释放内部成员）
- 标准库：`vus_print`、`vus_input`、`vus_add`、`vus_to_int`/`vus_to_string`/`vus_to_float`、`vus_typeof`
- 列表 `VusList`（动态数组，cap 倍增，type 支持严格/混合）、下标读写
- 字典 `VusDict`（链地址法哈希表，DJB2 风格，自动扩容；**无遍历接口**）
- `VusObject` 结构化容器（魔数 `VUS_OBJECT_MAGIC`，承载列表/字典/字符串）
- 闭包 `VusClosure`（运行时回调，语言层未直接暴露闭包字面量）
- 错误处理 `VusError`（错误码链，Go 风格，非 setjmp）
- 线程（pthread）与协程（`rt/vus_coro.c`，setjmp/longjmp + 汇编换栈，128KB 独立栈）句柄注册表各 64 槽
- 栈追踪（`VUS_MAX_STACK_DEPTH=256`）、分级日志（EasyLogger 惰性初始化）
- 插件运行时函数：TUI/网络/文件/日期/插件调用/JSON

### CLI 界面 — 稳定
- `vus run <文件>` — 编译并运行
- `vus run --debug <文件>` — 调试模式运行（含栈追踪、`vus_debug_enabled`）
- `vus build --c-only <文件>` — 仅编译为 C 代码
- `vus build --exe <文件>` — 编译为可执行文件
- `vus build --apk <文件> [--ndk-path <路径>] [--app-name <名称>] [--output <目录>]` — 编译为 Android APK 项目
- `vus init [--force]` — 交互式/强制项目初始化
- `vus test` — 运行 `/构建` / `测试` 下测试用例
- `vus lang list|load|info` — 语言插件管理（.vulage）
- `vus vux install|build|info|list|run` — `.vux` 功能插件管理
- `vus vusx list|info|build` — `.vusx` VUS 插件管理
- `vus update` — 预编译包版本校验与自动更新
- `vus --help / -h`、`vus --version / -v` — 帮助/版本信息

### C ABI 接口 (ABI v1.0.0)
- `vus_abi_version()` / `vus_abi_version_string()`（返回 `0x010000` / `"1.0.0"`）
- `vus_compile_file()` — 编译 .vus 文件 → C 代码
- `vus_compile_string()` — 从源码字符串编译 → C 代码
- `vus_compile_string_to_exe()` — 从源码编译并链接 → 可执行文件
- `vus_eval()` — 编译并执行代码片段，捕获 stdout（每次启动子进程，输出截断 4096 字节）
- 顶层文件级入口：`vus_compile_to_c` / `vus_compile_to_exe` / `vus_run`
- `extern "C"` 兼容 C++；Python ctypes 可调用；`VusConfig`/`VusResult` 完整

### 插件系统 — 稳定

**四层插件体系**：

| 类型 | 扩展名 | 编写语言 | 加载时机 | 状态 |
|------|--------|----------|----------|:----:|
| 源码 | `.vus` | VUS | 编译时 | ✅ 稳定 |
| VUS 插件 | `.vusx` | VUS | 编译时（自动编译 `.o` 并链接） | ✅ 新增 |
| 功能插件 | `.vux` | Python/C | 运行时 | ✅ 新增 |
| 语言插件 | `.vulage` | Python/C | 编译前预处理 | ✅ 新增 |

**`.vux` 运行时插件**：
- `VusPlugin` / `VusPluginAPI`、`vus_plugin_entry` 导出约定
- `vus_register_plugin` / `vus_plugin_load`（dlopen）/ 生命周期 `init_all`/`run_all`/`cleanup_all`/`unload_all`
- 查询与列表：`vus_plugin_find` / `count` / `list_all`（`--list-plugins`）
- Linux `.so` 与 Windows `.dll` 兼容导出宏

**`.vulage` 语言插件**：
- `VusLangPlugin`（name、version、preprocess）、`vus_lang_entry` 约定
- `vus_lang_load_from_config` 依 `vus.json` 在 `plugins/lang/<名>/<名>.vulage` 加载
- 词法前预处理（易语言 `.__init__.py` 把 `.功能→定义` 等转换）
- 上限 `VUS_MAX_LANG_PLUGINS=16`

**`.vusx` VUS 插件**：
- 编译期解析 `vusx.json` 元数据、VUS→C→`.o` 流水线、自动链接进主程序
- CLI：`vus vusx list|info|build`；上限依赖 16、导出 32

### APK 打包 — 已实现
- `vus build --apk`，有 NDK 时实际交叉编译，无 NDK 生成项目骨架提示手动构建
- NDK 探测（显式路径 → `ANDROID_NDK_HOME` → `ANDROID_HOME` → 默认路径）
- 自动生成 JNI 桥接（包名点号 → 下划线）、`main()` → `vus_main()` 替换、复制 `libvus_rt.c`
- 自动生成 `Android.mk`（`-DVUS_HAVE_CURL`）、`Application.mk`（arm64-v8a/armeabi-v7a/x86_64）、`AndroidManifest.xml`
- 包名规则：`com.vus.<应用名>` 小写化

### 线程与协程 — 已实现
- 关键字：`线程`、`等待线程`、`协程`、`恢复`、`让出`、`睡眠`
- 线程基于 POSIX pthread（`vus_thread_create/join/detach`、句柄注册表）
- 协程基于 `rt/vus_coro.c`（setjmp/longjmp + 汇编手工切换栈，**不依赖 ucontext**，可 Android/Termux 编译）
- 句柄以字符串索引（`VusString*`）返回，`VUS_MAX_HANDLES=64`
- 线程/协程函数可作为表达式嵌入任意位置
- `睡眠()` 已实现（`vus_thread_sleep`，基于 usleep），可正常链接使用

### 插件运行时函数 — 已实现

**终端 UI（TUI，基于 ANSI 转义码，无外部依赖）**：
| 函数 | 说明 |
|------|------|
| `tui_清屏()` | 清空终端屏幕 |
| `tui_重置()` | 重置终端属性 |
| `tui_设置颜色(前景色, 背景色)` | 设置 ANSI 标准色 |
| `tui_定位(行, 列)` | 移动光标 |
| `tui_进度条(当前值, 总值, 宽度)` | 显示进度条 |

**网络函数（基于 libcurl，需 `-DVUS_HAVE_CURL -lcurl`）**：
| 函数 | 说明 |
|------|------|
| `网络_GET(url)` | HTTP GET 请求 |
| `网络_POST(url, 数据)` | HTTP POST 请求 |
| `网络_下载(url, 文件路径)` | 下载文件到本地 |
| — | **未定义 `VUS_HAVE_CURL` 时返回错误消息，不发送请求** |

**文件操作函数（基于标准 C I/O + POSIX，无外部依赖）**：
| 函数 | 说明 |
|------|------|
| `文件_读取(路径)` | 读取文件全部内容 |
| `文件_写入(路径, 内容)` | 写入文件（覆盖写） |
| `文件_追加(路径, 内容)` | 追加到文件末尾 |
| `文件_存在(路径)` | 检查文件是否存在（`"true"/"false"`） |
| `文件_删除(路径)` | 删除文件 |
| `文件_列表(路径)` | 列出目录内容（每行一个文件名） |

**日期时间函数（基于 `<time.h>`，无外部依赖）**：
| 函数 | 说明 |
|------|------|
| `日期_现在()` | ISO 8601 格式当前时间 |
| `日期_时间戳()` | Unix 时间戳 |
| `日期_从时间戳(时间戳)` | 时间戳 → 日期字符串 |
| `日期_格式化(格式)` | 按 `strftime` 格式格式化当前时间 |
| `日期_解析(字符串, 格式)` | 解析日期字符串 |
| `日期_年/月/日/时/分/秒()` | 获取当前时间各部分 |

**分级日志（EasyLogger 集成）**：
| 函数 | 说明 |
|------|------|
| `日志_调试/信息/警告/错误(消息)` | 4 级输出，返回 `"0"` / `"-1"` |
| `日志_级别(级别)` | 设置过滤级别（调试/信息/警告/错误） |

**插件/JSON（受编译配置限制）**：
| 函数 | 说明 |
|------|------|
| `插件_运行(插件, 命令)` | 子进程运行 `.vux` Python 插件 |
| `插件_运行JSON(插件, 命令)` | 进程内调用返回结构化结果 |
| `JSON_解析(字符串)` / `JSON_生成(对象)` | JSON ↔ `VusObject` |
| — | 部分函数仅在 `VUS_USE_PY` 编译下可用；默认构建 JSON 解析/生成返回空、`类型` 恒返回 `"空"` |

---

## 二、测试状态

> 下表仅列出 `tests/` 目录中**实际存在**的测试用例（另含 `tests/examples/`、`tests/error_tests/` 目录与运行脚本）。

| 测试文件 | 覆盖功能 | 状态 |
|----------|---------|:----:|
| test_hello.vus | 基本输出 | ✅ 稳定 |
| test_variables.vus | 变量声明与赋值 | ✅ 稳定 |
| test_arithmetic.vus | 四则运算 | ✅ 稳定 |
| test_control.vus | 条件分支 | ✅ 稳定 |
| test_elif.vus | 否则如果 多路分支 | ✅ 稳定 |
| test_functions.vus | 函数定义与调用 | ✅ 稳定 |
| test_comparison.vus | 比较运算符 | ✅ 稳定 |
| test_concat.vus | 字符串拼接（`..` / `+` 自动拼接） | ✅ 稳定 |
| test_logical.vus | 逻辑 和/或 | ✅ 稳定 |
| test_string.vus | 字符串处理与转义 | ✅ 稳定 |
| test_null.vus | `空` / null | ✅ 稳定 |
| test_while_count.vus | 当循环 | ✅ 稳定 |
| test_factorial.vus | 递归阶乘 | ✅ 稳定 |
| test_fibonacci.vus | 递归斐波那契 | ✅ 稳定 |
| test_recursion.vus | 递归 | ✅ 稳定 |
| test_nested_control.vus | 嵌套循环（乘法表） | ✅ 稳定 |
| test_nested_control_2.vus | 嵌套控制流 | ✅ 稳定 |
| test_break_continue.vus | `跳出`/`继续` | ✅ 稳定 |
| test_expr_stmt.vus | 表达式语句 | ✅ 稳定 |
| test_modulo.vus | 取余 `%` | 新增 |
| test_bitwise.vus | 位运算 `& \| ^ ~ << >>` | 新增 |
| test_negative.vus | 负数 / 一元负号 | 新增 |
| test_not.vus / test_not_operator.vus | 逻辑非 `非`/`not` | 新增 |
| test_cast.vus | 类型转换 `转数字`/`转文本` | 新增 |
| test_type_annot.vus | 类型注解 | 新增 |
| test_global.vus | 全局变量 `全局` 声明 | 新增 |
| test_local_vars.vus | 函数内局部变量 | 新增 |
| test_param_shadow.vus | 参数与局部变量同名回归 | 新增/回归 |
| test_error.vus | 错误处理 | 新增 |
| test_exception.vus | `尝试/捕获/抛出` 异常 | 新增 |
| test_import.vus | `导入`/`从…导入` | 新增 |
| test_literal.vus | 字面量（列表/字典） | 新增 |
| test_generic.vus | 泛型函数调用 | ✅ 稳定 |
| test_generic_call.vus | 泛型调用语法 | ✅ 稳定 |
| test_struct_basic.vus | 结构体基本功能 | 新增 |
| test_struct_chain.vus | 结构体链式访问 | 新增 |
| test_subscript.vus | 列表下标访问 | 新增 |
| test_thread_coro.vus | 线程与协程 | ✅ 稳定 |
| test_logger.vus | 分级日志 | 新增 |
| test_plugins.vus | 插件运行时函数 | ✅ 稳定 |
| test_plugin_run_json.vus | 插件 JSON 调用 | 新增 |
| test_plugin_inproc.c | 进程内插件调用（C 侧） | 新增 |
| test_comprehensive.vus | 综合功能测试 | ✅ 稳定 |
| test_demo.vus | 多模块综合演示 | ✅ 稳定 |
| examples/test_basic.vus / examples/hello.vus | 示例程序 | ✅ 稳定 |
| error_tests/*（7 个） | 错误路径（缺冒号/未闭合字符串/列表/泛型、结构体无名、括号不匹配等） | ✅ 稳定 |

> 说明：`tests/` 下另有 `run_tests.sh`、`run_error_tests.sh` 批量脚本，`vus test` 与 `make test` 驱动。`test_gui.vus` 仅作 GUI 方向**实验性验证**，GUI 未完成，不计入已支持功能。

### 测试中 / 待开发

| 功能 | 状态 | 说明 |
|------|:----:|------|
| 泛型真实语义 | ⚠️ 占位 | 语法形态可用，代码生成仅注入注释，无实例化/类型检查 |
| 闭包/高阶函数（语言层） | ⚠️ 未实现 | 运行时 `VusClosure` 已有，语言层未提供闭包字面量/把函数当值传 |
| 字典遍历 / 字典下标 | ⚠️ 未完整 | `VusDict` 无遍历接口；字典值访问依赖列表语义下标，无专门读写函数 |
| 异步 `等待`（await） | ❌ 未实现 | 仅定义 token，解析器不消费 |
| `!` 逻辑取反 | ❌ 未实现 | 一元运算符仅支持 `-`、`非`/`not`、`~` |
| 手动内存管理 | ❌ 不存在 | 仅自动引用计数，无手动申请/释放 API |
| 类型注解强校验 | ⚠️ 未实现 | 类型注解仅 AST 记录/注释，不强制检查，动态类型 |
| 旧式标准库函数 | ❌ 未实现 | `断言`/`退出`/`长度`/`拼接`/`分割`/`替换`/`取子串`/`取整`/`遍历列表`/`创建字典`/`字典设值`/`读取文件` 等 generator 无接线 |
| 包管理 | 📋 规划 | vus.json 项目配置已解析，模块导入已实现，完整包管理待完善 |
| Python 完整桥接 | ⚠️ 部分 | 子进程调用 `.vux` 已实现；进程内嵌入需 `VUS_USE_PY` |

---

## 三、已知 Bug 和限制

### Bug

| # | 描述 | 状态 | 优先级 |
|---|------|:----:|:------:|
| 1 | 异步 `等待`（AWAIT）仅为已定义 token，解析器未实现 | 确认 | 中 |
| 2 | 函数内局部变量采用「函数顶部收集声明」（`gen_collect_locals`），非完整词法作用域；对未显式声明的嵌套赋值等场景存在局限 | 已知限制 | 中 |
| 3 | 引用计数 `vus_unref` 归零后仅 `free()`，**不递归释放容器内部成员**（`VusList`/`VusDict` 嵌套场景需调用方保证释放），存在潜在内存泄漏 | 已知限制 | 中 |
| 4 | 异步 `等待`（`VUS_TOKEN_CN_AWAIT`）只定义了 token，解析器/生成器**未实现** | 未实现 | 中 |
| 5 | `抛出的异常类型`目前即「抛出 的消息字符串」，`捕获 <名称>` 为字符串比对，无内建异常类型系统 | 已知限制 | 低 |

### 限制
- **函数局部变量作用域**：局部变量在函数顶部统一声明（顶部收集式），未实现完整词法作用域/块级作用域；顶部变量为全局变量，函数内修改全局变量须 `全局 名称` 声明。
- **布尔表示**：布尔值是运行时字符串 `"true"`/`"false"`，条件判定依赖 `strcmp`/`vus_to_int`；`返回 0` 返回字符串 `"0"`，需 `返回 0 == 0` 才得 `"true"`。逻辑运算与非布尔上下文混用时语义可能偏离类型系统预期。
- **泛型**：语法可用但无真实泛型语义（类型实参仅注释注入）。
- **类型系统**：VUS 是动态类型语言，标量均以 `VusString*` 承载，类型注解仅记录不强制，无编译期类型检查。
- **内存管理**：引用计数自动管理，但 `vus_unref` 只 `free` 不递归，结构化容器组合使用存在内存泄漏风险。
- **`vus_eval` 开销**：每次调用都编译 C 代码并启动子进程，且输出截断为 4096 字节，不适合高频/大数据量求值。
- **字典**：无遍历接口（`VusDict` 无迭代 API），JSON 生成对字典仅返回占位 `{}`。
- **句柄表上限**：线程/协程句柄注册表各 `VUS_MAX_HANDLES=64` 槽，溢出返回 `"-1"` 并回收；长期大量创建线程/协程可能触发上限。
- **网络依赖可选**：`网络_*` 依赖 `VUS_HAVE_CURL`（需 `-lcurl`），未启用时返回空串/错误，不发送请求。
- **进程内 Python 依赖编译配置**：`插件_运行JSON`、`JSON_解析/生成`、`类型` 需 `VUS_USE_PY`（存在 libpython）编译；未定义时 JSON 返回空、`类型` 恒返回 `"空"`，相关函数回退子进程方案。
- **最大插件数量**：`.vux` 插件硬限制 `VUS_MAX_PLUGINS=64`；语言插件限制 `VUS_MAX_LANG_PLUGINS=16`。
- **单文件/模块**：核心流水线面向单文件编译；`导入`/`从…导入` 与 `.vusx` 提供模块复用，完整多文件项目/包管理仍待完善。

---

## 四、API 和接口

### 4.1 C ABI 接口 (`include/vus/vus_abi.h`) — 版本 1.0.0

```c
// ABI 版本（返回 0x010000）
int         vus_abi_version(void);
const char *vus_abi_version_string(void);   // "1.0.0"

// 编译 .vus 文件 → C 代码
VusResult vus_compile_file(const char *path, VusConfig *config);

// 从源码字符串编译 → C 代码
VusResult vus_compile_string(const char *source, VusConfig *config);

// 从源码编译并链接 → 可执行文件
VusResult vus_compile_string_to_exe(const char *source, VusConfig *config);

// 求值代码片段，返回 stdout 输出（输出截断 4096 字节）
VusResult vus_eval(const char *code, VusConfig *config, char *output);
```

顶层文件级入口（`include/vus/vus.h`）：`vus_compile_to_c` / `vus_compile_to_exe` / `vus_run`。

### 4.2 插件接口 (`include/vus/vus_plugin.h`)

```c
// 必须且仅导出一个符号
VUS_PLUGIN_EXPORT void vus_plugin_entry(VusPlugin **plugin);

typedef struct VusPlugin {
    const char *name;                  // 插件名称
    const char *version;               // 版本号
    int  (*init)(VusPluginAPI *api);   // 初始化
    int  (*run)(VusPluginAPI *api, const char *input, char **output);
    void (*cleanup)(VusPluginAPI *api); // 清理
    const char *description;           // 描述（可选）
    const char *author;                // 作者（可选）
} VusPlugin;

#define VUS_MAX_PLUGINS 64

int  vus_register_plugin(VusPlugin *plugin);
int  vus_plugin_load(const char *path);
int  vus_plugin_init_all(void);
int  vus_plugin_run_all(const char *input, char **output);
void vus_plugin_cleanup_all(void);
vus_plugin_find / vus_plugin_count / vus_plugin_list_all / vus_plugin_unload_all;
```

`.vulage` 语言插件（`include/vus/vus_lang.h`）：`vus_lang_register/load/find/preprocess/…`，上限 `VUS_MAX_LANG_PLUGINS=16`。
`.vusx` 插件（`include/vus/vus_vusx.h`）：`vus_vusx_resolve/compile/resolve_all/compile_all/cleanup_all`，依赖上限 16、导出上限 32。

### 4.3 核心配置与结果类型

```c
typedef struct VusConfig {
    char project_dir[1024];      // 项目根目录
    char name[256];              // 项目名称
    char version[64];            // 项目版本
    char style[32];              // 语法风格（"函数"/"易语言"）
    char language_plugin[64];    // 语言插件名（易语言等，空=核心语法）
    char vusx_deps[VUS_CONFIG_MAX_VUSX_DEPS][256]; // vusx 依赖（最多 16）
    int  vusx_deps_count;
    char main_file[256];         // 主文件（默认 main.vus）
    char output_mode[16];        // "c" / "exe"
    char list_mode[16];          // "严格"/"混合"（字段存在，行为未消费）
    int  debug;                  // 0/1
    char target_platform[32];    // linux-gnu / linux-musl / android
    char rt_dir[1024];           // 运行时目录
    char build_dir[1024];        // 构建目录（默认 构建）
    char optimization[16];       // 速度/体积/调试
    char arm_version[16];        // ARM64/ARM32
} VusConfig;

typedef struct VusResult {
    int   success;               // 1=成功, 0=失败
    char  error_msg[512];
    char  c_output_path[1024];
    char  exe_output_path[1024];
} VusResult;
```

### 4.4 Python 调用示例（ctypes）

```python
import ctypes

class VusResult(ctypes.Structure):
    _fields_ = [
        ("success", ctypes.c_int),
        ("error_msg", ctypes.c_char * 512),
        ("c_output_path", ctypes.c_char * 1024),
        ("exe_output_path", ctypes.c_char * 1024),
    ]

lib = ctypes.CDLL("./libvus.so")
lib.vus_abi_version.restype = ctypes.c_int
print("ABI 版本:", hex(lib.vus_abi_version()))
```

---

## 五、构建和测试方法

### 5.1 编译

```bash
# 完整编译（编译器 + 运行时库 libvus_rt.a）
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
make test            # 调用 ./vus test
make run-tests       # bash tests/run_tests.sh
bash tests/run_error_tests.sh   # 错误路径测试
```

### 5.3 条件编译开关

```bash
# 网络插件（libcurl）：需系统安装 libcurl-dev
#   VUS_HAVE_CURL + -lcurl
# 进程内 Python 嵌入（默认在 python3-config 可用时自动启用 VUS_USE_PY）
#   无 python3-config / 无 libpython 时回退子进程方案
```

### 5.4 编写和运行 VUS 程序

```bash
# 创建 VUS 源文件
echo '打印("Hello, World!\n")' > hello.vus

# 编译并运行
./vus run hello.vus

# 仅编译为 C 代码
./vus build --c-only hello.vus

# 编译为可执行文件
./vus build --exe hello.vus
./构建/hello
```

### 5.5 项目结构

```
vus/
├── include/vus/       # 公共 API 头文件
│   ├── vus.h          # 核心类型（VusResult）与顶层编译入口
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
│   ├── vus_vusx.c/h   # .vusx 插件系统实现
│   └── vus_apk.c/h    # APK 打包
├── rt/                # 运行时库
│   ├── libvus_rt.h    # 运行时类型定义
│   ├── libvus_rt.c    # 运行时实现
│   ├── vus_coro.c/h   # 协程实现
│   └── easylogger/    # EasyLogger（分级日志）
├── tests/             # 测试用例（test_*.vus、error_tests/、run_tests.sh、vus.log）
├── scripts/           # 工具脚本（vux_plugin_manager.py 等）
├── plugins/           # 插件目录（lang/易语言 语言插件示例、func/）
├── examples/          # 示例程序
├── docs/              # 文档
└── Makefile / install.sh / vus.json
```

---

## 六、版本历史

| 版本 | 日期 | 变更 |
|------|------|------|
| v0.1 | 2026-07 | 初始版本：词法/语法分析、AST、C 代码生成、C ABI、插件系统 |
| v0.2 | 2026-07 | 调试体验优化 + 预编译包 + 安装脚本 |
| v1.0-alpha | 2026-07 | 泛型（语法占位）+ 结构体 + 线程/协程 + APK 打包（含 v1.0-alpha 规划 Issue 与功能落地） |
| v1.0-beta | 2026-07 | 语言核心稳定 + APK 修复优化 + TUI/网络/文件/日期 等插件运行时函数；本地变量收集/AST 释放等修复 |
| 后续提交 | 2026-07~08 | ORM/集成经验脚本、AST 字面量节点内存泄漏修复（列表/字典回归）、协程 Clang/ARM64 编译兼容性修复、显示相关实验（X11/GuiLite 方向探针、坐标翻转、x11_read 等，GUI 尚未完成，不属于已支持功能） |

---

*本文档为只读状态报告，未改动任何源码。如代码演进需同步更新，请以 `docs/LANGUAGE_REFERENCE.md`、`docs/ARCHITECTURE.md`、`docs/API_REFERENCE.md` 与 `src/`、`rt/` 为准。*