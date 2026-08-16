---
design_type: initiative
created_at: 2026-08-16
---

# EasyLogger 日志集成设计

## Intent Contract

```
intent: 将 EasyLogger 超轻量日志库静态集成进 VUS 运行时，为 VUS 语言提供
        分级日志内建函数（调试/信息/警告/错误）与运行时日志级别控制，
        替代当前仅有的单开关 vus_debug_print。
constraints:
  - 纯 C 集成，不引入 C++，不依赖嵌入式 Python
  - 输出通道为「控制台(stdout) + 文件追加」双通道
  - 不改变既有 VUS 语言语法与已编译程序行为（打印 等保持可用）
  - 遵循 EasyLogger 开源许可（MIT）并保留版权声明
success_criteria:
  - 新增内建函数（日志_调试/日志_信息/日志_警告/日志_错误）可分级输出到 stdout 与日志文件
  - 新增日志_级别(级别) 可运行时过滤低于指定级别的日志
  - 首次调用惰性初始化，调用方无需手动初始化
  - generator.c 映射、运行时包装、port 层、Makefile 编译链全部就绪
  - tests/test_logger.vus 覆盖 4 级分级输出与级别过滤
  - 关键路径（gcc 编译、生成器、运行时）通过 make test 回归
risk_level: medium   # 新增外部源码目录与编译链，但为纯 C 增量，风险可控
```

## Verification Contract

```
verify_steps:
  - run tests: make test && ./vus test   # 编译器回归
  - run tests: 新增 tests/test_logger.vus 断言 4 级日志输出与日志_级别 过滤
  - check: 生成器对 日志_调试/信息/警告/错误/级别 输出 vus_log_* 调用
  - check: port 层 elog_port_output 同时写入 stdout 与日志文件
  - confirm: 无 libpython 环境下编译仍成功（日志不依赖 Python）
```

## Governance Contract

```
approval_gates:
  - 设计文档审批（当前步骤）
  - 实现完成后代码审查（brooks-lint）
  - 合并前确认 EasyLogger 许可声明与测试覆盖
rollback: 新增函数与生成器分支为增量；移除 vus_log_* 与 easylogger 编译即回退，
          不影响既有编译产物
ownership: 项目作者
```

## Scope

### In
- 引入 EasyLogger 核心源码（`easylogger.c`、`inc/elog.h`、`inc/elog_cfg.h`）到 `rt/easylogger/`
- 新增 VUS 平台适配层 `rt/elog_port.c`（输出到 stdout + 文件）
- 运行时包装函数 `vus_log_init` / `vus_log_set_level` / `vus_log_debug|info|warn|error`
- 生成器内建函数映射（`日志_调试` 等 5 个）
- Makefile 将 EasyLogger 源码编入 `libvus_rt.a`
- 测试 `tests/test_logger.vus`

### Out
- 异步 / Flash 日志模式
- 自定义 tag 过滤、按模块分类
- 日志滚动 / 压缩
- 集成 `libfastcommon` 或 LIB-ZC 等其他基础库（本次仅日志）

## Decisions

| # | 决策 | 选项 | 选定理由 |
|---|------|------|----------|
| 1 | 集成形态 | (a) 完整引入 EasyLogger 源码<br>(b) 提取核心精简<br>(c) 自实现分级日志 | (a) 真正用上成熟库，分级/格式化现成，符合「直接集成」意图 |
| 2 | 输出通道 | (a) 控制台 + 文件<br>(b) 仅控制台<br>(c) 仅文件 | (a) 覆盖面最广，同时满足 CLI 与写日志文件场景 |
| 3 | 函数集合 | (a) 4 级 + 级别控制<br>(b) 仅 4 级 | (a) 可运行时过滤日志量 |
| 4 | 级别标签 | 中文标签（调试/信息/警告/错误）映射到 ELOG_LVL_* | 与 VUS 中文命名风格一致 |
| 5 | 初始化时机 | 首次调用惰性初始化 | 调用方无需手动 `vus_log_init`，与现有内建函数一致 |
| 6 | 返回值 | 4 级函数返回 `VusString*`（`"0"` 成功 / `"-1"` 失败） | 沿用现有内建函数约定 |

### Rejected Alternatives
- **整体引入 LIB-ZC**：C/C++ 混合全家桶，全量引入依赖与体积成本高，且与 VUS 已手写容器大量重叠。后续按需采用单文件库（yyjson / minicoro / libhv）。
- **自实现分级日志**：不满足「直接集成 EasyLogger」意图，且 EasyLogger 极轻量（ROM<1.6K），引入成本低。

## Surface

### 运行时库（rt/libvus_rt.c / libvus_rt.h）
- 新增 `int vus_log_init(void)`：初始化 EasyLogger（`elog_init` → 设置控制台/文件通道 → `elog_start`），失败返回 `-1`；幂等。
- 新增 `VusString* vus_log_set_level(VusString* level)`：解析中文级别（调试/信息/警告/错误）并调用 `elog_set_filter_lvl`。
- 新增 `VusString* vus_log_debug|info|warn|error(VusString* msg)`：映射到 `elog_d/i/w/e` 输出，首次调用自动初始化。
- 新增 `vus_log_init` 内部调用 `vus_log_port_init`（见 port 层）。

### 平台适配层（rt/elog_port.c）
- 实现 EasyLogger 要求的 port 接口：`elog_port_init` / `elog_port_output` / `elog_port_output_lock` / `elog_port_output_unlock` / `elog_port_get_time`。
- `elog_port_output` 同时写 stdout 与追加写入日志文件（默认 `vus.log`，可用环境变量 `VUS_LOG_FILE` 覆盖）。

### 生成器（src/generator.c）
- `日志_调试` → `vus_log_debug`
- `日志_信息` → `vus_log_info`
- `日志_警告` → `vus_log_warn`
- `日志_错误` → `vus_log_error`
- `日志_级别` → `vus_log_set_level`

### 编译链（Makefile）
- 将 `rt/easylogger/easylogger.c` 与 `rt/elog_port.c` 加入 `RT_OBJ`，随 `libvus_rt.a` 归档。
- 头文件搜索路径加入 `rt/easylogger/inc`。

## Risks & Open Questions

- **EasyLogger 源码获取**：需从 Gitee（Armink/EasyLogger）拉取稳定版源码，核对版本与许可声明。
- **port 层线程安全**：EasyLogger 默认带锁；VUS 协程为单线程协作式，锁开销可接受，但需确认不与协程切换冲突。
- **日志文件句柄**：追加写入需保证不缓冲丢失，进程退出时 flush。
- **Open Question**：默认日志文件名与路径策略（默认 `vus.log` 于当前目录，计划阶段确认）。