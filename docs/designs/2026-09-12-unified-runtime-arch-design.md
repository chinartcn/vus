# 统一运行时架构设计（vus_vn / vui / Cordis_dc / Gvui / vce）

> 版本：draft-0.2
> 日期：2026-09-12
> 状态：评审中；M1 骨架已落地（Java 侧实体见 §3.3）
> 范围：VUS 运行时目标架构——双后端执行、UI 语言规范化、动态上下文元框架、Java 宿主双 API 面。
> 原则：**增量演进，不重写**。本框架只定义目标形态与切分边界；全部落地走「在现有代码上补完/收编」，新增部分均给出首个落地物与完成标准。

## 1. 背景与目标

### 1.1 现状的三个断点

| 层 | 现状 | 断点 |
|----|------|------|
| 逻辑 | 单一「编译到 C」后端（`src/generator.c` → C），逻辑改动必须重编 `.so` / APK | 热重载只覆盖 `.vua` 与 `.dex`，逻辑层无热更通道 |
| UI | `.vua` + 渲染树（`rt/vua.c`）+ 控件表 | 控件表是**校验白名单**不是 schema；协议无版本号；中英文键双轨 |
| 宿主 | [VuaBridge.java](../examples/vua-android/app/src/main/java/com/vus/android/VuaBridge.java) 混装**能力面**（IO/Net/Media/扩展）与 **UI 回传面**（vuaTrigger）；逻辑全局态 / vua 屏栈 / Java 宿主态三张状态表各自为政 | 无统一定义、无事务式热重载、无能力注册契约 |

### 1.2 目标

| 目标 | 达成标准 |
|------|---------|
| 逻辑可热更 | 业务逻辑模块可替换而不重编 `.so`（VM 后端或模块级重编译通道） |
| UI 语言规范化 | `.vua` 升级为 `.vui`：类型化 schema、协议版本号、单一规范键 |
| 运行时可控 | 生命周期/依赖/热重载由单一元框架（Cordis_dc）接管，行为可观察、可回滚 |
| 宿主清晰分层 | 能力面（vce）与 UI 面（Gvui）从 VuaBridge 拆开，互不纠缠 |
| 双后端并存 | 新 VM 后端与现有 C 后端共存，语义一致性可验证 |

## 2. 分层与对称

```
Cordis_dc 动态上下文元框架（横切：生命周期 · 依赖 · 热重载）
┌──────────────┐        ┌──────────────┐
│  vus_vn      │        │  vui         │   语言运行时层
│  逻辑执行层   │        │  运行时 UI 语言│    （不依赖宿主）
└──────┬───────┘        └──────┬───────┘
       │ 能力调用              │ 渲染树
       │ 桥                    │ 翻译
┌──────┴───────┐        ┌──────┴───────┐
│  vce         │        │  Gvui        │   宿主实现层
│  宿主能力桥   │        │  Java UI 实现 │    （注册进语言运行时）
└──────────────┘        └──────────────┘
```

### 2.1 依赖规则（钉死）

1. **单向桥**：`vus_vn` 不依赖 `vce`，`vui` 不依赖 `Gvui`。宿主实现**注册**进语言运行时，语言层不反向查找宿主。
2. **事件归属**：UI 事件回传（点击/列表变化/输入）走 `vui ↔ Gvui`；能力调用（播放/IO/网络）走 `vus_vn ↔ vce`。vce 不出现第二个 vuaTrigger。
3. **对称**：两条桥各负责一半「语言↔宿主」语义：逻辑-能力桥、UI-翻译桥。Cordis_dc 的编织对象 = 四片叶子 + 两条桥。

## 3. 组件规格（draft）

### 3.1 vus_vn —— 逻辑执行层（C / VM 双后端并存）

> **M2 已落地（2026-09-12）**：一致性基线工具 `scripts/gen_conformance.py` +
> `tests/conformance_c.json`（89 用例，contract=1，`--check` 通过）；
> IR 契约文档 [2026-09-12-ir-contract-design.md](2026-09-12-ir-contract-design.md)（稳定面六条 + 版本演进 + 对跑协议）。

- **分工建议**：VM = 开发态与热更通道（体积小、逻辑可整包替换）；C = 发布态性能通道。Android 上现成载体：[UpdateManager](../examples/vua-android/app/src/main/java/com/vus/android/UpdateManager.java) 已能热更 `.vua`/`.dex`，逻辑热更的载体即 VM 字节码包。
- **首个产出物不是 VM，而是两件事**：
  1. `vus build` 输出**稳定 IR 契约**（现有 `--c-only` 产物 C 代码即准 IR，先把分叉点钉死）；
  2. **双后端一致性测试**：`tests/` 现有 100+ 用例对跑两后端，互相锁语义。
- **演进顺序**：IR 契约 → 模块级重编译（热重载物理基础）→（远期）字节码 VM 后端，先解释后 JIT，前端共享。

### 3.2 vui —— 运行时 UI 语言（.vua 规范化）

> **M4 已落地（2026-09-12）**：
> [testdata/vua_controls.json](../testdata/vua_controls.json) 全字段 schema 化——每个字段带「类型/默认值/描述」，
> 顶层新增「版本」=1（协议版本化依据）；补登 进度条/分隔线/间距 三个已在 Gvui 实现的控件（此前控件表缺失）；
> `rt/vua.c` 的 `vua_control_table_load` 同步升级（`VuaCtrlDef` 增加 `field_types/field_dflts`，解析 版本/类型/默认值），
> 新增 [vua_control_table_version()](../rt/vua.h) 供上层按版本决策兼容性；
> 类型校验走 `schema_type_ok`（整数/小数/布尔/数组/对象/颜色/字符串），不合规仅告警宽松放行（旧 `.vua` 不白屏）。
> 验证：`make` 全量编译 PASS；`vus lint` 8/8 `.vua` 通过；类型不符触发「schema 类型不符」告警且放行；版本解析抽查 =1。
> assets 副本（`app/src/main/assets/vua_controls.json`）与 testdata 同步一致。
> 剩：`.vui` 改名一次性迁移脚本（testdata 单一真源流程为新协议入口）、默认值注入（schema 声明已就位，落位留给校验器幂等注入）。

改名是表象，「规范化」的内容有三处不平整要补齐：

| # | 不平整 | 规范化动作 |
|---|--------|-----------|
| 1 | [控件表](../testdata/vua_controls.json) 只是 type+字段白名单（`rt/vua.c` 的 `vua_validate_node`） | 升级为**类型化 schema**：字段类型/默认值/校验器；同一份文件喂 IDE 补全、`vus lint`、运行期校验 |
| 2 | 渲染树协议无版本号 | `.vui` 加 `version` 字段；旧 `.vua` 走迁移器，新走新协议 |
| 3 | 中英文键双轨 | 收敛为单一规范键 + 兼容别名表；`.vua → .vui` 一次性脚本迁移（testdata 单一真源流程已就位） |

### 3.3 Cordis_dc —— 动态上下文元框架（编织者）

> **M1 已落地（2026-09-12）**，Java 侧实体：
> [CapabilityRegistry.java](../examples/vua-android/app/src/main/java/com/vus/android/CapabilityRegistry.java)（能力注册协议，vce/gvui 面清单）
> [VusSession.java](../examples/vua-android/app/src/main/java/com/vus/android/VusSession.java)（会话对象 + 生命周期钩子）
> [ReloadManager.java](../examples/vua-android/app/src/main/java/com/vus/android/ReloadManager.java)（事务状态机 + Pending_Reload 落盘/读档）
> **native 侧会话对齐**（同批完成，桌面测试通过）：
> [rt/vua.h](../rt/vua.h) / [rt/vua.c](../rt/vua.c) 新增 `vua_session_snapshot` / `vua_session_restore`（栈序屏名 + 全局变量转 JSON，按名重载屏栈，全程暂挂重绘钩子）；
> [VuaBridge.java](../examples/vua-android/app/src/main/java/com/vus/android/VuaBridge.java) 新增 `vuaSessionSnapshot` / `vuaSessionRestore` native；`scripts/gen_jni_bridge.py` 登记对应实现体，`jni_bridge.c` 已重新生成。
> 时序：`ReloadManager.onBoot()` 只受理（读 reload.json → 记快照路径 → 清标记）；`applyPendingRestore()` 在 **vuaInit 成功后** 恢复（native 屏栈需会话已建）。

现状态的「三张散装状态表」收编为一个会话对象：

```
session = { 逻辑字节码集合, 渲染树, 状态快照, 宿主绑定 }
热重载  = 校验新包 → 快照当前状态 → 原子替换三件套 → 应用快照
```

4 项交付（均不依赖 VM，可立即做）：

1. **会话对象**：统一持有逻辑/渲染/状态/绑定四件套来源；
2. **事务式热重载**：加载 → 校验 → 原子切换 → 失败回滚（禁止「一半新一半旧」）。含**延迟生效事务**：替换动作可推迟到重启后完成（`Pending_Reload` 标记落盘，重启读档补完最后两拍）；
3. **生命周期钩子**：挂载 / 变更 / 卸载 三态协议；
4. **能力注册协议**：把 `音乐_播放 → callJava → VusMedia` 这类隐式耦合形式化为「vce 向 vus_vn 注册能力」的显式契约。

> vgit（状态回溯/时间旅行）不在本期：session 的状态快照 + 事务替换即为未来的回溯载体，路径自然走出，本期不单独立名。

### 3.4 Gvui —— Java UI 实现

> **M5 已落地（2026-09-12）**：新增 [RenderAdapter.java](../examples/vua-android/app/src/main/java/com/vus/android/RenderAdapter.java)
> 渲染适配器接口——vui 与宿主渲染实现之间**唯一的渲染树翻译通道**，4 个方法恰好覆盖宿主真实调用面：
> `render(long)`（版本号协议入口）/ `saveInputs()` / `savedVals()`（会话快照取输入值）/ `restoreSaved(Map)`（快照恢复）；
> [VuaRenderer.java](../examples/vua-android/app/src/main/java/com/vus/android/VuaRenderer.java) 声明 `implements RenderAdapter`（Gvui 实现，渲染管线内部机制不外泄）；
> 宿主侧 [MainActivity.java](../examples/vua-android/app/src/main/java/com/vus/android/MainActivity.java) 与 [VusSession.java](../examples/vua-android/app/src/main/java/com/vus/android/VusSession.java) 一律以接口类型持有渲染器
> （VusSession 由 `renderer.savedVals` 字段直取改为 `savedVals()` 访问器）。javac 0 错误。
> 剩：vkt（Kotlin）第二实现时才真正分叉；届时新控件/新 case 只在各自适配器内落地，宿主流程不再改动。

- 现有：`VuaRenderer / Controls / Theme / RenderNode / MusicPlayerView / VideoPlayerView` + vuaTrigger 回传。
- 唯一增量已完成：**渲染适配器接口**——Gvui 成为 vui 的一个实现；vkt（Kotlin）以后只是第二个实现。现在抽成本最低，晚了是 VuaRenderer 每个 case 都要搬。

### 3.5 vce —— 宿主能力桥（capability bridge）

> **M3 已落地（2026-09-12）**：新建 [VceApi.java](../examples/vua-android/app/src/main/java/com/vus/android/VceApi.java)
> 承载 vce 面 10 个轻量能力（振动/剪贴板/设备信息/Toast/分享/电量/屏幕常亮/网络类型/通知），
> 自 VuaBridge.callJava 整体迁入并由 `CapabilityRegistry.isVce(api)` 判归属后分发；
> 第二刀补登记 file.*/http.* 等深耦合能力（实现仍在 VuaBridge 分支，调独立能力类），
> registry 全量覆盖 34 项（31 vce + ext.* 前缀 + 3 gvui），`describe()` 为唯一可查归属单点
> （JVM 抽查全绿，javac 0 错误）。theme.*（gvui 面）留在 VuaBridge。

- 与 Gvui 的切分线 = VuaBridge 现在的两个面：`callJava` 分发（media.*/video.*/IO/网络/剪贴板/振动/电量）归 vce；`vuaTrigger`/渲染上下文归 Gvui。
- 现有成员归类：`VusIo / VusNet / VusMedia / VusAsync / ExtensionLoader / UpdateManager` → vce；渲染四件套 + View 族 → Gvui。
- **命名说明**：`#ce` 取自 code；定义为「宿主能力桥」，避免与 Java `interface` 混淆。

### 3.6 v_mod —— 部署单元

v_mod 是运行时**部署单元**：一个 manifest + 四类文件（`.vus` 逻辑 / `.vui` 界面 / `.dex` 逻辑插件 / `.so` 原生库），可携带本地 Android 依赖库（引入需**重打包宿主**）。在此之上由 vce 提供 API 封装——能力无上限的前提是「核心轻量 + 依赖可挂载」。

加载路径分两案：

| 内容 | 生效方式 | 状态机 |
|------|---------|--------|
| `.vus/.vui/.dex` | 实时事务（现有 UpdateManager 已通 .vua/.dex） | installed → active |
| `.so` 或依赖库 | **延迟生效**：提示需重启；用户点"稍后重启"则写入 `Pending_Reload`，下次启动加载 | installed → pending_reload → active |

> 确认框语义：`Pending_Reload` 只在两选项间选——「现在重启」或「下次启动生效」。**没有"放弃更新"**：`Pending_Reload` 落盘即承诺，重启后读档补完事务。
>
> v_mod 状态快照与事务持久化（`reload.json` + `session.json`）必须活在**宿主层**且**可序列化**：`.so` 换版本后旧 native 堆（VusValue/MediaPlayer/屏栈内存）整体湮灭，任何只存在于 native 内存的状态都跨不过进程重启。现有库已接近可序列化（vua state 是 JSON、Java 侧 `hasSaved`），缺的只是统一会话快照对象。

#### 3.6.1 无感热重启（远期，结合 vpu）

```
保存快照(宿主序列化) → 写 Pending_Reload → Splash 遮挡
 → AlarmManager 拉起新进程 → 读档 → 加载新 .so
 → 应用快照 → 恢复现场（黑屏 ≈ 启动动画时长）
```

- **技术上必须进程重启**：`Activity.recreate()` 只重建 Activity/View 树，不卸已 map 的 `.so`（见 `docs/designs/2026-09-05-hotupdate-loader-design.md` §4.3「同进程换 JNI 表在 ART 上不可行」）。无感的"做法"是用 Splash/启动动画掩盖进程切换的瞬间，不是避免切换本身。
- vpu 本期的角色：**调度者**——选择重启时机（前台交互间隙/空闲）、动画遮挡、保存→重建→恢复的时序编排。作为 Cordis_dc 流程内的横切能力存在，不做独立层。

## 4. 与现有代码映射

| 组件 | 现有化身 | 规范化/增量 | 首个落地物 |
|------|---------|------------|-----------|
| vus_vn | 编译到 C 后端（generator.c 产出 C） | 并存后端 | 稳定 IR 契约 + 语义一致性测试 |
| vui | `.vua` + `rt/vua.c` + 控件表 | 改名 + schema 化 + 协议版本化 | vui.schema（控件表升级） |
| Cordis_dc | 三张散装状态表（逻辑全局态 / vua 屏栈 / Java 宿主态） | 会话对齐 + 事务式热重载 + 能力注册 | 会话对象 + 事务管理器 |
| Gvui | MainActivity / VuaBridge / VuaRenderer / Controls 等 | 抽渲染适配器 | 渲染适配器接口 |
| vce | VuaBridge.callJava 能力面 + VusIo/VusNet/VusMedia 等 | 拆包 + 能力注册契约 | VuaBridge 按能力面/UI 面拆开 |

## 5. 落地顺序与里程碑

| 里程碑 | 内容 | 完成标准 |
|--------|------|---------|
| M1 | **Cordis_dc 形式化**：会话对象 + 事务式热重载（含延迟生效事务 `Pending_Reload`）+ 能力注册协议 | **已落地**（Java + native 双侧）：CapabilityRegistry/VusSession/ReloadManager + `vua_session_snapshot/restore` + MainActivity/UpdateManager/`gen_jni_bridge` 接入。桌面功能测试 PASS。剩：能力注册协议在 callJava 分发的收口（M3 一并做） |
| M2 | **vus_vn IR 契约 + 双后端一致性**（分工已定：VM=开发/热更通道，C=发布态性能通道） | **工具与基线已落地**：`gen_conformance.py` + `conformance_c.json`（89 用例，contract=1，--check 通过）+ IR 契约文档（稳定面/版本演进/对跑协议）。剩：稳定面清单逐条核验、模块级重编译（热重载物理基础）、（远期）VM 后端对跑 |
| M3 | **vce 拆包**：VuaBridge 能力面/UI 面分离（CapabilityRegistry 单调核对分） | **已落地**：轻量能力 10 个迁入 `VceApi` 经 registry 分发；registry 全量覆盖 34 项（31 vce + ext.* + 3 gvui），describe() 为可查单点，JVM 抽查全绿、javac 0 错误 |
| M4 | **vui schema + `.vui` 改名迁移**（深度已定：Schema 化，不到类型化 AST） | **已落地**：`rt/vua.c` 控件表解析升级（字段类型/默认值/版本号 + 宽松类型校验，旧 `.vua` 兼容性保持）；[testdata/vua_controls.json](../testdata/vua_controls.json) 全字段 schema 化（类型/默认值/描述）+ 顶层「版本」=1 + 补登进度条/分隔线/间距；assets 副本同步一致；`make` 全量编译 PASS，`vus lint` 对 8 个 testdata `.vua` 全绿，类型不符触发告警且宽松放行。剩：`.vui` 改名一次性脚本 + vui.schema 校验器幂等注入（默认值落位） |
| M5 | **Gvui 渲染适配器接口** | **已落地**：新增 `RenderAdapter` 接口（render/saveInputs/savedVals/restoreSaved 4 方法），VuaRenderer 实现之（Gvui），MainActivity/VusSession 改持接口类型，javac 0 错误。vui 与 Gvui 之间只有一条渲染树翻译通道；vkt 第二实现时宿主流程不再改动 |

> 顺序理由：M1 不依赖 VM 且化解现状最痛的三张散装状态；M2 决定其后一切（热重载方式、状态快照成本）；M3 低风险高确定性；M4/M5 作为 UI 侧收尾。M4 与 M5 可并行。

## 6. 风险与红线

1. **副作用不可回滚**：网络/IO/媒体播放器等外部副作用不能参与快照回滚——热重载/状态回溯必须定义「可回放域」边界，副作用打标记，回滚时补偿或拒绝。
2. **热重载原子性**：禁止「一半新一半旧」；事务切换失败必须整体回滚并保留现场。
3. **双后端语义漂移**：无一致性测试集时，双后端 = 双份漂移。红线：任一后端改动必须过同一测试集。
4. **双宿主测试面**（若引入 vkt）：渲染器测试面翻倍，vkt 后置、只留适配器接口。
5. **快照成本**：状态快照与替换要按需做浅叠（引用共享），不做整树深拷贝。
6. **快照必须可序列化**：`.so` 换版本后旧 native 堆整体湮灭，跨进程状态（会话快照/`Pending_Reload`）只能存宿主层可序列化数据；不允许冻结 native 内存指针冒充状态。

## 7. 评审决策（2026-09-12 已定）

| # | 问题 | 结论 |
|---|------|------|
| 1 | 双后端分工 | **VM = 开发/热更通道**（体积小、逻辑可整包替换）；**C = 发布态性能通道**。倾向已记录，M2 按此展开 |
| 2 | vui 规范化深度 | **Schema 化**（字段类型/默认值/校验器，喂 IDE/lint/运行期），**不到类型化 AST** |
| 3 | Cordis_dc 代码形态 | **现在立刻写**——M1 骨架已落地（§3.3），会话对象 + 事务管理器已建 |
| 4 | 无感热重启排期 | **保持悬置**，近期不做；但**红线条款必须执行**（快照可序列化 §6-6、进程重启能力依据 §3.6.1），流程文档保留 |

悬置项：无感热重启（§3.6.1）不列入里程碑，仅保留流程设计与红线约束。

## 8. 术语表

| 术语 | 含义 |
|------|------|
| vus_vn | VUS 逻辑执行层：现有 C 后端 + 远期 VM 后端并存（#virtual execution） |
| vui | 运行时 UI 语言：`.vua` 规范化改名（渲染树 + 组件流 + schema） |
| Cordis_dc | 动态上下文元框架：编织语言与宿主，管理生命周期/依赖/热重载 |
| Gvui | Java UI API：vui 的宿主渲染实现 |
| vce | Java 宿主能力桥（capability bridge）：语言运行时向宿主要能力的契约 |
| v_mod | 部署单元：manifest + `.vus/.vui/.dex/.so` 四类文件，可携带 Android 依赖库 |
| Pending_Reload | 延迟生效事务标记：`.so`/依赖库更新落盘后，待重启读档补完的承诺 |
| 会话对象 | 逻辑字节码集合 + 渲染树 + 状态快照 + 宿主绑定的统一容器 |
| 事务式热重载 | 校验 → 快照 → 原子替换 → 应用快照 → 失败回滚 |
| 无感热重启 | Splash/动画掩盖下的"保存现场→进程重启→恢复现场"（远期，见 §3.6.1） |