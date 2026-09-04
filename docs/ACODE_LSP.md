> 文档版本：v1.0_apk（APK 功能时代）
> 最后更新时间：2026-09-04


# VUS 语言服务器在 ACode 中的使用

本文说明如何把 VUS 的 **语言服务器（LSP）** 接入 [ACode](https://acode.app/)（Android 代码编辑器），使其在真机上获得智能补全与文档符号能力。目标环境为 **Android 真机**，同时支持 **ARM64（arm64-v8a）** 与 **ARM32（armeabi-v7a）**。

---

## 1. 背景：VUS LSP 是什么

`vus lsp` 是 VUS 编译器内建的语言服务器子命令。它从 **stdin** 读取 JSON-RPC 2.0（LSP 标准 `Content-Length` 分帧），向 **stdout** 写响应，因此可以被任何支持 LSP 的编辑器直接拉起。

| 能力 | 触发方式 | 说明 |
| --- | --- | --- |
| 普通补全 | 输入函数/变量前缀 | 返回内置 `图形_*` 等函数（kind=3）与已定义符号 |
| 详细补全 | `.:` 前缀 | 返回完整签名（`detail`）+ 中文说明 + 示例（`documentation`） |
| 命令补全 | `..:` 前缀 | 返回可执行命令（kind=9）：`开始`/`结束`/`设置`/`索引`/`帮助` |
| 命令执行 | `workspace/executeCommand` | 执行 `开始` 等命令 |
| 文档符号 | `textDocument/documentSymbol` | 返回 `页_*` 函数与变量及其范围 |
| 悬停 | `textDocument/hover` | 返回函数签名 |

三层补全由同一个二进制提供；完整可执行文件即 VUS 编译器的 `vus`（含 `lsp` 子命令）。

---

## 2. ACode 如何连接语言服务器

ACode 的 CodeMirror LSP 客户端不会把 stdio 进程直接接到编辑器。它通过 **AXS 桥接**以 WebSocket 方式代理本地 stdio 服务器。因此接入 VUS 的标准做法是：用 ACode 的 LSP API（`acode.require("lsp")`）`defineServer()` 描述"用 `vus lsp` 启动该服务器"，再 `upsert()` 注册。

```
编辑层 (CodeMirror LSP)
   │  WebSocket
AXS 桥接
   │  stdio (Content-Length 分帧)
vus lsp  <── stdin / ──> stdout
```

---

## 3. 交叉编译 Android 二进制（ARM64 / ARM32）

> 目标系统是 Android，运行在 **Bionic libc + /system/bin/linker** 之上，因此必须用 **Android NDK** 的 clang 交叉编译；普通 `aarch64-linux-gnu-gcc` 产出的 glibc 二进制无法在安卓上运行。

### 3.1 方式一：仓库内脚本（推荐）

```bash
# 已安装 NDK（推荐显式指定）
export ANDROID_NDK_HOME=/path/to/android-ndk-r25c
bash scripts/build_lsp_android.sh

# 没有 NDK 时，脚本可自动下载后编译
bash scripts/build_lsp_android.sh --get-ndk
```

脚本会同时产出两种 ABI：

| ABI | 目录 | 对应设备 |
| --- | --- | --- |
| arm64-v8a | `release/android/arm64-v8a/vus` | 64 位设备 |
| armeabi-v7a | `release/android/armeabi-v7a/vus` | 32 位设备 |

NDK 定位顺序：`ANDROID_NDK_HOME` → `ANDROID_NDK_ROOT` → 常见安装路径。`release/` 与 `build-*/` 已写入 `.gitignore`，不会提交大二进制。

### 3.2 方式二：手动（等价命令）

以 NDK 放到 `/tmp/android-ndk-r25c` 为例：

```bash
NDK=/tmp/android-ndk-r25c/toolchains/llvm/prebuilt/linux-x86_64/bin

# ARM64
make PY_DEF= PY_INC= PY_LD= BUILD_DIR=build-arm64 \
     CC="$NDK/aarch64-linux-android21-clang" vus
mv vus release/android/arm64-v8a/vus

# ARM32
make PY_DEF= PY_INC= PY_LD= BUILD_DIR=build-arm32 \
     CC="$NDK/armv7a-linux-androideabi21-clang" vus
mv vus release/android/armeabi-v7a/vus

# 恢复宿主 x86-64 版
make
```

> `PY_DEF= PY_INC=` 用于屏蔽可选 libpython 嵌入，使交叉编译不依赖宿主 python 头；`vus` 目标不需要 GUI/运行时库。

### 3.3 校验

`file` 应显示 Android ELF（`interpreter /system/bin/linker64` 或 `/system/bin/linker`）：

```text
release/android/arm64-v8a/vus:   ELF 64-bit LSB pie executable, ARM aarch64, ... /system/bin/linker64
release/android/armeabi-v7a/vus: ELF 32-bit LSB pie executable, ARM, EABI5, ...  /system/bin/linker
```

---

## 4. ACode 插件

仓库已提供完整可用插件：`examples/acode-vus-lsp-plugin/`（`plugin.json` + `main.js`）。补全能弹出依赖两件事：**注册语言模式**（把 `.vus` 关联到 `vus` languageId，LSP 才能按语言路由）与**注册服务器**。核心代码如下：

```js
const lsp = acode.require("lsp");
const editorLanguages = acode.require("editorLanguages");

// 1) 注册语言模式：把 .vus 映射到 languageId "vus"（补全前提）
editorLanguages.register("vus", ["vus"], "VUS", () => Promise.resolve([]));

// 2) 注册服务器：以 `vus lsp` 启动
const server = lsp.defineServer({
  id: "vus-lsp",
  label: "VUS",
  languages: ["vus"],                 // .vus 经上一步被识别为 vus 语言
  useWorkspaceFolders: true,
  command: "vus",                     // 或绝对路径
  args: ["lsp"],
  checkCommand: "command -v vus",
});
lsp.upsert(server);
```

### 4.1 打包并安装插件

```bash
cd examples/acode-vus-lsp-plugin
zip -r /tmp/vus-lsp.zip plugin.json main.js
```

在 ACode 中：**主菜单 → 管理 → 插件 → 安装插件 → 选择本机 `vus-lsp.zip`**，安装后插件 `init()` 会把服务器注册到 ACode 的 LSP 注册表。

---

## 5. 部署到真机

按设备 ABI 选定二进制，放入可执行目录并加入 PATH（任选其一）：

- **Termux**：安装 `guixt`/`gcc` 后，把 `vus` 放到 `~/.local/bin`；`chmod +x`。
- **ACode 自带 bin**：或将 `vus` 复制到 ACode 能执行的目录，并把插件 `VUS_EXECUTABLE` 设为该绝对路径（`main.js` 顶部）。

确保 `command -v vus && vus lsp` 能正常启动（可用管道喂一个 `initialize` 请求自检）。

---

## 6. 启用并验证

1. 打开一个 `.vus` 文件。
2. 在文件中输入 `图形_`，应弹出 `图形_矩形` 等普通补全；
   输入 `.:图形_滚动容器` 应出现完整签名与说明；
   输入 `..:开始` 应出现命令候选。
3. 若补全无响应，检查：
   - 设备 ABI 是否与二进制匹配（`uname -m`）。
   - `vus lsp` 是否能启动（日志/终端可看到 `[vus-lsp]` 打印）。
   - **语言识别**：插件 `init()` 已调用 `editorLanguages.register("vus", ["vus"], ...)` 自动把 `.vus` 注册为 `vus` 语言，通常无需手动处理。若仍无响应，确认插件版本已含 `registerLanguage()`（见 `main.js`），並检查日志是否有 `[vus-lsp] 已注册 .vus 语言模式` 打印。

> **注意**：ACode（CodeMirror 6）界面上通常不会显示当前文件的语言名，所以不要靠“看不到 vus”来判断。**权威判据是 wslsp 桥终端**：打开 `.vus` 并输入字符后，若桥日志出现
> ```
> New connection: vus-... args=vus,lsp&type=stdio
> 🚀 Starting vus-... server: vus lsp (stdio)
> ```
> 就说明客户端已按 vus 语言路由、bridge 已拉起 `vus lsp`——补全随后即弹。若桥日志始终只有 `LSP bridge running...`，则说明客户端根本未发起连接（几乎总归因语言未识别成 `vus`，需装带 `registerLanguage()` 的插件）。

---

## 7. 可选：用 ws-lsp-bridge 手动桥接

若你的 ACode 内置 LSP 不会自动拉起本地 stdio 服务器（例如你实际用的是 acode-lsp-client 这类“自定义语言服务器 + WebSocket 地址”界面），可改走独立的 **WebSocket → LSP 桥**（[jobians/websocket-lsp-bridge](https://github.com/Jobians/websocket-lsp-bridge)）：

```text
ACode ──(WebSocket JSON-RPC)──> ws-lsp-bridge(:3030) ──(stdio Content-Length)──> vus lsp
```

1. 在设备上安装并常驻桥（需 node/npm）：
   ```bash
   npm i -g ws-lsp-bridge
   wslsp            # 监听 ws://localhost:3030（此终端保持运行）
   ```
2. bridge 在每个连接 URL 的 `args=` 里指定要拉起的 LSP。VUS 的连接地址为：
   ```
   ws://localhost:3030/vus?args=vus,lsp&type=stdio
   ```
3. 在 acode-lsp-client 的“自定义语言服务器 / LSP Settings”新增一条 VUS：
   ```json
   {
     "type": "socket",
     "serviceName": "vus-lsp",
     "modes": "vus",
     "label": "VUS",
     "socketUrl": "ws://localhost:3030/vus-{workspace}?args=vus,lsp&type=stdio"
   }
   ```
   > `args` 以逗号或空格分隔，`vus,lsp` 即命令 `vus`、参数 `lsp`。语言匹配仍要求 `.vus` 被识别为 `vus`——本仓库插件的 `registerLanguage()` 已替你注册好。
4. 也可直接用本仓库插件，并在 `main.js` 顶部把 `VUS_BRIDGE_URL` 设为上述地址：插件改为 WebSocket 直连桥（`transport.kind="websocket"`），而语言注册依旧生效。

---

## 8. 相关文件

| 文件 | 说明 |
| --- | --- |
| `src/lsp/lsp.c` | LSP 服务器核心（三层补全/文档符号/命令） |
| `src/lsp/vus_builtin.c` | 内置函数元数据（签名/说明/示例） |
| `tests/lsp_smoke.sh` | 一次性三层补全冒烟测试（宿主） |
| `tests/lsp_advance.sh` | 文档缓冲 + documentSymbol 自测 |
| `scripts/build_lsp_android.sh` | Android 交叉编译脚本（arm64/arm32） |
| `examples/acode-vus-lsp-plugin/` | ACode 插件（`plugin.json` + `main.js`） |