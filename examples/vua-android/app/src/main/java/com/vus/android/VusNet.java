/*
 * VusNet.java — 网络请求收敛（HttpURLConnection，零第三方）
 *
 * 收敛 VuaBridge / UpdateChecker / UpdateManager 内各自手写的
 * "HttpURLConnection 连接→读流→断开 + 主线程转子线程 + 重试"样板：
 *   - requestBytes：GET（data=null）或 POST；自定义请求头；按次数重试 IO 错误；
 *     主线程调用自动转子线程同步等待（Android 禁止主线程联网）。
 *   - upload：multipart/form-data 文件上传（手写 body 边界，不引库）。
 * 返回 VusAsync.Holder：err=IO 错误/超时；非 200 一律视为失败（val=null）。
 */
package com.vus.android;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.Map;

final class VusNet {

    private static final String UA = "VUS-Android/1.0";

    private VusNet() { }

    /** GET。返回字节；失败 Holder.err 或 val=null（非 200）。主线程规避。 */
    static VusAsync.Holder<byte[]> get(String url, int timeoutSec) {
        return requestBytes(url, null, null, timeoutSec, 1);
    }

    /** 通用请求。 */
    static VusAsync.Holder<byte[]> requestBytes(final String url, final String data,
                                                final Map<String, String> headers,
                                                final int timeoutSec, final int retry) {
        return VusAsync.bg(new RequestJob(url, data, headers, timeoutSec, retry),
                timeoutSec + 60L);
    }

    /** multipart 文件上传。返回是否 2xx/3xx。 */
    static VusAsync.Holder<Boolean> upload(final String url, final File file,
                                           final Map<String, String> fields,
                                           final Map<String, String> headers,
                                           final int timeoutSec) {
        return VusAsync.bg(new UploadJob(url, file, fields, headers, timeoutSec),
                timeoutSec + 60L);
    }

    /* ==================== 实现 ==================== */

    /** GET/POST 请求任务（命名类：规避 d8 dev 泛型匿名类 desugar bug）。 */
    private static final class RequestJob implements VusAsync.Job<byte[]> {
        private final String url, data;
        private final Map<String, String> headers;
        private final int timeoutSec, retry;
        RequestJob(String url, String data, Map<String, String> headers,
                   int timeoutSec, int retry) {
            this.url = url; this.data = data; this.headers = headers;
            this.timeoutSec = timeoutSec; this.retry = retry;
        }
        @Override public byte[] run() throws Throwable {
            int n = retry < 1 ? 1 : retry;
            Throwable last = null;
            for (int attempt = 0; attempt < n; attempt++) {
                try {
                    return doRequest(url, data, timeoutSec, headers);
                } catch (Throwable t) {
                    last = t;               // 超时/IO 错误：按 retry 次数重试
                }
            }
            if (last != null) throw last;
            return null;
        }
    }

    /** multipart 上传任务（命名类）。 */
    private static final class UploadJob implements VusAsync.Job<Boolean> {
        private final String url;
        private final File file;
        private final Map<String, String> fields, headers;
        private final int timeoutSec;
        UploadJob(String url, File file, Map<String, String> fields,
                  Map<String, String> headers, int timeoutSec) {
            this.url = url; this.file = file; this.fields = fields;
            this.headers = headers; this.timeoutSec = timeoutSec;
        }
        @Override public Boolean run() throws Throwable {
            return doUpload(url, file, fields, headers, timeoutSec);
        }
    }

    private static byte[] doRequest(String url, String data, int timeoutSec,
                                    Map<String, String> headers) throws Exception {
        HttpURLConnection conn = (HttpURLConnection) new URL(url).openConnection();
        conn.setConnectTimeout(timeoutSec * 1000);
        conn.setReadTimeout(timeoutSec * 1000);
        conn.setRequestProperty("User-Agent", UA);
        applyHeaders(conn, headers);
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
        try {
            byte[] buf = new byte[8192];
            int r;
            while ((r = in.read(buf)) > 0) bos.write(buf, 0, r);
        } finally {
            in.close();
        }
        return bos.toByteArray();
    }

    private static boolean doUpload(String url, File file, Map<String, String> fields,
                                    Map<String, String> headers, int timeoutSec) throws Exception {
        if (url == null || url.isEmpty() || file == null || !file.isFile()) return false;
        String boundary = "----VUS" + System.currentTimeMillis();

        ByteArrayOutputStream body = new ByteArrayOutputStream();
        byte[] CRLF = "\r\n".getBytes("UTF-8");
        if (fields != null) {
            for (Map.Entry<String, String> e : fields.entrySet()) {
                body.write(("--" + boundary + CRLF).getBytes("UTF-8"));
                body.write(("Content-Disposition: form-data; name=\"" + e.getKey() + "\"" + CRLF + CRLF).getBytes("UTF-8"));
                body.write(e.getValue() == null ? new byte[0] : e.getValue().getBytes("UTF-8"));
                body.write(CRLF);
            }
        }
        body.write(("--" + boundary + CRLF).getBytes("UTF-8"));
        body.write(("Content-Disposition: form-data; name=\"file\"; filename=\"" + file.getName() + "\"" + CRLF).getBytes("UTF-8"));
        body.write(("Content-Type: application/octet-stream" + CRLF + CRLF).getBytes("UTF-8"));
        InputStream in = new FileInputStream(file);
        try {
            byte[] buf = new byte[16384];
            int r;
            while ((r = in.read(buf)) > 0) body.write(buf, 0, r);
        } finally {
            in.close();
        }
        body.write(CRLF);
        body.write(("--" + boundary + "--" + CRLF).getBytes("UTF-8"));
        byte[] payload = body.toByteArray();

        HttpURLConnection conn = (HttpURLConnection) new URL(url).openConnection();
        conn.setConnectTimeout(timeoutSec * 1000);
        conn.setReadTimeout(timeoutSec * 1000);
        conn.setRequestMethod("POST");
        conn.setDoOutput(true);
        conn.setRequestProperty("Content-Type", "multipart/form-data; boundary=" + boundary);
        conn.setRequestProperty("User-Agent", UA);
        applyHeaders(conn, headers);
        OutputStream os = conn.getOutputStream();
        try {
            os.write(payload);
        } finally {
            os.close();
        }
        int code = conn.getResponseCode();
        InputStream resp = conn.getInputStream();
        try {
            while (resp.read() != -1) { }       // 读完响应便于连接复用
        } finally {
            resp.close();
        }
        return code >= 200 && code < 300;
    }

    private static void applyHeaders(HttpURLConnection conn, Map<String, String> headers) {
        if (headers != null) {
            for (Map.Entry<String, String> e : headers.entrySet()) {
                if (e.getKey() != null && e.getValue() != null)
                    conn.setRequestProperty(e.getKey(), e.getValue());
            }
        }
    }
}