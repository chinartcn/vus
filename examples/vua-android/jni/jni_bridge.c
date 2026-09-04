/*
 * jni_bridge.c — VUA JNI 桥（APK 侧）
 *
 * 对应 Java 端 com.vus.android.VuaBridge 的 4 个 static native 方法。
 * 与 rt/vua.c + 编译后的 .vus (vus_main) 链入同一个 libvus_app.so。
 *
 * 注意：包名决定 JNI 符号名。以下按 com.vus.android 生成；
 * `vus build --apk --app-name` 实际包名不同时，需相应改名。
 *
 * 本文件应由 `vus_apk.c` 生成模板产出（替换原来的 stdout 捕获桥），
 * 以承接 vua 的渲染树 / 事件回传。
 */
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vua.h"

/* .vus 编译成的入口（generator 已把 main 改名 vus_main 以避免与 Android 冲突） */
extern int vus_main(void);

/* 缓存 JavaVM，供重绘回调（可能非 Java 线程）Attach 使用。在 JNI_OnLoad 里存。 */
static JavaVM *g_vm = NULL;
/* VuaBridge 类的全局引用 + onNativeRerender 静态方法，避免每次回调都 FindClass。 */
static jclass     g_VuaBridge = NULL;
static jmethodID  g_onNativeRerender = NULL;
static jmethodID  g_callJava = NULL;

/* 平台能力桥分发（定义见下）：VUS 内建 → VuaBridge.callJava（Java 暴露网络/文件能力）。 */
static void java_rpc_dispatch(const char *api, const char *args, char **out);

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    g_vm = vm;
    JNIEnv *env = NULL;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_VERSION_1_6;
    jclass local = (*env)->FindClass(env, "com/vus/android/VuaBridge");
    if (!local) return JNI_VERSION_1_6;
    g_VuaBridge = (*env)->NewGlobalRef(env, local);
    (*env)->DeleteLocalRef(env, local);
    if (g_VuaBridge) {
        g_onNativeRerender = (*env)->GetStaticMethodID(env, g_VuaBridge, "onNativeRerender", "()V");
        g_callJava = (*env)->GetStaticMethodID(env, g_VuaBridge, "callJava",
                                               "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
        /* 平台能力桥：网络/文件由 Java 暴露、VUS 调用（桌面/纯 native 未注册时内建回退） */
        vus_set_java_callback(java_rpc_dispatch);
    }
    return JNI_VERSION_1_6;
}

/* ---- 平台能力桥分发（native → Java 同步 RPC） --------------------------------
 * VUS 内建（网络_GET/文件_读取 等）→ vus_java_rpc → 本函数 → VuaBridge.callJava →
 * JSON 结果原样回传。Java 侧保证不抛异常（错误转 ok:0）。 */
static void java_rpc_dispatch(const char *api, const char *args, char **out) {
    *out = NULL;
    if (!g_vm || !g_VuaBridge || !g_callJava || !api || !args) return;
    JNIEnv *env = NULL;
    jint st = (*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6);
    int attached = 0;
    if (st != JNI_OK) {
        if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK) return;
        attached = 1;
    }
    jstring jap = (*env)->NewStringUTF(env, api);
    jstring jars = (*env)->NewStringUTF(env, args);
    jstring jres = (jstring)(*env)->CallStaticObjectMethod(env, g_VuaBridge, g_callJava, jap, jars);
    (*env)->DeleteLocalRef(env, jap);
    (*env)->DeleteLocalRef(env, jars);
    if (jres) {
        const char *c = (*env)->GetStringUTFChars(env, jres, 0);
        if (c) { *out = strdup(c); (*env)->ReleaseStringUTFChars(env, jres, c); }
        (*env)->DeleteLocalRef(env, jres);
    }
    /* Java 异常（理论上 callJava 内部已捕获）：清空避免污染后续 JNI 调用 */
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (attached) (*g_vm)->DetachCurrentThread(g_vm);
}

/* ---- 重绘回调（native → Java） ----------------------------------------- */
/* 屏栈变化（界面_显示/返回/返回至）后由 vua 调用，回 Java 触发 View 重建。 */
static void native_rerender_cb(VuaSession *session, void *ud) {
    (void)session; (void)ud;
    if (!g_vm || !g_onNativeRerender) return;
    JNIEnv *env = NULL;
    /* 若当前已在 Java 线程则直接拿 env，否则 Attach。 */
    int attached = 0;
    if ((*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK) return;
        attached = 1;
    }
    (*env)->CallStaticVoidMethod(env, g_VuaBridge, g_onNativeRerender);
    if (attached) (*g_vm)->DetachCurrentThread(g_vm);
}

/* ---- vuaInit ------------------------------------------------------------ */
/* 建全局 VuaSession、预加载控件表、运行 .vus 逻辑（界面_显示 首页、界面_绑定 事件等）。 */
JNIEXPORT jint JNICALL
Java_com_vus_android_VuaBridge_vuaInit(JNIEnv *env, jclass clazz) {
    (void)env; (void)clazz;
    VuaError err = {0};
    VuaSession *s = vua_global_session(&err);
    if (!s) { fprintf(stderr, "[vua] vuaInit: %s\n", err.msg); return -1; }
    /* 预加载控件表（已由 vuaSetRootDir 切到文件目录，用相对路径读）。失败不致命。 */
    {
        FILE *cf = fopen("vua_controls.json", "rb");
        if (cf) {
            fseek(cf, 0, SEEK_END); long l = ftell(cf); fseek(cf, 0, SEEK_SET);
            if (l > 0) {
                char *buf = (char *)malloc((size_t)l + 1);
                if (buf) {
                    size_t n = fread(buf, 1, (size_t)l, cf);
                    buf[n] = '\0';
                    if (vua_control_table_load(buf, NULL) != 0)
                        fprintf(stderr, "[vua] 控件表加载失败\n");
                    free(buf);
                }
            }
            fclose(cf);
        } else {
            fprintf(stderr, "[vua] 未找到 vua_controls.json\n");
        }
    }
    /* 注册重绘钩子：vus_main 里 界面_显示/返回 换屏时，回 Java 重建 View。 */
    vua_session_set_rerender_hook(s, native_rerender_cb, NULL);
    int rc = vus_main();
    return rc == 0 ? 0 : -2;
}

/* ---- vuaSetRootDir ------------------------------------------------------- */
/* 把 VUS/VUA 运行时的当前工作目录切到应用文件目录，使 .vua 相对路径可解析。 */
JNIEXPORT void JNICALL
Java_com_vus_android_VuaBridge_vuaSetRootDir(JNIEnv *env, jclass clazz, jstring dir) {
    (void)clazz;
    if (!dir) return;
    const char *d = (*env)->GetStringUTFChars(env, dir, 0);
    if (d) { chdir(d); (*env)->ReleaseStringUTFChars(env, dir, d); }
}

/* ---- vuaRenderTreeBytes ------------------------------------------------- */
/* 返回当前屏（栈顶）的规范化渲染树 JSON 字节；无屏返回 NULL。
 * 以 byte[] 传输（替代 NewStringUTF）：省去 JNI 侧全量 UTF-8 校验/UTF-16 转换，
 * 由 Java new String(bytes, UTF-8) 用 JDK 快速解码器完成。
 * 返回值由 screen 侧缓存所有（不得 free，见 vua_screen_dump_rendertree）。 */
JNIEXPORT jbyteArray JNICALL
Java_com_vus_android_VuaBridge_vuaRenderTreeBytes(JNIEnv *env, jclass clazz) {
    (void)clazz;
    VuaSession *s = vua_global_session(NULL);
    VuaScreen *cur = s ? vua_session_current(s) : NULL;
    if (!cur) return NULL;
    const char *tree = vua_screen_dump_rendertree(cur);
    if (!tree) return NULL;
    jsize len = (jsize)strlen(tree);
    jbyteArray arr = (*env)->NewByteArray(env, len);
    if (!arr) return NULL;
    (*env)->SetByteArrayRegion(env, arr, 0, len, (const jbyte *)tree);
    return arr;
}

/* ---- vuaRenderHash ------------------------------------------------------ */
/* 渲染树内容指纹（版本号协议）：Java 只凭指纹决定是否重建/取 JSON。
 * 指纹未变 = 内容未变，可跳过整树传输；配合页面 View 缓存直接显缓存。
 * 无屏返回 -1。注意：FNV-1a 结果高位可能为 1，直接转有符号 jlong 会变负，
 * 与"无屏 -1"冲突导致 Java 误判为空界面——必须屏蔽符号位（损失 1 bit 碰撞
 * 余量，2^-63 概率可忽略）。 */
JNIEXPORT jlong JNICALL
Java_com_vus_android_VuaBridge_vuaRenderHash(JNIEnv *env, jclass clazz) {
    (void)env; (void)clazz;
    VuaSession *s = vua_global_session(NULL);
    VuaScreen *cur = s ? vua_session_current(s) : NULL;
    if (!cur) return -1;
    return (jlong)(vua_screen_rendertree_hash(cur) & 0x7FFFFFFFFFFFFFFFULL);
}

/* ---- vuaScreenId -------------------------------------------------------- */
/* 当前屏序号（View diff）：序号不变 = 还是同一屏（仅 state 变化，可增量更新控件）；
 * 变化 = 换页（走页面 View 缓存或全量重建）。无屏返回 -1。 */
JNIEXPORT jlong JNICALL
Java_com_vus_android_VuaBridge_vuaScreenId(JNIEnv *env, jclass clazz) {
    (void)env; (void)clazz;
    VuaSession *s = vua_global_session(NULL);
    VuaScreen *cur = s ? vua_session_current(s) : NULL;
    if (!cur) return -1;
    return (jlong)vua_screen_seq(cur);
}

/* ---- vuaTrigger / vuaTriggerById ----------------------------------------- */
/* 把变量 JSON 转成 VusDict，派发到当前屏的事件。 */
static int do_trigger(JNIEnv *env, jstring event_or_id, jstring vars_json, int by_id) {
    VuaSession *s = vua_global_session(NULL);
    VuaScreen *cur = s ? vua_session_current(s) : NULL;
    if (!cur) return -1;

    const char *ev = event_or_id ? (*env)->GetStringUTFChars(env, event_or_id, 0) : NULL;
    const char *vars = vars_json ? (*env)->GetStringUTFChars(env, vars_json, 0) : NULL;

    VuaError err = {0};
    VusDict *dict = (vars && vars[0]) ? vua_dict_from_json(vars, &err) : NULL;

    if (by_id) vua_trigger_by_id(cur, ev ? ev : "", dict);
    else       vua_trigger_event(cur, ev ? ev : "", dict);

    if (dict) vus_unref((void *)dict);
    if (ev) (*env)->ReleaseStringUTFChars(env, event_or_id, ev);
    if (vars) (*env)->ReleaseStringUTFChars(env, vars_json, vars);
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_vus_android_VuaBridge_vuaTrigger(JNIEnv *env, jclass clazz,
                                          jstring event_name, jstring vars_json) {
    (void)clazz;
    return do_trigger(env, event_name, vars_json, 0);
}

JNIEXPORT jint JNICALL
Java_com_vus_android_VuaBridge_vuaTriggerById(JNIEnv *env, jclass clazz,
                                              jstring node_id, jstring vars_json) {
    (void)clazz;
    return do_trigger(env, node_id, vars_json, 1);
}