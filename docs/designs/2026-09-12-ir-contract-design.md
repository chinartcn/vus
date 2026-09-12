# VUS IR 契约设计与一致性基线（vus_vn 双后端锚点）

> 版本：draft-0.1
> 日期：2026-09-12
> 状态：待评审（工具与基线已落地，稳定面契约待逐条核验）
> 范围：定义 VUS 语言语义的**稳定契约**（未来 VM 后端必须满足的语义基线）与**一致性对跑协议**。
> 前提：M2 评审决策——双后端分工 = **VM=开发/热更通道，C=发布态性能通道**。
> 原则：契约只写"现在已成立且未来不可变"的语义；可演进项明确标注为扩展点。

## 1. 背景：为什么先立契约，而不是先写 VM

VUS 目前**没有显式 IR 层**：前端（lexer/parser/AST/generator）直接产出 C 代码，
运行时语义由 `rt/libvus_rt.c` + `rt/vua.c` 承载。M2 的最大风险是——未来 VM 后端
与 C 后端**语义漂移**（同一 .vus 两后端行为不一致）而无法定位是哪边改了语义。

本设计把「现在的语义」冻结为两份物证，让双后端对跑有据可依：
1. **IR 契约**（本文档）：语言语义稳定面的文字化规格；
2. **一致性基线**（[tests/conformance_c.json](../tests/conformance_c.json)）：89 个用例的
   （退出码 + 输出 sha256）指纹，作为 C 后端语义的机器可读锚点。

VM 后端（远期）上线时：`scripts/gen_conformance.py --backend vm --check` 与 C 基线
对比——通过即语义一致，不通过即漂移（由新后端负责对齐，红线 §6-3）。

## 2. IR 的现状与演进路径

```
现状（IR v1 = 语义规格 + AST，显式中间表示缺席）：
  lexer → parser → AST → generator ──→ C 代码 ──→ GCC/Clang/NDK
                                              └─→ rt/libvus_rt（vua.c 独立 native 运行时）

演进路径（IR v2 = 显式中间表示，可选，非本期承诺）：
  lexer → parser → AST → IR(v2) → [C 后端 | 字节码/VM 后端]
```

契约的立场：**语义契约超前于任何新后端**。ir v1 阶段，契约以本文档 + conformance
基线为准；若未来引入显式 IR（v2），契约从"文字 + 指纹"迁移为"中间表示 + 指纹"，
公约不变。后端分歧点只允许出现在"中间表示的消费"（C 代码生成 / 字节码发射），
前端（lexer/parser/AST）与语义规格两后端共享。

## 3. 稳定面契约（双后端必须满足的语义）

### 3.1 对象模型与值语义
- 八类运行时值：字符串 / 整数 / 浮点 / 布尔 / 列表 / 字典 / 闭包（函数一等公民）/ 结构体实例；
- 引用计数所有权模型（`rt/libvus_rt.h`：`vus_ref`/`vus_unref`/`vus_var_set`），容器递归释放；
- 字符串：UTF-8 字节 + 长度，内容可能含中间 `\0`；
- 结构化容器（`VusObject`，魔数 `VUS_OBJECT_MAGIC`），`vus_object_to_string` 为跨值 JSON 文本序列化入口。
- 语义等值（`==`：字符串逐字节、容器递归/引用？）以现有 `test_*.vus` 行为为准，列入基线用例。

### 3.2 变量与作用域
- 文件级全局变量（`vus_var_set` 槽位；跨文件按「导入」语义）；
- 块/函数作用域遮蔽；`vus_literal` 字面量驻留语义；
- 变量键为字符串，值可为八类中的任意值。

### 3.3 控制流与函数
- 递归、多返回值、闭包捕获、迭代器（`字典_键`/列表遍历）；
- 协程：单线程协作式（`vus_coro.c`），`等待`/`睡眠` 原语；
- 异常：`VusError` 链 + 错误码，`异常_抛`/`异常_捕获` 约定。

### 3.4 内建函数映射契约
- generator 维护「中文内建名 → C 函数」映射表（`src/generator.c`）；
- 新增内建 = 新增契约行（含参数/返回/错误约定），如 `音乐_播放 → vus_plugin_media_play`；
- 双后端必须提供相同内建集与相同语义，否则视为漂移。

### 3.5 平台接口契约（双桥）
- 能力桥（vce 面）：`VuaBridge.callJava` 的 api 清单即能力契约
  （`CapabilityRegistry` 已登记 vce/gvui 面归属）；
- UI 面（vui ↔ Gvui）：渲染树协议（`tie type/id/children/variable/event` 六原语键）、
  事件派发（`vua_trigger_event`/`vua_trigger_by_id`）；
- 会话契约：`vua_session_snapshot / restore`（Cordis_dc M1，跨进程可序列化，红线 §6-6）。

### 3.6 生命周期
- 程序入口：`vus_main`（generator 把 `main` 更名；APK 经 `jni_bridge` 调用）；
- 单会话：`vua_global_session` 创建/持有/release；
- 屏栈与重绘钩子：`vua_session_show/back/back_to` + `VuaRerenderHook`（变化即回调）。

> 各条"以现有行为为准"均要求**用例锚定**：契约文字与 conformance 基线互为印证，
> 文字若有歧义，基线指纹为准。

## 4. 契约版本与演进规则

- 当前契约版本：`contract = 1`（写入基线 JSON）。
- **只增不改**：新增语义不破坏既有基线；改动既有语义必须：
  1. 显式升级契约版本（`contract = 2`）；
  2. 重新冻结 C 后端基线（`gen_conformance.py` 重生成 + `--check` 自检）；
  3. 在基线 JSON 的 `note` 字段（或 changelog）记录漂移原因。
- 非确定输出用例（`NON_DET`，如含时间戳的 logger 用例）：协议只约束退出码，
  输出指纹置 `null`——内容随环境漂移是**环境**语义，不是语言语义。

## 5. 一致性对跑协议（双后端）

```
生成 C 基线：  python3 scripts/gen_conformance.py                # 写 conformance_c.json + 自检
日常校验：    python3 scripts/gen_conformance.py --check        # 语义回归门
双后端对跑：  python3 scripts/gen_conformance.py --backend vm --check   # 未来 VM 后端
```

- 用例集合：`tests/test_*.vus`（89 个，与 `tests/run_tests.sh` 同批；排除两个 vua 专项
  由 C 单测覆盖——它们不经 `vus run` 链路）；
- 每条记录：`{"rc": 退出码, "sha256": 输出指纹}`；rc 恒比，指纹仅在确定输出时比；
- 超时（60s/用例）标记为 `rc=124`（无外网/长延迟场景，与 run_tests.sh 同语义）。

## 6. 已落地物与验证记录

| 物 | 位置 | 状态 |
|----|------|------|
| 一致性工具 | [scripts/gen_conformance.py](../scripts/gen_conformance.py) | ✅ 已落地（生成/校验/后端参数/超时/非确定输出处理） |
| C 基线（89 用例） | [tests/conformance_c.json](../tests/conformance_c.json) | ✅ 已生成并 `--check` 通过（contract=1） |
| IR 契约本文档 | 本文件 | ⏳ 待评审（稳定面清单逐条核验） |

验证记录（2026-09-12）：`gen_conformance.py --no-check` 生成 89 用例零超时；
`--check` 首轮 4 例输出漂移（logger/plugins/xyz/legacy_stdlib 含时间戳与环境信息），
归入 `NON_DET` 后全量一致：`89 个用例基线全部匹配（backend=c, contract=1）`。

## 7. 边界与风险

1. **基线只锚"当前 C 后端"**：基线是 C 后端的物证，不是语义真理——语义真理是契约文字；
   两者冲突时以契约为准并重新冻结基线（升 contract）。
2. **`vus run` 单例编译慢**：89 用例一轮约 2 分钟；对跑建议 `--check` 增量或并行化（未来优化，不影响协议）。
3. **vua/UI 语义不在本基线**：vua 专项走 C 单测（`vua_api_ext`/`driver_vua_global`），
   渲染树/事件语义由主文档 §3.3-§3.5 契约 + 各自单测锚定。
4. **新增用例即新增契约约束**：新用例应同时保证确定性，避免加入 `NON_DET`（仅为必须的环境依赖保留）。