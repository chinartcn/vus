# VUS 初期 IDE — 实施计划

日期：2026-08-18
基于设计：docs/designs/2026-08-18-vus-ide-design.md
工作目录：examples/gui-designer/

按序执行如下阶段。每阶段结束用标记命令验证后再进入下一阶段。

前置：项目已 `make`，`./vus` 可用；工作目录为 `examples/gui-designer/`。

---

## 阶段 0 — 前端依赖占位与目录
在 `examples/gui-designer/` 下建立目录结构。

- [ ] 0.1 建目录：`libs/`、`libs/cm/`、`app/`
- [ ] 0.2 放入 Vue 3 UMD 到 `libs/vue.global.js`（本地文件，无 CDN）——若当前环境无法联网下载，先放一个占位并在 README 标注"需补充 vue.global.js"（真机离线步骤未来 patch 补齐）。
- [ ] 0.3 放入 CodeMirror 资源到 `libs/cm/`（同上：环境不可达则占位）。
- 验证：`ls libs libs/cm app`

## 阶段 1 — 后端拆分 + API + Token
- [ ] 1.1 新建 `api.py`：抽离现有 `server.py` 中的 `/export` 编排，封装成可复用函数 `export_design(design)`（保持现有输出不变）。
- [ ] 1.2 `api.py` 新增收端点实现：
  - `compile(source)`：写临时 `.vus` → `vus build --c-only`，返回 `{ok, errors:[{line,msg}]}`。
  - `run(opts)`：kind vus/shell；用 `subprocess.Popen`，捕获 stdout/stderr，`timeout` 超时 `kill`，`max_output` 限行。
  - `list_dir/read_file/write_file(safe_root)`：`os.path.realpath` 前缀校验，越界抛 403。
  - `search(q)`：查 `docs_index.json` 包含匹配 + 评分。
- [ ] 1.3 `server.py`：接入 Token（`--token`，默认随机生成打印；`hmac.compare_digest` 比对头 `X-VUS-Token`）。路由：
  - `POST /api/compile`（免 Token）
  - `POST /api/run`（需 Token；响应为 text/event-stream 流式输出）
  - `GET/POST /api/fs`（写需 Token）
  - `GET /api/search?q=`（免 Token）
  - 保留 `POST /export`。
- [ ] 1.4 `server.py` 端口占用立即报错并提示换端口。
- 验证：`python3 - <<'PY'` 调用 `api.compile`/`api.run(timeout=2, shell 'echo hi')`/`read_file` 越界返回 403；`curl` 打 `/api/run` 无 Token 得 401。

## 阶段 2 — 前端 Vue 骨架 + 路由 + store
- [ ] 2.1 `index.html`：加入 `meta viewport`、引入 `libs/vue.global.js`、`app/main.js`、`app/style.css` 壳；`<div id="app">`。
- [ ] 2.2 `app/store.js`：共享响应式状态 `{ design, code, token, backend, meili, theme, flipSync }` + 持久化（localStorage）。
- [ ] 2.3 `app/main.js`：Vue createApp + 手写 hash 路由 `#/designer|editor|search|settings`；顶部导航（SVG 图标，无 emoji）。
- 验证：`python3 server.py` 后浏览器打开四视图可切换、状态跨视图保留。

## 阶段 3 — 设计器视图移植
- [ ] 3.1 `app/controls_schema.js`：每种控件 `{ key,label,kind }` 属性（label/button/slider/spin/switch/chart…）。
- [ ] 3.2 `app/designer.js`：控件库（SVG 图标）、画布拖放/选中、预览渲染；**控件库图标去 emoji**。
- [ ] 3.3 属性面板：按选中控件类型的 schema 渲染；空选时空态。
- [ ] 3.4 移动端：窄屏控件库折叠为抽屉；三栏→单栏堆叠。
- 验证：新增/选中控件，属性面板随类型变化；窄屏下控件库可折叠。

## 阶段 4 — 代码视图（编辑/检查/运行）
- [ ] 4.1 `app/editor.js`：CodeMirror 载入（高亮 + 用 `vusBuiltins.js` 补全）。
- [ ] 4.2 "编译检查"→ `POST /api/compile` → 行标注 + 错误列表。
- [ ] 4.3 终端面板：`POST /api/run`（带 Token）流式接收 chunk；"停止/清空"。
- 验证：改出语法错误可标注；`run` shell `echo ok` 在终端显示；无 Token 提示 401。

## 阶段 5 — 设计 ↔ 代码双向同步
- [ ] 5.1 `vus_export.py`：生成代码时为每个控件行加标记注释 `# Ctrl: 名（类型）` 与规范参数序（兼容现有导出）。
- [ ] 5.2 新增反向解析器（后端 `api.py` 或前端 `editor.js`）：按标记 + 参数正则提取控件参数，回灌 `store.design` 更新画布。
- [ ] 5.3 跟随开关 `flipSync`；仅同步标记行；解析失败跳过 + 状态栏提示。
- 验证：改设计→代码更新；手改标记行参数→画布更新；额外未标记行保留。

## 阶段 6 — 文档视图 + 本地索引
- [ ] 6.1 生成 `docs_index.json`（由 `vus_builtin` 元数据 + 手写文档）。
- [ ] 6.2 `app/search.js` + `/api/search`：`engine=local`（默认）；`meili` 已配置端点则转发，否则回退 local。
- 验证：离线搜索"图形_填充"返回签名/说明/示例。

## 阶段 7 — 设置视图 + 错误处理 + 测试 + README
- [ ] 7.1 `app/settings.js`：Token / 后端 host:port / Meili 端点 / 主题。
- [ ] 7.2 统一错误条：未连接、401、编译错、运行超时、fs 403。
- [ ] 7.3 后端断言脚本（token 校验、compile、超时杀进程、fs 越界、export 回归）+ 端到端 curl（含负例）。
- [ ] 7.4 更新 `README.md`：启动、Token、各视图、真机注意事项。
- 验证：`python3 server.py --token t1`；跑断言脚本全绿；curl 负例返回预期状态码。

## 收尾
- [ ] 成功标准核对（设计文档 §12 六条）。
- [ ] 提交 git（分类提交，阶段可逐个 commit）。