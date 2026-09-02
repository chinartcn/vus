/*
 * vus_apk.c — Android APK 构建实现
 *
 * 将 VUS 代码编译为 Android APK 包。
 * NDK 可选：有 NDK 时自动交叉编译，无 NDK 时生成项目结构提示手动构建。
 */

#include "vus_apk.h"
#include "generator.h"
#include "parser.h"
#include "lexer.h"
#include "../include/vus/vus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>

/* 主函数签名替换常量（APK 构建中需重命名 main → vus_main 避免与 Android 入口冲突） */
#define MAIN_SIG_VOID   "int main(void)"
#define MAIN_VUS_VOID   "int vus_main(void)"
#define MAIN_SIG_VOID_LEN 14

#define MAIN_SIG_ARGC   "int main(int argc"
#define MAIN_VUS_ARGC   "int vus_main(int argc"

/* ============ NDK 检测 ============ */

const char *vus_apk_detect_ndk(const char *ndk_path) {
    /* 1. 如果传入了路径，检查是否有效 */
    if (ndk_path && ndk_path[0]) {
        struct stat st;
        if (stat(ndk_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            return ndk_path;
        }
    }

    /* 2. 检查 ANDROID_NDK_HOME 环境变量 */
    const char *env = getenv("ANDROID_NDK_HOME");
    if (env && env[0]) {
        struct stat st;
        if (stat(env, &st) == 0 && S_ISDIR(st.st_mode)) {
            return env;
        }
    }

    /* 3. 检查 ANDROID_HOME 下的 NDK */
    const char *android_home = getenv("ANDROID_HOME");
    if (android_home && android_home[0]) {
        static char path[1024];
        /* 尝试常见 NDK 版本目录 */
        const char *versions[] = {
            "ndk/27.0.12077973", "ndk/26.0.10792818",
            "ndk/25.2.9519653", "ndk/24.0.8215888",
            "ndk/23.1.7779620", "ndk-bundle", NULL
        };
        for (int i = 0; versions[i]; i++) {
            snprintf(path, sizeof(path), "%s/%s", android_home, versions[i]);
            struct stat st;
            if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
                return path;
            }
        }
    }

    /* 4. 检查常见默认路径 */
    const char *default_paths[] = {
        "/usr/local/lib/android/sdk/ndk-bundle",
        "/opt/android-ndk",
        "$HOME/Android/Sdk/ndk-bundle",
        NULL
    };
    static char path[1024];
    for (int i = 0; default_paths[i]; i++) {
        /* 扩展 $HOME */
        if (default_paths[i][0] == '$') {
            const char *home = getenv("HOME");
            if (!home) continue;
            snprintf(path, sizeof(path), "%s%s", home, default_paths[i] + 5);
        } else {
            snprintf(path, sizeof(path), "%s", default_paths[i]);
        }
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            return path;
        }
    }

    return NULL; /* 未找到 NDK */
}

/* ============ 辅助函数 ============ */

/* 创建目录（如果不存在） */
static int ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return 0;
        return -1;
    }
    return mkdir(path, 0755);
}

/* 写入文件 */
static int write_file(const char *path, const char *content) {
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fputs(content, fp);
    fclose(fp);
    return 0;
}

/* 读模板文件 src，把其中所有出现的 old 替换为 rep，写为新文件 dst。
 * 用于把 examples/vua-android 的 VUA 模板（包名占位 com.vus.android）复用为
 * 当前 APK 的实际包名版本。失败返回 -1。
 */
static int copy_file_subst(const char *src, const char *dst,
                           const char *old, const char *rep) {
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    fseek(in, 0, SEEK_END); long len = ftell(in); fseek(in, 0, SEEK_SET);
    if (len < 0) { fclose(in); return -1; }
    char *data = (char *)malloc((size_t)len + 1);
    if (!data) { fclose(in); return -1; }
    size_t n = fread(data, 1, (size_t)len, in);
    fclose(in);
    data[n] = '\0';

    FILE *out = fopen(dst, "w");
    if (!out) { free(data); return -1; }
    if (old && old[0] && rep) {
        size_t olen = strlen(old);
        char *p = data, *q;
        while ((q = strstr(p, old)) != NULL) {
            fwrite(p, 1, (size_t)(q - p), out);
            fputs(rep, out);
            p = q + olen;
        }
        fputs(p, out);
    } else {
        fputs(data, out);
    }
    fclose(out);
    free(data);
    return 0;
}

/* 简单复制文件 src → dst（不做内容替换）。失败返回 -1。 */
static int copy_file(const char *src, const char *dst) {
    return copy_file_subst(src, dst, NULL, NULL);
}

/* ============ 模板生成 ============ */

/* 生成 AndroidManifest.xml */
static const char *gen_manifest(const char *app_name, const char *pkg_name) {
    static char buf[4096];
    snprintf(buf, sizeof(buf),
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\"\n"
        "    package=\"%s\">\n"
        "    <application\n"
        "        android:label=\"%s\"\n"
        "        android:allowBackup=\"true\"\n"
        "        android:hasCode=\"true\">\n"
        "        <activity android:name=\".MainActivity\"\n"
        "            android:exported=\"true\">\n"
        "            <intent-filter>\n"
        "                <action android:name=\"android.intent.action.MAIN\" />\n"
        "                <category android:name=\"android.intent.category.LAUNCHER\" />\n"
        "            </intent-filter>\n"
        "        </activity>\n"
        "    </application>\n"
        "</manifest>\n",
        pkg_name, app_name);
    return buf;
}

/* 生成 Android.mk */
static const char *gen_android_mk(void) {
    return
        "LOCAL_PATH := $(call my-dir)\n"
        "include $(CLEAR_VARS)\n\n"
        "LOCAL_MODULE := vus_app\n"
        "LOCAL_SRC_FILES := vus_app.c libvus_rt.c vua.c yyjson.c jni_bridge.c\n"
        "LOCAL_C_INCLUDES := $(LOCAL_PATH)\n"
        "LOCAL_LDLIBS := -llog -lm\n"
        "LOCAL_CFLAGS := -O2 -std=c11 -DVUS_HAVE_CURL\n\n"
        "include $(BUILD_SHARED_LIBRARY)\n";
}

/* 生成 Application.mk */
static const char *gen_application_mk(void) {
    return
        "APP_ABI := arm64-v8a armeabi-v7a x86_64\n"
        "APP_PLATFORM := android-21\n"
        "APP_STL := c++_static\n";
}

/* ============ APK 编译主函数 ============ */

VusApkResult vus_compile_to_apk(const char *file, VusConfig *config,
                                 const char *ndk_path, const char *app_name,
                                 const char *output_dir) {
    VusApkResult result = {0};
    result.success = 0;

    /* 1. 编译 VUS 到 C */
    VusResult vus_result = vus_compile_to_c(file, config);
    if (!vus_result.success) {
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "VUS 编译失败: %s", vus_result.error_msg);
        return result;
    }

    /* 2. 确定应用名称和包名 */
    const char *name = app_name ? app_name : "VusApp";
    char pkg_name[256];
    /* 使用反向域名：com.vus.userapp */
    snprintf(pkg_name, sizeof(pkg_name), "com.vus.%s", name);
    /* 小写化包名 */
    for (int i = 0; pkg_name[i]; i++) {
        if (pkg_name[i] >= 'A' && pkg_name[i] <= 'Z')
            pkg_name[i] = pkg_name[i] - 'A' + 'a';
    }

    /* 3. 确定输出目录 */
    char apk_dir[1024];
    if (output_dir && output_dir[0]) {
        snprintf(apk_dir, sizeof(apk_dir), "%s", output_dir);
    } else {
        snprintf(apk_dir, sizeof(apk_dir), "%s/apk_%s", config->build_dir, name);
    }

    /* 4. 创建目录结构 */
    if (ensure_dir(apk_dir) != 0) {
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "无法创建输出目录: %s", apk_dir);
        return result;
    }

    /* 4a. jni/ 目录 */
    char jni_dir[1024];
    snprintf(jni_dir, sizeof(jni_dir), "%s/jni", apk_dir);
    ensure_dir(jni_dir);

    /* 4b. java/ 目录 */
    char java_dir[1024];
    char java_pkg_dir[1024];
    snprintf(java_dir, sizeof(java_dir), "%s/java", apk_dir);
    ensure_dir(java_dir);
    /* 根据包名创建子目录（com.vus.demo → java/com/vus/demo） */
    snprintf(java_pkg_dir, sizeof(java_pkg_dir), "%s/java/", apk_dir);
    {
        char *cur = java_pkg_dir + strlen(java_pkg_dir);
        for (const char *q = pkg_name; *q; q++) {
            if (*q == '.') {
                *cur = '\0';
                ensure_dir(java_pkg_dir);
                *cur++ = '/';
            } else {
                *cur++ = *q;
            }
        }
        *cur = '\0';
        ensure_dir(java_pkg_dir);
    }

    /* 5. 复制 VUS 生成的 C 代码到 jni/ 目录，并将 main 函数重命名为 vus_main */
    char vus_c_path[1024];
    snprintf(vus_c_path, sizeof(vus_c_path), "%s/vus_app.c", jni_dir);
    {
        FILE *src = fopen(vus_result.c_output_path, "r");
        if (!src) {
            snprintf(result.error_msg, sizeof(result.error_msg),
                     "无法读取生成的 C 代码: %s", vus_result.c_output_path);
            return result;
        }
        FILE *dst = fopen(vus_c_path, "w");
        if (!dst) {
            fclose(src);
            snprintf(result.error_msg, sizeof(result.error_msg),
                     "无法写入: %s", vus_c_path);
            return result;
        }
        char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf) - 1, src)) > 0) {
            buf[n] = '\0';
            /* 将 main 替换为 vus_main 避免与 Android 入口冲突 */
            char *p = buf;
            char *q;
            while ((q = strstr(p, MAIN_SIG_VOID)) != NULL) {
                fwrite(p, 1, q - p, dst);
                fputs(MAIN_VUS_VOID, dst);
                p = q + MAIN_SIG_VOID_LEN;
            }
            /* 也处理 int main(int argc, char** argv) 的情况 */
            while ((q = strstr(p, MAIN_SIG_ARGC)) != NULL) {
                fwrite(p, 1, q - p, dst);
                fputs(MAIN_VUS_ARGC, dst);
                p = q + MAIN_SIG_VOID_LEN;
            }
            fwrite(p, 1, strlen(p), dst);
        }
        fclose(src);
        fclose(dst);
    }

    /* 5a. 复制 libvus_rt.h 和 libvus_rt.c 到 jni/ 目录 */
    {
        char rt_h_src[1024], rt_c_src[1024];
        char rt_h_dst[1024], rt_c_dst[1024];
        snprintf(rt_h_src, sizeof(rt_h_src), "%s/libvus_rt.h", config->rt_dir);
        snprintf(rt_c_src, sizeof(rt_c_src), "%s/libvus_rt.c", config->rt_dir);
        snprintf(rt_h_dst, sizeof(rt_h_dst), "%s/libvus_rt.h", jni_dir);
        snprintf(rt_c_dst, sizeof(rt_c_dst), "%s/libvus_rt.c", jni_dir);
        /* 复制 libvus_rt.h */
        FILE *src = fopen(rt_h_src, "r");
        if (src) {
            FILE *dst = fopen(rt_h_dst, "w");
            if (dst) {
                char copy_buf[4096];
                size_t n;
                while ((n = fread(copy_buf, 1, sizeof(copy_buf), src)) > 0)
                    fwrite(copy_buf, 1, n, dst);
                fclose(dst);
            }
            fclose(src);
        }
        /* 复制 libvus_rt.c */
        src = fopen(rt_c_src, "r");
        if (src) {
            FILE *dst = fopen(rt_c_dst, "w");
            if (dst) {
                char copy_buf[4096];
                size_t n;
                while ((n = fread(copy_buf, 1, sizeof(copy_buf), src)) > 0)
                    fwrite(copy_buf, 1, n, dst);
                fclose(dst);
            }
            fclose(src);
        }
    }

    /* 5b. 复制 VUA 界面运行时与 yyjson 到 jni/ 目录 */
    {
        const char *rt = config->rt_dir;
        char src[1024], dst[1024];
        snprintf(src, sizeof(src), "%s/vua.h", rt);
        snprintf(dst, sizeof(dst), "%s/vua.h", jni_dir);
        copy_file(src, dst);
        snprintf(src, sizeof(src), "%s/vua.c", rt);
        snprintf(dst, sizeof(dst), "%s/vua.c", jni_dir);
        copy_file(src, dst);

        snprintf(src, sizeof(src), "%s/yyjson/yyjson.c", rt);
        snprintf(dst, sizeof(dst), "%s/yyjson.c", jni_dir);
        copy_file(src, dst);
        snprintf(src, sizeof(src), "%s/yyjson/yyjson.h", rt);
        snprintf(dst, sizeof(dst), "%s/yyjson.h", jni_dir);
        copy_file(src, dst);
    }

    /* 6. 生成 VUA JNI 桥接代码（examples/vua-android/jni_bridge.c → 当前包名） */
    {
        char tpl_dir[1024];
        snprintf(tpl_dir, sizeof(tpl_dir), "%s/../examples/vua-android", config->rt_dir);
        char tpl[1024], dst[1024];
        snprintf(tpl, sizeof(tpl), "%s/jni_bridge.c", tpl_dir);
        snprintf(dst, sizeof(dst), "%s/jni_bridge.c", jni_dir);

        /* 包名 → JNI 各种写法（点/斜杠/下划线） */
        char pkg_slash[256], pkg_under[256];
        size_t k = 0;
        for (size_t i = 0; pkg_name[i]; i++) pkg_slash[k++] = (pkg_name[i] == '.') ? '/' : pkg_name[i];
        pkg_slash[k] = '\0';
        k = 0;
        for (size_t i = 0; pkg_name[i]; i++) pkg_under[k++] = (pkg_name[i] == '.') ? '_' : pkg_name[i];
        pkg_under[k] = '\0';

        /* 分步替换：斜杠 JNI 类名、下划线符号名、点包名，最后落到 jni_bridge.c */
        char tmp[1024];
        snprintf(tmp, sizeof(tmp), "%s/_jb.c", jni_dir);
        if (copy_file_subst(tpl, tmp, "com_vus_android", pkg_under) == 0 &&
            copy_file_subst(tmp, dst, "com/vus/android", pkg_slash) == 0) {
            remove(tmp);
        } else {
            remove(tmp);
        }
    }

    /* 7. 生成 Android.mk 和 Application.mk */
    char mk_path[1024];
    snprintf(mk_path, sizeof(mk_path), "%s/Android.mk", jni_dir);
    write_file(mk_path, gen_android_mk());

    char app_mk_path[1024];
    snprintf(app_mk_path, sizeof(app_mk_path), "%s/Application.mk", jni_dir);
    write_file(app_mk_path, gen_application_mk());

    /* 8. 生成 AndroidManifest.xml */
    char manifest_path[1024];
    snprintf(manifest_path, sizeof(manifest_path), "%s/AndroidManifest.xml", apk_dir);
    write_file(manifest_path, gen_manifest(name, pkg_name));

    /* 9. 生成 VUA Java 壳（MainActivity / VuaBridge / VuaRenderer，包名占位替换） */
    {
        char tpl_dir[1024];
        snprintf(tpl_dir, sizeof(tpl_dir), "%s/../examples/vua-android", config->rt_dir);
        char tpl[1024], dst[1024];
        snprintf(tpl, sizeof(tpl), "%s/MainActivity.java", tpl_dir);
        snprintf(dst, sizeof(dst), "%s/MainActivity.java", java_pkg_dir);
        copy_file_subst(tpl, dst, "com.vus.android", pkg_name);

        snprintf(tpl, sizeof(tpl), "%s/VuaBridge.java", tpl_dir);
        snprintf(dst, sizeof(dst), "%s/VuaBridge.java", java_pkg_dir);
        copy_file_subst(tpl, dst, "com.vus.android", pkg_name);

        snprintf(tpl, sizeof(tpl), "%s/VuaRenderer.java", tpl_dir);
        snprintf(dst, sizeof(dst), "%s/VuaRenderer.java", java_pkg_dir);
        copy_file_subst(tpl, dst, "com.vus.android", pkg_name);
    }

    /* 10. 检测 NDK 并尝试交叉编译 */
    const char *ndk = vus_apk_detect_ndk(ndk_path);
    result.ndk_found = (ndk != NULL);

    if (ndk) {
        /* 使用 NDK 编译 .so */
        char ndk_build[1024];
        snprintf(ndk_build, sizeof(ndk_build), "cd \"%s\" && \"%s/ndk-build\" NDK_PROJECT_PATH=\"%s\" APP_BUILD_SCRIPT=\"%s/Android.mk\" 2>&1",
                 jni_dir, ndk, apk_dir, jni_dir);

        int build_status = system(ndk_build);
        if (build_status != 0) {
            snprintf(result.error_msg, sizeof(result.error_msg),
                     "NDK 编译失败（退出码: %d），项目文件已生成在: %s\n"
                     "请手动执行: cd %s && ndk-build",
                     build_status, apk_dir, jni_dir);
            /* 不返回失败，项目结构已生成 */
            snprintf(result.apk_path, sizeof(result.apk_path),
                     "%s（项目结构，需手动 ndk-build 编译 .so）", apk_dir);
            result.success = 1;
            return result;
        }

        snprintf(result.apk_path, sizeof(result.apk_path),
                 "%s（已编译 .so，需进一步打包为 APK）", apk_dir);
        result.success = 1;
    } else {
        /* 未找到 NDK，生成项目结构提示 */
        snprintf(result.apk_path, sizeof(result.apk_path),
                 "%s（项目结构已生成，需安装 NDK 后编译）", apk_dir);
        result.success = 1;
    }

    return result;
}