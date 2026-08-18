// ACode 插件构建配置：将 src/main.js 及 CodeMirror 补全依赖打包为 dist/main.js。
// 目标为 ACode 的 Chromium WebView，支持现代 ES 语法，无需 Babel 转译。
const path = require("path");

module.exports = {
  mode: "production",
  entry: path.resolve(__dirname, "src/main.js"),
  output: {
    path: path.resolve(__dirname, "dist"),
    filename: "main.js",
    // ACode 以 CommonJS 方式加载插件入口
    libraryTarget: "commonjs2"
  },
  resolve: {
    extensions: [".js"]
  }
};