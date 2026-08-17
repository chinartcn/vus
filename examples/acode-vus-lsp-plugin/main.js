/**
 * VUS LSP — ACode 插件（CodeMirror LSP API）
 *
 * 运行方式：ACode 插件生命周期通过 acode.setPluginInit(id, init) 触发，
 * 这是 ACode 官方规定的入口（而非 module.exports）。
 *
 * - acode.require("lsp").defineServer() + upsert()
 *   注册 VUS 语言服务器；ACode 经 AXS bridge 自动拉起 `vus lsp`。
 * - acode.require("editorLanguages").register()
 *   把 .vus 映射为 languageId "vus"，LSP 客户端按此路由服务器。
 * - vus 二进制随插件打包（bin/<abi>/vus），按设备 ABI 选用。
 */
import plugin from "./plugin.json";

let serversRegistered = false;

/** ABI 探测：arm64-v8a / armeabi-v7a / null */
function detectAbi() {
  try {
    const ua = String(navigator.userAgent || "").toLowerCase();
    const cpu = (() => {
      try {
        return typeof device !== "undefined" && device.cpu
          ? String(device.cpu).toLowerCase()
          : "";
      } catch (_) {
        return "";
      }
    })();
    const blob = ua + " " + cpu;
    if (blob.includes("arm64") || blob.includes("aarch64")) return "arm64-v8a";
    if (
      blob.includes("armeabi-v7a") ||
      blob.includes("armv7") ||
      blob.includes("armeabi")
    )
      return "armeabi-v7a";
  } catch (_) {}
  return null;
}

/** file://baseUrl -> 绝对目录（去掉协议与末尾斜杠） */
function resolveDir(baseUrl) {
  let u = String(baseUrl || "");
  if (u.startsWith("file://")) u = u.slice(7);
  return u.replace(/\/+$/, "");
}

/** 从插件安装目录定位 vus 二进制绝对路径 */
function locateBinary(base, abi) {
  const fallbackAbi = abi || "arm64-v8a";
  // 优先精确 ABI；若目录里只有单架构，逐级向上回退
  const candidates = [fallbackAbi];
  if (!candidates.includes("arm64-v8a")) candidates.push("arm64-v8a");
  if (!candidates.includes("armeabi-v7a")) candidates.push("armeabi-v7a");
  for (const a of candidates) {
    const p = base + "/bin/" + a + "/vus";
    // 尽力校验存在；存在则选用，不存在则继续试下一个候选
    try {
      const fs = acode.require("fs");
      if (fs && typeof fs.isFile === "function") {
        if (fs.isFile(p)) return p;
        continue; // 明确存在性校验能力，此路径不存在则换 ABI
      }
    } catch (_) {}
    // fs API 不可用时，无校验能力，直接选用最优先候选
    return p;
  }
  return base + "/bin/" + fallbackAbi + "/vus";
}

/** 延迟到编辑器就绪后再执行注册（LSP API 依赖码镜编辑器初始化） */
function registerVus() {
  if (serversRegistered) return;
  const abi = detectAbi();
  const base = resolveDir(this?.baseUrl || "");

  // 1) 注册 .vus 语言 -> languageId "vus"
  try {
    const editorLanguages = acode.require("editorLanguages");
    if (editorLanguages && typeof editorLanguages.register === "function") {
      editorLanguages.register(
        "vus",
        ["vus"],
        "VUS",
        async () => [] // 高亮可选；返回空扩展以保证加载成功
      );
      console.log("[vus-lsp] 已注册 .vus 语言");
    }
  } catch (e) {
    console.error("[vus-lsp] 语言注册失败", e);
  }

  // 2) 注册语言服务器（官方 defineServer + upsert）
  try {
    const lsp = acode.require("lsp");
    if (!lsp || typeof lsp.defineServer !== "function") {
      console.error("[vus-lsp] 当前 ACode 无 defineServer API，请升级至最新版");
      return;
    }
    const bin = locateBinary(base, abi);
    const server = lsp.defineServer({
      id: "vus-lsp",
      label: "VUS",
      languages: ["vus"],
      useWorkspaceFolders: true,
      command: bin,
      args: ["lsp"],
      checkCommand: "test -x '" + bin + "'",
      initializationOptions: { provideFormatter: false },
    });
    lsp.upsert(server);
    serversRegistered = true;
    console.log("[vus-lsp] 服务器已注册 command=" + bin + " lsp");
  } catch (e) {
    console.error("[vus-lsp] 服务器注册失败", e);
  }
}

// —— ACode 官方入口：setPluginInit ——
acode.setPluginInit(plugin.id, (baseUrl, $page, cache) => {
  // 保存 baseUrl 供注册使用
  try {
    plugin.__baseUrl = baseUrl;
    // init 接收的第一个参数就是 baseUrl；用一个可复用作用域绑定
  } catch (_) {}

  // ：LSP API 需要编辑器可用，最常见做法是延迟到文档打开后注册。
  // 这里先做一次尝试，失败无副作用。
  registerVus.call({ baseUrl });

  // 监听活动文件切换，确保 .vus 文档语言正确并再次触发注册
  try {
    const editorManager = acode.require("editorManager");
    if (editorManager && typeof editorManager.on === "function") {
      editorManager.on("activeFileChange", () => {
        registerVus.call({ baseUrl });
      });
    }
  } catch (_) {}
});

acode.setPluginUnmount(plugin.id, () => {
  serversRegistered = false;
  try {
    const lsp = acode.require("lsp");
    if (lsp && typeof lsp.servers?.unregister === "function") {
      lsp.servers.unregister("vus-lsp");
    }
  } catch (_) {}
});