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
/**
 * 注意：不要在模块顶层调用 acode.require(...)。
 * 任一 API 在当前 ACode 版本不存在时，顶层抛错会让整个插件加载失败、
 * init() 永不执行。因此所有 acode.require 都放到函数内并用 try 保护，
 * 保证插件始终能加载、至少完成语言注册。
 */

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
 * 可选的 WebSocket→LSP 桥地址（ws-lsp-bridge）。
 * 若填了它，就改为直连该 WebSocket（需在设备上先运行 wslsp，
 * bridge 会按 URL 里的 args= 拉起 `vus lsp`），不再依赖 ACode 内置 AXS。
 * 留空则走 ACode 官方自动拉起（command + args）。
 * 例：VUS_BRIDGE_URL = "ws://localhost:3030/vus?args=vus,lsp&type=stdio";
 */
const VUS_BRIDGE_URL = "";

/**
 * 注册 VUS 语言模式：把扩展名 .vus 映射到 languageId "vus"。
 * 这是补全能弹出的前提——ACode 靠 languageId 决定为哪个文件启动 LSP 客户端。
 */
function registerLanguage() {
  // loader 返回 CodeMirror 语言扩展。这里返回空扩展，仅负责把 .vus 关联到
  // "vus" languageId；若想给中英文高亮，可在此改用 @lezer/lr 的 StreamLanguage。
  const loader = () => Promise.resolve([]);
  let editorLanguages = null;
  try { editorLanguages = acode.require("editorLanguages"); } catch (_) {}
  if (editorLanguages && typeof editorLanguages.register === "function") {
    editorLanguages.register("vus", ["vus"], "VUS", loader);
    console.log("[vus-lsp] 已注册 .vus 语言模式 (languageId=vus)");
  } else {
    // 兼容旧版 ACode（Ace 时代）的 mode 注册
    try {
      const aceModes = acode.require("aceModes");
      aceModes.addMode("vus", ["vus"], "VUS");
      console.log("[vus-lsp] 已注册 .vus 语言模式 (aceModes)");
    } catch (e) {
      console.warn("[vus-lsp] 无法注册 .vus 语言模式", e);
    }
  }
}

/**
 * 兜底：把当前活动文件强制切到 vus 模式。
 * register() 仅把 .vus 关联为可用语言；对已在编辑器里的文件，CodeMirror
 * 不一定自动应用该模式，而 LSP 客户端是按文件的 languageId 路由服务器的，
 * 所以必须显式 setMode("vus")，否则服务器永远不会被启动、补全不弹。
 * 见 docs.acode.app "Apply A Mode To Active File"。
 */
function applyVusModeToActiveFile() {
  try {
    const em = acode.require("editorManager");
    const f = em && em.activeFile;
    if (f && (f.extension === "vus" || String(f.name || "").endsWith(".vus"))) {
      f.setMode("vus");
      console.log("[vus-lsp] 已对活动文件应用 vus 模式:", f.name);
    }
  } catch (e) {
    // 某些版本 editorManager API 不同，忽略即可，register 的扩展名映射仍在
  }
}

async function init() {
  // ===== 诊断探针：证明插件 init() 确实被执行 =====
  // 只要本节代码运行，就会向本机 ws://127.0.0.1:3030 发一次连接。
  // 先跑 `wslsp`（ws-lsp-bridge），重装插件并重启 ACode 后，
  // 若 wslsp 终端出现 "New connection: ...vus-lsp-probe..." 即证明插件在跑。
  // 定位完成后可删除本节。
  try {
    if (typeof WebSocket !== "undefined") {
      const p = new WebSocket("ws://127.0.0.1:3030/vus-lsp-probe");
      p.onclose = p.onerror = function () {};
    }
  } catch (_) {}

  console.log("[vus-lsp] 插件正在初始化 ...");
  const lsp = (() => { try { return acode.require("lsp"); } catch (_) { return null; } })();
  // 先注册语言模式，确保 .vus 能被识别，LSP 才会为其启动
  registerLanguage();
  // 立即对当前打开的文件兜底 setMode，保证 languageId 是 vus
  applyVusModeToActiveFile();
  // 订阅文件切换，对新打开的 .vus 同样兜底（监听不到也不致命）
  try {
    const em = acode.require("editorManager");
    if (em && typeof em.on === "function") {
      const events = ["activeFileChange", "file-switch", "file-open"];
      for (const ev of events) {
        try { em.on(ev, applyVusModeToActiveFile); } catch (_) {}
      }
    }
  } catch (_) {}

  // 服务器定义（与命令/参数统一，便于新旧 API 共用）
  const command = VUS_COMMAND;
  const args = ["lsp"];
  const checkCommand = VUS_EXECUTABLE ? `test -x "${VUS_EXECUTABLE}"` : "command -v vus";

  // —— 新 API（当前 ACode 推荐）——
  // 若设了 VUS_BRIDGE_URL，以 WebSocket 直连外部 ws-lsp-bridge
  // （需在设备上先运行 wslsp；bridge 按 args= 拉起 vus lsp）。
  // 否则 defineServer 把 command/args 自动转成 ACode 内置 AXS 桥接。
  if (lsp && typeof lsp.defineServer === "function") {
    const server = lsp.defineServer(
      VUS_BRIDGE_URL
        ? {
            id: "vus-lsp",
            label: "VUS",
            languages: ["vus"],
            transport: { kind: "websocket", url: VUS_BRIDGE_URL },
            useWorkspaceFolders: false,
            enabled: true,
          }
        : {
            id: "vus-lsp",
            label: "VUS",
            languages: ["vus"],
            useWorkspaceFolders: true,
            command,
            args,
            checkCommand,
            initializationOptions: { provideFormatter: false },
          }
    );
    lsp.upsert(server); // upsert：已存在同 id 时替换而不报错
    console.log(
      "[vus-lsp] 已注册 (defineServer) " +
        (VUS_BRIDGE_URL ? "websocket=" + VUS_BRIDGE_URL : "command=" + command + " lsp")
    );
    return;
  }

  // —— 老 API（兼容较早内置 LSP）——
  // 手动声明 websocket transport + AXS stdio 桥接，作用等价于 defineServer。
  const legacy = {
    id: "vus-lsp",
    label: "VUS",
    languages: ["vus"],
    transport: { kind: "websocket" },
    launcher: {
      bridge: { kind: "axs", command, args },
      checkCommand,
    },
    enabled: true,
    initializationOptions: { provideFormatter: false },
  };
  if (lsp && typeof lsp.registerServer === "function") {
    lsp.registerServer(legacy, { replace: true });
    console.log("[vus-lsp] 已注册 (legacy/registerServer) command=" + command + " lsp");
  } else if (lsp && typeof lsp.upsert === "function") {
    lsp.upsert(legacy);
    console.log("[vus-lsp] 已注册 (legacy/upsert) command=" + command + " lsp");
  } else if (lsp && typeof lsp.register === "function") {
    lsp.register(legacy);
    console.log("[vus-lsp] 已注册 (legacy/register) command=" + command + " lsp");
  } else {
    console.error("[vus-lsp] 未找到可用的 LSP 注册 API（acode.require(\"lsp\") 为 null 或缺少方法）");
  }
}

module.exports = { init };