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
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

public final class VuaRenderer {

    private static final int COLOR_PRIMARY = 0xFF2962FF; // 主题主色（与 styles.xml colorAccent 一致）
    private static final int BG_LIGHT      = 0xFFF5F6FA; // 页面浅灰底（Material Light windowBackground）

    private final Context ctx;
    private final ViewGroup root;              // 把重建的 View 树放进这里
    private final Map<String, View> inputs;    // id -> 输入控件（供手机回填/取值）
    private final Map<String, String> savedVals; // variable/id -> 上次输入值（重建时恢复控件状态）
    private boolean darkTheme = false;

    public VuaRenderer(Context ctx, ViewGroup root) {
        this.ctx = ctx;
        this.root = root;
        this.inputs = new HashMap<>();
        this.savedVals = new HashMap<>();
    }

    /** 重建：清空根容器，把整棵树渲染进去。参数为 native 的渲染树 JSON 字符串。 */
    public void render(String renderTreeJson, String fallbackError) {
        saveInputs();   // 重建前先保存现有输入控件状态（下拉/滑块/输入框等）
        root.removeAllViews();
        if (renderTreeJson == null) {
            root.addView(TextView(ctx, fallbackError != null ? fallbackError : "(无渲染树)"));
            return;
        }
        try {
            JSONObject tree = new JSONObject(renderTreeJson);
            darkTheme = "dark".equalsIgnoreCase(tree.optString("主题", tree.optString("theme", "light")));
            root.setBackgroundColor(darkTheme ? 0xFF121212 : BG_LIGHT);
            buildInto(tree, root);
        } catch (Exception e) {
            String msg = "渲染失败: " + e.getMessage();
            root.addView(TextView(ctx, msg));
        }
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

    /** 触发后重建整棵 View（native 可能已换屏）。 */
    private void refresh() {
        String tree = VuaBridge.vuaRenderTree();
        render(tree, null);
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