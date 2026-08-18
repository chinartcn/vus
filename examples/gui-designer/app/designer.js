/* ================================================================
 * designer.js —— 设计器视图（Vue 组件）
 * 画布 WYSIWYG 渲染 + 拖拽/缩放/选中 + 动态属性面板 + 双向同步
 * ================================================================ */

window.VUS = window.VUS || {};
var C = window.VUS;

window.VUS.DesignerView = {
  name: "DesignerView",

  data: function () {
    return {
      C: C,                  // 模板中直接访问 C.xxx（工具函数）
      store: C.store,
      palette: C.PALETTE,
    };
  },

  mounted: function () {
    var self = this;
    this.ctx = this.$refs.preview.getContext("2d");
    // 画布命令式渲染，随设计深度变化重绘（初始化也触发一次）
    this.unwatch = this.$watch(
      function () { return self.store.design; },
      function () { self.redraw(); },
      { deep: true, immediate: true }
    );
    // 首次渲染后建立窗口级拖拽清理
    this._drag = null;
  },

  beforeUnmount: function () {
    if (this.unwatch) this.unwatch();
    this._teardownDrag();
  },

  methods: {
    /* ---- 几何 / 颜色 包装 ---- */
    ctrlBox: function (c) { return C.controlBox(c); },
    css: function (v) { return C.cssColor(v); },
    isCircle: function (t) { return C.isCircleType(t); },
    resizable: function (t) { return C.RESIZABLE.has(t); },
    schemaOf: function (type) { return C.PROPS[type] || []; },

    /* ---- 画布绘制（WYSIWYG，移植自旧 app.js） ---- */
    redraw: function () {
      var d = this.store.design;
      var ctx = this.ctx;
      var W = d.width, H = d.height, radius = d.radius || 0;
      var T = d.theme;
      if (!ctx || !d.width) return;
      ctx.clearRect(0, 0, W, H);
      ctx.fillStyle = C.cssColor(T.bg);
      ctx.fillRect(0, 0, W, H);
      var self = this;
      d.controls.forEach(function (c) {
        try { self.drawControl(c, W, H, radius, T); } catch (e) { /* 单个失败不阻断 */ }
      });
    },

    roundRect: function (x, y, w, h, r) {
      var ctx = this.ctx;
      r = Math.max(0, Math.min(r, w / 2, h / 2));
      ctx.beginPath();
      ctx.moveTo(x + r, y);
      ctx.arcTo(x + w, y, x + w, y + h, r);
      ctx.arcTo(x + w, y + h, x, y + h, r);
      ctx.arcTo(x, y + h, x, y, r);
      ctx.arcTo(x, y, x + w, y, r);
      ctx.closePath();
    },

    centerText: function (text, x, y, w, h, color, size) {
      var ctx = this.ctx;
      ctx.fillStyle = C.cssColor(color);
      ctx.font = (size || 12) + "px sans-serif";
      ctx.textAlign = "center";
      ctx.textBaseline = "middle";
      ctx.fillText(String(text), x + w / 2, y + h / 2 + 1);
    },
    leftText: function (text, x, y, color, size) {
      var ctx = this.ctx;
      ctx.fillStyle = C.cssColor(color);
      ctx.font = (size || 12) + "px sans-serif";
      ctx.textAlign = "left";
      ctx.textBaseline = "middle";
      ctx.fillText(String(text), x, y);
    },

    drawControl: function (c, W, H, radius, T) {
      var ctx = this.ctx;
      var t = c.type, R = radius;

      if (t === "label") {
        this.leftText(
          c.text,
          Number(c.x) || 0,
          Number(c.y) + ((Number(c.size) || 12) / 2),
          c.color != null ? c.color : T.text,
          Number(c.size) || 12
        );
        return;
      }
      var box = this.ctrlBox(c);
      var x = box.x, y = box.y, w = box.w, h = box.h;
      var self = this;

      switch (t) {
        case "button": {
          self.roundRect(x, y, w, h, R);
          ctx.fillStyle = C.cssColor(T.fg); ctx.fill();
          self.roundRect(x, y, w, h, R);
          ctx.strokeStyle = C.cssColor(T.border); ctx.stroke();
          self.centerText(c.text, x, y, w, h, T.text, 13);
          break;
        }
        case "slider": {
          var mx = Number(c.max) || 100, mn = Number(c.min) || 0, v = Number(c.value) || 0;
          var ratio = Math.max(0, Math.min(1, (v - mn) / (mx - mn || 1)));
          self.roundRect(x, y + h * 0.35, w, h * 0.3, h * 0.15);
          ctx.fillStyle = C.cssColor(T.border); ctx.fill();
          var kx = x + (w - 10) * ratio;
          ctx.beginPath(); ctx.arc(kx, y + h / 2, Math.max(5, h * 0.5), 0, Math.PI * 2);
          ctx.fillStyle = C.cssColor(T.highlight); ctx.fill();
          break;
        }
        case "switch": {
          self.roundRect(x, y, w, h, h / 2);
          ctx.fillStyle = c.on ? C.cssColor(T.highlight) : C.cssColor(T.border);
          ctx.fill();
          var swx = c.on ? x + w - h : x;
          ctx.beginPath(); ctx.arc(swx, y + h / 2, h / 2 - 2, 0, Math.PI * 2);
          ctx.fillStyle = "#ffffff"; ctx.fill();
          break;
        }
        case "spin": {
          self.roundRect(x, y, w, h, R);
          ctx.fillStyle = "#ffffff"; ctx.fill();
          ctx.strokeStyle = C.cssColor(T.border); ctx.stroke();
          ctx.strokeStyle = C.cssColor(T.border); ctx.textAlign = "center";
          ctx.beginPath(); ctx.moveTo(x + w - 22, y + h * 0.3);
          ctx.lineTo(x + w - 10, y + h * 0.55); ctx.lineTo(x + w - 28, y + h * 0.55); ctx.closePath();
          ctx.fillStyle = C.cssColor(T.fg); ctx.fill();
          ctx.beginPath(); ctx.moveTo(x + w - 22, y + h * 0.7);
          ctx.lineTo(x + w - 10, y + h * 0.45); ctx.lineTo(x + w - 28, y + h * 0.45); ctx.closePath();
          ctx.fillStyle = C.cssColor(T.fg); ctx.fill();
          ctx.fillStyle = C.cssColor(T.text); ctx.font = "12px sans-serif";
          ctx.textAlign = "left"; ctx.textBaseline = "middle";
          ctx.fillText(String(c.value != null ? c.value : 0), x + 8, y + h / 2 + 1);
          break;
        }
        case "radio": {
          var opts = String(c.options || "").split(";").filter(Boolean);
          var itemH = Number(c.item_h) || 20, sel = Number(c.sel) || 0;
          ctx.font = "12px sans-serif"; ctx.textBaseline = "middle"; ctx.textAlign = "left";
          opts.forEach(function (o, i) {
            var yy = y + itemH * i;
            ctx.beginPath(); ctx.arc(x + 8, yy + itemH / 2, 6, 0, Math.PI * 2);
            ctx.fillStyle = "#ffffff"; ctx.fill();
            ctx.strokeStyle = C.cssColor(T.border); ctx.stroke();
            if (i === sel) { ctx.beginPath(); ctx.arc(x + 8, yy + itemH / 2, 3, 0, Math.PI * 2); ctx.fillStyle = C.cssColor(T.highlight); ctx.fill(); }
            ctx.fillStyle = C.cssColor(T.text); ctx.fillText(o, x + 18, yy + itemH / 2);
          });
          break;
        }
        case "round_rect": {
          self.roundRect(x, y, w, h, c.radius != null ? Number(c.radius) : R);
          ctx.strokeStyle = C.cssColor(c.color || T.border);
          ctx.lineWidth = 2; ctx.stroke(); ctx.lineWidth = 1;
          break;
        }
        case "fill_rect": {
          ctx.fillStyle = C.cssColor(c.color || 0xCCEEFF);
          ctx.fillRect(x, y, w, h);
          break;
        }
        case "circle": {
          ctx.beginPath(); ctx.arc(x, y, w / 2, 0, Math.PI * 2);
          ctx.strokeStyle = C.cssColor(c.color || 0x888888); ctx.lineWidth = 2; ctx.stroke(); ctx.lineWidth = 1;
          break;
        }
        case "fill_circle": {
          ctx.beginPath(); ctx.arc(x, y, w / 2, 0, Math.PI * 2);
          ctx.fillStyle = C.cssColor(c.color || 0x44AA44); ctx.fill();
          break;
        }
        case "arc": {
          var st = (Number(c.start) || 0) * Math.PI / 180;
          var sp = (Number(c.sweep) || 270) * Math.PI / 180;
          ctx.beginPath(); ctx.arc(x, y, w / 2, st, st + sp);
          ctx.strokeStyle = C.cssColor(c.color || 0x666666); ctx.lineWidth = 2; ctx.stroke(); ctx.lineWidth = 1;
          break;
        }
        case "progress": {
          self.roundRect(x, y, w, h, Math.min(R, h / 2));
          ctx.fillStyle = C.cssColor(T.border); ctx.fill();
          var pr = Math.max(0, Math.min(1, (Number(c.value) || 0) / 100));
          if (pr > 0) {
            self.roundRect(x, y, w * pr, h, Math.min(R, h / 2));
            ctx.fillStyle = C.cssColor(T.highlight); ctx.fill();
          }
          break;
        }
        case "textbox": {
          self.roundRect(x, y, w, h, R);
          ctx.fillStyle = "#ffffff"; ctx.fill();
          ctx.strokeStyle = C.cssColor(T.border); ctx.stroke();
          ctx.fillStyle = C.cssColor(T.text); ctx.font = "13px sans-serif";
          ctx.textAlign = "left"; ctx.textBaseline = "middle";
          ctx.fillText(String(c.text || ""), x + 8, y + h / 2 + 1);
          break;
        }
      }
    },

    /* ---- 控件增删 ---- */
    addControl: function (type) {
      var d = this.store.design;
      var c = C.baseFor(type);
      var box = this.ctrlBox(c);
      if (this.isCircle(type)) { c.cx = Math.round(d.width / 2); c.cy = Math.round(d.height / 2); }
      else {
        c.x = Math.max(0, Math.round((d.width - box.w) / 2));
        c.y = Math.max(0, Math.round((d.height - box.h) / 2));
      }
      d.controls.push(c);
      this.store.selectedIndex = d.controls.length - 1;
      this.store.isPaletteOpen = false; // 移动端添加后收起控件库
      C.toast('ok', '已添加控件 ' + c.name);
    },

    removeControl: function () {
      var i = this.store.selectedIndex;
      if (i < 0 || i >= this.store.design.controls.length) return;
      this.store.design.controls.splice(i, 1);
      this.store.selectedIndex = -1;
      C.toast('ok', '已删除控件');
    },

    clearSelect: function () { this.store.selectedIndex = -1; },

    /* ---- 拖拽 / 缩放 ---- */
    startDrag: function (i, ev) {
      var self = this;
      var d = this.store.design;
      var c = d.controls[i];
      this.store.selectedIndex = i;
      var circle = this.isCircle(c.type);
      var ox = Number(circle ? c.cx : c.x) || 0;
      var oy = Number(circle ? c.cy : c.y) || 0;
      var sx = ev.clientX, sy = ev.clientY;
      var ctx2 = this;
      this._setDrag(function (ev2, up) {
        var nx = ox + (ev2.clientX - sx);
        var ny = oy + (ev2.clientY - sy);
        if (circle) {
          c.cx = Math.max(0, Math.min(d.width, nx));
          c.cy = Math.max(0, Math.min(d.height, ny));
        } else {
          c.x = Math.max(0, Math.min(d.width - 8, nx));
          c.y = Math.max(0, Math.min(d.height - 8, ny));
        }
        ctx2.redraw();
      });
    },

    startResize: function (i, ev) {
      var self = this;
      var c = this.store.design.controls[i];
      this.store.selectedIndex = i;
      var ow = Number(c.w) || 120, oh = Number(c.h) || 36;
      var sx = ev.clientX, sy = ev.clientY;
      var ctx2 = this;
      this._setDrag(function (ev2) {
        c.w = Math.max(8, ow + (ev2.clientX - sx));
        c.h = Math.max(8, oh + (ev2.clientY - sy));
        ctx2.redraw();
      });
    },

    _setDrag: function (move) {
      this._teardownDrag();
      var self = this;
      this._drag = {
        move: function (ev) { ev.preventDefault(); move(ev); },
        up: function () { self._teardownDrag(); },
      };
      window.addEventListener("pointermove", this._drag.move);
      window.addEventListener("pointerup", this._drag.up);
    },

    _teardownDrag: function () {
      if (this._drag) {
        window.removeEventListener("pointermove", this._drag.move);
        window.removeEventListener("pointerup", this._drag.up);
        this._drag = null;
      }
    },

    /* ---- 属性变更（经事件委托绑定到根面板） ---- */
    onPropInput: function (field, value) {
      var i = this.store.selectedIndex;
      if (i < 0 || i >= this.store.design.controls.length) return;
      var c = this.store.design.controls[i];
      var def = C.PROPS[c.type] ? C.PROPS[c.type].find(function (p) { return p.key === field; }) : null;
      var kind = def ? def.kind : "text";
      if (kind === "number") c[field] = Number(value) || 0;
      else if (kind === "bool") c[field] = !!value;
      else c[field] = value;
      this.redraw();
    },

    /* ---- 画布尺寸 / 主题 / 圆角 ---- */
    onCanvasSize: function () {
      var d = this.store.design;
      d.width = Math.max(16, Math.min(4000, Number(d.width) || 480));
      d.height = Math.max(16, Math.min(4000, Number(d.height) || 320));
      d.radius = Math.max(0, Number(d.radius) || 0);
      this.redraw();
    },

    /* ---- 导出（生成代码并进入代码页） ---- */
    exportDesign: async function () {
      var self = this;
      this.store.exporting = true;
      var r = await C.http('/export', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(C.buildDesign()),
      });
      this.store.exporting = false;
      if (r.data && r.data.ok) {
        C.store.code = r.data.vus;
        C.store.route = 'editor';
        C.toast('ok', '已生成代码，切换到代码页');
      } else {
        C.toast('err', (r.data && r.data.error) || '导出失败');
      }
    },

    /* 代码 → 设计：把现有代码解析回设计（供"从代码同步"用） */
    importFromCode: function () {
      C.parseCodeToDesign();
    },
  },

  template: `
  <div class="layout ide-designer">
    <!-- 左侧控件库 -->
    <aside class="palette" :class="{ collapsed: !store.isPaletteOpen }">
      <div class="palette-head">
        <h3>控件库<small>点击添加</small></h3>
        <button class="palette-toggle" @click="store.isPaletteOpen = !store.isPaletteOpen">
          {{ store.isPaletteOpen ? '收起' : '展开' }}
        </button>
      </div>
      <div class="palette-list" v-show="store.isPaletteOpen">
        <div class="palette-item" v-for="item in palette" :key="item.type" @click="addControl(item.type)">
          <span class="pico">{{ item.icon }}</span><span>{{ item.label }}</span>
        </div>
      </div>
    </aside>

    <!-- 中间画布 -->
    <div class="canvas-pane">
      <div class="canvas-tools">
        <label class="tool-label">名称
          <input type="text" v-model="store.design.name" style="width:90px">
        </label>
        <label class="tool-label">宽
          <input type="number" v-model.number="store.design.width" @change="onCanvasSize">
        </label>
        <label class="tool-label">高
          <input type="number" v-model.number="store.design.height" @change="onCanvasSize">
        </label>
        <label class="tool-label">圆角
          <input type="number" v-model.number="store.design.radius" @change="onCanvasSize">
        </label>
        <span class="sep">|</span>
        <span class="tool-label theme-group">主题色
          <span class="swatch"><input type="color" title="背景" :value="css(store.design.theme.bg)" @input="store.design.theme.bg = C.parseColor($event.target.value)"></span>
          <span class="swatch"><input type="color" title="边框" :value="css(store.design.theme.border)" @input="store.design.theme.border = C.parseColor($event.target.value)"></span>
          <span class="swatch"><input type="color" title="高亮" :value="css(store.design.theme.highlight)" @input="store.design.theme.highlight = C.parseColor($event.target.value)"></span>
          <span class="swatch"><input type="color" title="前景" :value="css(store.design.theme.fg)" @input="store.design.theme.fg = C.parseColor($event.target.value)"></span>
          <span class="swatch"><input type="color" title="文字" :value="css(store.design.theme.text)" @input="store.design.theme.text = C.parseColor($event.target.value)"></span>
        </span>
        <span class="sep">|</span>
        <button class="primary" @click="exportDesign" :disabled="store.exporting">
          {{ store.exporting ? '生成中…' : '生成代码' }}
        </button>
        <button class="ghost" @click="importFromCode">从代码同步</button>
      </div>

      <div class="canvas-scroll">
        <div id="canvasBox" :style="{ width: store.design.width + 'px', height: store.design.height + 'px' }">
          <canvas ref="preview" :width="store.design.width" :height="store.design.height"></canvas>
          <div id="overlay">
            <div class="overlay-bg" @pointerdown="clearSelect"></div>
            <div class="ctrl-overlay"
                 v-for="(c, i) in store.design.controls"
                 :key="'c'+i+'_'+(c.name||i)"
                 :class="{ selected: i === store.selectedIndex }"
                 :style="{ left: ctrlBox(c).x + 'px', top: ctrlBox(c).y + 'px', width: ctrlBox(c).w + 'px', height: ctrlBox(c).h + 'px' }"
                 @pointerdown.prevent.stop="startDrag(i, $event)">
              <div class="resize-handle" v-if="resizable(c.type)"
                   @pointerdown.prevent.stop="startResize(i, $event)"></div>
            </div>
          </div>
        </div>
      </div>
      <div class="status">
        <template v-if="store.selectedIndex >= 0">
          已选中控件：{{ store.design.controls[store.selectedIndex].name || store.design.controls[store.selectedIndex].type }}
        </template>
        <template v-else>就绪 — 从左侧控件库添加控件，或从代码页解析</template>
        <span class="sep">|</span> 控件数：{{ store.design.controls.length }}
      </div>
    </div>

    <!-- 右侧属性面板 -->
    <aside class="props" v-if="store.selectedIndex >= 0 && store.design.controls[store.selectedIndex]">
      <h3>属性</h3>
      <div class="prop-item-head">
        <strong>{{ store.design.controls[store.selectedIndex].name || '未命名' }}</strong>
        <code>{{ store.design.controls[store.selectedIndex].type }}</code>
      </div>
      <div class="prop-row" v-for="p in schemaOf(store.design.controls[store.selectedIndex].type)" :key="p.key">
        <label>{{ p.label }}</label>
        <input v-if="p.kind === 'number'"
               type="number"
               :value="store.design.controls[store.selectedIndex][p.key] != null ? store.design.controls[store.selectedIndex][p.key] : ''"
               @input="onPropInput(p.key, $event.target.value)">
        <input v-else-if="p.kind === 'bool'"
               type="checkbox"
               :checked="!!store.design.controls[store.selectedIndex][p.key]"
               @change="onPropInput(p.key, $event.target.checked)">
        <input v-else-if="p.kind === 'color'"
               type="color"
               :value="css(store.design.controls[store.selectedIndex][p.key] !== undefined ? store.design.controls[store.selectedIndex][p.key] : 0)"
               @input="onPropInput(p.key, C.parseColor($event.target.value))">
        <input v-else type="text"
               :value="store.design.controls[store.selectedIndex][p.key] || ''"
               @input="onPropInput(p.key, $event.target.value)">
      </div>
      <button class="btn-del" @click="removeControl">删除控件</button>
    </aside>
    <aside class="props" v-else>
      <h3>属性</h3>
      <p class="muted">未选中控件。<br>从左侧控件库添加，或在画布中点击一个控件进行编辑。</p>
    </aside>
  </div>
  `,
};