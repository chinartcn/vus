# VUS 语言服务器 (LSP) — ACode 插件

让 `.vus` 文件在 ACode 中获得语言服务：三层补全、文档符号、命令执行。

本插件使用 ACode **官方 LSP API**（`acode.require("lsp").defineServer()` + `upsert()`），
`vus` 二进制直接打进插件包内（`bin/<abi>/vus`），安装后即可使用，无需手工放二进制到 PATH。

## 特性

- 把 `.vus` 注册为 `vus` 语言（`editorLanguages.register`），LSP 按此语言路由服务器。
- 通过 `defineServer` 注册语言服务器，ACode 经 AXS bridge 自动拉起 `vus lsp`。
- 按设备 ABI 自动选用预编译二进制（arm64-v8a / armeabi-v7a）。

## 安装

1. 构建得到各架构二进制（参考本仓库 `scripts/build_lsp_android.sh`），
   产出 `bin/arm64-v8a/vus` 与 `bin/armeabi-v7a/vus`，它们在 `files` 中声明，会随插件包分发。
2. 把整个插件目录打包成 `zip`（根含 `plugin.json`、`main.js`、`README.md`、`bin/`），
   在 ACode 的插件管理器中选择该 zip 安装并启用。
3. 完全重启 ACode（避免旧 ACode/LSP 全局单例残留）。
4. 打开任意 `.vus` 文件：
   - 输入如 `图形_` 触发普通补全；
   - `.:` 触发详细补全（签名 + 参数说明 + 示例）；
   - `..:` 触发命令候选（开始/结束/设置/索引/帮助）。

> 依赖：需要 ACode 版本 ≥ v1002（含 `lsp.defineServer`）。请在插件中心/官方仓库更新到最新版。

## 目录结构

```
plugin.json         插件清单（files 声明二进制，minVersionCode=1002）
main.js             入口：setPluginInit 注册生命周期
bin/
  arm64-v8a/vus     ARM64 预编译二进制
  armeabi-v7a/vus   ARM32 预编译二进制
```

## 故障排查

- 控制台无任何 `[vus-lsp]` 日志：确认插件已被 ACode 加载（设置 → 插件，将其启用）；
  旧版 ACode 早期的 `module.exports` 入口已弃用，请用最新版。
- 报“无 defineServer API”：ACode 版本过旧，升级至 v1002+。
- 补全无反应：确认打开的是 `.vus` 文件，且已完全重启 ACode 让语言/服务器注册生效。
- 提示找不到二进制：核对插件 zip 内 `files` 对应的 `bin/<abi>/vus` 存在，
  且插件安装时已将这些文件解压到插件目录（`main.js` 会用 `baseUrl/bin/<abi>/vus` 定位）。