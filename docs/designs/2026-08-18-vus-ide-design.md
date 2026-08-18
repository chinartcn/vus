# VUS 初期 IDE（设计器→IDE 升级）设计

日期：2026-08-18
状态：已确认，待实施计划

## 1. 背景与目标

将 `examples/gui-designer/` 从"纯 HTML 排版设计器"升级为 VUS 初期集成开发环境（IDE）：
在保留排版设计能力的同时，能在浏览器中编写/检查/运行 VUS 代码，并把设计器与代码
双向打通。面向真机（Termux 类环境，通常断网/离线）与桌面浏览器，可整体离线运行。

核心目标：
1. 设计器功能完整保留。
2. 可在编辑界面同时编写 VUS 源码。
3. 后端能执行 VUS 的编译/运行，并支持执行 shell。
4. 集成文档检索（本地兜底索引为默认，可选对接外部 Meilisearch）。
5. 适配手机端，控件库菜单可折叠。
6. 前端改造为"多页（多视图）Vue"，无构建、可离线。

## 2. 范围外（本期不做，YAGNI）

- 断点/单步调试。
- 通用 AST（语法树）编辑器。
- shell 执行白名单（保留扩展点，详见 §5）。
- 工程浏览器文件树（本期 `/api/fs` 仅提供基础的 list/read/write）。
- 前端单元测试（无构建，前端以手动冒烟为主）。

## 3. 架构形态与目录

形态：**单 Python 进程后端 + 本地（vendored、无构建）Vue 多视图前端**。
浏览器通过 `http://127.0.0.1:<port>` 访问，所有能力收敛到一个进程，可整体离线。

在 `examples/gui-designer/` 目录内演进（保留现有文件兼容）：

```
gui-designer/
  server.py            # 后端入口：静态托管 + 统一 API 路由（含原 /export）
  vus_export.py        # 保留：设计 JSON → .vus（后端 + 命令行独立使用）
  api.py               # API 与安全逻辑（拆分，保持 server.py 精简）
  libs/                # 本地 vendored 前端依赖（无 CDN）
    vue.global.js      # Vue 3 UMD
    cm/                # CodeMirror（语法高亮/补全）
  index.html           # Vue 挂载入口（含移动端 viewport）
  app/
    main.js            # Vue 实例 + hash 路由（#/designer #/editor #/search #/settings）
    store.js           # 共享状态：当前设计 / 源码 / 工程 / Token 配置
    designer.js        # 原设计器逻辑移植为 Vue（控件库/画布/属性面板）
    editor.js          # 代码编辑 + 编译检查 + 终端面板
    search.js          # 文档/内置函数检索
    settings.js        # Token / 后端地址 / Meili 端点 / 主题
    vusBuiltins.js     # 内置函数元数据（复用，供补全 + 搜索）
    controls_schema.js # 控件类型 → 属性 schema（见 §8）
  style.css
  docs_index.json      # 本地兜底文档/内置函数索引
  sample_design.json   # 保留
  README.md            # 更新启动说明
```

## 4. 技术选型

- 后端：Python 3 标准库（仅 `http.server` + 少量内置模块），不引第三方依赖，延续现有
  `server.py` 的实现方式。
- 前端：Vue 3 UMD（`libs/vue.global.js` 本地托管，无 CDN、无 build 步骤）；
  hash 路由手写约 30 行，不引 Vue Router 依赖。四视图切换不丢失状态（状态存于 `store.js`）。
- 编辑器：CodeMirror，复用 `vusBuiltins.js` 做内置函数补全与高亮。
- 图标：**一律使用 SVG 或文本几何符号，禁止用 emoji 作图标**（含控件库图标）。
- 搜索：默认 `local`（检索 `docs_index.json`），可选 `meili`。

## 5. 后端 API 与安全

### 5.1 Token 门禁

- 启动：`python3 server.py [--port 8000] [--token 自定义]`；未指定则生成随机 Token，
  打印到控制台。
- 只绑定 `127.0.0.1`。
- 带副作用接口（`run`、`fs` 写）校验请求头 `X-VUS-Token`，比对使用
  `hmac.compare_digest`，失败返回 401。
- 只读/无副作用接口（`export`、`compile`、`search`）免 Token。

### 5.2 端点一览

| 方法/路径 | 作用 | 需 Token |
|---|---|---|
| `GET /` 及静态 | 宿主页面与 `app/` `libs/` 资源 | 否 |
| `POST /export` | 设计 JSON → `.vus`（保留原能力） | 否 |
| `POST /api/compile` | 代码 → 临时 `.vus` → `vus build --c-only`；返回 `{errors:[{line,msg}]}` | 否 |
| `POST /api/run` | 执行 vus 或 shell，流式回传输出 | 是 |
| `GET/POST /api/fs` | 工程文件 list/read/write | 读免、写需 Token |
| `GET /api/search?q=` | 文档/内置函数检索 | 否 |

### 5.3 执行器（`/api/run`）

- 请求体：`{ kind:"vus"|"shell", source?, command?, timeout=15 }`。
- vus 模式：写临时 `.vus` → `vus run`（或 `vus build --exe` 后运行）。GUI 会阻塞，
  故支持 `timeout` 与"仅编译"选项；Termux 已启 X11 时可视窗口可正常弹出。
- shell 模式：`Popen` 执行任意命令（Token 已保护，不设白名单；代码中预留白名单扩展点）。
- 统一：`Popen` 捕获 stdout/stderr，`--max-output` 限行防刷屏，超时 `kill`；
  通过 SSE/JSON-Lines 把输出 chunk 流式推给前端终端面板；前端有"停止"按钮。

### 5.4 文件边界

- 文件读写限定在指定工程目录内，`os.path.realpath` 前缀校验，阻止 `..` 穿越；
  越界返回 403。

## 6. 前端多视图与移动端

- 顶部导航：设计器 / 代码 / 文档 / 设置（SVG 图标）。
- **设计器视图**：左侧控件库（桌面固定栏；窄屏折叠为抽屉层，可点开）、中央画布、
  右侧属性面板。沿用现有控件映射与预览交互。
- **代码视图**：CodeMirror 编辑器；"编译检查"→ 行内错误标注 + 错误列表；
  下方终端面板（流式输出、停止、清空）。
- **文档视图**：搜索框 + 结果列表（签名/说明/示例），数据源可切换 local/meili。
- **设置视图**：Token、后端 host/port、Meili 端点、主题。
- **移动端**：响应式（窄屏三栏 → 单栏堆叠），控件库抽屉、触控友好的命中区，
  固定高度页脚工具栏便于拇指操作。切视图不丢状态（`store.js` 持有）。

## 7. 设计 ↔ 代码双向同步

- **单一可信源 + 跟随开关**：设计改动 → 复用 `vus_export` 重新生成代码，回写代码面板
  对应"控件段"。用户可临时关闭同步，避免手写代码被打断。
- **生成的代码可解析**：生成器为每个控件行输出标记注释形如
  `# Ctrl: 名（类型）` 与规范参数序，供反向解析。
- **代码 → 设计反射**：后端/前端按标记 + 参数正则提取各控件最新参数，回灌设计 JSON，
  更新画布。
- **冲突策略**：只同步"带标记的控件行"；用户额外手写的逻辑/未标记行原样保留；
  某行解析失败则跳过并在状态栏提示，不做整体覆盖。
- 第一期不解析任意逻辑/表达式。

## 8. 属性面板：控件类型驱动的动态 schema（用户补充）

属性面板必须**跟随选中控件类型**展示不同的属性集合，而非单一通用表单。

- 定义 `controls_schema.js`，每种控件类型对应一个属性字段数组
  `{ key, label, kind }`，`kind ∈ text | number | color | bool | textarea | options`。
- 控件类型与示例字段：
  - label：名称 / 文本 / X / Y / 字号 / 颜色 / 对齐
  - button：名称 / 文本 / X / Y / 宽 / 高 / 圆角 / 背景色 / 文字色
  - slider：名称 / X / Y / 宽 / 值 / 最小值 / 最大值
  - spin：名称 / X / Y / 值 / 最小值 / 最大值 / 步长
  - switch：名称 / X / Y / 开合(on)
  - chart / 其他可视控件：各自的尺寸、数据、样式字段
- 属性面板在选中控件时按该类型的 schema 渲染；未选中控件时不显示（或显示空态）。
- 属性值变更即时回流到画布与代码（走 §7 同步）。

注意：现有 `app.js` 已具备"按类型 schema 渲染"雏形（`defs`/`renderProps`），移植时保留
并强化（补齐字段类型校验、options 下拉等）。同时把现控件库的 emoji 图标全部替换为
SVG/文本几何符号。

## 9. 文档检索（本地兜底 + 可选 Meili）

- 默认 `local`：检索本地 `docs_index.json`（来源：`vus_builtin` 元数据 + 手写文档），
  做包含匹配 + 简单评分（标题命中 > 说明 > 示例），离线可用。
- `engine=meili`：仅在设置里配置了 Meilisearch 端点时才转发；未配置则回退 local。
- 无强制外部依赖；兼容将检索结果塞入 `store.js` 供编辑补全参考。

## 10. 数据流 / 错误处理

- 状态流：`store.js` 持"设计 + 代码" → 双向同步钩子 → `/api/compile`、`/api/run` → 结果
  回流到错误列表 / 终端面板。
- 后端错误统一为 JSON + HTTP 状态码；前端统一错误条/toast，覆盖：后端未连接、401 未填
  Token、编译错误、运行超时被 kill、fs 越界 403。
- 端口被占用：启动时立即报出占用信息并提示换端口。

## 11. 测试策略

- 后端为主：Python 单元/断言脚本覆盖 token 校验、compile、run 超时杀进程、
  fs 越界拒绝、export 回归。
- 端到端：`curl` 打各 API 验证（含 401/403 负例）。
- 前端：手动冒烟；移动端窄屏断点（控件库折叠）人工验证。
- 已有 GUI 示例回归：升级前后 `vus_export.py` 导出一致。

## 12. 成功标准

1. `python3 server.py` 即可启动 IDE，浏览器访问 `127.0.0.1:port` 可完成：
   设计 → 生成代码 → 编辑 → 编译检查 → 运行（含执行 shell）。
2. 属性面板随控件类型正确变化。
3. 界面无 emoji 图标。
4. 移动端控件库可折叠、页面可单手操作。
5. 断网环境下文档搜索可用（local 索引）。
6. 本次升级不破坏已有设计器的导出兼容性。