# VUS 语言服务器 (LSP)

为 VUS 语言接入 ACode 的 Language Server Protocol（LSP）：三层补全、文档符号、命令执行。

## 前置条件

- 设备上存在**可执行**的 `vus`（含 `lsp` 子命令）二进制。
  参考本仓库 `scripts/build_lsp_android.sh` 得到 arm64-v8a / armeabi-v7a 二进制，
  放在 ACode 可访问、且在 ACode 沙盒 PATH 中的绝对路径（例如 `/public/.local/bin/vus`）。

## 插件作用

- 把 `.vus` 注册为 `vus` 语言（这是 LSP 能按语言路由服务器的前提）。
- 注册/接管 VUS 语言服务器 `vus lsp`。

## 使用

1. 确保 `vus` 二进制就位。
2. 安装并启用本插件，完全重启 ACode。
3. 打开任意 `.vus` 文件，输入 `图形_` 触发普通补全、
   `.:` 触发详细补全、`..:` 触发命令候选。

## 配置

`main.js` 顶部有可调配置：

- `VUS_EXECUTABLE`：vus 可执行文件的绝对路径（推荐填写，避开 PATH 缺失）。
- `VUS_BRIDGE_URL`：可选，填了则改为直连外部 ws-lsp-bridge 的 WebSocket 地址。

## 验证

打开 ACode 的 **JavaScript 控制台**，若插件生效应看到：

```
[vus-lsp] 插件正在初始化 ...
[vus-lsp] 已注册 .vus 语言模式 (languageId=vus)
[vus-lsp] 已注册 (defineServer) command=... lsp
```