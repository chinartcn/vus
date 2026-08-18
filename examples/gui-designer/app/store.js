/* ================================================================
 * store.js —— VUS IDE 共享状态 + API 客户端 + 设计<->代码同步
 * ================================================================ */

window.VUS = window.VUS || {};
var C = window.VUS;

var DEFAULT_THEME = { bg: 0xFFFFFF, border: 0x888888, highlight: 0x0055AA, fg: 0x333333, text: 0x000000 };

C.store = (window.Vue ? Vue.reactive({
  route: 'designer',                    // designer | editor | search | settings
  backend: { port: 8000 },
  token: localStorage.getItem('vusToken') || '',
  meiliUrl: localStorage.getItem('vusMeili') || '',
  themeName: localStorage.getItem('vusTheme') || 'light',
  flipSync: true,                        // 设计改动是否自动回写代码（跟随开关）

  // ---- 设计器 ----
  design: {
    name: '我的界面',
    width: 480,
    height: 320,
    radius: 8,
    theme: Object.assign({}, DEFAULT_THEME),
    controls: [],
  },
  selectedIndex: -1,
  isPaletteOpen: false,

  // ---- 代码 ----
  code: '',

  // ---- 运行终端 ----
  terminal: [],
  running: false,

  // ---- 文档 ----
  searchEngine: 'local',
  searchResults: [],

  // ---- 代码编辑器状态 ----
  codeDirty: false,             // 代码是否有未写盘的改动（页签显示实心圆点）
  cursor: { y: 1, x: 1 },       // 编辑器光标 行:列（状态栏显示）

  // ---- 命令面板 (Ctrl+P) ----
  palette: { open: false, query: '' },

  // ---- 插件（扩展）管理 ----
  plugins: (function () {
    try {
      var raw = localStorage.getItem('vusPlugins');
      if (raw) return JSON.parse(raw);
    } catch (e) { /* 忽略 */ }
    return [
      { id: 'meili',       name: 'Meilisearch 文档检索', desc: '接入外部 Meilisearch 服务，自动回退本地索引。', builtin: true,  active: false },
      { id: 'fsbrowser',   name: '工程文件浏览',           desc: '在设置页浏览/编辑/保存工程目录下的 .vus 文件。', builtin: true,  active: true  },
      { id: 'terminal',    name: '运行终端（VUS / Shell）', desc: '流式执行 VUS 脚本或 Shell 命令，需后端 Token。',  builtin: true,  active: true  },
      { id: 'completion',  name: '代码补全 + 语法高亮',    desc: '内置函数名补全、括号匹配、VUS 高亮模式。',      builtin: true,  active: true  },
    ];
  })(),

  // ---- UI ----
  toast: { show: false, kind: '', text: '' },
}) : null);

C.apiBase = function () {
  // 同源部署：页面由后端托管，直接使用当前地址，与启动端口自动一致。
  if (window.location && window.location.origin &&
      window.location.protocol.indexOf('http') === 0) {
    // 去掉尾随斜杠，避免与相对路径（以 / 开头）拼成双斜杠 URL
    return String(window.location.origin).replace(/\/+$/, '');
  }
  // file:// 等场景回退到手动配置的端口
  return 'http://127.0.0.1:' + (C.store.backend.port || 8000);
};

C.authHeaders = function () {
  return C.store.token ? { 'X-VUS-Token': C.store.token } : {};
};

C.toast = function (kind, text) {
  C.store.toast = { show: true, kind: kind, text: text };
  setTimeout(function () {
    if (C.store.toast.show) C.store.toast = { show: false, kind: '', text: '' };
  }, 3200);
};

C.http = async function (path, opt) {
  opt = opt || {};
  opt.headers = (opt.headers || {});
  var res;
  try {
    res = await fetch(C.apiBase() + path, opt);
  } catch (err) {
    return { status: 0, data: { ok: false, error: '无法连接后端（请确认已运行 python3 server.py）' } };
  }
  var ct = res.headers.get('content-type') || '';
  var data = null;
  if (ct.indexOf('application/json') >= 0) {
    data = await res.json().catch(function () { return { ok: false, error: '返回数据解析失败' }; });
  }
  return { status: res.status, data: data };
};

/* ================= 颜色 / 几何 工具 ================= */
C.cssColor = function (v) {
  var n = Number(v) || 0;
  return '#' + n.toString(16).padStart(6, '0');
};
C.parseColor = function (hex) { return parseInt(hex.slice(1), 16) || 0; };
C.isCircleType = function (t) { return t === 'circle' || t === 'fill_circle' || t === 'arc'; };
C.controlBox = function (c) {
  if (C.isCircleType(c.type)) {
    // 圆/弧/实心圆：返回外接矩形（左上角），以便命中/拖拽/属性框对齐。
    var r = Number(c.r) || 20;
    var rcx = Number(c.cx) || 0, rcy = Number(c.cy) || 0;
    return { x: rcx - r, y: rcy - r, w: r * 2, h: r * 2,
             cx: rcx, cy: rcy, r: r };
  }
  var w, h;
  switch (c.type) {
    case 'switch': w = Number(c.w) || 48; h = Number(c.h) || 26; break;
    case 'spin': w = Number(c.w) || 52; h = Number(c.h) || 28; break;
    case 'slider': w = Number(c.w) || 160; h = Number(c.h) || 22; break;
    case 'radio': {
      var opts = String(c.options || '').split(';').filter(Boolean);
      h = (Number(c.h) || Number(c.item_h) || 20) * Math.max(1, opts.length);
      w = Number(c.w) || 170;
      break;
    }
    case 'label': w = Number(c.w) || 90; h = Number(c.h) || 24; break;
    default: w = Number(c.w) || 120; h = Number(c.h) || 36; break;
  }
  return { x: Number(c.x) || 0, y: Number(c.y) || 0, w: w, h: h };
};

/* ================= 设计 → 代码（生成） ================= */
C.buildDesign = function () {
  var d = C.store.design;
  return {
    name: d.name,
    width: d.width,
    height: d.height,
    theme: Object.assign({}, d.theme),
    radius: d.radius || 0,
    controls: d.controls.map(function (c) { return Object.assign({}, c); }),
  };
};

var pendingSync = 0;
C.syncDesignToCode = function () {
  if (!C.store.flipSync) return;
  clearTimeout(pendingSync);
  pendingSync = setTimeout(async function () {
    var r = await C.http('/export', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(C.buildDesign()),
    });
    if (r.data && r.data.ok) C.store.code = r.data.vus;
    else C.toast('err', (r.data && r.data.error) || '生成代码失败');
  }, 120);
};

/* ================= 代码 → 设计（反向解析） ================= */
// 函数名 → { 类型, 参数序 }
C.PARSE = {
  '图形_文字':   { type: 'label',       fields: ['x', 'y', 'text', 'color'] },
  '图形_按钮':   { type: 'button',      fields: ['name', 'x', 'y', 'w', 'h', 'text'] },
  '图形_滑块':   { type: 'slider',      fields: ['name', 'x', 'y', 'w', 'value', 'min', 'max'] },
  '图形_开关':   { type: 'switch',      fields: ['name', 'x', 'y', 'on'] },
  '图形_微调':   { type: 'spin',        fields: ['name', 'x', 'y', 'value', 'step'] },
  '图形_单选':   { type: 'radio',       fields: ['name', 'x', 'y', 'item_h', 'options', 'sel'] },
  '图形_圆角矩形': { type: 'round_rect', fields: ['x', 'y', 'w', 'h', 'radius', 'color'] },
  '图形_填充':   { type: 'fill_rect',   fields: ['x', 'y', 'w', 'h', 'color'] },
  '图形_画圆':   { type: 'circle',      fields: ['cx', 'cy', 'r', 'color'] },
  '图形_填充圆': { type: 'fill_circle', fields: ['cx', 'cy', 'r', 'color'] },
  '图形_圆弧':   { type: 'arc',         fields: ['cx', 'cy', 'r', 'start', 'sweep', 'color'] },
  '图形_进度':   { type: 'progress',    fields: ['name', 'x', 'y', 'w', 'h', 'value'] },
  '图形_文本框': { type: 'textbox',     fields: ['name', 'x', 'y', 'w', 'h', 'text'] },
};

function splitArgs(str) {
  var out = [], cur = '', inS = false, q = '';
  for (var i = 0; i < str.length; i++) {
    var ch = str[i];
    if (inS) {
      if (ch === '\\' ) { cur += ch + (str[++i] || ''); continue; }
      if (ch === q) { inS = false; }
      cur += ch;
      continue;
    }
    if (ch === '"' || ch === "'") { inS = true; q = ch; cur += ch; continue; }
    if (ch === ',') { out.push(cur.trim()); cur = ''; continue; }
    cur += ch;
  }
  if (cur.trim() !== '') out.push(cur.trim());
  return out;
}

function convertArg(s) {
  if (s === '') return '';
  if ((s[0] === '"' && s[s.length - 1] === '"') || (s[0] === "'" && s[s.length - 1] === "'")) {
    return s.slice(1, -1).replace(/\\"/g, '"').replace(/\\\\/g, '\\').replace(/\\n/g, '\n');
  }
  if (/^0x[0-9a-fA-F]+$/.test(s)) return parseInt(s, 16);
  if (s === 'true') return true;
  if (s === 'false') return false;
  if (/^-?\d+(\.\d+)?$/.test(s)) return Number(s);
  return s;
}

C.splitArgs = splitArgs;

C.parseCodeToDesign = function () {
  var code = C.store.code || '';
  var controls = [];
  var used = {};
  code.split('\n').forEach(function (line) {
    var m = line.replace(/#.*$/, '').trim().match(/^(\S+)\s*\((.*)\)\s*$/);
    if (!m) return;
    var f = m[1], dm = C.PARSE[f];
    if (!dm) return;
    var args = splitArgs(m[2]);
    if (args.length < dm.fields.length) return;
    var c = { type: dm.type };
    dm.fields.forEach(function (k, i) { c[k] = convertArg(args[i]); });
    if (!c.name) {
      var p = C.NAME_PREFIX[c.type] || 'c';
      var n = 1;
      while (used[p + n]) n++;
      c.name = p + n; used[p + n] = true;
    } else if (typeof c.name === 'string') {
      used[c.name] = true;
    }
    controls.push(c);
  });
  C.store.design.controls = controls;
  C.store.selectedIndex = controls.length ? 0 : -1;
  C.toast('ok', '已应用代码到设计（解析到 ' + controls.length + ' 个控件）');
};

/* ================= 文件读写（工程） ================= */
C.fsList = async function (rel) {
  var r = await C.http('/api/fs/list?path=' + encodeURIComponent(rel || '.'));
  return r.data || { items: [] };
};
C.fsRead = async function (rel) {
  var r = await C.http('/api/fs/read?path=' + encodeURIComponent(rel));
  return r.data || { ok: false };
};
C.fsWrite = async function (rel, text) {
  var r = await C.http('/api/fs/write', {
    method: 'POST',
    headers: Object.assign({ 'Content-Type': 'application/json' }, C.authHeaders()),
    body: JSON.stringify({ path: rel, text: text }),
  });
  return r.data || { ok: false };
};

/* ================= 插件（扩展）管理 ================= */
C.savePlugins = function () {
  try {
    localStorage.setItem('vusPlugins', JSON.stringify(C.store.plugins));
  } catch (e) { /* 忽略 */ }
};

C.togglePlugin = function (id) {
  var p = (C.store.plugins || []).find(function (x) { return x.id === id; });
  if (p) { p.active = !p.active; C.savePlugins(); }
};

C.addPlugin = function (o) {
  var plugins = C.store.plugins || [];
  var id = String(o && o.name || 'p').trim() || ('p' + (plugins.length + 1));
  // 生成唯一 id
  var base = id, n = 1;
  while (plugins.some(function (x) { return x.id === base; })) { base = id + '_' + (++n); }
  var item = {
    id: base,
    name: (o && o.name || '').trim() || ('扩展 ' + (plugins.filter(function (x) {
      return !x.builtin; }).length + 1)),
    desc: (o && o.desc || '').trim(),
    builtin: false,
    active: o && o.active !== false,
  };
  plugins.push(item);
  C.store.plugins = plugins;
  C.savePlugins();
  return item;
};

C.removePlugin = function (id) {
  var plugins = (C.store.plugins || []).filter(function (x) { return !(x.id === id && !x.builtin); });
  C.store.plugins = plugins;
  C.savePlugins();
};