/*
 * RenderNode.java — 渲染树节点 DTO（收敛 JSON 遍历样板）
 *
 * 渲染树节点只有六个原语键（type/id/children/variable/event），其余是控件属性。
 * 本类把"取文本属性（中/英多键回退 + 默认值）""取子组件数组""字符串快照"等
 * 高频操作收敛为单一入口，替换 VuaRenderer 里散落的 getJSONObject/optString 遍历。
 * 仅包装 org.json，不改变渲染树协议。
 */
package com.vus.android;

import org.json.JSONArray;
import org.json.JSONObject;

final class RenderNode {

    private final JSONObject o;

    RenderNode(JSONObject o) { this.o = o == null ? new JSONObject() : o; }

    /** 底层对象（列表数据项等仍需原始 JSON 的场景）。 */
    JSONObject raw() { return o; }

    String type() { return o.optString("type", ""); }
    String id() { return o.optString("id", ""); }
    String variable() { return o.optString("variable", ""); }

    /** 字符串属性：按顺序取第一个存在的键，全部缺失用 def（如 "内容"→"value"→def）。 */
    String attr(String def, String... keys) {
        for (String k : keys) {
            if (o.has(k)) return o.optString(k);
        }
        return def;
    }

    /** 整数属性：键序同上，缺失用 def。 */
    int intAttr(int def, String... keys) {
        for (String k : keys) {
            if (o.has(k)) return o.optInt(k, def);
        }
        return def;
    }

    boolean has(String key) { return o.has(key); }

    /** 数组属性：键序同上，缺失返回 null。 */
    JSONArray arr(String... keys) {
        for (String k : keys) {
            if (o.has(k)) return o.optJSONArray(k);
        }
        return null;
    }

    /* ==================== 子组件 ==================== */

    int childCount() {
        JSONArray ch = childrenArray();
        return ch == null ? 0 : ch.length();
    }

    RenderNode child(int i) {
        JSONArray ch = childrenArray();
        if (ch == null) return null;
        JSONObject c = ch.optJSONObject(i);
        return c == null ? null : new RenderNode(c);
    }

    private JSONArray childrenArray() {
        JSONArray cn = o.optJSONArray("children");
        if (cn != null) return cn;
        return o.optJSONArray("子组件");
    }

    /** 子树指纹（id 复用缓存判重用）。 */
    String snapshot() { return o.toString(); }

    /* ==================== 事件 / 回调变量 ==================== */

    /** 事件名：事件.事件名 → event.name → 裸 event，均无返回 null。 */
    String eventName() {
        JSONObject e = eventObj();
        if (e != null) return e.optString("事件名", null);
        return o.optString("event", null);
    }

    /** 收集变量名数组（触发时从输入控件的当前值读取）。 */
    JSONArray collectVarNames() {
        JSONObject e = eventObj();
        if (e == null) return null;
        JSONArray a = e.optJSONArray("收集变量");
        return a != null ? a : e.optJSONArray("collect");
    }

    /** 回调变量名数组（键=值 字面量参数，如 "星级=3"）。 */
    JSONArray callbackVarNames() {
        JSONObject e = eventObj();
        if (e == null) return null;
        JSONArray a = e.optJSONArray("回调变量");
        return a != null ? a : e.optJSONArray("callback");
    }

    private JSONObject eventObj() {
        JSONObject e = o.optJSONObject("事件");
        return e != null ? e : o.optJSONObject("event");
    }
}