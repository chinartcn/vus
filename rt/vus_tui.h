/*
 * vus_tui.h — VUS 终端 UI 画布子系统（帧缓冲 + 差分刷新 + 输入）
 *
 * 相对旧 vus_plugin_tui_*（流式 ANSI 指令）的升级：
 *   1. 画布模型：tui_绘制文本/边框 写入帧缓冲，tui_刷新 差分上屏——
 *      只输出与上次上屏不同的单元格，复杂界面不再整屏闪烁；
 *   2. 输入能力：tui_读取按键（非阻塞）/ tui_输入文本（行编辑）；
 *   3. 终端交互：终端尺寸查询、光标显隐、清行；
 *   4. 无终端环境（管道/CI/APK）：绘制静默缓冲，读取类返回空串，不阻塞不崩溃。
 *
 * 线程契约：与 libvus_rt 一致（进程级共享画布，单线程使用）。
 */
#ifndef VUS_TUI_H
#define VUS_TUI_H

#include "libvus_rt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 画布硬上限（超界单元格静默丢弃） */
#define VUS_TUI_MAX_ROWS 128
#define VUS_TUI_MAX_COLS 256

/* ---- 画布（帧缓冲） ---- */
/* 重设画布尺寸（rows/cols 为 0 表示保留当前值；超上限钳制）。返回 0。 */
int  vus_tui_canvas_resize(int rows, int cols);
/* 清空当前帧为空格（默认色）。 */
void vus_tui_canvas_clear(void);
/* 在 (row,col) 写入 UTF-8 文本（逐显示列放置）；fg/bg ∈ -1(默认)..255，style 位组合：
 * bit0=加粗 bit1=下划线 bit2=反显。超界丢弃。 */
void vus_tui_canvas_put(int row, int col, const char *utf8, int fg, int bg, int style);
/* 画横线：从 (row,col) 起 len 格填 ch。 */
void vus_tui_canvas_line(int row, int col, int len, char ch);
/* 画边框窗口：左上 (row,col)，高 h 宽 w（含框线），标题居中于顶线（UTF-8）。 */
void vus_tui_canvas_box(int row, int col, int h, int w, const char *title);
/* 差分上屏：对比显示缓冲，仅输出变化单元格（含颜色/样式切换优化）。
 * 首帧全量输出；无终端时仅记录缓冲不输出。 */
void vus_tui_flush(void);

/* ---- 终端交互 ---- */
int  vus_tui_rows(void);                /* 终端行数（ioctl；无终端→24） */
int  vus_tui_cols(void);                /* 终端列数（ioctl；无终端→80） */
void vus_tui_cursor(int show);          /* 1 显示光标 0 隐藏 */
void vus_tui_clear_line(int row);       /* 清指定行（画布 + 终端） */

/* ---- 输入（无终端/管道 → 返回空串，不阻塞） ---- */
VusString *vus_tui_read_key(void);      /* 非阻塞读键：返回 UTF-8 序列（含 ESC 前缀组合键） */
VusString *vus_tui_input(const char *prompt, int maxlen);  /* 行编辑：提示 + 逐字符回显 + 退格 + 回车 */

/* ---- 参数钳制（旧流式函数共用） ---- */
void vus_tui_color_clamp(int *fg, int *bg);   /* -1(默认) 或 0..255，越界钳制 */

/* ---- 流式样式（tui_文本样式 立即生效，与画布 style 位一致） ----
 * style 位：bit0=加粗(1) bit1=下划线(2) bit2=反显(4) bit3=闪烁(8)；0=复位。 */
void vus_tui_style(int style);

#ifdef __cplusplus
}
#endif

#endif /* VUS_TUI_H */
