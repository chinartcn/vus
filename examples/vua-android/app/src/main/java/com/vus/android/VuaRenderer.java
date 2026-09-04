/*
 * VuaRenderer.java — 把规范化渲染树 JSON 建成 Android View 树
 *
 * 渲染树格式见 docs/VUA_RENDER_TREE.md。本类只认内部原语键
 * （type/id/children/variable/event），其余键按 type 归集的控件属性读取。
 * 由于 native 侧渲染树透传的是 .vua 的属性和中文 type，这里据此分发。
 *
 * 仅使用 Android SDK + org.json，无第三方依赖。
 */
package com.vus.android;

import android.content.Context;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.Bitmap;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.ArrayAdapter;
import android.widget.Switch;
import android.widget.TableLayout;
import android.widget.TableRow;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public final class VuaRenderer {

    private static final int COLOR_PRIMARY = 0xFF2962FF; // 主题主色（与 styles.xml colorAccent 一致）
    private static final int BG_LIGHT      = 0xFFF5F6FA; // 页面浅灰底（Material Light windowBackground）
    private static final int PAGE_CACHE_MAX = 6;          // 页面 View 缓存上限（LRU 逐出）

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

    private final Context ctx;
    private final ViewGroup root;              // 把重建的 View 树放进这里
    private final Map<String, View> inputs;    // id -> 输入控件（供手机回填/取值）
    private final Map<String, String> savedVals; // variable/id -> 上次输入值（重建时恢复控件状态）
    private final Map<String, TextView> varTexts; // variable -> 普通文本控件（View diff 增量更新用）
    private final Map<String, String> lastVars;   // variable -> 上次显示的变量值（diff 基准）
    private boolean darkTheme = false;
    private long lastFp = -2L;                 // 当前显示内容的指纹（-2 初始占位，强制首帧）
    private long lastScreenId = -2L;           // 当前显示内容的屏序号（判断是否同一屏）
    private final LinkedHashMap<Long, PageCache> pageCache; // 渲染树指纹 -> 已构建 View（LRU）

    public VuaRenderer(Context ctx, ViewGroup root) {
        this.ctx = ctx;
        this.root = root;
        this.inputs = new HashMap<>();
        this.savedVals = new HashMap<>();
        this.varTexts = new HashMap<>();
        this.lastVars = new HashMap<>();
        this.pageCache = new LinkedHashMap<>(16, 0.75f, true);   // accessOrder=true → LRU
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
                        if (tryVarUpdate(parsed)) {
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
            JSONObject parsed = new JSONObject(tree);
            darkTheme = "dark".equalsIgnoreCase(parsed.optString("主题", parsed.optString("theme", "light")));
            root.setBackgroundColor(darkTheme ? 0xFF121212 : BG_LIGHT);
            /* 先构建到透明包装容器，整棵子树才能脱离 root 缓存复用 */
            varTexts.clear();
            LinearLayout wrapper = new LinearLayout(ctx);
            wrapper.setOrientation(LinearLayout.VERTICAL);
            buildInto(parsed, wrapper);
            root.addView(wrapper, matchWrap());
            lastVars.clear();
            lastVars.putAll(collectVars(parsed, new HashMap<String, String>()));
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
        while (pageCache.size() > PAGE_CACHE_MAX) {
            long eldest = pageCache.keySet().iterator().next();
            pageCache.remove(eldest);
        }
    }

    /** 换页缓存命中：挂回该页 View，并恢复输入/变量文本/变量值快照。 */
    private void swapPage(PageCache hit, long fp, long sid) {
        root.removeAllViews();
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

    /** View diff：新树只更新"变量文本"值变化的部分；若变化涉及非文本控件则
     *  退化返回 false（调用方走全量重建）。文本变化成功返回 true。 */
    private boolean tryVarUpdate(JSONObject tree) {
        Map<String, String> next = collectVars(tree, new HashMap<String, String>());
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
    private static Map<String, String> collectVars(JSONObject node, Map<String, String> out) {
        String variable = node.optString("variable", "");
        if (!variable.isEmpty()) {
            out.put(variable, node.optString("内容", node.optString("value", "")));
        }
        JSONArray ch = node.optJSONArray("children");
        if (ch == null) ch = node.optJSONArray("子组件");
        if (ch != null) {
            for (int i = 0; i < ch.length(); i++) {
                JSONObject c = ch.optJSONObject(i);
                if (c != null) collectVars(c, out);
            }
        }
        return out;
    }

    /* ---------- 分发 ---------- */

    private void buildInto(JSONObject node, ViewGroup parent) throws Exception {
        String type = node.optString("type");
        switch (type) {
            case "界面": case "列": case "column": case "卡片": case "card":
            case "表单": case "form":   { layout(node, parent, vertical(node)); return; }
            case "行": case "row":      { layout(node, parent, false); return; }
            case "文本": case "text":   { parent.addView(textView(node)); return; }
            case "按钮": case "button": { parent.addView(buttonView(node)); return; }
            case "输入框": case "text_input": case "tarea": {
                parent.addView(editView(node, true)); return;
            }
            case "复选框": case "checkbox": { parent.addView(checkView(node)); return; }
            case "开关": case "switch":      { parent.addView(switchView(node)); return; }
            case "滑块": case "slider":      { sliderView(node, parent); return; }
            case "下拉": case "spinner":     { spinnerView(node, parent); return; }
            case "图片": case "image":       { imageView(node, parent); return; }
            case "课表": case "table": case "grid": { classTable(node, parent); return; }
            default: {
                // 未知/扩展 type：降级为一个文本框占位（严格原则下应报错，这里保证不崩）。
                TextView t = TextView(ctx);
                t.setText("[" + type + "]");
                parent.addView(t);
            }
        }
    }

    private static boolean vertical(JSONObject node) {
        String t = node.optString("type");
        return !(t.equals("行") || t.equals("row"));
    }

    private LinearLayout makeLayout(boolean vert) {
        LinearLayout ll = new LinearLayout(ctx);
        ll.setOrientation(vert ? LinearLayout.VERTICAL : LinearLayout.HORIZONTAL);
        if (vert) {
            ll.setPadding(dp(12), dp(6), dp(12), dp(6));
        }
        return ll;
    }

    /** 竖容器（列/卡片/表单/界面）套圆角白卡片背景，浅灰页面上形成 Material 卡片层次。 */
    private void styleVertCard(LinearLayout ll) {
        android.graphics.drawable.GradientDrawable g = new android.graphics.drawable.GradientDrawable();
        g.setColor(darkTheme ? 0xFF1E1E1E : 0xFFFFFFFF);
        g.setCornerRadius(dp(14));
        if (!darkTheme) g.setStroke(dp(1), 0x14000000);
        ll.setBackground(g);
        ll.setElevation(dp(1));
        ll.setPadding(dp(16), dp(12), dp(16), dp(12));
    }

    private void layout(JSONObject node, ViewGroup parent, boolean vert) throws Exception {
        LinearLayout ll = makeLayout(vert);
        parent.addView(ll, matchWrap());
        if (vert) styleVertCard(ll);
        JSONArray ch = node.optJSONArray("children");
        if (ch == null) ch = node.optJSONArray("子组件");
        if (ch != null) {
            for (int i = 0; i < ch.length(); i++) {
                JSONObject c = ch.optJSONObject(i);
                if (c != null) buildInto(c, ll);
            }
        }
    }

    /* ---------- 叶子控件 ---------- */

    private TextView textView(JSONObject node) {
        TextView t = TextView(ctx);
        t.setText(node.optString("内容", node.optString("value", "")));
        t.setTextColor(darkTheme ? 0xFFE0E0E0 : 0xFF1A1F2E);
        String style = node.optString("样式", node.optString("style", "body"));
        float sp = 14; int styleB = Typeface.NORMAL;
        switch (style) {
            case "title": sp = 18; styleB = Typeface.BOLD; break;
            case "headline": sp = 22; styleB = Typeface.BOLD; break;
            case "caption": sp = 12; break;
            default: sp = 14;
        }
        t.setTextSize(sp); t.setTypeface(Typeface.DEFAULT, styleB);
        // 标题类文本用主题主色，正文保持正文色
        if (styleB == Typeface.BOLD) t.setTextColor(darkTheme ? 0xFF8FB4FF : COLOR_PRIMARY);
        rememberInput(node, t);
        // View diff：登记变量绑定文本，供同屏增量更新（只 setText 变化的部分）
        String variable = node.optString("variable", "");
        if (!variable.isEmpty()) varTexts.put(variable, t);
        return t;
    }

    private Button buttonView(final JSONObject node) {
        final Button b = new Button(ctx);
        b.setText(node.optString("文字", node.optString("text", "确定")));
        b.setAllCaps(false);
        String btxt = b.getText().toString();
        if ("★".equals(btxt)) {
            // 星级评分「★」用幽灵按钮：透明底 + 主色大字（主题 Colored 样式会误填色块）
            b.setBackgroundColor(Color.TRANSPARENT);
            b.setTextSize(22);
            b.setTextColor(darkTheme ? 0xFF8FB4FF : COLOR_PRIMARY);
            b.setMinHeight(0);
        } else {
            b.setMinHeight(dp(44));
            b.setPadding(dp(18), 0, dp(18), 0);
            if (darkTheme) b.setTextColor(0xFFE0E0E0);
        }
        // 存事件名/id，供点击时派发
        final String evName = eventName(node);
        final JSONArray collect = eventCollect(node);
        b.setOnClickListener(v -> {
            // 检查更新：直接在 Java 侧处理，不走 native 事件
            if ("检查更新".equals(evName)) {
                if (VuaBridge.onCheckUpdate != null) VuaBridge.onCheckUpdate.run();
                return;
            }
            String vars = collectVars(node);
            if (evName != null) {
                VuaBridge.vuaTrigger(evName, vars);
            } else {
                VuaBridge.vuaTriggerById(node.optString("id"), vars);
            }
            // native 若换屏会经 vua_notify_rerender 回调 onNativeRerender 触发重建；
            // 这里再主动刷一次，兜底事件 handler 只是改了变量、没有换屏的场景。
            refresh();
        });
        rememberInput(node, b);
        return b;
    }

    private EditText editView(JSONObject node, boolean multiline) {
        final EditText e = new EditText(ctx);
        e.setInputType(multiline ? (InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE)
                                 : InputType.TYPE_CLASS_TEXT);
        e.setHint(node.optString("提示", node.optString("placeholder", "")));
        e.setSingleLine(!multiline);
        // Material 风格：圆角浅底衬色输入框（弃默认下划线，外描边聚焦前后统一）
        android.graphics.drawable.GradientDrawable eg = new android.graphics.drawable.GradientDrawable();
        eg.setColor(darkTheme ? 0xFF2A2A2A : 0xFFF2F3F7);
        eg.setCornerRadius(dp(10));
        eg.setStroke(dp(1), darkTheme ? 0xFF444444 : 0xFFDDE2EC);
        e.setBackground(eg);
        e.setPadding(dp(12), dp(10), dp(12), dp(10));
        e.setMinHeight(dp(46));
        final int normalStroke = darkTheme ? 0xFF444444 : 0xFFDDE2EC;
        e.setOnFocusChangeListener((v, has) -> {
            android.graphics.drawable.GradientDrawable g2 = (android.graphics.drawable.GradientDrawable) e.getBackground();
            g2.setStroke(dp(has ? 2 : 1), has ? COLOR_PRIMARY : normalStroke);
        });
        if (darkTheme) e.setTextColor(0xFFE0E0E0);
        // 记录 variable——用户在触发按钮时由 collectVars 从这里读值
        rememberInput(node, e);
        // 重建时恢复输入框已录入内容（避免整树刷新后清空）
        String variable = node.optString("variable", "");
        if (!savedVals.isEmpty() && !variable.isEmpty()) {
            String saved = savedVals.get(variable);
            if (saved != null) {
                e.setText(saved);
                e.setSelection(e.length());   // 光标移到末尾
            }
        }
        return e;
    }

    private CheckBox checkView(JSONObject node) {
        CheckBox c = new CheckBox(ctx);
        c.setText(node.optString("标签", node.optString("label", "")));
        c.setTextColor(darkTheme ? 0xFFE0E0E0 : 0xFF1A1F2E);
        rememberInput(node, c);
        String variable = node.optString("variable", "");
        if (!savedVals.isEmpty() && !variable.isEmpty()) {
            String saved = savedVals.get(variable);
            if (saved != null) c.setChecked(Boolean.parseBoolean(saved));
        }
        return c;
    }

    private Switch switchView(JSONObject node) {
        Switch s = new Switch(ctx);
        s.setText(node.optString("标签", node.optString("label", "")));
        s.setTextColor(darkTheme ? 0xFFE0E0E0 : 0xFF1A1F2E);
        rememberInput(node, s);
        String variable = node.optString("variable", "");
        if (!savedVals.isEmpty() && !variable.isEmpty()) {
            String saved = savedVals.get(variable);
            if (saved != null) s.setChecked(Boolean.parseBoolean(saved));
        }
        return s;
    }

    /* ---------- 新控件：滑块 / 下拉框 / 图片 ---------- */

    private void sliderView(JSONObject node, ViewGroup parent) {
        final SeekBar sb = new SeekBar(ctx);
        sb.setMax(node.optInt("最大值", 100));
        String label = node.optString("标签", "");
        if (!label.isEmpty()) {
            sb.setContentDescription(label);
        }
        rememberInput(node, sb);
        // 重建时恢复滑块上次位置（避免点击其他控件后复位）
        String variable = node.optString("variable", "");
        if (!savedVals.isEmpty() && !variable.isEmpty()) {
            String saved = savedVals.get(variable);
            if (saved != null) {
                try { sb.setProgress((int) Math.round(Double.parseDouble(saved))); }
                catch (NumberFormatException ignored) { }
            }
        } else {
            sb.setProgress(node.optInt("值", 50));
        }
        parent.addView(sb);
    }

    private void spinnerView(JSONObject node, ViewGroup parent) {
        final Spinner sp = new Spinner(ctx);
        String label = node.optString("标签", "");
        if (!label.isEmpty()) {
            sp.setContentDescription(label);
        }
        List<String> options = new ArrayList<>();
        JSONArray opt = node.optJSONArray("选项");
        if (opt != null) {
            for (int i = 0; i < opt.length(); i++) {
                options.add(opt.optString(i));
            }
        }
        ArrayAdapter<String> adapter = new ArrayAdapter<>(ctx,
                android.R.layout.simple_spinner_dropdown_item, options);
        sp.setAdapter(adapter);
        rememberInput(node, sp);
        // 重建时恢复上次选中项（避免下拉被其他控件刷新复位到默认第一项）
        String variable = node.optString("variable", "");
        if (!savedVals.isEmpty() && !variable.isEmpty()) {
            String saved = savedVals.get(variable);
            if (saved != null) {
                int idx = options.indexOf(saved);
                if (idx >= 0) sp.setSelection(idx);
            }
        }
        parent.addView(sp);
    }

    private void imageView(JSONObject node, ViewGroup parent) {
        final ImageView iv = new ImageView(ctx);
        String src = node.optString("src", "");
        // 尝试从文件目录加载图片（extractAssets 已释放到 getFilesDir）
        if (!src.isEmpty()) {
            try {
                java.io.File imgFile = new java.io.File(ctx.getFilesDir(), src);
                if (imgFile.exists()) {
                    Bitmap bm = BitmapFactory.decodeFile(imgFile.getAbsolutePath());
                    if (bm != null) {
                        iv.setImageBitmap(bm);
                        iv.setAdjustViewBounds(true);
                        iv.setMaxWidth(dp(200));
                        iv.setMaxHeight(dp(200));
                        parent.addView(iv);
                        return;
                    }
                }
            } catch (Exception ignored) { }
        }
        // 加载失败则显示占位文字
        final TextView tv = new TextView(ctx);
        tv.setTextColor(darkTheme ? 0xFF888888 : 0xFF666666);
        tv.setText("[图片] " + src);
        tv.setGravity(Gravity.CENTER);
        tv.setPadding(dp(8), dp(16), dp(8), dp(16));
        parent.addView(tv);
        rememberInput(node, tv);
    }

    /* ---------- 新控件：课表（TableLayout 表格） ---------- */

    private void classTable(JSONObject node, ViewGroup parent) throws Exception {
        TableLayout tl = new TableLayout(ctx);
        tl.setStretchAllColumns(true);
        tl.setBackgroundColor(darkTheme ? 0xFF1E1E1E : 0xFFFFFFFF);
        tl.setPadding(dp(8), dp(8), dp(8), dp(8));

        String title = node.optString("标题", "课程表");
        TextView titleTv = TextView(ctx);
        titleTv.setText(title);
        titleTv.setTextSize(18);
        titleTv.setTypeface(Typeface.DEFAULT_BOLD);
        titleTv.setTextColor(darkTheme ? 0xFF8FB4FF : COLOR_PRIMARY);
        titleTv.setPadding(0, 0, 0, dp(12));
        tl.addView(titleTv);

        JSONArray weekdays = node.optJSONArray("星期");
        JSONArray periods = node.optJSONArray("节次");
        JSONArray classes = node.optJSONArray("课程");
        if (weekdays == null || periods == null || classes == null) return;

        int hdrBg = darkTheme ? 0xFF333333 : 0xFFE0E0E0;
        int cellText = darkTheme ? 0xFFE0E0E0 : 0xFF1A1F2E;

        TableRow headerRow = new TableRow(ctx);
        TextView corner = TextView(ctx);
        corner.setText(" ");
        headerRow.addView(corner);
        for (int j = 0; j < weekdays.length(); j++) {
            TextView tv = TextView(ctx);
            tv.setText(weekdays.optString(j));
            tv.setTypeface(Typeface.DEFAULT_BOLD);
            tv.setGravity(Gravity.CENTER);
            tv.setTextColor(darkTheme ? 0xFF8FB4FF : COLOR_PRIMARY);
            tv.setBackgroundColor(hdrBg);
            tv.setPadding(dp(4), dp(8), dp(4), dp(8));
            headerRow.addView(tv);
        }
        tl.addView(headerRow);

        for (int i = 0; i < periods.length(); i++) {
            TableRow row = new TableRow(ctx);
            TextView periodTv = TextView(ctx);
            periodTv.setText(periods.optString(i));
            periodTv.setTypeface(Typeface.DEFAULT_BOLD);
            periodTv.setGravity(Gravity.CENTER);
            periodTv.setTextColor(cellText);
            periodTv.setBackgroundColor(hdrBg);
            periodTv.setPadding(dp(4), dp(8), dp(4), dp(8));
            row.addView(periodTv);

            JSONArray dayClasses = classes.optJSONArray(i);
            if (dayClasses == null) continue;
            for (int j = 0; j < dayClasses.length(); j++) {
                TextView classTv = TextView(ctx);
                classTv.setText(dayClasses.optString(j));
                classTv.setGravity(Gravity.CENTER);
                classTv.setTextColor(cellText);
                classTv.setBackgroundColor((i + j) % 2 == 0
                        ? (darkTheme ? 0xFF2A2A2A : 0xFFF5F5F5)
                        : (darkTheme ? 0xFF242424 : 0xFFEEEEEE));
                classTv.setPadding(dp(4), dp(12), dp(4), dp(12));
                row.addView(classTv);
            }
            tl.addView(row);
        }
        parent.addView(tl);
    }

    /* ---------- 事件 / 变量工具 ---------- */

    private static String eventName(JSONObject node) {
        JSONObject e = node.optJSONObject("事件");
        if (e != null) return e.optString("事件名", null);
        JSONObject e2 = node.optJSONObject("event");
        if (e2 != null) return e2.optString("name", null);
        return node.optString("event", null);
    }

    private static JSONArray eventCollect(JSONObject node) {
        JSONObject e = node.optJSONObject("事件");
        if (e != null) return e.optJSONArray("收集变量");
        JSONObject e2 = node.optJSONObject("event");
        if (e2 != null) return e2.optJSONArray("collect");
        return null;
    }

    /** 收集按钮事件的变量参数，输出 JSON 对象字符串（如 {"星级":"1","金额":"1280"}）。
     *  收集变量：读输入控件当前值；回调变量：键=值 字面量参数（星级 1..5 等）。 */
    private String collectVars(JSONObject node) {
        StringBuilder sb = new StringBuilder("{");
        boolean first = true;
        JSONArray collect = eventCollect(node);
        if (collect != null) {
            for (int i = 0; i < collect.length(); i++) {
                String name = collect.optString(i);
                View v = inputs.get(name);
                if (v == null) continue;
                String val = inputVal(v);
                if (!first) sb.append(',');
                first = false;
                sb.append('"').append(escapeJs(name)).append("\":\"").append(escapeJs(val)).append('"');
            }
        }
        JSONObject e = node.optJSONObject("事件");
        if (e == null) e = node.optJSONObject("event");
        JSONArray cb = e != null ? e.optJSONArray("回调变量") : null;
        if (cb == null && e != null) cb = e.optJSONArray("callback");
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
    private String inputVal(View v) {
        if (v instanceof EditText) return ((EditText) v).getText().toString();
        if (v instanceof CheckBox) return String.valueOf(((CheckBox) v).isChecked());
        if (v instanceof Switch) return String.valueOf(((Switch) v).isChecked());
        if (v instanceof SeekBar) return String.valueOf(((SeekBar) v).getProgress());
        if (v instanceof Spinner) {
            Spinner sp = (Spinner) v;
            return sp.getSelectedItem() != null ? sp.getSelectedItem().toString() : "";
        }
        return "";
    }

    /** 重建前保存所有输入控件当前值，供重建后恢复（避免整树刷新导致控件复位）。 */
    private void saveInputs() {
        for (Map.Entry<String, View> e : inputs.entrySet()) {
            savedVals.put(e.getKey(), inputVal(e.getValue()));
        }
    }

    /** 触发后重建整棵 View（native 可能已换屏）。经 VuaBridge 合并请求，
     *  避免"换屏回调排队 + 本地立即刷新"两次全量重建。 */
    private void refresh() {
        VuaBridge.requestRender();
    }

    private static String escapeJs(String s) {
        return s == null ? "" : s.replace("\\", "\\\\").replace("\"", "\\\"");
    }

    /* ---------- 便捷工厂 ---------- */

    private TextView TextView(Context c) { TextView t = new TextView(c); return t; }
    private TextView TextView(Context c, String s) { TextView t = new TextView(c); t.setText(s); return t; }
    private int dp(float v) { return (int) (v * ctx.getResources().getDisplayMetrics().density); }
    private ViewGroup.LayoutParams matchWrap() {
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }
    private void rememberInput(JSONObject node, View v) {
        String id = node.optString("id", "");
        if (!id.isEmpty()) inputs.put(id, v);
        String variable = node.optString("variable", "");
        if (!variable.isEmpty()) { v.setTag("variable:" + variable); inputs.put(variable, v); }
    }
}