/*
 * MainActivity.java — VUA 组件流 APK 入口（替换 vus_apk.c 的老式 TextView 壳）
 *
 * 流程：vuaInit() 跑 .vus 逻辑并建首页 → 取渲染树 → VuaRenderer 建 View；
 * 点击由按钮的 OnClick 调回 native（触发事件），native 换屏后 Java 重取渲染树重建。
 */
package com.vus.android;

import android.app.Activity;
import android.content.res.AssetManager;
import android.os.Build;
import android.os.Bundle;
import android.os.StrictMode;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;

public class MainActivity extends Activity {

    private FrameLayout content;
    private VuaRenderer renderer;

    /** 从 assets 释放 .vua/.json/.jpg/.png/.dex 到文件目录（递归保留目录结构），
     *  供 native / 图片加载器 / DEX 插件加载器使用。动态枚举全部资源，避免新增页面漏拷。 */
    private void extractAssets() {
        try {
            AssetManager am = getAssets();
            File dir = getFilesDir();
            copyTree(am, "", dir);
        } catch (Exception ignored) { }
    }

    private static void copyTree(AssetManager am, String path, File outDir) throws Exception {
        String[] names = am.list(path);
        if (names == null) return;
        String prefix = path.isEmpty() ? "" : path + "/";
        for (String n : names) {
            String child = prefix + n;
            if (isAssetDir(am, child)) {
                File sub = new File(outDir, n);
                if (!sub.isDirectory() && !sub.mkdirs()) continue;
                copyTree(am, child, sub);
            } else if (n.endsWith(".vua") || n.endsWith(".json") || n.endsWith(".jpg")
                    || n.endsWith(".png") || n.endsWith(".dex")) {
                InputStream in = am.open(child);
                File out = new File(outDir, n);
                FileOutputStream fos = new FileOutputStream(out);
                byte[] b = new byte[8192];
                int r;
                while ((r = in.read(b)) > 0) fos.write(b, 0, r);
                fos.close();
                in.close();
            }
        }
    }

    private static boolean isAssetDir(AssetManager am, String path) {
        try {
            String[] sub = am.list(path);
            return sub != null && sub.length > 0;
        } catch (Exception e) {
            return false;
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // Android 7+ 禁止 file:// URI 跨应用，安装 APK 时放行
        if (Build.VERSION.SDK_INT >= 24) {
            StrictMode.setVmPolicy(new StrictMode.VmPolicy.Builder().build());
        }

        // 平台能力桥（callJava）解析相对路径/文件目录需要应用 Context
        VuaBridge.appContext = getApplicationContext();

        content = new FrameLayout(this);
        content.setLayoutParams(new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        setContentView(content);

        renderer = new VuaRenderer(this, content);

        // native 换屏（界面_显示/返回）后回调 onNativeRerender；切到主线程重建 View。
        VuaBridge.onRerender = () -> runOnUiThread(this::renderCurrent);

        // 注册检查更新回调
        VuaBridge.onCheckUpdate = () -> runOnUiThread(() -> new UpdateChecker(this).check());

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
        try {
            // 版本号协议：只把内容指纹交给 renderer，命中缓存时连渲染树 JSON 都不用取
            renderer.render(VuaBridge.vuaRenderHash());
        } finally {
            VuaBridge.renderHandled();
        }
    }

    /** 由 native 事件触发检查更新 */
    public void checkUpdate() {
        runOnUiThread(() -> {
            new UpdateChecker(this).check();
        });
    }
}