/*
 * Controls.java — 叶子控件构建器（从 VuaRenderer 析出的控件工厂 + 分发表）
 *
 * VuaRenderer 聚焦渲染管线（缓存/diff/递归）；本类只做一件事：
 * 按渲染树节点 type 分发到对应控件的 View 构建，隔离"控件外观逻辑"。
 * 依赖 VuaRenderer 提供的渲染上下文（Context/密度/登记表/主题/图片缓存）。
 */
package com.vus.android;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.Typeface;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.webkit.JavascriptInterface;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.AbsListView;
import android.widget.ArrayAdapter;
import android.widget.BaseAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.ProgressBar;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TableLayout;
import android.widget.TableRow;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.List;

/** 按渲染树 type 构建叶子控件的 View。容器类（界面/行/侧边栏）由 VuaRenderer 负责。 */
final class Controls {

    private static final int DP_ICON_DEFAULT = 24;

    private final VuaRenderer r;

    Controls(VuaRenderer r) { this.r = r; }

    /* ==================== 叶子控件 ==================== */

    TextView textView(RenderNode node) {
        TextView t = r.TextView(r.ctx);
        t.setText(node.attr("", "内容", "value"));
        t.setTextColor(Theme.text(r.darkTheme));
        String style = node.attr("body", "样式", "style");
        float sp = 14; int styleB = Typeface.NORMAL;
        if ("title".equals(style)) { sp = 18; styleB = Typeface.BOLD; }
        else if ("headline".equals(style)) { sp = 22; styleB = Typeface.BOLD; }
        else if ("caption".equals(style)) { sp = 12; }
        t.setTextSize(sp); t.setTypeface(Typeface.DEFAULT, styleB);
        applyFont(node, t);
        if (styleB == Typeface.BOLD) t.setTextColor(Theme.accent(r.darkTheme));
        r.rememberInput(node, t, true);
        return t;
    }

    Button buttonView(final RenderNode node) {
        final Button b = new Button(r.ctx);
        b.setText(node.attr("确定", "文字", "text"));
        b.setAllCaps(false);
        if ("★".equals(b.getText().toString())) {
            // 星级评分「★」用幽灵按钮：透明底 + 主色大字（主题 Colored 样式会误填色块）
            b.setBackgroundColor(Color.TRANSPARENT);
            b.setTextSize(22);
            b.setTextColor(Theme.accent(r.darkTheme));
            b.setMinHeight(0);
        } else {
            b.setMinHeight(r.dp(44));
            b.setPadding(r.dp(18), 0, r.dp(18), 0);
            if (r.darkTheme) b.setTextColor(Theme.text(true));
        }
        final String evName = node.eventName();
        b.setOnClickListener(v -> {
            if ("检查更新".equals(evName)) {           // 检查更新：Java 侧直接处理
                if (VuaBridge.onCheckUpdate != null) VuaBridge.onCheckUpdate.run();
                return;
            }
            String vars = r.collectVars(node);
            if (evName != null) {
                VuaBridge.vuaTrigger(evName, vars);
            } else {
                VuaBridge.vuaTriggerById(node.id(), vars);
            }
            r.refresh();                                // 兜底：仅改变量未换屏的场景
        });
        r.rememberInput(node, b, false);
        return b;
    }

    EditText editView(RenderNode node, boolean multiline) {
        final boolean dark = r.darkTheme;
        final EditText e = new EditText(r.ctx);
        e.setInputType(multiline ? (InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE)
                : InputType.TYPE_CLASS_TEXT);
        e.setHint(node.attr("", "提示", "placeholder"));
        e.setSingleLine(!multiline);
        android.graphics.drawable.GradientDrawable eg = new android.graphics.drawable.GradientDrawable();
        eg.setColor(Theme.field(dark));
        eg.setCornerRadius(r.dp(10));
        eg.setStroke(r.dp(1), Theme.stroke(dark));
        e.setBackground(eg);
        e.setPadding(r.dp(12), r.dp(10), r.dp(12), r.dp(10));
        e.setMinHeight(r.dp(46));
        final int normalStroke = Theme.stroke(dark);
        e.setOnFocusChangeListener((v, has) -> {
            android.graphics.drawable.GradientDrawable g2 =
                    (android.graphics.drawable.GradientDrawable) e.getBackground();
            g2.setStroke(r.dp(has ? 2 : 1), has ? Theme.PRIMARY : normalStroke);
        });
        if (dark) e.setTextColor(Theme.text(true));
        r.rememberInput(node, e, false);
        restore(node, e);                               // 重建时恢复已录入内容
        return e;
    }

    CheckBox checkView(RenderNode node) {
        CheckBox c = new CheckBox(r.ctx);
        c.setText(node.attr("", "标签", "label"));
        c.setTextColor(Theme.text(r.darkTheme));
        r.rememberInput(node, c, false);
        restore(node, c);
        return c;
    }

    Switch switchView(RenderNode node) {
        Switch s = new Switch(r.ctx);
        s.setText(node.attr("", "标签", "label"));
        s.setTextColor(Theme.text(r.darkTheme));
        r.rememberInput(node, s, false);
        restore(node, s);
        return s;
    }

    /* ---- 进度条 / 分隔线 / 间距（新增叶子控件） ---- */

    /** 彩色字符串 → int（"0xRRGGBB"/"#RRGGBB"/系统色名），解析失败回退默认。 */
    private static int parseColor(String s, int def) {
        if (s == null || s.isEmpty()) return def;
        try {
            if (s.startsWith("0x") || s.startsWith("0X")) {
                return 0xFF000000 | (int) Long.parseLong(s.substring(2), 16);
            }
            return android.graphics.Color.parseColor(s);
        } catch (Throwable t) {
            return def;
        }
    }

    /** 进度条：值/最大值/颜色，可选右侧百分比文本（百分比=1 时显示）。 */
    void progressView(RenderNode node, ViewGroup parent) {
        int max = Math.max(1, node.intAttr(100, "最大值"));
        int val = Math.max(0, Math.min(max, node.intAttr(0, "值")));
        boolean showPct = node.intAttr(0, "百分比") != 0;
        LinearLayout row = new LinearLayout(r.ctx);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        ProgressBar pb = new ProgressBar(r.ctx, null, android.R.attr.progressBarStyleHorizontal);
        pb.setMax(max);
        pb.setProgress(val);
        String lv = node.attr("", "标签");
        if (!lv.isEmpty()) pb.setContentDescription(lv);
        if (Build.VERSION.SDK_INT >= 21) {
            pb.setProgressTintList(android.content.res.ColorStateList
                    .valueOf(parseColor(node.attr("", "颜色"), Theme.accent(r.darkTheme))));
            pb.setProgressBackgroundTintList(android.content.res.ColorStateList
                    .valueOf(Theme.stroke(r.darkTheme)));
        }
        row.addView(pb, new LinearLayout.LayoutParams(0, r.dp(10), 1f));
        if (showPct) {
            TextView tv = r.TextView(r.ctx);
            tv.setText(Math.round(val * 100f / max) + "%");
            tv.setTextColor(Theme.sub(r.darkTheme));
            tv.setTextSize(12);
            tv.setPadding(r.dp(8), 0, 0, 0);
            row.addView(tv);
        }
        r.rememberInput(node, row, false);
        parent.addView(row, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
    }

    /** 分隔线：颜色/粗细(px dp)/上下外边距(dp)。 */
    void dividerView(RenderNode node, ViewGroup parent) {
        View line = new View(r.ctx);
        int th = Math.max(1, node.intAttr(1, "粗细"));
        line.setBackgroundColor(parseColor(node.attr("", "颜色"), Theme.divider(r.darkTheme)));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, r.dp(th));
        int top = node.intAttr(0, "上边距");
        int bottom = node.intAttr(0, "下边距");
        if (top != 0 || bottom != 0) lp.setMargins(0, r.dp(top), 0, r.dp(bottom));
        parent.addView(line, lp);
    }

    /** 间距：宽/高(dp) 的空白占位（不可见）。 */
    void spaceView(RenderNode node, ViewGroup parent) {
        View sp = new View(r.ctx);
        sp.setVisibility(View.INVISIBLE);
        int w = node.intAttr(0, "宽");
        int h = node.intAttr(0, "高");
        parent.addView(sp, new LinearLayout.LayoutParams(
                w > 0 ? r.dp(w) : 0, h > 0 ? r.dp(h) : r.dp(1)));
    }

    void sliderView(RenderNode node, ViewGroup parent) {
        final SeekBar sb = new SeekBar(r.ctx);
        sb.setMax(node.intAttr(100, "最大值"));
        String label = node.attr("", "标签");
        if (!label.isEmpty()) sb.setContentDescription(label);
        r.rememberInput(node, sb, false);
        String variable = node.variable();
        // 恢复上次位置；无历史用节点"值"（默认 50）
        if (!variable.isEmpty() && r.hasSaved(variable)) {
            String saved = r.savedVal(variable);
            try { sb.setProgress((int) Math.round(Double.parseDouble(saved))); }
            catch (NumberFormatException ignored) { }
        } else {
            sb.setProgress(node.intAttr(50, "值"));
        }
        parent.addView(sb);
    }

    void spinnerView(RenderNode node, ViewGroup parent) {
        final Spinner sp = new Spinner(r.ctx);
        String label = node.attr("", "标签");
        if (!label.isEmpty()) sp.setContentDescription(label);
        List<String> options = new ArrayList<>();
        JSONArray opt = node.arr("选项");
        if (opt != null) {
            for (int i = 0; i < opt.length(); i++) options.add(opt.optString(i));
        }
        sp.setAdapter(new ArrayAdapter<String>(r.ctx,
                android.R.layout.simple_spinner_dropdown_item, options));
        r.rememberInput(node, sp, false);
        String variable = node.variable();
        if (!variable.isEmpty() && r.hasSaved(variable)) {
            int idx = options.indexOf(r.savedVal(variable));
            if (idx >= 0) sp.setSelection(idx);
        }
        parent.addView(sp);
    }

    void imageView(RenderNode node, ViewGroup parent) {
        final ImageView iv = new ImageView(r.ctx);
        String src = node.attr("", "src", "地址");

        if (src.startsWith("http://") || src.startsWith("https://")) {
            iv.setScaleType(ImageView.ScaleType.FIT_CENTER);
            iv.setPadding(r.dp(4), r.dp(4), r.dp(4), r.dp(4));
            int w = node.intAttr(0, "宽"), h = node.intAttr(0, "高");
            if (w <= 0) w = r.dp(160);
            if (h <= 0) h = r.dp(160);
            LinearLayout.LayoutParams lp =
                    new LinearLayout.LayoutParams(w, h, node.intAttr(0, "权重"));
            lp.gravity = Gravity.CENTER;
            parent.addView(iv, lp);
            ImageLoader.get().load(src, iv);
            r.rememberInput(node, iv, false);
            return;
        }

        if (!src.isEmpty()) {
            try {
                java.io.File imgFile = new java.io.File(r.ctx.getFilesDir(), src);
                if (imgFile.exists()) {
                    Bitmap bm = r.loadLocalBitmap(imgFile.getAbsolutePath());
                    if (bm != null) {
                        iv.setImageBitmap(bm);
                        iv.setAdjustViewBounds(true);
                        iv.setMaxWidth(r.dp(200));
                        iv.setMaxHeight(r.dp(200));
                        parent.addView(iv);
                        return;
                    }
                }
            } catch (Exception ignored) { }
        }
        TextView tv = r.TextView(r.ctx);            // 加载失败占位
        tv.setTextColor(Theme.muted(r.darkTheme));
        tv.setText("[图片] " + src);
        tv.setGravity(Gravity.CENTER);
        tv.setPadding(r.dp(8), r.dp(16), r.dp(8), r.dp(16));
        parent.addView(tv);
        r.rememberInput(node, tv, false);
    }

    /** 图标：filesDir 下命名位图（png/jpg/webp）。 */
    void iconView(RenderNode node, ViewGroup parent) {
        String name = node.attr("", "名称", "src", "icon");
        ImageView iv = new ImageView(r.ctx);
        int w = node.intAttr(0, "宽");
        int h = node.intAttr(0, "高");
        if (w <= 0) w = r.dp(DP_ICON_DEFAULT);
        if (h <= 0) h = r.dp(DP_ICON_DEFAULT);
        if (!name.isEmpty()) {
            try {
                java.io.File f = new java.io.File(r.ctx.getFilesDir(), name);
                if (f.isFile()) {
                    Bitmap bm = r.loadLocalBitmap(f.getAbsolutePath());
                    if (bm != null) {
                        iv.setImageBitmap(bm);
                        iv.setContentDescription(node.attr(name, "标签"));
                        parent.addView(iv, new LinearLayout.LayoutParams(w, h));
                        r.rememberInput(node, iv, false);
                        return;
                    }
                }
            } catch (Exception ignored) { }
        }
        TextView tv = r.TextView(r.ctx, "[图标 " + name + "]");  // 占位
        tv.setTextColor(Theme.muted(r.darkTheme));
        tv.setGravity(Gravity.CENTER);
        parent.addView(tv);
        r.rememberInput(node, tv, false);
    }

    /** 自定义字体：节点 "字体"/"font" 指定 filesDir 下的 .ttf/.otf；失败静默回退。 */
    void applyFont(RenderNode node, TextView tv) {
        String font = node.attr("", "字体", "font");
        if (font.isEmpty()) return;
        try {
            java.io.File f = new java.io.File(r.ctx.getFilesDir(), font);
            if (f.isFile()) {
                Typeface tf = Typeface.createFromFile(f.getAbsolutePath());
                if (tf != null) tv.setTypeface(tf, Typeface.NORMAL);
            }
        } catch (Exception ignored) { }
    }

    /* ==================== 列表 ==================== */

    /** 列表：ListView 虚拟化 + 分组头 + convertView 复用 + 滚动到底"加载更多"。 */
    void listView(final RenderNode node, final ViewGroup parent) {
        final JSONArray data = node.arr("数据");
        if (data == null || data.length() == 0) {
            parent.addView(r.TextView(r.ctx, "(空列表)"));
            return;
        }
        final int rows = data.length();
        final String evName = node.eventName();
        final String loadMore = node.attr("", "加载更多", "onReachEnd");
        final int titleColor = Theme.text(r.darkTheme);
        final int subColor   = Theme.sub(r.darkTheme);

        final int titleId = View.generateViewId();
        final int subId   = View.generateViewId();

        final ListView lv = new ListView(r.ctx);
        lv.setDivider(new android.graphics.drawable.ColorDrawable(Theme.divider(r.darkTheme)));
        lv.setDividerHeight(r.dp(1));
        lv.setBackgroundColor(Theme.rowBg(r.darkTheme));

        lv.setAdapter(new BaseAdapter() {
            public int getCount() { return rows; }
            public Object getItem(int pos) { return data.opt(pos); }
            public long getItemId(int pos) { return pos; }
            @Override public int getViewTypeCount() { return 2; }
            @Override public int getItemViewType(int pos) {
                Object it = data.opt(pos);
                return (it instanceof JSONObject
                        && !((JSONObject) it).optString("节标题", "").isEmpty()) ? 1 : 0;
            }
            public View getView(int pos, View convertView, ViewGroup p) {
                Object it = data.opt(pos);
                if (getItemViewType(pos) == 1) {           // 分组头行
                    TextView h;
                    if (convertView == null) {
                        h = new TextView(r.ctx);
                        h.setTextSize(13);
                        h.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
                        h.setTextColor(titleColor);
                        h.setPadding(r.dp(12), r.dp(18), r.dp(12), r.dp(4));
                    } else {
                        h = (TextView) convertView;
                    }
                    h.setText(((JSONObject) it).optString("节标题", ""));
                    h.setTag(pos);
                    return h;
                }
                final LinearLayout row;
                final TextView titleTv, subTv;
                final ImageView imgV;
                if (convertView == null) {
                    row = new LinearLayout(r.ctx);
                    row.setOrientation(LinearLayout.HORIZONTAL);
                    row.setGravity(Gravity.CENTER_VERTICAL);
                    row.setPadding(r.dp(12), r.dp(10), r.dp(12), r.dp(10));
                    row.setMinimumHeight(r.dp(52));
                    imgV = new ImageView(r.ctx);
                    imgV.setScaleType(ImageView.ScaleType.FIT_CENTER);
                    imgV.setVisibility(View.GONE);
                    row.addView(imgV, new LinearLayout.LayoutParams(r.dp(44), r.dp(44)));
                    LinearLayout texts = new LinearLayout(r.ctx);
                    texts.setOrientation(LinearLayout.VERTICAL);
                    titleTv = new TextView(r.ctx);
                    titleTv.setId(titleId);
                    titleTv.setTextSize(15);
                    titleTv.setSingleLine(true);
                    titleTv.setTextColor(titleColor);
                    subTv = new TextView(r.ctx);
                    subTv.setId(subId);
                    subTv.setTextSize(12);
                    subTv.setSingleLine(true);
                    subTv.setVisibility(View.GONE);
                    subTv.setTextColor(subColor);
                    texts.addView(titleTv);
                    texts.addView(subTv);
                    row.addView(texts, new LinearLayout.LayoutParams(
                            0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
                    convertView = row;
                } else {
                    row = (LinearLayout) convertView;
                    titleTv = (TextView) convertView.findViewById(titleId);
                    subTv = (TextView) convertView.findViewById(subId);
                    imgV = (ImageView) ((ViewGroup) convertView).getChildAt(0);
                }

                Object item = data.opt(pos);                 // 数据项：字符串或 {标题,副标题,图片}
                String title = item instanceof String ? (String) item : "";
                String sub = "", img = "";
                if (item instanceof JSONObject) {
                    JSONObject o = (JSONObject) item;
                    title = o.optString("标题", o.optString("text", o.optString("title", "")));
                    sub = o.optString("副标题", o.optString("sub", o.optString("summary", "")));
                    img = o.optString("图片", o.optString("src", ""));
                }
                titleTv.setText(title);
                if (sub.isEmpty()) {
                    subTv.setVisibility(View.GONE);
                } else {
                    subTv.setVisibility(View.VISIBLE);
                    subTv.setText(sub);
                }
                if (img.startsWith("http://") || img.startsWith("https://")) {
                    imgV.setVisibility(View.VISIBLE);
                    ImageLoader.get().load(img, imgV);
                } else {
                    imgV.setVisibility(View.GONE);
                }
                row.setTag(pos);
                return convertView;
            }
        });

        lv.setOnItemClickListener((a, v, pos, id) -> {
            Object item = data.opt(pos);
            if (item instanceof JSONObject
                    && !((JSONObject) item).optString("节标题", "").isEmpty()) return;
            String title = item instanceof String ? (String) item
                    : item instanceof JSONObject ? ((JSONObject) item).optString("标题", "") : "";
            StringBuilder vb = new StringBuilder("{");
            vb.append("\"下标\":").append(pos)
              .append(",\"值\":\"").append(VuaRenderer.escapeJs(title)).append("\"");
            String base = r.collectVars(node);
            if (base.length() > 2) {
                vb.append(',').append(base.substring(1, base.length() - 1));
            }
            vb.append('}');
            if (evName != null) {
                VuaBridge.vuaTrigger(evName, vb.toString());
            } else {
                VuaBridge.vuaTriggerById(node.id(), vb.toString());
            }
            r.refresh();
        });

        if (!loadMore.isEmpty()) {                       // 滚动到底 → 加载更多事件
            final boolean[] loading = {false};
            lv.setOnScrollListener(new AbsListView.OnScrollListener() {
                public void onScrollStateChanged(AbsListView view, int state) { }
                public void onScroll(AbsListView view, int firstVisible,
                                     int visibleCount, int totalCount) {
                    if (totalCount > 0
                            && firstVisible + visibleCount >= totalCount && !loading[0]) {
                        loading[0] = true;
                        VuaBridge.vuaTrigger(loadMore, "{}");
                    }
                }
            });
        }

        int rowH = r.dp(54);
        int maxH = r.dp(node.intAttr(480, "高度"));
        parent.addView(lv, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, Math.min(rows * rowH, maxH)));
    }

    /* ==================== 课表 ==================== */

    void classTable(RenderNode node, ViewGroup parent) throws Exception {
        final boolean dark = r.darkTheme;
        TableLayout tl = new TableLayout(r.ctx);
        tl.setStretchAllColumns(true);
        tl.setBackgroundColor(Theme.rowBg(dark));
        tl.setPadding(r.dp(8), r.dp(8), r.dp(8), r.dp(8));

        TextView titleTv = r.TextView(r.ctx, node.attr("课程表", "标题"));
        titleTv.setTextSize(18);
        titleTv.setTypeface(Typeface.DEFAULT_BOLD);
        titleTv.setTextColor(Theme.accent(dark));
        titleTv.setPadding(0, 0, 0, r.dp(12));
        tl.addView(titleTv);

        JSONArray weekdays = node.arr("星期");
        JSONArray periods = node.arr("节次");
        JSONArray classes = node.arr("课程");
        if (weekdays == null || periods == null || classes == null) return;

        int hdrBg = dark ? Theme.ROW_HDR_DARK : Theme.ROW_HDR_LIGHT;
        int cellText = Theme.text(dark);

        TableRow headerRow = new TableRow(r.ctx);
        TextView corner = r.TextView(r.ctx, " ");
        headerRow.addView(corner);
        for (int j = 0; j < weekdays.length(); j++) {
            TextView tv = r.TextView(r.ctx);
            tv.setText(weekdays.optString(j));
            tv.setTypeface(Typeface.DEFAULT_BOLD);
            tv.setGravity(Gravity.CENTER);
            tv.setTextColor(Theme.accent(dark));
            tv.setBackgroundColor(hdrBg);
            tv.setPadding(r.dp(4), r.dp(8), r.dp(4), r.dp(8));
            headerRow.addView(tv);
        }
        tl.addView(headerRow);

        for (int i = 0; i < periods.length(); i++) {
            TableRow row = new TableRow(r.ctx);
            TextView periodTv = r.TextView(r.ctx);
            periodTv.setText(periods.optString(i));
            periodTv.setTypeface(Typeface.DEFAULT_BOLD);
            periodTv.setGravity(Gravity.CENTER);
            periodTv.setTextColor(cellText);
            periodTv.setBackgroundColor(hdrBg);
            periodTv.setPadding(r.dp(4), r.dp(8), r.dp(4), r.dp(8));
            row.addView(periodTv);

            JSONArray dayClasses = classes.optJSONArray(i);
            if (dayClasses == null) continue;
            for (int j = 0; j < dayClasses.length(); j++) {
                TextView classTv = r.TextView(r.ctx);
                classTv.setText(dayClasses.optString(j));
                classTv.setGravity(Gravity.CENTER);
                classTv.setTextColor(cellText);
                classTv.setBackgroundColor((i + j) % 2 == 0
                        ? (dark ? Theme.ROW_ALT_DARK : Theme.ROW_ALT_LIGHT)
                        : (dark ? Theme.ROW_EVEN_DARK : Theme.ROW_EVEN_LIGHT));
                classTv.setPadding(r.dp(4), r.dp(12), r.dp(4), r.dp(12));
                row.addView(classTv);
            }
            tl.addView(row);
        }
        parent.addView(tl);
    }

    /* ==================== 网页 / Markdown ==================== */

    /** 网页/富文本：url 直接加载；html 原样渲染；内容按 Markdown 转 HTML。 */
    void webView(RenderNode node, ViewGroup parent) {
        final WebView wv = new WebView(r.ctx);
        WebSettings ws = wv.getSettings();
        ws.setJavaScriptEnabled(true);
        ws.setDomStorageEnabled(true);
        try { ws.setMixedContentMode(WebSettings.MIXED_CONTENT_ALWAYS_ALLOW); } catch (Exception ignored) { }

        wv.addJavascriptInterface(new Object() {       // JS → VUS 事件
            @JavascriptInterface
            public void onEvent(final String name, final String json) {
                com.vus.android.VuaBridge.postToTrigger(name, json);
            }
            @JavascriptInterface
            public void triggerById(final String id, final String json) {
                com.vus.android.VuaBridge.postToTriggerById(id, json);
            }
        }, "vus");

        wv.setWebViewClient(new WebViewClient() {       // 链接点击 → VUS 事件
            @Override
            public boolean shouldOverrideUrlLoading(WebView view, String url) {
                if (url == null) return false;
                if (url.startsWith("#")) return false;
                VuaBridge.postToTrigger("链接点击",
                        "{\"url\":\"" + VuaRenderer.escapeJs(url) + "\"}");
                return true;
            }
        });

        parent.addView(wv, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, r.dp(node.intAttr(480, "高度"))));

        String url = node.attr("", "url");
        String html = node.attr("", "html");
        String content = node.attr("", "内容", "markdown", "text");
        try {
            if (!url.isEmpty()) {
                wv.loadUrl(url);
            } else if (!html.isEmpty()) {
                wv.loadDataWithBaseURL(null, html, "text/html", "UTF-8", null);
            } else if (!content.isEmpty()) {
                wv.loadDataWithBaseURL(null, mdToHtml(content), "text/html", "UTF-8", null);
            }
        } catch (Exception ignored) { }
    }

    /** 极简 Markdown → HTML（无第三方库，覆盖常见语法，来自原 VuaRenderer）。 */
    static String mdToHtml(String md) {
        StringBuilder out = new StringBuilder();
        out.append("<html><body style='padding:12px;margin:0;font-family:sans-serif;"
                + "color:#222;line-height:1.6'><div>");
        boolean inCode = false, inUl = false;
        String[] lines = md.split("\n");
        for (String raw : lines) {
            String line = raw.trim();
            if (line.startsWith("```")) {
                if (inCode) { out.append("</code></pre>"); inCode = false; }
                else { out.append("<pre><code>"); inCode = true; }
                continue;
            }
            if (inCode) { out.append(esc(line)).append("\n"); continue; }
            if (line.isEmpty()) { out.append("</div><div>"); continue; }
            if (line.startsWith("### ")) { out.append("<h3>").append(inline(line.substring(4))).append("</h3>"); continue; }
            if (line.startsWith("## "))  { out.append("<h2>").append(inline(line.substring(3))).append("</h2>"); continue; }
            if (line.startsWith("# "))   { out.append("<h1>").append(inline(line.substring(2))).append("</h1>"); continue; }
            if (line.startsWith("- ") || line.startsWith("* ")) {
                if (!inUl) { out.append("<ul>"); inUl = true; }
                out.append("<li>").append(inline(line.substring(2))).append("</li>");
                continue;
            }
            if (line.startsWith("> ")) { out.append("<blockquote>").append(inline(line.substring(2))).append("</blockquote>"); continue; }
            if (inUl) { out.append("</ul>"); inUl = false; }
            out.append("<p>").append(inline(line)).append("</p>");
        }
        if (inUl) out.append("</ul>");
        if (inCode) out.append("</code></pre>");
        out.append("</div></body></html>");
        return out.toString();
    }

    private static String inline(String s) {
        String r2 = esc(s);
        r2 = r2.replaceAll("\\*\\*(.+?)\\*\\*", "<b>$1</b>");
        r2 = r2.replaceAll("\\*(.+?)\\*", "<i>$1</i>");
        r2 = r2.replaceAll("\\[([^]]+)\\]\\(([^)]+)\\)", "<a href='$2'>$1</a>");
        return r2;
    }

    private static String esc(String s) {
        return s == null ? "" : s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;");
    }

    /* ==================== 状态恢复 ==================== */

    /** 重建时恢复输入控件已录入值（EditText 文本 / 勾选状态）。 */
    private void restore(RenderNode node, View v) {
        String variable = node.variable();
        if (variable.isEmpty() || !r.hasSaved(variable)) return;
        String saved = r.savedVal(variable);
        if (v instanceof EditText) {
            ((EditText) v).setText(saved);
            ((EditText) v).setSelection(((EditText) v).length());   // 光标移到末尾
        } else if (v instanceof CheckBox) {
            ((CheckBox) v).setChecked(Boolean.parseBoolean(saved));
        } else if (v instanceof Switch) {
            ((Switch) v).setChecked(Boolean.parseBoolean(saved));
        }
    }
}