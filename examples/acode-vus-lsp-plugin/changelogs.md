# 更新日志

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