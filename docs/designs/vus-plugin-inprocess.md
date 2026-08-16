---
design_type: initiative
created_at: 2026-08-16
---

# VUS 插件进程内集成与结构化数据传递设计

## Intent Contract

```
intent: 将 VUS 对 .vux Python 插件的调用从「子进程 + CLI」升级为「嵌入 Python 解释器、
        进程内直接调用」，并为插件返回值建立结构化数据（JSON/列表/字典）到 VUS 变量的
        传递机制，替代当前仅支持字符串的现状。
constraints:
  - 不破坏现有 VUS 语言语法与已编译程序的调用方式（插件_运行 保持可用）
  - 不破坏现有插件 API（VuxPlugin.init/run/cleanup 生命周期不变）
  - 嵌入 Python 失败时（如无 libpython 开发头）优雅降级到现有子进程方案
  - 不改变 Meilisearch 等既有 Python 插件的实现方式（它们无需重写）
success_criteria:
  - 插件_运行 通过进程内嵌入解释器调用插件，无 popen/CLI 子进程
  - 插件 run() 返回的结构化数据（dict/list）可写入 VUS 列表/字典变量
  - VUS 生成器支持列表/字典字面量（VUS_AST_LIST_LITERAL / VUS_AST_DICT_LITERAL）
  - 新内建函数（插件_运行JSON / VUS JSON 解析）有单元测试覆盖
  - 关键路径（gcc 编译、生成器、运行时）通过 make test 回归
risk_level: high   # 涉及解释器嵌入、编译链、内存管理，属高风险架构变更
```

## Verification Contract

```
verify_steps:
  - run tests: make test && ./vus test  # 编译器回归
  - run tests: 新增 C 单元测试（build/tests）覆盖 JSON↔Vus 对象转换、解释器嵌入调用
  - run tests: 插件调用测试（test_plugin_run.vus 断言结构化返回）
  - check: 生成器对列表/字典字面量输出 vus_list_new/vus_dict_new 调用
  - check: 无 libpython 环境下编译仍成功（降级路径）
  - confirm: 上述测试全部通过；插件_运行 进程内返回结构化数据
```

## Governance Contract

```
approval_gates:
  - 设计文档审批（当前步骤）
  - 实现完成后代码审查（brooks-lint）
  - 合并前确认降级路径与测试覆盖
rollback: 运行时库新增函数与生成器分支为增量；移除 vus_plugin_run_vux_inproc 的
          USE_EMBED 宏开关即回退到子进程方案；不影响既有编译产物
ownership: 项目作者
```

## Scope

### In
- 运行时库嵌入 Python 解释器（libpython），进程内加载并调用插件
- `插件_运行`/`插件_运行JSON` 内建函数的生成器映射
- JSON ↔ VusList/VusDict/VusString 双向转换（运行时库）
- 生成器支持列表/字典字面量
- 编译链（Makefile / vus_compile_c）注入 libpython 头文件与链接参数
- 插件调用与 JSON 转换的单元测试

### Out
- 插件注册表 / 热加载 / 跨进程隔离
- 深度学习或任意 Python 标准库的完整桥接
- 类型标注系统改动
- 调试器集成

## Decisions

| # | 决策 | 选项 | 选定理由 |
|---|------|------|----------|
| 1 | 嵌入方式 | (a) 静态链接 libpython<br>(b) 动态加载 dlopen libpython | (b) 运行时不强制编译期依赖，缺解释器时用 `dlsym` 惰性加载并降级；开 `VUS_USE_PY` 才启用 |
| 2 | 数据形态 | (a) 插件返回 JSON 字符串再解析<br>(b) Python 对象直接转 Vus 对象 | (b) 用 CPython C-API `PyObject`→`VusString/VusList/VusDict` 直接转换，避免二次序列化 |
| 3 | 生成器支持 | (a) 新增 JSON 字面量语法<br>(b) 复用既有列表/字典字面量 | (b) 解析器已产出 `VUS_AST_LIST_LITERAL`/`VUS_AST_DICT_LITERAL`，仅补生成器分支 |
| 4 | 降级策略 | CLI 方案保留为 `#ifndef VUS_USE_PY` 分支 | 保证无解释器环境仍可编译运行 |
| 5 | 新函数命名 | `插件_运行JSON` 返回结构化<br>`插件_运行` 保持字符串 | 兼顾向后兼容与结构化诉求 |
| 6 | GIL 与协程 | 插件调用**同步阻塞**执行（v0.1） | VUS 协程为单线程协作式，不并发，天然无 GIL 竞争；同步语义最简单。代价：插件长调用会冻结调度器，文档明确「插件调用期间协程不可切换」。独立线程回传会在以后真正引入 GIL 竞争，复杂度高，v0.1 不做 |
| 7 | VusObject 形态 | **组合容器**：带类型标记的结构，内部 `union` 持有指向 `VusList`/`VusDict`/`VusString` 的指针 | 不替代、不重写既有结构，改动最小；避免统一头伪继承带来的既有结构重构风险 |
| 8 | 类型感知 | 新增 **`typeof` 内建函数**（返回「列表/字典/…」） | 语言层目前无 typeof；配合既有流程判断即可。模式匹配是更大语法改动，YAGNI，留待后续 |

### Rejected Alternatives
- **常驻 Python 服务进程 + JSON-RPC**：保留进程边界，跨进程序列化开销与状态同步复杂，不满足「进程内直接调用」诉求。
- **纯 C ABI 插件(.so)**：需重写全部现有 Python 插件，破坏生态，成本过高。

## Surface

### 运行时库（rt/libvus_rt.c / libvus_rt.h）
- 新增 `vus_py_init()`：惰性 `dlopen` libpython，定位 `Py_*` 符号，初始化解释器。
- 新增 `vus_plugin_run_vux_inproc(plugin, cmd)`：进程内加载插件类、调用 `run()`、返回 `VusString*`。
- 新增 `vus_plugin_run_vux_json(plugin, cmd)`：返回结构化 `VusObject*`（映射到 VusList/VusDict）。
- 新增 JSON↔Vus 转换：`vus_json_parse(VusString*)`、`vus_py_to_vus(PyObject*)`、`vus_vus_to_json(void*)`。
- 现有 `vus_plugin_run_vux` 保留为 `#ifndef VUS_USE_PY` 降级实现。

### 生成器（src/generator.c）
- `插件_运行` → `vus_plugin_run_vux_inproc`（USE_PY 时）。
- `插件_运行JSON` → `vus_plugin_run_vux_json`。
- 补 `VUS_AST_LIST_LITERAL` / `VUS_AST_DICT_LITERAL` 分支，生成 `vus_list_new`/`vus_dict_new`/`vus_list_append`/`vus_dict_set` 调用。
- 新增 `JSON_解析` / `JSON_生成` 内建函数映射。
- 新增 `typeof` 内建函数：`typeof(expr)` 返回「整数/浮点/字符串/布尔/列表/字典/空」类型名，供流程判断结构化值类型。

### 编译链（Makefile / src/generator.c: vus_compile_c）
- 检测 `python3-config --includes --ldflags`，存在时追加 `-DVUS_USE_PY` 与头文件/链接参数。
- 增加 `-Wl,-rpath` 定位 libpython 运行时。

### 类型（include/vus Vus 对象）
- 复用既有 `VusList`/`VusDict`；引入轻量 `VusObject` 类型标记容器（type union）以承载结构化值。

## Risks & Open Questions

- **解释器嵌入与多线程**：按决策 #6，v0.1 采用同步阻塞调用，单线程无并发，规避了 GIL 竞争；但需在测试中验证「插件调用期间协程不可切换」这一约束，并确认无 hidden thread 借用。
- **版本兼容**：pyenv 3.14 与系统 3.12 的 libpython ABI 差异，后续验证中需确认仅依赖稳定 C-API。
- **内存所有权**：PyObject 与 Vus 对象生命周期转换的引用计数需明确，防泄漏/重复释放。
- **降级完整性**：`#ifndef VUS_USE_PY` 分支需保证与现有行为完全一致。
- **Open Question**：`插件_运行JSON` 返回值在 VUS 侧的类型标注如何表达（待实现阶段确认）。