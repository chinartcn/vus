/*
 * SamplePlugin.java — 示例 DEX 逻辑拓展插件
 *
 * 约定：类名 com.vus.plugins.SamplePlugin（首字母大写），实现 VusExtension，
 * 需无参构造。本插件只做纯逻辑，不接触界面（控件能力仍在主 APK）。
 *
 * 已实现操作：
 *   sum       求 a+b
 *   factorial 求 n 的阶乘
 *   upper     文本转大写
 *   echo      回显 msg
 */
package com.vus.plugins;

import com.vus.android.VusExtension;

import org.json.JSONObject;

public class SamplePlugin implements VusExtension {

    @Override
    public String invoke(String op, JSONObject args) {
        try {
            if ("sum".equals(op)) {
                double a = args.optDouble("a", 0);
                double b = args.optDouble("b", 0);
                return ok(String.valueOf(a + b));
            }
            if ("factorial".equals(op)) {
                int n = args.optInt("n", 0);
                long r = 1;
                for (int i = 2; i <= n; i++) r *= i;
                return ok(String.valueOf(r));
            }
            if ("upper".equals(op)) {
                String s = args.optString("text", "");
                return ok(s.toUpperCase());
            }
            if ("echo".equals(op)) {
                return ok(args.optString("msg", "pong"));
            }
            return err("未知操作: " + op);
        } catch (Throwable t) {
            return err(String.valueOf(t));
        }
    }

    private static String ok(String data) {
        try {
            JSONObject o = new JSONObject();
            o.put("ok", true);
            o.put("data", data);
            return o.toString();
        } catch (Exception e) {
            return "{\"ok\":false}";
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