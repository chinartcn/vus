/* ================================================================
 * plugins.js —— 插件（扩展）管理视图（Vue 组件）
 * 内置能力展示 + 用户自定义本地插件条目的增删与启停
 * 自定义条目仅持久化到 localStorage，不引入外部执行引擎。
 * ================================================================ */

window.VUS = window.VUS || {};
var C = window.VUS;

window.VUS.PluginsView = {
  name: "PluginsView",

  data: function () {
    return {
      C: C,
      store: C.store,
      form: { name: '', desc: '' },
    };
  },

  methods: {
    toggle: function (p) { C.togglePlugin(p.id); },
    remove: function (p) {
      C.removePlugin(p.id);
    },
    addLocal: function () {
      if (!this.form.name.trim()) { C.toast('err', '请填写扩展名称'); return; }
      var item = C.addPlugin({ name: this.form.name, desc: this.form.desc });
      this.form = { name: '', desc: '' };
      C.toast('ok', '已添加扩展「' + item.name + '」');
    },
    builtins: function () { return C.store.plugins.filter(function (p) { return p.builtin; }); },
    locals: function () { return C.store.plugins.filter(function (p) { return !p.builtin; }); },
  },

  template: `
  <div class="plugins-layout">
    <div class="plugins-head">
      <h3>扩展（插件）</h3>
      <p class="muted">当前为纯本地管理：内置能力可开关，自定义条目用于登记你想要的 VUS 扩展并记录启用状态（持久化在浏览器）。</p>
    </div>

    <div class="card">
      <h4>内置能力</h4>
      <div class="plugin-item" v-for="p in builtins()" :key="p.id">
        <div class="plugin-main">
          <strong>{{ p.name }}</strong>
          <span class="muted">{{ p.desc }}</span>
        </div>
        <span class="tag tag-builtin">内置</span>
        <label class="switch-mini">
          <input type="checkbox" :checked="p.active" @change="toggle(p)">
          <span>{{ p.active ? '启用' : '停用' }}</span>
        </label>
      </div>
    </div>

    <div class="card">
      <h4>自定义扩展</h4>
      <div class="plugin-item" v-for="p in locals()" :key="p.id">
        <div class="plugin-main">
          <strong>{{ p.name }}</strong>
          <span class="muted">{{ p.desc || '—' }}</span>
        </div>
        <label class="switch-mini">
          <input type="checkbox" :checked="p.active" @change="toggle(p)">
          <span>{{ p.active ? '启用' : '停用' }}</span>
        </label>
        <button class="ghost danger" @click="remove(p)">移除</button>
      </div>
      <div v-if="!locals().length" class="muted empty-line">还没有自定义扩展，可在下方登记。</div>

      <div class="plugin-add">
        <input type="text" v-model="form.name" placeholder="扩展名称，如：串口监视器"
               @keyup.enter="addLocal">
        <input type="text" v-model="form.desc" placeholder="一句话说明（可选）" style="flex:1"
               @keyup.enter="addLocal">
        <button class="primary" @click="addLocal">添加</button>
      </div>
    </div>
  </div>
  `,
};