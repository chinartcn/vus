/*
 * VideoPlayerView.java — VUS 内嵌视频播放器控件（页内，非全屏跳转）
 *
 * 页内 VideoView + 控制条：上部视频画面，下部一行
 *   [播放/暂停] [进度条 SeekBar] [时间 mm:ss/mm:ss] [⛶ 全屏]
 * 周期轮询 VideoView 当前位置驱动进度；全屏按钮转 VusMedia.video() 拉起
 * 独立全屏 MediaActivity（同一 src）。构造时不自动播放（除非显式 自动播放=1，
 * 整树重建会重新 prepare，自动播放场景需脚本侧自行控制）。
 *
 * 渲染树字段（均可选，缺省取样式默认值）：
 *   源/自动播放/高度/圆角/背景色/主色；（英文回退 src/autoplay/height/radius/corner/bg/background/accent）
 * 样式键说明：高度=视频区高度(dp)，圆角=卡片圆角，背景色=整卡底色，主色=按钮/进度条着色。
 */
package com.vus.android;

import android.content.Context;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.media.MediaPlayer;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;
import android.widget.VideoView;

import java.io.File;
import java.util.Locale;

public final class VideoPlayerView extends LinearLayout {

    private final String src;
    private final boolean autoplay;
    private final int accent;

    private final VideoView video;
    private final TextView btn;
    private final SeekBar seek;
    private final TextView timeTv;
    private final TextView fullBtn;

    private final Handler tick = new Handler(Looper.getMainLooper());
    private final Runnable poll = new Runnable() {
        @Override public void run() {
            refresh();
            tick.postDelayed(this, 500L);
        }
    };
    private boolean dragging = false;
    private boolean userPaused = false;   // 用户手动暂停：prepare 完成后不自动续播
    private boolean prepared = false;     // 源已成功 prepare
    private boolean failed = false;

    public VideoPlayerView(Context ctx, String src, boolean autoplay,
                           int heightDp, int cornerDp, int bg, int accent, boolean dark) {
        super(ctx);
        this.src = src == null ? "" : src;
        this.autoplay = autoplay;
        this.accent = accent != 0 ? accent : Theme.accent(dark);

        setOrientation(LinearLayout.VERTICAL);

        // 卡片外观：背景 + 圆角（可能透明）
        if (bg != 0 || cornerDp > 0) {
            GradientDrawable g = new GradientDrawable();
            g.setColor(bg != 0 ? bg : Color.TRANSPARENT);
            g.setCornerRadius(dp(Math.max(cornerDp, 0)));
            setBackground(g);
        }

        // 视频画面区（高度由字段指定，缺省 240dp）
        video = new VideoView(ctx);
        video.setBackgroundColor(0xFF000000);
        addView(video, new LayoutParams(LayoutParams.MATCH_PARENT, dp(Math.max(heightDp, 120))));

        // 控制条
        LinearLayout bar = new LinearLayout(ctx);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        int pad = dp(6);
        bar.setPadding(pad, pad, pad, pad);
        addView(bar, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT));

        // 播放/暂停按钮（字形文本，无资源依赖）
        btn = new TextView(ctx);
        btn.setText("▶");
        btn.setTextSize(18);
        btn.setTextColor(this.accent);
        btn.setGravity(Gravity.CENTER);
        btn.setLayoutParams(new LayoutParams(dp(36), dp(36)));
        bar.addView(btn);

        // 进度条
        seek = new SeekBar(ctx);
        seek.setMax(1000);
        seek.setPadding(dp(4), 0, dp(4), 0);
        seek.setProgressTintList(android.content.res.ColorStateList.valueOf(this.accent));
        seek.setThumbTintList(android.content.res.ColorStateList.valueOf(this.accent));
        seek.setLayoutParams(new LayoutParams(0, LayoutParams.WRAP_CONTENT, 1f));
        bar.addView(seek);

        // 时间文本
        timeTv = new TextView(ctx);
        timeTv.setText("00:00/00:00");
        timeTv.setTextSize(11);
        timeTv.setTextColor(Theme.muted(dark));
        timeTv.setLayoutParams(new LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT));
        bar.addView(timeTv);

        // 全屏按钮
        fullBtn = new TextView(ctx);
        fullBtn.setText("⛶");
        fullBtn.setTextSize(18);
        fullBtn.setTextColor(this.accent);
        fullBtn.setGravity(Gravity.CENTER);
        fullBtn.setPadding(dp(6), 0, 0, 0);
        fullBtn.setLayoutParams(new LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT));
        bar.addView(fullBtn);

        // 交互
        btn.setOnClickListener(v -> onToggle());
        seek.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar sb, int p, boolean fromUser) {
                if (fromUser && prepared) {
                    int dur = video.getDuration();
                    if (dur > 0) timeTv.setText(fmt(dur * p / 1000) + "/" + fmt(dur));
                }
            }
            @Override public void onStartTrackingTouch(SeekBar sb) { dragging = true; }
            @Override public void onStopTrackingTouch(SeekBar sb) {
                dragging = false;
                if (prepared) {
                    int dur = video.getDuration();
                    if (dur > 0) video.seekTo(dur * seek.getProgress() / 1000);
                }
            }
        });
        fullBtn.setOnClickListener(v -> {
            // 全屏：复用 VusMedia.video() 拉起 MediaActivity（同样解析相对路径）
            VusMedia.video(this.src);
        });

        video.setOnPreparedListener(mp -> onPrepared(mp));
        video.setOnErrorListener((mp, what, extra) -> {
            failed = true;
            prepared = false;
            timeTv.setText("加载失败");
            btn.setText("▶");
            destroyVideo();
            return true;
        });
        video.setOnCompletionListener(mp -> btn.setText("▶"));
    }

    private void onPrepared(MediaPlayer mp) {
        prepared = true;
        failed = false;
        if (autoplay && !userPaused) {
            mp.start();
        }
    }

    /* ---------- 状态轮询 / UI 刷新 ---------- */

    private void refresh() {
        if (failed) return;
        int posMs = prepared ? safePos() : 0;
        int durMs = prepared ? safeDur() : 0;
        boolean playing = prepared && video.isPlaying();
        btn.setText(playing ? "⏸" : "▶");
        if (!dragging && durMs > 0) {
            seek.setProgress((int) (1000L * Math.min(posMs, durMs) / durMs));
        }
        timeTv.setText(fmt(posMs) + "/" + fmt(durMs));
    }

    private void onToggle() {
        if (!prepared) {
            // 首次/失败后：重新装载源（自动播放语义由用户按播放触发）
            userPaused = false;
            load();
            return;
        }
        if (video.isPlaying()) {
            video.pause();
            userPaused = true;
        } else {
            video.start();
            userPaused = false;
        }
    }

    /** 装载源：相对路径 → 应用 filesDir 绝对路径；http(s)/file: 原样。 */
    private void load() {
        String path = resolvePath(src);
        if (path.isEmpty()) {
            timeTv.setText("缺少源");
            return;
        }
        prepared = false;
        failed = false;
        video.setVideoPath(path);
        video.requestFocus();
    }

    private int safePos() {
        try { return video.getCurrentPosition(); } catch (Throwable t) { return 0; }
    }

    private int safeDur() {
        try {
            int d = video.getDuration();
            return d > 0 ? d : 0;
        } catch (Throwable t) {
            return 0;
        }
    }

    /** 失败后释放内部播放器，避免 MediaPlayer 泄漏（下次播放重载）。 */
    private void destroyVideo() {
        try {
            video.stopPlayback();
        } catch (Throwable ignored) { }
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        tick.removeCallbacks(poll);
        tick.post(poll);
    }

    @Override
    protected void onDetachedFromWindow() {
        tick.removeCallbacks(poll);
        super.onDetachedFromWindow();
    }

    /* ---------- 帮助器 ---------- */

    private static String resolvePath(String src) {
        if (src == null || src.isEmpty()) return "";
        if (src.startsWith("http://") || src.startsWith("https://")
                || src.startsWith("file:")) {
            return src;
        }
        File f = new File(src);
        if (f.isAbsolute()) return src;
        if (VuaBridge.appContext == null) return src;
        return new File(VuaBridge.appContext.getFilesDir(), src).getAbsolutePath();
    }

    private static String fmt(int ms) {
        int s = Math.max(ms, 0) / 1000;
        return String.format(Locale.US, "%02d:%02d", s / 60, s % 60);
    }

    private int dp(int v) {
        return (int) (v * getResources().getDisplayMetrics().density);
    }
}