> 文档版本：v1.0_apk（APK 功能时代）
> 最后更新时间：2026-09-04


# VUS 插件系统使用指南

> 版本：v3.0.20260904150204（正式版）
> 覆盖：四层插件体系（`.vus` / `.vusx` / `.vux` / `.vulage`）、`vus vux`/`vus vusx`/`vus lang` CLI、进程内调用（`VUS_USE_PY`）、VUS 侧 `插件_*` 调用、`vaz` 扩展包与 LSP 生态命令。

## 一、四层插件体系一览

| 层 | 扩展名 | 编写语言 | 加载时机 | CLI | 头文件 |
|----|--------|----------|----------|-----|--------|
| 源码 | `.vus` | VUS | 编译时（import 复用）| — | — |
| VUS 插件 | `.vusx` | VUS | 编译期（自动编 .o 并链接）| `vus vusx` | `vus_vusx.h` |
| 功能插件 | `.vux` | Python / C | 运行期（子进程 / 进程内）| `vus vux` | `vus_plugin.h` |
| 语言插件 | `.vulage` | Python / C | 编译前预处理 | `vus lang` | `vus_lang.h` |

```
.vus（源码）
  → .vusx（编译期：VUS 写插件，vus.json 声明依赖，编译时 .o 链接）
  → .vux（运行期：Python/C 插件，vus vux 安装/运行，VUS 用 插件_运行* 调用）
  → .vulage（编译前：语法预处理，如 易语言 点前缀 → 函数风格）
```

## 二、功能插件（.vux）

### 2.1 结构

```text
my_plugin/
├── vux.json          # 元数据（名称/版本/描述/作者/最低VUS版本/依赖）
├── vux依赖.txt       # Python 依赖（pip 格式，一行一个）
├── vuxpy依赖.txt     # 构建期 Python 依赖（可选）
└── __init__.py       # 入口：继承 scripts/vux_plugin_entry.py 的 VuxPlugin
```

### 2.2 编写 Python 插件

```python
# __init__.py
from vux_plugin_entry import VuxPlugin

class MyPlugin(VuxPlugin):
    name = "示例"                   # 与 vux.json 的 名称 一致
    def init(self, api):           # 可选；返回 0 成功
        return 0
    def run(self, cmd, args):      # cmd=中文命令，args=参数列表，返回 str
        if cmd == "你好":
            return "你好，VUS 插件系统！"
        return "未知命令"
    def cleanup(self):             # 可选
        pass
```

### 2.3 打包 / 安装 / 运行

```bash
vus vux build my_plugin/                 # 打包为 示例-1.0.0.vux
vus vux install 示例-1.0.0.vux           # 安装（也可 install <仓库名> 从官方 .vux 仓库安装）
vus vux list                             # 查看已安装
vus vux info 示例                        # 查看信息
vus vux run 示例 [命令参数...]            # 命令行直接运行
```

### 2.4 在 VUS 程序里调用（`插件_*`）

```vus
结果 = 插件_运行("示例", "你好")           # 子进程调用，返回字符串
打印(结果)                                 # 你好，VUS 插件系统！

数据 = JSON_解析(插件_运行JSON("示例", "查询"))
打印(JSON_查询(数据, "ok"))
```

- `插件_运行(插件, 命令)`：子进程运行插件（无需编译期 Python）。
- `插件_运行JSON(插件, 命令)`：要求插件返回 JSON 结构化文本；编译期定义 `VUS_USE_PY` 时改走**进程内嵌入解释器**（`vus_plugin_run_vux_inproc`），免 IPC 开销。
- 仓库内置示例：`plugins/func/示例/`、`examples/test_plugin_run.vus`、`tests/test_plugin_run_json.vus`。

### 2.5 Meilisearch 搜索插件（官方 .vux 示例）

**安装/运行**：

```bash
vus vux install meilisearch
vus vux run meilisearch "状态"
```

**程序内调用**（示例 `examples/meili_search.vus`）：

```vus
结果 = 插件_运行("meilisearch", "搜索 关键词 --索引 文档库")
数据 = JSON_解析(结果)
```

连接配置优先级：环境变量 `VUS_MEILI_HOST`/`VUS_MEILI_API_KEY` → `~/.vus/plugins/meilisearch/config.json` → `http://localhost:7700`。

## 三、VUS 插件（.vusx，编译期）

### 3.1 结构

```text
my_utils.vusx/
├── vusx.json     # 名称/版本/入口/导出/依赖
├── main.vus      # VUS 编写的功能
└── 依赖.txt      # 可选依赖说明
```

### 3.2 项目引用

```json
{
  "风格": "函数",
  "vusx依赖": ["my_utils.vusx"]
}
```

编译主程序时，`vus_vusx.c` 会把依赖的 `.vusx` 走 词法→语法→AST→C 流水线编为 `.o`，追加到 GCC 链接命令。**vusx 以"库式"编译（`omit_main=1`），不生成 `main`，避免与宿主程序符号冲突**。

### 3.3 管理命令

```bash
vus vusx list          # 列出项目配置的 vusx 依赖
vus vusx info 路径     # 查看插件信息
vus vusx build 路径    # 单独编译（仅生成 .o）
```

> 曾用坑（已修复）：vusx 编译时若 `main.vus` 非第一入口参数导致函数名为空；现 `vus_vusx.c` 延后释放源码、设置 `omit_main`，导出函数名正常。

## 四、语言插件（.vulage，编译前预处理）

### 4.1 结构与加载

```text
plugins/lang/易语言/
├── vux.json        # 语言插件也是 vux 包形式（type: lang）
└── __init__.py     # 实现 preprocess(input) -> output
```

`vus.json` 配置：

```json
{ "风格": "易语言", "语言插件": "易语言" }
```

编译时引擎先在 `main.c` 加载语言插件并 `vus_lang_preprocess()` 把 `.功能`→`定义`、`.打印`→`打印`、`.结束`→删除，得到标准函数风格源码后再进核心词法器（核心词法器不认识点前缀关键字）。

### 4.2 命令

```bash
vus lang list      # 列出已加载/已安装语言插件
vus lang load 文件 # 加载语言插件
vus lang info 名称 # 查看信息
```

## 五、C 插件（共享库，进阶）

用 C 编写的 `.vux`/`.vulage` 插件通过 `dlopen` 加载，唯一导出入口：

```c
// .vux 功能插件
VUS_PLUGIN_EXPORT void vus_plugin_entry(VusPlugin **plugin);
// .vulage 语言插件
VUS_LANG_EXPORT void vus_lang_entry(VusLangPlugin **plugin);
```

结构体与 API 表（`VusPluginAPI` 含 `compile_file`/`compile_string`/`eval`/`compiler_version`）见 `include/vus/vus_plugin.h`、`include/vus/vus_lang.h`。生命周期：`vus_plugin_init_all` → `vus_plugin_run_all` → `vus_plugin_cleanup_all`（上限 `VUS_MAX_PLUGINS=64`）。

## 六、`.vaz` 扩展包（GUI 控件模板 + 逻辑库）

```text
包.vaz/
├── vaz.json
├── controls/        # 控件模板（rating_bar.json / search_bar.json …）
└── logic/           # 逻辑库（utils.vus）
```

```bash
vus vaz expand 页面目录 -v 包.vaz   # 构建期展开到页面目录
```

展开后 `.vua` 里的复合控件（如 `搜索条`/`星级评分`，见 `testdata/vua_home.vua`）被替换为组合组件 + 逻辑片段，再走 VUA 正常渲染/事件链路。

## 七、LSP 与可视化生态

| 命令/工程 | 用途 |
|----------|------|
| `vus lsp` | JSON-RPC 语言服务器（补全/文档），内置函数权威表 `src/lsp/vus_builtin.c` |
| `examples/acode-vus-lsp-plugin/` | ACode 编辑器 LSP 客户端插件（Webpack 构建） |
| `examples/gui-designer/` | 可视化 GUI 设计器（控件拖拽 → 导出 `.vus`，`server.py` + `api.py`） |
| `scripts/build_lsp_android.sh` | LSP 服务端构建为 Android 可执行 |

## 八、常见问题（FAQ）

**Q：`插件_运行JSON` 在无 Python 环境下可用吗？**
A：可用——子进程方案不依赖编译期 Python；`VUS_USE_PY` 只是把调用升级为进程内嵌入式解释器（更快）。`typeof` 在无 `VUS_USE_PY` 时恒返回 `"空"`；`JSON_*` 基于 yyjson，始终可用。

**Q：vusx 插件链接报 `multiple definition of main`？**
A：确认 vusx 目录下 `main.vus` 未被当作主程序编译为可执行（v1.0-beta 已默认 `omit_main` 库式编译）；同时检查项目 `vus.json` 的 `vusx依赖` 路径正确。

**Q：写插件时中文命令怎么传参？**
A：`插件_运行(名, "命令 参数1 参数2")`——字符串整体传插件 `run(cmd, args)`，插件侧按空格拆分（或自定分隔协议）。