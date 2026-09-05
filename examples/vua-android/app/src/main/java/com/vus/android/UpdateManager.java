/*
 * UpdateManager.java — VUS 热更协议实体（更新包：拉清单 → 校验 → 原子提交 → 回滚）
 *
 * 实现「VUS 热更加载器」设计（docs/designs/2026-09-05-hotupdate-loader-design.md）：
 *   §5.1  路径穿越防护：manifest 相对路径与 zip 内条目逐条 canonical 校验，
 *         只在 filesDir 以内放行（拒绝绝对路径/../空段，任一非法整包拒绝）。
 *   §5.2  last-good 晋升时机：应用成功（实时层生效）不晋升；下次启动
 *         VuaBridge.ensureNative() 加载 .so 成功（onSoLoaded）才晋升。
 *   §5.3  目录布局：patch/version、patch/pending、patch/needs_rollback、
 *         patch/last-good/、patch/暂存/、patch/下载/。
 *   §5.4  单一真源 = filesDir：内置版本即"版本 0 的 patch"（extractAssets 缺失才写），
 *         首次启动写 version=0，之后启动只读版本号。
 *
 * 宿主内建、纯 Java（HttpURLConnection + ZipInputStream + org.json），不依赖插件 .so，自举。
 */
package com.vus.android;

import android.content.Context;
import android.os.Looper;
import android.util.Log;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

public final class UpdateManager {

    private static final String TAG = "VusUpd";
    // §5.3 目录布局
    private static final String PATCH       = "patch";
    private static final String VER_FILE    = "patch/version";          // 当前已应用版本号（int 文本）
    private static final String PEND_FILE   = "patch/pending";          // 已应用、待晋升版本
    private static final String RB_FLAG     = "patch/needs_rollback";   // 应用未安全完成 → 启动回滚
    private static final String LGOOD       = "patch/last-good";        // 上一可用版本快照
    private static final String STAGE       = "patch/暂存";
    private static final String DOWNLOAD    = "patch/下载";
    private static final String FILES_JSON  = "patch/current_files.json"; // 已应用版本的文件相对路径清单

    private static Context ctx;

    private UpdateManager() { }

    /** MainActivity onCreate 最早调用。 */
    public static void init(Context context) {
        ctx = context == null ? null : context.getApplicationContext();
    }

    private static File base() { return ctx.getFilesDir(); }

    /* ==================== 启动钩子（§5.4 / §5.2） ==================== */

    /** 首次启动写 version=0（"版本 0 的 patch"由 MainActivity.extractAssets 缺失才写完成）。 */
    public static void ensureVersion0() {
        try {
            File v = new File(base(), VER_FILE);
            if (!v.isFile()) {
                File p = new File(base(), PATCH);
                if (!p.isDirectory()) p.mkdirs();
                writeText(v, "0");
            }
        } catch (Exception e) {
            Log.w(TAG, "ensureVersion0 失败: " + e);
        }
    }

    /** 每次启动调用：清理中间态；存在 needs_rollback → 用 last-good 恢复（§5.2/§5.3）。 */
    public static void onBoot() {
        try {
            cleanupTransition();
            if (new File(base(), RB_FLAG).isFile()) {
                Log.w(TAG, "检测到未安全完成的应用，回滚到 last-good");
                rollback();
            }
        } catch (Exception e) {
            Log.w(TAG, "onBoot 异常: " + e);
        }
    }

    /** VuaBridge.ensureNative() 加载 .so 成功后调用：晋升 last-good（§5.2）。 */
    public static void onSoLoaded() {
        if (ctx == null) return;
        try {
            File pend = new File(base(), PEND_FILE);
            if (!pend.isFile()) return;
            String ver = readText(pend).trim();
            List<String> rels = currentRels();
            File lg = new File(base(), LGOOD);
            if (!lg.isDirectory()) lg.mkdirs();
            for (String rel : rels) copyFile(new File(base(), rel), new File(lg, rel));
            writeText(new File(lg, "version"), ver);
            pend.delete();
            Log.i(TAG, "last-good 晋升到版本 " + ver + "（.so 加载验证通过）");
        } catch (Exception e) {
            Log.w(TAG, "晋升 last-good 失败: " + e);
        }
    }

    /* ==================== 应用更新包（§5.1 / §5.2） ==================== */

    /**
     * 拉 manifest → 校验 → 下载 zip → 解包（路径穿越防护）→ 逐文件 sha256 →
     * 备份旧文件到 last-good → .new + rename 原子提交 → 写 version。
     * 阻塞调用；建议后台线程执行，成功后由调用方重渲染/重载（实时层）生效。
     *
     * 返回：0=已应用；1=无更新（版本 ≤ 当前）；-1=宿主版本过低；-2=失败（已清理/回滚）。
     */
    public static int applyUpdate(String manifestUrl) {
        if (ctx == null || manifestUrl == null || manifestUrl.isEmpty()) return -2;
        try {
            String text = httpText(manifestUrl, 30);
            if (text == null || text.isEmpty()) return -2;
            JSONObject m = new JSONObject(text);
            int ver = m.optInt("版本", 0);
            int minHost = m.optInt("最低宿主版本", 0);
            int host = ctx.getPackageManager()
                    .getPackageInfo(ctx.getPackageName(), 0).versionCode;
            if (minHost > host) return -1;                       // 旧宿主不解新包
            if (ver <= currentVersion()) return 1;               // 版本单调，防降级

            String zipUrl = m.optString("地址", "");
            if (zipUrl.isEmpty()) {
                zipUrl = manifestUrl.substring(0, manifestUrl.lastIndexOf('/') + 1)
                        + "vus_update_" + ver + ".zip";
            }

            File stage = stageDir(ver);
            if (!stage.isDirectory() && !stage.mkdirs()) return -2;
            File zip = new File(base(), DOWNLOAD + "/vus_update_" + ver + ".zip");
            if (!zip.getParentFile().isDirectory() && !zip.getParentFile().mkdirs()) return -2;

            /* 通过 each 尝试前标记"未安全完成"：崩溃/失败时启动回滚 */
            File rb = new File(base(), RB_FLAG);
            try {
                if (httpToFile(zipUrl, zip, 120) == 0) return -2;
                unzipSafely(zip, stage);

                JSONObject files = m.getJSONObject("文件");
                writeText(new File(base(), RB_FLAG), "1");        // 备份/替换开始 → 进入防御期
                List<String> applied = new ArrayList<>();
                Iterator<String> it = files.keys();
                while (it.hasNext()) {
                    String rel = it.next();
                    File target = safepath(base(), rel);          // §5.1 路径穿越校验
                    File staged = new File(stage, rel);
                    String expect = files.getString(rel);
                    int colon = expect.indexOf("sha256:");
                    String want = colon >= 0 ? expect.substring(colon + 7) : expect;
                    String got = sha256Hex(staged);
                    if (!want.equalsIgnoreCase(got)) {
                        throw new Exception("sha256 不匹配: " + rel);
                    }
                    if (target.isFile()) copyFile(target, new File(new File(base(), LGOOD), rel)); // 备份旧版
                    atomicReplace(staged, target);                // .new + rename
                    applied.add(rel);
                }
                writeRels(applied);                               // 晋升依据：当前版本的 rel 清单
                writeText(new File(base(), VER_FILE), String.valueOf(ver));
                writeText(new File(base(), PEND_FILE), String.valueOf(ver));
                rb.delete();                                      // 安全完成，退出防御期
                cleanupTransition();
                Log.i(TAG, "更新包已应用, 版本 " + ver + "（.vua/.dex 实时生效，.so 下次启动）");
                return 0;
            } catch (Throwable t) {
                Log.w(TAG, "应用更新包失败: " + t);
                rollback();
                return -2;
            }
        } catch (Exception e) {
            Log.w(TAG, "applyUpdate 异常: " + e);
            return -2;
        }
    }

    /** 当前已应用版本号（无 version 文件视作 0）。 */
    static int currentVersion() {
        try {
            File v = new File(base(), VER_FILE);
            return v.isFile() ? Integer.parseInt(readText(v).trim()) : 0;
        } catch (Exception e) { return 0; }
    }

    /* ==================== 内部：回滚 / 清理 / 安全工具 ==================== */

    /** §5.1 路径穿越防护：目标必须在 filesDir 内（canonical 前缀校验）。 */
    private static File safepath(File baseDir, String rel) throws Exception {
        if (rel == null || rel.isEmpty()) throw new Exception("空路径");
        File f = new File(baseDir, rel);
        String canon = f.getCanonicalPath();
        String bCanon = baseDir.getCanonicalPath();
        if (!canon.equals(bCanon) && !canon.startsWith(bCanon + File.separator)) {
            throw new Exception("路径越界: " + rel);
        }
        return f;
    }

    /** zip 解包：逐条目路径穿越防护（zip-slip），仅解到 stage。 */
    private static void unzipSafely(File zip, File dest) throws Exception {
        ZipInputStream zin = new ZipInputStream(new FileInputStream(zip));
        ZipEntry e;
        while ((e = zin.getNextEntry()) != null) {
            if (e.isDirectory()) continue;
            File out = safepath(dest, e.getName());
            if (!out.getParentFile().isDirectory()) out.getParentFile().mkdirs();
            FileOutputStream fo = new FileOutputStream(out);
            byte[] buf = new byte[8192];
            int r;
            while ((r = zin.read(buf)) > 0) fo.write(buf, 0, r);
            fo.close();
            zin.closeEntry();
        }
        zin.close();
    }

    /** .new + rename 原子替换。 */
    private static void atomicReplace(File from, File to) throws Exception {
        if (!to.getParentFile().isDirectory()) to.getParentFile().mkdirs();
        File nf = new File(to.getParentFile(), to.getName() + ".new");
        copyFile(from, nf);
        if (!nf.renameTo(to)) {
            nf.delete();
            throw new Exception("rename 失败: " + to);
        }
    }

    /** 回滚：用 last-good 恢复被替换文件，版本回退，清理防御标记。 */
    private static void rollback() {
        try {
            File lg = new File(base(), LGOOD);
            if (lg.isDirectory()) {
                File[] all = lg.listFiles();
                if (all != null) {
                    for (File f : all) copyTree(f, new File(base(), f.getName()));
                }
            }
            File lv = new File(lg, "version");
            writeText(new File(base(), VER_FILE),
                    lv.isFile() ? readText(lv).trim() : "0");
            new File(base(), PEND_FILE).delete();
            new File(base(), RB_FLAG).delete();
            cleanupTransition();
            Log.w(TAG, "已回滚到 last-good");
        } catch (Exception e) {
            Log.w(TAG, "回滚异常: " + e);
        }
    }

    private static void copyTree(File from, File to) throws Exception {
        if (from.isDirectory()) {
            if (!to.isDirectory() && !to.mkdirs()) return;
            File[] kids = from.listFiles();
            if (kids != null) for (File k : kids) copyTree(k, new File(to, k.getName()));
        } else if (from.isFile()) {
            copyFile(from, to);
        }
    }

    private static void copyFile(File from, File to) throws Exception {
        if (!from.isFile()) return;
        if (!to.getParentFile().isDirectory()) to.getParentFile().mkdirs();
        InputStream in = new FileInputStream(from);
        FileOutputStream fo = new FileOutputStream(to);
        byte[] b = new byte[8192];
        int r;
        while ((r = in.read(b)) > 0) fo.write(b, 0, r);
        fo.close();
        in.close();
    }

    /** 清理暂存/下载/残留 .new（启动与应用后各一次）。 */
    private static void cleanupTransition() {
        try {
            for (String d : new String[] { STAGE, DOWNLOAD }) {
                File dir = new File(base(), d);
                if (!dir.isDirectory()) continue;
                File[] kids = dir.listFiles();
                if (kids != null) for (File k : kids) rm(k);
                dir.delete();
            }
        } catch (Exception ignored) { }
    }

    private static void rm(File f) {
        if (f.isDirectory()) {
            File[] kids = f.listFiles();
            if (kids != null) for (File k : kids) rm(k);
        }
        f.delete();
    }

    private static List<String> currentRels() {
        List<String> out = new ArrayList<>();
        try {
            File j = new File(base(), FILES_JSON);
            if (j.isFile()) {
                JSONObject o = new JSONObject(readText(j));
                Iterator<String> it = o.keys();
                while (it.hasNext()) out.add(it.next());
            }
        } catch (Exception ignored) { }
        return out;
    }

    private static void writeRels(List<String> rels) throws Exception {
        StringBuilder sb = new StringBuilder("{");
        for (int i = 0; i < rels.size(); i++) {
            if (i > 0) sb.append(',');
            sb.append('"').append(rels.get(i).replace("\\", "\\\\").replace("\"", "\\\"")).append("\":1");
        }
        sb.append('}');
        writeText(new File(base(), FILES_JSON), sb.toString());
    }

    private static File stageDir(int ver) { return new File(base(), STAGE + "/" + ver); }

    /* ==================== 网络 / 哈希 / IO 工具 ==================== */

    private static byte[] httpBytes(final String url, final int timeoutSec) {
        final byte[][] h = new byte[1][];
        final Throwable[] terr = new Throwable[1];
        final boolean main = Looper.myLooper() == Looper.getMainLooper();
        Runnable job = () -> {
            try {
                HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
                c.setConnectTimeout(timeoutSec * 1000);
                c.setReadTimeout(timeoutSec * 1000);
                c.setRequestProperty("User-Agent", "VUS-Update/1.0");
                if (c.getResponseCode() != 200) { terr[0] = new Exception("HTTP " + c.getResponseCode()); return; }
                ByteArrayOutputStream bos = new ByteArrayOutputStream();
                InputStream in = c.getInputStream();
                byte[] b = new byte[8192];
                int r;
                while ((r = in.read(b)) > 0) bos.write(b, 0, r);
                in.close();
                h[0] = bos.toByteArray();
            } catch (Throwable t) { terr[0] = t; }
        };
        if (!main) { job.run(); return terr[0] == null ? h[0] : null; }
        CountDownLatch latch = new CountDownLatch(1);
        new Thread(() -> { job.run(); latch.countDown(); }).start();
        try { latch.await((long) timeoutSec + 60L, TimeUnit.SECONDS); } catch (InterruptedException e) { return null; }
        return terr[0] == null ? h[0] : null;
    }

    private static String httpText(String url, int timeout) throws Exception {
        byte[] b = httpBytes(url, timeout);
        return b == null ? null : new String(b, "UTF-8");
    }

    private static int httpToFile(String url, File dest, int timeout) {
        try {
            byte[] b = httpBytes(url, timeout);
            if (b == null) return 0;
            OutputStream os = new FileOutputStream(dest);
            os.write(b);
            os.close();
            return 1;
        } catch (Exception e) {
            return 0;
        }
    }

    private static String sha256Hex(File f) throws Exception {
        MessageDigest md = MessageDigest.getInstance("SHA-256");
        InputStream in = new FileInputStream(f);
        byte[] buf = new byte[8192];
        int r;
        while ((r = in.read(buf)) > 0) md.update(buf, 0, r);
        in.close();
        return hex(md.digest());
    }

    private static String hex(byte[] d) {
        StringBuilder sb = new StringBuilder(d.length * 2);
        for (byte x : d) {
            sb.append(Character.forDigit((x >> 4) & 0xF, 16));
            sb.append(Character.forDigit(x & 0xF, 16));
        }
        return sb.toString();
    }

    private static void writeText(File f, String s) throws Exception {
        if (!f.getParentFile().isDirectory()) f.getParentFile().mkdirs();
        FileOutputStream fo = new FileOutputStream(f);
        fo.write(s.getBytes("UTF-8"));
        fo.close();
    }

    private static String readText(File f) throws Exception {
        byte[] b = new byte[(int) f.length()];
        InputStream in = new FileInputStream(f);
        int off = 0;
        while (off < b.length) {
            int r = in.read(b, off, b.length - off);
            if (r < 0) break;
            off += r;
        }
        in.close();
        return new String(b, 0, off, "UTF-8");
    }
}