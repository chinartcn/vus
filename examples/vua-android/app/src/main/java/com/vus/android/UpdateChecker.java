/*
 * UpdateChecker.java — 检查 Gitee 上的新版并下载安装
 *
 * 流程：
 *   1. 从 Gitee raw 取 version.json
 *   2. 对比 versionCode（本地 → AndroidManifest）
 *   3. 若新版存在，下载 APK 到 Downloads 目录
 *   4. 下载完成后调用系统安装器
 */
package com.vus.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.DownloadManager;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
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
    private long downloadId = -1;

    public UpdateChecker(Activity activity) {
        this.activity = activity;
    }

    /** 入口：在后台线程检查更新，结果在主线程弹窗 */
    public void check() {
        Toast.makeText(activity, "正在检查更新…", Toast.LENGTH_SHORT).show();
        new Thread(() -> {
            try {
                // 1. 获取本地版本
                int localCode = activity.getPackageManager()
                        .getPackageInfo(activity.getPackageName(), 0).versionCode;

                // 2. 获取远程版本信息
                JSONObject remote = fetchRemoteVersion();
                if (remote == null) {
                    showToast("检查更新失败：无法获取远程版本信息");
                    return;
                }
                int remoteCode = remote.optInt("versionCode", 0);
                String remoteName = remote.optString("versionName", "");
                final String apkUrl = remote.optString("apkUrl", "");
                final String changelog = remote.optString("changelog", "");

                // 3. 对比版本
                if (remoteCode <= localCode) {
                    showToast("已是最新版本 v" + remoteName);
                    return;
                }

                // 4. 发现新版本，弹窗提示下载
                String msg = "发现新版本 v" + remoteName + "\n\n更新内容：\n" + changelog;
                showDialog("发现新版本", msg, () -> downloadApk(apkUrl));

            } catch (Exception e) {
                final String err = e.getMessage();
                activity.runOnUiThread(() ->
                        Toast.makeText(activity, "检查更新失败: " + err, Toast.LENGTH_LONG).show());
            }
        }).start();
    }

    /** 从 Gitee 获取 version.json */
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

    /** 使用 DownloadManager 下载 APK */
    private void downloadApk(String apkUrl) {
        try {
            DownloadManager.Request req = new DownloadManager.Request(Uri.parse(apkUrl));
            req.setTitle("VUS 更新");
            req.setDescription("正在下载新版 APK…");
            req.setNotificationVisibility(DownloadManager.Request.VISIBILITY_VISIBLE_NOTIFY_COMPLETED);
            req.setDestinationInExternalPublicDir(Environment.DIRECTORY_DOWNLOADS, "VUS.apk");
            req.setMimeType("application/vnd.android.package-archive");

            DownloadManager dm = (DownloadManager) activity.getSystemService(Context.DOWNLOAD_SERVICE);
            if (dm == null) {
                showToast("下载失败：无法获取下载服务");
                return;
            }
            downloadId = dm.enqueue(req);

            // 注册下载完成广播
            activity.registerReceiver(downloadReceiver,
                    new IntentFilter(DownloadManager.ACTION_DOWNLOAD_COMPLETE));
        } catch (Exception e) {
            showToast("下载失败: " + e.getMessage());
        }
    }

    /** 下载完成后安装 APK */
    private final BroadcastReceiver downloadReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            long id = intent.getLongExtra(DownloadManager.EXTRA_DOWNLOAD_ID, -1);
            if (id != downloadId) return;
            activity.unregisterReceiver(this);

            DownloadManager dm = (DownloadManager) activity.getSystemService(Context.DOWNLOAD_SERVICE);
            if (dm == null) return;

            DownloadManager.Query q = new DownloadManager.Query().setFilterById(id);
            Cursor c = dm.query(q);
            if (c != null && c.moveToFirst()) {
                int status = c.getInt(c.getColumnIndexOrThrow(DownloadManager.COLUMN_STATUS));
                if (status == DownloadManager.STATUS_SUCCESSFUL) {
                    String uri = c.getString(c.getColumnIndexOrThrow(DownloadManager.COLUMN_LOCAL_URI));
                    installApk(Uri.parse(uri));
                } else {
                    showToast("下载失败");
                }
                c.close();
            }
        }
    };

    /** 调用系统安装器 */
    private void installApk(Uri apkUri) {
        Intent intent = new Intent(Intent.ACTION_VIEW);
        intent.setDataAndType(apkUri, "application/vnd.android.package-archive");
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        activity.startActivity(intent);
    }

    /* ---------- UI 辅助（切回主线程） ---------- */

    private void showToast(final String msg) {
        activity.runOnUiThread(() -> Toast.makeText(activity, msg, Toast.LENGTH_LONG).show());
    }

    private void showDialog(String title, String msg, final Runnable onConfirm) {
        activity.runOnUiThread(() -> new AlertDialog.Builder(activity)
                .setTitle(title)
                .setMessage(msg)
                .setPositiveButton("下载更新", (d, w) -> onConfirm.run())
                .setNegativeButton("取消", null)
                .show());
    }
}