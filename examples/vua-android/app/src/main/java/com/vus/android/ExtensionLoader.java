/*
 * ExtensionLoader.java — DEX 逻辑拓展加载器
 *
 * - 插件 dex 位置：filesDir/plugins/<插件名>.dex（相对路径与 callJava 的文件解析一致；
 *   运行期热更新：用 existing http.download 下载到该路径即可，无需重装 APK）。
 * - 安全：同目录存在 <插件名>.dex.sha256（64 位小写十六进制）时强制校验 SHA-256；
 *   无校验文件则放行（开发模式），由上层决定信任策略。
 * - ClassLoader 复用：按 dex 文件修改时间缓存 VusExtension 实例；文件被替换后（lastModified
 *   变化）自动重建 ClassLoader，避免 ART 卸载限制造成的内存累积。
 * - 约定类名：com.vus.plugins.<插件名首字母大写>Plugin，须有无参构造。
 */
package com.vus.android;

import android.util.Log;

import org.json.JSONObject;

import java.io.File;
import java.util.HashMap;
import java.util.Map;

import dalvik.system.DexClassLoader;

final class ExtensionLoader {

    private static final String TAG = "VusExt";
    private static final String PLUGIN_DIR = "plugins";

    private static final Map<String, Loaded> loaded = new HashMap<>();

    private static final class Loaded {
        long dexStamp;
        VusExtension ext;
    }

    private ExtensionLoader() { }

    /** api 形如 "插件名.操作"（callJava 已剥掉 "ext." 前缀）。返回 JSON 字符串。 */
    static String dispatch(String api, JSONObject args) {
        int dot = api.indexOf('.');
        if (dot <= 0 || dot >= api.length() - 1) return err("拓展调用格式错误: " + api);
        String plugin = api.substring(0, dot);
        String op = api.substring(dot + 1);
        try {
            VusExtension ext = load(plugin);
            if (ext == null) return err("插件未加载: " + plugin);
            return ext.invoke(op, args != null ? args : new JSONObject());
        } catch (Throwable t) {
            return err("插件调用异常: " + t);
        }
    }

    private static VusExtension load(String plugin) {
        if (VuaBridge.appContext == null) return null;
        File dir = new File(VuaBridge.appContext.getFilesDir(), PLUGIN_DIR);
        File dex = new File(dir, plugin + ".dex");
        if (!dex.isFile()) return null;
        if (!verifyIntegrity(dex)) {
            Log.w(TAG, "插件校验失败，拒绝加载: " + dex.getAbsolutePath());
            return null;
        }
        long stamp = dex.lastModified();
        synchronized (loaded) {
            Loaded l = loaded.get(plugin);
            if (l != null && l.dexStamp == stamp) return l.ext;
            try {
                File opt = new File(dir, "opt");
                if (!opt.isDirectory() && !opt.mkdirs()) opt = dir;
                DexClassLoader cl = new DexClassLoader(
                        dex.getAbsolutePath(), opt.getAbsolutePath(), null,
                        VusExtension.class.getClassLoader());
                String clsName = "com.vus.plugins." + capitalize(plugin) + "Plugin";
                Class<?> c = Class.forName(clsName, true, cl);
                Object inst = c.getDeclaredConstructor().newInstance();
                if (!(inst instanceof VusExtension)) {
                    Log.w(TAG, "插件未实现契约: " + clsName);
                    return null;
                }
                Loaded n = new Loaded();
                n.dexStamp = stamp;
                n.ext = (VusExtension) inst;
                loaded.put(plugin, n);
                return n.ext;
            } catch (Throwable t) {
                Log.w(TAG, "插件加载失败: " + plugin + " -> " + t);
                loaded.remove(plugin);
                return null;
            }
        }
    }

    private static String capitalize(String s) {
        if (s == null || s.isEmpty()) return s;
        return Character.toUpperCase(s.charAt(0)) + s.substring(1);
    }

    /** 校验 dex 的 SHA-256（若存在 .sha256 校验文件）。 */
    private static boolean verifyIntegrity(File dex) {
        try {
            File hf = new File(dex.getParentFile(), dex.getName() + ".sha256");
            if (!hf.isFile()) return true;
            String expect = VusIo.readText(hf).trim().toLowerCase();
            if (expect.length() != 64) return false;
            return expect.equals(VusIo.sha256Hex(dex));
        } catch (Exception e) {
            return false;
        }
    }

    private static String err(String msg) {
        try {
            JSONObject o = new JSONObject();
            o.put("ok", false);
            o.put("err", msg);
            return o.toString();
        } catch (Exception e) {
            return "{\"ok\":false}";
        }
    }
}