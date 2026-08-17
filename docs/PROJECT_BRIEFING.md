# VUS 项目简报（Repository Briefing）

> 用途：给新成员 / AI 代理 / 协作者一个快速、紧凑的项目第一印象。
> 生成方式：结合 context-pack 思路对该仓库做一次高信号文件优先的首轮概览。
> 注意：本简报是启发式摘要；如与源码冲突，以源码为准。

---

## 一、这是什么

**VUS** 是一款面向 Linux、Android Termux、嵌入式 ARM 设备的**中文友好多范式编译型强类型编程语言**。

- **口号**：中西合璧，天下无敌
- **定位**：填补 C 与 Python 之间的空白，编译到 C → GCC/Clang → ARM/x86 原生可执行文件
- **协议**：MIT

核心卖点：不懂英文的人也能用中文写代码，同时保留对底层硬件的控制权。

---

## 二、仓库形状与入口

仓库根目录 `/workspace/vus`，核心是**一个 C 语言写的编译器和配套运行时**，外加插件生态。

| 目录 | 内容 | 优先级 |
|------|------|:------:|
| `src/` | 编译器内核（入口 `main.c`，含 lexer/parser/ast/generator/config） | 核心，先看 |
| `rt/` | 运行时库 `libvus_rt`（引用计数/字符串/列表/字典/闭包/线程协程句柄） | 核心，先看 |
| `include/vus/` | 公共 C ABI 头文件（`vus.h`/`vus_abi.h`/`vus_plugin.h`/`vus_lang.h`/`vus_vusx.h`） | 看 API 前先读 |
| `plugins/` | 插件：语言插件（易语言/）、功能插件（meilisearch 等） | 次级 |
| `scripts/` | 构建发布、vux 插件管理脚本 | 次级 |
| `tests/` | 大量 `.vus` 测试用例 + 错误测试 + 运行脚本 | 验证时看 |
| `examples/` | 示例程序（hello、gui、meili 等） | 上手指引 |
| `docs/` | 完整文档（见下） | 先读文档 |

**入口文件**：`src/main.c` — CLI 参数解析与子命令分发（run/build/test/init/vux/vusx/lang）。

---

## 三、先读哪些文档

文档已较完整，按需选择：

| 文档 | 适合谁 | 内容 |
|------|--------|------|
| `README.md` | 所有人 | 项目简介、特性、安装、快速入门、命令速查、版本规划 |
| `docs/TUTORIAL.md` | 语言使用者 | 从零开始的完整教程（12 章） |
| `docs/LANGUAGE_REFERENCE.md` | 语言使用者 | 完整语言参考（关键字/运算符/类型/语句/函数/插件运行时函数），标注了已实现 vs 未实现 |
| `docs/API_REFERENCE.md` | 插件/嵌入开发者 | C ABI 与四层插件接口完整参考 |
| `docs/ARCHITECTURE.md` | 编译器开发者 | 编译器流水线与各模块实现机制 |
| `docs/COMPILER_GUIDELINES.md` | 编译器开发者 | 编码规范、架构原则 |
| `docs/ECOSYSTEM.md` | 生态关注者 | 生态全景、插件体系、构建系统、CLI 参考 |
| `docs/STATUS.md` | 项目管理者 | 已实现功能清单、测试状态、已知 Bug/限制 |
| `设计文档.md` | 语言设计者 | 语言设计定稿（含完整语法） |

---

## 四、语言与编译核心速览

- **双语法体系**：`函数风格`（`定义`/`打印`）与 `易语言风格`（`.功能`/`.打印`），通过 `vus_keyword_lookup` 将中英文关键字映射到统一 Token 枚举。
- **编译流水线**：源码 →（`.vulage` 语言插件预处理）→ 词法 → 语法 → AST → C 代码 → GCC/Clang → 可执行文件（可选 Android NDK → APK）。
- **数据类型**：字符串(`VusString`)、整数/浮点、布尔（运行时为 `"true"/"false"` 字符串）、null、列表(`VusList`)、字典(`VusDict`)、闭包(`VusClosure`)、结构体、错误(`VusError`)。
- **内存管理**：引用计数 + 循环检测（自动），`vus_ref`/`vus_unref`。
- **并发**：`线程`/`等待线程`/`协程`/`恢复`/`让出`，基于 pthread + 协作式协程，句柄注册表各 64 槽。
- **泛型调用**：`函数名<类型>()` 语法；多线程已支持。

### 关键需注意（已知限制/未完成）
- `睡眠()` 运行时符号缺失，会导致链接失败（尚未实现）。
- 异步 `等待` 关键字仅为 token，parser 尚未实现。
- 函数内部局部变量仅支持函数顶收集式声明；布尔相关 `返回` 值使用注意。
- 网络函数依赖 `VUS_HAVE_CURL`（`-lcurl`）；进程内 Python 需 `VUS_USE_PY`。
- 引用计数存在少量内存泄漏（临时表达式结果未释放）。
- **GUI 部分尚未完成**，本文档除外，其余文档也已约定不涉及。

---

## 五、插件体系（四层）

| 层 | 扩展名 | 编写语言 | 加载时机 |
|----|--------|----------|----------|
| 源码 | `.vus` | VUS | 编译时 |
| VUS 插件 | `.vusx` | VUS | 编译时（自动编译为 `.o` 并链接） |
| 功能插件 | `.vux` | Python/C | 运行时（dlopen/子进程） |
| 语言插件 | `.vulage` | Python/C | 编译前预处理 |

---

## 六、构建与测试

```bash
make                     # 完整编译（编译器 + 运行时库）
make clean && make       # 清理重建
make libvus_rt.a         # 仅运行时库
./vus run tests/test_hello.vus    # 单测
bash tests/run_tests.sh           # 批量测试
bash install.sh                   # 一键安装
```

---

## 七、待读的高信号文件

1. `src/main.c` — CLI 入口与子命令分发
2. `src/lexer.c` / `parser.c` — 核心编译前端
3. `rt/libvus_rt.c` — 运行时实现
4. `include/vus/*.h` — 各公共接口契约
5. `docs/LANGUAGE_REFERENCE.md` — 语言能力权威清单

---

## 八、快速上手

```bash
#// hello.vus
定义 问候(名字):
    打印("你好，" .. 名字 .. "！")

问候("世界")
```

```bash
vus run hello.vus        # 输出：你好，世界！
```

---

> 开源协议：MIT © 2026 VUS Language Contributors