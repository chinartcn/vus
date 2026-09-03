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

public final class VuaBridge {

    static {
        // 与 vus_apk.c 生成的壳保持一致：native 库名 libvus_app.so
        System.loadLibrary("vus_app");
    }

    /** native → Java 重绘回调：屏栈变化（界面_显示/返回/返回至）后由 native 调用。
     * MainActivity 在此注册一个 runnable 来重建当前屏的 View。
     */
    public static Runnable onRerender = null;

    /** Java 检查更新回调：由 VuaRenderer 按钮处理触发 */
    public static Runnable onCheckUpdate = null;

    /** 被 native（jni_bridge.c）调用的入口；可能来自非 UI 线程，需自行切到主线程。 */
    public static void onNativeRerender() {
        if (onRerender != null) onRerender.run();
    }

    /**
     * native：创建全局 VuaSession 并运行 vus_main()（.vus 入口）。
     * 返回 0 成功，非 0 失败。此后当前屏（若 .vus 调用了界面_显示）可被渲染。
     */
    public static native int vuaInit();

    /**
     * native：返回当前屏（栈顶）的规范化渲染树 JSON；无屏/失败返回 null。
     * 返回的字符串在 native 侧 malloc，Java 无需释放（native 在跨 JNI 时已拷贝）。
     */
    public static native String vuaRenderTree();

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