/* ================================================================
 * main.js —— VUS IDE 应用骨架
 * 创建 Vue 应用、hash 路由、顶部导航（移动端可折叠）、Toast
 * 组件：DesignerView / EditorView / SearchView / SettingsView
 * ================================================================ */

window.VUS = window.VUS || {};
var C = window.VUS;

// 根组件：顶部导航 + 路由视图 + Toast
var Root = {
  name: 'Root',

  data: function () {
    return { store: C.store };
  },

  mounted: function () {
    var self = this;
    this.syncRouteFromHash();
    window.addEventListener('hashchange', function () { self.syncRouteFromHash(); });
    // 命令面板快捷键：Ctrl/Cmd+P
    window.addEventListener('keydown', function (ev) {
      var mod = ev.ctrlKey || ev.metaKey;
      if (mod && ev.key.toLowerCase() === 'p') {
        if (ev.shiftKey) ev.preventDefault();
        ev.preventDefault();
        self.paletteToggle();
      } else if (ev.key === 'Escape' && self.store.palette.open) {
        self.paletteClose();
      }
    });
    // 应用初始主题（与 index.html 首帧 inline 一致，避免闪烁）
    var t = this.store.themeName || 'light';
    document.documentElement.setAttribute('data-theme', t);
    if (document.body) document.body.setAttribute('data-theme', t);
  },

  methods: {
    // hash 路由：#/designer #/editor #/search #/settings #/plugins
    syncRouteFromHash: function () {
      var h = (location.hash || '').replace(/^#\/?/, '').trim() || 'designer';
      var names = { designer: 1, editor: 1, search: 1, settings: 1, plugins: 1 };
      if (names[h]) this.store.route = h;
      else this.store.route = 'designer';
    },
    setRoute: function (r) {
      this.store.route = r;
      location.hash = '#/' + r;
      this.store.palette.open = false;
    },
    currentView: function () {
      return this.store.route;
    },
    isActive: function (r) { return this.store.route === r; },
    apiHost: function () {
      return C.apiBase().replace(/^https?:\/\//, '');
    },
    routeLabel: function () {
      return { designer: '设计器', editor: '代码', search: '文档', settings: '设置', plugins: '扩展' }[this.store.route] || '设计器';
    },

    /* ---- 命令面板 (Ctrl+P / Ctrl+Shift+P) ---- */
    paletteToggle: function () {
      var p = this.store.palette;
      p.open = !p.open;
      if (p.open) {
        p.query = '';
        var self = this;
        this.$nextTick(function () {
          var el = self.$refs.palInput;
          if (el) el.focus();
        });
      }
    },
    paletteClose: function () {
      this.store.palette.open = false;
      this.store.palette.query = '';
    },
    commands: function () {
      var self = this;
      return [
        { label: '打开… 设计器',        k: 'designer view',        run: function () { self.setRoute('designer'); } },
        { label: '打开… 代码编辑器',    k: 'editor code 代码',      run: function () { self.setRoute('editor'); } },
        { label: '打开… 文档检索',      k: 'search docs 文档',      run: function () { self.setRoute('search'); } },
        { label: '打开… 设置',          k: 'settings 设置',         run: function () { self.setRoute('settings'); } },
        { label: '打开… 扩展管理',      k: 'plugins 插件 扩展',     run: function () { self.setRoute('plugins'); } },
        { label: '清空设计画布',        k: 'clear canvas 清空',     run: function () {
          C.store.design.controls = []; C.store.selectedIndex = -1; C.toast('ok', '画布已清空');
        } },
        { label: '复制当前代码到剪贴板', k: 'copy vus 代码 复制',   run: function () {
          var code = C.store.code || '';
          (navigator.clipboard ? navigator.clipboard.writeText(code) : Promise.resolve())
            .then(function () { C.toast('ok', '代码已复制'); })
            .catch(function () { C.toast('err', '复制失败'); });
        } },
      ];
    },
    filteredCommands: function () {
      var q = (this.store.palette.query || '').trim().toLowerCase();
      return this.commands().filter(function (c) {
        if (!q) return true;
        return (c.label + ' ' + c.k).toLowerCase().indexOf(q) >= 0;
      });
    },
    runCommand: function (cmd) {
      cmd.run();
      this.paletteClose();
    },
  },

  template: `
  <div class="ide-app">
    <!-- 顶部页签导航（VS Code 式编辑器页签） -->
    <nav class="topnav" role="tablist">
      <div class="brand">VUS<span class="brand-dim">ide</span></div>
      <button class="nav-btn" :class="{ active: isActive('designer') }" @click="setRoute('designer')" role="tab">
        <span class="nav-ico">▦</span><span class="nav-label">设计器</span>
      </button>
      <button class="nav-btn" :class="{ active: isActive('editor') }" @click="setRoute('editor')" role="tab">
        <span class="nav-ico">&lt;/&gt;</span><span class="nav-label">代码</span>
        <span class="nav-dot" v-if="store.codeDirty" title="有未保存的代码改动"></span>
      </button>
      <button class="nav-btn" :class="{ active: isActive('search') }" @click="setRoute('search')" role="tab">
        <span class="nav-ico">⌕</span><span class="nav-label">文档</span>
      </button>
      <button class="nav-btn" :class="{ active: isActive('plugins') }" @click="setRoute('plugins')" role="tab">
        <span class="nav-ico">◇</span><span class="nav-label">扩展</span>
      </button>
      <button class="nav-btn" :class="{ active: isActive('settings') }" @click="setRoute('settings')" role="tab">
        <span class="nav-ico">⚙</span><span class="nav-label">设置</span>
      </button>
      <span class="topnav-spacer"></span>
      <span class="topnav-token" :class="{ on: store.token }">
        {{ store.token ? '· 已授权' : '· 未设置 Token' }}
      </span>
    </nav>

    <!-- 路由视图 -->
    <DesignerView v-if="store.route === 'designer'"></DesignerView>
    <EditorView v-if="store.route === 'editor'"></EditorView>
    <SearchView v-if="store.route === 'search'"></SearchView>
    <PluginsView v-if="store.route === 'plugins'"></PluginsView>
    <SettingsView v-if="store.route === 'settings'"></SettingsView>

    <!-- 底部状态栏（全局） -->
    <footer class="status-bar">
      <span class="sb-item"><i class="sb-dot"></i>{{ routeLabel() }}</span>
      <span class="sb-item sb-sep"></span>
      <span class="sb-item sb-lang">&lbrace;&nbsp;&rbrace;&nbsp;.vus</span>
      <span class="sb-item sb-sep"></span>
      <span class="sb-item sb-muted">后端</span>
      <span class="sb-item sb-mono">{{ apiHost() }}</span>
      <span class="sb-spacer"></span>
      <span class="sb-item" v-if="store.route === 'editor' && !store.codeDirty" title="代码已保存/无改动">● 已同步</span>
      <span class="sb-item sb-dirty" v-else-if="store.codeDirty" title="代码有未保存改动">● 未保存</span>
      <span class="sb-item sb-mono" v-if="store.route === 'editor'">Ln {{ store.cursor.y }}, Col {{ store.cursor.x }}</span>
      <span class="sb-sep" v-if="store.route === 'editor'"></span>
      <button class="sb-btn" @click="paletteToggle()" title="命令面板 (Ctrl+P)">⌥ 命令 ⌘P</button>
      <span class="sb-item">主题 {{ store.themeName }}</span>
    </footer>

    <!-- 命令面板 (Ctrl+P) -->
    <transition name="pal">
      <div v-if="store.palette.open" class="palette-mask" @click.self="paletteClose()">
        <div class="palette-box" @keydown.esc.stop="paletteClose()" @keydown.enter.prevent="runCommand(filteredCommands()[0])">
          <div class="palette-input-row">
            <span class="palette-prompt">›</span>
            <input ref="palInput" v-model="store.palette.query" placeholder="输入命令或视图名称…" @keyup.esc="paletteClose()">
          </div>
          <ul class="palette-list">
            <li v-for="c in filteredCommands()" :key="c.label" class="palette-item" @click="runCommand(c)" @mousedown.prevent>
              <span class="palette-item-label">{{ c.label }}</span>
            </li>
            <li v-if="!filteredCommands().length" class="palette-empty muted">没有匹配的命令</li>
          </ul>
        </div>
      </div>
    </transition>

    <!-- Toast 提示 -->
    <transition name="toast">
      <div v-if="store.toast.show" class="toast" :class="store.toast.kind">
        {{ store.toast.text }}
      </div>
    </transition>
  </div>
  `,
};

document.addEventListener('DOMContentLoaded', function () {
  var app = Vue.createApp(Root);
  app.component('DesignerView', window.VUS.DesignerView);
  app.component('EditorView', window.VUS.EditorView);
  app.component('SearchView', window.VUS.SearchView);
  app.component('PluginsView', window.VUS.PluginsView);
  app.component('SettingsView', window.VUS.SettingsView);
  app.mount('#app');
});