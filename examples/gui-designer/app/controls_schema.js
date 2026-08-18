/* ================================================================
 * controls_schema.js —— 控件库定义 + 属性面板动态 schema
 * 控件类型驱动属性面板：每种类型拥有自己的属性字段数组。
 * 图标一律使用文本几何符号（禁止使用 emoji 作图标）。
 * ================================================================ */

window.VUS = window.VUS || {};
var C = window.VUS;

// 左侧控件库条目：type / label / 几何图标 / 名称前缀
C.PALETTE = [
  { type: "label",       label: "文字",     icon: "T" },
  { type: "button",      label: "按钮",     icon: "▭" },
  { type: "slider",      label: "滑块",     icon: "▁◉" },
  { type: "switch",      label: "开关",     icon: "◧" },
  { type: "spin",        label: "微调",     icon: "▲▼" },
  { type: "radio",       label: "单选",     icon: "◉" },
  { type: "round_rect",  label: "圆角矩形", icon: "▢" },
  { type: "fill_rect",   label: "填充矩形", icon: "▦" },
  { type: "circle",      label: "空心圆",   icon: "○" },
  { type: "fill_circle", label: "实心圆",   icon: "●" },
  { type: "arc",         label: "圆弧",     icon: "◡" },
  { type: "progress",    label: "进度条",   icon: "▬" },
  { type: "textbox",     label: "文本框",   icon: "▯" },
];

C.NAME_PREFIX = {
  label: "t", button: "b", slider: "s", switch: "sw", spin: "sp",
  radio: "r", round_rect: "rr", fill_rect: "fr", circle: "c",
  fill_circle: "fc", arc: "a", progress: "p", textbox: "tb",
};

// 可拖拽右下角手柄改变大小的控件（需同时有 w/h）
C.RESIZABLE = new Set(["button", "round_rect", "fill_rect", "progress", "textbox"]);

// 属性面板 schema：{ key, label, kind }，kind ∈ text | number | color | bool
// 不同控件类型 → 不同属性集合。
var PROPS = {
  label:       [{ key: "name", label: "名称", kind: "text" }, { key: "text", label: "文本", kind: "text" },
                { key: "color", label: "颜色", kind: "color" }, { key: "size", label: "字号", kind: "text" },
                { key: "x", label: "X", kind: "number" }, { key: "y", label: "Y", kind: "number" }],
  button:      [{ key: "name", label: "名称", kind: "text" }, { key: "text", label: "文本", kind: "text" },
                { key: "x", label: "X", kind: "number" }, { key: "y", label: "Y", kind: "number" },
                { key: "w", label: "宽", kind: "number" }, { key: "h", label: "高", kind: "number" }],
  slider:      [{ key: "name", label: "名称", kind: "text" }, { key: "x", label: "X", kind: "number" },
                { key: "y", label: "Y", kind: "number" }, { key: "w", label: "宽", kind: "number" },
                { key: "value", label: "当前值", kind: "number" },
                { key: "min", label: "最小值", kind: "number" }, { key: "max", label: "最大值", kind: "number" }],
  switch:      [{ key: "name", label: "名称", kind: "text" }, { key: "x", label: "X", kind: "number" },
                { key: "y", label: "Y", kind: "number" }, { key: "on", label: "开启", kind: "bool" }],
  spin:        [{ key: "name", label: "名称", kind: "text" }, { key: "x", label: "X", kind: "number" },
                { key: "y", label: "Y", kind: "number" }, { key: "value", label: "当前值", kind: "number" },
                { key: "step", label: "步长", kind: "number" }],
  radio:       [{ key: "name", label: "名称", kind: "text" }, { key: "x", label: "X", kind: "number" },
                { key: "y", label: "Y", kind: "number" }, { key: "item_h", label: "选项行高", kind: "number" },
                { key: "options", label: "选项（; 分隔）", kind: "text" }, { key: "sel", label: "选中项", kind: "number" }],
  round_rect:  [{ key: "name", label: "名称", kind: "text" }, { key: "x", label: "X", kind: "number" },
                { key: "y", label: "Y", kind: "number" }, { key: "w", label: "宽", kind: "number" },
                { key: "h", label: "高", kind: "number" }, { key: "radius", label: "圆角", kind: "number" },
                { key: "color", label: "颜色", kind: "color" }],
  fill_rect:   [{ key: "name", label: "名称", kind: "text" }, { key: "x", label: "X", kind: "number" },
                { key: "y", label: "Y", kind: "number" }, { key: "w", label: "宽", kind: "number" },
                { key: "h", label: "高", kind: "number" }, { key: "color", label: "颜色", kind: "color" }],
  circle:      [{ key: "name", label: "名称", kind: "text" }, { key: "cx", label: "中心 X", kind: "number" },
                { key: "cy", label: "中心 Y", kind: "number" }, { key: "r", label: "半径", kind: "number" },
                { key: "color", label: "颜色", kind: "color" }],
  fill_circle: [{ key: "name", label: "名称", kind: "text" }, { key: "cx", label: "中心 X", kind: "number" },
                { key: "cy", label: "中心 Y", kind: "number" }, { key: "r", label: "半径", kind: "number" },
                { key: "color", label: "颜色", kind: "color" }],
  arc:         [{ key: "name", label: "名称", kind: "text" }, { key: "cx", label: "中心 X", kind: "number" },
                { key: "cy", label: "中心 Y", kind: "number" }, { key: "r", label: "半径", kind: "number" },
                { key: "start", label: "起始角", kind: "number" }, { key: "sweep", label: "扫过角度", kind: "number" },
                { key: "color", label: "颜色", kind: "color" }],
  progress:    [{ key: "name", label: "名称", kind: "text" }, { key: "x", label: "X", kind: "number" },
                { key: "y", label: "Y", kind: "number" }, { key: "w", label: "宽", kind: "number" },
                { key: "h", label: "高", kind: "number" }, { key: "value", label: "进度 %", kind: "number" }],
  textbox:     [{ key: "name", label: "名称", kind: "text" }, { key: "text", label: "文本", kind: "text" },
                { key: "x", label: "X", kind: "number" }, { key: "y", label: "Y", kind: "number" },
                { key: "w", label: "宽", kind: "number" }, { key: "h", label: "高", kind: "number" }],
};
C.PROPS = PROPS;

// 名称 = 前缀 + 自增计数
C.nameCounter = 0;
C.nextName = function (type) {
  var p = C.NAME_PREFIX[type] || "c";
  return p + (++C.nameCounter);
};

// 默认实例（与 vus_export.py 参数序保持一致，保证双向同步）
C.baseFor = function (type) {
  var n = C.nextName(type);
  switch (type) {
    case "label": return { type: type, name: n, x: 20, y: 10, text: "文字", color: 0x000000, size: "12" };
    case "button": return { type: type, name: n, x: 60, y: 60, w: 120, h: 36, text: "按钮" };
    case "slider": return { type: type, name: n, x: 60, y: 60, w: 160, value: 50, min: 0, max: 100 };
    case "switch": return { type: type, name: n, x: 60, y: 60, on: true };
    case "spin": return { type: type, name: n, x: 60, y: 60, value: 3, step: 1 };
    case "radio": return { type: type, name: n, x: 60, y: 60, item_h: 20, options: "A;B;C", sel: 0 };
    case "round_rect": return { type: type, name: n, x: 60, y: 60, w: 120, h: 60, radius: 8, color: 0xE0E0E0 };
    case "fill_rect": return { type: type, name: n, x: 60, y: 60, w: 120, h: 60, color: 0xCCEEFF };
    case "circle": return { type: type, name: n, cx: 150, cy: 110, r: 30, color: 0xFF8800 };
    case "fill_circle": return { type: type, name: n, cx: 150, cy: 110, r: 30, color: 0x44AA44 };
    case "arc": return { type: type, name: n, cx: 150, cy: 110, r: 24, start: 0, sweep: 270, color: 0x666666 };
    case "progress": return { type: type, name: n, x: 60, y: 60, w: 200, h: 18, value: 72 };
    case "textbox": return { type: type, name: n, x: 60, y: 60, w: 160, h: 30, text: "文本框" };
    default: return { type: type, name: n, x: 60, y: 60, w: 120, h: 36 };
  }
};