/* ============================================================
 * app.js —— HTML 高级排版设计器（vanilla JS，无依赖）
 * 拖拽式可视化设计 VUS GUI 界面，导出 .vus 源文件
 * ============================================================ */

"use strict";

/* ---------- 常量 ---------- */

// 左侧控件库条目：label / 名称 / emoji 图标 / 属性面板 schema
const PALETTE = [
  { type: "label",       label: "文字",    icon: "🅣" },
  { type: "button",      label: "按钮",    icon: "🔲" },
  { type: "slider",      label: "滑块",    icon: "📶" },
  { type: "switch",      label: "开关",    icon: "🛝" },
  { type: "spin",        label: "微调",    icon: "🔢" },
  { type: "radio",       label: "单选",    icon: "⚪" },
  { type: "round_rect",  label: "圆角矩形", icon: "▭" },
  { type: "fill_rect",   label: "填充矩形", icon: "◼" },
  { type: "circle",      label: "空心圆",  icon: "⭕" },
  { type: "fill_circle", label: "实心圆",  icon: "🔵" },
  { type: "arc",         label: "圆弧",    icon: "🌙" },
  { type: "progress",    label: "进度条",  icon: "📊" },
  { type: "textbox",     label: "文本框",  icon: "📝" },
];

// 名称前缀
const NAME_PREFIX = {
  label: "t", button: "b", slider: "s", switch: "sw", spin: "sp",
  radio: "r", round_rect: "rr", fill_rect: "fr", circle: "c",
  fill_circle: "fc", arc: "a", progress: "p", textbox: "tb",
};

// 可拖拽右下角手柄改变大小的控件（需同时有 w/h）
const RESIZABLE = new Set(["button", "round_rect", "fill_rect", "progress", "textbox"]);

// 属性面板 schema：{ key, label, kind }，kind ∈ text | number | color | bool | textarea
const PROPS = {
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

/* ---------- 状态 ---------- */

const canvas = document.getElementById("preview");
const ctx = canvas.getContext("2d");
const overlay = document.getElementById("overlay");
const statusEl = document.getElementById("status");

// 主题
let theme = {
  bg: 0xFFFFFF, border: 0x888888,
  highlight: 0x0055AA, fg: 0x333333, text: 0x000000,
};
let W = 480, H = 320, radius = 8;

let controls = [];          // 控件数组
let selectedIndex = -1;     // 选中的控件下标
let nameCounter = 0;        // 名称计数
let drag = null;            // 当前拖拽状态

/* ---------- 颜色工具 ---------- */

function cssColor(v) { return "#" + Number(v).toString(16).padStart(6, "0"); }
// '#rrggbb' -> 十进制整数
function parseColor(hex) { return parseInt(hex.slice(1), 16) || 0; }
const isCircleType = (t) => t === "circle" || t === "fill_circle" || t === "arc";

/* ---------- 控件几何 ---------- */

// 返回值：{x,y,w,h}，用于布局/命中/画布绘制
function controlBox(c) {
  if (isCircleType(c.type)) {
    const r = Number(c.r) || 20;
    return { x: Number(c.cx) || 0, y: Number(c.cy) || 0, w: r * 2, h: r * 2 };
  }
  let w, h;
  switch (c.type) {
    case "switch": w = Number(c.w) || 48; h = Number(c.h) || 26; break;
    case "spin":   w = Number(c.w) || 52; h = Number(c.h) || 28; break;
    case "slider": w = Number(c.w) || 160; h = Number(c.h) || 22; break;
    case "radio": {
      const opts = String(c.options || "").split(";").filter(Boolean);
      const cnt = Math.max(1, opts.length);
      w = Number(c.w) || 170; h = (Number(c.h) || Number(c.item_h) || 20) * cnt;
      break;
    }
    case "label":  w = Number(c.w) || 90; h = Number(c.h) || 24; break;
    default:       w = Number(c.w) || 120; h = Number(c.h) || 36; break;
  }
  return { x: Number(c.x) || 0, y: Number(c.y) || 0, w, h };
}

/* ---------- 画布绘制（WYSIWYG） ---------- */

function roundRect(x, y, w, h, r) {
  r = Math.max(0, Math.min(r, w / 2, h / 2));
  ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.arcTo(x + w, y, x + w, y + h, r);
  ctx.arcTo(x + w, y + h, x, y + h, r);
  ctx.arcTo(x, y + h, x, y, r);
  ctx.arcTo(x, y, x + w, y, r);
  ctx.closePath();
}

function centerText(text, x, y, w, h, color, size) {
  ctx.fillStyle = cssColor(color);
  ctx.font = (size || 12) + "px sans-serif";
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.fillText(String(text), x + w / 2, y + h / 2 + 1);
}
function leftText(text, x, y, color, size) {
  ctx.fillStyle = cssColor(color);
  ctx.font = (size || 12) + "px sans-serif";
  ctx.textAlign = "left";
  ctx.textBaseline = "middle";
  ctx.fillText(String(text), x, y);
}

function drawControl(c) {
  const T = theme;
  const t = c.type;
  const R = radius;

  if (t === "label") {
    leftText(c.text, Number(c.x) || 0, Number(c.y) + ((Number(c.size) || 12) / 2), c.color != null ? c.color : T.text, Number(c.size) || 12);
    return;
  }
  const box = controlBox(c);
  const x = box.x, y = box.y, w = box.w, h = box.h;

  switch (t) {
    case "button": {
      roundRect(x, y, w, h, R);
      ctx.fillStyle = cssColor(T.fg);
      ctx.fill();
      roundRect(x, y, w, h, R);
      ctx.strokeStyle = cssColor(T.border);
      ctx.stroke();
      centerText(c.text, x, y, w, h, T.text, 13);
      break;
    }
    case "slider": {
      const max = Number(c.max) || 100, min = Number(c.min) || 0, v = Number(c.value) || 0;
      const ratio = Math.max(0, Math.min(1, (v - min) / (max - min || 1)));
      roundRect(x, y + h * 0.35, w, h * 0.3, h * 0.15);
      ctx.fillStyle = cssColor(T.border); ctx.fill();
      const kx = x + (w - 10) * ratio;
      ctx.beginPath(); ctx.arc(kx, y + h / 2, Math.max(5, h * 0.5), 0, Math.PI * 2);
      ctx.fillStyle = cssColor(T.highlight); ctx.fill();
      break;
    }
    case "switch": {
      roundRect(x, y, w, h, h / 2);
      ctx.fillStyle = c.on ? cssColor(T.highlight) : cssColor(T.border);
      ctx.fill();
      const kx = c.on ? x + w - h : x;
      ctx.beginPath(); ctx.arc(kx, y + h / 2, h / 2 - 2, 0, Math.PI * 2);
      ctx.fillStyle = "#ffffff"; ctx.fill();
      break;
    }
    case "spin": {
      roundRect(x, y, w, h, R);
      ctx.fillStyle = "#ffffff"; ctx.fill();
      ctx.strokeStyle = cssColor(T.border); ctx.stroke();
      ctx.strokeStyle = cssColor(T.border); ctx.textAlign = "center";
      // 上箭头
      ctx.beginPath(); ctx.moveTo(x + w - 22, y + h * 0.3); ctx.lineTo(x + w - 10, y + h * 0.55); ctx.lineTo(x + w - 28, y + h * 0.55); ctx.closePath();
      ctx.fillStyle = cssColor(T.fg); ctx.fill();
      // 下箭头
      ctx.beginPath(); ctx.moveTo(x + w - 22, y + h * 0.7); ctx.lineTo(x + w - 10, y + h * 0.45); ctx.lineTo(x + w - 28, y + h * 0.45); ctx.closePath();
      ctx.fillStyle = cssColor(T.fg); ctx.fill();
      ctx.fillStyle = cssColor(T.text); ctx.font = "12px sans-serif";
      ctx.textAlign = "left"; ctx.textBaseline = "middle";
      ctx.fillText(String(c.value != null ? c.value : 0), x + 8, y + h / 2 + 1);
      break;
    }
    case "radio": {
      const opts = String(c.options || "").split(";").filter(Boolean);
      const itemH = Number(c.item_h) || 20;
      const sel = Number(c.sel) || 0;
      ctx.font = "12px sans-serif"; ctx.textBaseline = "middle"; ctx.textAlign = "left";
      opts.forEach((o, i) => {
        const yy = y + itemH * i;
        ctx.beginPath(); ctx.arc(x + 8, yy + itemH / 2, 6, 0, Math.PI * 2);
        ctx.fillStyle = "#ffffff"; ctx.fill();
        ctx.strokeStyle = cssColor(T.border); ctx.stroke();
        if (i === sel) { ctx.beginPath(); ctx.arc(x + 8, yy + itemH / 2, 3, 0, Math.PI * 2); ctx.fillStyle = cssColor(T.highlight); ctx.fill(); }
        ctx.fillStyle = cssColor(T.text); ctx.fillText(o, x + 18, yy + itemH / 2);
      });
      break;
    }
    case "round_rect": {
      roundRect(x, y, w, h, Number(c.radius) != null ? Number(c.radius) : R);
      ctx.strokeStyle = cssColor(c.color || T.border);
      ctx.lineWidth = 2; ctx.stroke(); ctx.lineWidth = 1;
      break;
    }
    case "fill_rect": {
      ctx.fillStyle = cssColor(c.color || 0xCCEEFF);
      ctx.fillRect(x, y, w, h);
      break;
    }
    case "circle": {
      ctx.beginPath(); ctx.arc(x, y, w / 2, 0, Math.PI * 2);
      ctx.strokeStyle = cssColor(c.color || 0x888888); ctx.lineWidth = 2; ctx.stroke(); ctx.lineWidth = 1;
      break;
    }
    case "fill_circle": {
      ctx.beginPath(); ctx.arc(x, y, w / 2, 0, Math.PI * 2);
      ctx.fillStyle = cssColor(c.color || 0x44AA44); ctx.fill();
      break;
    }
    case "arc": {
      const start = (Number(c.start) || 0) * Math.PI / 180;
      const sweep = (Number(c.sweep) || 270) * Math.PI / 180;
      ctx.beginPath(); ctx.arc(x, y, w / 2, start, start + sweep);
      ctx.strokeStyle = cssColor(c.color || 0x666666); ctx.lineWidth = 2; ctx.stroke(); ctx.lineWidth = 1;
      break;
    }
    case "progress": {
      roundRect(x, y, w, h, Math.min(R, h / 2));
      ctx.fillStyle = cssColor(T.border); ctx.fill();
      const ratio = Math.max(0, Math.min(1, (Number(c.value) || 0) / 100));
      if (ratio > 0) {
        roundRect(x, y, w * ratio, h, Math.min(R, h / 2));
        ctx.fillStyle = cssColor(T.highlight); ctx.fill();
      }
      break;
    }
    case "textbox": {
      roundRect(x, y, w, h, R);
      ctx.fillStyle = "#ffffff"; ctx.fill();
      ctx.strokeStyle = cssColor(T.border); ctx.stroke();
      ctx.fillStyle = cssColor(T.text); ctx.font = "13px sans-serif";
      ctx.textAlign = "left"; ctx.textBaseline = "middle";
      ctx.fillText(String(c.text || ""), x + 8, y + h / 2 + 1);
      break;
    }
  }
}

function draw() {
  ctx.clearRect(0, 0, W, H);
  ctx.fillStyle = cssColor(theme.bg);
  ctx.fillRect(0, 0, W, H);
  for (const c of controls) { try { drawControl(c); } catch (e) { /* 单个控件绘制失败不阻断整体 */ } }
}

/* ---------- 覆盖层（拖拽 / 选中 / 缩放） ---------- */

function buildOverlay() {
  overlay.innerHTML = "";
  // 空白区域点击 → 取消选中
  const bg = document.createElement("div");
  bg.className = "overlay-bg";
  bg.addEventListener("pointerdown", (e) => { if (e.target === bg) { select(-1); } });
  overlay.appendChild(bg);

  controls.forEach((ctrl, i) => {
    const box = controlBox(ctrl);
    const d = document.createElement("div");
    d.className = "ctrl-overlay" + (i === selectedIndex ? " selected" : "");
    d.style.left = box.x + "px";
    d.style.top = box.y + "px";
    d.style.width = box.w + "px";
    d.style.height = box.h + "px";
    if (RESIZABLE.has(ctrl.type)) {
      const handle = document.createElement("div");
      handle.className = "resize-handle";
      handle.addEventListener("pointerdown", (e) => { e.stopPropagation(); startResize(e, i); });
      d.appendChild(handle);
    }
    d.addEventListener("pointerdown", (e) => { e.preventDefault(); e.stopPropagation(); select(i); startDrag(e, i); });
    overlay.appendChild(d);
  });
}

function refresh() {
  draw();
  buildOverlay();
}

function select(i) {
  selectedIndex = i;
  buildOverlay();
  renderProps();
  statusEl.textContent = i >= 0 ? "已选中控件：" + (controls[i].name || controls[i].type) : "就绪";
}

/* ---------- 拖拽 / 缩放 ---------- */

function addDragListeners(move, up) {
  const onMove = (e) => move(e);
  const onUp = (e) => { window.removeEventListener("pointermove", onMove); window.removeEventListener("pointerup", onUp); up(); };
  window.addEventListener("pointermove", onMove);
  window.addEventListener("pointerup", onUp);
}

function startDrag(e, i) {
  const c = controls[i];
  const box = controlBox(c);
  const startX = e.clientX, startY = e.clientY;
  const isCircle = isCircleType(c.type);
  const ox = Number(isCircle ? c.cx : c.x) || 0;
  const oy = Number(isCircle ? c.cy : c.y) || 0;
  addDragListeners((ev) => {
    let nx = ox + (ev.clientX - startX);
    let ny = oy + (ev.clientY - startY);
    if (isCircle) { nx = Math.max(0, Math.min(W, nx)); ny = Math.max(0, Math.min(H, ny)); c.cx = nx; c.cy = ny; }
    else { nx = Math.max(0, Math.min(W - 8, nx)); ny = Math.max(0, Math.min(H - 8, ny)); c.x = nx; c.y = ny; }
    refresh();
  }, () => {});
}

function startResize(e, i) {
  const c = controls[i];
  const ow = Number(c.w) || 120, oh = Number(c.h) || 36;
  const startX = e.clientX, startY = e.clientY;
  addDragListeners((ev) => {
    c.w = Math.max(8, ow + (ev.clientX - startX));
    c.h = Math.max(8, oh + (ev.clientY - startY));
    refresh();
  }, () => {});
}

/* ---------- 属性面板 ---------- */

function renderProps() {
  const panel = document.getElementById("propsPanel");
  if (selectedIndex < 0 || selectedIndex >= controls.length) {
    panel.innerHTML = '<p class="muted">未选中控件。<br>从左侧控件库添加，或在画布中点击一个控件进行编辑。</p>';
    return;
  }
  const c = controls[selectedIndex];
  const schema = PROPS[c.type] || [];
  const rows = [];
  rows.push('<div class="prop-item-head"><strong>' + (c.name || c.type) + "</strong></div>");
  for (const p of schema) {
    const v = c[p.key];
    rows.push(propRow(p, v, (newVal, key, el) => onPropChange(key, newVal, el)));
  }
  rows.push('<button class="btn-del" id="btnDelete">删除控件</button>');
  panel.innerHTML = rows.join("");

  document.getElementById("btnDelete").addEventListener("click", () => { deleteControl(); });
}

function propRow(p, value, onChange) {
  let input;
  if (p.kind === "color") {
    input = '<div class="color-line"><input type="color" data-key="' + p.key + '" value="' + cssColor(Number(value) || 0) + '"><span>' + p.label + "</span></div>";
    return '<div class="prop-row">' + input + "</div>";
  }
  switch (p.kind) {
    case "number":
      input = '<input type="number" data-key="' + p.key + '" value="' + (value != null ? value : "") + '">';
      break;
    case "bool":
      input = '<input type="checkbox" data-key="' + p.key + '"' + (value ? " checked" : "") + '>';
      break;
    default:
      input = '<input type="text" data-key="' + p.key + '" value="' + escAttr(value) + '">';
  }
  return '<div class="prop-row"><label>' + p.label + "</label>" + input + "</div>";
}

function escAttr(s) {
  return String(s == null ? "" : s).replace(/&/g, "&amp;").replace(/"/g, "&quot;").replace(/</g, "&lt;");
}

function onPropChange(key, newVal, el) {
  if (selectedIndex < 0) return;
  const c = controls[selectedIndex];
  const row = el.closest(".prop-row") || el.closest(".prop-item-head");
  const schema = PROPS[c.type] || [];
  const def = schema.find((p) => p.key === key);
  const kind = def ? def.kind : "text";
  if (kind === "number") c[key] = Number(newVal) || 0;
  else if (kind === "bool") c[key] = !!newVal;
  else c[key] = newVal;

  refresh();
  statusEl.textContent = "已更新属性 " + key;
}

// 事件委托：属性面板变更
document.getElementById("propsPanel").addEventListener("input", (ev) => {
  const t = ev.target;
  if (!t.dataset.key) return;
  onPropChange(t.dataset.key, t.type === "number" ? t.value : t.value, t);
});
document.getElementById("propsPanel").addEventListener("change", (ev) => {
  const t = ev.target;
  if (t.type === "checkbox" && t.dataset.key) {
    onPropChange(t.dataset.key, t.checked, t);
  }
});

function deleteControl() {
  if (selectedIndex < 0) return;
  controls.splice(selectedIndex, 1);
  selectedIndex = -1;
  refresh();
  renderProps();
  statusEl.textContent = "已删除控件";
}

/* ---------- 控件库：添加默认实例 ---------- */

function baseFor(type) {
  const p = NAME_PREFIX[type] || "c";
  const n = p + (++nameCounter);
  switch (type) {
    case "label": return { type, name: n, x: 20, y: 10, text: "文字", color: 0x000000, size: "12" };
    case "button": return { type, name: n, x: 60, y: 60, w: 120, h: 36, text: "按钮" };
    case "slider": return { type, name: n, x: 60, y: 60, w: 160, value: 50, min: 0, max: 100 };
    case "switch": return { type, name: n, x: 60, y: 60, on: true };
    case "spin": return { type, name: n, x: 60, y: 60, value: 3, step: 1 };
    case "radio": return { type, name: n, x: 60, y: 60, item_h: 20, options: "选项一;选项二;选项三", sel: 0 };
    case "round_rect": return { type, name: n, x: 60, y: 60, w: 120, h: 60, radius: 8, color: 0xE0E0E0 };
    case "fill_rect": return { type, name: n, x: 60, y: 60, w: 120, h: 60, color: 0xCCEEFF };
    case "circle": return { type, name: n, cx: 150, cy: 110, r: 30, color: 0xFF8800 };
    case "fill_circle": return { type, name: n, cx: 150, cy: 110, r: 30, color: 0x44AA44 };
    case "arc": return { type, name: n, cx: 150, cy: 110, r: 24, start: 0, sweep: 270, color: 0x666666 };
    case "progress": return { type, name: n, x: 60, y: 60, w: 200, h: 18, value: 72 };
    case "textbox": return { type, name: n, x: 60, y: 60, w: 160, h: 30, text: "文本框" };
    default: return { type, name: n, x: 60, y: 60, w: 120, h: 36 };
  }
}

function addControl(type) {
  const c = baseFor(type);
  const box = controlBox(c);
  if (isCircleType(type)) { c.cx = Math.round(W / 2); c.cy = Math.round(H / 2); }
  else {
    c.x = Math.max(0, Math.round((W - box.w) / 2));
    c.y = Math.max(0, Math.round((H - box.h) / 2));
  }
  controls.push(c);
  select(controls.length - 1);
  statusEl.textContent = "已添加 " + type + " 控件";
}

function buildPalette() {
  const list = document.getElementById("paletteList");
  list.innerHTML = "";
  for (const item of PALETTE) {
    const b = document.createElement("button");
    b.className = "palette-item";
    b.innerHTML = '<span class="pico">' + item.icon + '</span><span>' + item.label + "</span>";
    b.addEventListener("click", () => addControl(item.type));
    list.appendChild(b);
  }
}

/* ---------- 画布尺寸 / 主题 / 圆角 ---------- */

function applyCanvasSize() {
  W = Math.max(16, parseInt(document.getElementById("canvasW").value, 10) || 480);
  H = Math.max(16, parseInt(document.getElementById("canvasH").value, 10) || 320);
  radius = Math.max(0, parseInt(document.getElementById("canvasRadius").value, 10) || 0);
  canvas.width = W; canvas.height = H;
  refresh();
}

function syncThemeInputs() {
  document.getElementById("themeBg").value = cssColor(theme.bg);
  document.getElementById("themeBorder").value = cssColor(theme.border);
  document.getElementById("themeHl").value = cssColor(theme.highlight);
  document.getElementById("themeFg").value = cssColor(theme.fg);
  document.getElementById("themeText").value = cssColor(theme.text);
}

function readThemeFromInputs() {
  theme.bg = parseColor(document.getElementById("themeBg").value);
  theme.border = parseColor(document.getElementById("themeBorder").value);
  theme.highlight = parseColor(document.getElementById("themeHl").value);
  theme.fg = parseColor(document.getElementById("themeFg").value);
  theme.text = parseColor(document.getElementById("themeText").value);
}

// 工具栏尺寸/圆角变更
document.getElementById("canvasW").addEventListener("change", applyCanvasSize);
document.getElementById("canvasH").addEventListener("change", applyCanvasSize);
document.getElementById("canvasRadius").addEventListener("change", applyCanvasSize);
// 主题色变更
for (const id of ["themeBg", "themeBorder", "themeHl", "themeFg", "themeText"]) {
  document.getElementById(id).addEventListener("input", () => { readThemeFromInputs(); refresh(); });
}

/* ---------- 导出 ---------- */

function buildDesignPayload() {
  applyCanvasSize();
  readThemeFromInputs();
  return {
    name: "我的界面",
    width: W,
    height: H,
    theme: { bg: theme.bg, border: theme.border, highlight: theme.highlight, fg: theme.fg, text: theme.text },
    radius: radius,
    controls: controls.map((c) => Object.assign({}, c)),
  };
}

function showExportModal(vus) {
  const modal = document.getElementById("exportModal");
  const text = document.getElementById("exportText");
  document.getElementById("modalCount").textContent = controls.length;
  document.getElementById("exportError").textContent = "";
  text.value = vus;
  // 下载链接
  const blob = new Blob([vus], { type: "text/plain;charset=utf-8" });
  const link = document.getElementById("downloadLink");
  link.href = URL.createObjectURL(blob);
  link.download = "design.vus";
  modal.classList.remove("hidden");
}

document.getElementById("btnExport").addEventListener("click", () => {
  const payload = buildDesignPayload();
  statusEl.textContent = "正在导出…";
  fetch("/export", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(payload),
  })
    .then((r) => r.json())
    .then((res) => {
      if (res.ok) { showExportModal(res.vus); statusEl.textContent = "导出成功"; }
      else {
        statusEl.textContent = "导出失败";
        const modal = document.getElementById("exportModal");
        modal.classList.remove("hidden");
        document.getElementById("exportText").value = "";
        document.getElementById("exportError").textContent = res.error || "未知错误";
      }
    })
    .catch((err) => {
      statusEl.textContent = "导出失败：无法连接后端";
      alert("无法连接后端，请确认已运行 python3 server.py");
    });
});

document.getElementById("modalClose").addEventListener("click", () => {
  document.getElementById("exportModal").classList.add("hidden");
});

/* ---------- 初始化 ---------- */

applyCanvasSize();
syncThemeInputs();
buildPalette();
buildOverlay();
renderProps();