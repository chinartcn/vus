/*
 * ReloadManager.java — 事务管理器（Cordis_dc §3.3，M1 首个落地物）
 *
 * 把热更新收编为显式事务状态机，并实现「延迟生效事务」（Pending_Reload，§3.6）：
 *
 *   实时层（.vua/.dex）  : noteApplied → APPLIED（随应用完成，最后两拍当场补完）
 *   .so / 依赖库        : noteApplied → 快照会话 → 写 patch/reload.json → PENDING_RELOAD
 *                         重启后 onBoot() 读档 → 应用快照 → 补完事务 → 清标记
 *
 * 与 UpdateManager 的衔接：applyUpdate 成功路径追加 noteApplied()。
 * 与 UpdateManager 的现有 patch/pending（待晋升 last-good 版本号）不同——
 * reload.json 是 Cordis_dc 的 Pending_Reload 事务标记（v_mod 清单 + 状态快照路径）。
 *
 * 状态机：IDLE → CHECKING → SNAPSHOTTED → PENDING_RELOAD →(重启)→ APPLIED / ROLLED_BACK
 */
package com.vus.android;

import android.content.Context;
import android.util.Log;

import com.google.gson.Gson;
import com.google.gson.JsonArray;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import java.io.File;
import java.util.List;

public final class ReloadManager {

    private static final String TAG = "VusReload";
    /** Pending_Reload 事务标记：patch/reload.json */
    private static final String RELOAD_FILE = "patch/reload.json";

    public enum Phase {
        IDLE, CHECKING, SNAPSHOTTED, PENDING_RELOAD, APPLIED, ROLLED_BACK
    }

    private static final Gson GSON = new Gson();
    private static Context c;
    private static volatile Phase phase = Phase.IDLE;
    /** 重启后待恢复的快照文件相对路径（patch/session_<ver>.json）；null=无。 */
    private static volatile String pendingStatePath = null;

    private ReloadManager() { }

    /** MainActivity.onCreate 在 UpdateManager.init 之后调用。 */
    public static void init(Context context) {
        c = context == null ? null : context.getApplicationContext();
    }

    public static Phase phase() { return phase; }

    /** 是否有待重启生效的延迟事务（UI 提示"需要重启"的依据）。 */
    public static boolean pendingReload() { return phase == Phase.PENDING_RELOAD; }

    /* ==================== 启动钩子：受理上次重启前的延迟事务 ==================== */

    /**
     * 每次启动调用（UpdateManager.onBoot 之后；vuaInit 之前）：受理 Pending_Reload
     * 事务——读 patch/reload.json，取出快照路径候选并清标记；真正恢复（重建屏栈 +
     * 全局变量，需 native 会话已建）推迟到 vuaInit 成功后由 applyPendingRestore() 执行。
     */
    public static void onBoot() {
        if (c == null) return;
        phase = Phase.IDLE;
        pendingStatePath = null;
        try {
            File f = new File(c.getFilesDir(), RELOAD_FILE);
            if (!f.isFile()) return;
            JsonObject o = JsonParser.parseString(VusIo.readText(f)).getAsJsonObject();
            JsonArray pend = o.has("pending") ? o.getAsJsonArray("pending") : null;
            if (pend == null || pend.size() == 0) {
                f.delete();
                return;
            }
            if (o.has("state_path")) pendingStatePath = o.get("state_path").getAsString();
            f.delete();   // 已受理：事务由本次启动补完，标记即废
            phase = Phase.PENDING_RELOAD;
            Log.i(TAG, "启动受理 Pending_Reload 事务（" + pend.size() + " 个待生效文件）");
        } catch (Throwable t) {
            Log.w(TAG, "onBoot 处理失败: " + t);
            phase = Phase.ROLLED_BACK;
        }
    }

    /**
     * vuaInit 成功后调用：恢复会话快照（native 屏栈 + 全局变量 + 渲染器输入状态），
     * 恢复成功即删除快照文件。新 .so 的加载由 VuaBridge.ensureNative 以 filesDir
     * 优先天然生效；此处只负责把上次会话的"现场"捡回来。
     */
    public static void applyPendingRestore() {
        if (c == null || pendingStatePath == null) return;
        try {
            VusSession s = VusSession.get();
            File sf = new File(c.getFilesDir(), pendingStatePath);
            if (s != null && sf.isFile()) {
                String body = VusIo.readText(sf);
                if (body != null && !body.isEmpty() && s.restore(body)) {
                    Log.i(TAG, "延迟事务补完：已恢复会话快照 " + pendingStatePath);
                }
            }
            sf.delete();   // 无论成败，快照只消费一次
            pendingStatePath = null;
            phase = Phase.APPLIED;
            if (s != null) s.notifyChanged("reload:completed");
        } catch (Throwable t) {
            Log.w(TAG, "applyPendingRestore 失败: " + t);
            phase = Phase.ROLLED_BACK;
        }
    }

    /* ==================== 更新包应用后：进入事务 ==================== */

    /**
     * UpdateManager.applyUpdate 成功（return 0）后调用。
     *
     * applied 含 .so/.jar（依赖库）→ 延迟生效：快照会话 → 写 Pending_Reload，
     * 提示"需要重启"，重启后 onBoot() 读档补完（见 §3.6 两案加载表）。
     * 纯 .vua/.dex → 实时事务已完成（APPLIED）。
     */
    public static void noteApplied(int version, List<String> applied) {
        if (c == null || applied == null) return;
        phase = Phase.CHECKING;
        try {
            boolean needsRestart = false;
            for (String rel : applied) {
                if (rel != null && (rel.endsWith(".so") || rel.endsWith(".jar"))) {
                    needsRestart = true;
                    break;
                }
            }
            VusSession s = VusSession.get();
            if (needsRestart) {
                phase = Phase.SNAPSHOTTED;
                String snap = s != null ? s.snapshot() : "{}";
                String statePath = "patch/session_" + version + ".json";
                VusIo.writeText(new File(c.getFilesDir(), statePath), snap);
                JsonObject o = new JsonObject();
                o.addProperty("state_path", statePath);
                JsonArray pend = new JsonArray();
                for (String rel : applied) {
                    if (rel != null) pend.add(rel);
                }
                o.add("pending", pend);
                VusIo.writeText(new File(c.getFilesDir(), RELOAD_FILE), GSON.toJson(o));
                phase = Phase.PENDING_RELOAD;
                Log.i(TAG, "更新含 .so/.jar → Pending_Reload（重启读档生效）, version " + version);
            } else {
                phase = Phase.APPLIED;
                Log.i(TAG, "更新纯实时层 → 事务已完成, version " + version);
            }
            if (s != null) s.notifyChanged("update:" + version);
        } catch (Throwable t) {
            Log.w(TAG, "noteApplied 失败: " + t);
            phase = Phase.ROLLED_BACK;
        }
    }

    /** 手动清 Pending_Reload 标记（预留；设计上无"放弃更新"，仅失败恢复时用）。 */
    static void clearPending() {
        if (c == null) return;
        try {
            File f = new File(c.getFilesDir(), RELOAD_FILE);
            if (f.isFile()) f.delete();
            phase = Phase.IDLE;
        } catch (Throwable ignored) { }
    }
}