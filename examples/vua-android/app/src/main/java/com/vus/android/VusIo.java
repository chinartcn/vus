/*
 * VusIo.java — 文件 / 哈希 / 目录树工具（零依赖，全工程共享）
 *
 * 收敛 VuaBridge / UpdateManager / ExtensionLoader / MainActivity 内重复的
 * 读文本、写文件、拷贝树、删除树、SHA-256 等样板实现（各文件此前各写一份）。
 */
package com.vus.android;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.security.MessageDigest;

final class VusIo {

    private VusIo() { }

    /* ==================== 读写 ==================== */

    /** 读全部字节（文件不存在/读失败抛 IOException）。 */
    static byte[] readBytes(File f) throws IOException {
        FileInputStream in = new FileInputStream(f);
        try {
            long len = f.length();
            if (len > Integer.MAX_VALUE) throw new IOException("文件过大: " + f);
            byte[] b = new byte[(int) len];
            int off = 0;
            while (off < b.length) {
                int r = in.read(b, off, b.length - off);
                if (r < 0) break;
                off += r;
            }
            if (off != b.length) {
                byte[] t = new byte[off];
                System.arraycopy(b, 0, t, 0, off);
                return t;
            }
            return b;
        } finally {
            in.close();
        }
    }

    static String readText(File f) throws IOException {
        return new String(readBytes(f), "UTF-8");
    }

    /** 写字节（自动建父目录）。 */
    static void writeBytes(File f, byte[] b, boolean append) throws IOException {
        if (!f.getParentFile().isDirectory() && !f.getParentFile().mkdirs()) {
            throw new IOException("创建目录失败: " + f.getParent());
        }
        FileOutputStream fo = new FileOutputStream(f, append);
        try {
            fo.write(b);
        } finally {
            fo.close();
        }
    }

    static void writeText(File f, String s) throws IOException {
        writeBytes(f, s.getBytes("UTF-8"), false);
    }

    /** 拷贝文件（自动建父目录），源缺失直接返回。 */
    static void copyFile(File from, File to) throws IOException {
        if (!from.isFile()) return;
        if (!to.getParentFile().isDirectory() && !to.getParentFile().mkdirs()) {
            throw new IOException("创建目录失败: " + to.getParent());
        }
        InputStream in = new FileInputStream(from);
        try {
            OutputStream fo = new FileOutputStream(to);
            try {
                byte[] b = new byte[8192];
                int r;
                while ((r = in.read(b)) > 0) fo.write(b, 0, r);
            } finally {
                fo.close();
            }
        } finally {
            in.close();
        }
    }

    /* ==================== 目录树 ==================== */

    /** 递归拷贝目录/文件。 */
    static void copyTree(File from, File to) throws IOException {
        if (from.isDirectory()) {
            if (!to.isDirectory() && !to.mkdirs()) return;
            File[] kids = from.listFiles();
            if (kids != null) for (File k : kids) copyTree(k, new File(to, k.getName()));
        } else if (from.isFile()) {
            copyFile(from, to);
        }
    }

    /** 递归删除文件/目录（含空目录本身）。 */
    static void rm(File f) {
        if (f.isDirectory()) {
            File[] kids = f.listFiles();
            if (kids != null) for (File k : kids) rm(k);
        }
        f.delete();
    }

    /* ==================== 哈希 / 编码 ==================== */

    static String sha256Hex(File f) throws Exception {
        return hex(hashFile(f, "SHA-256"));
    }

    static String md5Hex(String s) {
        try {
            MessageDigest md = MessageDigest.getInstance("MD5");
            return hex(md.digest(s.getBytes("UTF-8")));
        } catch (Exception e) {
            return Integer.toHexString(s.hashCode());
        }
    }

    private static byte[] hashFile(File f, String algo) throws Exception {
        MessageDigest md = MessageDigest.getInstance(algo);
        FileInputStream in = new FileInputStream(f);
        try {
            byte[] buf = new byte[8192];
            int r;
            while ((r = in.read(buf)) > 0) md.update(buf, 0, r);
        } finally {
            in.close();
        }
        return md.digest();
    }

    static String hex(byte[] d) {
        StringBuilder sb = new StringBuilder(d.length * 2);
        for (byte x : d) {
            sb.append(Character.forDigit((x >> 4) & 0xF, 16));
            sb.append(Character.forDigit(x & 0xF, 16));
        }
        return sb.toString();
    }
}