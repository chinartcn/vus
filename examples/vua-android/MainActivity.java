/*
 * MainActivity.java — VUA 组件流 APK 入口（替换 vus_apk.c 的老式 TextView 壳）
 *
 * 流程：vuaInit() 跑 .vus 逻辑并建首页 → 取渲染树 → VuaRenderer 建 View；
 * 点击由按钮的 OnClick 调回 native（触发事件），native 换屏后 Java 重取渲染树重建。
 */
package com.vus.android;

import android.app.Activity;
import android.content.res.AssetManager;
import android.os.Bundle;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;

public class MainActivity extends Activity {

    private FrameLayout content;
    private VuaRenderer renderer;

    /** 从 assets 释放 .vua/.json 到文件目录，供 native 以相对路径读取。
     *  动态枚举全部资源文件，避免新增页面漏拷（曾因漏 vua_logic.vua 导致导航无反应）。 */
    private void extractAssets() {
        try {
            AssetManager am = getAssets();
            File dir = getFilesDir();
            String[] names = am.list("");
            if (names == null) return;
            for (String n : names) {
                if (!n.endsWith(".vua") && !n.endsWith(".json")) continue;
                InputStream in = am.open(n);
                File out = new File(dir, n);
                FileOutputStream fos = new FileOutputStream(out);
                byte[] b = new byte[8192];
                int r;
                while ((r = in.read(b)) > 0) fos.write(b, 0, r);
                fos.close();
                in.close();
            }
        } catch (Exception ignored) { }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        content = new FrameLayout(this);
        content.setLayoutParams(new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        setContentView(content);

        renderer = new VuaRenderer(this, content);

        // native 换屏（界面_显示/返回）后回调 onNativeRerender；切到主线程重建 View。
        VuaBridge.onRerender = () -> runOnUiThread(this::renderCurrent);

        // 释放 .vua 到文件目录，并把工作目录切到那里，再启动 native（建会话 + 跑 .vus）
        extractAssets();
        VuaBridge.vuaSetRootDir(getFilesDir().getAbsolutePath());

        // 启动 native：建 VuaSession 并运行 .vus（界面_显示 首页 / 界面_绑定 事件）
        int rc = VuaBridge.vuaInit();
        if (rc != 0) {
            TextView tv = new TextView(this);
            tv.setText("vuaInit 失败 rc=" + rc);
            content.addView(tv);
            return;
        }
        // 渲染当前屏
        renderCurrent();
    }

    private void renderCurrent() {
        String tree = VuaBridge.vuaRenderTree();
        renderer.render(tree, "(空界面)");
    }
}