/*
 * vus_plugin.h — VUS 插件系统接口
 *
 * 提供插件注册、加载与生命周期管理 API。
 * 插件是动态共享库（.so / .dll），通过 VusPlugin 结构体暴露
 * 给编译器，可在编译流水线的各个阶段插入自定义行为。
 *
 * === 插件编写示例 ===
 *   #include <vus/vus_plugin.h>
 *
 *   static int my_init(VusPluginAPI *api) {
 *       printf("插件已加载，ABI 版本: %d\n", api->version);
 *       return 0;
 *   }
 *
 *   static int my_run(VusPluginAPI *api, const char *input, char **output) {
 *       *output = strdup("插件处理结果");
 *       return 0;
 *   }
 *
 *   static void my_cleanup(VusPluginAPI *api) { }
 *
 *   VusPlugin g_plugin = {
 *       .name    = "my_plugin",
 *       .version = "1.0.0",
 *       .init    = my_init,
 *       .run     = my_run,
 *       .cleanup = my_cleanup
 *   };
 *
 *   // 编译器加载时会自动调用此函数
 *   VUS_PLUGIN_EXPORT void vus_plugin_entry(VusPlugin **plugin) {
 *       *plugin = &g_plugin;
 *   }
 *
 * === 编译为共享库 ===
 *   gcc -shared -fPIC -o my_plugin.so my_plugin.c -I/path/to/vus/include
 *
 * === 使用方法 ===
 *   vus_plugin_load("my_plugin.so");
 *   vus_plugin_init_all();
 *   vus_plugin_run_all("input", &output);
 *   vus_plugin_cleanup_all();
 */

#ifndef VUS_VUS_PLUGIN_H
#define VUS_VUS_PLUGIN_H

#include "vus.h"              /* VusResult, VusConfig */
#include "vus_abi.h"          /* vus_compile_file, vus_eval, ... */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================
 * 插件对外导出宏
 * ===================================================================
 * 插件 .so 中必须且仅定义一个 VUS_PLUGIN_EXPORT 符号。
 * 编译器在加载 .so 时通过 dlsym("vus_plugin_entry") 定位入口。
 */
#if defined(_WIN32) || defined(_WIN64)
#  define VUS_PLUGIN_EXPORT __declspec(dllexport)
#else
#  define VUS_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

/* ===================================================================
 * VusPluginAPI — 插件可调用的编译器 API 表
 * ===================================================================
 * 插件通过此结构体访问编译器功能，无需直接链接编译器。
 * 编译器在调用 plugin->init() 前填充此结构体并传入。
 */
typedef struct {
    /* ABI 版本号（编译器的 ABI 版本，用于兼容性检查）*/
    int version;

    /* 编译 .vus 文件 → C 代码 */
    VusResult (*compile_file)(const char *path, VusConfig *config);

    /* 编译 VUS 源码字符串 → C 代码 */
    VusResult (*compile_string)(const char *source, VusConfig *config);

    /* 编译字符串 → 可执行文件 */
    VusResult (*compile_string_to_exe)(const char *source, VusConfig *config);

    /* 求值 VUS 表达式，返回 stdout 输出 */
    VusResult (*eval)(const char *code, VusConfig *config, char *output);

    /* 获取编译器版本 */
    const char *(*compiler_version)(void);
} VusPluginAPI;

/* ===================================================================
 * VusPlugin — 插件描述符
 * ===================================================================
 * 每个插件必须定义此结构体并通过 vus_plugin_entry 导出。
 * 所有字段均为必填（除非明确指出可选）。
 */
typedef struct VusPlugin {
    /* 插件名称（唯一标识符，不可为 NULL）*/
    const char *name;

    /* 插件版本号（如 "1.0.0"）*/
    const char *version;

    /* ── 生命周期回调 ── */

    /* 初始化：插件加载后、使用前调用。
     * 返回 0 表示成功，非 0 表示初始化失败（编译器将卸载插件）。
     * api 指针在插件的整个生命周期内有效。 */
    int  (*init)(VusPluginAPI *api);

    /* 执行：插件的主要功能入口。
     * input   — 输入数据（由调用方提供，可为 NULL）
     * output  — 输出数据（由插件分配，调用方负责 free）
     * 返回 0 表示成功，非 0 表示执行失败。 */
    int  (*run)(VusPluginAPI *api, const char *input, char **output);

    /* 清理：插件卸载前调用，释放插件分配的资源。
     * 可为 NULL（表示无需清理）。 */
    void (*cleanup)(VusPluginAPI *api);

    /* ── 可选字段 ── */

    /* 插件描述（用于 --list-plugins 显示，可为 NULL）*/
    const char *description;

    /* 插件作者（可为 NULL）*/
    const char *author;
} VusPlugin;

/* ===================================================================
 * 插件注册和管理
 * =================================================================== */

/* 最大注册插件数量 */
#define VUS_MAX_PLUGINS 64

/* 注册一个插件。
 * 插件通常在加载 .so 时自动注册，也可手动调用此函数。
 * 返回 0 成功，-1 插件已满，-2 名称冲突。 */
int vus_register_plugin(VusPlugin *plugin);

/* 从共享库加载插件。
 * path — .so / .dll 文件路径。
 * 返回 0 成功，-1 无法打开，-2 未找到入口符号。 */
int vus_plugin_load(const char *path);

/* 初始化所有已注册插件。
 * 遍历插件列表，调用每个插件的 init 回调。
 * 返回成功初始化的插件数量（失败的不计入）。 */
int vus_plugin_init_all(void);

/* 运行所有已注册插件。
 * 对每个插件调用其 run 回调。
 * 返回成功运行的插件数量。 */
int vus_plugin_run_all(const char *input, char **output);

/* 清理所有已注册插件。
 * 遍历插件列表（逆序），调用每个插件的 cleanup 回调。 */
void vus_plugin_cleanup_all(void);

/* 按名称查找已注册插件。
 * 返回插件指针，未找到时返回 NULL。 */
VusPlugin *vus_plugin_find(const char *name);

/* 获取已注册插件的数量。 */
int vus_plugin_count(void);

/* 列出所有已注册插件（打印到 stdout）。 */
void vus_plugin_list_all(void);

/* 卸载所有插件并释放内部资源。 */
void vus_plugin_unload_all(void);

#ifdef __cplusplus
}
#endif

#endif /* VUS_VUS_PLUGIN_H */