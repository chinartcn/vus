/*
 * UpdateChecker.java — 检查 Gitee 上的新版并下载安装
 *
 * 流程：
 *   1. 从 Gitee raw 取 version.json
 *   2. 对比 versionCode（本地 → PackageManager）
 *   3. 若新版存在，下载 APK 到 Downloads 目录
 *   4. 下载完成后调用系统安装器
 */
package com.vus.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.DownloadManager;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Environment;
import android.widget.Toast;

import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.net.HttpURLConnection;
import java.net.URL;

public final class UpdateChecker {

    private static final String VERSION_URL =
            "https://gitee.com/rtccn_mc/vus/raw/master/version.json";

    private final Activity activity;

    public UpdateChecker(Activity activity) {
        this.activity = activity;
    }

    public void check() {
        Toast.makeText(activity, "正在检查更新…", Toast.LENGTH_SHORT).show();
        new Thread(new CheckTask()).start();
    }

    /* 检查任务 */
    private class CheckTask implements Runnable {
        public void run() {
            try {
                int localCode = activity.getPackageManager()
                        .getPackageInfo(activity.getPackageName(), 0).versionCode;
                JSONObject remote = fetchRemoteVersion();
                if (remote == null) {
                    toast("检查更新失败：无法获取远程版本信息");
                    return;
                }
                int remoteCode = remote.optInt("versionCode", 0);
                String remoteName = remote.optString("versionName", "");
                String apkUrl = remote.optString("apkUrl", "");
                String changelog = remote.optString("changelog", "");

                if (remoteCode <= localCode) {
                    toast("已是最新版本 v" + remoteName);
                    return;
                }

                String msg = "发现新版本 v" + remoteName + "\n\n更新内容：\n" + changelog;
                dialog("发现新版本", msg, new DownloadTask(apkUrl));
            } catch (Exception e) {
                toast("检查更新失败: " + e.getMessage());
            }
        }
    }

    /* 下载任务 */
    private class DownloadTask implements DialogInterface.OnClickListener {
        private String apkUrl;
        DownloadTask(String url) { this.apkUrl = url; }
        public void onClick(DialogInterface d, int w) {
            try {
                DownloadManager.Request req = new DownloadManager.Request(Uri.parse(apkUrl));
                req.setTitle("VUS 更新");
                req.setDescription("正在下载新版 APK…");
                req.setNotificationVisibility(
                        DownloadManager.Request.VISIBILITY_VISIBLE_NOTIFY_COMPLETED);
                req.setDestinationInExternalPublicDir(
                        Environment.DIRECTORY_DOWNLOADS, "VUS.apk");
                req.setMimeType("application/vnd.android.package-archive");

                DownloadManager dm = (DownloadManager)
                        activity.getSystemService(Context.DOWNLOAD_SERVICE);
                if (dm == null) {
                    toast("下载失败：无法获取下载服务");
                    return;
                }
                long downloadId = dm.enqueue(req);
                new Thread(new PollTask(dm, downloadId)).start();
            } catch (Exception e) {
                toast("下载失败: " + e.getMessage());
            }
        }
    }

    /* 轮询下载进度 */
    private class PollTask implements Runnable {
        private DownloadManager dm;
        private long id;
        PollTask(DownloadManager dm, long id) { this.dm = dm; this.id = id; }
        public void run() {
            DownloadManager.Query q = new DownloadManager.Query();
            q.setFilterById(id);
            while (true) {
                try { Thread.sleep(1000); } catch (InterruptedException e) { break; }
                Cursor c = dm.query(q);
                if (c != null && c.moveToFirst()) {
                    int status = c.getInt(c.getColumnIndexOrThrow(
                            DownloadManager.COLUMN_STATUS));
                    if (status == DownloadManager.STATUS_SUCCESSFUL) {
                        String uri = c.getString(c.getColumnIndexOrThrow(
                                DownloadManager.COLUMN_LOCAL_URI));
                        install(Uri.parse(uri));
                        c.close();
                        return;
                    } else if (status == DownloadManager.STATUS_FAILED) {
                        toast("下载失败");
                        c.close();
                        return;
                    }
                    c.close();
                }
            }
        }
    }

    /* UI 辅助 - 使用命名类避免 d8 匿名类 bug */
    private class ToastTask implements Runnable {
        private String msg;
        ToastTask(String msg) { this.msg = msg; }
        public void run() { Toast.makeText(activity, msg, Toast.LENGTH_LONG).show(); }
    }
    private void toast(String msg) {
        activity.runOnUiThread(new ToastTask(msg));
    }

    private class DialogTask implements Runnable {
        private String title;
        private String msg;
        private DialogInterface.OnClickListener listener;
        DialogTask(String title, String msg, DialogInterface.OnClickListener listener) {
            this.title = title; this.msg = msg; this.listener = listener;
        }
        public void run() {
            new AlertDialog.Builder(activity)
                    .setTitle(title)
                    .setMessage(msg)
                    .setPositiveButton("下载更新", listener)
                    .setNegativeButton("取消", null)
                    .show();
        }
    }
    private void dialog(String title, String msg, DialogInterface.OnClickListener listener) {
        activity.runOnUiThread(new DialogTask(title, msg, listener));
    }

    private JSONObject fetchRemoteVersion() throws Exception {
        HttpURLConnection conn = (HttpURLConnection)
                new URL(VERSION_URL).openConnection();
        conn.setConnectTimeout(8000);
        conn.setReadTimeout(8000);
        conn.setRequestProperty("User-Agent", "VUS-Android/1.0");
        int code = conn.getResponseCode();
        if (code != 200) return null;
        BufferedReader br = new BufferedReader(
                new InputStreamReader(conn.getInputStream(), "UTF-8"));
        StringBuilder sb = new StringBuilder();
        String line;
        while ((line = br.readLine()) != null) sb.append(line);
        br.close();
        return new JSONObject(sb.toString());
    }

    private void install(Uri apkUri) {
        Intent intent = new Intent(Intent.ACTION_VIEW);
        intent.setDataAndType(apkUri, "application/vnd.android.package-archive");
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        activity.startActivity(intent);
    }
}