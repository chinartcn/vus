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
import android.graphics.Color;
import android.graphics.Typeface;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.CheckBox;
import android.widget.Switch;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

public final class VuaRenderer {

    private final Context ctx;
    private final ViewGroup root;              // 把重建的 View 树放进这里
    private final Map<String, View> inputs;    // id -> 输入控件（供手机回填/取值）
    private boolean darkTheme = false;

    public VuaRenderer(Context ctx, ViewGroup root) {
        this.ctx = ctx;
        this.root = root;
        this.inputs = new HashMap<>();
    }

    /** 重建：清空根容器，把整棵树渲染进去。参数为 native 的渲染树 JSON 字符串。 */
    public void render(String renderTreeJson, String fallbackError) {
        root.removeAllViews();
        if (renderTreeJson == null) {
            root.addView(TextView(ctx, fallbackError != null ? fallbackError : "(无渲染树)"));
            return;
        }
        try {
            JSONObject tree = new JSONObject(renderTreeJson);
            darkTheme = "dark".equalsIgnoreCase(tree.optString("主题", tree.optString("theme", "light")));
            root.setBackgroundColor(darkTheme ? 0xFF121212 : Color.WHITE);
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

    private void layout(JSONObject node, ViewGroup parent, boolean vert) throws Exception {
        LinearLayout ll = makeLayout(vert);
        parent.addView(ll, matchWrap());
        JSONArray ch = node.optJSONArray("children");
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
        t.setTextColor(0xFF1A1F2E);
        String style = node.optString("样式", node.optString("style", "body"));
        float sp = 14; int styleB = Typeface.NORMAL;
        switch (style) {
            case "title": sp = 18; styleB = Typeface.BOLD; break;
            case "headline": sp = 22; styleB = Typeface.BOLD; break;
            case "caption": sp = 12; break;
            default: sp = 14;
        }
        t.setTextSize(sp); t.setTypeface(Typeface.DEFAULT, styleB);
        rememberInput(node, t);
        return t;
    }

    private Button buttonView(final JSONObject node) {
        final Button b = new Button(ctx);
        b.setText(node.optString("文字", node.optString("text", "确定")));
        b.setAllCaps(false);
        // 存事件名/id，供点击时派发
        final String evName = eventName(node);
        final JSONArray collect = eventCollect(node);
        b.setOnClickListener(v -> {
            String vars = collectVars(collect);
            if (evName != null) {
                VuaBridge.vuaTrigger(evName, vars);
            } else {
                VuaBridge.vuaTriggerById(node.optString("id"), vars);
            }
            // native 若换屏会经 vua_notify_rerender 回调 onNativeRerender 触发重建；
            // 这里再主动刷一次，兜底事件 handler 只是改了变量、没有换屏的场景。
            refresh();
        });
        return b;
    }

    private EditText editView(JSONObject node, boolean multiline) {
        final EditText e = new EditText(ctx);
        e.setInputType(multiline ? (InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE)
                                 : InputType.TYPE_CLASS_TEXT);
        e.setHint(node.optString("提示", node.optString("placeholder", "")));
        e.setSingleLine(!multiline);
        // 记录 variable——用户在触发按钮时由 collectVars 从这里读值
        String variable = node.optString("variable");
        if (!variable.isEmpty()) {
            e.setTag("variable:" + variable);
            inputs.put(variable, e);
        }
        String id = node.optString("id");
        if (!id.isEmpty()) inputs.put(id, e);
        return e;
    }

    private CheckBox checkView(JSONObject node) {
        CheckBox c = new CheckBox(ctx);
        c.setText(node.optString("标签", node.optString("label", "")));
        String variable = node.optString("variable");
        if (!variable.isEmpty()) {
            c.setTag("variable:" + variable);
            inputs.put(variable, c);
        }
        return c;
    }

    private Switch switchView(JSONObject node) {
        Switch s = new Switch(ctx);
        s.setText(node.optString("标签", node.optString("label", "")));
        String variable = node.optString("variable");
        if (!variable.isEmpty()) {
            s.setTag("variable:" + variable);
            inputs.put(variable, s);
        }
        return s;
    }

    /* ---------- 事件 / 变量工具 ---------- */

    private static String eventName(JSONObject node) {
        JSONObject e = node.optJSONObject("event");
        if (e != null) return e.optString("name", null);
        return node.optString("event", null);
    }

    private static JSONArray eventCollect(JSONObject node) {
        JSONObject e = node.optJSONObject("event");
        if (e != null) return e.optJSONArray("collect");
        return null;
    }

    /** 从 collect 变量表收集当前输入值，输出 JSON 对象字符串（如 {"金额":"1280"}）。 */
    private String collectVars(JSONArray collect) {
        if (collect == null) return "{}";
        StringBuilder sb = new StringBuilder("{");
        boolean first = true;
        for (int i = 0; i < collect.length(); i++) {
            String name = collect.optString(i);
            View v = inputs.get(name);
            if (v == null) continue;
            String val;
            if (v instanceof EditText) val = ((EditText) v).getText().toString();
            else if (v instanceof CheckBox) val = String.valueOf(((CheckBox) v).isChecked());
            else if (v instanceof Switch) val = String.valueOf(((Switch) v).isChecked());
            else val = "";
            if (!first) sb.append(',');
            first = false;
            sb.append('"').append(escapeJs(name)).append("\":\"").append(escapeJs(val)).append('"');
        }
        return sb.append('}').toString();
    }

    /** 触发后重建整棵 View（native 可能已换屏）。 */
    private void refresh() {
        String tree = VuaBridge.vuaRenderTree();
        render(tree, null);
    }

    private static String escapeJs(String s) {
        return s == null ? "" : s.replace("\\", "\\\\").replace("\"", "\\\"");
    }

    private static String eventIndexLookup(JSONObject tree, String id, String key) {
        JSONObject idx = tree.optJSONObject("eventIndex");
        JSONObject ev = idx != null ? idx.optJSONObject(id) : null;
        return (ev != null) ? ev.optString(key, null) : null;
    }

    /* ---------- 便捷工厂 ---------- */

    private TextView TextView(Context c) { TextView t = new TextView(c); return t; }
    private TextView TextView(Context c, String s) { TextView t = new TextView(c); t.setText(s); return t; }
    private TextView textView(String s) { TextView t = new TextView(ctx); t.setText(s); return t; }
    private int dp(float v) { return (int) (v * ctx.getResources().getDisplayMetrics().density); }
    private ViewGroup.LayoutParams matchWrap() {
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }
    private void rememberInput(JSONObject node, View v) {
        String id = node.optString("id");
        if (!id.isEmpty()) inputs.put(id, v);
        String variable = node.optString("variable");
        if (!variable.isEmpty()) { v.setTag("variable:" + variable); inputs.put(variable, v); }
    }
}