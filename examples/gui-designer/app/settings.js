/* ================================================================
 * settings.js —— 设置与工程文件视图（Vue 组件）
 * 后端地址/Token/Meilisearch/双向同步开关 + 工程文件浏览
 * ================================================================ */

window.VUS = window.VUS || {};
var C = window.VUS;

window.VUS.SettingsView = {
  name: "SettingsView",

  data: function () {
    return {
      C: C,                    // 模板中直接访问 C.xxx
      store: C.store,
      fs: { path: '.', items: [], loading: false, fileText: '', fileName: '' },
    };
  },

  mounted: function () {
    this.refreshFs();
  },

  methods: {
    saveBackend: function () {
      localStorage.setItem('vusToken', this.store.token);
      localStorage.setItem('vusMeili', this.store.meiliUrl);
      localStorage.setItem('vusTheme', this.store.themeName);
      C.toast('ok', '设置已保存');
    },
    applyTheme: function (name) {
      this.store.themeName = name;
      document.documentElement.setAttribute('data-theme', name);
      document.body.setAttribute('data-theme', name);
    },
    toggleSync: function () {
      // flipSync 决定是否在设计改动时自动回写代码
      C.toast(this.store.flipSync ? 'ok' : 'info',
        this.store.flipSync ? '已开启：设计改动自动回写代码' : '已暂停自动同步');
    },
    isDark: function () {
      return this.store.themeName === 'dark';
    },

    /* ---- 工程文件 ---- */
    refreshFs: async function () {
      this.fs.loading = true;
      var r = await C.fsList(this.fs.path);
      this.fs.loading = false;
      this.fs.items = r.items || [];
      this.fs.fileText = ''; this.fs.fileName = '';
    },
    async openDir(name, type) {
      var cur = this.fs.path === '.' ? '' : this.fs.path;
      this.fs.path = cur ? cur + '/' + name : name;
      await this.refreshFs();
    },
    async upDir() {
      var parts = this.fs.path.split('/');
      parts.pop();
      this.fs.path = parts.length ? parts.join('/') : '.';
      await this.refreshFs();
    },
    async openFile(item) {
      var p = this.fs.path === '.' ? item.name : this.fs.path + '/' + item.name;
      var r = await C.fsRead(p);
      if (r.ok) { this.fs.fileText = r.text; this.fs.fileName = p; }
      else C.toast('err', r.error || '读取失败');
    },
    async saveFile() {
      if (!this.fs.fileName) return;
      var r = await C.fsWrite(this.fs.fileName, this.fs.fileText);
      if (r.ok) C.toast('ok', '已保存 ' + this.fs.fileName);
      else C.toast('err', r.error || '保存失败');
    },
    loadIntoEditor() {
      if (!this.fs.fileName) return;
      this.store.code = this.fs.fileText;
      this.store.route = 'editor';
      C.toast('ok', '已载入编辑器');
    },
    fileName: function (item) { return item.name; },
    isDir: function (item) { return item.type === 'dir'; },
  },

  template: `
  <div class="settings-layout">
    <div class="settings-cols">
      <!-- 左侧：连接与偏好 -->
      <div class="card">
        <h3>后端连接</h3>
        <div class="form-row">
          <label>后端地址（端口）</label>
          <input type="number" v-model.number="store.backend.port" placeholder="8000">
        </div>
        <div class="form-row">
          <label>访问 Token</label>
          <input type="password" v-model="store.token" placeholder="启动 server.py 时打印的 Token">
          <small>运行 VUS / Shell、写入文件需要该 Token。</small>
        </div>
        <div class="form-row">
          <label>Meilisearch 服务地址（可选）</label>
          <input type="text" v-model="store.meiliUrl" placeholder="如 http://127.0.0.1:7700">
          <small>留空则仅使用本地文档索引。</small>
        </div>
        <button class="primary" @click="saveBackend">保存设置</button>
      </div>

      <div class="card">
        <h3>偏好</h3>
        <div class="form-row">
          <label>界面主题</label>
          <select :value="store.themeName" @change="applyTheme($event.target.value)">
            <option value="light">浅色</option>
            <option value="dark">深色</option>
          </select>
        </div>
        <div class="form-row checkrow">
          <label class="inline">设计→代码自动同步</label>
          <input type="checkbox" v-model="store.flipSync" @change="toggleSync">
          <small>关闭后，设计页改动不会自动覆盖代码。</small>
        </div>
      </div>

      <div class="card">
        <h3>关于</h3>
        <p class="muted">VUS 初期 IDE：HTML 排版设计器 + VUS 代码编辑/执行 + 文档检索，纯本地、可离线运行，后端为 Python 标准库。带副作用接口受 Token 门禁保护，仅绑定 127.0.0.1。</p>
      </div>
    </div>

    <!-- 右侧：工程文件 -->
    <div class="card fs-card">
      <div class="fs-head">
        <h3>工程文件</h3>
        <div class="fs-actions">
          <button v-if="fs.path !== '.'" @click="upDir">上级</button>
          <button @click="refreshFs" :disabled="fs.loading">刷新</button>
        </div>
      </div>
      <div class="fs-breadcrumb">路径：/{{ fs.path }}</div>
      <ul class="fs-list">
        <li v-for="item in fs.items" :key="item.path" @click="isDir(item) ? openDir(item.name, item.type) : openFile(item)">
          <span class="fs-icon">{{ isDir(item) ? '▸' : '·' }}</span>
          <span>{{ item.name }}</span>
        </li>
        <li v-if="!fs.loading && !fs.items.length" class="fs-empty">（空目录）</li>
      </ul>
      <div class="fs-file" v-if="fs.fileName">
        <div class="fs-file-head">
          <strong>{{ fs.fileName }}</strong>
          <span class="fs-actions">
            <button class="ghost" @click="loadIntoEditor">载入编辑器</button>
            <button @click="saveFile">保存</button>
          </span>
        </div>
        <textarea class="fs-textarea" v-model="fs.fileText" spellcheck="false"></textarea>
      </div>
    </div>
  </div>
  `,
};