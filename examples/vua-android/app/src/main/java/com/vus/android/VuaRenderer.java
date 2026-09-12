/*
 * VuaRenderer.java — 把规范化渲染树 JSON 建成 Android View 树
 *
 * 渲染树格式见 docs/VUA_RENDER_TREE.md。本类只认内部原语键
 * （type/id/children/variable/event），其余键按 type 归集的控件属性读取。
 *
 * 职责划分（2026-09 重构）：
 *   - RenderAdapter：vui 与宿主渲染器之间的翻译通道契约（M5；本类 = Gvui 实现）；
 *   - 本类：渲染管线——版本号协议缓存 / View diff / 页面 LRU / 屏内 id 子树复用 /
 *           容器（布局/侧边栏）递归构建 / 变量收集与保存；
 *   - Controls：叶子控件构建器（文本/按钮/输入/列表/课表/网页等外观逻辑）；
 *   - RenderNode：渲染树节点读取收敛（多键属性/子组件/事件），消除散落 JSON 遍历；
 *   - Theme：颜色单一真源。
 * 仅使用 Android SDK + org.json，无第三方依赖。
 */
package com.vus.android;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.os.Build;
import android.util.LruCache;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;

import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.Map;

/** Gvui 渲染实现：把规范化渲染树 JSON 建成 Android View 树（RenderAdapter 首个实现；
 *  vkt 将来是第二个实现，宿主侧只依赖接口）。 */
public final class VuaRenderer implements RenderAdapter {

    private static final int PAGE_CACHE_MAX = 6;           // 页面 View 缓存上限（LRU 逐出）
    private static final int DEFAULT_PAGE_CACHE_MAX = PAGE_CACHE_MAX;

    /** 本地图片位图缓存（G1）：文件路径 -> Bitmap。ImageLoader 只覆盖远程图，
     *  本地 decodeFile 每帧全量重建时反复解压同一文件，这里按路径 LRU 复用解码结果。 */
    private static final class BitmapMemCache extends LruCache<String, Bitmap> {
        BitmapMemCache(int maxBytes) { super(maxBytes); }
        @Override protected int sizeOf(String key, Bitmap value) {
            return value.getByteCount();
        }
    }

    /** 页面 View 缓存条目：已构建好的整棵子树 + 输入控件/变量文本/变量值快照。
     *  缓存命中（重新进入同一页面且内容相同）时直接挂回 root，跳过 JSON 解析与
     *  整树 View 重建；输入控件真实状态随 View 保留。 */
    private static final class PageCache {
        final View view;
        final Map<String, View> inputs;
        final Map<String, TextView> varTexts;
        final Map<String, String> lastVars;
        PageCache(View view, Map<String, View> inputs,
                  Map<String, TextView> varTexts, Map<String, String> lastVars) {
            this.view = view;
            this.inputs = inputs;
            this.varTexts = varTexts;
            this.lastVars = lastVars;
        }
    }

    final Context ctx;                         // 供 Controls 取 Context/密度
    private final ViewGroup root;              // 把重建的 View 树放进这里
    private final Map<String, View> inputs;    // id -> 输入控件（供手机回填/取值）
    final Map<String, String> savedVals;       // variable/id -> 上次输入值（重建时恢复控件状态）
    private final Map<String, TextView> varTexts; // variable -> 普通文本控件（View diff 增量更新用）
    private final Map<String, String> lastVars;   // variable -> 上次显示的变量值（diff 基准）
    boolean darkTheme = false;
    private long lastFp = -2L;                 // 当前显示内容的指纹（-2 初始占位，强制首帧）
    private long lastScreenId = -2L;           // 当前显示内容的屏序号（判断是否同一屏）
    private final LinkedHashMap<Long, PageCache> pageCache; // 渲染树指纹 -> 已构建 View（LRU）
    private final BitmapMemCache bitmapCache;               // G1：本地图片按路径复用位图
    private final int pageCacheMax;                         // G5：页面 View 缓存上限（参数化）

    /* G4：id 键控子树复用缓存。同一屏连续构建时，带 id 节点的子树快照（node.toString）
     * 相同则直接复用上次构建的 View 实例（含其输入/文本/变量登记），非文本局部变化
     * 只重建受影响子树，不再 removeAllViews + 整树 new JSONObject。换屏时清空。 */
    private static final class IdEntry {
        final long sid;
        final String snap;
        final View view;
        final Map<String, View> subInputs;          // 该子树登记的 输入控件（id/variable → View）
        final Map<String, TextView> subVarTexts;    // 该子树登记的 变量文本
        final Map<String, String> subVars;          // 该子树登记的 变量快照（构建路径收集）
        IdEntry(long sid, String snap, View view,
                Map<String, View> subInputs, Map<String, TextView> subVarTexts,
                Map<String, String> subVars) {
            this.sid = sid; this.snap = snap; this.view = view;
            this.subInputs = subInputs; this.subVarTexts = subVarTexts; this.subVars = subVars;
        }
    }
    private final LinkedHashMap<String, IdEntry> idCache; // 屏内 id -> 子树条目
    private long idCacheSid = -2L;                        // idCache 所属屏序号（-2=还未首次构建）

    private final Controls controls;                       // 叶子控件构建器

    public VuaRenderer(Context ctx, ViewGroup root) {
        this(ctx, root, DEFAULT_PAGE_CACHE_MAX);
    }

    public VuaRenderer(Context ctx, ViewGroup root, int pageCacheMax) {
        this.ctx = ctx;
        this.root = root;
        this.pageCacheMax = pageCacheMax;
        this.inputs = new HashMap<>();
        this.savedVals = new HashMap<>();
        this.varTexts = new HashMap<>();
        this.lastVars = new HashMap<>();
        this.pageCache = new LinkedHashMap<>(16, 0.75f, true);   // accessOrder=true → LRU
        this.bitmapCache = new BitmapMemCache(
                (int) (Runtime.getRuntime().maxMemory() / 16));  // 1/16 堆给本地位图
        this.idCache = new LinkedHashMap<>(32, 0.75f, true);
        this.controls = new Controls(this);
    }

    /** 版本号协议 + View diff：仅凭指纹/屏序号决定动作。
     *  1) 指纹与当前显示相同 → 零工作量；
     *  2) 页面 View 缓存命中（换页 + 内容未变，可跨屏实例）→ 直接显缓存；
     *  3) 同一屏（屏序号不变）→ 变量文本增量更新（View diff，只 setText 变化的控件）；
     *  4) 其余 → 取整树 JSON 解析构建，构建结果按指纹入 LRU 缓存。
     *  无屏指纹 < 0 → 显示空界面占位。 */
    public void render(long fp) {
        if (fp == lastFp) return;                           // 当前内容未变
        long sid = fp >= 0 ? VuaBridge.vuaScreenId() : -1;
        if (fp >= 0) {
            PageCache hit = pageCache.get(fp);              // 换页缓存命中（get 自动 LRU 置新）
            if (hit != null) {
                swapPage(hit, fp, sid);
                return;
            }
            if (sid == lastScreenId && lastScreenId >= 0) {
                /* 同一屏、内容变化 → View diff：先取新树尝试仅更新变化的文本 */
                try {
                    byte[] raw = VuaBridge.vuaRenderTreeBytes();
                    if (raw != null) {
                        JSONObject parsed = new JSONObject(new String(raw, StandardCharsets.UTF_8));
                        if (tryVarUpdate(new RenderNode(parsed))) {
                            lastFp = fp;                    // 文本已同步，当前内容即新指纹
                            return;
                        }
                    }
                } catch (Exception ignored) { }
            }
        }
        saveInputs();
        root.removeAllViews();
        lastFp = fp;
        lastScreenId = sid;
        if (fp < 0) {
            root.addView(TextView(ctx, "(空界面)"));
            return;
        }
        byte[] raw = VuaBridge.vuaRenderTreeBytes();
        String tree = raw == null ? null : new String(raw, StandardCharsets.UTF_8);
        try {
            if (tree == null || tree.isEmpty()) throw new Exception("空渲染树");
            RenderNode top = new RenderNode(new JSONObject(tree));
            darkTheme = resolveDark(top.attr("light", "主题", "theme"));
            root.setBackgroundColor(darkTheme ? Theme.BG_DARK : Theme.BG_LIGHT);
            applyStatusBar(darkTheme);
            /* 先构建到透明包装容器，整棵子树才能脱离 root 缓存复用 */
            varTexts.clear();
            idCacheSync(sid);                          // G4：换屏时清空屏内子树复用缓存
            LinearLayout wrapper = new LinearLayout(ctx);
            wrapper.setOrientation(LinearLayout.VERTICAL);
            Map<String, String> builtVars = new HashMap<>();
            buildInto(top, wrapper, builtVars);
            root.addView(wrapper, matchWrap());
            /* G2：变量快照在构建路径上顺带收集（不变量控件时 same diff 基准），
             * 不再整树第二遍 collectVars 扫描 */
            lastVars.clear();
            lastVars.putAll(builtVars);
            cachePage(fp, wrapper);
        } catch (Exception e) {
            String msg = "渲染失败: " + e.getMessage();
            root.addView(TextView(ctx, msg));
        }
    }

    /** 页面 View 入缓存（以指纹为 key，LRU，超出上限逐出最久未用的页）。 */
    private void cachePage(long fp, View view) {
        pageCache.put(fp, new PageCache(view, new HashMap<>(inputs),
                new HashMap<>(varTexts), new HashMap<>(lastVars)));
        while (pageCache.size() > pageCacheMax) {
            long eldest = pageCache.keySet().iterator().next();
            pageCache.remove(eldest);
        }
    }

    /** 换页缓存命中：挂回该页 View，并恢复输入/变量文本/变量值快照。 */
    private void swapPage(PageCache hit, long fp, long sid) {
        root.removeAllViews();
        idCacheSync(sid);                     // 换页：屏内子树复用缓存随屏切换
        root.addView(hit.view, matchWrap());
        inputs.clear();
        inputs.putAll(hit.inputs);
        varTexts.clear();
        varTexts.putAll(hit.varTexts);
        lastVars.clear();
        lastVars.putAll(hit.lastVars);
        lastFp = fp;
        lastScreenId = sid;
    }

    /** 屏内子树复用缓存与屏序号同步：换屏（sid 变化）即清空，防跨屏误用。 */
    private void idCacheSync(long sid) {
        if (sid != idCacheSid) {
            idCache.clear();
            idCacheSid = sid;
        }
    }

    /** View diff：新树只更新"变量文本"值变化的部分；若变化涉及非文本控件则
     *  退化返回 false（调用方走全量重建）。文本变化成功返回 true。 */
    private boolean tryVarUpdate(RenderNode top) {
        Map<String, String> next = collectVarsAll(top);
        boolean changed = false;
        for (Map.Entry<String, String> e : next.entrySet()) {
            String old = lastVars.get(e.getKey());
            if (old == null || !old.equals(e.getValue())) { changed = true; break; }
        }
        if (!changed) return false;                 // 变量未变却指纹不同 → 安全退化全量
        for (Map.Entry<String, String> e : next.entrySet()) {
            String old = lastVars.get(e.getKey());
            if (old != null && old.equals(e.getValue())) continue;
            TextView tv = varTexts.get(e.getKey());
            if (tv == null) return false;           // 变化变量不是普通文本 → 退化全量
            tv.setText(e.getValue());
        }
        lastVars.clear();
        lastVars.putAll(next);
        return true;
    }

    /** 递归收集渲染树中所有 variable → 显示值。 */
    private static Map<String, String> collectVarsAll(RenderNode node) {
        Map<String, String> out = new HashMap<>();
        collectInto(node, out);
        return out;
    }

    private static void collectInto(RenderNode node, Map<String, String> out) {
        String variable = node.variable();
        if (!variable.isEmpty()) {
            out.put(variable, node.attr("", "内容", "value"));
        }
        int n = node.childCount();
        for (int i = 0; i < n; i++) {
            RenderNode c = node.child(i);
            if (c != null) collectInto(c, out);
        }
    }

    /* ---------- 递归构建 ---------- */

    /* G4：id 键控子树复用。同屏连续构建时，带 id 节点的子树快照（node.toString）
     * 相同 → 直接复用上次构建的 View 实例（输入/文本/变量登记一并并回）；
     * 不同 → 构建到临时容器，归因子树对登记表的贡献后缓存新条目。 */
    private void buildInto(RenderNode node, ViewGroup parent,
                           Map<String, String> builtVars) throws Exception {
        String id = node.id();
        if (!id.isEmpty()) {
            String snap = node.snapshot();
            IdEntry hit = idCache.get(id);
            if (hit != null && hit.sid == idCacheSid && snap.equals(hit.snap)) {
                /* 复用：登记快照并回（等价于该子树本次“重建登记”，值取上次快照）。
                 * 必须先解绑旧 parent：全量重建时 root.removeAllViews() 只摘直接子节点
                 * （wrapper），不会递归解绑子孙——旧实例的 parent 仍是旧 wrapper，
                 * 直接 addView 到新树会抛 "already has a parent"。 */
                ViewGroup oldP = (ViewGroup) hit.view.getParent();
                if (oldP != null) oldP.removeView(hit.view);
                inputs.putAll(hit.subInputs);
                varTexts.putAll(hit.subVarTexts);
                builtVars.putAll(hit.subVars);
                parent.addView(hit.view);
                return;
            }
            /* 记账基线：构建前后登记表的差集 = 该子树的贡献 */
            java.util.Set<String> inB = new java.util.HashSet<>(inputs.keySet());
            java.util.Set<String> vtB = new java.util.HashSet<>(varTexts.keySet());
            java.util.Set<String> bvB = new java.util.HashSet<>(builtVars.keySet());
            LinearLayout tmp = new LinearLayout(ctx);
            buildIntoBody(node, tmp, builtVars);
            View got = tmp.getChildCount() == 1 ? tmp.getChildAt(0)
                    : (tmp.getChildCount() == 0 ? null : tmp);
            if (got != null) {
                idCache.put(id, new IdEntry(idCacheSid, snap, got,
                        diffMap(inB, inputs), diffMap(vtB, varTexts), diffMap(bvB, builtVars)));
                parent.addView(got);
            }
            return;
        }
        buildIntoBody(node, parent, builtVars);
    }

    /** 实况主题：运行期覆盖（VuaBridge.sTheme）优先，其次页面 .vua 声明；
     * "跟随系统"/system 按系统夜间模式（uiMode）判定。 */
    private boolean resolveDark(String pageTheme) {
        String t = VuaBridge.sTheme != null ? VuaBridge.sTheme : pageTheme;
        if ("system".equals(t) || "跟随系统".equals(t)) {
            int mode = ctx.getResources().getConfiguration().uiMode
                    & android.content.res.Configuration.UI_MODE_NIGHT_MASK;
            return mode == android.content.res.Configuration.UI_MODE_NIGHT_YES;
        }
        return "dark".equalsIgnoreCase(t) || "暗色".equals(t);
    }

    /** 构建分发主体：容器/侧边栏在本类递归，叶子控件委托 Controls。 */
    private void buildIntoBody(RenderNode node, ViewGroup parent,
                               Map<String, String> builtVars) throws Exception {
        String vv = node.variable();
        if (!vv.isEmpty()) {
            builtVars.put(vv, node.attr("", "内容", "value"));
        }
        switch (node.type()) {
            case "界面": case "列": case "column": case "卡片": case "card":
            case "表单": case "form":   { layout(node, parent, true, builtVars); return; }
            case "行": case "row":      { layout(node, parent, false, builtVars); return; }
            case "文本": case "text":   { parent.addView(controls.textView(node)); return; }
            case "按钮": case "button": { parent.addView(controls.buttonView(node)); return; }
            case "输入框": case "text_input": case "tarea": {
                parent.addView(controls.editView(node, true)); return;
            }
            case "复选框": case "checkbox": { parent.addView(controls.checkView(node)); return; }
            case "开关": case "switch":      { parent.addView(controls.switchView(node)); return; }
            case "滑块": case "slider":      { controls.sliderView(node, parent); return; }
            case "下拉": case "spinner":     { controls.spinnerView(node, parent); return; }
            case "进度条": case "progress":  { controls.progressView(node, parent); return; }
            case "分隔线": case "divider":   { controls.dividerView(node, parent); return; }
            case "间距": case "space":       { controls.spaceView(node, parent); return; }
            case "图片": case "image":       { controls.imageView(node, parent); return; }
            case "图标": case "icon":        { controls.iconView(node, parent); return; }
            case "课表": case "table": case "grid": { controls.classTable(node, parent); return; }
            case "列表": case "list": case "listview": { controls.listView(node, parent); return; }
            case "侧边栏": case "drawer": case "sidebar": { drawerView(node, parent, builtVars); return; }
            case "网页": case "web": case "浏览器": { controls.webView(node, parent); return; }
            case "音乐播放器": case "music_player": case "musicplayer": { controls.musicPlayerView(node, parent); return; }
            case "视频播放器": case "video_player": case "videoplayer": { controls.videoPlayerView(node, parent); return; }
            default: {
                // 未知/扩展 type：降级为一个文本框占位（严格原则下应报错，这里保证不崩）。
                parent.addView(TextView(ctx, "[" + node.type() + "]"));
            }
        }
    }

    /** 复用判定用的登记表差集：返回 cur 中不在 before 里的键值对。 */
    private static <K, V> Map<K, V> diffMap(java.util.Set<K> before, Map<K, V> cur) {
        Map<K, V> m = new HashMap<>();
        for (Map.Entry<K, V> e : cur.entrySet()) {
            if (!before.contains(e.getKey())) m.put(e.getKey(), e.getValue());
        }
        return m;
    }

    /** 竖容器（列/卡片/表单/界面）套大圆角卡片背景，阴影分层形成 MD3 表面层级（无描边）。 */
    private void styleVertCard(LinearLayout ll) {
        android.graphics.drawable.GradientDrawable g = new android.graphics.drawable.GradientDrawable();
        g.setColor(Theme.card(darkTheme));
        g.setCornerRadius(dp(16));
        ll.setBackground(g);
        ll.setElevation(dp(2));
        ll.setPadding(dp(16), dp(12), dp(16), dp(12));
    }

    /** 状态栏跟随页面主题底色与深浅图标（系统自带 MD3 主题下自动处理，此处保证 10/11 一致）。 */
    private void applyStatusBar(boolean dark) {
        if (VuaBridge.sActivity == null) return;
        android.view.Window w = VuaBridge.sActivity.getWindow();
        if (w == null) return;
        w.addFlags(android.view.WindowManager.LayoutParams.FLAG_DRAWS_SYSTEM_BAR_BACKGROUNDS);
        w.setStatusBarColor(dark ? Theme.BG_DARK : Theme.BG_LIGHT);
        if (Build.VERSION.SDK_INT >= 23) {
            int v = w.getDecorView().getSystemUiVisibility();
            if (dark) v &= ~android.view.View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR;
            else v |= android.view.View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR;
            w.getDecorView().setSystemUiVisibility(v);
        }
    }

    private LinearLayout makeLayout(boolean vert) {
        LinearLayout ll = new LinearLayout(ctx);
        ll.setOrientation(vert ? LinearLayout.VERTICAL : LinearLayout.HORIZONTAL);
        if (vert) ll.setPadding(dp(12), dp(6), dp(12), dp(6));
        return ll;
    }

    private void layout(RenderNode node, ViewGroup parent, boolean vert,
                        Map<String, String> builtVars) throws Exception {
        LinearLayout ll = makeLayout(vert);
        /* 宽/高/权重：静态布局参数（布局模板展开产物使用，如侧边栏"列宽/主区权重"）。
         * 缺省 = matchWrap，与既有行为一致；显式任一参数才走精确 LayoutParams。 */
        int w = node.intAttr(0, "宽度", "宽");
        int h = node.intAttr(0, "高度", "高");
        int weight = node.intAttr(0, "权重");
        if (w > 0 || h > 0 || weight > 0) {
            int lw = w > 0 ? dp(w) : ViewGroup.LayoutParams.WRAP_CONTENT;
            int lh = h > 0 ? dp(h) : ViewGroup.LayoutParams.WRAP_CONTENT;
            parent.addView(ll, new LinearLayout.LayoutParams(lw, lh, weight));
        } else {
            parent.addView(ll, matchWrap());
        }
        if (vert) styleVertCard(ll);
        int n = node.childCount();
        for (int i = 0; i < n; i++) {
            RenderNode c = node.child(i);
            if (c != null) buildInto(c, ll, builtVars);
        }
    }

    /* ---------- 新控件：侧边栏抽屉（DrawerLayout 语义，无第三方依赖手写） ----------
     * 渲染树节点：
     *   { type:"侧边栏",
     *     "菜单":[ "首页" | {"标题":"..","副标题":".."} ],
     *     "内容":[ 子组件... ],
     *     "事件":{"事件名":"切页", 可带 收集变量/回调变量},
     *     "宽度":240, "侧":"左"|"右" }
     * 抽屉默认收起；「侧」边缘 20dp 水平向内拖动滑出，点击遮罩或菜单项后收起。
     * 菜单项点击事件载荷与「列表」一致：{"下标":i,"值":"标题"}（+ 收集/回调变量）。 */
    private void drawerView(final RenderNode node, final ViewGroup parent,
                            final Map<String, String> builtVars) throws Exception {
        final String evName = node.eventName();
        final String nodeId = node.id();
        final boolean left = !"右".equals(node.attr("左", "侧", "side"));
        final int width = dp(node.intAttr(240, "宽度", "width"));
        final JSONArray menu = node.arr("菜单");
        final JSONArray content = node.arr("内容");

        final FrameLayout host = new FrameLayout(ctx);
        parent.addView(host, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        /* 主内容区：占满容器，抽屉滑出时被遮罩盖住（点击遮罩收起） */
        final FrameLayout contentHost = new FrameLayout(ctx);
        host.addView(contentHost, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        if (content != null) {
            for (int i = 0; i < content.length(); i++) {
                JSONObject c = content.optJSONObject(i);
                if (c != null) buildInto(new RenderNode(c), contentHost, builtVars);
            }
        }

        /* 遮罩：抽屉打开时盖住内容区并拦截点击 → 收起 */
        final View scrim = new View(ctx);
        scrim.setBackgroundColor(0x66000000);
        scrim.setAlpha(0f);
        scrim.setVisibility(View.GONE);
        host.addView(scrim, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        /* 菜单面板：宽度由「宽度」决定，靠「侧」对齐；初始平移出屏（收起态） */
        final LinearLayout panel = new LinearLayout(ctx);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setBackgroundColor(Theme.card(darkTheme));
        panel.setElevation(dp(8));
        host.addView(panel, new FrameLayout.LayoutParams(width,
                ViewGroup.LayoutParams.MATCH_PARENT, left ? Gravity.LEFT : Gravity.RIGHT));
        final float closedTx = left ? -width : width;
        panel.setTranslationX(closedTx);

        /* 边缘滑出：左/右边缘 20dp 透明手柄，水平拖动带出面板（关闭时才在前台） */
        final View handle = new View(ctx);
        host.addView(handle, new FrameLayout.LayoutParams(
                dp(20), ViewGroup.LayoutParams.MATCH_PARENT, left ? Gravity.LEFT : Gravity.RIGHT));

        /* 开合动画：面板平移 + 遮罩淡显（ViewPropertyAnimator，SDK 自带） */
        final Runnable doOpen = () -> {
            if (Math.abs(panel.getTranslationX()) < 1f) return;   // 已开
            panel.animate().translationX(0f).setDuration(180).start();
            scrim.setVisibility(View.VISIBLE);
            scrim.animate().alpha(0.55f).setDuration(180).start();
            handle.setVisibility(View.GONE);
        };
        final Runnable doClose = () -> {
            if (Math.abs(panel.getTranslationX()) < 1f) return;   // 已关
            panel.animate().translationX(closedTx).setDuration(180).start();
            scrim.animate().alpha(0f).setDuration(180)
                    .withEndAction(() -> scrim.setVisibility(View.GONE)).start();
            handle.setVisibility(View.VISIBLE);
        };
        scrim.setOnClickListener(v -> doClose.run());

        /* 菜单项 → 行；点击事件载荷与列表一致（下标/值 + 收集/回调变量），触发后收起 */
        if (menu != null) {
            for (int i = 0; i < menu.length(); i++) {
                final int pos = i;
                Object it = menu.opt(i);
                String title = it instanceof String ? (String) it : "";
                if (it instanceof JSONObject) {
                    JSONObject o = (JSONObject) it;
                    title = o.optString("标题", o.optString("文字",
                            o.optString("text", o.optString("title", ""))));
                }
                final String fTitle = title;
                LinearLayout row = new LinearLayout(ctx);
                row.setOrientation(LinearLayout.HORIZONTAL);
                row.setGravity(Gravity.CENTER_VERTICAL);
                row.setPadding(dp(16), dp(12), dp(16), dp(12));
                row.setMinimumHeight(dp(48));
                TextView rowTv = TextView(ctx);
                rowTv.setText(title);
                rowTv.setTextSize(15);
                rowTv.setTextColor(darkTheme ? 0xFFE0E0E0 : 0xFF1A1F2E);
                row.addView(rowTv);
                row.setOnClickListener(v -> {
                    StringBuilder vb = new StringBuilder("{");
                    vb.append("\"下标\":").append(pos)
                      .append(",\"值\":\"").append(escapeJs(fTitle)).append("\"");
                    String base = collectVars(node);
                    if (base.length() > 2) {
                        vb.append(',').append(base.substring(1, base.length() - 1));
                    }
                    vb.append('}');
                    if (evName != null) {
                        VuaBridge.vuaTrigger(evName, vb.toString());
                    } else if (!nodeId.isEmpty()) {
                        VuaBridge.vuaTriggerById(nodeId, vb.toString());
                    }
                    doClose.run();
                    refresh();
                });
                panel.addView(row, new LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            }
        }

        /* 手势：手柄捕获拖动，实时跟手、松手按过半判开合 */
        final float[] downX = { 0f };
        final boolean[] dragging = { false };
        handle.setOnTouchListener((v, e) -> {
            switch (e.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    downX[0] = e.getRawX();
                    dragging[0] = true;
                    return true;
                case MotionEvent.ACTION_MOVE:
                    if (!dragging[0]) return true;
                    float dx = e.getRawX() - downX[0];
                    float ntx = closedTx + dx;
                    if (left) ntx = Math.max(-width, Math.min(0f, ntx));
                    else      ntx = Math.min(width, Math.max(0f, ntx));
                    panel.setTranslationX(ntx);
                    float p = 1f - Math.abs(ntx) / (float) width;
                    scrim.setAlpha(0.55f * p);
                    scrim.setVisibility(p > 0.01f ? View.VISIBLE : View.GONE);
                    return true;
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_CANCEL:
                    dragging[0] = false;
                    if (Math.abs(panel.getTranslationX()) < width / 2f) doOpen.run();
                    else doClose.run();
                    return true;
            }
            return false;
        });
    }

    /* ---------- 事件 / 变量工具 ---------- */

    /** 收集按钮事件的变量参数，输出 JSON 对象字符串（如 {"星级":"1","金额":"1280"}）。
     *  收集变量：读输入控件当前值；回调变量：键=值 字面量参数（星级 1..5 等）。 */
    String collectVars(RenderNode node) {
        StringBuilder sb = new StringBuilder("{");
        boolean first = true;
        JSONArray collect = node.collectVarNames();
        if (collect != null) {
            for (int i = 0; i < collect.length(); i++) {
                String name = collect.optString(i);
                View v = inputs.get(name);
                if (v == null) continue;
                String val = inputOf(v);
                if (!first) sb.append(',');
                first = false;
                sb.append('"').append(escapeJs(name)).append("\":\"").append(escapeJs(val)).append('"');
            }
        }
        JSONArray cb = node.callbackVarNames();
        if (cb != null) {
            for (int i = 0; i < cb.length(); i++) {
                String s = cb.optString(i);
                int eq = s.indexOf('=');
                if (eq <= 0) continue;
                String k = s.substring(0, eq).trim();
                String v = s.substring(eq + 1).trim();
                if (k.isEmpty()) continue;
                if (!first) sb.append(',');
                first = false;
                sb.append('"').append(escapeJs(k)).append("\":\"").append(escapeJs(v)).append('"');
            }
        }
        return sb.append('}').toString();
    }

    /** 读取输入控件的当前值（字符串形态，供 collect 与状态保存共用）。 */
    static String inputOf(View v) {
        if (v instanceof EditText) return ((EditText) v).getText().toString();
        if (v instanceof android.widget.CheckBox) return String.valueOf(((android.widget.CheckBox) v).isChecked());
        if (v instanceof android.widget.Switch) return String.valueOf(((android.widget.Switch) v).isChecked());
        if (v instanceof android.widget.SeekBar) return String.valueOf(((android.widget.SeekBar) v).getProgress());
        if (v instanceof android.widget.Spinner) {
            android.widget.Spinner sp = (android.widget.Spinner) v;
            return sp.getSelectedItem() != null ? sp.getSelectedItem().toString() : "";
        }
        return "";
    }

    /** 重建前保存所有输入控件当前值，供重建后恢复（避免整树刷新导致控件复位）。
     *  RenderAdapter：宿主（VusSession 快照）与整树重建前共用；整树重建由 render 调用。 */
    @Override public void saveInputs() {
        for (Map.Entry<String, View> e : inputs.entrySet()) {
            savedVals.put(e.getKey(), inputOf(e.getValue()));
        }
    }

    /** RenderAdapter：输入控件值保存表（variable/id -> 值，可序列化）。 */
    @Override public Map<String, String> savedVals() { return savedVals; }

    /** 恢复会话快照状态：把快照的变量/控件值写回保存表（随后按节点 restore 生效）。 */
    @Override public void restoreSaved(Map<String, String> vals) {
        if (vals != null) savedVals.putAll(vals);
    }

    /* ---------- 便捷工厂（Controls 经包级访问） ---------- */

    TextView TextView(Context c) { return new TextView(c); }
    TextView TextView(Context c, String s) { TextView t = new TextView(c); t.setText(s); return t; }
    int dp(float v) { return (int) (v * ctx.getResources().getDisplayMetrics().density); }
    private ViewGroup.LayoutParams matchWrap() {
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    /** 触发后重建整棵 View（native 可能已换屏）。经 VuaBridge 合并请求，
     *  避免"换屏回调排队 + 本地立即刷新"两次全量重建。 */
    void refresh() { VuaBridge.requestRender(); }

    void rememberInput(RenderNode node, View v, boolean registerVarText) {
        String id = node.id();
        if (!id.isEmpty()) inputs.put(id, v);
        String variable = node.variable();
        if (!variable.isEmpty()) {
            v.setTag("variable:" + variable);
            inputs.put(variable, v);
            // 文本控件额外登记变量文本（View diff 增量更新用）
            if (registerVarText && v instanceof TextView) varTexts.put(variable, (TextView) v);
        }
    }

    boolean hasSaved(String variable) { return savedVals.containsKey(variable); }
    String savedVal(String variable) { return savedVals.get(variable); }

    static String escapeJs(String s) {
        return s == null ? "" : s.replace("\\", "\\\\").replace("\"", "\\\"");
    }

    /* ---------- 本地图片（G1）：按路径 LRU 复用位图，避免反复 decodeFile ---------- */
    Bitmap loadLocalBitmap(String absPath) {
        Bitmap bm = bitmapCache.get(absPath);
        if (bm != null) return bm;
        try {
            bm = BitmapFactory.decodeFile(absPath);
            if (bm != null) bitmapCache.put(absPath, bm);
        } catch (Exception ignored) { }
        return bm;
    }
}