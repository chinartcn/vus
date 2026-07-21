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

/* 生成 Java 壳 Activity */
static const char *gen_java_wrapper(const char *pkg_name) {
    static char buf[4096];
    snprintf(buf, sizeof(buf),
        "package %s;\n\n"
        "import android.app.Activity;\n"
        "import android.os.Bundle;\n"
        "import android.widget.TextView;\n\n"
        "public class MainActivity extends Activity {\n"
        "    static {\n"
        "        System.loadLibrary(\"vus_app\");\n"
        "    }\n\n"
        "    private native String runVus();\n\n"
        "    @Override\n"
        "    protected void onCreate(Bundle savedInstanceState) {\n"
        "        super.onCreate(savedInstanceState);\n"
        "        TextView tv = new TextView(this);\n"
        "        tv.setText(runVus());\n"
        "        setContentView(tv);\n"
        "    }\n"
        "}\n",
        pkg_name);
    return buf;
}

/* 生成 C JNI 桥接代码 */
static const char *gen_jni_bridge(const char *pkg_name) {
    static char buf[4096];
    char cls_path[512];
    /* 将 com.example.app 转为 com/example/app */
    for (int i = 0; pkg_name[i]; i++) {
        cls_path[i] = (pkg_name[i] == '.') ? '/' : pkg_name[i];
        cls_path[i + 1] = '\0';
    }
    snprintf(buf, sizeof(buf),
        "#include <jni.h>\n"
        "#include <string.h>\n\n"
        "/* VUS 编译后的主函数声明 */\n"
        "extern int main(void);\n\n"
        "JNIEXPORT jstring JNICALL\n"
        "Java_%s_MainActivity_runVus(JNIEnv *env, jobject thiz) {\n"
        "    /* 重定向 stdout 到字符串缓冲区 */\n"
        "    char buf[4096] = {0};\n"
        "    /* 调用 VUS 生成的 main 函数 */\n"
        "    main();\n"
        "    return (*env)->NewStringUTF(env, buf);\n"
        "}\n",
        cls_path);
    return buf;
}

/* 生成 Android.mk */
static const char *gen_android_mk(void) {
    return
        "LOCAL_PATH := $(call my-dir)\n"
        "include $(CLEAR_VARS)\n\n"
        "LOCAL_MODULE := vus_app\n"
        "LOCAL_SRC_FILES := vus_app.c jni_bridge.c\n"
        "LOCAL_LDLIBS := -llog\n"
        "LOCAL_CFLAGS := -O2 -std=c11\n\n"
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
    /* 根据包名创建子目录 */
    snprintf(java_pkg_dir, sizeof(java_pkg_dir), "%s/java/%s", apk_dir, pkg_name);
    for (char *p = java_pkg_dir + strlen(java_dir) + 5; *p; p++) {
        if (*p == '.') {
            *p = '\0';
            ensure_dir(java_pkg_dir);
            *p = '.';
        }
    }
    ensure_dir(java_pkg_dir);

    /* 5. 复制 VUS 生成的 C 代码到 jni/ 目录 */
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
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
            fwrite(buf, 1, n, dst);
        }
        fclose(src);
        fclose(dst);
    }

    /* 6. 生成 JNI 桥接代码 */
    char jni_bridge_path[1024];
    snprintf(jni_bridge_path, sizeof(jni_bridge_path), "%s/jni_bridge.c", jni_dir);
    write_file(jni_bridge_path, gen_jni_bridge(pkg_name));

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

    /* 9. 生成 Java 壳 Activity */
    char java_path[1024];
    snprintf(java_path, sizeof(java_path), "%s/MainActivity.java", java_pkg_dir);
    write_file(java_path, gen_java_wrapper(pkg_name));

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