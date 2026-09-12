/*
 * VceApi.java — 宿主能力桥（vce 面）轻量能力实现（Cordis_dc §3.5 / M3 拆包）
 *
 * 从 VuaBridge.callJava 抽出的「Android 轻量能力」内联实现：振动/剪贴板/设备信息/
 * Toast/分享/电量/屏幕常亮/网络类型/通知。原生依赖仅 VuaBridge.appContext /
 * VuaBridge.sActivity（公开静态）；JSON 信封（ok/err）与 callJava 同格式。
 *
 * 归属：CapabilityRegistry 登记这些 api 为 vce 面；VuaBridge.callJava 对 vce 面
 * 先经本位分发，未知 api 返回 null 回落既有分支（file/http/media 等深耦合能力
 * 本期不移，交后续 M3 迭代）。
 */
package com.vus.android;

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
import android.widget.Toast;

import com.google.gson.Gson;
import com.google.gson.JsonObject;

final class VceApi {

    private static final Gson GSON = new Gson();
    /** 主线程执行器（Toast 弹窗等串扰主线程的动作）。 */
    private static final Handler MAIN = new Handler(Looper.getMainLooper());

    private VceApi() { }

    /**
     * vce 面（宿主能力桥）无缝分发。处理成功返回 ok/err JSON 字符串；
     * 非本类能力返回 null（调用方回落既有分支）。api 与 callJava 协议一致。
     */
    static String handle(String api, JsonObject a) {
        try {
            if ("vibrate".equals(api)) {
                vibrate(num(a, 100, "ms"));
                return ok("0");
            }
            if ("clipboard.read".equals(api)) return ok(clipboardRead());
            if ("clipboard.write".equals(api)) {
                clipboardWrite(str(a, "text"));
                return ok("0");
            }
            if ("device.info".equals(api)) return ok(deviceInfo());
            if ("toast".equals(api)) {
                toast(str(a, "text"), num(a, 0, "long") != 0);
                return ok("0");
            }
            if ("share.text".equals(api)) return ok(shareText(str(a, "text")));
            if ("battery.status".equals(api)) return ok(batteryStatus());
            if ("screen.keepon".equals(api)) {
                keepScreenOn(str(a, "flag", "1"));
                return ok("0");
            }
            if ("network.type".equals(api)) return ok(networkType());
            if ("notify.send".equals(api)) {
                return ok(sendNotify(str(a, "title"), str(a, "body")));
            }
            return null;   // 非本类能力：回落 VuaBridge 既有分支
        } catch (Throwable t) {
            return err(String.valueOf(t));
        }
    }

    /* ---- 实现（自 VuaBridge 整体迁入，行为不变） ---- */

    /** 振动（毫秒）。API 26+ 用 VibrationEffect；API 31+ 走 VibratorManager。 */
    private static void vibrate(long ms) {
        Context appContext = VuaBridge.appContext;
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
        Context appContext = VuaBridge.appContext;
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
        Context appContext = VuaBridge.appContext;
        if (appContext == null || text == null) return;
        try {
            ClipboardManager cm = (ClipboardManager) appContext.getSystemService(Context.CLIPBOARD_SERVICE);
            if (cm != null) cm.setPrimaryClip(ClipData.newPlainText("vus", text));
        } catch (Throwable ignored) { }
    }

    /** 设备信息（JSON 字符串）：品牌/型号/系统版本/SDK/应用版本。 */
    private static String deviceInfo() {
        Context appContext = VuaBridge.appContext;
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
        Context appContext = VuaBridge.appContext;
        if (appContext == null || text == null || text.isEmpty()) return;
        final String t = text;
        final int len = isLong ? Toast.LENGTH_LONG : Toast.LENGTH_SHORT;
        MAIN.post(() -> Toast.makeText(appContext.getApplicationContext(), t, len).show());
    }

    /** 分享文本（系统分享面板）。无可用面板返回 "-1"，已发起分享返回 "0"。 */
    private static String shareText(String text) {
        Context appContext = VuaBridge.appContext;
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
        Context appContext = VuaBridge.appContext;
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
        if (VuaBridge.sActivity == null || VuaBridge.sActivity.getWindow() == null) return;
        boolean on = !"0".equals(flag);
        if (on) VuaBridge.sActivity.getWindow().addFlags(
                android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        else VuaBridge.sActivity.getWindow().clearFlags(
                android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    /** 网络类型："wifi" / "mobile" / "none"。无权限时返回 "none"。 */
    private static String networkType() {
        Context appContext = VuaBridge.appContext;
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
        Context appContext = VuaBridge.appContext;
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

    /* ---- JSON 帮助器（与 callJava 同格式信封） ---- */

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

    private static String str(JsonObject o, String key) { return str(o, key, ""); }
    private static String str(JsonObject o, String key, String def) {
        if (o != null && o.has(key) && !o.get(key).isJsonNull()) return o.get(key).getAsString();
        return def;
    }

    private static long num(JsonObject o, long def, String key) {
        if (o != null && o.has(key) && !o.get(key).isJsonNull()) {
            try { return (long) o.get(key).getAsDouble(); } catch (Throwable ignored) { }
        }
        return def;
    }
}