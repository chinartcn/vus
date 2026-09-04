/*
 * VusExtension.java — DEX 逻辑拓展契约（仅逻辑，不含 UI）
 *
 * 插件 dex 必须包含一个实现本接口、带无参构造的类（约定类名
 * com.vus.plugins.<插件名首字母大写>Plugin），由 ExtensionLoader 加载。
 *
 * 调用链：.vus 拓展_调用("插件.操作", 参数JSON) →
 *   vus_plugin_ext_call → callJava("ext.插件.操作") →
 *   ExtensionLoader.dispatch("插件.操作", args)。
 *
 * 返回约定（字符串）：{"ok":1,"data":"..."} 或 {"ok":0,"err":"..."}。
 * 插件只做逻辑计算/IO，不接触界面；控件能力仍在主 APK（.VAZ/VuaRenderer）。
 */
package com.vus.android;

import org.json.JSONObject;

public interface VusExtension {

    /** 执行插件操作。op 为 "插件.操作" 去掉插件名的部分；args 为调用参数。
     *  整个调用可能运行在 JNI 工作线程，插件不得在主线程假设下执行重活。 */
    String invoke(String op, JSONObject args);
}