> 文档版本：v1.0_apk（APK 功能时代）
> 最后更新时间：2026-09-04


# VUS 本地补全 — ACode 插件

为 ACode 编辑器中的 `.vus` 文件提供**函数补全**：内置 69 个 VUS 内置函数的
名称、签名、中文说明与示例。

纯浏览器 JS 实现（无需二进制 / LSP 服务器），架构参考 ACode 的 Prettier 插件，
使用 CodeMirror 6 的 `@codemirror/autocomplete` 在本地完成补全，不依赖跨进程通信。

## 特性

- 把 `.vus` 注册为 `VUS` 语言（`editorLanguages.register`）。
- 基于内置函数元数据（`src/vusBuiltins.js`，由 `src/lsp/vus_builtin.c` 生成）提供补全：
  - 标签 = 函数名；详情 = 完整签名；info = 中文说明 + 示例；分组 = 类别（图形/输入输出/JSON/日期……）。
- 支持中文函数名（`图形_`、`打印` 等），输入 `图形_` 即可联想全部图形函数。
- 无后端、无二进制、无网络请求，安装即用。

## 安装

1. 构建：`npm install && npm run build`，产物为 `dist/main.js`。
2. 打包：把 `plugin.json`、`dist/main.js`、`icon.png`、`README.md`、`changelogs.md`
   打包成 `zip`（根目录含 `plugin.json`）。
3. 在 ACode 的插件管理器中选择该 zip 安装并启用。
4. 打开任意 `.vus` 文件，输入 `图形_` 或任意内置函数前缀触发补全。

> 依赖：ACode ≥ v1000（含 `editorLanguages.register` 语言工厂注入能力）。

## 目录结构

```
plugin.json         插件清单（main 指向 dist/main.js）
src/
  main.js           入口：注册语言工厂 → 注入 cm6 autocompletion 扩展
  vusBuiltins.js    内置函数元数据（自动生成，勿手改）
dist/
  main.js           webpack 构建产物（插件实际加载的入口）
webpack.config.js  构建配置
package.json        依赖（@codemirror/autocomplete 等）
```

## 开发

修改 `src/main.js` 或补全数据后，运行：

```sh
npm install
npm run build
```

## 数据源

`src/vusBuiltins.js` 中的函数元数据从语言源码
`src/lsp/vus_builtin.c`（权威表）自动生成。如需更新内置函数列表，请修改
`vus_builtin.c` 后重新生成该 JS 模块。

## 故障排查

- 无补全：确认打开的是 `.vus` 文件，且 ACode 已加载并启用插件（设置 → 插件）。
- 装完没生效：完全重启 ACode，让语言工厂注册生效。
- 控制台无日志：检查插件是否被 ACode 正常加载；日志前缀为 `[vus-complete]`。