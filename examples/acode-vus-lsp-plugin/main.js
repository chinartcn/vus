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

async function init() {
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