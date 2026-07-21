/*
 * vus_apk.h — Android APK 构建接口
 *
 * 将 VUS 代码编译为 Android APK 包。
 * 依赖 Android NDK 进行交叉编译，用户可自定义 NDK 路径。
 */

#ifndef VUS_APK_H
#define VUS_APK_H

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* APK 构建结果 */
typedef struct {
    int  success;
    char apk_path[1024];
    char error_msg[1024];
    int  ndk_found;  /* 是否找到 NDK */
} VusApkResult;

/* 检测 Android NDK 路径
 * 优先级：ndk_path 参数 > ANDROID_NDK_HOME 环境变量 > 默认路径
 * 返回找到的路径，或 NULL */
const char *vus_apk_detect_ndk(const char *ndk_path);

/* 编译 VUS 文件为 APK
 * file — VUS 源文件路径
 * config — 编译器配置
 * ndk_path — NDK 路径（可为 NULL，自动检测）
 * app_name — 应用名称（可为 NULL，默认 "VusApp"）
 * output_dir — 输出目录（可为 NULL，默认 构建/） */
VusApkResult vus_compile_to_apk(const char *file, VusConfig *config,
                                 const char *ndk_path, const char *app_name,
                                 const char *output_dir);

#ifdef __cplusplus
}
#endif

#endif /* VUS_APK_H */