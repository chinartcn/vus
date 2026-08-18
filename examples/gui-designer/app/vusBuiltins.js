/* ================================================================
 * vusBuiltins.js —— VUS 内置函数补全列表 + CodeMirror VUS 高亮模式
 * ================================================================ */

window.VUS = window.VUS || {};
var C = window.VUS;

// 内置函数名（用于补全提示；与 docs_index.json 保持一致）
C.BUILTIN_NAMES = [
  "图形_初始化", "图形_刷新", "图形_保持", "图形_画点", "图形_画线",
  "图形_矩形", "图形_填充", "图形_文字", "图形_圆角矩形", "图形_圆角填充",
  "图形_画圆", "图形_填充圆", "图形_圆弧", "图形_外观", "图形_主题",
  "图形_取事件", "图形_按键", "图形_按键码", "图形_鼠标位置", "图形_鼠标x",
  "图形_鼠标y", "图形_滚轮", "图形_键按下", "图形_悬停", "图形_按钮",
  "图形_按钮点击", "图形_滑块", "图形_滑块值", "图形_开关", "图形_开关值",
  "图形_微调", "图形_微调值", "图形_单选", "图形_单选值", "图形_进度",
  "图形_文本框", "图形_图片", "图形_标签", "图形_卡片", "图形_面板",
  "图形_圆环", "图形_列表", "图形_列表行", "图形_列表选中", "图形_列表行点击",
  "图形_画布", "图形_画布命中", "图形_画布点", "图形_滚动容器", "图形_滚动容器滚",
  "图形_滚动容器偏移", "图形_页面_打开", "图形_页面_返回", "图形_页面_当前",
  "图形_背景图", "图形_动画_打开", "图形_动画_下一步", "图形_动画_帧数",
  "图形_动画_关闭", "图形_MD",
  "打印", "输入", "睡眠", "转数字", "转文本", "typeof", "对象文本",
  "JSON_解析", "JSON_生成", "JSON_查询", "字典_键",
  "日期_现在", "日期_时间戳", "日期_年", "日期_月", "日期_日", "日期_时",
  "日期_分", "日期_秒", "日期_格式化", "日期_解析", "日期_从时间戳",
  "音频_打开", "音频_播放", "音频_暂停", "音频_续", "音频_跳转",
  "音频_进度", "音频_时长",
  "传感器_读", "传感器_可用", "时钟"
];

// VUS 关键字（高亮）
C.KEYWORDS = ["如果", "否则", "否则如果", "当循环", "返回", "且", "或", "不是",
              "打印", "true", "false"];

// ---- CodeMirror VUS 高亮模式 ----
if (typeof CodeMirror !== "undefined" && CodeMirror.defineMode) {
  var builtinSet = {};
  C.BUILTIN_NAMES.forEach(function (n) { builtinSet[n] = true; });
  var kwSet = {};
  C.KEYWORDS.forEach(function (n) { kwSet[n] = true; });

  CodeMirror.defineMode("vus", function () {
    return {
      token: function (stream, state) {
        if (stream.eatSpace()) return null;
        // 注释：#
        if (stream.match(/^#/)) { stream.skipToEnd(); return "comment"; }
        // 字符串 "..."（保留转义）
        if (stream.match(/^"(\\.|[^"\\])*"/)) return "string";
        if (stream.match(/^'(\\.|[^'\\])*'/)) return "string";
        // 数字：十六进制 / 十进制
        if (stream.match(/^0x[0-9a-fA-F]+/)) return "number";
        if (stream.match(/^\d+(\.\d+)?/)) return "number";
        // 标识符（含中文函数名与 ASCII）
        if (stream.match(/^[\u4e00-\u9fa5A-Za-z0-9_]+/)) {
          var w = stream.current();
          if (kwSet[w]) return "keyword";
          if (builtinSet[w]) return "builtin";
          return "variable-2";
        }
        stream.next();
        return null;
      }
    };
  });
}