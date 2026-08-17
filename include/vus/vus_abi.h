/*
 * vus_abi.h — VUS 编译器 C ABI 接口
 *
 * 提供稳定的 C 语言 ABI（Application Binary Interface），
 * 供外部程序（C/C++、Python、Ruby 等）嵌入 VUS 编译器。
 * 所有函数均以 extern "C" 导出，C++ 编译时自动兼容。
 *
 * === 使用示例 (C) ===
 *   VusConfig config;
 *   memset(&config, 0, sizeof(config));
 *   strcpy(config.style, "函数");
 *   strcpy(config.project_dir, ".");
 *
 *   VusResult res = vus_compile_file("script.vus", &config);
 *   if (res.success) printf("生成: %s\n", res.c_output_path);
 *
 * === 使用示例 (Python ctypes) ===
 *   lib = ctypes.CDLL("./libvus.so")
 *   lib.vus_compile_file.argtypes = [c_char_p, POINTER(VusConfig)]
 *   lib.vus_compile_file.restype = VusResult
 */

#ifndef VUS_VUS_ABI_H
#define VUS_VUS_ABI_H

#include "vus.h"      /* VusResult, VusConfig */

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================
 * ABI 版本信息
 * ===================================================================
 * 主版本号 — 不兼容的 ABI 变更
 * 次版本号 — 向后兼容的新增功能
 * 补丁版本 — 向后兼容的缺陷修复
 */
#define VUS_ABI_VERSION_MAJOR 1
#define VUS_ABI_VERSION_MINOR 0
#define VUS_ABI_VERSION_PATCH 0

/* 获取 ABI 版本号（编码为 0xMMmmpp）*/
int vus_abi_version(void);

/* 获取 ABI 版本字符串（如 "1.0.0"）*/
const char *vus_abi_version_string(void);

/* ===================================================================
 * 文件编译
 * ===================================================================
 * 编译单个 .vus 文件，生成 C 代码。
 *
 * @param path    .vus 源文件路径
 * @param config  编译配置（可为 NULL，此时使用默认配置）
 * @return        VusResult（success==1 表示成功）
 *
 * 成功时 result.c_output_path 为生成的 C 文件路径。
 * 失败时 result.error_msg 包含错误描述。
 */
VusResult vus_compile_file(const char *path, VusConfig *config);

/* ===================================================================
 * 字符串编译
 * ===================================================================
 * 从内存中的 VUS 源码字符串编译，生成 C 代码。
 * 不涉及文件系统 I/O（除最终写入 C 文件外）。
 *
 * @param source   VUS 源码字符串（以 '\0' 结尾）
 * @param config   编译配置（可为 NULL）
 * @return         VusResult
 *
 * 写入的 C 文件路径由 config->build_dir 或 config->project_dir 决定，
 * 文件名取 source 的前 16 个字符 sanitize 后作为名称。
 */
VusResult vus_compile_string(const char *source, VusConfig *config);

/* ===================================================================
 * 字符串编译 + 链接
 * ===================================================================
 * 从内存中的 VUS 源码编译并链接为可执行文件。
 * 相当于 vus_compile_string() + GCC 链接。
 *
 * @param source   VUS 源码
 * @param config   编译配置
 * @return         VusResult（exe_output_path 为可执行文件路径）
 */
VusResult vus_compile_string_to_exe(const char *source, VusConfig *config);

/* ===================================================================
 * 表达式求值
 * ===================================================================
 * 编译并执行 VUS 代码片段，返回求值结果字符串。
 * 实现方式：将代码包装为完整程序 → 编译 → 运行 → 捕获 stdout。
 *
 * 注意：
 * - 每次调用都会启动一个子进程，开销较大。
 * - 输出截断为 4096 字节以内。
 * - result.success 表示编译和执行均成功。
 *
 * @param code     VUS 代码片段（如 "1 + 2 * 3"）
 * @param config   编译配置
 * @param output   输出缓冲区（由调用方分配，至少 4096 字节），
 *                 填写程序执行的标准输出内容。
 * @return         VusResult（success==1 表示成功）
 */
VusResult vus_eval(const char *code, VusConfig *config, char *output);

/* ===================================================================
 * import 源码展开
 * ===================================================================
 * 把源码中的「导入 X / 从 X 导入」行就地替换为对应 X.vus 的源码
 * （递归展开其自身的 import），使外部模块的全部函数随主程序一起编译，
 * 从而可被 dlsym 反查，支撑「外部 .vus 页面」等多文件场景。
 *
 * 返回 malloc 分配的展开后源码；失败返回 NULL（此时应沿用原 source）。
 * 调用方负责 free 返回值。
 *
 * @param source       VUS 源码（'\0' 结尾）
 * @param source_name  主脚本路径（用于解析同目录模块），可为 NULL
 * @return             展开后源码，或 NULL
 */
char *vus_source_expand_imports(const char *source, const char *source_name);

#ifdef __cplusplus
}
#endif

#endif /* VUS_VUS_ABI_H */