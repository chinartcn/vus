> 文档版本：v1.0_apk（APK 功能时代）
> 最后更新时间：2026-09-06


# VUS 插件系统使用指南

> 版本：v3.0.20260904150204（正式版）
> 覆盖：四层插件体系（`.vus` / `.vusx` / `.vux` / `.vulage`）、`vus vux`/`vus vusx`/`vus lang` CLI、进程内调用（`VUS_USE_PY`）、VUS 侧 `插件_*` 调用、`vaz` 扩展包与 LSP 生态命令、Android 端（APK 构建）各插件层可用性、运行期外部桥（`导入外部`/`导出变量`/`别名.成员` 的 C 域与 Python 域）。

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

**结构占位符（布局模板）**：模板可在 `子组件` 数组里放 `{名称}` 占位串。调用方传**数组**时整体**展平插入**（splice，数组作为一组子组件），传单个对象/标量时替换为该元素——布局模板据此在"子组件"位置注入整组节点。见 `testdata/vaz/layout/controls/sidebar.json` 的 `{菜单}`/`{内容}`：

```vua
{ "type": "侧边栏布局", "宽度": 220, "菜单": [ { "type": "按钮", "文字": "概览" } ], "内容": [ { "type": "文本", "内容": "主区" } ] }
```

**默认参数**：模板顶层可声明 `"默认": { "键": 值, ... }`，调用方**未传**的键用默认值（显式传的键优先）。侧边栏默认 `"宽度": 180`、`"菜单"`/`"内容"` 为空数组，缺省即得空壳骨架。

**布局模板包**：`testdata/vaz/layout/`（清单 `vaz.json`）登记以下布局模板，与逻辑包一样在 `vus vaz expand` 时构建期展开；APK 构建脚本 `build_apk.sh` 串行展开 common-controls 与 layout 两个包。展开产物为标准 `行/列/卡片/按钮/文本/列表` 组件树，走既有渲染/事件链路：

| 模板（中文名 / 英文名） | 展开结构 | 关键参数 | 备注 |
|---|---|---|---|
| `侧边栏布局` / `sidebar` | 行(列菜单, 卡片主区) | `菜单` / `内容` / `宽度` / `菜单样式` | 缺省宽度 180 |
| `顶栏布局` / `topbar` | 列(行[返回, 标题, 动作], 卡片内容) | `标题` / `返回文字` / `动作` / `内容` | 返回按钮固定触发 `返回页面` |
| `宫格导航` / `gridnav` | 列(行们) | `行们` | 调用侧按列数把格子分组为行 |
| `分组列表` / `groups` | 列表(数据, 高度, 事件) | `分组` / `高度` / `事件` | 列表项带 `节标题` 渲染分组头 |

模板参数**数组整体展平注入**（`{菜单}`/`{内容}`/`{动作}`/`{行们}`/`{分组}` 都是结构占位符），字符串参数（如 `{标题}`/`{宽度}`）字段级替换；顶栏/宫格/分组的最小骨架可用：`{ "type": "顶栏布局", "内容": [...] }`。

配套能力：`列表` 控件数据项对象带非空 `节标题` 字段时渲染为分组头行（粗体，不参与行点击派发），分组列表模板直接复用——见 `layout_smoke.vua` 冒烟。

## 七、LSP 与可视化生态

| 命令/工程 | 用途 |
|----------|------|
| `vus lsp` | JSON-RPC 语言服务器（补全/文档），内置函数权威表 `src/lsp/vus_builtin.c` |
| `examples/acode-vus-lsp-plugin/` | ACode 编辑器 LSP 客户端插件（Webpack 构建） |
| `examples/gui-designer/` | 可视化 GUI 设计器（控件拖拽 → 导出 `.vus`，`server.py` + `api.py`） |
| `scripts/build_lsp_android.sh` | LSP 服务端构建为 Android 可执行 |

## 八、Android 端（APK 构建）可用性

`vus build --apk` 链路由 `src/vus_apk.c` 生成工程：`vus_compile_to_c` 产出 C → 复制为 `jni/vus_app.c`（`main`→`vus_main`）→ `gen_jni_bridge.py` 生成 JNI 桥 → 固定源码列表的 `Android.mk` 交叉编译。各插件层在 Android 端的可用性如下：

| 系统 | 机制 | Android 可用性 | 说明 |
|------|------|:---:|------|
| `.vux`（功能插件） | Python 实现，运行期 `插件_运行*` 调用 | ✗ | 无 Python 运行时 |
| `.vusx`（依赖插件） | 编译期编 `.o` 并追加 GCC 链接 | ✗ | APK 链路不参与链接 |
| `.vulage`（语言插件） | 编译前 dlopen + 词法预处理 | ✓ | 构建期生效，产物无运行时依赖 |
| `vua`（UI 标记） | 构建期解析渲染树 | ✓ | `vua.c` 在 `Android.mk` 源码列表内 |
| `.vaz`（扩展包） | 构建期模板展开合并为纯 VUS 源码 | ✓ | 展开产物无运行时依赖 |
| 通用 `.so` 插件（`vus_register_plugin`） | dlopen 加载进编译器进程 | ✓（构建机） | 与 APK 产物无关 |

### 8.1 不可用的根因

- **`.vux` 两条运行路径在 Android 上都走不通**：
  1. 进程内嵌入：由 `VUS_USE_PY` 条件编译控制（`rt/libvus_rt.c` 的 `vus_plugin_run_vux_inproc`，dlopen 惰性加载 libpython）；APK 的 `Android.mk` 中 `LOCAL_CFLAGS` 仅含 `-DVUS_HAVE_CURL`，未定义 `VUS_USE_PY`，`vus_py_init()` 恒失败；
  2. 子进程回退：`vus_plugin_run_vux` 通过 popen 执行 `python3 vux_plugin_manager.py`，Android 上无 python3 可执行文件 → 返回空/失败。
- **`.vusx` 不参与 APK 链接**：`--exe` 路径会把 vusx 编译出的 `.o` 追加到 GCC 命令（`main.c`），但 `--apk` 仅做 `vus_compile_to_c` + 固定源码列表的 `Android.mk`（`vus_app.c libvus_rt.c vua.c yyjson.c jni_bridge.c`），vusx 的 `.o` 不会进入产物——引用 vusx 的工程在 APK 构建下会缺符号/功能缺失。

### 8.2 可行的替代方案

- 需要 `.vux` 能力（如搜索、网络增强）→ 改用**内建库**（哈希/ZIP/正则/网络等已内置于 `libvus_rt`）或编译为 `.vusx`、`.vaz` 逻辑库（构建期展开，APK 可用）。
- 需要 `.vusx` 复用 → 将插件源码以 `vus`/`vaz` 逻辑库形式合并进主脚本再构建 APK。

## 九、运行期外部桥·C 域（`导入外部`，dlopen 插件）

> 完整 ABI 见 `docs/designs/2026-09-06-runtime-ffi-abi-design.md`（总纲）与
> `docs/designs/2026-09-06-runtime-ffi-c-impl.md`（C 域实现规格）。本节目录用途。

**运行期外部桥**让 VUS 程序在运行时直接调用外部代码的函数并双向读写变量：
C 域（`.so`，dlopen 装载，本文）与 Python 域（模块路径，见 py-impl 规格）共用同一
`导入外部`/`导出变量`/`别名.成员` 语法。C 域在桌面与构建机可用；APK 运行期默认不加载。

### 9.1 插件形态（只依赖 `vus_rt_bridge.h`，纯 C）

```c
#include "vus_rt_bridge.h"

static VusRTValue g_slot = { .t = VUS_RT_INT, .v = { .i64 = 0 } };  /* 指针槽 */
static int g_ratio = 3;                                             /* 回调槽私有存储 */

static int add(VusRTValue *v, int n, VusRTValue *out, VusRTEnv *e) {
    if (n != 2 || v[0].t != VUS_RT_INT || v[1].t != VUS_RT_INT) {
        snprintf(e->errs, sizeof(e->errs), "add 需要两个整数");
        return -1;
    }
    out->t = VUS_RT_INT; out->v.i64 = v[0].v.i64 + v[1].v.i64;
    return 0;
}
static VusRTFunc g_funcs[] = {
    { "加法", 2, 2, add }, { "", 0, 0, NULL }                       /* 空名哨兵 */
};
static VusRTVar g_vars[] = {
    { "计数", VUS_RT_INT, 0, &g_slot, NULL, NULL },
    { "", 0, 0, NULL, NULL, NULL }
};
VUS_RT_EXPORT void vus_rt_module_entry(VusRTModule **m) {
    static VusRTModule mod = { .name = "c_math", .version = "1.0.0",
                               .funcs = g_funcs, .vars = g_vars };
    *m = &mod;
}
```

构建（不链接 libvus，仅头文件接口）：

```bash
gcc -shared -fPIC -I<include> -o c_math.so c_math.c     # include 含 vus_rt_bridge.h
```

### 9.2 VUS 侧使用

```
导入外部 (cm, {源: "./c_math.so", 参数: {基准: 10}})   # 惰性加载；参数传给 init
断言(cm.加法(3, 4) == "7", "函数调用")
cm.计数 = 42                                            # 变量写 → 指针槽
断言(cm.计数 == "42", "变量读")
导出变量 (["我的计数"])                                 # 反向：外部读 VUS 全局
我的计数 = 100
断言(cm.我的计数 == "100", "宿主槽双向同步")
```

### 9.3 变量三种机制与约束

| 机制 | 声明方式 | 语义 |
|------|----------|------|
| 指针槽 | `slot` 非 NULL | 桥层按 `VusRTValue` 布局直接读写该内存，零拷贝 |
| 回调槽 | `slot` 为 NULL + `get`/`set` | 两侧各一次值转换；`readonly=1` 拦截 VUS 侧写 |
| 宿主转换槽 | VUS `导出变量` | C 侧经桥读写 VUS 全局（`vus_var_set` 引用安全），双向同步零缓存 |

约束：函数表/变量表以**空名哨兵**结尾；域内重名（函数/函数、变量/变量、函数/变量交叉）→ 加载报错；
字符串跨界按 UTF-8 复制；容器深拷贝、深度上限 64；数值不做隐式转换；`VUS_RT_NIL` ⇄ VUS 空。

### 9.4 与既有 C 插件（`vus_register_plugin`）的区别

| | 旧 C 插件 | 运行期外部桥·C 域 |
|---|---|---|
| 时机 | 编译期注册（`vus_register_plugin`） | 运行期 `导入外部` 惰性 dlopen |
| 接口 | 编译器内建插件 API | `vus_rt_bridge.h` 独立 ABI（可独立构建） |
| 场景 | 编译器/生态扩展 | 用 VUS 脚本驱动任意 `.so` |

## 十、常见问题（FAQ）

**Q：`插件_运行JSON` 在无 Python 环境下可用吗？**
A：可用——子进程方案不依赖编译期 Python；`VUS_USE_PY` 只是把调用升级为进程内嵌入式解释器（更快）。`typeof` 在无 `VUS_USE_PY` 时恒返回 `"空"`；`JSON_*` 基于 yyjson，始终可用。

**Q：vusx 插件链接报 `multiple definition of main`？**
A：确认 vusx 目录下 `main.vus` 未被当作主程序编译为可执行（v1.0-beta 已默认 `omit_main` 库式编译）；同时检查项目 `vus.json` 的 `vusx依赖` 路径正确。

**Q：写插件时中文命令怎么传参？**
A：`插件_运行(名, "命令 参数1 参数2")`——字符串整体传插件 `run(cmd, args)`，插件侧按空格拆分（或自定分隔协议）。