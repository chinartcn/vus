/*
 * vus_vusx.h — VUS .vusx 插件接口
 *
 * .vusx 插件是用 VUS 本身编写的功能扩展插件。
 * 在编译时由编译器自动解析、编译、链接到最终可执行文件中。
 *
 * 与 .vux 和 .vulage 的区别：
 *   .vux     — 运行时功能插件（Python/C 编写）
 *   .vulage  — 编译前语法插件（Python/C 编写）
 *   .vusx    — 编译时功能插件（VUS 编写）
 *
 * === 加载时机 ===
 * .vusx 插件在编译主程序之前被解析和编译：
 *   读取 vus.json → 解析 vusx 依赖 → 编译 .vusx → 链接到主程序
 *
 * === .vusx 目录结构 ===
 *   my_plugin.vusx/
 *   ├── vusx.json      # 插件元数据（必需）
 *   ├── main.vus       # VUS 源码（必需）
 *   └── 依赖.txt       # 依赖列表（可选）
 *
 * === vusx.json 格式 ===
 *   {
 *       "名称": "my_utils",
 *       "版本": "1.0.0",
 *       "入口": "main.vus",
 *       "导出": ["问候", "计算"],
 *       "依赖": {}
 *   }
 *
 * === vus.json 中声明 ===
 *   {
 *       "vusx依赖": ["my_utils.vusx", "path/to/other.vusx"],
 *       ...
 *   }
 */

#ifndef VUS_VUS_VUSX_H
#define VUS_VUS_VUSX_H

#include "vus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================
 * 常量
 * =================================================================== */

#define VUS_MAX_VUSX_DEPS    16   /* 最大 vusx 依赖数量 */
#define VUS_MAX_VUSX_EXPORTS 32   /* 最大导出函数数量 */
#define VUS_VUSX_NAME_LEN    64   /* 插件名称最大长度 */
#define VUS_VUSX_PATH_LEN    256  /* 路径最大长度 */

/* ===================================================================
 * VusVusxPlugin — .vusx 插件描述符
 * =================================================================== */
typedef struct {
    char name[VUS_VUSX_NAME_LEN];      /* 插件名称 */
    char version[32];                   /* 插件版本 */
    char dir[VUS_VUSX_PATH_LEN];        /* 插件目录路径 */
    char main_vus[VUS_VUSX_PATH_LEN];   /* main.vus 路径 */
    char c_output[VUS_VUSX_PATH_LEN];   /* 编译后的 C 文件路径 */
    char obj_output[VUS_VUSX_PATH_LEN]; /* 编译后的 .o 文件路径 */
    char exports[VUS_MAX_VUSX_EXPORTS][VUS_VUSX_NAME_LEN]; /* 导出函数 */
    int  export_count;                  /* 导出函数数量 */
} VusVusxPlugin;

/* ===================================================================
 * .vusx 操作
 * =================================================================== */

/* 解析 .vusx 目录，填充 VusVusxPlugin 结构体。
 * path — .vusx 目录路径。
 * 返回 0 成功，-1 目录不存在，-2 vusx.json 解析失败。 */
int vus_vusx_resolve(const char *path, VusVusxPlugin *plugin);

/* 编译 .vusx 插件：将 VUS 源码编译为 C 代码，再编译为 .o 文件。
 * 返回 0 成功，非 0 失败。 */
int vus_vusx_compile(VusVusxPlugin *plugin, VusConfig *config);

/* 从 config 中解析所有 vusx 依赖。
 * plugins — 输出数组，count — 输入为数组大小，输出为实际数量。
 * 返回 0 成功，-1 超过最大数量，-2 解析失败。 */
int vus_vusx_resolve_all(VusConfig *config, VusVusxPlugin *plugins, int *count);

/* 编译所有 vusx 依赖。返回 0 成功，非 0 失败。 */
int vus_vusx_compile_all(VusVusxPlugin *plugins, int count, VusConfig *config);

/* 清理 vusx 插件（释放临时文件）。 */
void vus_vusx_cleanup_all(VusVusxPlugin *plugins, int count);

#ifdef __cplusplus
}
#endif

#endif /* VUS_VUS_VUSX_H */