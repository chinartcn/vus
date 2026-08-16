#ifndef VUS_GUILITE_BRIDGE_H
#define VUS_GUILITE_BRIDGE_H

/*
 * VUS 集成 GuiLite 绘图基础层 —— C 桥接层头文件
 *
 * 本文件对 VUS 语言编译器生成的 C 代码暴露 图形_* 内建函数的 C 实现。
 * 桥接层采用纯 C API，返回 VusString*（"1" 成功 / "0" 失败），
 * 与 VUS 运行时库其它内建函数（如 日志_*）的约定保持一致。
 *
 * 颜色统一使用 0xRRGGBB 整数（红/绿/蓝各 8 bit）。
 */

#include "libvus_rt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化图形环境：创建 GuiLite surface 与显示后端（X11 或 headless）。
 * 返回 VusString*："1" 成功 / "0" 失败。 */
VusString* vus_gui_init(int width, int height, const char* title);

/* 基础绘图内建函数（颜色 color 为 0xRRGGBB） */
VusString* vus_gui_draw_pixel(int x, int y, unsigned int color);
VusString* vus_gui_draw_line(int x1, int y1, int x2, int y2, unsigned int color);
VusString* vus_gui_draw_rect(int x, int y, int width, int height, unsigned int color);
VusString* vus_gui_fill_rect(int x, int y, int width, int height, unsigned int color);
VusString* vus_gui_draw_text(int x, int y, const char* text, unsigned int color);

/* 刷新：把帧缓冲送到显示后端（X11 送窗 / headless 导出 PPM）。 */
VusString* vus_gui_redraw(void);

/* 保持：进入显示后端事件循环，保持窗口存活。 */
VusString* vus_gui_run(void);

/* 内部接口：供桥接实现与平台层使用的 C++ 包装（在 guilite_wrapper.cpp 中实现） */
int vus_gui_surface_init(int width, int height);          /* 0 成功 / -1 失败 */
void vus_gui_surface_free(void);
void vus_gui_surface_draw_pixel(int x, int y, unsigned int argb);
void vus_gui_surface_draw_line(int x1, int y1, int x2, int y2, unsigned int argb);
void vus_gui_surface_draw_rect(int x, int y, int width, int height, unsigned int argb);
void vus_gui_surface_fill_rect(int x, int y, int width, int height, unsigned int argb);
void vus_gui_surface_draw_text(int x, int y, const char* text, unsigned int argb);
unsigned int* vus_gui_surface_framebuffer(void);           /* 返回 ARGB8888 帧缓冲指针 */
int vus_gui_surface_width(void);
int vus_gui_surface_height(void);

/* 平台层接口（guilite_platform.c 实现） */
int  vus_gui_platform_init(int width, int height, const char* title);
void vus_gui_platform_redraw(int width, int height, const unsigned int* fb);
void vus_gui_platform_run(int width, int height, const unsigned int* fb);

#ifdef __cplusplus
}
#endif

#endif /* VUS_GUILITE_BRIDGE_H */