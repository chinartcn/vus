/*
 * config.h — VUS 项目配置加载接口
 *
 * 从 vus.json 加载项目配置，提供配置字段访问函数。
 */

#ifndef VUS_CONFIG_H
#define VUS_CONFIG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============ 配置结构体 ============ */
typedef struct {
    char  project_dir[1024];   /* 项目根目录 */
    char  name[256];           /* 项目名称 */
    char  version[64];         /* 版本号 */
    char  style[32];           /* 语法风格（默认"函数"） */
    char  language_plugin[64]; /* 语言插件名称（如"易语言"，空=使用核心语法）*/
    char  main_file[256];      /* 主文件路径 */
    char  output_mode[16];     /* "c" 或 "exe" */
    char  list_mode[16];       /* "严格" 或 "混合" */
    int   debug;               /* 0 或 1 */
    char  target_platform[32]; /* "linux-gnu" / "linux-musl" / "android" */
    char  rt_dir[1024];        /* 运行时库目录 */
    char  build_dir[1024];     /* 构建输出目录 */

    /* 编译选项 */
    char  optimization[16];    /* "速度" / "体积" / "调试" */
    char  arm_version[16];     /* "ARM64" / "ARM32" */
} VusConfig;

/* ============ 配置操作 ============ */

/* 加载配置（从 project_dir/vus.json） */
int vus_config_load(VusConfig *config, const char *project_dir);

/* 获取主文件完整路径 */
void vus_config_main_path(VusConfig *config, char *buf, size_t buf_size);

/* 获取构建目录路径 */
void vus_config_build_path(VusConfig *config, char *buf, size_t buf_size);

/* 获取运行时库头文件路径 */
void vus_config_rt_header_path(VusConfig *config, char *buf, size_t buf_size);

/* 获取运行时库源文件路径 */
void vus_config_rt_source_path(VusConfig *config, char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif /* VUS_CONFIG_H */