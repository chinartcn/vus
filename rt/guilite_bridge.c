/*
 * VUS 集成 GuiLite —— C 桥接层实现
 *
 * 实现 图形_* 内建函数对应的 vus_gui_* API：
 *  - 持有基础静态状态（是否已初始化）
 *  - 绘制调用转交 guilite_wrapper.cpp（C++ GuiLite 包装）
 *  - 显示调用转交 guilite_platform.c（X11 / headless）
 *
 * 颜色约定：VUS 传入 0xRRGGBB，转换成 GuiLite 的 ARGB8888 后交给包装层。
 */

#include "guilite_bridge.h"

static int s_initialized = 0;

/* 把 0xRRGGBB 转为 0xAARRGGBB（不透明） */
static unsigned int argb_from_rgb(unsigned int rgb)
{
    return 0xFF000000u | (rgb & 0x00FFFFFFu);
}

VusString* vus_gui_init(int width, int height, const char* title)
{
    if (vus_gui_surface_init(width, height) != 0)
    {
        s_initialized = 0;
        return vus_string_new("0");
    }
    vus_gui_platform_init(width, height, title);
    s_initialized = 1;
    return vus_string_new("1");
}

VusString* vus_gui_draw_pixel(int x, int y, unsigned int color)
{
    if (!s_initialized) { return vus_string_new("0"); }
    vus_gui_surface_draw_pixel(x, y, argb_from_rgb(color));
    return vus_string_new("1");
}

VusString* vus_gui_draw_line(int x1, int y1, int x2, int y2, unsigned int color)
{
    if (!s_initialized) { return vus_string_new("0"); }
    vus_gui_surface_draw_line(x1, y1, x2, y2, argb_from_rgb(color));
    return vus_string_new("1");
}

VusString* vus_gui_draw_rect(int x, int y, int width, int height, unsigned int color)
{
    if (!s_initialized) { return vus_string_new("0"); }
    vus_gui_surface_draw_rect(x, y, width, height, argb_from_rgb(color));
    return vus_string_new("1");
}

VusString* vus_gui_fill_rect(int x, int y, int width, int height, unsigned int color)
{
    if (!s_initialized) { return vus_string_new("0"); }
    vus_gui_surface_fill_rect(x, y, width, height, argb_from_rgb(color));
    return vus_string_new("1");
}

VusString* vus_gui_draw_text(int x, int y, const char* text, unsigned int color)
{
    if (!s_initialized) { return vus_string_new("0"); }
    vus_gui_surface_draw_text(x, y, text, argb_from_rgb(color));
    return vus_string_new("1");
}

VusString* vus_gui_redraw(void)
{
    if (!s_initialized)
    {
        return vus_string_new("0");
    }
    vus_gui_platform_redraw(vus_gui_surface_width(), vus_gui_surface_height(),
                            vus_gui_surface_framebuffer());
    return vus_string_new("1");
}

VusString* vus_gui_run(void)
{
    if (!s_initialized)
    {
        return vus_string_new("0");
    }
    vus_gui_platform_run(vus_gui_surface_width(), vus_gui_surface_height(),
                         vus_gui_surface_framebuffer());
    return vus_string_new("1");
}