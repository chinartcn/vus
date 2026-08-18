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
    // 应用初始主题（与 index.html 首帧 inline 一致，避免闪烁）
    var t = this.store.themeName || 'light';
    document.documentElement.setAttribute('data-theme', t);
    if (document.body) document.body.setAttribute('data-theme', t);
  },

  methods: {
    // hash 路由：#/designer #/editor #/search #/settings
    syncRouteFromHash: function () {
      var h = (location.hash || '').replace(/^#\/?/, '').trim() || 'designer';
      var names = { designer: 1, editor: 1, search: 1, settings: 1 };
      if (names[h]) this.store.route = h;
      else this.store.route = 'designer';
    },
    setRoute: function (r) {
      this.store.route = r;
      location.hash = '#/' + r;
    },
    currentView: function () {
      return this.store.route;
    },
    isActive: function (r) { return this.store.route === r; },
    apiHost: function () {
      return C.apiBase().replace(/^https?:\/\//, '');
    },
    routeLabel: function () {
      return { designer: '设计器', editor: '代码', search: '文档', settings: '设置' }[this.store.route] || '设计器';
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
      </button>
      <button class="nav-btn" :class="{ active: isActive('search') }" @click="setRoute('search')" role="tab">
        <span class="nav-ico">⌕</span><span class="nav-label">文档</span>
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
    <SettingsView v-if="store.route === 'settings'"></SettingsView>

    <!-- 底部状态栏（全局） -->
    <footer class="status-bar">
      <span class="sb-item"><i class="sb-dot"></i>{{ routeLabel() }}</span>
      <span class="sb-item sb-sep"></span>
      <span class="sb-item sb-muted">后端</span>
      <span class="sb-item sb-mono">{{ apiHost() }}</span>
      <span class="sb-spacer"></span>
      <span class="sb-item">主题 {{ store.themeName }}</span>
    </footer>

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
  app.component('SettingsView', window.VUS.SettingsView);
  app.mount('#app');
});