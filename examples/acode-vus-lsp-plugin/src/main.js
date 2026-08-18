/**
 * VUS 本地补全 — ACode 插件（纯浏览器 JS + CodeMirror autocomplete）
 *
 * 架构参考 ACode 的 Prettier 插件：无需二进制 / LSP 服务器，完全在浏览器端
 * 通过 CodeMirror 6 的 @codemirror/autocomplete 实现补全。
 *
 * 关键点：
 * - 用 acode.require("editorLanguages").register("vus", ["vus"], "VUS", factory)
 *   注册语言，其中 factory 返回的 cm6 扩展会在创建 .vus 编辑器时自动注入，
 *   从而启用本地补全（无需跨进程通信，避免 LSP 二进制在 AXS 中启动的问题）。
 * - 补全数据来自 src/vusBuiltins.js（由 src/lsp/vus_builtin.c 自动生成）。
 *
 * 生命周期遵循 ACode 官方规范：acode.setPluginInit / acode.setPluginUnmount。
 */
import { autocompletion } from "@codemirror/autocomplete";
import { VUS_BUILTINS } from "./vusBuiltins.js";

/* ============ 领域常量 ============ */
const LANGUAGE_ID = "vus";
const LANGUAGE_EXTENSIONS = ["vus"];
const LANGUAGE_CAPTION = "VUS";

/* ============ 标识符字符集：ASCII 字母数字、下划线、中文（CJK） ============ */
/** 单个标识符字符：用于 validFor，也用于构造光标词匹配正则 */
const IDENT_CHAR = /[\w\u4e00-\u9fa5]/u;
/** 光标前的完整词（后缀截取），中文函数名也可识别 */
const IDENT_RE = /[\w\u4e00-\u9fa5]+$/u;

/** autocomplete 的 type 用英文字母作为图标分组键；缺省用 "fx" */
function kindLabel(kind) {
  return kind || "fx";
}

/** 预编译补全选项（69 个内置函数）：标签=函数名，详情=签名，info=说明+示例 */
const completionOptions = VUS_BUILTINS.map((b) => ({
  label: b.name,
  detail: b.signature,
  info:
    (b.description ? b.description + "\n\n" : "") +
    "示例：\n" +
    (b.example || ""),
  type: kindLabel(b.kind)
}));

/**
 * 本地补全 CompletionSource：
 * 命中内置函数名（含中文），从当前光标词起点开始替换。
 */
function vusCompletionSource(context) {
  const word = context.matchBefore(IDENT_RE);
  const from = word ? word.from : context.pos;
  return {
    from,
    options: completionOptions,
    validFor: IDENT_CHAR
  };
}

/** 语言扩展：启用 autocompletion 并使用我们的补全源 */
const vusLanguageExtensions = autocompletion({
  override: [vusCompletionSource]
});

/** editorLanguages.register 要求的异步工厂：返回 cm6 扩展数组 */
async function vusLanguageFactory() {
  return [vusLanguageExtensions];
}

/* ============ ACode 生命周期 ============ */

let registered = false;

function init() {
  if (registered) return;
  registered = true;
  try {
    const editorLanguages = acode.require("editorLanguages");
    if (!editorLanguages || typeof editorLanguages.register !== "function") {
      throw new Error("editorLanguages 模块不可用");
    }
    editorLanguages.register(
      LANGUAGE_ID,
      LANGUAGE_EXTENSIONS,
      LANGUAGE_CAPTION,
      vusLanguageFactory
    );
    console.log("[vus-complete] 已注册 .vus 语言与本地补全 (" + completionOptions.length + " 个函数)");
    if (typeof toast === "function") toast("VUS 补全已启用");
  } catch (err) {
    console.error("[vus-complete] 初始化失败：", err);
    registered = false;
  }
}

function unmount() {
  registered = false;
}

const pluginId = (typeof plugin !== "undefined" && plugin.id) || "org.vus.lsp";
acode.setPluginInit(pluginId, init);
acode.setPluginUnmount(pluginId, unmount);