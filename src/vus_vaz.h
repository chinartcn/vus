/* =============================================================================
 * vus_vaz.h — VUS Android 扩展包（.vaz）处理接口
 * =============================================================================
 */
#ifndef VUS_VAZ_H
#define VUS_VAZ_H

#include <stddef.h>

/* 处理 .vaz 扩展包：加载控件模板并展开 pages_dir 下所有 *.vua 页面，
 * 合并包内逻辑库到 out_logic（可为 NULL）。
 * 返回 0 成功；非 0 失败（错误信息写入 err）。
 */
int vus_vaz_expand(const char *vaz_path, const char *pages_dir,
                   const char *out_logic, char *err, size_t errsz);

/* 按包名查找 .vaz 依赖包并返回其逻辑库源码（合并拼接，malloc 缓冲）。
 * modname：导入的包名（如 通用控件包 / 通用控件包.vaz）。
 * base_dir：主脚本所在目录（相对项目内 deps/vaz 查找）。
 * 成功返回 0，out_src 为合并源码（调用者 free），out_libdir 为包目录
 * （zip 形式会解压缓存到 base_dir/.vus/vaz-cache/<名> 供递归导入复用）。
 * 找不到返回 1（不写错误，由调用方保留 import 原行）；出错返回 -1。
 */
int vus_vaz_import(const char *modname, const char *base_dir,
                   char **out_src, size_t *out_len,
                   char *out_libdir, size_t libdir_sz);

#endif /* VUS_VAZ_H */