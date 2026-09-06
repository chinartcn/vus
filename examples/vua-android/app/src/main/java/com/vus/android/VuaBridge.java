/*
 * VuaBridge.java — VUA native 交换桥（Android 组件流）
 *
 * 声明 APK 侧调用 native `rt/vua` 所需的 JNI 方法。native 侧（rt/vua.c 的
 * generator 映射 / apk 生成的 jni_bridge.c）须导出与本类方法对应的
 * `Java_<包名>_VuaBridge_<方法名>` 符号。
 *
 * 数据流：
 *   1. vuaInit()         native 建 VuaSession 并运行 vus_main()（.vus 逻辑：
 *                       界面_显示 首页 → 界面_绑定 事件）
 *   2. vuaRenderTree()   取当前屏（栈顶）的规范化渲染树 JSON（vua_screen_dump_rendertree）
 *   3. Java 用 VuaRenderer 把 JSON 建成 Android View 树
 *   4. 触摸 → vuaTriggerById / vuaTrigger 回传 native → .vus handler → 可能换屏
 *   5. native 屏栈变化时经 vua_notify_rerender → 本类的 onNativeRerender 回调
 *      MainActivity 重建当前屏 View（native → Java 回流，无需 Java 轮询）
 *
 * 2026-09 重构：网络/文件/线程收敛到 VusNet/VusIo/VusAsync；网络 JSON 改 Gson。
 * 注：包名必须与 APK 包名一致，否则 JNI 符号对不上。下面以 com.vus.android 为例。
 */
package com.vus.android;

import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.util.Log;
import android.widget.Toast;

import com.google.gson.Gson;
import com.google.gson.JsonObject;

import java.io.File;
import java.util.HashMap;
import java.util.Map;

public final class VuaBridge {

    private static final Gson GSON = new Gson();

    /*
     * 单一真源（热更设计 §5.4）：native 库由 ensureNative() 显式加载——
     * filesDir/lib/libvus_app.so（patch 释放产物）优先，缺失/加载失败回退 APK 内
     * lib/<abi>/libvus_app.so（内置版本 0）。加载成功触发 UpdateManager.onSoLoaded()
     * 作为 last-good 的晋升时机（§5.2）。类加载本身不再隐式 loadLibrary，
     * 避免 appContext 注入前（filesDir 判定条件）就加载错库。
     */
    private static volatile boolean libLoaded = false;

    /** 加载 native 库（单一真源）。首次调用任一 native 方法前必须执行（MainActivity 起步）。
     * 幂等：重复调用直接返回。 */
    public static synchronized void ensureNative() {
        if (libLoaded) return;
        if (appContext != null) {
            try {
                File f = new File(appContext.getFilesDir(), "lib/libvus_app.so");
                if (f.isFile()) {
                    System.load(f.getAbsolutePath());
                    libLoaded = true;
                    UpdateManager.onSoLoaded();
                    return;
                }
            } catch (Throwable t) {
                Log.w("VuaBridge", "filesDir 版本 libvus_app.so 加载失败，回退 APK 内置", t);
            }
        }
        System.loadLibrary("vus_app");
        libLoaded = true;
        UpdateManager.onSoLoaded();
    }

    /** 应用 Context：由 MainActivity.onCreate 注入，供 callJava 平台能力桥解析文件路径/目录。 */
    public static Context appContext = null;

    /** 主 Activity：由 MainActivity.onCreate 注入，供 屏幕_常亮 等窗口相关能力使用。 */
    public static Activity sActivity = null;

    /** native → Java 重绘回调：屏栈变化（界面_显示/返回/返回至）后由 native 调用。
     * MainActivity 在此注册一个 runnable 来重建当前屏的 View。 */
    public static Runnable onRerender = null;

    /** Java 检查更新回调：由 VuaRenderer 按钮处理触发 */
    public static Runnable onCheckUpdate = null;

    /** 渲染请求合并标志：native 换屏回调与按钮本地刷新多次请求合并为一次重建，
     *  避免一次点击触发两次全量重建（先 runOnUiThread 排队、再 refresh 立即渲染）。 */
    private static boolean renderPending = false;

    /** 请求一次界面重建（合并）。native 重绘回调与 VuaRenderer.refresh 都走这里。 */
    public static void requestRender() {
        if (renderPending) return;
        renderPending = true;
        if (onRerender != null) onRerender.run();
    }

    /** 界面真正重建完成后调用，复位合并标志，允许下一次重建。 */
    public static void renderHandled() {
        renderPending = false;
    }

    /** 被 native（jni_bridge.c）调用的入口；可能来自非 UI 线程，需自行切到主线程。 */
    public static void onNativeRerender() {
        requestRender();
    }

    /* ==================== Java 平台能力桥（网络/文件由 Java 暴露、VUS 调用） ====================
     * jni_bridge.c 把 VUS 内建（网络_GET/文件_读取 等）同步转发到这里。
     * args：JSON 对象字符串（{"path":"..."} / {"url":"...","data":"..."}）；
     * 返回：{"ok":1,"data":"..."} 或 {"ok":0,"err":"..."}（均为 JSON 字符串）。
     * 文件路径按相对名（相对应用 filesDir）解析，与 native 的 cwd 一致。 */

    /** 被 native 调用的 RPC 入口。保证本方法内不抛异常（错误转成 ok:0 返回）。 */
    public static String callJava(String api, String argsJson) {
        try {
            JsonObject a = args(argsJson);
            if ("file.read".equals(api)) {
                File f = resolve(str(a, "path"));
                if (!f.isFile()) return err("文件不存在");
                return ok(VusIo.readText(f));
            }
            if ("file.write".equals(api) || "file.append".equals(api)) {
                File f = resolve(str(a, "path"));
                byte[] b = str(a, "content").getBytes("UTF-8");
                VusIo.writeBytes(f, b, "file.append".equals(api));
                return ok("0");
            }
            if ("file.exists".equals(api)) {
                return ok(resolve(str(a, "path")).exists() ? "1" : "0");
            }
            if ("file.delete".equals(api)) {
                return ok(resolve(str(a, "path")).delete() ? "0" : "-1");
            }
            if ("file.isdir".equals(api)) {
                return ok(resolve(str(a, "path")).isDirectory() ? "true" : "false");
            }
            if ("file.list".equals(api)) {
                String[] names = resolve(str(a, "path")).list();
                if (names == null) return ok("");
                StringBuilder sb = new StringBuilder();
                for (String n : names) sb.append(n).append('\n');
                return ok(sb.toString());
            }
            if ("http.get".equals(api) || "http.post".equals(api)) {
                JsonObject hd = obj(a, "headers");
                byte[] b = http(a, "http.post".equals(api), hd);
                return ok(b == null ? "" : new String(b, "UTF-8"));
            }
            /* 通用请求：method=GET|POST，headers 自定义请求头（如 Authorization token 认证）、
             * timeout 秒、retry 重试次数。覆盖「认证/超时/重试」类需求。 */
            if ("http.request".equals(api)) {
                String method = "GET".equalsIgnoreCase(str(a, "method", "GET")) ? "GET" : "POST";
                byte[] b = http(a, "POST".equals(method), obj(a, "headers"));
                return ok(b == null ? "" : new String(b, "UTF-8"));
            }
            if ("http.upload".equals(api)) {
                // multipart/form-data 文件上传：url + 本地文件 path + 附加字段 fields + 头 headers
                Map<String, String> fields = stringsOf(obj(a, "fields"));
                boolean up = upload(str(a, "url"), str(a, "path"),
                        fields, headersOf(obj(a, "headers")), num(a, 60, "timeout"));
                return up ? ok("1") : err("上传失败");
            }
            if ("http.download".equals(api)) {
                VusAsync.Holder<byte[]> h = VusNet.requestBytes(str(a, "url"), null, null, 60, 2);
                byte[] b = h.err != null ? null : h.val;
                if (b == null) return ok("0");
                File f = resolve(str(a, "path"));
                VusIo.writeBytes(f, b, false);
                return ok("1");
            }
            /* ---- Android 轻量能力（纯 Java 实现）：振动/剪贴板/设备信息/Toast ---- */
            if ("vibrate".equals(api)) { vibrate(num(a, 100, "ms")); return ok("0"); }
            if ("clipboard.read".equals(api)) return ok(clipboardRead());
            if ("clipboard.write".equals(api)) { clipboardWrite(str(a, "text")); return ok("0"); }
            if ("device.info".equals(api)) return ok(deviceInfo());
            if ("toast".equals(api)) { toast(str(a, "text"), num(a, 0, "long") != 0); return ok("0"); }
            /* 系统能力延伸：分享 / 电量 / 屏幕常亮 / 网络类型 / 通知 */
            if ("share.text".equals(api)) return ok(shareText(str(a, "text")));
            if ("battery.status".equals(api)) return ok(batteryStatus());
            if ("screen.keepon".equals(api)) { keepScreenOn(str(a, "flag", "1")); return ok("0"); }
            if ("network.type".equals(api)) return ok(networkType());
            if ("notify.send".equals(api)) return ok(sendNotify(str(a, "title"), str(a, "body")));
            // DEX 逻辑拓展：api 形如 "ext.<插件名>.<操作>"，交给 ExtensionLoader 动态加载调用。
            // 插件 dex 位于 filesDir/plugins/<插件名>.dex，支持运行期热更新（配合 http.download）。
            if (api.startsWith("ext.")) {
                return ExtensionLoader.dispatch(api.substring(4), toOrgJson(a));
            }
            // 热更协议：应用含新 .so/.vua/.dex 的更新包（UpdateManager.applyUpdate）。
            // vars: {"url":"<manifest.json 地址>"}，返回 data: 0=已应用 1=无更新 -1=宿主过低 -2=失败。
            if ("hotupdate.apply".equals(api)) {
                final String url = str(a, "url");
                VusAsync.Holder<Integer> h = VusAsync.bg(new ApplyUpdateJob(url), 200L);
                if (h.err != null) return err(String.valueOf(h.err));
                return ok(String.valueOf(h.val));
            }
            return err("未知能力: " + api);
        } catch (Exception e) {
            return err(String.valueOf(e));
        }
    }

    /* ---- JSON 帮助器（Gson） ---- */

    /** 热更应用任务（命名类：规避 d8 dev 泛型匿名类 desugar bug）。 */
    private static final class ApplyUpdateJob implements VusAsync.Job<Integer> {
        private final String url;
        ApplyUpdateJob(String url) { this.url = url; }
        @Override public Integer run() throws Throwable {
            return UpdateManager.applyUpdate(url);
        }
    }

    private static JsonObject args(String json) {
        try {
            JsonObject o = GSON.fromJson(json == null ? "{}" : json, JsonObject.class);
            return o != null ? o : new JsonObject();
        } catch (Throwable t) {
            return new JsonObject();
        }
    }

    /** 按键读字符串，缺省 ""。 */
    private static String str(JsonObject o, String key) { return str(o, key, ""); }
    private static String str(JsonObject o, String key, String def) {
        if (o != null && o.has(key) && !o.get(key).isJsonNull()) return o.get(key).getAsString();
        return def;
    }

    private static int num(JsonObject o, int def, String key) {
        if (o != null && o.has(key) && !o.get(key).isJsonNull()) {
            try { return o.get(key).getAsInt(); } catch (Throwable ignored) { }
        }
        return def;
    }

    private static JsonObject obj(JsonObject o, String key) {
        if (o != null && o.has(key) && !o.get(key).isJsonNull()) {
            try { return o.getAsJsonObject(key); } catch (Throwable ignored) { }
        }
        return null;
    }

    private static Map<String, String> stringsOf(JsonObject o) {
        Map<String, String> out = new HashMap<>();
        if (o != null) {
            for (Map.Entry<String, com.google.gson.JsonElement> e : o.entrySet()) {
                out.put(e.getKey(), e.getValue() == null || e.getValue().isJsonNull()
                        ? "" : e.getValue().getAsString());
            }
        }
        return out.isEmpty() ? null : out;
    }

    /** 保持既有 RPC 契约（插件期望 org.json）——转换入参对象。 */
    private static org.json.JSONObject toOrgJson(JsonObject o) {
        try {
            return o == null ? new org.json.JSONObject() : new org.json.JSONObject(GSON.toJson(o));
        } catch (Exception e) {
            return new org.json.JSONObject();
        }
    }

    private static String ok(String data) {
        JsonObject o = new JsonObject();
        o.addProperty("ok", true);
        o.addProperty("data", data == null ? "" : data);
        return GSON.toJson(o);
    }

    private static String err(String msg) {
        JsonObject o = new JsonObject();
        o.addProperty("ok", false);
        o.addProperty("err", msg == null ? "" : msg);
        return GSON.toJson(o);
    }

    /* ---- 相对路径解析 ---- */

    /** 相对名 → 应用文件目录下的绝对文件；绝对路径则原样使用。 */
    private static File resolve(String name) {
        File f = new File(name == null ? "" : name);
        if (f.isAbsolute() || appContext == null) return f;
        return new File(appContext.getFilesDir(), name);
    }

    /* ---- Android 轻量能力（纯 Java）：振动/剪贴板/设备信息/Toast ---- */

    /** 振动（毫秒）。API 26+ 用 VibrationEffect；API 31+ 走 VibratorManager。
     * 无振动器/无权限时静默失败（VUS 侧只关心调用不报错）。 */
    private static void vibrate(long ms) {
        if (appContext == null || ms <= 0) return;
        try {
            if (Build.VERSION.SDK_INT >= 31) {
                VibratorManager vm = (VibratorManager) appContext.getSystemService(Context.VIBRATOR_MANAGER_SERVICE);
                if (vm != null)
                    vm.getDefaultVibrator().vibrate(
                            VibrationEffect.createOneShot(ms, VibrationEffect.DEFAULT_AMPLITUDE));
            } else {
                Vibrator v = (Vibrator) appContext.getSystemService(Context.VIBRATOR_SERVICE);
                if (v == null) return;
                if (Build.VERSION.SDK_INT >= 26)
                    v.vibrate(VibrationEffect.createOneShot(ms, VibrationEffect.DEFAULT_AMPLITUDE));
                else
                    //noinspection deprecation
                    v.vibrate(ms);
            }
        } catch (Throwable ignored) { }
    }

    /** 读系统剪贴板文本；无剪贴板管理器/无内容/读取受限返回 ""。 */
    private static String clipboardRead() {
        if (appContext == null) return "";
        try {
            ClipboardManager cm = (ClipboardManager) appContext.getSystemService(Context.CLIPBOARD_SERVICE);
            if (cm == null || !cm.hasPrimaryClip() || cm.getPrimaryClip() == null
                    || cm.getPrimaryClip().getItemCount() == 0) return "";
            return String.valueOf(cm.getPrimaryClip().getItemAt(0).coerceToText(appContext));
        } catch (Throwable t) {
            return "";
        }
    }

    /** 写系统剪贴板（text 为空串则清空）。 */
    private static void clipboardWrite(String text) {
        if (appContext == null || text == null) return;
        try {
            ClipboardManager cm = (ClipboardManager) appContext.getSystemService(Context.CLIPBOARD_SERVICE);
            if (cm != null) cm.setPrimaryClip(ClipData.newPlainText("vus", text));
        } catch (Throwable ignored) { }
    }

    /** 设备信息（JSON 字符串）：品牌/型号/系统版本/SDK/应用版本。 */
    private static String deviceInfo() {
        JsonObject o = new JsonObject();
        o.addProperty("品牌", Build.MANUFACTURER == null ? "" : Build.MANUFACTURER);
        o.addProperty("型号", Build.MODEL == null ? "" : Build.MODEL);
        o.addProperty("系统版本", Build.VERSION.RELEASE == null ? "" : Build.VERSION.RELEASE);
        o.addProperty("SDK", Build.VERSION.SDK_INT);
        if (appContext != null) {
            try {
                o.addProperty("应用版本", appContext.getPackageManager()
                        .getPackageInfo(appContext.getPackageName(), 0).versionName);
            } catch (Throwable ignored) { }
        }
        return GSON.toJson(o);
    }

    /** Toast 提示（isLong=true 约 3.5s，默认 2s）；统一切主线程弹出。 */
    private static void toast(String text, boolean isLong) {
        if (appContext == null || text == null || text.isEmpty()) return;
        final String t = text;
        final int len = isLong ? Toast.LENGTH_LONG : Toast.LENGTH_SHORT;
        sMain.post(() -> Toast.makeText(appContext.getApplicationContext(), t, len).show());
    }

    /* ---- 系统能力延伸：分享 / 电量 / 屏幕常亮 / 网络类型 / 通知 ---- */

    /** 分享文本（系统分享面板）。无可用面板返回 "-1"，已发起分享返回 "0"。 */
    private static String shareText(String text) {
        if (appContext == null || text == null || text.isEmpty()) return "0";
        try {
            Intent i = new Intent(Intent.ACTION_SEND);
            i.setType("text/plain");
            i.putExtra(Intent.EXTRA_TEXT, text);
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            appContext.startActivity(Intent.createChooser(i, "分享到"));
            return "0";
        } catch (Throwable t) {
            return "-1";
        }
    }

    /** 电量 JSON：{"电量":0~100(-1 未知),"充电中":true/false}。 */
    private static String batteryStatus() {
        JsonObject o = new JsonObject();
        int level = -1; boolean charging = false;
        if (appContext != null) {
            try {
                BatteryManager bm = (BatteryManager) appContext.getSystemService(Context.BATTERY_SERVICE);
                if (bm != null) level = bm.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY);
                Intent bi = appContext.registerReceiver(null,
                        new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
                if (bi != null) {
                    int st = bi.getIntExtra(BatteryManager.EXTRA_STATUS, -1);
                    charging = (st == BatteryManager.BATTERY_STATUS_CHARGING
                            || st == BatteryManager.BATTERY_STATUS_FULL);
                }
            } catch (Throwable ignored) { }
        }
        o.addProperty("电量", level);
        o.addProperty("充电中", charging);
        return GSON.toJson(o);
    }

    /** 屏幕常亮开关（0 关 / 非 0 开）：作用于主窗口 FLAG_KEEP_SCREEN_ON。 */
    private static void keepScreenOn(String flag) {
        if (sActivity == null || sActivity.getWindow() == null) return;
        boolean on = !"0".equals(flag);
        if (on) sActivity.getWindow().addFlags(
                android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        else sActivity.getWindow().clearFlags(
                android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    /** 网络类型："wifi" / "mobile" / "none"。无权限时返回 "none"。 */
    private static String networkType() {
        if (appContext == null) return "none";
        try {
            ConnectivityManager cm = (ConnectivityManager) appContext
                    .getSystemService(Context.CONNECTIVITY_SERVICE);
            if (cm == null) return "none";
            if (Build.VERSION.SDK_INT >= 23) {
                Network n = cm.getActiveNetwork();
                if (n == null) return "none";
                NetworkCapabilities nc = cm.getNetworkCapabilities(n);
                if (nc == null) return "none";
                if (nc.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) return "wifi";
                if (nc.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET)) return "wifi";
                if (nc.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR)) return "mobile";
                return "none";
            } else {
                android.net.NetworkInfo ni = cm.getActiveNetworkInfo();
                if (ni == null || !ni.isConnected()) return "none";
                int t = ni.getType();
                if (t == ConnectivityManager.TYPE_WIFI
                        || t == ConnectivityManager.TYPE_ETHERNET) return "wifi";
                if (t == ConnectivityManager.TYPE_MOBILE) return "mobile";
                return "none";
            }
        } catch (Throwable t) {
            return "none";
        }
    }

    /** 发送通知栏通知（Android 13+ 需 POST_NOTIFICATIONS 运行时权限，未授予返回 "需要权限"）。 */
    private static String sendNotify(String title, String body) {
        if (appContext == null || title == null || title.isEmpty()) return "-1";
        try {
            NotificationManager nm = (NotificationManager) appContext
                    .getSystemService(Context.NOTIFICATION_SERVICE);
            if (nm == null) return "-1";
            if (Build.VERSION.SDK_INT >= 33) {
                if (appContext.checkSelfPermission("android.permission.POST_NOTIFICATIONS")
                        == android.content.pm.PackageManager.PERMISSION_DENIED) {
                    return "需要权限";   // 脚本可提示用户到系统设置开启
                }
            }
            if (Build.VERSION.SDK_INT >= 26) {
                NotificationChannel ch = new NotificationChannel(
                        "vus", "VUS 通知", NotificationManager.IMPORTANCE_DEFAULT);
                nm.createNotificationChannel(ch);
            }
            Notification.Builder b = Build.VERSION.SDK_INT >= 26
                    ? new Notification.Builder(appContext, "vus")
                    : new Notification.Builder(appContext);
            b.setSmallIcon(android.R.drawable.ic_dialog_info);
            b.setContentTitle(title);
            b.setContentText(body == null ? "" : body);
            b.setAutoCancel(true);
            b.setWhen(System.currentTimeMillis());
            nm.notify((int) (System.currentTimeMillis() & 0x7fffffff), b.build());
            return "0";
        } catch (Throwable t) {
            return "-1";
        }
    }

    /* ---- 网络（VusNet 封装，主线程规避已在 VusNet 内处理） ---- */

    private static byte[] http(JsonObject a, boolean post, JsonObject headers) {
        String data = post ? str(a, "data") : null;
        VusAsync.Holder<byte[]> h = VusNet.requestBytes(str(a, "url"), data,
                headersOf(headers), num(a, 30, "timeout"), num(a, 2, "retry"));
        return h.err != null ? null : h.val;
    }

    private static boolean upload(String url, String path, Map<String, String> fields,
                                  Map<String, String> headers, int timeoutSec) {
        File file = resolve(path);
        VusAsync.Holder<Boolean> h = VusNet.upload(url, file, fields, headers, timeoutSec);
        if (h.err != null || h.val == null) return false;
        return h.val.booleanValue();
    }

    /** JSON 对象 → 请求头 Map（{"Authorization":"Bearer x", ...}）。 */
    private static Map<String, String> headersOf(JsonObject h) {
        return stringsOf(h);
    }

    /* ---- WebView JS 桥 → VUA 事件（反馈「JS 回调要接回 vuaTrigger 事件」） ----
     * 网页内 window.vus.onEvent("事件名", "{...}") 由此转发；JS 回调线程不保证是
     * 主线程，统一切主线程再进 native，避免跨线程 JNI 状态问题。 */
    private static final Handler sMain = new Handler(Looper.getMainLooper());
    public static void postToTrigger(final String name, final String varsJson) {
        sMain.post(() -> vuaTrigger(name == null ? "" : name, varsJson == null ? "{}" : varsJson));
    }
    public static void postToTriggerById(final String id, final String varsJson) {
        sMain.post(() -> vuaTriggerById(id == null ? "" : id, varsJson == null ? "{}" : varsJson));
    }

    /**
     * native：创建全局 VuaSession 并运行 vus_main()（.vus 入口）。
     * 返回 0 成功，非 0 失败。此后当前屏（若 .vus 调用了界面_显示）可被渲染。
     */
    public static native int vuaInit();

    /**
     * native：返回当前屏（栈顶）的规范化渲染树 JSON 字节（UTF-8）；无屏返回 null。
     * 以 byte[] 传输省去 NewStringUTF 全量校验/转换；字符串在 native 侧缓存所有。
     */
    public static native byte[] vuaRenderTreeBytes();

    /**
     * native：返回当前屏渲染树的内容指纹（版本号协议）。
     * 指纹不变 = 内容不变，可跳过 vuaRenderTree 整树传输，直接命中页面 View 缓存。
     * 无屏返回 -1。
     */
    public static native long vuaRenderHash();

    /**
     * native：当前屏序号（View diff）。序号不变 = 仍是同一屏（仅变量值变化，
     * 可增量更新文本控件）；变化 = 换页。无屏返回 -1。
     */
    public static native long vuaScreenId();

    /**
     * native：设置 VUS/VUA 运行时的工作目录（应传 Context.getFilesDir()）。
     * 会让相对路径（如 界面_显示("vua_home.vua")）在该目录下解析。
     */
    public static native void vuaSetRootDir(String filesDir);

    /** native：按事件名派发，携带回调变量（JSON 对象，如 {"金额":"1280"}）。 */
    public static native int vuaTrigger(String eventName, String varsJson);

    /** native：按控件 id 派发（走 eventIndex），携带回调变量 JSON。 */
    public static native int vuaTriggerById(String nodeId, String varsJson);

    private VuaBridge() { }
}