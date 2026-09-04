/*
 * ImageLoader.java — 远程图片异步加载 + 内存/磁盘两级缓存
 *
 * 反馈项「远程图片异步加载」：原 imageView 用 BitmapFactory 同步解码，
 * 网络图会卡 UI 线程、无缓存。本类：
 *   - 线程池异步下载/解码（不阻塞 UI 线程）；
 *   - 内存 LruCache + 磁盘缓存（Context.getCacheDir()/img）；
 *   - ImageView 打 url tag 防错位复用（列表滚动时旧请求结果不占新位置）。
 *
 * 注：不用 lambda/匿名子类（d8 8.2 dev 对匿名泛型子类的 desugar 有 NPE bug），
 * 一律显式静态嵌套类实现。
 */
package com.vus.android;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.os.Handler;
import android.os.Looper;
import android.util.LruCache;
import android.widget.ImageView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.security.MessageDigest;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class ImageLoader {

    private static final ImageLoader INSTANCE = new ImageLoader();

    public static ImageLoader get() { return INSTANCE; }

    /** 内存缓存：LruCache 的子类抽成命名静态类（泛型匿名子类会触发 d8 NPE）。 */
    private static final class MemCache extends LruCache<String, Bitmap> {
        MemCache(int maxBytes) { super(maxBytes); }
        @Override protected int sizeOf(String key, Bitmap value) {
            return value.getByteCount();
        }
    }

    /** 一次加载任务：磁盘命中则解码；否则网络下载→落盘→解码→入内存缓存。 */
    private static final class LoadTask implements Runnable {
        private final String url;
        private final ImageView iv;
        private final File disk;
        private final ImageLoader loader;

        LoadTask(String url, ImageView iv, File disk, ImageLoader loader) {
            this.url = url; this.iv = iv; this.disk = disk; this.loader = loader;
        }

        @Override public void run() {
            Bitmap bm = disk != null && disk.exists() && disk.length() > 0 ? loader.decode(disk) : null;
            if (bm == null) bm = loader.loadFromNetwork(url, disk);
            if (bm != null) loader.memCache.put(url, bm);
            loader.deliver(url, iv, bm);
        }
    }

    private final LruCache<String, Bitmap> memCache;   // url -> Bitmap（内存缓存）
    private final ExecutorService pool;                // 下载/解码线程池
    private final Handler main = new Handler(Looper.getMainLooper());
    private Context appCtx;

    private ImageLoader() {
        final int maxMem = (int) (Runtime.getRuntime().maxMemory() / 8); // 1/8 堆
        memCache = new MemCache(maxMem);
        pool = Executors.newFixedThreadPool(3);
    }

    /** 绑定 app ctx（MainActivity.onCreate 注入），用于取磁盘缓存目录。 */
    public void attach(Context ctx) { appCtx = ctx.getApplicationContext(); }

    /** 异步加载远程图片到 ImageView（主线程回调）。本地/空 src 返回 false（调用方按原逻辑）。 */
    public boolean load(final String url, final ImageView iv) {
        if (url == null || url.isEmpty()) return false;
        if (!(url.startsWith("http://") || url.startsWith("https://"))) return false;

        iv.setTag("img:" + url);                       // 防错位：本次请求的目标 url

        Bitmap hit = memCache.get(url);
        if (hit != null) { iv.setImageBitmap(hit); return true; }

        pool.execute(new LoadTask(url, iv, diskFile(url), this));
        return true;
    }

    /** 网络下载：先落盘再解码（磁盘即缓存），失败返回 null。 */
    private Bitmap loadFromNetwork(String url, File disk) {
        try {
            HttpURLConnection conn = (HttpURLConnection) new URL(url).openConnection();
            conn.setConnectTimeout(15000);
            conn.setReadTimeout(30000);
            conn.setInstanceFollowRedirects(true);
            InputStream in = conn.getInputStream();
            if (disk != null) {
                disk.getParentFile().mkdirs();
                FileOutputStream fo = new FileOutputStream(disk);
                byte[] buf = new byte[16384];
                int r;
                while ((r = in.read(buf)) > 0) fo.write(buf, 0, r);
                fo.close();
                in.close();
                return decode(disk);
            }
            Bitmap tmp = BitmapFactory.decodeStream(in);
            in.close();
            return tmp;
        } catch (Exception e) { return null; }
    }

    /** 主线程交付：仅当 ImageView 仍指向同一 url 时设置（滚动复用防错位）。 */
    private void deliver(final String url, final ImageView iv, final Bitmap bm) {
        main.post(new Runnable() {
            @Override public void run() {
                Object tag = iv.getTag();
                if (tag != null && ("img:" + url).equals(tag)) {
                    iv.setImageBitmap(bm);   // bm 为 null 时清空，与旧占位一致
                }
            }
        });
    }

    private Bitmap decode(File f) {
        try {
            FileInputStream in = new FileInputStream(f);
            Bitmap bm = BitmapFactory.decodeStream(in);
            in.close();
            return bm;
        } catch (Exception e) { return null; }
    }

    private File diskFile(String url) {
        if (appCtx == null) return null;
        File dir = new File(appCtx.getCacheDir(), "img");
        return new File(dir, md5(url));
    }

    private static String md5(String s) {
        try {
            MessageDigest md = MessageDigest.getInstance("MD5");
            byte[] d = md.digest(s.getBytes("UTF-8"));
            StringBuilder sb = new StringBuilder();
            for (byte b : d) sb.append(String.format("%02x", b & 0xff));
            return sb.toString();
        } catch (Exception e) { return Integer.toHexString(s.hashCode()); }
    }
}