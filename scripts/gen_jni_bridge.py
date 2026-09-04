#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_jni_bridge.py — 从 Java native 声明自动生成 jni_bridge.c（P3 / 反馈 3.1）

背景：`Java_包名_类名_方法名` 必须与 Java 的 `public static native` 声明逐字
匹配，手工维护时包名一改、方法名一改就运行期崩溃（UnsatisfiedLinkError）。
本脚本把 jni_bridge.c 变成生成产物：
  - 从 <java_dir> 里所有 .java 的 native 声明提取方法名/参数，生成正确的
    JNI 符号（包名以 Java 文件的 `package` 行为准，包名变了符号自动跟着变）；
  - 方法体查 KNOWN_BODIES（本脚本内维护的纯 C 逻辑，与包名无关）；
  - 有 native 声明但没有对应方法体 → 非零退出并点名，强制声明与实现同步。

用法:
  python3 scripts/gen_jni_bridge.py --java <java源码目录> \
          [--package <包名(缺省读 Java 文件 package 行)>] \
          [--class <桥类名(缺省取含 native 的类名)>] \
          [--bridge <输出 C 文件(缺省 stdout)>]
  python3 scripts/gen_jni_bridge.py --expect --java <java源码目录> \
          [--package <包名>]   # 只打印应导出的 Java_* 符号（供 nm 核对）
"""

import argparse
import os
import re
import sys

# ---------------- JNI 类型映射 ----------------
JNI_TYPES = {
    "int": "jint",
    "long": "jlong",
    "void": "void",
    "boolean": "jboolean",
    "byte": "jbyte",
    "byte[]": "jbyteArray",
    "String": "jstring",
    "Object": "jobject",
    "Class": "jclass",
}


def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    return src


NATIVE_RE = re.compile(
    r"public\s+static\s+native\s+([\w\[\]\.<>]+)\s+(\w+)\s*\(([^)]*)\)\s*;",
    re.S)


def mangle_pkg(pkg):
    """com.vus.android -> com_vus_android（JNI 下划线符号名）"""
    return pkg.replace(".", "_")


def mangle_slash(pkg):
    """com.vus.android -> com/vus/android（FindClass 路径）"""
    return pkg.replace(".", "/")


def parse_java_args(arglist):
    """'String eventName, String varsJson' -> [('String','eventName'), ...] 不含 final 等修饰"""
    out = []
    for chunk in arglist.split(","):
        chunk = chunk.strip()
        if not chunk:
            continue
        # 去掉 final/可变参数省略号
        chunk = chunk.replace("final ", "").replace("...", "[]")
        parts = chunk.split()
        if len(parts) < 2:
            continue
        jtype = " ".join(parts[:-1])
        name = parts[-1]
        out.append((jtype, name))
    return out


def jni_fn_sig(jtype, name):
    """jstring name -> jstring name"""
    return "%s %s" % (jtype, name)


def collect_natives(java_dir, pkg_override=None, class_filter=None):
    """返回 {类名: {'package':pkg, 'methods':[(ret,name,args:list)]}}；类名取文件名去扩展名。"""
    classes = {}
    for fn in sorted(os.listdir(java_dir)):
        if not fn.endswith(".java"):
            continue
        path = os.path.join(java_dir, fn)
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            src = f.read()
        pkg_m = re.search(r"^\s*package\s+([\w\.]+)\s*;", src, re.M)
        pkg = pkg_override or (pkg_m.group(1) if pkg_m else None)
        if not pkg:
            continue
        cls = os.path.splitext(fn)[0]
        if class_filter and cls != class_filter:
            continue
        body = strip_comments(src)
        methods = []
        for m in NATIVE_RE.finditer(body):
            ret = m.group(1).strip()
            name = m.group(2).strip()
            args = parse_java_args(m.group(3))
            methods.append((ret, name, args))
        if methods:
            classes[cls] = {"package": pkg, "methods": methods}
    return classes


# ---------------- 方法体（纯 C，与包名无关；与 VuaBridge.java 的 native 一一对应） ----------------

BODY_VUA_INIT = """
    VuaError err = {0};
    VuaSession *s = vua_global_session(&err);
    if (!s) { fprintf(stderr, "[vua] vuaInit: %s\\n", err.msg); return -1; }
    /* 预加载控件表（已由 vuaSetRootDir 切到文件目录，用相对路径读）。失败不致命。 */
    {
        FILE *cf = fopen("vua_controls.json", "rb");
        if (cf) {
            fseek(cf, 0, SEEK_END); long l = ftell(cf); fseek(cf, 0, SEEK_SET);
            if (l > 0) {
                char *buf = (char *)malloc((size_t)l + 1);
                if (buf) {
                    size_t n = fread(buf, 1, (size_t)l, cf);
                    buf[n] = '\\0';
                    if (vua_control_table_load(buf, NULL) != 0)
                        fprintf(stderr, "[vua] 控件表加载失败\\n");
                    free(buf);
                }
            }
            fclose(cf);
        } else {
            fprintf(stderr, "[vua] 未找到 vua_controls.json\\n");
        }
    }
    /* 注册重绘钩子：vus_main 里 界面_显示/返回 换屏时，回 Java 重建 View。 */
    vua_session_set_rerender_hook(s, native_rerender_cb, NULL);
    int rc = vus_main();
    return rc == 0 ? 0 : -2;
"""

BODY_VUA_SET_ROOT_DIR = """
    if (!filesDir) return;
    const char *d = (*env)->GetStringUTFChars(env, filesDir, 0);
    if (d) { chdir(d); (*env)->ReleaseStringUTFChars(env, filesDir, d); }
"""

BODY_VUA_RENDER_TREE_BYTES = """
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
"""

BODY_VUA_RENDER_HASH = """
    VuaSession *s = vua_global_session(NULL);
    VuaScreen *cur = s ? vua_session_current(s) : NULL;
    if (!cur) return -1;
    return (jlong)(vua_screen_rendertree_hash(cur) & 0x7FFFFFFFFFFFFFFFULL);
"""

BODY_VUA_SCREEN_ID = """
    VuaSession *s = vua_global_session(NULL);
    VuaScreen *cur = s ? vua_session_current(s) : NULL;
    if (!cur) return -1;
    return (jlong)vua_screen_seq(cur);
"""

BODY_VUA_TRIGGER = """
    return do_trigger(env, eventName, varsJson, 0);
"""

BODY_VUA_TRIGGER_BY_ID = """
    return do_trigger(env, nodeId, varsJson, 1);
"""

KNOWN_BODIES = {
    "vuaInit": BODY_VUA_INIT,
    "vuaSetRootDir": BODY_VUA_SET_ROOT_DIR,
    "vuaRenderTreeBytes": BODY_VUA_RENDER_TREE_BYTES,
    "vuaRenderHash": BODY_VUA_RENDER_HASH,
    "vuaScreenId": BODY_VUA_SCREEN_ID,
    "vuaTrigger": BODY_VUA_TRIGGER,
    "vuaTriggerById": BODY_VUA_TRIGGER_BY_ID,
}

# ---------------- 固定基础设施（结构不变，只随包名/类名替换） ----------------

HEADER = """/* AUTO-GENERATED by scripts/gen_jni_bridge.py — DO NOT EDIT（P3）。
 * 由 Java native 声明生成：改 Java 签名后请重新生成，勿手改符号名。
 *   生成: python3 scripts/gen_jni_bridge.py --java <java目录> --bridge <输出.c>
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
/* {CLASS} 类的全局引用 + 静态方法 id，避免每次回调都 FindClass。 */
static jclass     g_{CLASS} = NULL;
static jmethodID  g_onNativeRerender = NULL;
static jmethodID  g_callJava = NULL;

/* 平台能力桥分发（定义见下）：VUS 内建 → {CLASS}.callJava（Java 暴露网络/文件能力）。 */
static void java_rpc_dispatch(const char *api, const char *args, char **out);
"""

JNI_ON_LOAD = """JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    g_vm = vm;
    JNIEnv *env = NULL;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_VERSION_1_6;
    jclass local = (*env)->FindClass(env, "{PKG}/{CLASS}");
    if (!local) return JNI_VERSION_1_6;
    g_{CLASS} = (*env)->NewGlobalRef(env, local);
    (*env)->DeleteLocalRef(env, local);
    if (g_{CLASS}) {
        g_onNativeRerender = (*env)->GetStaticMethodID(env, g_{CLASS}, "onNativeRerender", "()V");
        g_callJava = (*env)->GetStaticMethodID(env, g_{CLASS}, "callJava",
                                               "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
        /* 平台能力桥：网络/文件由 Java 暴露、VUS 调用（桌面/纯 native 未注册时内建回退） */
        vus_set_java_callback(java_rpc_dispatch);
    }
    return JNI_VERSION_1_6;
}


/* ---- 平台能力桥分发（native → Java 同步 RPC） --------------------------------
 * VUS 内建（网络_GET/文件_读取 等）→ vus_java_rpc → 本函数 → {CLASS}.callJava →
 * JSON 结果原样回传。Java 侧保证不抛异常（错误转 ok:0）。 */
static void java_rpc_dispatch(const char *api, const char *args, char **out) {
    *out = NULL;
    if (!g_vm || !g_{CLASS} || !g_callJava || !api || !args) return;
    JNIEnv *env = NULL;
    jint st = (*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6);
    int attached = 0;
    if (st != JNI_OK) {
        if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK) return;
        attached = 1;
    }
    jstring jap = (*env)->NewStringUTF(env, api);
    jstring jars = (*env)->NewStringUTF(env, args);
    jstring jres = (jstring)(*env)->CallStaticObjectMethod(env, g_{CLASS}, g_callJava, jap, jars);
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
    int attached = 0;
    if ((*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK) return;
        attached = 1;
    }
    (*env)->CallStaticVoidMethod(env, g_{CLASS}, g_onNativeRerender);
    if (attached) (*g_vm)->DetachCurrentThread(g_vm);
}


/* ---- 事件触发共用（按 事件名 / 控件 id 派发） ---- */
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
"""


def gen_bridge(classes, out):
    for cls, info in classes.items():
        pkg = info["package"]
        upkg = mangle_pkg(pkg)
        spkg = mangle_slash(pkg)
        out.write(HEADER.replace("{CLASS}", cls))
        out.write("\n")
        out.write(JNI_ON_LOAD.replace("{PKG}", spkg).replace("{CLASS}", cls))
        out.write("\n")
        for ret, name, args in info["methods"]:
            if ret not in JNI_TYPES:
                sys.stderr.write("gen_jni_bridge: %s.%s 返回类型 %s 无法映射 JNI\n"
                                 % (cls, name, ret))
                sys.exit(1)
            if ret not in ("byte[]", "String", "int", "long", "void"):
                pass  # 其余类型仍按 void 生成会失败, 交由编译器把关即可
            body = KNOWN_BODIES.get(name)
            if body is None:
                sys.stderr.write(
                    "gen_jni_bridge: 错误 —— Java native 声明了 `%s.%s`，但生成器 KNOWN_BODIES 没有对应实现。\n"
                    "  请在 scripts/gen_jni_bridge.py 的 KNOWN_BODIES 补上该方法的 C 实现后重新生成。\n"
                    "  这是刻意设计：JNI 符号必须与 Java 声明一一对应，禁止悄悄漏生成（P3）。\n" % (cls, name))
                sys.exit(2)
            jret = JNI_TYPES[ret]
            sig = "(JNIEnv *env, jclass clazz"
            for jt, an in args:
                if jt not in JNI_TYPES:
                    sys.stderr.write("gen_jni_bridge: %s.%s 参数 %s %s 无法映射 JNI\n"
                                     % (cls, name, jt, an))
                    sys.exit(1)
                sig += ", %s" % jni_fn_sig(JNI_TYPES[jt], an)
            sig += ")"
            # 无参方法：生成 (void) 消 unused 警告；有参方法体直接用参数名
            unused = "    (void)env; (void)clazz;\n" if not args else ""
            out.write("/* ---- %s ------------------------------------------------------- */\n" % name)
            out.write("JNIEXPORT %s JNICALL\n" % jret)
            out.write("Java_%s_%s_%s%s {\n" % (upkg, cls, name, sig))
            out.write(unused)
            out.write(body)
            out.write("\n}\n\n")
    return 0


def expect_symbols(classes):
    """只打印应导出的 Java_* 符号（供 nm 核对，P3.1 阶段二）。"""
    for cls, info in classes.items():
        upkg = mangle_pkg(info["package"])
        for _ret, name, _args in info["methods"]:
            print("Java_%s_%s_%s" % (upkg, cls, name))


def main():
    ap = argparse.ArgumentParser(description="从 Java native 声明生成 jni_bridge.c")
    ap.add_argument("--java", required=True, help="Java 源码目录（扫描 *.java 的 native 声明）")
    ap.add_argument("--package", default=None, help="包名（缺省读取 Java 文件 package 行）")
    ap.add_argument("--class", dest="cls", default=None, help="只处理指定桥类（缺省处理所有含 native 的类）")
    ap.add_argument("--bridge", default=None, help="输出 C 文件（缺省打印到 stdout）")
    ap.add_argument("--expect", action="store_true", help="只打印应导出符号列表")
    args = ap.parse_args()

    if not os.path.isdir(args.java):
        sys.stderr.write("gen_jni_bridge: java 目录不存在: %s\n" % args.java)
        sys.exit(1)
    classes = collect_natives(args.java, args.package, args.cls)
    if not classes:
        sys.stderr.write("gen_jni_bridge: %s 下未找到任何 native 声明\n" % args.java)
        sys.exit(1)

    if args.expect:
        expect_symbols(classes)
        return 0

    if args.bridge:
        with open(args.bridge, "w", encoding="utf-8") as f:
            gen_bridge(classes, f)
        sys.stderr.write("gen_jni_bridge: 已生成 %s\n" % args.bridge)
    else:
        gen_bridge(classes, sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main())