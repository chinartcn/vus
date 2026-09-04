/*
 * gen_builtin.h — VUS 内置函数映射模块接口
 *
 * 将 generator.c 中巨型的 gen_expr_call 函数按功能类别拆分为独立模块，
 * 每个模块处理一类内置函数的 C 代码生成。
 *
 * 使用方式：gen_expr_call 依次调用各模块的 handler，若 handler 返回非 NULL
 * 则直接使用该结果；否则继续尝试下一个模块，均不匹配则回退到普通函数调用。
 */

#ifndef VUS_GEN_BUILTIN_H
#define VUS_GEN_BUILTIN_H

#include "generator.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============ 需跨模块访问的全局变量 ============ */

/* GuiLite 图形库使用标记 */
extern int g_uses_gui;
extern int g_uses_png;
extern int g_uses_freetype;

/* VUA（Android 组件流）界面内建相关 */
extern int     g_uses_vua;
extern GenBuf *g_vua_premain;
extern GenBuf *g_vua_fwd;
extern int     g_vua_bind_count;

/* ============ 内置函数 Handler 声明 ============ */

/* 每个 handler 检查 call->func_name 是否属于本模块的范畴。
 * 若匹配，生成 C 代码并返回 malloc 分配的字符串；若不匹配，返回 NULL。
 * 调用方负责 free 返回值。 */

/* 基础 IO + 日志：打印/输入/转数字/转文本/睡眠/日志_xx */
char *gen_builtin_io(GenBuf *buf, VusAstCall *call);

/* 图形 + 传感器 + 音频 + TUI：图形_xx/传感器_xx/音频_xx/tui_xx */
char *gen_builtin_gui(GenBuf *buf, VusAstCall *call);

/* 网络 + 文件：网络_xx/文件_xx/命令_执行/文本_分割 */
char *gen_builtin_net_file(GenBuf *buf, VusAstCall *call);

/* 插件 + JSON：插件_xx/JSON_xx/对象文本/typeof/字典_键 */
char *gen_builtin_plugin(GenBuf *buf, VusAstCall *call);

/* 日期 + Termux：日期_xx/Termux_xx */
char *gen_builtin_date(GenBuf *buf, VusAstCall *call);

/* 数据结构：文本_xx/列表_xx/字典_xx */
char *gen_builtin_data(GenBuf *buf, VusAstCall *call);

#ifdef __cplusplus
}
#endif

#endif /* VUS_GEN_BUILTIN_H */