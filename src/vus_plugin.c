/*
 * vus_plugin.c — VUS 插件系统实现
 *
 * 实现插件注册表、共享库加载、生命周期管理。
 * 使用 dlopen/dlsym 动态加载 .so 插件。
 */

#include "../include/vus/vus_plugin.h"
#include "../include/vus/vus_abi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

/* ===================================================================
 * 内部数据结构
 * =================================================================== */

/* 插件注册表项 */
typedef struct {
    VusPlugin *plugin;       /* 插件描述符指针 */
    void      *handle;       /* dlopen 句柄（NULL 表示手动注册）*/
    int        initialized;  /* 是否已初始化 */
} VusPluginEntry;

/* 全局插件注册表 */
static VusPluginEntry g_plugins[VUS_MAX_PLUGINS];
static int g_plugin_count = 0;
static int g_abi_inited = 0;

/* 全局 VusPluginAPI 实例 */
static VusPluginAPI g_plugin_api;

/* ===================================================================
 * 内部 API 表填充
 * =================================================================== */

static void init_plugin_api(void) {
    if (g_abi_inited) return;

    g_plugin_api.version = vus_abi_version();
    g_plugin_api.compile_file        = vus_compile_file;
    g_plugin_api.compile_string      = vus_compile_string;
    g_plugin_api.compile_string_to_exe = vus_compile_string_to_exe;
    g_plugin_api.eval                = vus_eval;
    g_plugin_api.compiler_version    = vus_abi_version_string;

    g_abi_inited = 1;
}

/* ===================================================================
 * 插件注册
 * =================================================================== */

int vus_register_plugin(VusPlugin *plugin) {
    if (!plugin || !plugin->name || !plugin->init) {
        fprintf(stderr, "vus_plugin: invalid plugin descriptor (name or init missing)\n");
        return -1;
    }

    if (g_plugin_count >= VUS_MAX_PLUGINS) {
        fprintf(stderr, "vus_plugin: max plugins reached (%d)\n", VUS_MAX_PLUGINS);
        return -1;
    }

    /* 检查名称冲突 */
    for (int i = 0; i < g_plugin_count; i++) {
        if (strcmp(g_plugins[i].plugin->name, plugin->name) == 0) {
            fprintf(stderr, "vus_plugin: duplicate plugin name '%s'\n", plugin->name);
            return -2;
        }
    }

    g_plugins[g_plugin_count].plugin      = plugin;
    g_plugins[g_plugin_count].handle      = NULL;
    g_plugins[g_plugin_count].initialized = 0;
    g_plugin_count++;

    return 0;
}

/* ===================================================================
 * 共享库加载
 * =================================================================== */

int vus_plugin_load(const char *path) {
    if (!path) return -1;

    /* 打开共享库 */
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "vus_plugin: failed to load '%s': %s\n", path, dlerror());
        return -1;
    }

    /* 查找入口符号 */
    typedef void (*entry_func_t)(VusPlugin **);
    entry_func_t entry = (entry_func_t)dlsym(handle, "vus_plugin_entry");
    if (!entry) {
        fprintf(stderr, "vus_plugin: symbol 'vus_plugin_entry' not found in '%s': %s\n",
                path, dlerror());
        dlclose(handle);
        return -2;
    }

    /* 调用入口函数获取插件描述符 */
    VusPlugin *plugin = NULL;
    entry(&plugin);

    if (!plugin) {
        fprintf(stderr, "vus_plugin: plugin entry returned NULL in '%s'\n", path);
        dlclose(handle);
        return -2;
    }

    /* 注册插件 */
    int ret = vus_register_plugin(plugin);
    if (ret != 0) {
        dlclose(handle);
        return ret;
    }

    /* 将 handle 关联到注册表项 */
    for (int i = 0; i < g_plugin_count; i++) {
        if (g_plugins[i].plugin == plugin) {
            g_plugins[i].handle = handle;
            break;
        }
    }

    return 0;
}

/* ===================================================================
 * 生命周期管理
 * =================================================================== */

int vus_plugin_init_all(void) {
    init_plugin_api();

    int success_count = 0;
    for (int i = 0; i < g_plugin_count; i++) {
        if (g_plugins[i].initialized) continue;

        if (g_plugins[i].plugin->init(&g_plugin_api) == 0) {
            g_plugins[i].initialized = 1;
            success_count++;
        } else {
            fprintf(stderr, "vus_plugin: init failed for '%s'\n",
                    g_plugins[i].plugin->name);
        }
    }
    return success_count;
}

int vus_plugin_run_all(const char *input, char **output) {
    int success_count = 0;
    for (int i = 0; i < g_plugin_count; i++) {
        if (!g_plugins[i].initialized) continue;
        if (!g_plugins[i].plugin->run) continue;

        if (g_plugins[i].plugin->run(&g_plugin_api, input, output) == 0) {
            success_count++;
        }
    }
    return success_count;
}

void vus_plugin_cleanup_all(void) {
    for (int i = g_plugin_count - 1; i >= 0; i--) {
        if (!g_plugins[i].initialized) continue;
        if (g_plugins[i].plugin->cleanup) {
            g_plugins[i].plugin->cleanup(&g_plugin_api);
        }
        g_plugins[i].initialized = 0;
    }
}

/* ===================================================================
 * 查询与列表
 * =================================================================== */

VusPlugin *vus_plugin_find(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < g_plugin_count; i++) {
        if (strcmp(g_plugins[i].plugin->name, name) == 0) {
            return g_plugins[i].plugin;
        }
    }
    return NULL;
}

int vus_plugin_count(void) {
    return g_plugin_count;
}

void vus_plugin_list_all(void) {
    if (g_plugin_count == 0) {
        printf("No plugins registered.\n");
        return;
    }

    printf("Registered plugins (%d):\n", g_plugin_count);
    printf("  %-24s %-10s %-8s %s\n", "Name", "Version", "Status", "Description");
    printf("  %-24s %-10s %-8s %s\n", "----", "-------", "------", "-----------");
    for (int i = 0; i < g_plugin_count; i++) {
        VusPlugin *p = g_plugins[i].plugin;
        const char *status = g_plugins[i].initialized ? "active" : "inactive";
        const char *desc = p->description ? p->description : "";
        printf("  %-24s %-10s %-8s %s\n",
               p->name,
               p->version ? p->version : "",
               status,
               desc);
    }
}

/* ===================================================================
 * 卸载
 * =================================================================== */

void vus_plugin_unload_all(void) {
    /* 逆序清理 */
    vus_plugin_cleanup_all();

    for (int i = g_plugin_count - 1; i >= 0; i--) {
        if (g_plugins[i].handle) {
            dlclose(g_plugins[i].handle);
        }
        g_plugins[i].plugin      = NULL;
        g_plugins[i].handle      = NULL;
        g_plugins[i].initialized = 0;
    }
    g_plugin_count = 0;
}