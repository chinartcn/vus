/*
 * VUS 集成 GuiLite —— 显示平台层
 *
 * 负责把 GuiLite 的 ARGB8888 帧缓冲送到显示后端：
 *  - VUS_GUI_X11 定义时使用 X11 后端（XOpenDisplay / XPutImage / 事件循环），
 *    同一套代码适用于 Termux X11（DISPLAY=:0）、PC X11 与 Xvfb 无头。
 *  - 未定义 VUS_GUI_X11 时为 headless 模式：vus_gui_redraw 把帧缓冲导出为
 *    PPM 文件（/tmp/gui_out.ppm），便于自动化测试断言像素。
 *
 * 无论哪种模式，每次 redraw 都会导出 PPM，便于统一验证像素。
 */

#include "guilite_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef VUS_GUI_X11
#include <X11/Xlib.h>
#include <X11/Xutil.h>

static Display* s_dpy = 0;
static Window   s_win = 0;
static GC       s_gc = 0;
static XImage*  s_img = 0;
static Atom     s_wm_delete = 0;
static int      s_running = 0;

/* 显示方向补偿开关：部分后端（如 Termux-X11 的 GL 渲染）会把窗口内容
 * 翻转显示。用环境变量 VUS_X11_FLIP 控制：
 *   空/未设置  不翻转
 *   v          垂直翻转（上下颠倒）
 *   h          水平翻转（左右镜像）
 *   vh / hv    垂直+水平翻转（180度）
 * 在 vus_gui_platform_init 中读取一次，redraw 时按开关映射像素坐标。 */
static int s_flip_v = 0;
static int s_flip_h = 0;

/* 返回无符号整数中最低有效位 SET 的位置（用于把 8bit 颜色分量左移到掩码位） */
static int ffs_pos(unsigned long mask)
{
    int pos = 0;
    while (mask && !(mask & 1)) { mask >>= 1; pos++; }
    return pos;
}
#endif

/* 把 ARGB8888 帧缓冲导出为 PPM P6 图像（RGB 各 8bit） */
static void export_ppm(const char* path, int width, int height, const unsigned int* fb)
{
    FILE* f = fopen(path, "wb");
    if (!f) { return; }
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (int i = 0; i < width * height; i++)
    {
        unsigned int p = fb[i]; /* ARGB8888 */
        unsigned char r = (unsigned char)((p >> 16) & 0xFF);
        unsigned char g = (unsigned char)((p >> 8) & 0xFF);
        unsigned char b = (unsigned char)(p & 0xFF);
        fwrite(&r, 1, 1, f);
        fwrite(&g, 1, 1, f);
        fwrite(&b, 1, 1, f);
    }
    fclose(f);
}

int vus_gui_platform_init(int width, int height, const char* title)
{
    (void)title;
#ifdef VUS_GUI_X11
    s_dpy = XOpenDisplay(NULL);
    if (!s_dpy)
    {
        /* X11 不可用则回退 headless（仅导出 PPM） */
        return 0;
    }
    int screen = DefaultScreen(s_dpy);

    /* 读取显示方向补偿开关（Termux-X11 等后端可能翻转） */
    s_flip_v = 0;
    s_flip_h = 0;
    {
        const char* fl = getenv("VUS_X11_FLIP");
        if (fl)
        {
            if (strchr(fl, 'v') || strchr(fl, 'V')) { s_flip_v = 1; }
            if (strchr(fl, 'h') || strchr(fl, 'H')) { s_flip_h = 1; }
        }
    }

    s_win = XCreateSimpleWindow(s_dpy, RootWindow(s_dpy, screen),
                                0, 0, (unsigned int)width, (unsigned int)height,
                                1, BlackPixel(s_dpy, screen), WhitePixel(s_dpy, screen));
    if (title)
    {
        XStoreName(s_dpy, s_win, title);
    }
    XSelectInput(s_dpy, s_win, ExposureMask | StructureNotifyMask | KeyPressMask);
    XMapWindow(s_dpy, s_win);

    s_gc = XCreateGC(s_dpy, s_win, 0, NULL);
    s_wm_delete = XInternAtom(s_dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(s_dpy, s_win, &s_wm_delete, 1);

    /* 创建与显示匹配的 XImage；XCreateImage(data=NULL) 不会分配数据缓冲，
     * 需自行分配（32bpp 时 bytes_per_line = width*4，与 ARGB 帧缓冲布局一致）。 */
    s_img = XCreateImage(s_dpy, DefaultVisual(s_dpy, screen),
                         (unsigned int)DefaultDepth(s_dpy, screen),
                         ZPixmap, 0, NULL, (unsigned int)width, (unsigned int)height,
                         32, 0);
    if (!s_img)
    {
        XDestroyWindow(s_dpy, s_win);
        XCloseDisplay(s_dpy);
        s_dpy = 0;
        return 0;
    }
    s_img->data = (char*)calloc((size_t)s_img->bytes_per_line * (size_t)s_img->height, 1);
    if (!s_img->data)
    {
        XDestroyImage(s_img);
        XDestroyWindow(s_dpy, s_win);
        XCloseDisplay(s_dpy);
        s_dpy = 0;
        s_img = 0;
        return 0;
    }
    s_running = 1;
#endif /* VUS_GUI_X11 */
    return 0;
}

void vus_gui_platform_redraw(int width, int height, const unsigned int* fb)
{
    if (!fb) { return; }

    /* 始终导出 PPM 供自动化验证像素 */
    export_ppm("/tmp/gui_out.ppm", width, height, fb);

#ifdef VUS_GUI_X11
    if (!s_dpy || !s_img || !fb) { return; }
    /* 用 XPutPixel 逐像素写入 s_img->data：Xlib 会根据 XImage 的
     * byte_order / bit_order / 各颜色掩码自动换算像素值，从而适配
     * 任意 visual / 深度 / 字节序（Termux Xwayland、PC X11、Xvfb）。
     * 相比直接 memcpy 假设内存布局与服务器一致，可避免错位与方向颠倒。 */
    for (int y = 0; y < height; y++)
    {
        for (int x = 0; x < width; x++)
        {
            unsigned int px = fb[(size_t)y * (size_t)width + (size_t)x];
            unsigned long val = 0;
            if (s_img->red_mask)
                val |= ((unsigned long)((px >> 16) & 0xFF) << ffs_pos(s_img->red_mask)) & s_img->red_mask;
            if (s_img->green_mask)
                val |= ((unsigned long)((px >> 8) & 0xFF) << ffs_pos(s_img->green_mask)) & s_img->green_mask;
            if (s_img->blue_mask)
                val |= ((unsigned long)(px & 0xFF) << ffs_pos(s_img->blue_mask)) & s_img->blue_mask;
            /* 翻转补偿：按 VUS_X11_FLIP 映射目标像素坐标 */
            int tx = s_flip_h ? (width - 1 - x) : x;
            int ty = s_flip_v ? (height - 1 - y) : y;
            XPutPixel(s_img, tx, ty, val);
        }
    }
    XPutImage(s_dpy, s_win, s_gc, s_img, 0, 0, 0, 0,
              (unsigned int)width, (unsigned int)height);
    XFlush(s_dpy);
#endif
}

void vus_gui_platform_run(int width, int height, const unsigned int* fb)
{
#ifdef VUS_GUI_X11
    if (!s_dpy)
    {
        return; /* headless 回退：无事件循环 */
    }
    while (s_running)
    {
        XEvent ev;
        XNextEvent(s_dpy, &ev);
        switch (ev.type)
        {
        case Expose:
            vus_gui_platform_redraw(width, height, fb);
            break;
        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == s_wm_delete)
            {
                s_running = 0;
            }
            break;
        case KeyPress:
            /* 任意按键退出事件循环，便于测试 */
            s_running = 0;
            break;
        default:
            break;
        }
    }
    if (s_img)
    {
        XDestroyImage(s_img);
        s_img = 0;
    }
    if (s_gc)
    {
        XFreeGC(s_dpy, s_gc);
        s_gc = 0;
    }
    if (s_win)
    {
        XDestroyWindow(s_dpy, s_win);
        s_win = 0;
    }
    XCloseDisplay(s_dpy);
    s_dpy = 0;
#else
    /* headless 模式：短暂等待后返回，保证程序可正常结束 */
    (void)width; (void)height; (void)fb;
    /* 空实现：无显示后端时无需保持 */
#endif
}