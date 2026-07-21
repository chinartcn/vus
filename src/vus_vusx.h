/*
 * vus_vusx.h — VUS .vusx 插件管理（内部接口）
 *
 * 提供 .vusx 插件解析、编译、链接等内部实现。
 * 外部接口声明在 include/vus/vus_vusx.h。
 */

#ifndef VUS_VUS_VUSX_INTERNAL_H
#define VUS_VUS_VUSX_INTERNAL_H

#include "../include/vus/vus_vusx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 将 vusx 依赖的 .o 文件路径追加到 GCC 命令字符串中。
 * cmd      — GCC 命令缓冲区（已包含基础命令）
 * cmd_size — 缓冲区大小
 * plugins  — vusx 插件数组
 * count    — 插件数量
 * 返回 0 成功，-1 命令太长。 */
int vus_vusx_append_objects(char *cmd, size_t cmd_size,
                            VusVusxPlugin *plugins, int count);

#ifdef __cplusplus
}
#endif

#endif /* VUS_VUS_VUSX_INTERNAL_H */