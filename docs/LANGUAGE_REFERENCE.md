> 文档版本：v1.0_apk（APK 功能时代）
> 最后更新时间：2026-09-04


# VUS 语言参考手册

> **版本**：对应 v1.0-alpha/beta 代码（`src`、`rt` 目录现状）
> **范围**：基于 VUS 真实源码（`token.h/c`、`lexer.c/h`、`parser.c/h`、`ast.h`、`generator.c`、`rt/libvus_rt.h/c`、`config.c/h`、`main.c`）撰写。
> **说明**：本文只描述**已实际落地在代码中**的能力；凡属设计文档（`设计文档.md`）但尚未在代码中实现的功能，均明确标注为「尚未实现 / 待办」，绝不写成已支持。
> **约定**：本文覆盖**已投入生产**的语言能力（含 **GUI**：`图形_*` 画布流与 `界面_*` VUA 组件流）；早期 X11 探针时代"不涉及 GUI"的约定已废止。GUI 细节见 [API_REFERENCE.md](API_REFERENCE.md) 与 [VUA_REFERENCE.md](VUA_REFERENCE.md)。

---

## 目录

1. [简介](#1-简介)
2. [项目风格与 vus.json 配置](#2-项目风格与-vusjson-配置)
3. [词法规则](#3-词法规则)
4. [关键字完整清单](#4-关键字完整清单)
5. [运算符表与优先级](#5-运算符表与优先级)
6. [数据类型与值语义](#6-数据类型与值语义)
7. [语句语法](#7-语句语法)
8. [函数](#8-函数)
9. [结构体](#9-结构体)
10. [线程与协程](#10-线程与协程)
11. [内置函数与插件运行时函数](#11-内置函数与插件运行时函数)
12. [错误信息与调试](#12-错误信息与调试)
13. [附录：完整语法速查（EBNF / 伪语法）](#13-附录完整语法速查ebnf--伪语法)

---

## 1. 简介

### 1.1 定位

VUS 是一款面向 **Linux / Android Termux / 嵌入式 ARM** 的中文友好多范式**编译型**编程语言。VUS 源码经词法分析 → 语法分析 → AST → **C 代码生成**后，交由 **GCC/Clang** 编译成原生可执行文件，最终在目标平台上直接运行。

- **源码扩展名**：`.vus`
- **编译产物**：`xxx.c`（C 源码）→ 可执行文件
- **运行时**：纯 C 编写的 `libvus_rt` 运行时库，负责字符串/列表/字典等对象与引用计数管理
- **重定位**：VUS 是「C 与 Python 之间的桥梁」，让不懂英文的中文用户也能写代码，同时保留底层控制能力。

### 1.2 双语法体系

VUS 提供两套语法风格，通过项目级 `vus.json` 的 `风格` 字段锁定：

| 风格 | 说明 | 关键字形态 |
|------|------|-----------|
| **函数风格** | 关键字提供英文 **与** 中文别名，可混用或全中文 | `def`/`定义`、`if`/`如果`、`打印()` |
| **易语言风格** | 关键字带 `.` 前缀，如 `.功能`、`.打印`、`.结束` | 通过**语言插件**（`.vulage`）在词法分析**之前**预处理实现 |

> **关键事实（易语言风格实现方式）**：VUS **核心词法/语法分析器不认识 `.功能` 等点前缀关键字**（`token.h` 中并不存在这些 token）。易语言风格是通过位于 `plugins/lang/易语言/__init__.py` 的**语言插件**，在编译前调用 `vus_lang_preprocess()` 把 `.功能`→`定义`、`.如果`→`如果`、`.返回`→`返回`、`.结束`→删除等，转换成标准函数风格源码后，再进入正常编译流水线。也就是说：**易语言风格是「编译前预处理」层，而非核心语法层**。核心语法始终是函数风格（中英别名）。
>
> - 引擎在 `main.c` 中：若 `vus.json` 配置了 `"语言插件": "易语言"`，则先加载并预处理源码（`vus lang load` 可加载语言插件）。
> - 若 `风格` 配置为 `"易语言"`，但未安装/加载对应语言插件，则该源码无法被核心词法器解析。

### 1.3 编译流水线

```
.vus 源码（UTF-8）
    ↓ ① 可选：语言插件预处理（如易语言 → 函数风格）
    ↓ ② 词法分析 Lexer   → Token 流（含行号/列号、INDENT/DEDENT）
    ↓ ③ 语法分析 Parser  → AST（抽象语法树，递归下降）
    ↓ ④ 代码生成 Generator → C 源码（引用计数插入、错误链、栈追踪）
    ↓ ⑤ GCC/Clang 编译   → 原生可执行文件
```

编译命令（`main.c` 实现）：

| 命令 | 说明 |
|------|------|
| `vus run <文件>` | 编译并运行 |
| `vus run --debug <文件>` | 以调试模式编译并运行（开启 `vus_debug_enabled`）|
| `vus build --c-only <文件>` | 仅生成 C 源码 |
| `vus build --exe <文件>` | 生成 C 源码并调用 GCC 生成可执行文件 |
| `vus build --apk <文件>` | 生成 Android APK 项目（见 `vus_apk`）|
| `vus init [--force]` | 交互式初始化项目 / 强制重建配置 |
| `vus test` | 运行 `/构建` 下测试（详见下文第 12 章）|
| `vus lang list/load/info` | 语言插件管理 |
| `vus vux install/build/info/list/run` | `.vux` 功能插件管理 |
| `vus vusx list/info/build` | `.vusx` VUS 插件管理 |
| `vus --version / -v` | 版本信息 |
| `vus --help / -h` | 帮助 |

> 配置从项目根目录 `vus.json` 读取（见第 2 章）。`build_dir` 默认 `构建/`；编译输出 `${构建}/<源文件名>.c` 与同名可执行文件。

---

## 2. 项目风格与 vus.json 配置

`vus.json` 位于项目根目录，由 `config.c` 加载，字段映射到 `VusConfig` 结构体：

```json
{
    "风格": "函数",
    "语言插件": "",
    "vusx依赖": [],
    "运行时目录": "rt",
    "构建目录": "构建",
    "优化": "速度"
}
```

实际 `VusConfig` 支持字段（`config.h`）：

| 配置项 | `VusConfig` 字段 | 说明 |
|--------|------------------|------|
| `风格` | `style` | `"函数"` 或 `"易语言"`；易语言需配 `语言插件` |
| `语言插件` | `language_plugin` | 语言插件名，如 `"易语言"`；空 = 核心语法 |
| `vusx依赖` | `vusx_deps[]` | `.vusx` 插件目录路径（最多 16 个）|
| `运行时目录` | `rt_dir` | 运行时库目录 |
| `构建目录` | `build_dir` | 编译输出目录（默认 `构建`）|
| `优化` | `optimization` | `"速度"`/<`"体积"`>/`"调试"`（映射 GCC `-O2`/`-Os`/`-O0 -g`）|
| `名称` / `版本` | `name` / `version` | 项目元数据 |
| `主文件` | `main_file` | 主文件路径 |
| `输出模式` | `output_mode` | `"c"` 或 `"exe"` |
| `列表模式` | `list_mode` | `"严格"`/`"混合"`（解码字段存在，但见下）|
| `调试` | `debug` | 0/1，控制 `vus_debug_enabled`、非 `断言` |
| `目标平台` | `target_platform` | `"linux-gnu"`/`"linux-musl"`/`"android"` |
| `ARM版本` | `arm_version` | `"ARM64"`/`"ARM32"` |
| `省略主函数` | `omit_main` | 0/1；为 1 时**不生成** `main`（`.vusx` 插件等库式编译，避免与宿主程序 `main` 冲突）|

> **特别注意（实际实现与设计模式的差异）**：设计文档中的 `断言(条件, 消息)`、`"列表模式"` 宏注入（`VUS_LIST_MODE_STRICT` / `VUS_LIST_MODE_MIXED`）等字段/机制，在**当前代码生成器中并未落地**（见第 11、6 章）。`列表`、`列表模式`、`主文件`、`输出模式` 等字段已解析到结构体，但**尚未在代码生成路径中被真正消费**，标注为「字段已定义、行为未实现」。请勿按设计文档假设其已生效。

---

## 3. 词法规则

词法规则由 `lexer.c/h` 与 `token.c/h` 实现。

### 3.1 缩进与换行（INDENT / DEDENT）

- 源码以**缩进**表示代码块层级，类似 Python，使用一个缩进栈。
- 缩进计量：**空格 = 1 级，制表符 Tab = 4 级**（`lexer_handle_indent`）。
- 缩进栈初始为 `[0]`：
  - 缩进增加 → 发出 `INDENT`；
  - 缩进减少 → 按减少的量发出一个或多个 `DEDENT`；
  - 行结束 → 发出 `NEWLINE`。
- **空行与纯注释行不产生 `NEWLINE`，不影响缩进栈**。
- 同一代码块缩进必须一致；建议统一使用 4 空格、不要混用 Tab 与空格。
- 块必须正确闭合（`DEDENT`）；`if`/`for`/`while`/`try`/`def`/`struct` 等要求缩进体与结束缩进（`parser.expect(INDENT)` / `expect(DEDENT)`）。

### 3.2 注释

- **行注释**：`#` 或 `//` 开头到行尾。
  ```vus
  # 这是行注释
  // 这也是行注释
  打印("hi")  # 行尾注释
  ```
- **多行注释**：当前语言**没有块注释 `/* */`**；「多行注释」用连续多行 `#`/`//` 表达（教程所称多行注释即逐行 `#`）。
  ```vus
  #// 这是多行说明
  #// 可以写多行
  ```
- 设计文档提到的 `# 行尾注释` 与 `#//` 风格在词法层都只是 `#` 开始的整行注释，`//` 也在词法层被等同处理（`lexer_skip_comment` 起始符为 `#` 或 `//`）。

### 3.3 字符串字面量与转义

双引号字符串字面量（`lexer_read_string`）支持转义序列：

| 转义 | 含义 |
|------|------|
| `\n` | 换行 LF |
| `\r` | 回车 CR |
| `\t` | 制表符 |
| `\"` | 双引号 |
| `\\` | 反斜杠 |
| `\xHH` | 十六进制字节（两位，如 `\xE4`）；非法时词法报错「`\x` 后需要两位十六进制数字」|
| `\uHHHH` | 四位十六进制 Unicode 码点（如 `\u4F60` = "你"）；非法时词法报错；超出基本多文种平面（BMP）时报错 |

- **不支持八进制转义**（`\0`、`\123` 等）。
- **未认识转义序列按原样保留**（词法器不报错，保留 `\x` 字面）。
- 字符串内不允许出现未转义的换行（读到行尾换行即视作字符串结束/异常）。
- 源码必须为 **UTF-8**。

示例：
```vus
打印("你好\n")
msg = "引号：\" 反斜杠：\\ 十六进制：\xE4\xB8\xAD Unicode：\u4F60"
```

### 3.4 数字字面量

- 数字字面量被解释为十进制数（`parse_primary` 依据是否含 `.` 判 `is_float`）。
- 数值运算在运行时统一按 **64 位整数（`vus_to_int`）** 处理（见第 5 章）。
- 数字不能以「`.x` 且无整数部分」之外的写法触发歧义；标准写法为 `10`、`3.14`。

### 3.5 标识符（UTF-8 中文）

- 标识符起始字符（`vus_is_ident_start`）：ASCII 字母 `a-zA-Z`、下划线 `_`、以及**中文**（CJK 汉字，含扩展区 A/B、部首补充、兼容汉字、全角字母/数字等，见 `vus_is_chinese_id_char`）。
- 标识符延续字符（`vus_is_ident_continue`）：起始字符外加数字 `0-9`。
- **不能以数字开头**。
- 中文、英文、数字、下划线可混合；变量名**禁止与关键字同名**（如 `真`、`假`、`空`、`定义`、`如果` 等）。
- UTF-8 多字节字符按码点解码后分类（`vus_utf8_decode`）。

```vus
姓名 = "张三"     # 中文标识符
_value1 = "x"    # 下划线开头
2号 = "非法"      # ← 不能以数字开头
```

---

## 4. 关键字完整清单

`token.h` 定义了全部关键字 Token（英文、中文别名、以及类型关键字），`token.c` 的 `s_keywords[]` 表是可搜索的完整清单。以下**逐一列出**。

### 4.1 函数风格（英文关键字）

| 英文关键字 | Token | 用途 |
|-----------|-------|------|
| `def` | `VUS_TOKEN_DEF` | 定义函数 |
| `if` | `VUS_TOKEN_IF` | 条件判断 |
| `elif` | `VUS_TOKEN_ELIF` | 否则如果 |
| `else` | `VUS_TOKEN_ELSE` | 否则 |
| `for` | `VUS_TOKEN_FOR` | for 循环 / 遍历 |
| `while` | `VUS_TOKEN_WHILE` | while 循环 |
| `return` | `VUS_TOKEN_RETURN` | 返回 |
| `import` | `VUS_TOKEN_IMPORT` | 导入 |
| `from` | `VUS_TOKEN_FROM` | 从…导入 |
| `true` | `VUS_TOKEN_TRUE` | 真 |
| `false` | `VUS_TOKEN_FALSE` | 假 |
| `null` | `VUS_TOKEN_NULL` | 空 |
| `and` | `VUS_TOKEN_AND` | 逻辑与 |
| `or` | `VUS_TOKEN_OR` | 逻辑或 |
| `not` | `VUS_TOKEN_NOT` | 逻辑非（一元）|
| `try` | `VUS_TOKEN_TRY` | 尝试 |
| `except` | `VUS_TOKEN_EXCEPT` | 捕获 |
| `global` | `VUS_TOKEN_GLOBAL` | 全局变量声明 |
| `break` | `VUS_TOKEN_BREAK` | 跳出 |
| `continue` | `VUS_TOKEN_CONTINUE` | 继续 |
| `throw` | `VUS_TOKEN_THROW` | 抛出异常 |
| `in` | `VUS_TOKEN_IN` | for…in 遍历 |
| `struct` | `VUS_TOKEN_STRUCT` | 定义结构体 |

### 4.2 中文别名关键字（函数风格）

| 中文别名 | Token | 对应英文 |
|---------|-------|---------|
| `定义` | `VUS_TOKEN_CN_DEF` | `def` |
| `如果` | `VUS_TOKEN_CN_IF` | `if` |
| `否则如果` | `VUS_TOKEN_CN_ELIF` | `elif` |
| `否则` | `VUS_TOKEN_CN_ELSE` | `else` |
| `循环` | `VUS_TOKEN_CN_FOR` | `for` |
| `当循环` | `VUS_TOKEN_CN_WHILE` | `while` |
| `返回` | `VUS_TOKEN_CN_RETURN` | `return` |
| `导入` | `VUS_TOKEN_CN_IMPORT` | `import` |
| `从` | `VUS_TOKEN_CN_FROM` | `from` |
| `到` | `VUS_TOKEN_CN_TO` | （`循环 i 从 a 到 b` 中的分隔）|
| `真` | `VUS_TOKEN_CN_TRUE` | `true` |
| `假` | `VUS_TOKEN_CN_FALSE` | `false` |
| `空` | `VUS_TOKEN_CN_NULL` | `null` |
| `和` | `VUS_TOKEN_CN_AND` | `and` |
| `或` | `VUS_TOKEN_CN_OR` | `or` |
| `非` | `VUS_TOKEN_CN_NOT` | `not` |
| `尝试` | `VUS_TOKEN_CN_TRY` | `try` |
| `捕获` | `VUS_TOKEN_CN_EXCEPT` | `except` |
| `全局` | `VUS_TOKEN_CN_GLOBAL` | `global` |
| `跳出` | `VUS_TOKEN_CN_BREAK` | `break` |
| `继续` | `VUS_TOKEN_CN_CONTINUE` | `continue` |
| `抛出` | `VUS_TOKEN_CN_THROW` | `throw` |
| `在` | `VUS_TOKEN_CN_IN` | `in`（`循环 x 在 列表`）|
| `结构` | `VUS_TOKEN_CN_STRUCT` | `struct` |

### 4.3 类型关键字（类型注解）

| 关键字 | Token | 含义 |
|--------|-------|------|
| `int` / `整型` | `VUS_TOKEN_TYPE_INT` | 整型 |
| `float` / `浮点型` | `VUS_TOKEN_TYPE_FLOAT` | 浮点型 |
| `str` / `字符串` | `VUS_TOKEN_TYPE_STR` | 字符串 |
| `bool` / `布尔型` | `VUS_TOKEN_TYPE_BOOL` | 布尔型 |
| `list` / `列表` | `VUS_TOKEN_TYPE_LIST` | 列表 |
| `dict` / `字典` | `VUS_TOKEN_TYPE_DICT` | 字典 |

这些用在**类型注解**（`变量: 类型 = 值`、参数注解、结构体字段注解）以及**泛型参数**（`函数名<类型>()`）中。

> **注意（类型注解实际行为）**：类型注解在 AST 中被记录（`type_annotation`、`type_params`），但在当前代码生成器中**不执行强类型检查**，类型注解主要作为**代码/注释或类型信息**保留，不真正约束赋值。运行时所有标量值均为字符串（见第 6 章），因此**目前不存在编译期强制类型报错**。这与设计文档的「强类型」存在差距，属「尚未完全实现」项。

### 4.4 线程 / 协程关键字（仅中文，作表达式/函数调用形式）

| 关键字 | Token | 用法 |
|--------|-------|------|
| `线程` | `VUS_TOKEN_CN_THREAD` | `线程(函数, 实参)` 创建并启动线程 |
| `等待线程` | `VUS_TOKEN_CN_JOIN_THREAD` | `等待线程(线程句柄)` 等待线程并取返回值 |
| `睡眠` | `VUS_TOKEN_CN_THREAD_SLEEP` | `睡眠(毫秒)` 休眠 |
| `协程` | `VUS_TOKEN_CN_COROUTINE` | `协程(函数, 实参)` 创建协程 |
| `恢复` | `VUS_TOKEN_CN_RESUME` | `恢复(协程句柄)` 恢复协程 |
| `让出` | `VUS_TOKEN_CN_YIELD` | `让出()` 协程让出 |
| `等待` | `VUS_TOKEN_CN_AWAIT` | 保留关键字（当前**未在解析器中消费**，见后）|

> `等待`（AWAIT）是 token 已定义的关键字，但 **`parser.c` 并未实现它**（仅出现在 `token` 名称表）。因此「异步等待语法」**尚未实现**。

### 4.5 分隔符与标点 Token

| Token | 符号 | 说明 |
|-------|------|------|
| `VUS_TOKEN_LPAREN`/`RPAREN` | `(` `)` | 括号 |
| `VUS_TOKEN_LBRACKET`/`RBRACKET` | `[` `]` | 下标 / 列表字面量 |
| `VUS_TOKEN_LBRACE`/`RBRACE` | `{` `}` | 字典字面量 |
| `VUS_TOKEN_COMMA` | `,` | 逗号 |
| `VUS_TOKEN_COLON` | `:` | 冒号（块头、类型注解）|
| `VUS_TOKEN_DOT` | `.` | 点号（成员访问）|

另：`NEWLINE`、`INDENT`、`DEDENT`、`EOF`、`ERROR` 为特殊 Token。

> 中文空格规则（设计文档 E015）：**当前核心词法器并未强制实现**「中文关键字与标识符之间必须有空格」的 E015 检查。请勿把 E015 视为已生效规则。

---

## 5. 运算符表与优先级

### 5.1 运算符总表

| 类别 | 运算符 | 语义 | 约束 / 行为 |
|------|--------|------|-------------|
| 算术 | `+` | 加 / 自动拼接 | 两操作数都可解析为整数 → 算术加法；否则**字符串拼接**（`vus_add`）|
| 算术 | `-` | 减 | 按 `vus_to_int` 整数运算 |
| 算术 | `*` | 乘 | 整数运算 |
| 算术 | `/` | 除 | **整数除法**（截断）|
| 算术 | `%` | 取余 | 整数运算 |
| 比较 | `==` `!=` `<` `>` `<=` `>=` | 比较 | 按 `vus_to_int` 数值比较，结果返回 `"true"`/`"false"` 字符串 |
| 逻辑 | `和`/`and`、`或`/`or` | 逻辑与/或 | 操作数比较其字符串是否为 `"true"`；非短路（两边都求值）|
| 逻辑 | `非`/`not`、`!`（**未实现**）| 逻辑非（一元）| 按字符串是否等于 `"true"` 取反 |
| 位运算 | `&` `\|` `^` | 与 / 或 / 异或 | 整数位运算 |
| 位运算 | `<<` `>>` | 左移 / 右移 | 整数移位 |
| 位运算 | `~` | 按位取反（一元）| 整数取反 |
| 拼接 | `..` | 字符串拼接 | 调用 `vus_string_concat` |
| 一元 | `-` | 负数（一元）| `vus_to_int` 取负 |

> **运算符可用性的精确事实**（依据 `parser.c` 二元/一元分支与 `generator.c` 的 `gen_expr_binary`/`gen_expr_unary`）：
> - `+ - * / % & | ^ << >> .. == != < > <= >= and or not 非 和 或 ~` 均已实现。
> - AST 中 `VUS_AST_BINARY_OP`/`VUS_AST_UNARY_OP` 一律生成上述代码。
> - 一元分支中解析器只接受 `-`、`not`/`非`、`~`（`!` 与位补充运算符未在解析器分支出现，属「未实现」）。
> - 比较/逻辑/一元返回 `"true"`/`"false"` 字符串；`如果` 条件本质是检查表达式结果是否为字符串 `"true"`。

### 5.2 运算符优先级

解析器采用递归下降，按「从低结合到高结合」共 **12 个语法层**（`parser.c` 的 `parse_expr → … → parse_primary`）。可归纳为 **8 组**：

| 组 | 语法层 | 运算符 | 结合性 |
|----|--------|--------|--------|
| G1（最低）| logical_or | `或` `or` | 左结合 |
| G2 | logical_and | `和` `and` | 左结合 |
| G3 | bitwise_or → xor → and | `\|` → `^` → `&` | 左结合 |
| G4 | comparison | `==` `!=` `<` `>` `<=` `>=` | 左结合 |
| G5 | shift | `<<` `>>` | 左结合 |
| G6 | concat | `..` | 左结合 |
| G7 | additive → multiplicative | `+` `-` → `*` `/` `%` | 左结合 |
| G8（最高）| unary | `-` `非`/`not` `~` | 右结合（作用于其后的一元/原子）|
| — | primary / 原子 | 字面量、标识符、函数调用、成员访问、下标、`()` `[]` `{}` | — |

优先级示例：
```vus
结果 = 1 + 2 * 3      # 7（先乘后加）
结果 = 12 & 10        # 8
结果 = 5 << 2         # 20
结果 = "a" .. "b"     # "ab"
结果 = 1 + "x"        # "1x"（+ 自动拼接，因为 "x" 不是整数）
```

> `or` 优先级高于…：实际层级严格如上的**语法生产式**决定（`||` < `&&` < 位运算 < 比较 < 移位 < 拼接 < 加减 < 乘除 < 一元）。括号 `( )` 可强制任何顺序。

---

## 6. 数据类型与值语义

先给出**精确模型**：当前运行时的值实际上是「**UTF-8 字符串（`VusString*`）为主**」的动态模型，配合**结构化容器 `VusObject`**（承载列表/字典）与 **NULL**。所有标量（整数、浮点、布尔、字符串）都以 `VusString*` 字符串保存，运算时按需求经 `vus_to_int`/`vus_to_string` 转换；布尔是字符串 `"true"`/`"false"`；`null` 是 `NULL` 指针。

### 6.1 类型总览

| 语言层类型 | 底层表示 | 说明 |
|-----------|---------|------|
| 字符串 | `VusString*` | UTF-8，不可变，`data` 以 `\0` 结尾，`len` 为字节长度；标量通用载体 |
| 整数 / 浮点 / 布尔 | 即以字符串形式保存 | 运算走 `vus_to_int` / `vus_to_string`；布尔为 `"true"`/`"false"` |
| 空 | `NULL` | `null`/`空` |
| 列表 | `VusObject{ type=TYPE_LIST; u.list=VusList* }` | 动态数组，`vus_list_new(TYPE_MIXED)`（混合模式）|
| 字典 | `VusObject{ type=TYPE_DICT; u.dict=VusDict* }` | 键值哈希表，键为字符串 |
| 结构体 | `struct vus_struct_<名>` | 见第 9 章 |
| 闭包 | `VusClosure*` | 底层存在；语言层**未直接暴露**闭包字面量（见 8.6）|

`VusObject` 带魔数 `VUS_OBJECT_MAGIC`（`'VOB!'`），供 `vus_print`/`vus_typeof` 判类型。

### 6.2 值语义细节

- **标量一律字符串**：`x = 10` 得到 `"10"`；`打印(x)` 直接输出。类型注解（`x: int = 10`）不强制校验。
- **`+` 智能行为**：两操作数都可解析为整数 → 加法；否则 → 字符串拼接（见 5.1）。
- **列表**：`[e1, e2, …]` 生成一个 `VusObject`（`TYPE_MIXED`），元素可混类型。通过下标 `列表[索引]`（生成 `vus_list_get`）读取。
- **字典**：`{键: 值, …}` 生成 `VusObject`（`TYPE_DICT`）。`v0.1` **无字典遍历接口**（`VusDict` 无遍历 API；`vus_object_to_string` 对字典返回占位 `"{}"`）；字典值访问主要靠**下标**（生成 `vus_list_get`，故下标实际作用于列表语义——字典下标在语言层尚无专门读写函数，属「未完整实现」项）。
- **空**：`null`/`空` 生成 `NULL`。
- **布尔**：`真`/`true` → `"true"`，`假`/`false` → `"false"`；`如果 <expr>:` 判定 expr 结果字符串是否为 `"true"`。
- **`typeof`/`类型`** 返回类型名：`空`/`字符串`/`整数`/`浮点`/`布尔`/`列表`/`字典`（`vus_typeof`）。

示例：
```vus
a = [1, "two", 3.5]     # 列表（混合）
打印(a)                  # [1, two, 3.5]
打印(类型(a))            # 列表
d = {"k": "v"}
```

> 内存管理：对象带 `ref` 引用计数，代码生成器按规则自动插入 `vus_ref`/`vus_unref`（赋值、参数传递、返回值、列表/结构体字段持有）。**手动内存管理 API 不存在**（设计文档 E006 相关能力未落地）。

---

## 7. 语句语法

语句由 `parse_statement` 分发到具体解析函数。所有控制流/块语句都以**冒号 + 缩进块**书写。

### 7.1 表达式语句

任意表达式（含函数调用）单独成行即为表达式语句（`VUS_AST_EXPR_STMT`）：
```vus
打印("hello")
tui_清屏()
```

### 7.2 变量声明与赋值

```vus
a = 10                     # 创建/赋值，自动类型
名字 = "张三"
b: int = 20                # 显式类型注解（记录在 AST，不强制检查）
```
- 变量无需提前声明，直接赋值即创建。
- 赋值语句在函数**外部** → 全局变量（`VUS_AST_ASSIGN`）；在函数**内部** → 局部变量（`VUS_AST_ASSIGN_LOCAL`）。
- **无复合赋值运算符**（如 `+=`）——`x = x + 1` 需写完整。

### 7.3 条件判断：`如果 if` / `否则如果 elif` / `否则 else`

```vus
如果 分数 >= 90:
    打印("优秀")
否则如果 分数 >= 80:
    打印("良好")
否则:
    打印("其它")
```
- 支持多 `否则如果`，并且 `else` 可选。
- 中文 `否则如果` 是**连写**关键字，中间不能空格。
- 条件为表达式，判定其结果是否为 `"true"`。

### 7.4 循环：`循环…从…到`（for-range）与 `循环…在…`（foreach）

三种形式（`parse_for_stmt` 自动区分）：

```vus
# ① 数字范围循环（包含结束值，步长 1；底层 for 使用 <=）
循环 i 从 1 到 10:
    打印(i)

# ② 英文风格范围循环
for i in range(1, 11):
    打印(i)

# ③ 列表遍历
循环 元素 在 列表:
    打印(元素)

# 英文 foreach
for 元素 in 列表:
    打印(元素)
```
- 范围循环包含`到`/`range` 的**结束值**（生成 `for (_i = 开始; _i <= 结束; _i++)`）。
- 列表遍历生成 `for (i=0; i<列表长度; i++)` + `vus_list_get`。

### 7.5 当循环：`当循环` / `while`

```vus
x = 1
当循环 x <= 5:
    打印(x)
    x = x + 1

while x <= 10:
    x = x + 1
```

### 7.6 返回：`返回` / `return`

```vus
定义 加(a, b):
    返回 a + b        # 带返回值
    # 也可 `返回` （无值）
```
- `返回` 后可跟表达式或无值。

### 7.7 `跳出` / `break`、`继续` / `continue`

```vus
循环 i 从 1 到 10:
    如果 i == 5:
        跳出        # break
    如果 i % 2 == 0:
        继续        # continue
    打印(i)
```
（英文 `break` / `continue` 同样支持。）

### 7.8 全局变量声明：`全局` / `global`、导入：`导入`/`import`、`从…导入`/`from…import`

```vus
全局 计数            # 在函数内声明要修改的外层全局变量

导入 "工具"          # 或 import
从 "工具" 导入 计算   # from ... import ...
```
- 函数内部默认访问局部变量；要通过赋值修改全局变量，需先 `全局 名称`（生成 `VUS_AST_GLOBAL_DECL`，在 AST 层为标记，实际代码生成以注释形式记录）。
- 导入机制支持同项目内路径导入与 `from ... import *`（见设计；实际 `parser` 实现 `parse_import_stmt`/`parse_from_import_stmt`）。

### 7.9 异常处理：`尝试 try` / `捕获 except` / `抛出 throw`

```vus
尝试:
    抛出 "错误1"          # throw 一条消息（字符串）
    打印("这行不会执行")
捕获:
    打印("捕获到异常")
捕获 具体类型:
    打印("按类型捕获")
```
- `抛出`/`throw`（`VUS_AST_THROW`）：生成 `_vus_err = vus_error_new(1, <消息>, __LINE__, __func__)`，并以 break 跳出当前 try 的 `do{…}while(0)`。
- `尝试`/`try`（`VUS_AST_TRY`）：生成保存/恢复错误态的块：
  1. 保存 `_saved_err`，清空 `_vus_err`；
  2. try 体放入 `do{…}while(0)`；
  3. 依序匹配 `捕获` 子句：
     - **带类型**的捕获：`if (strcmp(_vus_err->msg, "类型名") == 0)`（即**按错误消息字符串匹配**，属粗略类型匹配）；
     - **通配**捕获（无类型）：`else { ... }`；
  4. 命中则 `vus_error_print` + `vus_error_free` 并复位 `_vus_err`，执行捕获体；
  5. 若无任何捕获命中，错误恢复到外层（`vus_error_push` 链式传递）。
- 异常模型为**错误码链（Go 风格）**加 `do-while` 控制流，**不使用 `setjmp`/`longjmp`**。
- 未捕获异常在最外层处理并终止程序。
- **异常类型**目前即「`抛出` 的消息字符串」，并无内建异常类型系统；`捕获 <名称>` 只是字符串比对。

### 7.10 结构体定义：`struct`/`结构`（见第 9 章）

```vus
struct Point:
    x: int
    y: int
```

---

## 8. 函数

### 8.1 定义与调用

```vus
定义 问候(名字):
    打印("你好，" + 名字 + "\n")
问候("张三")

def 加(a, b):
    返回 a + b
```
- 函数体要求缩进，以 `:` + 缩进块书写。
- 调用生成 `vus_<净化名>(参数数组)`；未内置、非自定义的调用按此规则链接（自定义函数/结构体构造均可）。

### 8.2 参数

```vus
定义 例(a, b, 名称="默认", 计数: int = 0):
    打印(名称)
```
- 参数可带**类型注解**（`: 类型`）与**默认值**（`= 表达式`，对应 `VUS_AST_PARAM_DEFAULT`）。
- 解析为 `VUS_AST_PARAM`/`VUS_AST_PARAM_DEFAULT`。类型注解记录但**不强制检查**。

### 8.3 返回值

```vus
定义 平方(n):
    返回 n * n
结果 = 平方(3)     # "9"
```
- `返回` 生成函数 `return`，并做栈追踪弹出（见第 12 章）。返回值以字符串承载。

### 8.4 作用域与全局变量限制

- 顶层变量 = 全局变量（`VUS_AST_ASSIGN`）。
- 函数内赋值默认创建局部变量（`VUS_AST_ASSIGN_LOCAL`）。
- 函数内要修改全局变量，须 `全局 名称` 声明。
- **循环/条件块无独立块级作用域**（缩进只控制语法结构，不产生新作用域）。

```vus
全局总量 = 100
定义 改():
    全局 全局总量
    全局总量 = 200
```

### 8.5 泛型函数调用

语法层支持**泛型声明**与**泛型调用**：

```vus
定义 标识<T>(a):
    返回 a

定义 合并<T, U>(a, b):
    返回 a + b

x = 标识<int>(42)            # 泛型调用：标识<int>(42)
y = 合并<string, string>("a", "b")
```
- 泛型类型参数在 AST 中记录为 `type_params` / `type_args`，**但当前代码生成将其作为注释注入**（`/* <int> */`），**不执行真正的泛型实例化或类型检查**。
- 也就是说：**「泛型」目前只有语法形态，尚无真实泛型语义**（为待办/占位实现）。`tests/test_generic.vus` 可运行，只是因为生成的得以经普通调用路径链接。

### 8.6 递归 / 闭包

- **递归**：支持。测试含阶乘、斐波那契递归（通过 `vus_<函数名>` 自调用）。
- **闭包**：运行时库提供 `VusClosure*`（`func`+`env`，引用计数管理），`vus_closure_call` 供 C 层回调使用（如高阶回调）。**语言层语法未提供闭包字面量/`fn` 表达式**——语言层函数以顶层命名函数形式编译为 `void vus_<名>(void** args)`。因此「把匿名函数当作普通值传递」在语言层**未直接暴露**；设计文档中的「函数作为值（闭包）如 `执行(加, 3, 5)`」在当前编译模型下**并非已支持的顶级特性**（普通函数调用即可，但把函数名当参数的小节，行为依赖代码生成模式，应视为「未完全支持」）。

---

## 9. 结构体

### 9.1 定义

```vus
struct Point:
    x: int
    y: int
```
- 字段每行一个，可带类型注解。
- 生成 C 结构体：`typedef struct vus_struct_Point { int ref; VusString* vus_x; VusString* vus_y; } vus_struct_Point;`

### 9.2 实例化

```vus
p = Point(10, 20)
```
- `struct 名(…)` 按**普通函数调用**解析；生成器为该结构体自动生成构造函数 `void vus_Point(void** args)`（`gen_struct_constructor`），按字段序把参数 `vus_ref` 后赋给字段，并把对象指针放回 `_args[0]`。
- 结构体对象也带 `ref` 引用计数。

### 9.3 链式访问（`.`）与下标

```vus
p.x = ...            # 赋值 / 读取字段
面积 = p.x * p.y
```
- 成员访问（`VUS_AST_ACCESS`）生成 `({if(!obj){...}((vus_struct_类型*)obj)->vus_成员;})`，支持**链式**访问如 `a.b.c`（生成器通过结构体类型表 `s_gen_structs` 解析中间类型）。
- 下标（`VUS_AST_SUBSCRIPT`）`expr[idx]` 生成 `vus_list_get((VusList*)expr, vus_to_int(idx))`，主要服务于**列表**下标访问。
- 测试用例 `test_struct_chain.vus`、`test_subscript.vus` 覆盖链式访问与列表下标。

---

## 10. 线程与协程

### 10.1 线程

关键字为**中文函数式表达式**（`parse_primary` 的线程分支 + `generator` 的 `VUS_AST_THREAD_CREATE/JOIN`）：

| 语法 | 语义 | 底层 |
|------|------|------|
| `线程(函数, 实参)` | 创建并启动新线程，返回字符串句柄 | `vus_thread_create_handle`（Pthread）|
| `等待线程(句柄)` | 等待线程结束并返回其 `void*` 结果 | `vus_thread_join_handle` |
| `睡眠(毫秒)` | 休眠指定毫秒 | `vus_thread_sleep`（已实现，基于 usleep） |

示例：
```vus
定义 任务(参数):
    打印("线程：" + 参数 + "\n")
    返回 "完成"

t = 线程(任务, "hello")
结果 = 等待线程(t)
打印(结果)              # "完成"
```

### 10.2 协程

| 语法 | 语义 | 底层 |
|------|------|------|
| `协程(函数, 实参)` | 创建协程，返回句柄 | `vus_coro_create_handle`（ucontext 轻量协程）|
| `恢复(句柄)` | 恢复协程执行 | `vus_coro_resume_handle` |
| `让出()` | 暂停协程，交还控制权 | `vus_coro_yield()` |

```vus
定义 生成器(参数):
    打印("开始\n")
    让出()
    打印("中间\n")
    让出()
    返回 "完成"

c = 协程(生成器, "")
恢复(c)     # 输出 开始
恢复(c)     # 输出 中间
```

### 10.3 目标平台 / 依赖

- **线程**基于 POSIX 线程；**协程**基于 `ucontext`（`rt/vus_coro.c`）。
- 工作频率上限：进程内线程/协程句柄表 `VUS_MAX_HANDLES = 64`。

> 说明：`睡眠(毫秒)` 生成运行时调用 `vus_thread_sleep(毫秒)`，该函数已实现（基于 `usleep`），可正常链接使用。

---

## 11. 内置函数与插件运行时函数

所有内置函数在 `generator.c` 的 `gen_expr_call` 中按函数名映射到对应运行时函数。下表**逐条列出当前已接线的函数**。

### 11.1 核心内置函数

| 调用形式 | 生成/运行时 | 说明 |
|---------|------------|------|
| `打印(x)` / `print(x)` | `vus_print` | 输出文本；接收 `VusString*` 或结构化 `VusObject*`（列表/字典自动序列化）|
| `输入([提示])` | `vus_input` | 从 stdin 读一行（去掉行尾 `\n`/`\r`），提示可选 |
| `转数字(文本)` | `vus_to_int` | 字符串 → 64 位整数，成功返回数值、失败返回 0 |
| `转文本(数值)` | `vus_to_string` | 整数 → 字符串 |
| `睡眠(毫秒)` | `vus_thread_sleep` | 基于 `usleep`，已实现可链接（见 10.3）|
| `typeof(x)` / `类型(x)` | `vus_typeof` | 返回类型名：`空`/`字符串`/`整数`/`浮点`/`布尔`/`列表`/`字典` |

### 11.2 分级日志内置函数（EasyLogger 集成）

首次调用任一 `日志_*` 时惰性初始化 `vus_log_init`（EasyLogger）；4 级输出返回 `"0"`（成功）或 `"-1"`（失败）。

| 调用 | 运行时 | 说明 |
|------|--------|------|
| `日志_调试(消息)` | `vus_log_debug` | 输出 Debug 级日志 |
| `日志_信息(消息)` | `vus_log_info` | 输出 Info 级日志 |
| `日志_警告(消息)` | `vus_log_warn` | 输出 Warn 级日志 |
| `日志_错误(消息)` | `vus_log_error` | 输出 Error 级日志 |
| `日志_级别(级别)` | `vus_log_set_level` | 设置过滤级别（`调试`/`信息`/`警告`/`错误`）|

### 11.3 插件运行时函数（TUI，基于 ANSI 转义，无 ncurses）

| 调用 | 运行时 | 说明 |
|------|--------|------|
| `tui_清屏()` | `vus_plugin_tui_clear` | 清屏 |
| `tui_重置()` | `vus_plugin_tui_reset` | 重置终端属性 |
| `tui_设置颜色(前景, 背景)` | `vus_plugin_tui_set_color` | 设置 ANSI 颜色，如 `("32","40")` |
| `tui_定位(行, 列)` | `vus_plugin_tui_locate` | 移动光标 |
| `tui_进度条(当前, 总值, 宽度)` | `vus_plugin_tui_progress` | 显示进度条 |

### 11.4 插件运行时函数（网络，基于 libcurl）

| 调用 | 运行时 | 依赖 |
|------|--------|------|
| `网络_GET(url)` | `vus_plugin_http_get` | 需编译期定义 `VUS_HAVE_CURL` + `-lcurl` |
| `网络_POST(url, 数据)` | `vus_plugin_http_post` | 同上 |
| `网络_下载(url, 文件路径)` | `vus_plugin_http_download` | 同上 |

> 未定义 `VUS_HAVE_CURL` 时这些函数返回错误消息（不发送请求）。

### 11.5 插件运行时函数（文件，基于标准 C I/O + POSIX）

| 调用 | 运行时 | 返回 |
|------|--------|------|
| `文件_读取(路径)` | `vus_plugin_file_read` | 文件内容字符串 |
| `文件_写入(路径, 内容)` | `vus_plugin_file_write` | 成功 `"true"` |
| `文件_追加(路径, 内容)` | `vus_plugin_file_append` | 成功 `"true"` |
| `文件_存在(路径)` | `vus_plugin_file_exists` | `"true"`/`"false"` |
| `文件_删除(路径)` | `vus_plugin_file_delete` | 成功 `"true"` |
| `文件_列表(路径)` | `vus_plugin_file_list` | 每行一个文件名 |

### 11.6 插件运行时函数（日期时间，基于 `<time.h>`）

| 调用 | 运行时 | 说明 |
|------|--------|------|
| `日期_现在()` | `vus_plugin_date_now` | ISO 8601 当前时间字符串 |
| `日期_时间戳()` | `vus_plugin_date_timestamp` | Unix 秒级时间戳（字符串）|
| `日期_从时间戳(ts)` | `vus_plugin_date_from_timestamp` | 时间戳 → 日期字符串 |
| `日期_格式化(格式)` | `vus_plugin_date_format` | 按 `strftime` 格式格式化当前时间，如 `"%Y-%m-%d"` |
| `日期_解析(字符串, 格式)` | `vus_plugin_date_parse` | 按格式解析日期 |
| `日期_年()` / `日期_月()` / `日期_日()` | `vus_plugin_date_year/month/day` | 当前年/月/日 |
| `日期_时()` / `日期_分()` / `日期_秒()` | `vus_plugin_date_hour/minute/second` | 当前时/分/秒 |

### 11.7 插件 / JSON 内置函数

| 调用 | 运行时 | 说明 |
|------|--------|------|
| `插件_运行(插件, 命令)` | `vus_plugin_run_vux` | 在子进程中运行 `.vux` Python 插件 |
| `插件_运行JSON(插件, 命令)` | `vus_plugin_run_vux_json` | 子进程调用插件并解析 JSON 结果；`VUS_USE_PY` 开启时走嵌入式解释器（`vus_plugin_run_vux_inproc`）|
| `JSON_解析(字符串)` | `vus_json_parse` | JSON → `VusObject*`（基于 yyjson 纯 C 库，**默认可用**）|
| `JSON_生成(对象)` | `vus_json_generate` | `VusObject*` → JSON 字符串（yyjson，默认可用）|
| `JSON_查询(json, 路径)` | `vus_json_query` | 按 `a.b[0]` 路径取字段（yyjson，默认可用）|
| `对象文本(值)` | `vus_object_to_string` | 结构化对象安全转文本 |
| `字典_键(字典)` | `vus_dict_keys` | 返回字典键列表 |

> **依赖说明**：`JSON_解析/生成/查询` 基于内嵌 `yyjson`（纯 C，MIT），**不需要** Python；仅 **`typeof`** 与**嵌入式（进程内）插件调用**在未定义 `VUS_USE_PY` 时降级——`typeof` 恒返回 `"空"`，`插件_运行JSON` 回落子进程方案。`vus.json` 里已默认开启或由 Makefile 注入 `VUS_USE_PY`（见 `Makefile`）。

### 11.8 GUI 内置函数（`图形_*`，GuiLite 画布流）

两套 GUI 机制中**画布流**的内置函数（`generator.c` → `guilite_bridge.c` → GuiLite）。完整签名与示例请查 LSP 补全表 `src/lsp/vus_builtin.c`，此处列分组：

| 分组 | 函数 |
|------|------|
| 初始化/生命周期 | `图形_初始化(宽,高,标题)` `图形_刷新()` `图形_保持()` `图形_取事件()` `图形_主题(...)` `图形_外观(圆角[,抗锯齿])` |
| 绘制原语 | `图形_画点` `图形_画线` `图形_矩形` `图形_填充` `图形_画圆` `图形_填充圆` `图形_圆弧` `图形_圆角矩形` `图形_圆角填充` `图形_圆环` `图形_文字` |
| 文本 | `图形_MD(x,y,宽,文本)`（最小 Markdown 子集，返回行数）`图形_字体_加载(路径,字号)` `图形_字体已加载()` |
| 图片/动画 | `图形_背景图` `图形_图片`（.png/.svg/.gif）`图形_动画_打开/下一步/帧数/关闭` |
| 滚动容器 | `图形_滚动容器(名,x,y,宽,高,内容高)` `图形_滚动容器滚(名,dy)` `图形_滚动容器偏移(名)` |
| 页面 | `图形_页面_打开(名)`（约定函数 `页_<名>()`）`图形_页面_返回()` `图形_页面_当前()` `图形_页面_绘制()` |
| 基础控件 | `图形_按钮(名,x,y,宽,高,文本)` `图形_标签` `图形_文本框` `图形_卡片` `图形_面板` `图形_表单行` `图形_进度` `图形_列表/列表行/列表选中/列表行点击` `图形_画布/画布命中/画布点` |
| 交互控件 | `图形_滑块/滑块值` `图形_开关/开关值` `图形_微调/微调值` `图形_单选/单选值` `图形_复选框` `图形_按钮点击` `图形_行点击` `图形_悬停` |
| 输入状态 | `图形_按键()` `图形_按键码()` `图形_鼠标位置()` `图形_鼠标x/y()` `图形_滚轮()` `图形_键按下(键)` `图形_模拟点击(x,y)`（headless 测试用）|

> 平台：Termux 需先 `Termux_启动X11()` / `Termux_启动GPU()`；Linux 直接跑 X11。同屏空间内全部控件为"自绘 + 命中矩形检测"（无平台控件树）。

### 11.9 VUA 组件流内置函数（`界面_*`，Android APK）

组件解析流（`.vua` 定义界面 + `.vus` 逻辑 + Java 原生 View 渲染）的驱动函数（`generator.c` → `rt/vua.c`）：

| `.vus` 调用 | 运行时 | 说明 |
|------------|--------|------|
| `界面_显示(路径)` | `vua_session_show` | 读 `.vua` 解析压栈成为当前屏 |
| `界面_显示_JSON(串)` | `vua_show_json` | `.vua` 格式 JSON 字符串直接上屏（动态渲染树）|
| `界面_返回()` / `界面_返回至(名)` | `vua_session_back` / `back_to` | 弹栈导航 |
| `界面_绑定(事件名, 函数)` | `vua_on` | 事件名 → `.vus` 函数登记（同名覆盖）|
| `界面_设置(变量, 值)` / `界面_取(变量)` | `vua_state_set` / `get_or_empty` | 读写当前屏变量状态 |

> `.vua` 语言、控件表、事件/变量规则详见 [VUA_REFERENCE.md](VUA_REFERENCE.md) 与 [VUA_RENDER_TREE.md](VUA_RENDER_TREE.md)。

### 11.10 未实现 / 已废弃的内置函数（务必区分）

设计文档/教程中提到的下列函数，在 `generator.c` 的 `gen_expr_call` **并无接线**，即**当前未实现**（若在代码中调用会落到普通 `vus_<名>` 拼接，通常导致链接失败）：

- `断言(条件, 消息)`、`调试输出`、`退出`、`长度`、`拼接`、`分割`、`替换`、`取子串`、`取整`、`取随机数`、`创建列表`、`添加元素`、`取元素`、`删除元素`、`列表长度`、`遍历列表`、`创建字典`、`字典设值`、`字典取值`、`字典删除`、`字典长度`、`读取文件`、`写入文件`、`追加文件`、`删除文件`、`文件是否存在`、`当前时间`、`等待` 等（第 11.3–11.6 中的 `文件_*`/`日期_*`/`网络_*`/`tui_*` 为**已实现**版本，其余旧式名称未实现）。

> 结论：**当前真正可用的内置函数** = 11.1 的核心 + 11.2 的日志 + 11.3–11.6 的四组插件运行时函数 + 11.7 的插件/JSON + **11.8–11.9 的 GUI**（`图形_*` 画布流、`界面_*` 组件流）。

---

## 12. 错误信息与调试

### 12.1 编译器错误

- **词法错误**：`lexer_set_error`，如 `\x 后需要两位十六进制数字`、`\u 后需要四位十六进制数字`、`\u 转义超出基本多文种平面`。
- **语法错误**：`parser_set_error`，如 `期望 缩进，但遇到 …`、`期望 '>' 或 ',' 在泛型参数中`、`期望 字符串 …`、`期望 ':'`、`意外的 Token: <类型>（第 x 行第 y 列）` 等，均含行/列号。
- 编译器（`VusResult.error_msg`）返回失败原因；`vus build`/`vus run` 以非零退出码停止。

> 设计文档中的 E001–E015 编号错误码体系**未在源码中实现**（源码用中文自由文本报错）。本文不把 E001–E015 作为「已支持」的对外协议。

### 12.2 运行时错误与调用链（栈追踪）

- 运行时提供栈追踪：`VUS_MAX_STACK_DEPTH=256`；函数进入 `vus_stack_push(name)`、退出 `vus_stack_pop()`，`vus_stack_print()` 打印调用链。
- 生成代码为**每个函数**注入 `vus_stack_push("函数名")`（调试模式/正常均生成）；`throw` 记录 `__LINE__` 与 `__func__`。
- **`断言`**：当前**未实现**（无断言内置函数）。

### 12.3 调试模式 `vus run --debug`

- `vus run --debug <文件>` 把 `config.debug` 置 1；`gen_main_function` 在 debug 时生成 `vus_debug_enabled = 1;`，随后运行时 `vus_debug_print` 可输出调试信息。
- 与 `.vus` 文件的异常（栈追踪、错误链）配合，便于定位问题。

### 12.4 测试运行

`vus test` 逻辑由主程序驱动（`main.c` 的测试运行分支），顺序运行 `构建/`/`测试/` 下的测试脚本并汇总成功/失败。

---

## 13. 附录：完整语法速查（EBNF / 伪语法）

### 13.1 语句层（伪 EBNF）

```
program        := statement*
statement      := function_def | struct_def
               | if_stmt | for_stmt | while_stmt | try_stmt
               | return_stmt | import_stmt | from_import_stmt
               | break_stmt | continue_stmt | throw_stmt | global_stmt
               | assign_or_expr

function_def   := ("def"|"定义") IDENT [ "<" (IDENT ("," IDENT)*)? ">" ]
                  "(" params? ")" ":" NEWLINE INDENT statement* DEDENT
params         := param ("," param)*
param          := IDENT [":" IDENT] ["=" expr]            // 类型注解/默认值

struct_def     := ("struct"|"结构") IDENT [":"] NEWLINE INDENT field* DEDENT
field          := IDENT [":" IDENT] NEWLINE

if_stmt        := ("if"|"如果") expr ":" block
                  ( ("elif"|"否则如果") expr ":" block )*
                  [ ("else"|"否则") ":" block ]
for_stmt       := ("for"|"循环") IDENT
                    ( "in" "range" "(" expr "," expr ")"     // for-range
                    | "in" expr                             // foreach
                    | "从" expr "到" expr                    // 中文 for-range
                    | "在" expr )                           // 中文 foreach
                  ":" block
while_stmt     := ("while"|"当循环") expr ":" block
try_stmt       := ("try"|"尝试") ":" block
                  ( ("except"|"捕获") [IDENT|"(" IDENT ")"] ":" block )*
throw_stmt     := ("throw"|"抛出") expr
return_stmt    := ("return"|"返回") [expr]
break_stmt     := ("break"|"跳出")
continue_stmt  := ("continue"|"继续")
global_stmt    := ("global"|"全局") IDENT
import_stmt    := ("import"|"导入") IDENT
from_import_stmt:= ("from"|"从") IDENT ("import"|"导入") (IDENT|"*")
assign_or_expr := (IDENT [":" IDENT] "=" expr) | expr        // 赋值 或 表达式语句

block          := NEWLINE INDENT statement* DEDENT
```

### 13.2 表达式层（优先级由低到高）

```
expr          := logical_or
logical_or    := logical_and (("or"|"或") logical_and)*
logical_and   := bitwise_or (("and"|"和") bitwise_or)*
bitwise_or    := bitwise_xor ("|" bitwise_xor)*
bitwise_xor   := bitwise_and ("^" bitwise_and)*
bitwise_and   := comparison ("&" comparison)*
comparison    := shift (("=="|"!="|"<"|">"|"<="|">=") shift)*
shift         := concat (("<<"|">>") concat)*
concat        := additive (".." additive)*
additive      := multiplicative (("+"|"-") multiplicative)*
multiplicative:= unary (("*"|"/"|"%") unary)*
unary         := ("-"|"not"|"非"|"~") unary | primary
primary       := IDENT [ "<" typeargs ">" ]   // 泛型调用
               | NUMBER | STRING | "true"|"真" | "false"|"假" | "null"|"空"
               | "(" expr ")"
               | "[" [expr ("," expr)*] "]"
               | "{" [key ":" value ("," key ":" value)*] "}"
               | IDENT ("(" args? ")")? ( "." IDENT )* ( "[" expr "]" )*
               | ("线程"|"协程") "(" expr "," expr ")"
               | ("等待线程"|"恢复") "(" expr ")"
               | "让出" "(" ")" | "睡眠" "(" expr ")"
```

### 13.3 语句模板速查

```vus
# 变量与赋值
a = 1
名字 = "张三"
b: int = 2

# 条件
如果 a > 0:
    ...
否则如果 a == 0:
    ...
否则:
    ...

# 循环
循环 i 从 1 到 10:
    ...
循环 x 在 列表:
    ...
当循环 条件:
    ...
for i in range(1, 11):
    ...

# 函数
定义 名(参数1, 参数2=默认):
    返回 表达式

# 异常
尝试:
    抛出 "消息"
捕获:
    ...

# 结构体
struct 点:
    x: int
    y: int
p = 点(1, 2)
打印(p.x)

# 线程 / 协程
t = 线程(任务, 实参)
结果 = 等待线程(t)
c = 协程(生成器, 实参)
恢复(c)
让出()
```

### 13.4 示例：完整可运行程序

```vus
#// 综合示例
定义 阶乘(n):
    如果 n <= 1:
        返回 1
    返回 n * 阶乘(n - 1)

结构 Point:
    x: int
    y: int

p = Point(3, 4)
打印("阶乘(5)=")
打印(阶乘(5))
打印("\n")

当循环 p.x < 6:
    打印(p.x)
    p.x = p.x + 1
```

---

## 附录：实际已支持 vs 尚未实现 关键清单

| 分类 | 项目 | 状态 |
|------|------|------|
| 核心流水线 | 词法/语法/AST/C 代码生成/GCC 编译 | ✅ 已实现 |
| 双语法 | 函数风格英文 + 中文别名关键字 | ✅ 已实现（核心语法层）|
| 双语法 | 易语言风格（`.功能`/`.结束`）| ⚠️ 通过 `.vulage` 语言插件**编译前预处理**实现，非核心语;核心词法器不认识点前缀 |
| 类型注解 | `变量: 类型`、参数注解、泛型参数 | ⚠️ AST 记录，**无强类型校验**，代码中作注释/信息 |
| 泛型 | `函数名<T>()` 语法 | ⚠️ 语法可用，但**仅注入注释，无实例化/类型检查**（占位）|
| 运算符 | `+ - * / % .. == != < > <= >= & \| ^ ~ << >> and or not 和 或 非` | ✅ 已实现（比较/逻辑/一元返回 `"true"`/`"false"` 字符串）|
| 控制流 | if/elif/else、for-range、foreach、while、return、break、continue、global | ✅ 已实现 |
| 异常 | try/except/throw（错误链 + do-while）| ✅ 已实现；异常类型 = 抛出消息字符串比对 |
| 结构体 | struct 定义、`Point(...)` 构造、`.` 链式访问 | ✅ 已实现 |
| 线程 | `线程`/`等待线程` | ✅ 已实现 |
| 协程 | `协程`/`恢复`/`让出` | ✅ 已实现 |
| 睡眠 | `睡眠(毫秒)` | ✅ 已实现 |
| 异步 `等待` | `VUS_TOKEN_CN_AWAIT` | ❌ 仅定义 token，解析器**未实现** |
| 内置函数 | 打印/输入/转数字/转文本/typeof/类型 | ✅ 已实现 |
| 日志 | 日志_调试/信息/警告/错误/级别 | ✅ 已实现（可随链接 EasyLogger；随 `libvus_rt.c` 编译）|
| 插件运行时 | `tui_*`、`网络_*`、`文件_*`、`日期_*` | ✅ 已实现；网络需 `VUS_HAVE_CURL` |
| 插件/JSON | `插件_运行`、`插件_运行JSON`、`JSON_解析`/`生成`/`查询`、`对象文本`、`字典_键` | ✅ 已实现；JSON 走内嵌 yyjson（默认可用）；`typeof` 与嵌入式插件调用需 `VUS_USE_PY`（否则 typeof 恒 `"空"`、进程内调用回落子进程）|
| 旧式标准库 | 断言/退出/长度/拼接/分割/替换/取子串/取整/遍历列表/字典操作/文件旧名等 | ❌ **未实现**（generator 无接线；现代版本见 `文本_*`/`列表_*`/`字典_*`/`文件_*`）|
| 手动内存管理 | 手动申请/释放 | ❌ 不存在（仅自动引用计数）|
| GUI 画布流 | `图形_*`（GuiLite：控件/绘制/图片/动画/页面/滚动/事件）| ✅ 已实现（见 11.8 与 [API_REFERENCE.md](API_REFERENCE.md)）|
| GUI 组件流 | `界面_*`（VUA：`.vua` + 多屏导航 + 事件绑定）| ✅ 已实现（见 11.9 与 [VUA_REFERENCE.md](VUA_REFERENCE.md)）|