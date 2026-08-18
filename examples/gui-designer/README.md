# HTML 高级排版设计器（VUS GUI Designer）

一个**纯浏览器**的 VUS GUI 可视化设计器：拖拽式排版 VUS 界面，实时预览，并导出成 `.vus` 源文件。

- 前端：`vanilla JS` + `HTML` + `CSS`，无框架、无构建、无第三方 CDN。
- 后端：仅用 **Python 3 标准库**（`http.server` / `threading` / `json` / `argparse` / `urllib.parse` / `os`）。
- 与项目其余 C 代码完全无关，不改动任何已有 `.c / .h / Makefile / src` 文件。

## 目录结构

```
gui-designer/
  server.py            # 标准库 HTTP 后端（静态托管 + POST /export）
  vus_export.py        # 设计 JSON → .vus 源码（纯标准库，可独立命令行使用）
  index.html           # 前端单页
  style.css            # 前端样式
  app.js               # 前端逻辑
  sample_design.json   # 示例设计（覆盖全部 13 种控件）
  README.md            # 本文档
```

## 启动方法

```bash
cd examples/gui-designer
python3 server.py [--port 8000]
```

启动后日志打印 `listening on http://127.0.0.1:8000`，在浏览器打开：

```
http://127.0.0.1:8000
```

## 使用说明

1. **添加控件**：点击左侧"控件库"中的条目，在画布中心生成一个默认实例。
2. **选中/拖动**：点击画布中的控件即可选中（高亮边框）；按住拖动可改变位置；
   对有宽高的控件，右下角出现手柄，可拖动改变大小。
3. **编辑属性**：右侧"属性"面板显示被选中控件的全部属性（名称/文本/颜色/坐标/尺寸/取值等），
   修改即时生效并实时重绘预览。
4. **画布/主题**：顶部工具栏可调整画布逻辑宽高、全局控件圆角以及主题五色（背景/边框/高亮/前景/文字）。
5. **导出 `.vus`**：点击"导出 .vus"，前端收集当前设计为 JSON，POST 到 `/export`，
   后端生成 `.vus` 源码，在一个弹窗中展示，可直接"下载 .vus"（Blob 下载）。

## POST /export 接口

- 请求：`POST /export`，`Content-Type: application/json`，
  请求体为设计数据 JSON（结构见 `sample_design.json`）。
- 成功响应：`{ "ok": true, "vus": "<生成的 .vus 源码>" }`
- 失败响应：`{ "ok": false, "error": "<错误信息>" }`

示例（curl）：

```bash
curl -s -X POST http://127.0.0.1:8000/export \
  -H "Content-Type: application/json" \
  -d '{"name":"T","width":100,"height":60,"radius":6,"controls":[{"type":"button","name":"b","x":0,"y":0,"w":50,"h":20,"text":"ok"}]}'
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

## 示例往返（B3 验收）

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