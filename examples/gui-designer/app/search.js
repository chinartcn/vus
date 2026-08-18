/* ================================================================
 * search.js —— 文档检索视图（Vue 组件）
 * 默认本地兜底索引；可选联调外部 Meilisearch（失败自动回退本地）
 * ================================================================ */

window.VUS = window.VUS || {};
var C = window.VUS;

window.VUS.SearchView = {
  name: "SearchView",

  data: function () {
    return {
      C: C,                    // 模板中直接访问 C.xxx
      store: C.store,
      query: '',
      searching: false,
      results: [],
      engineLabel: 'local',
    };
  },

  methods: {
    doSearch: async function () {
      var q = (this.query || '').trim();
      this.searching = true;
      this.results = [];
      var url = C.apiBase() + '/api/search?q=' + encodeURIComponent(q) +
        '&engine=' + encodeURIComponent(this.store.searchEngine);
      if (this.store.meiliUrl) {
        url += '&meili_url=' + encodeURIComponent(this.store.meiliUrl);
      }
      var r = await C.http(url);
      this.searching = false;
      if (r.data && r.data.ok) {
        this.results = r.data.results || [];
        this.engineLabel = r.data.engine || 'local';
        if (this.engineLabel === 'meili') C.toast('ok', '已使用 Meilisearch 检索');
      } else {
        C.toast('err', (r.data && r.data.error) || '搜索请求失败');
      }
    },

    copyExample: function (ex) {
      if (!ex) return;
      (navigator.clipboard ? navigator.clipboard.writeText(ex) : Promise.resolve())
        .then(function () { C.toast('ok', '示例已复制'); })
        .catch(function () { C.toast('err', '复制失败'); });
    },
  },

  template: `
  <div class="search-layout">
    <div class="search-bar">
      <input v-model="query" placeholder="搜索 VUS 内置函数，如：图形_初始化、传感器_读、JSON_解析"
             @keyup.enter="doSearch" autofocus style="flex:1">
      <select v-model="store.searchEngine">
        <option value="local">本地索引</option>
        <option value="meili">Meilisearch</option>
      </select>
      <button class="primary" @click="doSearch" :disabled="searching">{{ searching ? '搜索中…' : '搜索' }}</button>
    </div>
    <div class="search-note">
      引擎：{{ engineLabel }}。Meilisearch 需在设置页填入服务地址，未填或不可达时自动回退本地索引。
      <template v-if="store.meiliUrl">服务地址：{{ store.meiliUrl }}</template>
    </div>

    <div class="search-result-list">
      <div class="doc-item" v-for="(it, i) in results" :key="i">
        <div class="doc-head">
          <code class="doc-name">{{ it.name }}</code>
          <span class="doc-cat">{{ it.category || 'misc' }}</span>
          <button v-if="it.example" class="ghost small" @click="copyExample(it.example)">复制示例</button>
        </div>
        <div class="doc-sig">{{ it.signature }}</div>
        <div class="doc-desc">{{ it.doc }}</div>
        <pre v-if="it.example" class="doc-example">{{ it.example }}</pre>
      </div>

      <div v-if="!searching && !results.length && query.trim()" class="search-empty">未找到相关文档。</div>
      <div v-if="!searching && !results.length && !query.trim()" class="search-empty search-kind">
        输入关键词开始检索。本地索引覆盖 VUS 常用内置函数（图形、交互、输入输出、JSON、日期、音频、传感器等）。
      </div>
    </div>
  </div>
  `,
};