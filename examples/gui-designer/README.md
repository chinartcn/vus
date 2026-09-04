> 文档版本：v1.0_apk（APK 功能时代）
> 最后更新时间：2026-09-04


# VUS 初期 IDE（排版设计器 + 代码编辑器）

本项目把原"HTML 高级排版设计器"升级为 **VUS 初期 IDE**：保留可视化排版设计功能，同时支持在浏览器中**编写 / 编译检查 / 执行** VUS 代码，并实现**设计 ↔ 代码双向同步**，还附带**文档检索**。

- 前端：多页 Vue 3 应用（本地 vendored、无 CDN、可离线），含**设计器 / 代码 / 文档 / 设置**四页，适配手机端（控件库可折叠、导航可折叠）。
- 后端：`Python 3 标准库`（`http.server / threading / json / argparse`），仅绑定 `127.0.0.1`，带副作用接口受 **Token 门禁**保护，文件操作有**路径越界防护**。
- 与项目其余 C 代码完全无关，不改动任何已有 `.c / .h / Makefile / src` 文件。

## 目录结构

```
gui-designer/
  server.py            # HTTP 后端：静态托管 + API 路由 + Token 校验 + SSE
  api.py               # 后端业务逻辑：导出/编译/执行/文件/检索（与 HTTP 解耦）
  vus_export.py        # 设计 JSON → .vus 源码（纯标准库）
  docs_index.json      # 本地文档索引（内置函数名/签名/说明/示例）
  index.html           # 前端入口（加载 Vue / CodeMirror / app 模块）
  style.css            # 前端样式（含深色主题 + 移动端适配）
  app/
    main.js            # Vue 应用骨架 + hash 路由 + 顶部导航 + Toast
    controls_schema.js # 控件库 + 属性面板动态 schema（每种控件不同属性）
    store.js           # 共享状态 + API 客户端 + 设计<->代码双向同步
    designer.js        # 设计器视图（画布 WYSIWYG + 拖拽/缩放/属性面板）
    editor.js          # 代码视图（CodeMirror + 编译检查 + 流式终端）
    search.js          # 文档检索视图（本地兜底 / 可选 Meilisearch）
    settings.js        # 设置视图（端口/Token/Meili + 工程文件浏览）
    vusBuiltins.js     # VUS 内置函数补全列表 + CodeMirror 高亮模式
  libs/                # 本地 vendored 前端依赖（vue、codemirror）
  sample_design.json   # 示例设计（覆盖全部 13 种控件）
  test_integration.py  # 后端 API 集成测试
  README.md            # 本文档
```

## 启动方法

```bash
cd examples/gui-designer
python3 server.py [--port 8000] [--token 自定义]
```

启动后控制台打印：

```
VUS IDE 已启动：  http://127.0.0.1:8000
访问 Token：      <32位随机Token>
```

在浏览器打开 `http://127.0.0.1:8000`。第一次使用先去**设置页**把打印出的 **Token** 填进去（用于"运行 VUS / Shell"与"工程文件写入"）。

> 安全说明：服务只绑定 `127.0.0.1`。`/api/run`（执行命令）与 `/api/fs/write`（写文件）需要请求头 `X-VUS-Token`；未填/填错时返回 `401`。文件操作仅允许访问工程目录内，路径越界会被拒绝（`403`）。

## 四个页面

### 1. 设计器
拖拽式排版 VUS 界面：从左侧控件库点击添加控件，拖动改变位置、拖动右下角手柄改变大小；右侧属性面板随控件类型动态变化。顶部工具栏可调画布宽高、全局圆角、主题五色。

- **生成代码**：把当前设计转成 `.vus` 源码并进入代码页。
- **从代码同步**：把当前代码反向解析回设计（基于控件函数调用与参数）。
- 开启"设计→代码自动同步"（设置页，默认开启）时，设计改动会自动写回代码页。

### 2. 代码
CodeMirror 编辑器（VUS 语法高亮 + 内置函数补全）。

- **编译检查**：调用 `POST /api/compile`，排除/定位错误（需本机安装了 `vus` 编译器，可用环境变量 `VUS_BIN` 覆盖）。
- **运行 VUS**：调用 `POST /api/run`，SSE 流式输出到右侧终端。
- **运行 Shell**：切到 "执行 Shell" 后输入命令（如 `ls -la`、`echo hi`）。
- **应用到设计**：把当前代码解析并重建设计器中的控件。

### 3. 文档
搜索 VUS 内置函数。默认使用**本地索引**（`docs_index.json`）实现离线检索；若在设置页配置了 **Meilisearch 服务地址**并选择 Meilisearch 引擎，则优先联调外部服务，失败时自动回退本地索引。

### 4. 设置
- 后端地址（端口）、访问 Token、Meilisearch 地址。
- 界面主题（浅色/深色）、"设计→代码自动同步"开关。
- **工程文件**浏览：列出/读取工程目录内文件，可"载入编辑器"或编辑后保存（写入需 Token）。

## API 一览

| 方法 | 路径 | 说明 | 需 Token |
|------|------|------|----------|
| POST | `/export` | 设计 JSON → `.vus` 源码 | 否 |
| POST | `/api/compile` | 编译检查 | 否 |
| POST | `/api/run` | 执行 VUS / Shell（SSE 流式） | **是** |
| GET  | `/api/search` | 文档检索（`q`、`engine`、`meili_url`） | 否 |
| GET  | `/api/fs/list?path=` | 列出工程目录 | 否 |
| GET  | `/api/fs/read?path=` | 读取文件 | 否 |
| POST | `/api/fs/write` | 写入文件（路径越界防护） | **是** |

示例（curl）：

```bash
# 依赖：先启动并取得 token
curl -s -X POST http://127.0.0.1:8000/export \
  -H "Content-Type: application/json" \
  -d '{"name":"T","width":100,"height":60,"radius":6,"controls":[{"type":"button","name":"b","x":0,"y":0,"w":50,"h":20,"text":"ok"}]}'

# 执行 shell（SSE）
curl -s -N -X POST http://127.0.0.1:8000/api/run \
  -H "Content-Type: application/json" -H "X-VUS-Token: <token>" \
  -d '{"kind":"shell","command":"echo hi"}'

# 文档检索
curl -s "http://127.0.0.1:8000/api/search?q=%E4%BC%A0%E6%84%9F%E5%99%A8_%E8%AF%BB"

# 写文件（需 Token，路径仅限工程目录内）
curl -s -X POST http://127.0.0.1:8000/api/fs/write \
  -H "Content-Type: application/json" -H "X-VUS-Token: <token>" \
  -d '{"path":"out.txt","text":"hello"}'
```

## 命令行导出（不依赖后端）

```bash
python3 vus_export.py sample_design.json sample_out.vus   # 写出到文件
python3 vus_export.py sample_design.json                  # 打印到 stdout
```

### 控件类型 → VUS 函数映射

| 控件 type      | 输出的 VUS 调用                        |
|----------------|----------------------------------------|
| label          | `图形_文字(x, y, "text", 0xcolor)`      |
| button         | `图形_按钮("name", x, y, w, h, "text")` |
| slider         | `图形_滑块("name", x, y, w, value, min, max)` |
| switch         | `图形_开关("name", x, y, on?1:0)`       |
| spin           | `图形_微调("name", x, y, value, step)`   |
| radio          | `图形_单选("name", x, y, item_h, "options", sel)` |
| round_rect     | `图形_圆角矩形(x, y, w, h, radius, 0xcolor)` |
| fill_rect      | `图形_填充(x, y, w, h, 0xcolor)`         |
| circle         | `图形_画圆(cx, cy, r, 0xcolor)`          |
| fill_circle    | `图形_填充圆(cx, cy, r, 0xcolor)`        |
| arc            | `图形_圆弧(cx, cy, r, start, sweep, 0xcolor)` |
| progress       | `图形_进度条("name", x, y, w, h, value)`  |
| textbox        | `图形_文本框("name", x, y, w, h, "text")` |

导出结构：首行 `图形_初始化(width, height, name)` → 可选 `图形_主题(...)`、`图形_外观(radius)`
→ 每个控件调用 → 末尾 `图形_刷新()` 及总控件数注释。颜色统一输出为 `0x%06X`，文本中的引号/换行已转义。

## 测试

```bash
python3 test_integration.py
```

该脚本启动一个临时 server（随机端口 + 固定 Token），依次验证：静态资源、导出、编译接口、
文档检索（本地）、文件 list/read/write（含 Token 门禁与路径越界防护）、`/api/run` 的 Token 校验与 SSE 流式输出。

## 示例往返

```bash
python3 vus_export.py sample_design.json sample_out.vus
```

生成结果片段：

```vus
图形_初始化(480, 320, "我的界面")
图形_主题(0xFFFFFF, 0x888888, 0x0055AA, 0x333333, 0x000000)
图形_外观(8)

# 控件：b1（button）
图形_按钮("b1", 20, 40, 120, 36, "确定")
# 控件：s1（slider）
图形_滑块("s1", 20, 90, 160, 50, 0, 100)
# 控件：sw1（switch）
图形_开关("sw1", 20, 120, 1)
# 控件：rr（round_rect）
图形_圆角矩形(200, 40, 120, 60, 8, 0xE0E0E0)
# 控件：a1（arc）
图形_圆弧(400, 210, 24, 0, 270, 0x666666)

图形_刷新()
# 共 13 个控件，界面设计完成。
```