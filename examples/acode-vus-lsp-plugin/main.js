/**
 * VUS LSP — ACode 插件主脚本
 *
 * 通过 ACode 的 LSP API（acode.require("lsp")）注册 VUS 语言服务器。
 * VUS 服务器以 `vus lsp` 形式从 stdin 读 JSON-RPC、向 stdout 写响应，
 * ACode 通过其 AXS 桥接作为本地 stdio 服务器拉起该命令。
 *
 * 前置条件：
 *   设备上存在可执行的 `vus`（含 lsp 子命令）并位于 PATH / bin 目录。
 *   参考 scripts/build_lsp_android.sh 得到 arm64-v8a / armeabi-v7a 二进制，
 *   放置到 Termux 的 ~/.local/bin，或用绝对路径覆盖下方 command。
 */
const lsp = acode.require("lsp");
// 语言模式注册 API（CodeMirror 6 版）。把 .vus 关联到 "vus" languageId，
// LSP 客户端按此 id 路由到下方登记的服务器，补全才能弹出。
const editorLanguages = acode.require("editorLanguages");

/**
 * 设备上 vus 可执行文件的绝对路径。
 * 若通过 Termux 安装并加入 PATH（command -v vus 可命中），
 * 可将此值留空，走下方 command: "vus"。
 */
const VUS_EXECUTABLE = "";

/**
 * 启动 VUS 语言服务器的命令 + 参数。
 * 留空 command 时 ACode 会将其解析进 AXS 桥接的 stdio 启动器；
 * 若配了绝对路径，则优先使用该可执行文件。
 */
const VUS_COMMAND = VUS_EXECUTABLE || "vus";

/**
 * 注册 VUS 语言模式：把扩展名 .vus 映射到 languageId "vus"。
 * 这是补全能弹出的前提——ACode 靠 languageId 决定为哪个文件启动 LSP 客户端。
 */
function registerLanguage() {
  // loader 返回 CodeMirror 语言扩展。这里返回空扩展，仅负责把 .vus 关联到
  // "vus" languageId；若想给中英文高亮，可在此改用 @lezer/lr 的 StreamLanguage。
  const loader = () => Promise.resolve([]);
  if (editorLanguages && typeof editorLanguages.register === "function") {
    editorLanguages.register("vus", ["vus"], "VUS", loader);
  } else {
    // 兼容旧版 ACode（Ace 时代）的 mode 注册
    try {
      const aceModes = acode.require("aceModes");
      aceModes.addMode("vus", ["vus"], "VUS");
    } catch (e) {
      console.warn("[vus-lsp] 无法注册 .vus 语言模式", e);
    }
  }
  console.log("[vus-lsp] 已注册 .vus 语言模式 (languageId=vus)");
}

async function init() {
  // 先注册语言模式，确保 .vus 能被识别，LSP 才会为其启动
  registerLanguage();

  const server = lsp.defineServer({
    id: "vus-lsp",
    label: "VUS",
    // VUS 源码文件的语言 id；.vus 需在 ACode 中被识别为该语言（见文档）。
    languages: ["vus"],
    useWorkspaceFolders: true,
    command: VUS_COMMAND,
    args: ["lsp"],
    // 启动前校验可执行文件是否存在
    checkCommand: VUS_EXECUTABLE
      ? `test -x "${VUS_EXECUTABLE}"`
      : "command -v vus",
    initializationOptions: {
      provideFormatter: false,
    },
  });

  // upsert：可重复安装，已有同 id 定义时替换而不报错
  lsp.upsert(server);
  console.log("[vus-lsp] 服务器已注册（command=" + VUS_COMMAND + " lsp）");
}

module.exports = { init };