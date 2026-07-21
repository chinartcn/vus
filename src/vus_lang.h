/*
 * vus_lang.h — VUS 语言插件管理（内部接口）
 *
 * 提供语言插件注册表、共享库加载、预处理调度等内部实现。
 * 外部接口声明在 include/vus/vus_lang.h。
 */

#ifndef VUS_VUS_LANG_INTERNAL_H
#define VUS_VUS_LANG_INTERNAL_H

#include "../include/vus/vus_lang.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 从 vus.json 配置中自动加载语言插件。
 * 基于 config 中的语言插件名称，在标准路径下查找 .vulage 文件。
 * 标准查找路径：plugins/lang/<名称>/<名称>.vulage
 * 返回 0 成功，-1 未找到配置，-2 加载失败。 */
int vus_lang_load_from_config(const char *name, const char *project_dir);

/* 获取当前激活的语言插件名称（用于编译时显示信息）*/
const char *vus_lang_active_name(void);

#ifdef __cplusplus
}
#endif

#endif /* VUS_VUS_LANG_INTERNAL_H */