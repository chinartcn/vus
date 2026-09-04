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
import java.io.FileInputStream;
import java.io.InputStream;
import java.security.MessageDigest;
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
            String expect = readText(hf).trim().toLowerCase();
            if (expect.length() != 64) return false;
            byte[] digest = sha256(dex);
            StringBuilder actual = new StringBuilder(digest.length * 2);
            for (byte x : digest) {
                actual.append(Character.forDigit((x >> 4) & 0xF, 16));
                actual.append(Character.forDigit(x & 0xF, 16));
            }
            return expect.equals(actual.toString());
        } catch (Exception e) {
            return false;
        }
    }

    private static byte[] sha256(File f) throws Exception {
        MessageDigest md = MessageDigest.getInstance("SHA-256");
        InputStream in = new FileInputStream(f);
        try {
            byte[] buf = new byte[8192];
            int r;
            while ((r = in.read(buf)) > 0) md.update(buf, 0, r);
        } finally {
            in.close();
        }
        return md.digest();
    }

    private static String readText(File f) throws Exception {
        byte[] b = new byte[(int) f.length()];
        InputStream in = new FileInputStream(f);
        int off = 0;
        try {
            while (off < b.length) {
                int r = in.read(b, off, b.length - off);
                if (r < 0) break;
                off += r;
            }
        } finally {
            in.close();
        }
        return new String(b, 0, off, "UTF-8");
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