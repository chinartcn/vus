# VUS 贡献者指南（CONTRIBUTING）

> 让任何新协作者最快看懂"项目现在是什么样子、我改哪里、怎么验证、怎么提交"。
> 本文基于仓库真实代码（`src/ rt/ include/ tests/ examples/`）与 `docs/` 编写，2026-09-05 校订。
> 协议：MIT。权威细节按需查 `docs/STATUS.md`、`docs/ARCHITECTURE.md`、`docs/COMPILER_GUIDELINES.md`、`docs/LANGUAGE_REFERENCE.md`。

---

## 0. 一句话定位

VUS 是一个用 C 写成的**编译型中文编程语言**：源码 → 词法 → 语法 → AST → **生成 C** → GCC/Clang → 原生可执行 / Android APK。
"中文写代码、转 C 得原生级性能、自带 GUI(图形_*)与 Android 组件流(VUA/界面_*)"。

---

## 1. 环境与依赖

| 依赖 | 用途 | 缺失后果 |
|---|---|---|
| Linux（本机/容器/WSL） | 主平台 | — |
| `gcc` + `make` | 编译器本体自有程序 | 无法构建 |
| `libcurl-dev`（可选） | `网络_*`（需 `-DVUS_HAVE_CURL`） | 网络函数返回空，不发送请求 |
| `libpng/freetype`（可选） | `图形_*` GUI（X11 显示） | 无 GUI 时 `make vus` 仅编编译器+LSP |
| `python3`（可选） | `.vux` 插件 / `JSON_*` 进程内（`VUS_USE_PY`） | 插件/JSON 走子进程回退 |
| Android SDK/NDK（可选） | `vus build --apk`、examples/vua-android | 仅 APK 场景需要 |

> 最简可跑：`gcc make` 即可打通 `make && ./vus run hello.vus`。

---

## 2. 五分钟上手

```bash
git clone https://gitee.com/rtccn_mc/vus.git && cd vus
make                                        # 编出 ./vus
./vus --version                             # 版本
echo '打印("你好，世界！")' > h.vus
./vus run h.vus                             # 运行
./vus build --c-only h.vus                  # 看生成的 C（在 构建/）
./vus run tests/test_hello.vus              # 跑单个用例
make test                                   # 全量测试（tests/）
bash tests/run_tests.sh                     # 批量跑 .vus 用例
bash tests/run_error_tests.sh               # 错误路径用例
```

GUI 需要显示环境（headless 会因无 X11 **段错误而非逻辑错误**，这不算回归；见 STATUS 说明）。

---

## 3. 项目结构地图

```
vus/
├── src/           编译器内核（C）            → 我先看哪？
│   ├── main.c     CLI 入口 + 子命令分发        √ 入口
│   ├── token.[ch] Token 类型、关键字枚举
│   ├── lexer.[ch] 分词、中英关键字查表、缩进、UTF-8 中文标识符
│   ├── parser.[ch] 递归下降 → VusAstProgram
│   ├── ast.[ch]   tagged-union AST 节点
│   ├── generator.[ch] 生成 C、引用计数插入、ident sanitize、内置函数映射
│   ├── config.[ch] 解析 vus.json
│   ├── vus_abi.c  嵌入式 C ABI
│   ├── vus_plugin/vus_lang/vus_vusx  四层插件
│   ├── vus_apk.c  VUS→APK 打包
│   ├── vus_chart.c 音游谱面、vus_vaz.c 模板、lsp/ 语言服务器
├── rt/            运行时库（C）
│   ├── libvus_rt.[ch]  VusString 引用计数、列表/字典、闭包、错误链、内置函数运行时
│   ├── vus_coro.[ch]   协程（setjmp/longjmp+换栈，Android/Termux 可用）
│   ├── vua.c           VUA 组件流（界面_*、屏栈、渲染树）
│   ├── guilite_bridge.c 桌面 GUI（GuiLite+ X11/Xft 中文）
│   └── yyjson/ easylogger/ 等第三方
├── include/vus/   公共 C ABI 头（vus.h/vus_abi.h/vus_plugin.h/vus_lang.h/vus_vusx.h）
├── tests/         86 个 .vus 用例 + error_tests/ + run_tests.sh
├── examples/      示例（Hello/GUI/VUS-OS/插件/Android VUA）
├── examples/vua-android/  完整 Android 工程（VUA 渲染器/Java 桥/DEX 插件/热更新）
├── scripts/       构建/发布/vux 插件脚本
├── plugins/       语言插件(易语言/.vulage)与功能插件示例
├── docs/          40 文件文档体系
└── Makefile / install.sh / vus.json
```

代码量参考：`src/` ≈14,910 行（generator 4104 / parser 2580 / main 1256 / lexer 1157 …），`rt/` ≈8,469 行。

---

## 4. 编译流水线（改前必懂）

```
.vus 源码
  → 可选：.vulage 语言插件预处理（词法前）
  → lexer 词法 → Token 流（行号/列、INDENT/DEDENT）
  → parser 语法 → AST（无独立语义分析阶段；类型在 AST/生成期"酌情"处理）
  → generator 生成 C（引用计数、中文名 sanitize _XXXX、GNU 语句表达式调用）
  → GCC/Clang 链接 → 可执行（可选 NDK → APK）
```

关键实现事实（避免踩坑）：
- 标量统一是 `VusString*`（首字段 `int ref`）；生成器在赋值/循环/传参/返回处插 `vus_ref`/`vus_unref`。
- 函数调用用 GNU `({...})` 语句表达式 + `_vus_args[0]` 做返回槽。
- 中文标识符 sanitize：UTF-8 转 `_XXXX` 十六进制，全符号加 `vus_` 前缀（`gen_sanitize_name`）。
- 内置函数在 `generator.c` 的 `gen_expr_call` 里按 `strcmp(call->func_name,"打印")==0` 映射到运行时函数。

---

## 5. "想改 XX → 去哪个文件" 速查表（最常用）

| 你想做的事 | 主改文件 | 辅助/说明 |
|---|---|---|
| 加一个**关键字** | `token.h`(枚举) + `lexer.c`(`vus_keyword_lookup` 中文映射) | parser 用新 token 时同步 |
| 加**语法/语句/表达式** | `parser.c` + `ast.h`/`ast.c`(节点+构造/销毁) | 大改动参考 f-string/多返回的既有实现 |
| 加一个**内置函数** | `generator.c`(`gen_expr_call` 映射) + `rt/libvus_rt.c`(实现该名函数) + `libvus_rt.h`(声明) | Technical先写 test_*.vus 回归；GI/GUI 函数映射也在 generator |
| 改**运行时引计/字符串/列表/字典** | `rt/libvus_rt.[ch]` | 看 `vus_ref/vus_unref/vus_var_set/vus_string_*/vus_list_*/vus_dict_*` |
| 改**协程/线程** | `rt/vus_coro.[ch]` | 真 await 在 `vus_coro_await_handle` |
| 加 **CLI 子命令/参数** | `src/main.c` | 分发到现有子命令风格 |
| 改 **APK 打包** | `src/vus_apk.c` + `examples/vua-android/scripts/build_apk.sh` | 后者是 APK 实际构建链 |
| 改 **GUI `图形_*`** | generator 映射 + `rt/guilite_bridge.c`(+平台层) | headless 无 X11 会段错误，需真机/带显环境验证 |
| 改 **VUA `界面_*`** | `rt/vua.c`（native）+ `examples/vua-android/`（Java 消费） | `.vua` 是 JSON 渲染树 |
| 加 **插件（.vux/.vusx/.vulage/.vaz）** | `src/vus_plugin.c`/`vus_vusx.c`/`vus_lang.c`/`src/vus_vaz.c` | 看 `docs/PLUGIN_USAGE.md` |
| 改 **安装脚本** | `install.sh` | 含可选 Android SDK 下载逻辑 |
| 改 **文档** | `docs/` | 见 §9 文档同步要求 |

> 例：加 `我的函数()` → 1) `tests/test_myfunc.vus`；2) generator 加映射到 `vus_myfunc`；3) `libvus_rt.c` 实现；4) `make && make test`。

---

## 6. 开发工作流（推荐）

1. **先 TDD**：修 Bug 或加功能前，先在 `tests/` 写一个能复现/验证的 `test_*.vus`（并可选在 `error_tests/` 写错误路径）。
2. 改代码 → `make` → `./vus run tests/test_你的.vus` 快验证。
3. 全量回归：`make test`（调用 `vus test`）+ `bash tests/run_tests.sh` + `bash tests/run_error_tests.sh`。
4. 若涉及 APK/GUI：在真机或带 X11 环境验证（headless 的 GUI 段错误不算回归）。
5. **提交规范**（仓库历史统一）：
   - 每个提交只做一件事、小而聚焦。
   - 前缀式信息（历史即约）：`feat: ...` / `fix(模块): ...` / `docs: ...` / `test: ...` / `perf: ...` / `refactor: ...`。
   - 中文描述，说明"为什么"而非仅"改了什么"。
6. 同步文档：语言语法→`LANGUAGE_REFERENCE.md`；内置函数→`API_REFERENCE.md`/`STATUS.md`；架构改动→`ARCHITECTURE.md`；重大特性→更新 `STATUS.md` 已实现/待办。

代码风格：类型 `VusXxx` 驼峰、函数 `vus_xxx_yyy` 蛇形、宏 `VUS_XXX` 全大写；每个源文件头部注释；头文件防重；`malloc` 配 `free`，输出指针标 `/* out */`。详见 `COMPILER_GUIDELINES.md`（架构先行/不盲从/正确性>清晰>维护>诊断>性能/小步快跑）。

---

## 7. 现况速览（贡献者可据此选题）

### 已实现（稳定）
- 语言：中英双风格、类型、结构体、列表/字典、引用计数、异常（尝试/抛出）、**多返回值 / 真 await / 闭包-函数一等公民 / f-string / 复合赋值 / 字典遍历**（09-05 补强）。
- 子命令：`run/build --c-only|--exe|--apk / init / test / lang / vux / vusx / vaz / chart / lsp / update`。
- 平台：GUI `图形_*`（GuiLite+Xft 中文）、Android VUA `界面_*` + APK（多 ABI）、TUI、音游 `chart`、四层插件、C ABI、热更新与 Android SDK 镜像。
- 性能：生成 C 体积 −45%、编译 −29%、热循环 +47%（`docs/PERFORMANCE.md`）。

### 已知限制 / 待办（高价值选题，见 `STATUS.md` §"测试中/待开发"）
| 选题 | 现状 | 难度 |
|---|---|---|
| **泛型真实语义** | `函数名<T>()` 仅注释注入，无实例化/类型检查 | 中 |
| **完整词法作用域 / 块级作用域** | 现为"函数顶收集式"局部变量 | 中 |
| **类型注解强校验** | 注解仅记录，不强制（动态类型） | 中 |
| **异常类型系统** | 现为"消息字符串比对"捕获 | 低 |
| **完整包管理** | vus.json 已解析、`导入` 已实现，多文件/包待完善 | 中 |
| **JSON/网络完善（`VUS_USE_PY`/`VUS_HAVE_CURL` 回退增强）** | 默认回退子进程/空 | 低-中 |
| **更多内置函数**（如旧式别名 `长度/拼接/替换` 接线到 `文本_*`/`列表_*`） | 未接线 | 低 |
| **移动端/终端打磨**、`--apk` 离线、CI 回归矩阵、SDK 镜像多平台自动化 | 进行中 | 中 |

---

## 8. 常见坑（别人踩过的）

- **布尔是字符串**：`"true"/"false"`，条件靠 `strcmp`/`vus_to_int`；`返回 0` 返回 `"0"`，要 `返回 0==0` 才得 `"true"`。
- **局部变量作用域**：函数内变量在函数顶统一收集声明；改全局变量需 `全局 名` 声明。
- **网络/JSON 依赖**：`网络_*` 需 `VUS_HAVE_CURL`；进程内 `JSON_*`/`.vux` 需 `VUS_USE_PY`，否则回退（不报错但功能弱）。
- **句柄上限**：线程/协程注册表各 64 槽。
- **GUI 需显示**：headless 跑 `test_gui_*` 会段错误，是环境非逻辑错误。
- **`断言/退出/长度/拼接/替换` 等旧名未接线**：别在被不支持的函数上浪费时间，用 `文本_*`/`列表_*`/`字典_*`。
- **引用计数仍是手动插点**：改生成器时注意在赋值/返回/传参处的 ref/unref 平衡。

---

## 9. 文档体系与同步职责

先读 `docs/PROJECT_BRIEFING.md`（概览）、`docs/ARCHITECTURE.md`（机制）、`docs/STATUS.md`（现状/待办）、`docs/COMPILER_GUIDELINES.md`（规范）。
贡献涉及新语法/新函数/新模块时，请同步：`LANGUAGE_REFERENCE.md` → `API_REFERENCE.md` → `STATUS.md`；新增面向使用者的能力时并在 `docs/TUTORIAL.md`/`tutorials/` 补例子。
另有两份辅助文档：`docs/VUS发展报告.md`、`docs/VUS分析报告.md`（发展/对比概况，供快速了解）。

---

## 10. 你还可以做的非代码贡献

- 写更多 `tests/*.vus` 回归（尤其覆盖你踩过的 bug）。
- 补齐文档示例 / 中文化用词 / 教程分册。
- 用 `vus build --apk` 跑真实 Android 设备，反馈体验（仓库历史显示这类反馈是迭代主驱动力）。
- 打包/镜像多平台（linux-x86_64 / arm32 / aarch64）构建产物，维护 `android-sdk`/`android-ndk` 发布。

> 一切以"真实跑一遍"为准：改完 `make && make test`，能过、文档对得上，就是一个好的贡献。