# 更新日志

## v0.2.0 (2026-08-18)

### 重构：LSP → 纯浏览器本地补全
- 放弃 LSP 二进制 / AXS bridge，改为纯浏览器 JS + CodeMirror autocomplete 实现本地补全，不再需要 `vus` 可执行文件。
- 采用 webpack 工程构建：入口 `src/main.js`，产物 `dist/main.js`；参考 ACode Prettier 插件的纯 JS 架构。
- 通过 `editorLanguages.register("vus", ["vus"], "VUS", factory)` 在语言工厂中注入 cm6 `autocompletion` 扩展，规避「LSP 服务器在 AXS 中启动失败」的整类问题。
- 数据源 `src/vusBuiltins.js` 由 `src/lsp/vus_builtin.c` 重新生成（修复先前中文乱码编码问题），涵盖 69 个内置函数的名称、签名、说明、示例与类别。
- 补全支持中文函数名（`\u4e00-\u9fa5` 纳入标识符字符集）。
- 插件 `plugin.json` 入口指向 `dist/main.js`，移除 `bin` 与 `files` 声明；`minVersionCode` 下调为 1000。

## v0.1.3 (2026-08-18)

### 修复
- 监听错误事件名 `activeFileChange` → 官方 `switch-file`，修复「编辑器就绪前注册失败后不重试」的问题。
- 注册成功后调用 `editorManager.restartLsp()`，让已打开的 `.vus` 文件立即连上服务器（修复「装完仍没补全」的常见原因）。
- 重读官方 LSP API：确认 `defineServer(command,args)` 即本地 stdio 经 AXS WebSocket 桥的标准用法；`upsert()`、`minVersionCode=1002` 用法不变。

## v0.1.2 (2026-08-17)

### 新增
- 运行时 Toast 自诊断：注册成功/失败弹出提示（含二进制路径），便于真机定位断点（语言路由 vs 二进制启动）。

## v0.1.0 (2026-08-17)

首发版本。

### 新增
- 接入 ACode 官方 LSP API：`lsp.defineServer()` + `upsert()` 注册 VUS 语言服务器，经 AXS bridge 自动拉起 `vus lsp`。
- 使用 `acode.setPluginInit` / `setPluginUnmount` 官方生命周期入口。
- 把 `.vus` 注册为 `vus` 语言（`editorLanguages.register`），LSP 客户端按语言路由。
- vus 二进制随插件打包（`bin/<abi>/vus`），按设备 ABI（arm64-v8a / armeabi-v7a）自动选用。
- 三层补全：普通补全（前缀匹配）、详细补全（`.:` 前缀）、命令补全（`..:` 前缀，开始/结束/设置/索引/帮助）。
- 插件 icon（VUS logo）。

### 修复
- 遵循 ACode 官方生命周期，修复早期 `module.exports` 入口导致 init 不执行的问题。
- 拆分 `registerVus` 为语言注册与服务器注册，职责单一。
- 空 catch 补充日志，提升「LSP 不运行」场景的可观测性。
- 语言/服务器标识收敛为常量，避免散落硬编码。

### 依赖
- ACode ≥ v1002（含 `lsp.defineServer`）。