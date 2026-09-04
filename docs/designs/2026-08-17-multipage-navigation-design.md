> 文档版本：v1.0_apk（APK 功能时代）
> 最后更新时间：2026-09-04


# VUS 多页导航 —— 设计文档（页面栈 + 外部 .vus 页面 import）

日期：2026-08-17
状态：已实现并基本验证通过
相关批次：承接既有《2026-08-17-guilite-md-line-scroll-design.md》，该批拆出「多页导航（含外部 .vus import 页）」作为独立批次，本设计文档补全其方案沉淀。

## 1. 背景与目标

### 1.1 为什么需要多页导航

既有 VUS 的 GUI 能力（`图形_*` 内建函数 + 控件库）全部面向「单屏画布」：脚本在函数 `主程序` 内自上而下绘制一次画面，交互通过 `图形_取事件`/命中检测轮询。这无法支撑更复杂的应用形态：

- **多界面切换**：设置页 / 详情页 / 关于页等需要进入、返回、记忆栈内前后文。
- **模块化拆分**：单文件脚本难以维护，期望把每页抽成独立 `.vus` 文件、按需接入。
- **回调复用**：VUS 没有一等函数，函数指针不能作值传递，需要一套「约定式」机制让平台层能动态调用脚本定义的函数。

因此需要一个**运行时页面栈**（管理当前页与历史）配合**约定式页函数**（`页_<名>()`）以及**编译期 import 源码展开**（把外部 `.vus` 页面并入单次编译产物），共同实现多页导航。

### 1.2 外部 .vus 页面如何接入

接入采用「两条已有/新增机制拼装」的设计，不引入复杂的动态加载：

1. **运行时页面栈 API**：`图形_页面_打开/返回/当前/绘制`，维护一个页名栈；`绘制` 时通过 `dlsym` 反查当前页对应的约定函数并调用。
2. **编译期 import 源码展开**：复用语言既有 `导入 X` / `从 X 导入` 语法，编译时把 import 行**递归替换**为对应 `X.vus` 的源码，使外部页面的全部函数（含约定式 `页_关于` 等）**合并进同一次编译产物**，从而其符号能像主脚本函数一样被 `dlsym(RTLD_DEFAULT, ...)` 反查到。

外部 `.vus` 页面的页函数因此和主脚本内定义的页函数在 ABI 上完全等价——它们最终都编译为同一可执行文件中的 `void vus_<sanitized>(void*)` 全局符号。

## 2. 页面栈机制

### 2.1 数据结构

桥接层 `rt/guilite_bridge.c`（阶段E）维护一个页名栈：

| 成员 | 类型 | 说明 |
|------|------|------|
| `s_pages[VUS_PAGE_STACK_MAX]` | `char*` | 每层登记页名的深拷贝（`strdup` 语义的 `page_dup`），避免依赖 POSIX 特性宏声明 |
| `s_page_top` | `int` | 栈顶索引；`-1` 表示空栈，`0` 为栈底（首页） |
| `VUS_PAGE_STACK_MAX` | 宏 | 栈深度上限，值为 `32` |

```c
#define VUS_PAGE_STACK_MAX 32
static char* s_pages[VUS_PAGE_STACK_MAX];
static int   s_page_top = -1;   /* -1 = 空栈 */
```

### 2.2 四个 API 语义

桥接函数由 generator 把下表中 `图形_*` 内建调用映射而来（`src/generator.c` 阶段E，约 L821–845）：

| vus 端内建 | C 桥接函数 | 语义 |
|------------|-----------|------|
| `图形_页面_打开(名)` | `vus_gui_page_open` | 把名为「名」的页置为当前页，返回 `"1"/"0"` |
| `图形_页面_返回()` | `vus_gui_page_back` | 弹栈回到上一页，返回 `"1"/"0"` |
| `图形_页面_当前()` | `vus_gui_page_current` | 返回当前页名字符串；无页返回空串 |
| `图形_页面_绘制()` | `vus_gui_page_draw` | `dlsym` 当前页对应 `页_<名>` 函数并调用，返回 `"1"/"0"` |

**打开的去重回跳规则（核心）**：`vus_gui_page_open` 首先自栈底向上遍历查找「名」：

- 若该页**已在栈中**：弹出其上方所有层（释放并置空每层 `s_pages[j]`），令 `s_page_top = i` 回到该页。即「重复打开某页」不会产生重复层，而是回跳并清理它之上的全部历史。
- 若该页**不在栈中**：压入栈顶（`s_page_top + 1 >= VUS_PAGE_STACK_MAX` 时视为满栈，返回 `"0"` 拒绝；页名拷贝失败同样返回 `"0"`）。
- 名称为空 / 为 NULL：直接返回 `"0"`。

```c
VusString* vus_gui_page_open(const char* name) {
    if (!name || !*name) return vus_string_new("0");
    /* 已在栈中：弹出其上所有层，回到该页 */
    for (int i = 0; i <= s_page_top; i++) {
        if (strcmp(s_pages[i], name) == 0) {
            for (int j = s_page_top; j > i; j--) { free(s_pages[j]); s_pages[j] = 0; }
            s_page_top = i;
            return vus_string_new("1");
        }
    }
    /* 不在栈中：压栈（超上限忽略） */
    if (s_page_top + 1 >= VUS_PAGE_STACK_MAX) return vus_string_new("0");
    ... /* page_dup + 压栈 */
}
```

**栈底返回 vs 未初始化行为**：

- `vus_gui_page_back`：`s_page_top <= 0`（即空栈或已在栈底首页）时返回 `"0"`；否则释放栈顶、`s_page_top--` 回到上一页并返回 `"1"`。
- `vus_gui_page_current`：`s_page_top < 0`（空栈）或栈顶层为 NULL 时返回**空串** `""`，否则返回当前页名。
- `vus_gui_page_draw`：`s_page_top < 0` 时直接返回 `"0"`（未初始化/空栈，不调用、不崩溃）。

### 2.3 绘制如何通过 dlsym 反查约定函数

`vus_gui_page_draw`：

1. `s_page_top < 0` → 返回 `"0"`。
2. 取栈顶页名，构造约定函数全名 `页_<名>`（`VUS_PAGE_FN_PREFIX` = `"页_"`），经 `vus_gui_sanitize_name` 生成规范化符号名，前缀 `vus_` 拼出目标符号：
   ```c
   snprintf(name, sizeof(name), "页_%s", s_pages[s_page_top]);
   vus_gui_sanitize_name(name, san, sizeof(san));
   snprintf(sym, sym_size, "vus_%s", san);
   ```
3. `dlsym(RTLD_DEFAULT, sym)` 反查并强转为 `void(*)(void*)`；**找不到则返回 `"0"`（不崩溃）**。
4. 找到则以 `VusString* args[1] = { 0 };`（仅返回槽，无参数）调用 `fn(args)`，返回 `"1"`。

该机制与 `事件_点击` 回调（`vus_gui_platform_emit_click`）共用同一 dlsym 反查模式，只是页面绘制无参数、只需返回槽。

## 3. 约定式页函数

### 3.1 符号生成规则

对每页，脚本定义一个约定式函数 `页_<名>()`。编译器 `gen_function`（`src/generator.c` L2135）对任意用户函数：

- 把函数名经 `gen_sanitize_name` 规范化为 `san`；
- 生成全局非 static 符号 `void vus_<san>(void*)`，其中 `void*` 为 `VusString**`，`[0]` 是返回值槽、`[1..]` 是参数。

因此 `页_关注` 之类的页函数会编译为：

```
定义 页_关于():  ...
  ↓
void vus_<sanitize("页_关于")>(void* _args) { ... }
```

`vus_gui_sanitize_name`（桥接层）与 `gen_sanitize_name`（编译器）以相同规则把中文（UTF-8 多字节，`0x80` 以上）换算为 `_XXXX`、ASCII 字母/数字/下划线原样保留；两者结果必须一致，桥接层才能 `dlsym` 到编译器生成的确切符号。

### 3.2 与 `事件_点击` 同一调用约定

页函数与 `事件_点击`/`事件_按键` 使用**完全相同的调用约定**：

- 目标符号均为 `void vus_<sanitized>(void*)`；
- `void*` 解引用为 `VusString**`；
- `[0]` 为返回值槽（页绘制时置 `{0}` 即返回空）；
- 基座层调用方负责释放参数（VUS 函数体内只 `vus_ref`，不负责 `unref` 参数）。

### 3.3 一致性约束

`rt/guilite_bridge.c` 的 `vus_gui_sanitize_name` 与 `src/generator.c` 的 `gen_sanitize_name` **必须保持同步修改**：任何一方调整规范化规则（如新增字符映射、前缀规则），另一方不同步就会导致 dlsym 反查到空符号、页面/事件回调失效但无编译报错。这是约定式机制的隐性维护成本。

> 注：两者存在一处细微差异——编译器 `gen_sanitize_name` 对「数字开头」的名称会补一个 `_` 前缀，而桥接层 `vus_gui_sanitize_name` 无此前缀分支。由于页函数名恒以 `页_`（非数字）开头，该差异不影响页面反查；但若未来出现其他数字开头的中文回调名，需评估此差异。（待确认：是否需要统一两端行为）

## 4. import 源码展开方案

### 4.1 编译期替换流程

`src/vus_abi.c` 提供公共函数（声明于 `include/vus/vus_abi.h`）：

```c
char *vus_source_expand_imports(const char *source, const char *source_name);
```

展开语义：

- 逐行扫描，`import_line`（L80）识别 **`导入 X`**、**`import X`**、**`从 X 导入`**、**`from X import`** 四类导入语句，解析出模块名。
- 在 `compile_source` 流水线中，**词法分析之前**把识别的 import 行就地替换为对应 `X.vus` 的完整源码文本；其余行原样保留。
- 展开是**递归的**：`expand_rec` 读取模块源码后，以模块所在目录为新 `base_dir` 继续展开其自身的 import 行。

### 4.2 为何能支撑外部 .vus 页面 dlsym 反查

关键点在于「展开发生在编译（词法分析）之前且作为整段源码送入编译器」，因此：

- 外部页面的函数定义（如 `页_关于`）与主脚本函数**在同一次编译中**被 `gen_function` 生成全局符号；
- 最终链接只产生**一个可执行文件**，所有这些函数都作为非 static 符号存在，`dlsym(RTLD_DEFAULT, ...)` 即可反查到；
- 页面栈无需感知「页函数来自哪个文件」——`图形_页面_绘制` 只按当前页名反查符号，来源被 import 展开透明抹平。

### 4.3 循环 import 防护与降级

- **去重（防循环）**：`expand_rec` 维护一个已展开路径集合 `VusImportSet`（`set_has`）。同一文件只展开一次；已展开过则跳过（既不重复展开，也不因循环引用无限递归）。
- **深度上限**：集合大小上限 `VUS_IMPORT_MAX_DEPTH` = `32`；超限后 `set_add` 不再记录，作为额外兜底。
- **找不到模块（降级）**：解析出的模块在约定路径下找不到时，**保留原 import 行**（`grow_add(out, line, "\n")`），交由后续编译器正常处理（语法层面它本就是合法关键字，即使不展开也不会导致硬错误）。文件读取失败同样保留原行。
- **路径解析顺序**：优先「主脚本同目录」（`base_dir/<mod>.vus` → `base_dir/<mod>`），再「当前工作目录」（`<mod>.vus`）。

### 4.4 流水线入口

`vus_source_expand_imports` 从 `vus_abi.c` 的 `compile_source` 中调用，`compile_source` 被 `vus compile` / `vus run` / `vus build` 等多条编译流水线复用，保证在所有入口 import 展开行为一致。

## 5. 外部 .vus 页面接入流程

以「外部关于页」为例，三步接入：

**① 在外部模块定义页函数**（`tests/test_pages_ext.vus`）：

```vus
#// 外部 .vus 页面模块
定义 页_关于():
    打印("关于页绘制(外部模块)\n")
```

**② 在主脚本 import 接入**（`tests/test_gui_pages.vus`）：

```vus
导入 test_pages_ext
```

编译时该行被替换为 `test_pages_ext.vus` 的源码，`页_关于` 并入编译产物。

**③ 打开 / 绘制**：

```vus
r = 图形_页面_打开("关于")   # 页名 "关于" 对应约定函数 页_关于
d = 图形_页面_绘制()          # dlsym("vus_<sanitize("页_关于")>") 并调用
```

页面栈只按页名工作，页函数来源（主文件或 import 的外部文件）对页面层透明。

## 6. 已验证场景

测试文件：

- `tests/test_gui_pages.vus` —— 多页导航主测试（headless）。
- `tests/test_pages_ext.vus` —— 外部 `.vus` 页面模块（提供 `页_关于`）。

覆盖场景（对应实现行为）：

| 场景 | 用例 |
|------|------|
| 页面打开 | `图形_页面_打开("首页")` 返回 `"1"` |
| 当前页查询 | `图形_页面_当前()` 返回 `"首页"`，再打开 `"设置"` 后返回 `"设置"` |
| 页面绘制 | `图形_页面_绘制()` 调用当前页函数，返回 `"1"` |
| 去重回跳 | 打开已存在的 `"首页"`，弹出上方 `"设置"` 层，当前回到 `"首页"`（`"去重后当前=首页"`） |
| 外部页面绘制 | 打开 `"关于"`（来自 import 的外部模块）并绘制，验证跨文件 `dlsym` 反查 |
| 页面返回 | 从外部页返回，当前回到 `"首页"` |
| 栈底返回 | 在首页（栈底）再次 `图形_页面_返回()` 返回 `"0"` |
| 级联导入 | import 行被递归展开（测试 `test_pages_ext.vus` 通过 `导入` 接入即为验证路径） |

## 7. 限制与后续扩展

- **栈深度固定**：`VUS_PAGE_STACK_MAX = 32`，超深跳转会被拒绝（返回 `"0"`）。极端深层级联可能存在上限压力（待确认：是否需要动态扩容或提升常量）。
- **页函数无参**：当前 `图形_页面_绘制` 只以 `VusString* args[1] = {0}`（仅返回槽）调用页函数，页函数不接受参数。跨页传参需通过页面向量/全局变量等其它途径。
- **绘制即调用，无默认清屏**：`图形_页面_绘制` 只调用页函数，页函数自身需负责清屏/重绘契约（与既有单屏绘制模式一致），页面切换动画/过渡不在当前实现。（待确认：是否需要「绘制前自动清屏」的内建支持）
- **事件与页面的组合**：页面绘制与 `事件_点击` 回调相互独立。用户须在 `事件_点击` 中自行判断当前页并决定是否 `图形_页面_打开/返回`；没有「页面级事件路由」的自动绑定。（待确认：是否需要更自动化的页面-事件挂钩）
- **import 展开是源码级替换**：展开结果为串接源码，同一函数名在不同模块冲突时无命名空间隔离；循环 import 依靠「按文件路径去重」而非显式语义错误。
- **未来可能的页面切换动画**：当前页面切换是瞬时替换、无转场；可基于既有派发/脏标记机制叠加（动画需新增事件与帧更新入口，超出本批）。

## 附：相关实现位置速查

- 页面栈 + 页 API + sanitize：`rt/guilite_bridge.c`（阶段E，`s_pages`/`s_page_top`/`vus_gui_page_*`/`vus_gui_sanitize_name`）
- 内建映射：`src/generator.c`（阶段E，`图形_页面_*` 约 L821–845；`gen_sanitize_name` 约 L110；`gen_function` 约 L2135）
- import 展开公共函数：`src/vus_abi.c`（`vus_source_expand_imports`/`expand_rec`/`import_line`）、`include/vus/vus_abi.h`
- 测试：`tests/test_gui_pages.vus`、`tests/test_pages_ext.vus`