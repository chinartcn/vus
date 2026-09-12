/*
 * MediaActivity.java — VUS 全屏视频播放器（框架 API，无第三方依赖）
 *
 * 由 VusMedia.video() 启动：Intent extra "src" 传视频 uri（file:// 或 http(s)://）。
 * 布局：全屏 VideoView（自动缩放适配），内置播放进度/控制（MediaController）。
 * 生命周期：onPause 暂停播放，onDestroy 释放——回到 VUS 界面后按 主窗口 lifecycle 恢复。
 */
package com.vus.android;

import android.app.Activity;
import android.media.MediaPlayer;
import android.os.Bundle;
import android.widget.MediaController;
import android.widget.VideoView;

public final class MediaActivity extends Activity {

    private VideoView mVideo;
    private MediaController mController;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // 全屏无标题
        requestWindowFeature(android.view.Window.FEATURE_NO_TITLE);
        getWindow().setFlags(android.view.WindowManager.LayoutParams.FLAG_FULLSCREEN,
                android.view.WindowManager.LayoutParams.FLAG_FULLSCREEN);

        mVideo = new VideoView(this);
        setContentView(mVideo);

        String src = getIntent().getStringExtra("src");
        if (src == null || src.isEmpty()) {
            finish();
            return;
        }

        mController = new MediaController(this);
        mController.setAnchorView(mVideo);
        mVideo.setMediaController(mController);

        mVideo.setVideoPath(src);
        mVideo.setOnPreparedListener(this::onPrepared);
        mVideo.setOnErrorListener((mp, what, extra) -> {
            finish();
            return true;
        });
        mVideo.setOnCompletionListener(mp -> finish());
        mVideo.requestFocus();
    }

    private void onPrepared(MediaPlayer mp) {
        // 交由 VideoView 自动开始播放（内置 MediaController 进度联动）
    }

    @Override
    protected void onPause() {
        super.onPause();
        if (mVideo != null && mVideo.isPlaying()) mVideo.pause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (mVideo != null && !mVideo.isPlaying()) {
            try {
                mVideo.start();
            } catch (Throwable ignored) { }
        }
    }

    @Override
    protected void onDestroy() {
        try {
            if (mController != null) mController.removeAllViews();
        } catch (Throwable ignored) { }
        if (mVideo != null) {
            try {
                mVideo.stopPlayback();
            } catch (Throwable ignored) { }
        }
        super.onDestroy();
    }
}