/* ================================================================
 * editor.js —— 代码视图（Vue 组件）
 * CodeMirror 编辑器 + 编译检查 + 流式终端（VUS / Shell，SSE）
 * ================================================================ */

window.VUS = window.VUS || {};
var C = window.VUS;

window.VUS.EditorView = {
  name: "EditorView",

  data: function () {
    var self = this;
    return {
      C: C,                    // 模板中直接访问 C.xxx
      store: C.store,
      runKind: 'vus',          // vus | shell
      shellCmd: '',
      compiling: false,
      compileErrors: [],
      terminalText: '',
    };
  },

  mounted: function () {
    var self = this;
    if (typeof CodeMirror === 'undefined') return;
    this.cm = CodeMirror.fromTextArea(this.$refs.codearea, {
      mode: 'vus',
      lineNumbers: true,
      matchBrackets: true,
      indentUnit: 2,
      tabSize: 2,
      autofocus: false,
      theme: 'default',
      extraKeys: { 'Ctrl-Space': 'autocomplete' },
    });
    this.cm.setOption('extraKeys',
      { 'Ctrl-Space': this.autocomplete.bind(this), 'Ctrl-Enter': function(){ self.runVus(); } });
    this.cm.setValue(this.store.code);
    // 若设计里已有控件但代码尚为空，主动同步一次，避免切页后短暂空白
    if (!this.store.code && this.store.design.controls.length) {
      C.syncDesignToCode();
    }
    this.cm.on('change', function () {
      self.store.code = self.cm.getValue();
    });
    // 提供补全宿主
    this.cm.on('keyup', this.maybeHint.bind(this));
    // 外部代码变化（例如设计同步过来）时刷新编辑器
    this.unwatch = this.$watch(function () { return self.store.code; }, function (v) {
      if (self.cm && v !== self.cm.getValue()) {
        self.cm.setValue(v);
      }
    });
    if (typeof this.cm.showHint === 'undefined') {
      this.cm.showHint = function () {};
    }
  },

  beforeUnmount: function () {
    if (this.unwatch) this.unwatch();
    if (this.cm) this.cm.toTextArea && this.cm.toTextArea();
  },

  methods: {
    autocomplete: function () {
      if (this.cm && this.cm.showHint) {
        this.cm.showHint({ hint: this.hintProvider });
      }
    },

    hintProvider: function (cm) {
      var cur = cm.getCursor();
      var line = cm.getLine(cur.line).slice(0, cur.ch);
      var m = line.match(/[\u4e00-\u9fa5A-Za-z0-9_]+$/);
      var token = m ? m[0] : '';
      var list = C.BUILTIN_NAMES
        .filter(function (n) { return n.indexOf(token) === 0; })
        .map(function (n) { return { text: n, displayText: n, type: 'builtin', origin: 'vus' }; });
      return { list: list, from: CodeMirror.Pos(cur.line, cur.ch - token.length), to: cur };
    },

    maybeHint: function (cm, ev) {
      if (ev && (ev.ctrlKey || ev.metaKey || ev.keyCode === 13)) return;
      if (this.cm && this.cm.showHint) {
        setTimeout(() => {
          this.cm.showHint({ hint: this.hintProvider, completeSingle: false });
        }, 260);
      }
    },

    /* ---- 编译检查 ---- */
    compile: async function () {
      this.compiling = true;
      this.compileErrors = [];
      var r = await C.http('/api/compile', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ source: this.store.code }),
      });
      this.compiling = false;
      if (r.data && r.data.ok) {
        if (r.data.errors && r.data.errors.length) this.compileErrors = r.data.errors;
        else C.toast('ok', '编译通过');
      } else {
        C.toast('err', (r.data && r.data.error) || '编译请求失败');
      }
    },

    /* ---- 运行（流式终端） ---- */
    runVus: function () {
      this.run({ kind: 'vus', source: this.store.code });
    },
    runShell: function () {
      this.run({ kind: 'shell', command: this.shellCmd });
    },

    run: async function (payload) {
      if (!this.store.token) {
        C.toast('err', '请先在设置页填写后端 Token');
        return;
      }
      if (this.store.running) return;
      var self = this;
      this.store.running = true;
      this.store.terminal = [];
      this.terminalText = '';

      try {
        var res = await fetch(C.apiBase() + '/api/run', {
          method: 'POST',
          headers: Object.assign({ 'Content-Type': 'application/json' }, C.authHeaders()),
          body: JSON.stringify(payload),
        });
        if (res.status === 401) {
          this.store.running = false;
          C.toast('err', '未授权：Token 无效，请在设置页检查');
          return;
        }
        var reader = res.body.getReader();
        var decoder = new TextDecoder('utf-8');
        while (true) {
          var r = await reader.read();
          if (r.done) break;
          var txt = decoder.decode(r.value, { stream: true });
          // 解析 SSE：data: {...}\n\n
          var lines = txt.split('\n');
          for (var i = 0; i < lines.length; i++) {
            var l = lines[i];
            if (l.indexOf('data: ') !== 0) continue;
            var jsonStr = l.slice(6).trim();
            if (!jsonStr) continue;
            try {
              var evt = JSON.parse(jsonStr);
              if (evt.e === 'exit') {
                this.store.terminal.push('\\n[进程退出，返回码 ' + evt.d + ']');
                this.store.running = false;
              } else {
                this.store.terminal.push(evt.d);
              }
            } catch (e) { /* 忽略空行/心跳 */ }
          }
          this.flushTerminal();
        }
        this.store.running = false;
      } catch (err) {
        this.store.running = false;
        /* 稍后 scroll 到终端；_flush 兜底 */
        C.toast('err', '运行失败：' + (err && err.message ? err.message : '无法连接后端'));
      }
      this.flushTerminal();
    },

    flushTerminal: function () {
      var self = this;
      this.$nextTick(function () {
        var el = self.$refs.term;
        if (el) el.scrollTop = el.scrollHeight;
      });
    },
    termLineCls: function (line) {
      if (typeof line !== 'string') return '';
      if (line.indexOf('[进程退出') >= 0) return 'term-exit';
      if (line.indexOf('error') >= 0 || line.indexOf('错误') >= 0) return 'term-err';
      return 'term-out';
    },

    applyToDesign: function () {
      C.parseCodeToDesign();
    },
  },

  template: `
  <div class="editor-layout">
    <div class="editor-pane">
      <div class="editor-bar">
        <span class="tool-label">代码(.vus)</span>
        <button @click="compile" :disabled="compiling">{{ compiling ? '检查中…' : '编译检查' }}</button>
        <button class="primary" @click="runVus" :disabled="store.running">运行 VUS</button>
        <button class="ghost" @click="applyToDesign">应用到设计</button>
      </div>
      <div class="editor-wrap">
        <textarea ref="codearea" :value="store.code"></textarea>
      </div>
      <div class="compile-errors" v-if="compileErrors.length">
        <div class="ce-title">编译错误（{{ compileErrors.length }}）</div>
        <div class="ce-line" v-for="(e, i) in compileErrors" :key="i">
          {{ e.line ? ('第' + e.line + '行' + (e.col ? ' 第' + e.col + '列' : '')) : '' }} {{ e.msg }}
        </div>
      </div>
    </div>

    <div class="term-pane">
      <div class="term-bar">
        <span class="tool-label">运行终端</span>
        <select v-model="runKind">
          <option value="vus">执行 VUS</option>
          <option value="shell">执行 Shell</option>
        </select>
        <input v-if="runKind === 'shell'" v-model="shellCmd" placeholder="输入 shell 命令，如 ls -la、echo hi"
               @keyup.enter="runShell" class="shell-cmd">
        <button v-if="runKind === 'shell'" @click="runShell" :disabled="store.running">执行</button>
      </div>
      <div class="terminal" ref="term">
        <template v-for="(line, i) in store.terminal" :key="'l'+i">
          <div :class="termLineCls(line)">{{ line }}</div>
        </template>
        <div v-if="!store.terminal.length" class="term-hint">
          暂无输出。运行 VUS 将展示打印与图形事件输出；也可执行 Shell 命令。
        </div>
      </div>
      <div class="term-note">说明：/api/run 需要 Token，须先在设置页填写后端 Token。</div>
    </div>
  </div>
  `,
};