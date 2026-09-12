/*
 * VusSession.java — 会话对象（Cordis_dc §3.3，M1 首个落地物）
 *
 * 统一持有「逻辑版本 / 当前页 / 状态 / 宿主绑定」的单一容器，替代三张散装状态表：
 *   - 逻辑版本：UpdateManager.currentVersion()（对应已应用 .so/.vus 版本）
 *   - 当前页：activePage（当前 .vua 页，由宿主渲染流程 noteActivePage 记录）
 *   - 状态：渲染器输入控件值（VuaRenderer.savedVals，可序列化）
 *   - 宿主绑定：bindings（描述当前来自哪个 v_mod/manifest，预留）
 *
 * 状态快照必须可序列化（红线 §6-6）：.so 换版本后旧 native 堆湮灭，
 * 跨进程状态只能存宿主层 JSON。生命周期钩子（Listener）供热更高低层订阅。
 */
package com.vus.android;

import android.util.Log;

import com.google.gson.Gson;
import com.google.gson.JsonObject;

import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

public final class VusSession {

    /** 会话生命周期钩子：挂载 / 变更 / 卸载（Cordis_dc §3.3-3）。 */
    public interface Listener {
        void onChanged(String reason);
    }

    private static final Gson GSON = new Gson();
    private static final String TAG = "VusSession";
    private static VusSession sInst;

    private long startedAt = System.currentTimeMillis();
    private long seq = 1;
    private String activePage = "";
    private String bindings = "";                 // 宿主绑定描述（来自哪个 v_mod/manifest）
    private volatile RenderAdapter renderer = null; // 可选：快照时拉取输入控件值（M5：依赖接口非实现）
    private final List<Listener> listeners = new ArrayList<>();

    private VusSession() { }

    /** MainActivity.onCreate 最早调用（幂等）。 */
    public static synchronized void init() {
        if (sInst == null) sInst = new VusSession();
    }

    /** 会话句柄；未 init（如桌面/测试）返回 null，调用方自行判空。 */
    public static VusSession get() { return sInst; }

    /* ==================== 挂载 / 生命周期 ==================== */

    /** 绑定当前渲染器（重建时先 deattach 再 attach 新实例）。 */
    public void attachRenderer(RenderAdapter r) {
        renderer = r;
        notifyChanged("attachRenderer");
    }

    public void deattachRenderer() {
        renderer = null;
        notifyChanged("deattachRenderer");
    }

    public void addListener(Listener l) { if (l != null && !listeners.contains(l)) listeners.add(l); }

    /** 通知所有订阅方：生命周期/状态已变更（如热更事务推进）。 */
    public void notifyChanged(String reason) {
        for (Listener l : listeners) {
            try { l.onChanged(reason); } catch (Throwable ignored) { }
        }
    }

    /* ==================== 会话字段 ==================== */

    public long startedAt() { return startedAt; }
    public long seq() { return ++seq; }

    /** 记录当前 .vua 页（由宿主渲染流程调用）。 */
    public void noteActivePage(String page) {
        activePage = (page == null ? "" : page);
    }
    public String activePage() { return activePage; }

    /** 记录宿主绑定来源（v_mod/manifest 名，预留）。 */
    public void noteBindings(String b) { bindings = (b == null ? "" : b); }
    public String bindings() { return bindings; }

    /** 当前逻辑版本（已应用更新版本，0=内置）。 */
    public int version() { return UpdateManager.currentVersion(); }

    /* ==================== 状态快照（可序列化，红线 §6-6） ==================== */

    /** 生成会话快照 JSON：seq/版本/当前页/绑定/输入状态（刷新自当前渲染器）。 */
    public String snapshot() {
        if (renderer != null) renderer.saveInputs();          // 刷新 savedVals 为最新
        JsonObject o = new JsonObject();
        o.addProperty("seq", seq);
        o.addProperty("time", new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US)
                .format(new Date(startedAt)));
        o.addProperty("version", version());
        o.addProperty("activePage", activePage);
        o.addProperty("bindings", bindings);
        JsonObject st = new JsonObject();
        if (renderer != null) {
            Map<String, String> sv = renderer.savedVals();
            synchronized (sv) {
                for (Map.Entry<String, String> e : sv.entrySet()) {
                    st.addProperty(e.getKey(), e.getValue() == null ? "" : e.getValue());
                }
            }
        }
        o.add("state", st);
        /* native 会话（屏栈 + 全局变量）并入快照（Cordis_dc M1 native 侧会话对齐）。
         * vuaSessionSnapshot 需 native 已 init（vuaInit 后）；未就绪时为 null，跳过。 */
        String nativeSnap = VuaBridge.vuaSessionSnapshot();
        if (nativeSnap != null && !nativeSnap.isEmpty()) {
            try {
                o.add("native", GSON.fromJson(nativeSnap, JsonObject.class));
            } catch (Throwable t) {
                Log.w(TAG, "会话快照: native 段解析失败", t);
            }
        }
        return GSON.toJson(o);
    }

    /**
     * 恢复快照（重启用，须在 vuaInit 之后调用——native 会话已建）：
     * native 段重建屏栈+全局变量，state 写回渲染器 savedVals（重建后按节点恢复）。
     * 任一段成功即视为部分成功；格式异常返回 false（调用方容忍）。
     */
    public boolean restore(String snapshotJson) {
        if (snapshotJson == null || snapshotJson.isEmpty()) return false;
        boolean ok = false;
        try {
            JsonObject o = GSON.fromJson(snapshotJson, JsonObject.class);
            if (o == null) return false;
            /* native 会话恢复（屏栈 + 全局变量）优先 */
            if (o.has("native")) {
                String n = GSON.toJson(o.get("native"));
                int rc = VuaBridge.vuaSessionRestore(n);
                Log.i(TAG, "会话恢复: native 段 rc=" + rc);
                ok |= (rc == 0);
            }
            if (renderer != null && o.has("state")) {
                JsonObject st = o.getAsJsonObject("state");
                Map<String, String> vals = new LinkedHashMap<>();
                for (String k : st.keySet()) {
                    vals.put(k, st.get(k).isJsonNull() ? "" : st.get(k).getAsString());
                }
                renderer.restoreSaved(vals);
                ok = true;                 /* state 段存在即视为已应用（重建后按节点恢复） */
            }
            if (o.has("activePage")) noteActivePage(o.get("activePage").getAsString());
        } catch (Throwable t) {
            Log.w(TAG, "会话恢复: 失败", t);
            return false;
        }
        return ok;
    }

    /** 清空会话（进程内重建会话时调用，预留）。 */
    void reset() {
        startedAt = System.currentTimeMillis();
        seq = 1;
        activePage = "";
        bindings = "";
        renderer = null;
    }
}