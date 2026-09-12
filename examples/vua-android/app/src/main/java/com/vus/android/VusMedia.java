/*
 * VusMedia.java — VUS 媒体播放（音频后台播放 + 视频全屏播放）
 *
 * 平台能力桥 media.* / video.* 的 Java 实现（对应 VuaBridge.callJava 分发）：
 *   audio.play/stop/pause/resume/seek/status —— 单例 MediaPlayer，后台音乐。
 *   video.play —— 全屏 VideoActivity（MediaPlayer + SurfaceView 内置控件的简化封装）。
 *
 * 线程模型：callJava 可能来自 native 线程，MediaPlayer 全部操作统一切主线程
 * 执行（sMain），并以 CountDownLatch 同步等待结果，保证调用时序可控。
 *
 * 源（src）：本地相对路径（相对应用 filesDir，与 native cwd 一致）、绝对路径、
 * 或 http(s):// 网络地址。本地文件可用同步 prepare，网络统一 prepareAsync。
 */
package com.vus.android;

import android.content.Context;
import android.content.Intent;
import android.media.MediaPlayer;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import com.google.gson.Gson;
import com.google.gson.JsonObject;

import java.io.File;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

public final class VusMedia {

    private static final String TAG = "VusMedia";
    private static final Gson GSON = new Gson();
    private static final Handler sMain = new Handler(Looper.getMainLooper());

    /** 音频播放器单例（多媒体操作只能在创建它的线程——主线程——执行）。 */
    private static MediaPlayer sPlayer = null;
    private static boolean sLoop = false;
    private static String sSrc = "";        // 当前源
    private static String sState = "idle";  // idle/playing/paused/error
    private static String sErr = "";

    private VusMedia() { }

    /** 在主线程执行 fn 并同步等待结果（callJava 调用方可能是 native 线程）。
     * 若调用方已是主线程（如播放器控件 UI 刷新）则直接执行，避免 self-deadlock。 */
    private static String syncOnMain(final java.util.concurrent.Callable<String> fn) {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            try {
                return fn.call();
            } catch (Throwable t) {
                Log.w(TAG, "media 操作失败", t);
                return err(String.valueOf(t));
            }
        }
        final AtomicReference<String> out = new AtomicReference<>();
        final CountDownLatch latch = new CountDownLatch(1);
        sMain.post(() -> {
            try {
                out.set(fn.call());
            } catch (Throwable t) {
                out.set(err(String.valueOf(t)));
                Log.w(TAG, "media 操作失败", t);
            } finally {
                latch.countDown();
            }
        });
        try {
            if (!latch.await(8, TimeUnit.SECONDS)) return err("媒体操作超时");
        } catch (InterruptedException e) {
            return err("媒体操作被中断");
        }
        return out.get() != null ? out.get() : err("媒体操作无结果");
    }

    /* ---- 音频：播放 ----
     * 参数：src=路径或URL，loop=0/1，volume=0~1。先释放旧播放器再建新实例。 */
    public static String play(final String src, final boolean loop, final float volume) {
        if (src == null || src.isEmpty()) return err("缺少 src");
        return syncOnMain(() -> {
            releaseQuietly();
            sLoop = loop;
            sSrc = src;
            sErr = "";
            try {
                MediaPlayer mp = new MediaPlayer();
                mp.setAudioStreamType(android.media.AudioManager.STREAM_MUSIC);
                mp.setLooping(loop);
                mp.setVolume(volume, volume);
                mp.setOnPreparedListener(p -> {
                    sState = "playing";
                    p.start();
                });
                mp.setOnCompletionListener(p -> {
                    if (!sLoop) sState = "stopped";
                });
                mp.setOnErrorListener((p, what, extra) -> {
                    sState = "error";
                    sErr = "error(" + what + "," + extra + ")";
                    return true;
                });
                if (src.startsWith("http://") || src.startsWith("https://")) {
                    mp.setDataSource(src);
                } else {
                    File f = resolve(src);
                    if (!f.isFile()) { releaseQuietly(); return err("文件不存在: " + src); }
                    mp.setDataSource(f.getAbsolutePath());
                }
                sPlayer = mp;
                sState = "loading";
                mp.prepareAsync();
                return ok("0");
            } catch (Throwable t) {
                releaseQuietly();
                return err(String.valueOf(t));
            }
        });
    }

    public static String stop() {
        return syncOnMain(() -> {
            releaseQuietly();
            sState = "idle";
            return ok("0");
        });
    }

    public static String pause() {
        return syncOnMain(() -> {
            if (sPlayer != null && sPlayer.isPlaying()) {
                sPlayer.pause();
                sState = "paused";
            }
            return ok("0");
        });
    }

    public static String resume() {
        return syncOnMain(() -> {
            if (sPlayer != null && !sPlayer.isPlaying()) {
                if ("error".equals(sState) || sSrc.isEmpty()) return err("无可用播放器");
                sPlayer.start();
                sState = "playing";
            }
            return ok("0");
        });
    }

    /** 跳转：pos 为秒数（可用浮点）。 */
    public static String seek(final String pos) {
        return syncOnMain(() -> {
            if (sPlayer == null) return err("未在播放");
            int ms = (int) (parseFloat(pos, 0) * 1000f);
            ms = Math.max(0, ms);
            int dur = sPlayer.getDuration();
            if (dur > 0 && ms > dur) ms = dur;
            sPlayer.seekTo(ms);
            return ok("0");
        });
    }

    /** 状态 JSON：{"state","duration","position","src"}（秒级，浮点）。 */
    public static String status() {
        return syncOnMain(() -> {
            JsonObject o = new JsonObject();
            o.addProperty("state", sState);
            o.addProperty("src", sSrc);
            o.addProperty("loop", sLoop);
            o.addProperty("err", sErr);
            if (sPlayer != null) {
                try {
                    int d = sPlayer.getDuration();
                    int p = sPlayer.getCurrentPosition();
                    o.addProperty("duration", d > 0 ? d / 1000.0 : 0);
                    o.addProperty("position", p > 0 ? p / 1000.0 : 0);
                } catch (Throwable t) {
                    o.addProperty("duration", 0);
                    o.addProperty("position", 0);
                }
            } else {
                o.addProperty("duration", 0);
                o.addProperty("position", 0);
            }
            return ok(GSON.toJson(o));
        });
    }

    /* ---- 视频：全屏播放（VideoActivity 独立界面，自带播放控制） ---- */

    /** 启动全屏视频播放器。src=路径/URL。返回 "0" 或错误。 */
    public static String video(final String src) {
        if (src == null || src.isEmpty()) return err("缺少 src");
        return syncOnMain(() -> {
            if (VuaBridge.sActivity == null) return err("无宿主 Activity");
            Context ctx = VuaBridge.sActivity.getApplicationContext();
            Intent i = new Intent(ctx, MediaActivity.class);
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            String uri = src;
            if (!src.startsWith("http://") && !src.startsWith("https://")) {
                File f = resolve(src);
                if (!f.isFile()) return err("文件不存在: " + src);
                uri = Uri.fromFile(f).toString();
            }
            i.putExtra("src", uri);
            ctx.startActivity(i);
            return ok("0");
        });
    }

    /* ---- 帮助器 ---- */

    /** 相对名 → 应用 filesDir 下的绝对文件；绝对路径原样使用。 */
    private static File resolve(String name) {
        File f = new File(name);
        if (f.isAbsolute() || VuaBridge.appContext == null) return f;
        return new File(VuaBridge.appContext.getFilesDir(), name);
    }

    private static float parseFloat(String s, float def) {
        if (s == null) return def;
        try {
            return Float.parseFloat(s.trim());
        } catch (Throwable t) {
            return def;
        }
    }

    /** 释放当前播放器（不改变状态字段，由调用方置位）。 */
    private static void releaseQuietly() {
        if (sPlayer != null) {
            try {
                sPlayer.stop();
            } catch (Throwable ignored) { }
            try {
                sPlayer.release();
            } catch (Throwable ignored) { }
            sPlayer = null;
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
}