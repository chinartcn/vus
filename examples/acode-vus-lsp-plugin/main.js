/**
 * VUS LSP — ACode 插件（CodeMirror LSP API）
 *
 * 运行方式：ACode 插件生命周期通过 acode.setPluginInit(id, init) 触发，
 * 这是 ACode 官方规定的入口（而非 module.exports）。
 *
 * - acode.require("lsp").defineServer() + upsert()
 *   注册 VUS 语言服务器；ACode 经 AXS bridge 代理为 WebSocket 并自动拉起 `vus lsp`。
 * - acode.require("editorLanguages").register()
 *   把 .vus 映射为 languageId "vus"，LSP 客户端按此语言路由服务器。
 * - vus 二进制随插件打包（bin/<abi>/vus），按设备 ABI 选用。
 *
 * 本文件刻意保持单文件、零依赖，便于 ACode 直接加载。
 */
import plugin from "./plugin.json";

/* ============ 领域常量（集中声明，避免散落硬编码） ============ */

/** CodeMirror/LSP 内部语言 id 与展示名 */
const LANGUAGE_ID = "vus";
const LANGUAGE_EXTENSIONS = ["vus"];
const LANGUAGE_CAPTION = "VUS";

/** LSP 服务器标识 */
const SERVER_ID = "vus-lsp";
const SERVER_LABEL = "VUS";

/** 启动 vus 二进制时附加的子命令 */
const LSP_SUBCOMMAND = "lsp";

/** 插件内的二进制架构目录名 */
const ABI_ARM64 = "arm64-v8a";
const ABI_ARM32 = "armeabi-v7a";

/** 二进制在插件内的路径：<base>/<BIN_DIR>/<abi>/<BIN_NAME> */
const BIN_DIR = "bin";
const BIN_NAME = "vus";

/* 全局：服务器是否已成功注册，避免重复注册 */
let serversRegistered = false;

/* ============ ABI 探测：arm64-v8a / armeabi-v7a / null ============ */

function detectAbi() {
  try {
    const ua = String(navigator.userAgent || "").toLowerCase();
    const cpu = (() => {
      try {
        return typeof device !== "undefined" && device.cpu
          ? String(device.cpu).toLowerCase()
          : "";
      } catch (err) {
        console.warn("[vus-lsp] 读取 device.cpu 失败", err);
        return "";
      }
    })();
    const blob = ua + " " + cpu;
    if (blob.includes("arm64") || blob.includes("aarch64")) return ABI_ARM64;
    if (blob.includes("armeabi-v7a") || blob.includes("armv7") || blob.includes("armeabi"))
      return ABI_ARM32;
  } catch (err) {
    console.warn("[vus-lsp] ABI 探测异常", err);
  }
  return null;
}

/** file://baseUrl -> 绝对目录（去掉协议与末尾斜杠） */
function resolveDir(baseUrl) {
  let u = String(baseUrl || "");
  if (u.startsWith("file://")) u = u.slice("file://".length);
  return u.replace(/\/+$/, "");
}

/** 插件内二进制目录：<base>/<BIN_DIR>/<abi>/<BIN_NAME> */
function binaryPath(base, abi) {
  return base + "/" + BIN_DIR + "/" + abi + "/" + BIN_NAME;
}

/** 从插件安装目录定位 vus 二进制绝对路径 */
function locateBinary(base, abi) {
  const fallbackAbi = abi || ABI_ARM64;
  // 优先精确 ABI；若目录里只有单架构，逐级回退
  const candidates = [fallbackAbi];
  if (!candidates.includes(ABI_ARM64)) candidates.push(ABI_ARM64);
  if (!candidates.includes(ABI_ARM32)) candidates.push(ABI_ARM32);

  for (const a of candidates) {
    const p = binaryPath(base, a);
    try {
      const fs = acode.require("fs");
      if (fs && typeof fs.isFile === "function") {
        if (fs.isFile(p)) return p;
        // 明确有判断能力但不存在，换下个 ABI 候选
        continue;
      }
    } catch (err) {
      console.warn("[vus-lsp] fs.isFile 不可用，采用首个候选", p, err);
    }
    // fs 不可用时无校验能力，直接采用当前候选
    return p;
  }
  return binaryPath(base, fallbackAbi);
}

/* ============ 语言注册：把 .vus 映射为 languageId "vus" ============ */

function registerVusLanguage() {
  const editorLanguages = acode.require("editorLanguages");
  if (!editorLanguages || typeof editorLanguages.register !== "function") {
    throw new Error("editorLanguages 模块不可用");
  }
  editorLanguages.register(
    LANGUAGE_ID,
    LANGUAGE_EXTENSIONS,
    LANGUAGE_CAPTION,
    async () => [] // 高亮可选；返回空扩展以保证加载成功
  );
  console.log("[vus-lsp] 已注册 .vus 语言 (languageId=" + LANGUAGE_ID + ")");
}

/* ============ 服务器注册：defineServer + upsert ============ */

function registerVusServer(base, abi) {
  const lsp = acode.require("lsp");
  if (!lsp || typeof lsp.defineServer !== "function") {
    throw new Error("当前 ACode 无 defineServer API，请升级至最新版（≥ v1002）");
  }
  const bin = locateBinary(base, abi);
  const server = lsp.defineServer({
    id: SERVER_ID,
    label: SERVER_LABEL,
    languages: [LANGUAGE_ID],
    useWorkspaceFolders: true,
    command: bin,
    args: [LSP_SUBCOMMAND],
    checkCommand: "test -x '" + bin + "'",
    initializationOptions: { provideFormatter: false },
  });
  lsp.upsert(server);
  serversRegistered = true;
  console.log("[vus-lsp] 服务器已注册 command=" + bin + " " + LSP_SUBCOMMAND);
}

/** 延迟到编辑器就绪后执行注册（LSP API 依赖码镜编辑器初始化） */
function registerVus() {
  if (serversRegistered) return;
  const abi = detectAbi();
  const base = resolveDir(this?.baseUrl || "");

  // 语言注册失败也不阻断服务器注册，但给出明确日志
  try {
    registerVusLanguage();
  } catch (err) {
    console.error("[vus-lsp] 语言注册失败：", err);
  }

  // 服务器注册失败需要保留重试机会（不置 serversRegistered）
  try {
    registerVusServer(base, abi);
  } catch (err) {
    console.error("[vus-lsp] 服务器注册失败：", err);
  }
}

/* ============ ACode 官方入口 ============ */

function init(baseUrl) {
  try {
    const editorManager = acode.require("editorManager");
    if (editorManager && typeof editorManager.on === "function") {
      // 编辑器就绪后再注册；activeFileChange 触发时 baseUrl 已可用
      editorManager.on("activeFileChange", () => registerVus.call({ baseUrl }));
    }
  } catch (err) {
    console.warn("[vus-lsp] 监听 activeFileChange 失败：", err);
  }
  // 先尝试一次，失败无副作用（activeFileChange 会重试直到就绪）
  registerVus.call({ baseUrl });
}

function unmount() {
  serversRegistered = false;
  try {
    const lsp = acode.require("lsp");
    if (lsp && typeof lsp.servers?.unregister === "function") {
      lsp.servers.unregister(SERVER_ID);
    }
  } catch (err) {
    console.warn("[vus-lsp] 卸载时注销服务器失败：", err);
  }
}

acode.setPluginInit(plugin.id, init);
acode.setPluginUnmount(plugin.id, unmount);