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

/* dlsym 需要 <dlfcn.h> 与链接期 -rdynamic/-ldl（见 src/generator.c GUI 链接参数） */
#include <dlfcn.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static int s_initialized = 0;

/* 把 0xRRGGBB 转为 0xAARRGGBB（不透明） */
static unsigned int argb_from_rgb(unsigned int rgb)
{
    return 0xFF000000u | (rgb & 0x00FFFFFFu);
}

/* ============ 回调函数名反查 ============
 * VUS 语言没有一等函数，函数指针不能作值传递，因此采用“约定式回调”：
 * 脚本只需定义一个固定名函数（如 事件_点击(x,y)），平台层在事件发生时
 * 按同名反查其编译后的 C 符号并调用。VUS 编译器把用户函数 `X` 编译为
 * 全局非 static 的 `void vus_<sanitized X>(void*)`，参数为 VusString**，
 * 约定 [0] 为返回值槽、[1..] 为参数。 */

/* 回调约定名：命中后回调 <fn>(x, y) */
#define VUS_CALLBACK_CLICK "事件_点击"

/* 与 src/generator.c 的 gen_sanitize_name 保持一致：把中文（UTF-8 多字节）
 * 与其它非 ASCII 标识符换算为 _XXXX、ASCII 下划线保留，从而生成与编译器
 * 完全一致的符号名，才能 dlsym 到用户脚本定义的函数。 */
static void vus_gui_sanitize_name(const char* name, char* out, size_t out_size)
{
    size_t i = 0, o = 0;
    while (name[i] && o < out_size - 1)
    {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x80 && (isalnum(c) || c == '_'))
        {
            out[o++] = (char)c;
            i++;
        }
        else if (c >= 0x80)
        {
            unsigned int code = 0;
            int bytes = 0;
            if ((c & 0xE0) == 0xC0)      { code = c & 0x1F; bytes = 2; }
            else if ((c & 0xF0) == 0xE0) { code = c & 0x0F; bytes = 3; }
            else if ((c & 0xF8) == 0xF0) { code = c & 0x07; bytes = 4; }
            else { i++; continue; }
            size_t j;
            for (j = 1; j < (size_t)bytes; j++)
                if (name[i + j]) code = (code << 6) | ((unsigned char)name[i + j] & 0x3F);
            i += bytes;
            if (o + 7 < out_size)
            {
                int n = snprintf(out + o, out_size - o, "_%04X", code);
                if (n > 0) o += (size_t)n;
            }
        }
        else
        {
            if (o + 1 < out_size) out[o++] = '_';
            i++;
        }
    }
    out[o] = '\0';
}

/* 最近一次点击坐标（-1 = 尚无点击），供 图形_按钮点击 命中检测 */
static int s_click_x = -1;
static int s_click_y = -1;

/* ===== 控件表（阶段2：按钮命中检测） ===== */
#define VUS_BUTTON_MAX 64
typedef struct { char name[64]; int x, y, w, h; } VusButton;
static VusButton s_buttons[VUS_BUTTON_MAX];
static int       s_button_cnt = 0;

/* 记录最近点击坐标，并触发约定回调 事件_点击(x,y)（可选，见上）。 */
void vus_gui_platform_emit_click(int x, int y)
{
    /* 无论是否有回调，都更新点击状态，供轮询式 图形_按钮点击 命中检测 */
    s_click_x = x;
    s_click_y = y;

    char san[256];
    char sym[512];
    vus_gui_sanitize_name(VUS_CALLBACK_CLICK, san, sizeof(san));
    snprintf(sym, sizeof(sym), "vus_%s", san);

    void (*fn)(void*) = (void(*)(void*))dlsym(RTLD_DEFAULT, sym);
    if (!fn) return;

    VusString* xs = vus_to_string(x);
    VusString* ys = vus_to_string(y);
    if (!xs || !ys)
    {
        if (xs) vus_unref(xs);
        if (ys) vus_unref(ys);
        return;
    }
    VusString* args[3] = { 0, xs, ys };
    fn(args);
    /* 参数由调用方负责释放（VUS 函数体内只 vus_ref，不负责 unref 参数） */
    vus_unref(xs);
    vus_unref(ys);
}

/* 创建并绘制一个按钮：填充底色 + 边框 + 文本（文本优先 X11 叠加，方向正常），
 * 并登记命中矩形。同名按钮重复调用时更新位置/尺寸。 */
VusString* vus_gui_button(const char* name, int x, int y, int w, int h, const char* text)
{
    if (!s_initialized || !name || !*name || w <= 0 || h <= 0)
    {
        return vus_string_new("0");
    }
    if (s_button_cnt >= VUS_BUTTON_MAX)
    {
        return vus_string_new("0");
    }
    int idx = -1;
    for (int i = 0; i < s_button_cnt; i++)
    {
        if (strcmp(s_buttons[i].name, name) == 0) { idx = i; break; }
    }
    if (idx < 0) { idx = s_button_cnt++; }
    strncpy(s_buttons[idx].name, name, sizeof(s_buttons[idx].name) - 1);
    s_buttons[idx].name[sizeof(s_buttons[idx].name) - 1] = '\0';
    s_buttons[idx].x = x; s_buttons[idx].y = y;
    s_buttons[idx].w = w; s_buttons[idx].h = h;

    /* 默认配色：蓝底 + 深蓝边框 + 白字（居中） */
    vus_gui_surface_fill_rect(x, y, w, h, argb_from_rgb(0x3399CC));
    vus_gui_surface_draw_rect(x, y, w, h, argb_from_rgb(0x0A2A3A));
    int tw = text ? (int)(strlen(text) * 6) : 0; /* 估宽：6px/字符（X 6x13 字体） */
    int tx = x + (tw < w ? (w - tw) / 2 : 0);
    int ty = y + (h - 13) / 2;
    if (vus_gui_platform_draw_text(tx, ty, text ? text : "", 0xFFFFFF) != 1)
    {
        /* X11 字体不可用时回退 GuiLite 帧缓冲文字 */
        vus_gui_surface_draw_text(tx, ty, text ? text : "", argb_from_rgb(0xFFFFFF));
    }
    return vus_string_new("1");
}

/* 非阻塞处理 X 事件队列（轮询式交互核心），更新点击坐标。 */
VusString* vus_gui_poll(void)
{
    if (!s_initialized)
    {
        return vus_string_new("0");
    }
    vus_gui_platform_poll(vus_gui_surface_width(), vus_gui_surface_height(),
                          vus_gui_surface_framebuffer());
    return vus_string_new("1");
}

/* 命中检测：最近一次点击是否落在名为 name 的按钮矩形内。 */
VusString* vus_gui_button_clicked(const char* name)
{
    if (!s_initialized || !name || s_click_x < 0 || s_click_y < 0)
    {
        return vus_string_new("false");
    }
    for (int i = 0; i < s_button_cnt; i++)
    {
        if (strcmp(s_buttons[i].name, name) == 0)
        {
            int in = (s_click_x >= s_buttons[i].x && s_click_x < s_buttons[i].x + s_buttons[i].w &&
                      s_click_y >= s_buttons[i].y && s_click_y < s_buttons[i].y + s_buttons[i].h);
            return vus_string_new(in ? "true" : "false");
        }
    }
    return vus_string_new("false");
}

/* 模拟点击：注入一次点击坐标，验证命中检测与 如果 条件分支（headless 测试用）。 */
VusString* vus_gui_sim_click(int x, int y)
{
    if (!s_initialized)
    {
        return vus_string_new("0");
    }
    vus_gui_platform_emit_click(x, y);
    return vus_string_new("1");
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
    /* 优先用 X11 文字（XDrawString，方向正常）；X11 不可用时回退 GuiLite */
    if (vus_gui_platform_draw_text(x, y, text, color) == 1)
    {
        return vus_string_new("1");
    }
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