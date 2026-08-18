/*
 * VUS 集成 GuiLite —— C++ 包装层
 *
 * GuiLite 是 C++ 单头文件库（GuiLite.h）。VUS 的纯 C 桥接层（guilite_bridge.c）
 * 无法直接调用 C++ 的 c_surface/c_display，因此本文件以 extern "C" 暴露一组
 * 精简的绘制接口，内部持有 GuiLite 的 display/surface/framebuffer 全局状态。
 *
 * 颜色约定：所有 *_argb 参数为 ARGB8888（0xAARRGGBB），由调用方（桥接层）把
 * VUS 的 0xRRGGBB 转换为 ARGB。
 *
 * 像素格式：GuiLite 默认 ARGB8888（color_bytes=4），写入物理帧缓冲 m_phy_fb。
 */

/* GUI_LITE_ON：在唯一包含 GuiLite.h 的编译单元中启用全局定义
 * （_assert / log_out / c_theme 静态成员等），否则链接期会出现未定义引用。 */
#define GUILITE_ON

#include "guilite/GuiLite.h"
#include "guilite_bridge.h"

#include <stdlib.h>
#include <string.h>

/* 集成的 8x8 位图字体（Public Domain，见 font8x8_basic.h） */
#include "guilite/font8x8_basic.h"

/* ============ 内部全局状态（双缓冲） ============
 * s_back：后台缓冲（绘制目标）。所有 VUS 绘制调用写入这里。
 * s_fb  ：前台缓冲（显示目标）。vus_gui_surface_present() 把后台一次性
 *          memcpy 到前台，platform_redraw 再把前台送上屏/导出 PPM。
 * 这样整帧绘制完成后再原子上屏，避免绘制过程的部分像素闪现（撕裂/闪烁）。 */
static unsigned int* s_fb = 0;      /* ARGB8888 前台物理帧缓冲（显示） */
static unsigned int* s_back = 0;    /* ARGB8888 后台帧缓冲（绘制） */
static c_display*    s_display = 0; /* GuiLite display（拥有 surface） */
static c_surface*    s_surface = 0; /* 绘制 surface（绑定后台缓冲） */
static c_lattice_font_op s_font_op; /* 字体绘制算子 */
static int           s_width = 0;
static int           s_height = 0;

/* ============ 字体构建（8x8 位图 -> GuiLite 游程编码 LATTICE） ============ */

/* GuiLite 的 draw_lattice 使用游程编码（RLE）：像素缓冲为连续的
 * [灰度值, 连续个数] 块，覆盖 width*height 个像素。
 * 这里把 font8x8_basic 的每字符 8x8 位图编码为 RLE。 */
#define FONT_WIDTH  8
#define FONT_HEIGHT 8
#define FONT_FIRST  0x20
#define FONT_LAST   0x7E
#define FONT_COUNT  (FONT_LAST - FONT_FIRST + 1)

static unsigned char s_rle[FONT_COUNT][FONT_WIDTH * FONT_HEIGHT * 2];
static LATTICE       s_lattice[FONT_COUNT];
static LATTICE_FONT_INFO s_font_info;

static void build_font(void)
{
    for (int ci = 0; ci < FONT_COUNT; ci++)
    {
        int code = FONT_FIRST + ci;
        const unsigned char* glyph = font8x8_basic[code];
        unsigned char* out = s_rle[ci];
        int o = 0;
        unsigned char cur = 0;
        int run = 0;
        /* 逐行扫描；font8x8_basic 中 bit7 为最左侧像素 */
        for (int y = 0; y < FONT_HEIGHT; y++)
        {
            unsigned char row = (unsigned char)glyph[y];
            for (int x = 0; x < FONT_WIDTH; x++)
            {
                unsigned char v = ((row & (1u << (7 - x))) != 0) ? 255 : 0;
                if (run > 0 && v == cur)
                {
                    run++;
                }
                else
                {
                    if (run > 0) { out[o++] = cur; out[o++] = (unsigned char)run; }
                    cur = (unsigned char)v;
                    run = 1;
                }
            }
        }
        if (run > 0) { out[o++] = cur; out[o++] = (unsigned char)run; }

        s_lattice[ci].utf8_code = (unsigned int)code;
        s_lattice[ci].width = FONT_WIDTH;
        s_lattice[ci].pixel_buffer = s_rle[ci];
    }
    s_font_info.height = FONT_HEIGHT;
    s_font_info.count = FONT_COUNT;
    s_font_info.lattice_array = s_lattice;
}

/* ============ extern "C" 接口 ============ */

extern "C" {

int vus_gui_surface_init(int width, int height)
{
    if (width <= 0 || height <= 0)
    {
        return -1;
    }
    vus_gui_surface_free();
    build_font();

    s_width = width;
    s_height = height;
    s_fb = (unsigned int*)calloc((size_t)width * (size_t)height, sizeof(unsigned int));
    s_back = (unsigned int*)calloc((size_t)width * (size_t)height, sizeof(unsigned int));
    if (!s_fb || !s_back)
    {
        if (s_fb) { free(s_fb); s_fb = 0; }
        if (s_back) { free(s_back); s_back = 0; }
        return -1;
    }
    /* 单 surface，color_bytes=4（ARGB8888），driver=NULL（直接写 m_phy_fb）。
     * surface 挂到后台缓冲 s_back：所有绘制写后台，上屏时再 present 到前台。 */
    s_display = new c_display((void*)s_back, width, height, width, height, 4, 1, NULL);
    s_surface = s_display->alloc_surface(Z_ORDER_LEVEL_0, c_rect(0, 0, width - 1, height - 1));
    /* 激活 surface：draw_pixel_low_level 仅在 m_is_active 为真时才把像素写进
     * 物理帧缓冲 m_phy_fb（即 s_back），否则绘制只在图层缓冲中、ref 不到。 */
    s_surface->set_active(true);
    c_theme::add_font(FONT_DEFAULT, (const void*)&s_font_info);
    return 0;
}

void vus_gui_surface_free(void)
{
    if (s_display)
    {
        delete s_display;
        s_display = 0;
    }
    if (s_fb)
    {
        free(s_fb);
        s_fb = 0;
    }
    if (s_back)
    {
        free(s_back);
        s_back = 0;
    }
    s_surface = 0;
    s_width = 0;
    s_height = 0;
}

void vus_gui_surface_draw_pixel(int x, int y, unsigned int argb)
{
    if (!s_surface) { return; }
    s_surface->draw_pixel(x, y, argb, Z_ORDER_LEVEL_0);
}

void vus_gui_surface_draw_line(int x1, int y1, int x2, int y2, unsigned int argb)
{
    if (!s_surface) { return; }
    s_surface->draw_line(x1, y1, x2, y2, argb, Z_ORDER_LEVEL_0);
}

void vus_gui_surface_draw_rect(int x, int y, int width, int height, unsigned int argb)
{
    if (!s_surface) { return; }
    int x1 = x + width - 1;
    int y1 = y + height - 1;
    s_surface->draw_rect(x, y, x1, y1, argb, Z_ORDER_LEVEL_0, 1);
}

void vus_gui_surface_fill_rect(int x, int y, int width, int height, unsigned int argb)
{
    if (!s_surface) { return; }
    int x1 = x + width - 1;
    int y1 = y + height - 1;
    s_surface->fill_rect(x, y, x1, y1, argb, Z_ORDER_LEVEL_0);
}

void vus_gui_surface_draw_text(int x, int y, const char* text, unsigned int argb)
{
    if (!s_surface || !text) { return; }
    s_font_op.draw_string(s_surface, Z_ORDER_LEVEL_0, text, x, y,
                          (const void*)&s_font_info, argb, 0);
}

unsigned int* vus_gui_surface_framebuffer(void)
{
    return s_fb;
}

/* 后台绘制缓冲：桥接层逐像素写入（write_scrolled_pixel / read_scrolled_pixel）
 * 应写入后台，redraw 时 present 到前台再上屏。 */
unsigned int* vus_gui_surface_backbuffer(void)
{
    return s_back;
}

/* 双缓冲提交：把后台整块拷贝到前台。之后 platform_redraw 读前台送入显示。 */
void vus_gui_surface_present(void)
{
    if (s_fb && s_back && s_width > 0 && s_height > 0)
    {
        memcpy(s_fb, s_back, (size_t)s_width * (size_t)s_height * sizeof(unsigned int));
    }
}

int vus_gui_surface_width(void)
{
    return s_width;
}

int vus_gui_surface_height(void)
{
    return s_height;
}

} /* extern "C" */