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
 * 注：包名必须与 APK 包名一致，否则 JNI 符号对不上。下面以 com.vus.android 为例。
 */
package com.vus.android;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.HashMap;
import java.util.Iterator;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public final class VuaBridge {

    static {
        // 与 vus_apk.c 生成的壳保持一致：native 库名 libvus_app.so
        System.loadLibrary("vus_app");
    }

    /** 应用 Context：由 MainActivity.onCreate 注入，供 callJava 平台能力桥解析文件路径/目录。 */
    public static Context appContext = null;

    /** native → Java 重绘回调：屏栈变化（界面_显示/返回/返回至）后由 native 调用。
     * MainActivity 在此注册一个 runnable 来重建当前屏的 View。
     */
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
            JSONObject a = new JSONObject(argsJson == null ? "{}" : argsJson);
            if ("file.read".equals(api)) {
                File f = resolve(a.optString("path"));
                if (!f.isFile()) return err("文件不存在");
                return ok(readUtf8(f));
            }
            if ("file.write".equals(api) || "file.append".equals(api)) {
                File f = resolve(a.optString("path"));
                new File(f.getParent()).mkdirs();
                byte[] b = a.optString("content").getBytes("UTF-8");
                writeBytes(f, b, "file.append".equals(api));
                return ok("0");
            }
            if ("file.exists".equals(api)) {
                return ok(resolve(a.optString("path")).exists() ? "1" : "0");
            }
            if ("file.delete".equals(api)) {
                return ok(resolve(a.optString("path")).delete() ? "0" : "-1");
            }
            if ("file.isdir".equals(api)) {
                return ok(resolve(a.optString("path")).isDirectory() ? "true" : "false");
            }
            if ("file.list".equals(api)) {
                String[] names = resolve(a.optString("path")).list();
                if (names == null) return ok("");
                StringBuilder sb = new StringBuilder();
                for (String n : names) sb.append(n).append('\n');
                return ok(sb.toString());
            }
            if ("http.get".equals(api)) {
                int timeout = a.optInt("timeout", 30);
                int retry = a.optInt("retry", 2);
                byte[] b = runHttp(a.optString("url"), null, timeout,
                        headersOf(a.optJSONObject("headers")), retry);
                return ok(b == null ? "" : new String(b, "UTF-8"));
            }
            if ("http.post".equals(api)) {
                int timeout = a.optInt("timeout", 30);
                int retry = a.optInt("retry", 2);
                byte[] b = runHttp(a.optString("url"), a.optString("data"), timeout,
                        headersOf(a.optJSONObject("headers")), retry);
                return ok(b == null ? "" : new String(b, "UTF-8"));
            }
            /* 通用请求：method=GET|POST，headers 自定义请求头（如 Authorization token 认证）、
             * timeout 秒、retry 重试次数。覆盖「认证/超时/重试」类需求。 */
            if ("http.request".equals(api)) {
                String method = "GET".equalsIgnoreCase(a.optString("method", "GET")) ? "GET" : "POST";
                int timeout = a.optInt("timeout", 30);
                int retry = a.optInt("retry", 2);
                byte[] b = runHttp(a.optString("url"),
                        "POST".equals(method) ? a.optString("data", "") : null,
                        timeout, headersOf(a.optJSONObject("headers")), retry);
                return ok(b == null ? "" : new String(b, "UTF-8"));
            }
            if ("http.upload".equals(api)) {
                // multipart/form-data 文件上传：url + 本地文件 path + 附加字段 fields + 头 headers
                boolean up = uploadMultipart(a.optString("url"), a.optString("path"),
                        a.optJSONObject("fields"), headersOf(a.optJSONObject("headers")),
                        a.optInt("timeout", 60));
                return up ? ok("1") : err("上传失败");
            }
            if ("http.download".equals(api)) {
                byte[] b = runHttp(a.optString("url"), null, 60, null, 2);
                if (b == null) return ok("0");
                File f = resolve(a.optString("path"));
                new File(f.getParent()).mkdirs();
                writeBytes(f, b, false);
                return ok("1");
            }
            // DEX 逻辑拓展：api 形如 "ext.<插件名>.<操作>"，交给 ExtensionLoader 动态加载调用。
            // 插件 dex 位于 filesDir/plugins/<插件名>.dex，支持运行期热更新（配合 http.download）。
            if (api.startsWith("ext.")) {
                return ExtensionLoader.dispatch(api.substring(4), a);
            }
            return err("未知能力: " + api);
        } catch (Exception e) {
            return err(String.valueOf(e));
        }
    }

    private static String ok(String data) {
        try {
            JSONObject o = new JSONObject();
            o.put("ok", true);
            o.put("data", data == null ? "" : data);
            return o.toString();
        } catch (Exception e) { return "{\"ok\":false,\"err\":\"encode\"}"; }
    }

    private static String err(String msg) {
        try {
            JSONObject o = new JSONObject();
            o.put("ok", false);
            o.put("err", msg == null ? "" : msg);
            return o.toString();
        } catch (Exception e) { return "{\"ok\":false}"; }
    }

    /** 相对名 → 应用文件目录下的绝对文件；绝对路径则原样使用。 */
    private static File resolve(String name) {
        File f = new File(name == null ? "" : name);
        if (f.isAbsolute() || appContext == null) return f;
        return new File(appContext.getFilesDir(), name);
    }

    /* ---- 文件 IO ---- */
    private static String readUtf8(File f) throws Exception {
        byte[] b = new byte[(int) f.length()];
        InputStream in = new java.io.FileInputStream(f);
        int off = 0;
        while (off < b.length) {
            int r = in.read(b, off, b.length - off);
            if (r < 0) break;
            off += r;
        }
        in.close();
        return new String(b, 0, off, "UTF-8");
    }

    private static void writeBytes(File f, byte[] b, boolean append) throws Exception {
        FileOutputStream fo = new FileOutputStream(f, append);
        fo.write(b);
        fo.close();
    }

    /* ---- HTTP（主线程规避：Android 禁止主线程联网时转子线程同步等待） ---- */
    private static boolean isMainThread() {
        return Looper.myLooper() == Looper.getMainLooper();
    }

    /** 线程安全请求入口：headers 自定义请求头、retry 重试；主线程自动转子线程等待。 */
    private static byte[] runHttp(final String url, final String data, final int timeoutSec,
                                  final Map<String, String> headers, final int retry) throws Exception {
        if (!isMainThread()) return doHttp(url, data, timeoutSec, headers, retry);
        final byte[][] holder = new byte[1][];
        final Throwable[] terr = new Throwable[1];
        final CountDownLatch latch = new CountDownLatch(1);
        new Thread(() -> {
            try { holder[0] = doHttp(url, data, timeoutSec, headers, retry); }
            catch (Throwable t) { terr[0] = t; }
            finally { latch.countDown(); }
        }).start();
        try {
            latch.await((long) timeoutSec + 60L, TimeUnit.SECONDS);
        } catch (InterruptedException ie) {
            return null;
        }
        if (terr[0] != null) return null;
        return holder[0];
    }

    private static byte[] doHttp(String url, String data, int timeoutSec,
                                 Map<String, String> headers, int retry) throws Exception {
        if (retry < 1) retry = 1;
        Throwable last = null;
        for (int attempt = 0; attempt < retry; attempt++) {
            try {
                return doHttpOnce(url, data, timeoutSec, headers);
            } catch (Throwable t) {
                last = t;                       // 超时/IO 错误：按 retry 次数重试
            }
        }
        if (last != null) throw new Exception(last);
        return null;
    }

    private static byte[] doHttpOnce(String url, String data, int timeoutSec,
                                     Map<String, String> headers) throws Exception {
        HttpURLConnection conn = (HttpURLConnection) new URL(url).openConnection();
        conn.setConnectTimeout(timeoutSec * 1000);
        conn.setReadTimeout(timeoutSec * 1000);
        conn.setRequestProperty("User-Agent", "VUS-Android/1.0");
        if (headers != null) {
            for (Map.Entry<String, String> e : headers.entrySet()) {
                if (e.getKey() != null && e.getValue() != null)
                    conn.setRequestProperty(e.getKey(), e.getValue());
            }
        }
        if (data != null) {
            conn.setRequestMethod("POST");
            conn.setDoOutput(true);
            conn.getOutputStream().write(data.getBytes("UTF-8"));
        } else {
            conn.setRequestMethod("GET");
        }
        int code = conn.getResponseCode();
        if (code != 200) return null;
        ByteArrayOutputStream bos = new ByteArrayOutputStream();
        InputStream in = conn.getInputStream();
        byte[] buf = new byte[8192];
        int r;
        while ((r = in.read(buf)) > 0) bos.write(buf, 0, r);
        in.close();
        return bos.toByteArray();
    }

    /** JSON 对象 → 请求头 Map（{"Authorization":"Bearer x", ...}）。 */
    private static Map<String, String> headersOf(JSONObject h) {
        if (h == null) return null;
        Map<String, String> out = new HashMap<>();
        Iterator<String> keys = h.keys();
        while (keys.hasNext()) {
            String k = keys.next();
            out.put(k, h.optString(k));
        }
        return out.isEmpty() ? null : out;
    }

    /** multipart/form-data 文件上传（无第三方库，手写 multipart body）。
     * path 为本地文件（相对 filesDir 或绝对路径）；fields 为附加表单字段。 */
    private static boolean uploadMultipart(final String url, final String path,
                                           final JSONObject fields,
                                           final Map<String, String> headers,
                                           final int timeoutSec) {
        if (url == null || url.isEmpty() || path == null || path.isEmpty()) return false;
        final boolean[] holder = new boolean[1];
        if (!isMainThread()) {
            try { holder[0] = doUpload(url, path, fields, headers, timeoutSec); }
            catch (Throwable t) { holder[0] = false; }
            return holder[0];
        }
        final CountDownLatch latch = new CountDownLatch(1);
        new Thread(() -> {
            try { holder[0] = doUpload(url, path, fields, headers, timeoutSec); }
            catch (Throwable t) { holder[0] = false; }
            finally { latch.countDown(); }
        }).start();
        try { latch.await((long) timeoutSec + 60L, TimeUnit.SECONDS); }
        catch (InterruptedException ie) { return false; }
        return holder[0];
    }

    private static boolean doUpload(String url, String path, JSONObject fields,
                                    Map<String, String> headers, int timeoutSec) throws Exception {
        File file = resolve(path);
        if (!file.isFile()) return false;
        String boundary = "----VUS" + System.currentTimeMillis();

        ByteArrayOutputStream body = new ByteArrayOutputStream();
        byte[] CRLF = "\r\n".getBytes("UTF-8");
        // 附加表单字段
        if (fields != null) {
            Iterator<String> ks = fields.keys();
            while (ks.hasNext()) {
                String k = ks.next();
                body.write(("--" + boundary + CRLF).getBytes("UTF-8"));
                body.write(("Content-Disposition: form-data; name=\"" + k + "\"" + CRLF + CRLF).getBytes("UTF-8"));
                body.write(fields.optString(k).getBytes("UTF-8"));
                body.write(CRLF);
            }
        }
        // 文件部分
        body.write(("--" + boundary + CRLF).getBytes("UTF-8"));
        String filename = file.getName();
        body.write(("Content-Disposition: form-data; name=\"file\"; filename=\"" + filename + "\"" + CRLF).getBytes("UTF-8"));
        body.write(("Content-Type: application/octet-stream" + CRLF + CRLF).getBytes("UTF-8"));
        InputStream in = new java.io.FileInputStream(file);
        byte[] buf = new byte[16384];
        int r;
        while ((r = in.read(buf)) > 0) body.write(buf, 0, r);
        in.close();
        body.write(CRLF);
        body.write(("--" + boundary + "--" + CRLF).getBytes("UTF-8"));
        byte[] payload = body.toByteArray();

        HttpURLConnection conn = (HttpURLConnection) new URL(url).openConnection();
        conn.setConnectTimeout(timeoutSec * 1000);
        conn.setReadTimeout(timeoutSec * 1000);
        conn.setRequestMethod("POST");
        conn.setDoOutput(true);
        conn.setRequestProperty("Content-Type", "multipart/form-data; boundary=" + boundary);
        conn.setRequestProperty("User-Agent", "VUS-Android/1.0");
        if (headers != null) {
            for (Map.Entry<String, String> e : headers.entrySet()) {
                if (e.getKey() != null && e.getValue() != null)
                    conn.setRequestProperty(e.getKey(), e.getValue());
            }
        }
        OutputStream os = conn.getOutputStream();
        os.write(payload);
        os.flush();
        os.close();
        int code = conn.getResponseCode();
        InputStream resp = conn.getInputStream();
        while (resp.read() != -1) { }            // 读完响应便于连接复用
        resp.close();
        return code >= 200 && code < 300;
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