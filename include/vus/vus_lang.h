/*
 * vus_lang.h — VUS 语言插件（语法风格插件）接口
 *
 * 语言插件（.vulage）负责在编译前端将不同风格的源代码解析为
 * 统一的标准 AST。核心编译器定义标准 AST 结构，所有语言插件
 * 最终都输出相同的 AST，从而确保：
 *   1. 核心编译器不需要预知所有可能的语法风格
 *   2. 功能插件（.vux）只需处理 AST，不关心源代码风格
 *   3. 用户可以自由选择语法风格，不影响功能插件的使用
 *
 * === 加载时机 ===
 * 语言插件在编译流水线的词法分析阶段之前加载：
 *   加载 .vulage → 源码预处理 → 词法分析 → 语法分析 → AST → 代码生成
 *
 * === 与 .vux 功能插件的区别 ===
 *   .vulage（语言插件）  |  .vux（功能插件）
 *   ---------------------|-------------------
 *   编译前端生效          |  运行时生效
 *   解析前加载            |  解析后加载
 *   影响"怎么读代码"     |  影响"怎么运行代码"
 *   项目级锁定            |  可按需加载
 *
 * === 插件编写示例 ===
 *   #include <vus/vus_lang.h>
 *
 *   static int my_preprocess(const char *input, char **output) {
 *       *output = strdup(input);  // 示例：不做转换
 *       return 0;
 *   }
 *
 *   VusLangPlugin g_lang = {
 *       .name       = "my_lang",
 *       .version    = "1.0.0",
 *       .ast_version= "1.0",
 *       .preprocess = my_preprocess,
 *       .init       = NULL,
 *       .cleanup    = NULL
 *   };
 *
 *   VUS_LANG_EXPORT void vus_lang_entry(VusLangPlugin **plugin) {
 *       *plugin = &g_lang;
 *   }
 *
 * === 编译为 .vulage ===
 *   gcc -shared -fPIC -o my_lang.vulage my_lang.c -I/path/to/vus/include
 *
 * === 在 vus.json 中配置 ===
 *   {
 *       "语言插件": "易语言",
 *       ...
 *   }
 */

#ifndef VUS_VUS_LANG_H
#define VUS_VUS_LANG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================
 * 插件对外导出宏
 * ===================================================================
 * 语言插件 .vulage 中必须且仅定义一个 VUS_LANG_EXPORT 符号。
 * 编译器在加载 .vulage 时通过 dlsym("vus_lang_entry") 定位入口。
 */
#if defined(_WIN32) || defined(_WIN64)
#  define VUS_LANG_EXPORT __declspec(dllexport)
#else
#  define VUS_LANG_EXPORT __attribute__((visibility("default")))
#endif

/* ===================================================================
 * VusLangPlugin — 语言插件描述符
 * ===================================================================
 * 每个语言插件必须定义此结构体并通过 vus_lang_entry 导出。
 * 语言插件是编译前端的预处理插件，将特定风格的源代码
 * 转换为标准 VUS 函数风格语法，然后由核心编译器继续处理。
 */
typedef struct VusLangPlugin {
    /* 插件名称（唯一标识符，如 "易语言"、"函数风格"）*/
    const char *name;

    /* 插件版本号（如 "1.0.0"）*/
    const char *version;

    /* 兼容的 AST 版本号（如 "1.0"）*/
    const char *ast_version;

    /* ── 核心回调 ── */

    /* 预处理：将输入源码转换为标准 VUS 函数风格语法。
     * input   — 原始源代码
     * output  — 转换后的代码（由插件分配，调用方负责 free）
     * 返回 0 表示成功，非 0 表示转换失败。
     * 如果插件不需要预处理（如本身就是标准函数风格），
     * 可直接 *output = strdup(input) 原样返回。 */
    int (*preprocess)(const char *input, char **output);

    /* ── 生命周期回调（可选，可为 NULL）── */

    /* 初始化：插件加载后、使用前调用。
     * 返回 0 表示成功，非 0 表示初始化失败。 */
    int  (*init)(void);

    /* 清理：插件卸载前调用，释放资源。 */
    void (*cleanup)(void);

    /* ── 可选字段 ── */

    const char *description;  /* 插件描述 */
    const char *author;       /* 作者 */
} VusLangPlugin;

/* ===================================================================
 * 语言插件注册和管理
 * =================================================================== */

/* 最大注册语言插件数量 */
#define VUS_MAX_LANG_PLUGINS 16

/* 注册一个语言插件。返回 0 成功，-1 插件已满，-2 名称冲突。 */
int vus_lang_register(VusLangPlugin *plugin);

/* 从 .vulage 文件加载语言插件。
 * path — .vulage 文件路径。
 * 返回 0 成功，-1 无法打开，-2 未找到入口符号。 */
int vus_lang_load(const char *path);

/* 按名称查找已注册语言插件。返回插件指针，未找到时返回 NULL。 */
VusLangPlugin *vus_lang_find(const char *name);

/* 对输入源码应用指定语言插件的预处理。
 * 如果 name 为 NULL 或未找到，直接 strdup 原样返回。
 * 返回 0 成功，非 0 失败。 */
int vus_lang_preprocess(const char *name, const char *input, char **output);

/* 获取已注册语言插件数量。 */
int vus_lang_count(void);

/* 列出所有已注册语言插件（打印到 stdout）。 */
void vus_lang_list_all(void);

/* 初始化所有已注册语言插件。 */
int vus_lang_init_all(void);

/* 清理所有已注册语言插件。 */
void vus_lang_cleanup_all(void);

/* 卸载所有语言插件。 */
void vus_lang_unload_all(void);

#ifdef __cplusplus
}
#endif

#endif /* VUS_VUS_LANG_H */