/*
 * vus_lang.c — VUS 语言插件系统实现
 *
 * 实现语言插件（.vulage）注册表、共享库加载、预处理调度。
 * 使用 dlopen/dlsym 动态加载 .vulage 共享库。
 *
 * 语言插件用于在编译前端将不同风格的源代码转换为标准 VUS 函数风格，
 * 确保核心编译器只需处理统一的语法。
 */

#define _POSIX_C_SOURCE 200809L

#include "vus_lang.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

/* ===================================================================
 * 内部数据结构
 * =================================================================== */

/* 语言插件注册表项 */
typedef struct {
    VusLangPlugin *plugin;       /* 插件描述符指针 */
    void          *handle;       /* dlopen 句柄（NULL 表示手动注册）*/
    int            initialized;  /* 是否已初始化 */
} VusLangEntry;

/* 全局语言插件注册表 */
static VusLangEntry g_lang_plugins[VUS_MAX_LANG_PLUGINS];
static int g_lang_count = 0;
static char g_active_lang_name[64] = "";  /* 当前激活的语言插件 */

/* ===================================================================
 * 注册
 * =================================================================== */

int vus_lang_register(VusLangPlugin *plugin) {
    if (!plugin || !plugin->name || !plugin->preprocess) {
        fprintf(stderr, "vus_lang: invalid plugin descriptor (name or preprocess missing)\n");
        return -1;
    }

    if (g_lang_count >= VUS_MAX_LANG_PLUGINS) {
        fprintf(stderr, "vus_lang: max plugins reached (%d)\n", VUS_MAX_LANG_PLUGINS);
        return -1;
    }

    /* 检查名称冲突 */
    for (int i = 0; i < g_lang_count; i++) {
        if (strcmp(g_lang_plugins[i].plugin->name, plugin->name) == 0) {
            fprintf(stderr, "vus_lang: duplicate plugin name '%s'\n", plugin->name);
            return -2;
        }
    }

    g_lang_plugins[g_lang_count].plugin      = plugin;
    g_lang_plugins[g_lang_count].handle      = NULL;
    g_lang_plugins[g_lang_count].initialized = 0;
    g_lang_count++;

    return 0;
}

/* ===================================================================
 * 共享库加载
 * =================================================================== */

int vus_lang_load(const char *path) {
    if (!path) return -1;

    /* 打开共享库 */
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "vus_lang: failed to load '%s': %s\n", path, dlerror());
        return -1;
    }

    /* 查找入口符号 */
    typedef void (*entry_func_t)(VusLangPlugin **);
    entry_func_t entry = (entry_func_t)dlsym(handle, "vus_lang_entry");
    if (!entry) {
        fprintf(stderr, "vus_lang: symbol 'vus_lang_entry' not found in '%s': %s\n",
                path, dlerror());
        dlclose(handle);
        return -2;
    }

    /* 调用入口函数获取插件描述符 */
    VusLangPlugin *plugin = NULL;
    entry(&plugin);

    if (!plugin) {
        fprintf(stderr, "vus_lang: plugin entry returned NULL in '%s'\n", path);
        dlclose(handle);
        return -2;
    }

    /* 注册插件 */
    int ret = vus_lang_register(plugin);
    if (ret != 0) {
        dlclose(handle);
        return ret;
    }

    /* 将 handle 关联到注册表项 */
    for (int i = 0; i < g_lang_count; i++) {
        if (g_lang_plugins[i].plugin == plugin) {
            g_lang_plugins[i].handle = handle;
            break;
        }
    }

    return 0;
}

/* ===================================================================
 * 从配置加载
 * =================================================================== */

int vus_lang_load_from_config(const char *name, const char *project_dir) {
    if (!name || name[0] == '\0') return -1;

    /* 如果已加载同名的插件，直接返回成功 */
    VusLangPlugin *existing = vus_lang_find(name);
    if (existing) {
        strncpy(g_active_lang_name, name, sizeof(g_active_lang_name) - 1);
        g_active_lang_name[sizeof(g_active_lang_name) - 1] = '\0';
        return 0;
    }

    /* 标准查找路径：project_dir/plugins/lang/<名称>/<名称>.vulage */
    char path[2048];
    int n = snprintf(path, sizeof(path), "%s/plugins/lang/%s/%s.vulage",
                     project_dir, name, name);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        fprintf(stderr, "vus_lang: plugin path too long\n");
        return -1;
    }

    /* 尝试加载 */
    int ret = vus_lang_load(path);
    if (ret != 0) {
        /* 也尝试在编译器安装目录下查找 */
        const char *install_dir = getenv("VUS_HOME");
        if (install_dir) {
            n = snprintf(path, sizeof(path), "%s/plugins/lang/%s/%s.vulage",
                         install_dir, name, name);
            if (n >= 0 && (size_t)n < sizeof(path)) {
                ret = vus_lang_load(path);
            }
        }
    }

    if (ret == 0) {
        strncpy(g_active_lang_name, name, sizeof(g_active_lang_name) - 1);
        g_active_lang_name[sizeof(g_active_lang_name) - 1] = '\0';
    }

    return ret;
}

/* ===================================================================
 * 查询
 * =================================================================== */

VusLangPlugin *vus_lang_find(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < g_lang_count; i++) {
        if (strcmp(g_lang_plugins[i].plugin->name, name) == 0) {
            return g_lang_plugins[i].plugin;
        }
    }
    return NULL;
}

const char *vus_lang_active_name(void) {
    if (g_active_lang_name[0] == '\0') return NULL;
    return g_active_lang_name;
}

int vus_lang_count(void) {
    return g_lang_count;
}

void vus_lang_list_all(void) {
    if (g_lang_count == 0) {
        printf("No language plugins registered.\n");
        return;
    }

    printf("Registered language plugins (%d):\n", g_lang_count);
    printf("  %-24s %-10s %-8s %s\n", "Name", "Version", "AST Ver", "Description");
    printf("  %-24s %-10s %-8s %s\n", "----", "-------", "--------", "-----------");
    for (int i = 0; i < g_lang_count; i++) {
        VusLangPlugin *p = g_lang_plugins[i].plugin;
        const char *ast_ver = p->ast_version ? p->ast_version : "?";
        const char *desc = p->description ? p->description : "";
        printf("  %-24s %-10s %-8s %s\n",
               p->name,
               p->version ? p->version : "",
               ast_ver,
               desc);
    }
}

/* ===================================================================
 * 预处理
 * =================================================================== */

int vus_lang_preprocess(const char *name, const char *input, char **output) {
    if (!input) {
        *output = NULL;
        return -1;
    }

    /* 如果未指定语言插件名称，直接原样输出 */
    if (!name || name[0] == '\0') {
        *output = strdup(input);
        return *output ? 0 : -1;
    }

    VusLangPlugin *plugin = vus_lang_find(name);
    if (!plugin) {
        /* 未找到插件，原样输出并警告 */
        *output = strdup(input);
        return *output ? 0 : -1;
    }

    /* 调用预处理回调 */
    return plugin->preprocess(input, output);
}

/* ===================================================================
 * 生命周期管理
 * =================================================================== */

int vus_lang_init_all(void) {
    int success_count = 0;
    for (int i = 0; i < g_lang_count; i++) {
        if (g_lang_plugins[i].initialized) continue;
        if (g_lang_plugins[i].plugin->init) {
            if (g_lang_plugins[i].plugin->init() == 0) {
                g_lang_plugins[i].initialized = 1;
                success_count++;
            } else {
                fprintf(stderr, "vus_lang: init failed for '%s'\n",
                        g_lang_plugins[i].plugin->name);
            }
        } else {
            g_lang_plugins[i].initialized = 1;
            success_count++;
        }
    }
    return success_count;
}

void vus_lang_cleanup_all(void) {
    for (int i = g_lang_count - 1; i >= 0; i--) {
        if (!g_lang_plugins[i].initialized) continue;
        if (g_lang_plugins[i].plugin->cleanup) {
            g_lang_plugins[i].plugin->cleanup();
        }
        g_lang_plugins[i].initialized = 0;
    }
}

void vus_lang_unload_all(void) {
    vus_lang_cleanup_all();

    for (int i = g_lang_count - 1; i >= 0; i--) {
        if (g_lang_plugins[i].handle) {
            dlclose(g_lang_plugins[i].handle);
        }
        g_lang_plugins[i].plugin      = NULL;
        g_lang_plugins[i].handle      = NULL;
        g_lang_plugins[i].initialized = 0;
    }
    g_lang_count = 0;
    g_active_lang_name[0] = '\0';
}