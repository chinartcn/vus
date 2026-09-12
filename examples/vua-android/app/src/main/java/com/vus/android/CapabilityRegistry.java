/*
 * CapabilityRegistry.java — 能力注册协议（Cordis_dc §3.5 / vce 边界骨架）
 *
 * 把「VUS 内建函数 → 宿主能力」的隐式耦合（VuaBridge.callJava 的 if 链）
 * 登记为显式清单：每个能力名标归属面——"vce"（宿主能力桥）或 "gvui"（UI 面）。
 * 本期只建注册/查询骨架，不重构 VuaBridge 分发（重构 = M3 vce 拆包）。
 * 能力清单以 VuaBridge.callJava 现有分发的 api 名为准，注册表做单一真源。
 */
package com.vus.android;

import java.util.LinkedHashMap;
import java.util.Map;

public final class CapabilityRegistry {

    /** 能力归属面。vce = 宿主能力（逻辑层向宿主要能力）；gvui = UI 面（渲染/外观）。 */
    public static final String FACE_VCE  = "vce";
    public static final String FACE_GVUI = "gvui";

    /** 能力名 → 归属面（保存登记顺序 = 文档化顺序）。 */
    private static final Map<String, String> sApiFace = new LinkedHashMap<>();
    private static String sDesc = null;   // describe() 缓存

    static {
        /* vce 面：文件 / 网络 / 设备能力 / 媒体 / 扩展 / 热更（对应 real callJava 分发）。
         * 实现分两类：轻量能力在 VceApi（handle 分发）；file/http/media/hotupdate 等
         * 深耦合能力实现仍在 VuaBridge 分支（调 VusIo/VusNet/VusMedia 等独立类），
         * 本注册表对其只登记归属——全量清单即 CapabilityRegistry.describe() 的可查单点。 */
        register("file.read"); register("file.write"); register("file.append");
        register("file.exists"); register("file.delete"); register("file.isdir");
        register("file.list");
        register("http.get"); register("http.post"); register("http.request");
        register("http.upload"); register("http.download");
        register("vibrate"); register("clipboard.read"); register("clipboard.write");
        register("device.info"); register("toast"); register("share.text");
        register("battery.status"); register("screen.keepon"); register("network.type");
        register("notify.send");
        register("media.play"); register("media.stop"); register("media.pause");
        register("media.resume"); register("media.seek"); register("media.status");
        register("video.play");
        register("hotupdate.apply");
        /* ext.* 为动态前缀能力（DEX 插件），按前缀登记 */
        registerPrefix("ext.");

        /* gvui 面：影响渲染外观/主题的调用（参数进入 UI 层） */
        registerGvui("theme.set"); registerGvui("theme.get"); registerGvui("theme.primary");
    }

    private CapabilityRegistry() { }

    /** 登记一个能力到 vce 面。 */
    static void register(String api) { sApiFace.put(api, FACE_VCE); sDesc = null; }

    /** 登记一个能力到 gvui（UI）面。 */
    static void registerGvui(String api) { sApiFace.put(api, FACE_GVUI); sDesc = null; }

    /** 前缀能力（如 ext.xxx）：按前缀匹配归属。 */
    static void registerPrefix(String prefix) { sApiFace.put(prefix, FACE_VCE); sDesc = null; }

    /** 查询能力归属面；未知返回 null。ext.<name>.<op> 命中 "ext." 前缀。 */
    public static String faceOf(String api) {
        if (api == null || api.isEmpty()) return null;
        String exact = sApiFace.get(api);
        if (exact != null) return exact;
        for (Map.Entry<String, String> e : sApiFace.entrySet()) {
            if (e.getKey().endsWith(".") && api.startsWith(e.getKey())) return e.getValue();
        }
        return null;
    }

    /** 是否属于 vce（宿主能力桥）面。 */
    public static boolean isVce(String api) { return FACE_VCE.equals(faceOf(api)); }

    /** 导出清单 JSON（能力归属表，供诊断/文档）。 */
    public static String describe() {
        if (sDesc == null) {
            StringBuilder sb = new StringBuilder("{");
            boolean first = true;
            for (Map.Entry<String, String> e : sApiFace.entrySet()) {
                if (!first) sb.append(',');
                first = false;
                sb.append('"').append(e.getKey()).append("\":\"").append(e.getValue()).append('"');
            }
            sb.append('}');
            sDesc = sb.toString();
        }
        return sDesc;
    }

    /** 已登记能力总数（含前缀）。 */
    public static int count() { return sApiFace.size(); }
}