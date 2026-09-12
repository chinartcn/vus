/*
 * MusicPlayerView.java — VUS 内嵌音乐播放器控件（页内，非跳转）
 *
 * 复用 VusMedia 单例音频播放器（与 音乐_播放 同源）：一行三件套布局
 *   [播放/暂停按钮] [进度条 SeekBar] [时间 mm:ss/mm:ss]
 * 周期轮询 VusMedia.status() 驱动 UI（不重复触发播放）；按钮按当前状态
 * play/pause/resume，进度条拖动 → 音乐_跳转。构造时不自动播放，
 * 避免整树重建时重复拉起播放器。
 *
 * 渲染树字段（均可选，缺省取样式默认值）：
 *   源/循环/音量/高度/背景色/圆角/主色；（英文回退 src/loop/volume/高度/bg/radius/accent）
 * 样式键说明：主色=按钮/进度条着色，背景色=整卡底色，圆角=卡片圆角(px → dp)，高度=卡片高度(dp)。
 */
package com.vus.android;

import android.content.Context;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;

import org.json.JSONObject;

import java.util.Locale;

public final class MusicPlayerView extends LinearLayout {

    private final String src;
    private final boolean loop;
    private final float volume;
    private final int accent;

    private final TextView btn;
    private final SeekBar seek;
    private final TextView timeTv;

    private final Handler tick = new Handler(Looper.getMainLooper());
    private final Runnable poll = new Runnable() {
        @Override public void run() {
            refresh();
            tick.postDelayed(this, 500L);
        }
    };
    private boolean dragging = false;
    private String lastState = "";

    public MusicPlayerView(Context ctx, String src, boolean loop, float volume,
                           int heightDp, int cornerDp, int bg, int accent, boolean dark) {
        super(ctx);
        this.src = src;
        this.loop = loop;
        this.volume = volume;
        this.accent = accent != 0 ? accent : Theme.accent(dark);

        setOrientation(LinearLayout.HORIZONTAL);
        setGravity(Gravity.CENTER_VERTICAL);
        int pad = dp(6);
        setPadding(pad, pad, pad, pad);

        // 卡片外观：背景 + 圆角（可能透明）
        if (bg != 0 || cornerDp > 0) {
            GradientDrawable g = new GradientDrawable();
            g.setColor(bg != 0 ? bg : Color.TRANSPARENT);
            g.setCornerRadius(dp(Math.max(cornerDp, 0)));
            setBackground(g);
        }

        // 播放/暂停按钮（字形文本，无资源依赖）
        btn = new TextView(ctx);
        btn.setText("▶");
        btn.setTextSize(20);
        btn.setTextColor(this.accent);
        btn.setGravity(Gravity.CENTER);
        LayoutParams blp = new LayoutParams(dp(40), dp(40));
        btn.setLayoutParams(blp);
        addView(btn);

        // 进度条
        seek = new SeekBar(ctx);
        seek.setMax(1000);
        seek.setPadding(dp(4), 0, dp(4), 0);
        LayoutParams slp = new LayoutParams(0, LayoutParams.WRAP_CONTENT, 1f);
        seek.setLayoutParams(slp);
        addView(seek);

        // 时间文本
        timeTv = new TextView(ctx);
        timeTv.setText("00:00/00:00");
        timeTv.setTextSize(11);
        timeTv.setTextColor(Theme.muted(dark));
        LayoutParams tlp = new LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT);
        timeTv.setLayoutParams(tlp);
        addView(timeTv);

        // 按钮动作：按当前状态 play / pause / resume
        btn.setOnClickListener(v -> onToggle());
        seek.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar sb, int p, boolean fromUser) { }
            @Override public void onStartTrackingTouch(SeekBar sb) { dragging = true; }
            @Override public void onStopTrackingTouch(SeekBar sb) {
                dragging = false;
                // 跳到进度条对应位置（依赖 duration；status 里含时长）
                JSONObject st = status();
                if (st == null) return;
                double dur = st.optDouble("duration", 0);
                if (dur > 0) {
                    float frac = seek.getProgress() / 1000f;
                    VusMedia.seek(String.format(Locale.US, "%.1f", dur * frac));
                }
            }
        });

        setMinimumHeight(dp(Math.max(heightDp, 48)));
    }

    /** 轮询窗口内的回调状态（VusMedia.status → data JSON）。 */
    private JSONObject status() {
        try {
            JSONObject outer = new JSONObject(VusMedia.status());
            return new JSONObject(outer.optString("data", "{}"));
        } catch (Throwable t) {
            return null;
        }
    }

    /* ---------- 状态轮询 / UI 刷新 ---------- */

    private void refresh() {
        JSONObject st = status();
        if (st == null) return;
        String state = st.optString("state", "idle");
        if (!state.equals(lastState)) {
            lastState = state;
            if ("playing".equals(state)) btn.setText("⏸");
            else if ("paused".equals(state)) btn.setText("▶");
            else btn.setText("▶");
        }
        int durMs = (int) (st.optDouble("duration", 0) * 1000);
        int posMs = (int) (st.optDouble("position", 0) * 1000);
        if (!dragging) {
            if (durMs > 0) {
                seek.setMax(1000);
                seek.setProgress((int) (1000L * Math.min(posMs, durMs) / durMs));
            } else {
                seek.setProgress(0);
            }
        }
        timeTv.setText(fmt(posMs) + "/" + fmt(durMs));
    }

    private void onToggle() {
        JSONObject st = status();
        String state = st != null ? st.optString("state", "idle") : "idle";
        if ("playing".equals(state)) {
            VusMedia.pause();
        } else if ("paused".equals(state)) {
            VusMedia.resume();
        } else {
            // idle / stopped / error：重新加载源（循环/音量参数随构建传入）
            VusMedia.play(src == null ? "" : src, loop, volume);
        }
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

    private static String fmt(int ms) {
        int s = Math.max(ms, 0) / 1000;
        return String.format(Locale.US, "%02d:%02d", s / 60, s % 60);
    }

    private int dp(int v) {
        return (int) (v * getResources().getDisplayMetrics().density);
    }
}