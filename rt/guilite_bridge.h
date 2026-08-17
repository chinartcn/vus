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
/* 平台层文字绘制：X11 可用且加载了 X 字体时用 XDrawString 叠加入队，返回 1；
 * 否则返回 0，由桥接层回退到 GuiLite 帧缓冲绘制。 */
int  vus_gui_platform_draw_text(int x, int y, const char* text, unsigned int color);
/* 平台层非阻塞取事件：处理当前 X 事件队列（含点击/重绘/退出），供轮询式交互。 */
void vus_gui_platform_poll(int width, int height, const unsigned int* fb);

/* 点击事件派发：平台层（X11 事件循环）捕获鼠标点击后回调本接口，
 * 本接口负责按约定函数名反查 VUS 回调（事件_点击）并调用（见 guilite_bridge.c）。
 * x / y 为窗口内坐标（像素，左上为原点）。 */
void vus_gui_platform_emit_click(int x, int y);

/* ============ 阶段2：控件与轮询交互 API ============ */

/* 创建并绘制一个按钮，登记命中矩形（供 图形_按钮点击 命中检测）。
 * name：控件唯一名；x/y：左上角；w/h：宽高；text：按钮文本。
 * 返回 VusString*："1" 成功 / "0" 失败。 */
VusString* vus_gui_button(const char* name, int x, int y, int w, int h, const char* text);

/* 非阻塞处理 X 事件队列（轮询式交互模型的核心），更新最近点击坐标。
 * 返回 VusString*："1" 成功 / "0" 失败。 */
VusString* vus_gui_poll(void);

/* 命中检测：最近一次点击是否落在名为 name 的按钮矩形内。
 * 返回 VusString*："true" / "false"，可直接用作 如果 条件。 */
VusString* vus_gui_button_clicked(const char* name);

/* 模拟一次点击（x/y 为窗口内坐标），等价于平台层收到一次鼠标按下。
 * 用于 headless 自动化测试注入点击，验证命中检测与 如果 条件分支。
 * 返回 VusString*："1" 成功 / "0" 失败。 */
VusString* vus_gui_sim_click(int x, int y);

/* ============ 阶段3：控件库 ============
 * 统一控件表：按钮/标签/文本框/复选框/进度条/列表/画布 共享命中检测。
 * 文本绘制优先 X11（方向正常），失败回退 GuiLite 帧缓冲。 */

/* 标签：绘制一行文本，登记为可命中控件（估宽矩形）。
 * 返回 VusString*："1" 成功 / "0" 失败。 */
VusString* vus_gui_label(const char* name, int x, int y, const char* text, unsigned int color);

/* 文本框：白底 + 边框 + 文本，登记矩形。
 * 返回 VusString*："1" 成功 / "0" 失败。 */
VusString* vus_gui_textbox(const char* name, int x, int y, int w, int h, const char* text);

/* 复选框：方格 + 勾选标记 + 文本。点击（未消费）切换勾选状态。
 * 返回 VusString*：切换后状态 "true"/"false"，可作 如果 条件。 */
VusString* vus_gui_checkbox(const char* name, int x, int y, const char* text, int checked);

/* 进度条：填充底色 + 按 value(0-100) 画比例的进度 + 边框。
 * 返回 VusString*："1" 成功 / "0" 失败。 */
VusString* vus_gui_progress(const char* name, int x, int y, int w, int h, int value);

/* 列表：声明列表区域（void），返回 "1" 成功。rows_h 为每行像素高。 */
VusString* vus_gui_list(const char* name, int x, int y, int w, int h, int row_h);

/* 列表行：在第 line 行（0 起）写入并绘制文本（选中行高亮）。
 * 返回 VusString*："1" 成功 / "0" 失败（越界/未创建列表）。 */
VusString* vus_gui_list_row(const char* name, int line, const char* text);

/* 列表选中行：最近一次点击命中的行索引，未命中/无列表返回 "-1"。
 * 返回 VusString*：整数字符串，脚本用 vus_to_int 或与数字比较。 */
VusString* vus_gui_list_selected(const char* name);

/* 列表行命中：最近一次点击是否落在 name 列表的第 line 行内。
 * 返回 VusString*："true"/"false"。 */
VusString* vus_gui_list_row_clicked(const char* name, int line);

/* 画布：声明一个可命中区域，脚本自行在其内绘制。可选描边。
 * 返回 VusString*："1" 成功 / "0" 失败。 */
VusString* vus_gui_canvas(const char* name, int x, int y, int w, int h);

/* 画布命中：最近一次点击是否落在 name 画布内（仅当相对坐标在范围内时）。
 * 返回 VusString*："true" 命中 / "false" 未命中，可作 如果 条件。 */
VusString* vus_gui_canvas_hit(const char* name);

/* 画布相对坐标：最近一次画布命中的相对位置，返回 "x,y"；未命中返回 "-1,-1"。 */
VusString* vus_gui_canvas_pos(const char* name);

#ifdef __cplusplus
}
#endif

#endif /* VUS_GUILITE_BRIDGE_H */