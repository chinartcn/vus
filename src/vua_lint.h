/*
 * vua_lint.h — .vua 离线校验（vus lint）与 LSP 校验闭环共用入口
 *
 * 复用 rt/vua.c 的严格校验（控件表 type 校验 + 字段词典校验）与渲染树归一
 * （vua_screen_load → vua_screen_dump_rendertree），见 tests/vua_smoke.c 的
 * 同套行为；此处以 CLI 命令 + 进程级控件表/词典加载封装之。
 */

#ifndef VUS_VUA_LINT_H
#define VUS_VUA_LINT_H

#include <stdio.h>
#include "vua.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 进程级一次（幂等）：加载默认控件表/词典，供 CLI 与 LSP 共用。
 * 控件表探测顺序：VUS_VUA_CONTROLS 环境变量 → <hint_dir>/vua_controls.json
 * → <hint_dir>/testdata/vua_controls.json → ./vua_controls.json
 * → ./testdata/vua_controls.json。词典由 VUS_VUA_DICT 指定，缺省不加载。
 * 返回 0=已加载控件表；-1=未找到任何控件表（后续仅做基础 JSON/结构校验）。
 * info 输出加载摘要（可 NULL）；info_sz 为 info 容量。
 */
int vua_lint_ensure_catalog(const char *hint_dir, char *info, size_t info_sz);

/*
 * 校验一段 .vua JSON 文本（严格校验 + 渲染树归一）。
 * 返回 0=通过（hash_out 可输出渲染树 FNV-1a 指纹）；-1=失败（首个错误挂到 err）。
 * display 仅用于错误命名（如文件名），可 NULL。
 */
int vua_lint_text(const char *vua_json, const char *display, VuaError *err,
                  unsigned long long *hash_out);

/* 打印一条 VuaError（含错误链），前缀加 display。 */
void vua_lint_print_error(FILE *f, const char *display, const VuaError *err);

/* CLI 入口：vus lint [--controls <控件表.json>] [--dict <词典.json>] <file.vua>... */
int vus_lint_cmd(int argn, char **args);

#ifdef __cplusplus
}
#endif

#endif /* VUS_VUA_LINT_H */