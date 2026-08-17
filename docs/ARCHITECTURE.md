# VUS 架构与实现文档

> 版本：v1.0-beta
> 撰写依据：基于 `/workspace/vus` 项目真实源码
> 说明：本文档描述 VUS 编译器的整体架构与各模块的实现机制。

---

## 目录

1. [系统总览](#一系统总览)
2. [编译流水线](#二编译流水线)
3. [词法分析器实现](#三词法分析器实现)
4. [语法分析器实现](#四语法分析器实现)
5. [抽象语法树设计](#五抽象语法树设计)
6. [代码生成器实现](#六代码生成器实现)
7. [配置系统](#七配置系统)
8. [运行时库实现](#八运行时库实现)
9. [插件系统实现](#九插件系统实现)
10. [C ABI 与嵌入式](#十c-abi-与嵌入式)
11. [APK 打包实现](#十一apk-打包实现)
12. [构建系统](#十二构建系统)
13. [开发指南要点](#十三开发指南要点)

---

## 一、系统总览

VUS 是一门编译到 C 的中文编程语言，其编译器采用经典的多层架构，将 VUS 源码逐级降级为 C 代码，最终交给系统 C 编译器（GCC / Clang / Android NDK）生成可执行文件。

```
┌──────────────────────────────────────────────────────────────┐
│                        用户层（VUS 源码）                       │
│  main.vus  │  项目配置 vus.json  │  测试用例  │  各类插件      │
└───────────────────────┬──────────────────────────────────────┘
                        │
                        ▼
┌──────────────────────────────────────────────────────────────┐
│                      编译器内核（src/）                        │
│  词法分析(lexer) → 语法分析(parser) → AST → C 代码生成(generator)│
│  API 层：C ABI / 插件系统 / 语言插件 / VUSX 插件 / APK 打包     │
└───────────────────────┬──────────────────────────────────────┘
                        │
                        ▼
┌──────────────────────────────────────────────────────────────┐
│                       编译后端                                │
│    VUS → C 代码 → GCC/Clang → 原生可执行文件                   │
│                        ↘ Android NDK → APK 项目               │
└───────────────────────┬──────────────────────────────────────┘
                        │
                        ▼
┌──────────────────────────────────────────────────────────────┐
│                      运行时支撑层（rt/）                        │
│  libvus_rt（引用计数/字符串/列表/字典/闭包/错误处理/协程/线程）   │
│  EasyLogger（分级日志）│ libcurl（可选）│ POSIX 线程             │
└──────────────────────────────────────────────────────────────┘
```

职责分层：

| 层 | 目录/文件 | 职责 |
|----|-----------|------|
| 用户层 | `.vus` 文件、`vus.json`、插件目录 | 用户编写的源码与项目配置 |
| 编译器内核 | `src/` | 词法/语法分析、AST、C 代码生成 |
| 编译后端 | 系统 C 编译器 | 将生成 C 代码编译为可执行文件 |
| 运行时支撑层 | `rt/` | 提供运行时数据类型与标准库函数 |

### 模块依赖关系

```
main.c → vus_abi.c / vus_apk.c
   ↑          │
   │          ├→ lexer.c ─→ token.c
   │          ├→ parser.c ─→ ast.c
   │          └→ generator.c
   │
 config.c（被几乎所有模块依赖）
 include/vus/*.h（对外公开接口：vus.h / vus_abi.h / vus_plugin.h / vus_lang.h / vus_vusx.h）
 rt/libvus_rt.[ch]（生成代码链接的对象）
```

---

## 二、编译流水线

整体编译流程从入口函数开始，依次经过词法、语法、代码生成三个阶段。

```
VUS 源码字符串（已可选经过语言插件预处理）
   │  vus_lexer_new + vus_lexer_tokenize
   ▼
Token 数组  ──→  vus_parser_new + vus_parser_parse
   │  （生成 AST；出错则携错误信息返回）
   ▼
VusAstProgram（AST 根节点）
   │  vus_generate_c
   ▼
C 源码字符串 → 写入 .c 文件 → vus_compile_c（调用 GCC/Clang）
   ▼
可执行文件 / 对象文件（供 APK 打包复用）
```

核心流水线实现在 `src/vus_abi.c` 的 `compile_source()` 函数中，责任链如下：

1. **语言插件预处理**：若配置了 `language_plugin`，先调用 `vus_lang_preprocess` 将源码规范化为标准 VUS 函数风格。
2. **词法分析**：`vus_lexer_new(processed_source, processed_len)` → `vus_lexer_tokenize` 得到 Token 数组。
3. **语法分析**：`vus_parser_new(tokens, token_count)` → `vus_parser_parse` 得到 `VusAstProgram *program`。
4. **代码生成**：`vus_generate_c(program, config)` 得到 C 源码字符串，写入 `.c` 文件。
5. **后端编译**：`vus_compile_c(...)` 调用系统 C 编译器将 `.c` 文件链接为可执行文件。

用户层入口 `main.c` 通过子命令分发（`build` / `run` / `init` / `test` / `lang` / `vux` / `vusx` / `update` / `apk` 等）触发上述不同阶段的流水线（仅生成 C、生成可执行、或直接运行）。

---

## 三、词法分析器实现

文件：`src/lexer.c` / `src/lexer.h`，核心数据结构 `VusLexer`。

### 3.1 Token 类型体系

Token 类型全部集中在 `src/token.h` 的 `VusTokenType` 枚举中，主要类别：

- **标识符与字面量**：`IDENTIFIER` / `STRING` / `NUMBER`
- **关键字（英文函数风格）**：`def / if / elif / else / for / while / return / import / from / true / false / null / and / or / not / try / except / global / break / continue / throw / in`
- **关键字（中文别名）**：`定义 / 如果 / 否则如果 / 否则 / 循环 / 当 / 返回 / 导入 / 从 / 到 / 真 / 假 / 空 / 并且 / 或者 / 非 / 尝试 / 异常 / 全局 / 中断 / 继续 / 抛出 / 在`
- **结构体关键字**：`struct / 结构体`
- **线程/协程关键字**：`线程 / 等待线程 / 线程休眠 / 协程 / 恢复 / 让出 / 等待`
- **类型注解关键字**：`整数 / 浮点 / 文本 / 布尔 / 列表 / 字典`
- **分隔符与运算符**：括号、点、逗号、冒号，算术/位/逻辑/比较/拼接运算符
- **特殊 Token**：`NEWLINE / INDENT / DEDENT / EOF / ERROR`

`VusToken` 结构体记录类型、指向源码的起始指针 `start`、长度 `length`、行号 `line`、列号 `column` 与动态分配的 `value`（仅字面量使用）。

### 3.2 UTF-8 中文标识符处理

`token.c` 中通过 `vus_utf8_decode` 解码 UTF-8 序列得到 Unicode 码点，并用 `vus_is_chinese_id_char` / `vus_is_ident_start` / `vus_is_ident_continue` 判断字符是否为合法标识符字符，从而支持中文变量名、函数名。

### 3.3 双语法检测与关键字查找

`vus_keyword_lookup(start, len)` 依据字符串精确匹配返回对应 Token 类型；找不到时返回 `IDENTIFIER`。词法分析器在读取标识符后调用此函数，将中英文关键字映射到统一 Token 类型，实现单套编译器内核同时支持英文函数风格与中文函数风格。

### 3.4 缩进处理（INDENT/DEDENT）

`lexer_handle_indent` 负责度量行首缩进：

- 空格计 1、Tab 计 4 个缩进级别；
- 跳过空行与纯注释行（不影响缩进栈）；
- 基于缩进栈（`indent_stack`）比较当前行与栈顶缩进级别，生成 `INDENT`（进入更深缩进）或 `DEDENT`（回退缩进）Token；
- 文件结束依据编译期确定的缩进级别数补齐 `DEDENT`。

这种基于缩进的换行处理使语法分析阶段无需依赖花括号即可表达代码块层级。

### 3.5 注释与字符串

- 注释：状态机中支持行注释（跳过至换行），注释行不产生 Token、不影响缩进。
- 字符串：`lexer_read_string` 解析带转义的字符串字面量，支持的转义序列包括 `\n`、`\r`、`\t`、`\\`、`\"`、`\xHH`（十六进制字节）、`\uHHHH`（Unicode 码点）。字符串内容以 UTF-8 字节形式存入 Token 的 `value`。

### 3.6 数字字面量

`lexer_read_number` 支持：

- 十进制整数（如 `123`）、十进制浮点（如 `3.14`）；
- 十六进制（`0x` 前缀）；
- 二进制（`0b` 前缀）。

---

## 四、语法分析器实现

文件：`src/parser.c` / `src/parser.h`。

语法分析器采用**递归下降分析法（recursive descent）**，以 Token 数组为输入，生成 AST 根节点 `VusAstProgram`。

### 4.1 语句解析函数

各语句解析函数大致对应：

| 语句 | 解析函数 | 说明 |
|------|----------|------|
| 函数定义 | `parse_function_def` | 支持参数、泛型类型参数、函数体 |
| if/elif/else | `parse_if_stmt` | 分支条件与多路选择 |
| for 数值循环 | `parse_for_stmt` | for-range |
| for 遍历 | `parse_for_each` | foreach 遍历可迭代对象 |
| while | `parse_while_stmt` | 条件循环 |
| try/except | `parse_try_stmt` | 异常捕获 |
| return | `parse_return` | 函数返回 |
| break/continue | 对应解析 | 循环控制 |
| import/from | 对应解析 | 模块导入 |

解析器在遇到错误时记录错误信息到 `parser->error` 并通过 `vus_parser_error` 对外暴露，同时程序（Program）节点保留部分已解析语句供容错或调试。

### 4.2 表达式优先级（8 级）

表达式按优先级从低到高递归下降解析：

```
逻辑或       or / 或
逻辑与       and / 并且
按位或       |
按位异或     ^
按位与       &
比较         ==  !=  <  >  <=  >=
移位         <<  >>
拼接         ..   （字符串拼接）
加减         +  -
乘除取模     *  /  %
一元         负号、非、按位取反
原子         字面量、标识符、调用、下标、括号、列表/字典字面量
```

### 4.3 双语法支持

语法分析器对中英文关键字均以同一 Token 枚举类型接收，因此解析逻辑只需处理单一 Token 集：`def` 与 `定义`、`if` 与 `如果` 等都在派生效时被合并。这使得代码生成阶段完全无需区分源语言的书写风格。

---

## 五、抽象语法树设计

文件：`src/ast.h` / `src/ast.c`。

AST 采用 **tagged union** 设计，所有节点以 `VusAstNode` 作为通用头（仅含 `type`、`line`、`column`），具体节点在此基础上扩展字段；子节点列表用动态数组 `VusAstList` 管理。

### 5.1 节点类型枚举

`VusAstNodeType` 共约 40 种节点，主要分为几类：

| 类别 | 节点 |
|------|------|
| 根/程序 | `PROGRAM` |
| 定义 | `FUNCTION_DEF`、`STRUCT_DEF`、`PARAM`、`PARAM_DEFAULT` |
| 控制流语句 | `IF`、`FOR_RANGE`、`FOR_EACH`、`WHILE`、`TRY`、`RETURN`、`BREAK`、`CONTINUE`、`THROW`、`GLOBAL_DECL` |
| 赋值/表达式语句 | `ASSIGN`、`EXPR_STMT` |
| 表达式 | `BINARY_OP`、`UNARY_OP`、`CALL`、`IDENTIFIER`、`STRING_LITERAL`、`NUMBER_LITERAL`、`BOOL_LITERAL`、`NULL_LITERAL`、`LIST_LITERAL`、`DICT_LITERAL`、`SUBSCRIPT`、`ACCESS` |
| 导入 | `IMPORT`、`FROM_IMPORT` |
| 结构体 | `STRUCT_INSTANTIATE` |
| 线程/协程 | `THREAD_CREATE`、`THREAD_JOIN`、`CORO_CREATE`、`CORO_RESUME`、`CORO_YIELD` |

关键节点字段：

- `VusAstFunctionDef`：`type_params`（泛型类型参数）、`params`（含默认值参数）、`body`。
- `VusAstIf`：`elif_conditions` / `elif_bodies` 数组 + `else_body`，支持多路分支。
- `VusAstAssign`：带 `is_local` 标记区分函数内局部变量与全局变量，以影响代码生成策略。
- `VusAstCall`：`type_args`（泛型类型实参）。

### 5.2 创建、遍历、销毁策略

- **创建**：每个节点由专用 `vus_ast_*_new` 工厂函数分配并初始化。
- **遍历**：代码生成器通过 `node->type` 分派到对应的 `gen_*` 函数，递归下行遍历子节点。
- **销毁**：`vus_ast_node_free` 依据 `type` 递归释放子节点、节点数组（`VusAstList`）及动态字符串字段，`vus_ast_list_free` 释放子节点列表。

---

## 六、代码生成器实现

文件：`src/generator.c` / `src/generator.h`。核心接口为 `vus_generate_c(program, config)`（返回 C 源码字符串）与 `vus_compile_c(...)`（调用系统 C 编译器）。

### 6.1 中文标识符 sanitize（__XXXX 规则）

`gen_sanitize_name` 将 VUS 标识符转换为合法 C 标识符：

- ASCII 字母/数字/下划线保留原样；
- **数字开头的名字**自动加 `_` 前缀；
- **多字节 UTF-8 字符**解码为 Unicode 码点，编码为 `_XXXX`（4 位十六进制）形式，例如中文 `打印` 转为形如 `_6253_5370`；
- 其他非字母数字 ASCII 字符替换为下划线。

所有生成的 C 变量、函数名统一加 `vus_` 前缀（如 `vus_XXX`）以避免与系统符号冲突。

### 6.2 字符串类型与引用计数插入

运行时用 `VusString*` 表示所有值。代码生成器在**赋值**、**循环变量更新**、**参数传递**、**返回**处显式插入引用计数调用以管理生命周期。例如 `gen_stmt_assign` 生成：

```c
{ VusString* _tmp = <expr>;
  vus_ref(_tmp);
  vus_unref(vus_<变量>);
  vus_<变量> = _tmp; }
```

即对新值 `vus_ref`、对旧值 `vus_unref`，再更新指针。

### 6.3 二元运算的类型调度（vus_add）

`gen_expr_binary` 将二元运算符映射到 C 表达式：

- 字符串拼接 `..` → `vus_string_concat(a, b)`
- 算术 `+` → `vus_add(a, b)`（运行时判断：若两侧均可解析为整数则做算术加法，否则做字符串拼接）
- 其余算术/位运算（`- * / % & | ^ << >>`）→ 通过 `vus_to_int(...)` 转为 `int64_t` 运算，再用 `vus_to_string(...)` 包裹
- 比较运算（`== != < > <= >=`）→ `vus_to_int` 后比较，结果以 `"true"/"false"` 字符串表达
- 逻辑 `and/或`、`or/或` → `strcmp(vus_string_cstr(...), "true")` 判断后返回布尔字符串

**布尔值注意点**：VUS 的布尔在运行时是字符串 `"true"/"false"`。条件判断（if/while）依赖 `vus_to_int`（对 `"true"` 会失败使 `err` 置非零，取 0）或 `strcmp` 比较，因此布尔语义依赖常量字符串文本，存在与类型系统映射不一致的隐患（详见「已知缺陷与限制」）。

### 6.4 GNU 语句表达式处理返回值

普通/泛型函数调用会生成 `({ ... })` 的 **GNU statement expression**（需 `-std=gnu*`，实际以 `-std=c11` + GCC 扩展编译）。典型生成：

```c
({VusString* _vus_args[N+1]; _vus_args[0]=NULL;
  _vus_args[1]=<arg1>; ...
  vus_<func>(_vus_args); _vus_args[0];})
```

约定：`_vus_args[0]` 用作返回值槽位，函数通过写 `_vus_args[0]` 返回，语句表达式整体求值即为返回值（`_vus_args[0];`）。这一机制同时实现了：
- 以 `VusString**` 数组传递全部参数（规避 C 不定参数类型问题）；
- 返回值回传（写回 args[0]）；
- 泛型函数调用无需在 C 层面特化，类型实参仅作为注释保留。

### 6.5 函数生成

`gen_function` 生成 `void vus_<名>(void* _args)`：

1. 将 `_args` 强转为 `VusString**`，逐个提取参数（`PARAM_DEFAULT` 支持 NULL 即缺省）；
2. 声明 `_vus_result`（返回值）、`_err`、`_vus_err` 等辅助变量；
3. 通过 `gen_collect_locals` 扫描函数体，在**函数顶部**统一声明所有局部变量 `VusString* vus_<名> = NULL;`；
4. 调用 `vus_stack_push("<函数名>")` 记录调用栈，结束前 `vus_stack_pop()`；
5. `return` 语句将结果写入 `_vus_params[0]` 后 `return`（见 `gen_stmt_return`），函数末尾若 `_vus_result` 非空则回写 args[0]。

### 6.6 内建函数映射

`gen_expr_call` 将具名内建函数直接映射到运行时库函数：

| VUS 内建 | 运行时函数 |
|----------|-----------|
| `打印`/`print`、`输入` | `vus_print` / `vus_input` |
| `转数字`、`转文本` | `vus_to_int` / `vus_to_string` |
| `睡眠` | `vus_thread_sleep`（已实现，基于 usleep） |
| `日志_调试/信息/警告/错误/级别` | `vus_log_*`（EasyLogger）|
| `文件_读/写/追加/存在/删除/列表` | `vus_plugin_file_*` |
| `插件_运行`、`插件_运行JSON`、`JSON_解析`、`JSON_生成`、`类型`/`typeof` | `vus_plugin_run_vux*` / `vus_json_*` / `vus_typeof` |
| `日期_*` 系列 | `vus_plugin_date_*` |

若调用的名字非内建函数，则作为普通 VUS 函数调用生成 `vus_<sanitize 名>(_vus_args)`。

### 6.7 结构体与成员访问

生成器维护 `s_gen_structs` 结构体类型表：为每个 `STRUCT_DEF` 生成 C 结构体类型（`gen_struct_type_def`）与构造函数（`gen_struct_constructor`）。成员访问 `gen_expr_access` 依据 `gen_find_struct_by_field` 在类型表中定位字段以生成正确的 C 成员访问。

### 6.8 主函数与全局变量

`gen_main_function` 生成 `int main(void)`：按配置开启 `vus_debug_enabled`，声明 `_err` / `_vus_err`，遍历顶层非函数/结构体语句执行。全局变量在 `vus_generate_c` 中**去重收集**后统一声明为 `VusString* vus_<名> = NULL;`（用字符串查重避免重复声明）。

---

## 七、配置系统

文件：`src/config.c` / `src/config.h`，`VusConfig` 结构体承载可配置项。

### 7.1 简易 JSON 解析器

`config.c` 内建一个无外部依赖的迭代式 JSON 解析器（`JsonCtx`），支持读取字符串、布尔、数字并跳过无关对象/数组，用于解析 `vus.json`。字段解析示例：

- `名称 name`、`version`、`风格 style`、`语言插件 language_plugin`
- `主文件 main_file`（默认 `main.vus`）、`输出模式 output_mode`（默认 `c`）
- `列表模式 list_mode`（默认 `严格`）、`调试 debug`
- `目标平台`（默认 `linux-gnu`）、`编译选项`（内含 `优化`、`ARM版本`）
- `vusx依赖 vusx_deps`（字符串数组，最多 `VUS_CONFIG_MAX_VUSX_DEPS` 项）
- `rt_dir`（默认 `rt`）、`build_dir`（默认 `构建`）

`vus_config_load` 先填充默认值（`style="函数"`、`main_file="main.vus"`、`output_mode="c"`、`list_mode="严格"` 等），再读取 `vus.json` 覆盖。`vus.json` 不存在时返回 0 并使用默认配置。

### 7.2 路径构建

- `vus_config_main_path` → `<project_dir>/<main_file>`
- `vus_config_build_path` → `<project_dir>/<build_dir>`（缺省 `构建`）
- `vus_config_rt_header_path` / `vus_config_rt_source_path` → `rt/libvus_rt.h` / `rt/libvus_rt.c`（可被 `rt_dir` 覆盖）

### 7.3 风格锁定

`style` 字段（默认 `"函数"`）锁定源语言的书写风格，与语言插件配合决定是否启用预处理器。

---

## 八、运行时库实现

文件：`rt/libvus_rt.c` / `rt/libvus_rt.h`，以静态归档 `libvus_rt.a` 链接进用户可执行文件。

### 8.1 引用计数通用操作

`vus_ref` / `vus_unref` 直接操作对象首字段 `ref`（约定所有对象第一个字段为 `int ref`）。`vus_unref` 在引用归零时 `free(obj)`（简化实现，不递归释放内部成员，需调用方保证正确释放）。

### 8.2 VusString 引用计数字符串

```c
struct VusString {
    int    ref;
    int    len;      /* UTF-8 字节长度 */
    char  *data;     /* 始终以 '\0' 结尾 */
};
```

- `data` 始终以 `'\0'` 结尾以方便 C 互操作，但内容可能含中间 `'\0'`；`len` 为有效字节长度，保证 `len <= strlen(data)`。
- 接口：`vus_string_new` / `vus_string_new_len` / `vus_string_concat` / `vus_string_slice` / `vus_string_len` / `vus_string_cstr`。

### 8.3 VusList / VusDict

- **VusList**：动态数组，`cap` 按需倍增（初始 4）。`vus_list_append` / `set` 对元素 `vus_ref`，`remove` / `set` 对旧元素 `vus_unref`。`type` 字段支持严格/混合元素类型标记。
- **VusDict**：链地址法哈希表（`DictImpl` + `DictEntry`），DJB2 风格哈希（`hash=5381; h=(h<<5)+h+byte`），负载因子超过 0.75 自动 `dict_resize` 扩容翻倍。键仅支持 `VusString*`。`v0.1` 未提供字典遍历接口。

### 8.4 VusObject 结构化容器

`VusObject` 是带类型标记的组合容器，供插件返回结构化数据（列表/字典/字符串）。头部含 `magic = 0x564F4221 ('VOB!')`，`vus_is_object` 依据 magic 区分 `VusObject` 与普通 `VusString`（后者紧跟 `ref` 的是 len 小整数，不会与魔数冲突）。该机制不替代也不重写既有结构，仅用于承载插件结构化结果。

### 8.5 VusClosure 闭包

```c
struct VusClosure {
    int ref;
    void (*func)(void* env, void* args);
    void* env;
};
```

`vus_closure_new` 创建时对 `env` 增引用；`vus_closure_call` 调用回调。args 由调用方分配、调用方释放。

### 8.6 VusError 错误链

```c
struct VusError {
    int code; const char* func; const char* msg;
    int line; VusError* next;  /* 最新错误在链头 */
};
```

- `vus_error_new` / `vus_error_push`（头插）/ `vus_error_print`（递归打印 `E%03d ... (代码行号: %d)`）/ `vus_error_free`（整链释放）。
- **注意**：`VusError` 不参与引用计数，由运行时库独立管理，用户无需 `vus_ref/vus_unref`。

### 8.7 线程与协程

- **线程**：`vus_thread_create/join/detach` 基于 pthread，`VusThreadTask` 封装回调与参数。
- **句柄注册表**：`vus_thread_handles` / `vus_coro_handles` 各为 `VUS_MAX_HANDLES(64)` 个槽位。句柄以索引字符串（`VusString*`）返回，避免指针类型转换问题。槽位溢出（达 64）时返回 `"-1"` 并回收资源。`vus_thread_join_handle` / `vus_coro_resume_handle` 按索引回填，完成（协程 done 或线程 join）后将槽位置 NULL。
- **协程**：`rt/vus_coro.c` / `vus_coro.h` 实现轻量级协程。基于 `setjmp/longjmp` + 少量平台特定汇编手工切换栈/寄存器（**不依赖 ucontext**，可在 Android/Bionic/Termux 上编译）。每个协程拥有独立 128KB 栈（`VUS_CORO_STACK_SIZE`）。接口：`vus_coro_create/resume/yield/is_done/free`。

### 8.8 内存释放策略

- 引用计数为首要策略，`vus_unref` 归零 `free`；
- 句柄注册表在完成/失败路径上显式 `free` 线程/协程对象并清空槽位；
- 注释明确"简化实现"，容器内部成员、复杂对象仍需调用方确保正确释放。

### 8.9 分级日志（EasyLogger 集成）

静态集成 EasyLogger（`rt/easylogger/`）。`vus_log_init` 惰性、幂等地初始化（配置格式：级别+标签+时间），后续使用 `日志_*` 内建会触发。`vus_log_set_level` 解析中文级别名（`调试/信息/警告/错误` 等）并设置过滤级别。4 级输出返回 `"0"/"-1"` 字符串，沿用内建函数约定。

### 8.10 标准库辅助

- `vus_print`（感知 `VusObject`，结构化数据先行序列化）、`vus_input`（读一行去换行）。
- `vus_add`：两侧均可解析为整数则算术加，否则字符串拼接。
- `vus_to_int` / `vus_to_float`：基于 `strtoll/strtod`，用 `err` 输出参数指示解析失败；`vus_to_string` 格式化。

### 8.11 插件运行时函数

按功能分组，多以 `VusString*` 入出参返回 `"0"/"-1"`/数据：

- **TUI**：`vus_plugin_tui_*`（ANSI 转义码，清屏/配色/定位/进度条/复位）。
- **网络**：`vus_plugin_http_get/post/download`（`VUS_HAVE_CURL` 下用 libcurl，否则返回空串/`-1`）。
- **文件**：`vus_plugin_file_*`（读/写/追加/存在/删除/列表）。
- **日期**：`vus_plugin_date_*`（now/format/parse/timestamp/年/月/日/时/分/秒）。
- **插件调用**：
  - `vus_plugin_run_vux`：子进程执行 `python3 <manager> run <插件> "<命令>" --raw`，定位管理器脚本路径优先级为 `VUS_PLUGIN_MANAGER` → `VUS_HOME/scripts` → `./scripts`。
  - `vus_plugin_run_vux_inproc` / `vus_plugin_run_vux_json`：`VUS_USE_PY` 下通过 dlopen 惰性加载 libpython 嵌入式调用 `.vux` 插件（结构化走 JSON），否则回退子进程/返回空。
  - `vus_json_parse` / `vus_json_generate`：基于嵌入式 Python `json` 模块（`VUS_USE_PY` 下生效）。
  - `vus_typeof`：返回结构化值的类型名（整数/浮点/字符串/布尔/列表/字典/空）。
- **日志**：见 8.9。

### 8.12 栈追踪

`vus_stack_depth` + `vus_stack_frames[VUS_MAX_STACK_DEPTH=256]` 构成调用栈记录，`vus_stack_push/pop/print` 供错误诊断（调试模式下启用）。

---

## 九、插件系统实现

VUS 拥有层次化的插件体系：`.so` 编译器插件（vus_plugin）、语言插件 `.vulage`（vus_lang）、`vusx` 编译期插件，以及 `.vux` 运行期 Python 插件。前四类在编译流水线中生效。

### 9.1 编译器插件（vus_plugin）

文件：`src/vus_plugin.c` + `include/vus/vus_plugin.h`。

- 插件为动态共享库（`.so/.dll`），通过 `VusPlugin` 描述符暴露，必须且仅导出符号 `vus_plugin_entry`（`VUS_PLUGIN_EXPORT void vus_plugin_entry(VusPlugin** plugin)`）。
- `VusPlugin` 含 `name` / `version` / 生命周期回调 `init` / `run` / `cleanup` / 可选 `description`。
- `VusPluginAPI` 是编译器预填的能力表（版本号、`compile_file/compile_string/compile_string_to_exe/eval/compiler_version`），在调用插件 `init` 前填充传入，插件无需直接链接编译器。
- 加载流程：`vus_plugin_load(path)` → `dlopen` → `dlsym("vus_plugin_entry")` → 调用入口 → `vus_register_plugin` 注册。注册表为静态数组 `g_plugins[VUS_MAX_PLUGINS]`，检查数量上限与名称重复。
- 生命周期：`vus_plugin_init_all`（逐个 init，失败不阻断）、`vus_plugin_run_all`（执行各插件的 `run`）、`vus_plugin_cleanup_all`、`vus_plugin_unload_all`（逆序 cleanup 后 `dlclose`）。
- 查询/列表：`vus_plugin_find` / `vus_plugin_count` / `vus_plugin_list_all`（支持 `--list-plugins`）。

### 9.2 语言插件 `.vulage`（vus_lang）

文件：`src/vus_lang.c` / `vus_lang.h` + `include/vus/vus_lang.h`。

语言插件用于在编译**前端**将不同风格的源代码转换为标准 VUS 函数风格，确保核心编译器只需处理统一语法。机制与 vus_plugin 类似：

- `.vulage` 共享库导出 `vus_lang_entry`，注册 `VusLangPlugin`（含 `name`、`preprocess` 回调）。
- `vus_lang_load_from_config(name, project_dir)` 依据配置中的语言插件名在标准路径 `plugins/lang/<名称>/<名称>.vulage` 查找加载（返回 0 成功 / -1 未找到配置 / -2 加载失败）。
- `vus_lang_preprocess(name, source, &out)` 在 `vus_abi.c::compile_source` 中于词法分析前调用，输出预处理后的源码。
- `vus_lang_active_name` 返回当前激活语言插件名，用于编译时信息展示。

### 9.3 VUSX 编译期插件

文件：`src/vus_vusx.c` / `vus_vusx.h` + `include/vus/vus_vusx.h`。

`.vusx` 插件是用 VUS 编写的功能扩展（打包自 `.vux`/源码），在**编译期**由编译器自动解析并编译为 `.o`，再链接进最终可执行文件：

- 用简易 JSON 解析查找 `vusx.json` 中的 `导出` 等字段（`json_find_string` 等）。
- `vus_vusx_append_objects` 将 vusx 依赖的 `.o` 文件路径追加到 GCC 命令字符串（通过 `vus_config_load` 的 `vusx_deps` 数组驱动，加入 `vus_compile_c` 的 `extra_objects` 参数）。

### 9.4 运行期 `.vux` Python 插件

`.vux` 是用 Python 编写的运行期插件（子进程 `vux_plugin_manager.py` 执行，或 `VUS_USE_PY` 下通过嵌入式 Python 进程内调用），详见运行时库 8.11 插件运行时函数。其加载模型在生成代码中以内建函数形式触发，不参与编译流水线的编译期阶段。

### 9.5 四层插件加载时序总览

```
源头码
   │  vus_lang_preprocess（.vulage 语言插件）→ 规范化源码
   ▼
vus_lexer → vus_parser → AST
   │
   ▼
vus_generate_c（.so 编译器插件 vus_plugin 提供编译期钩子/内建映射）
   ▼
C 源码 → vus_compile_c（追加 .vusx 编译期插件的 .o 到 GCC 命令，实现编译期扩展）
   ▼
可执行文件（运行时调用 .vux Python 插件：子进程，或 VUS_USE_PY 嵌入）
```

---

## 十、C ABI 与嵌入式

文件：`include/vus/vus_abi.h` + `src/vus_abi.c`。

提供稳定的 C ABI，供外部程序（C/C++、Python ctypes、Ruby 等）嵌入 VUS 编译器。所有函数以 `extern "C"` 导出。

### 10.1 版本信息

- 宏 `VUS_ABI_VERSION_MAJOR/MINOR/PATCH`（当前 `1.0.0`）。
- `vus_abi_version` 返回编码为 `0xMMmmpp` 的整数；`vus_abi_version_string` 返回 `"1.0.0"`。

### 10.2 核心接口实现要点

| 接口 | 实现要点 |
|------|----------|
| `vus_compile_file(path, config)` | 读文件 → `compile_source` 流水线 → 写 C 文件 |
| `vus_compile_string(source, config)` | 从内存源码编译（不读文件），C 文件名取 source 前 16 字符 sanitize 后命名 |
| `vus_compile_string_to_exe` | = 字符串编译 + GCC 链接 |
| `vus_eval(code, config, output)` | 将代码包装为完整程序 → 编译 → 运行子进程 → 捕获 stdout 到 `output`；**每次调用启动一个子进程，开销较大**；输出截断为 4096 字节内 |

- 内部辅助 `read_file` 读文件全部内容；`compile_source` 为整个编译流水线的核心实现（预处理 → 词法 → 语法 → 生成 C → 写文件）。
- `vus.h` 的 `VusResult` 结构体含 `success`、`error_msg[512]`、`c_output_path[1024]`、`exe_output_path[1024]`，供调用方读取编译结果；`vus_compile_to_c` / `vus_compile_to_exe` / `vus_run` 为用户层文件级入口。

---

## 十一、APK 打包实现

文件：`src/vus_apk.c` / `src/vus_apk.h`。将 VUS 代码编译为 Android APK；有 NDK 时自动交叉编译，无 NDK 时生成项目结构并提示手动构建。

### 11.1 NDK 检测

`vus_apk_detect_ndk(ndk_path)` 依次探测：显式路径 → `ANDROID_NDK_HOME` → `ANDROID_HOME` 下常见 NDK 版本目录 → 默认路径（含 `$HOME/Android/Sdk/ndk-bundle`、`/opt/android-ndk` 等）。未找到返回 NULL。

### 11.2 打包步骤

`vus_compile_to_apk(file, config, ndk_path, app_name, output_dir)`：

1. `vus_compile_to_c` 将 VUS 编译为 C。
2. 确定应用名与包名：包名取反向域名 `com.vus.<应用名>` 并小写化。
3. 创建目录结构：`<build_dir>/apk_<应用名>/` 下含 `jni/`、`java/<包名路径>/`。
4. 复制生成的 C 代码到 `jni/vus_app.c`，并将 `int main(void)` / `int main(int argc` **重命名为 `int vus_main`**（`strstr` 替换），同时嵌入运行时源文件 `libvus_rt.c`。
5. **生成 JNI 桥接**：`gen_jni_bridge(pkg_name)` 把包名点号改下划线，导出 `Java_<com_example_app>_MainActivity_runVus(JNIEnv*, jobject)`；内部用 `fmemopen` 捕获 stdout，调用 `vus_main()`，返回 `NewStringUTF`。
6. **生成构建脚本**：`gen_android_mk`（`LOCAL_MODULE=vus_app`，`LOCAL_SRC_FILES=vus_app.c libvus_rt.c jni_bridge.c`，`-DVUS_HAVE_CURL` 等）与 `gen_application_mk`（`APP_ABI=arm64-v8a armeabi-v7a x86_64`，`APP_PLATFORM=android-21`，`APP_STL=c++_static`）。同时生成 `AndroidManifest.xml` 模板（含 `MainActivity`）。
7. 有 NDK 时调用 `ndk-build` 实际构建，否则生成项目骨架提示手动构建。

要点小结：**JNI 桥接**作为 Java 与原生 VUS 的分界；通过 `main → vus_main` 替换实现运行时库的嵌入复用；Android.mk/Manifest/Application.mk 由模板生成，编译宏与 ABI 在此层约定。

---

## 十二、构建系统

文件：`Makefile`（GNU Make）。

### 12.1 主要目标

| 目标 | 作用 |
|------|------|
| `all` | 默认；构建 `vus` 编译器 + `libvus_rt.a` 运行时库 |
| `vus` | 链接编译器（`main/token/lexer/parser/ast/generator/config/vus_abi/vus_plugin/vus_lang/vus_vusx/vus_apk.o`）+ `-lm -ldl` |
| `libvus_rt.a` | 归档运行时（`libvus_rt.o`、`vus_coro.o`、EasyLogger `elog.o/elog_utils.o/elog_port.o`，及可选 GuiLite） |
| `test` / `run-tests` | `./vus test` / `cd tests && bash run_tests.sh` |
| `run` / `build-c` / `build-exe` | 便捷运行 / 仅生成 C / 生成可执行文件 |
| `install` / `uninstall` | 安装到 `/usr/local/bin/vus` + 共享脚本/头文件 |
| `format` | 用 `clang-format` 格式化 `src/rt` 下的 C/H |

### 12.2 编译宏

- `CFLAGS = -Wall -Wextra -g -O2 -std=c11 -Wno-format-truncation`（GCC 语句表达式等扩展在实际编译中以 GNU C11 级别可用）。
- **libpython 检测**：`python3-config` 可用时定义 `-DVUS_USE_PY`（启用嵌入式解释器），并注入 `-DVUS_PY_SONAME="libpythonX.Y.so"` 匹配 soname；不可用则仅编译子进程方案。
- 协程模块单独编译，`-Wno-format-truncation` 下避免 `libvus_rt.c` 中 inline asm 与 C11 冲突。

---

## 十三、开发指南要点

### 13.1 代码规范

- 纯 C（`.c/.h`）实现，`-std=c11`，遵循 `-Wall -Wextra` 严格告警；跨平台兼容（Linux / Android Termux / 嵌入式 ARM）。
- 命名：编译器内部函数前缀 `vus_`；生成的 C 符号统一 `vus_` 前缀；运行时对象首字段约定为 `int ref`。
- 头文件以 `VUS_*_H` 宏防重复包含；对外接口放 `include/vus/*.h`，内部接口放 `src/*.h`。
- 编译目标在 Makefile 中集中管理依赖头文件，新增源文件需同步 SRCS。

### 13.2 测试体系

- `tests/` 下以 `.vus` 用例覆盖语言特性（算术、位运算、控制流、函数、递归、泛型、结构体、异常、线程/协程、日志等）。
- `error_tests/` + `run_error_tests.sh` 覆盖错误路径（缺冒号、未闭合字符串/列表、泛型未闭合、结构体无名、括号不匹配等）。
- 运行方式：`make test`（调用 `./vus test`）或 `make run-tests`（直接跑 `run_tests.sh`）。

### 13.3 提交与协作风格

- 提交信息聚焦"为何"；文档与设计先落 `docs/designs/`、`docs/plans/` 再实现（见 multi-AI 协作与消息转发规范文档）。
- 保持仅 read-only 的审查类任务不触碰源码；变更类任务严格按既有结构与命名模式扩展。

---

## 附、已知缺陷与限制

> 本节如实列出源码中存在的局限，供使用者与后续维护者知悉：

1. **函数/局部变量作用域限制**：局部变量在函数顶部统一声明（`gen_collect_locals` 扫描收集），通过 `is_local` 标记区分。函数内对未显式局部声明的变量赋值、以及特定嵌套场景的作用域解析存在局限（依赖收集策略，未实现完整词法作用域）。
2. **引用计数实现简化**：`vus_unref` 归零后仅 `free(obj)`，不递归释放容器内部成员；`VusDict`、`VusList` 嵌套结构需调用方正确管理，注释明确"由调用方确保正确释放"。存在潜在内存泄漏风险（尤其结构化容器组合使用场景）。
3. **布尔表示的注意点**：布尔值是运行时字符串 `"true"/"false"`，条件判断依赖 `strcmp` / `vus_to_int`。逻辑运算与非布尔上下文混用、以及把字符串直接当布尔用时，语义可能偏离类型系统预期。
4. **`vus_eval` 开销**：每次求值都启动一个子进程，且输出截断为 4096 字节，不适合高频/大数据量求值。
5. **字典遍历缺失**：v0.1 未提供字典遍历接口（`VusDict` 无迭代 API），相应地 JSON 生成对字典仅返回占位 `{}`。
6. **句柄表上限**：线程/协程句柄注册表各 64 槽，溢出返回 `"-1"` 并回收；长时间大量创建线程/协程可能触发上限。
7. **网络依赖可选**：HTTP/TUI 中网络功能依赖 `VUS_HAVE_CURL`；未启用时相应内建返回空串/`-1`。
8. **嵌入式 Python 降级**：未定义 `VUS_USE_PY`（无 `python3-config` 或无 libpython）时，`vus_plugin_run_vux_inproc` 回退子进程，`vus_json_parse/generate/vus_plugin_run_vux_json` 返回空/`NULL`。

---

*本文档由对 `/workspace/vus` 源码的审阅整理而成，覆盖编译器整体架构与词法、语法、AST、代码生成、配置、运行时、插件、ABI、APK、构建等模块的实现机制。*