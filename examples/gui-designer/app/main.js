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
    // 应用初始主题
    document.body.setAttribute('data-theme', this.store.themeName || 'light');
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
  },

  template: `
  <div class="ide-app">
    <!-- 顶部导航栏 -->
    <nav class="topnav">
      <div class="brand">VUS IDE</div>
      <button class="nav-btn" :class="{ active: isActive('designer') }" @click="setRoute('designer')">
        <span class="nav-ico">▦</span><span class="nav-label">设计器</span>
      </button>
      <button class="nav-btn" :class="{ active: isActive('editor') }" @click="setRoute('editor')">
        <span class="nav-ico">&lt;/&gt;</span><span class="nav-label">代码</span>
      </button>
      <button class="nav-btn" :class="{ active: isActive('search') }" @click="setRoute('search')">
        <span class="nav-ico">⌕</span><span class="nav-label">文档</span>
      </button>
      <button class="nav-btn" :class="{ active: isActive('settings') }" @click="setRoute('settings')">
        <span class="nav-ico">⚙</span><span class="nav-label">设置</span>
      </button>
    </nav>

    <!-- 路由视图 -->
    <DesignerView v-if="store.route === 'designer'"></DesignerView>
    <EditorView v-if="store.route === 'editor'"></EditorView>
    <SearchView v-if="store.route === 'search'"></SearchView>
    <SettingsView v-if="store.route === 'settings'"></SettingsView>

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