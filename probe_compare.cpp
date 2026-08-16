/*
 * 同屏对比探针：GuiLite 字体 vs X 核心字体
 *
 * 上半部分：用真实 GuiLite wrapper（guilite_wrapper.cpp）把 "VUS GUI Test"
 *           画进 ARGB8888 帧缓冲，再以与 guilite_platform.c 相同的
 *           XPutPixel/XPutImage 逻辑贴到窗口。
 * 下半部分：用 X 核心字体 XDrawString 显示同一串 "VUS GUI Test"。
 *
 * 若上半文字和下半文字在屏幕上方向一致 => X 层也正常，应一致正；
 * 若上半(Image)反向而下半(DrawString)正向 => 差异在 GuiLite 渲染后的传输/呈现。
 *
 * 用法：
 *   cd ~/.vus && git pull origin master
 *   g++ -O3 -o probe_compare probe_compare.cpp rt/guilite_wrapper.cpp -I rt -I rt/guilite -lX11
 *   unset VUS_X11_FLIP
 *   export DISPLAY=:0
 *   ./probe_compare
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

/* GuiLite wrapper 的 extern "C" 接口（见 guilite_bridge.h 内部接口段） */
extern "C" {
int vus_gui_surface_init(int width, int height);
void vus_gui_surface_free(void);
void vus_gui_surface_draw_text(int x, int y, const char* text, unsigned int argb);
void vus_gui_surface_fill_rect(int x, int y, int width, int height, unsigned int argb);
unsigned int* vus_gui_surface_framebuffer(void);
int vus_gui_surface_width(void);
int vus_gui_surface_height(void);
}

/* 复用 guilite_platform.c 的 ffs_pos：颜色分量左移到位掩码 */
static int ffs_pos(unsigned long mask)
{
    int pos = 0;
    while (mask && !(mask & 1)) { mask >>= 1; pos++; }
    return pos;
}

int main(void)
{
    int W = 460, H = 240;
    if (vus_gui_surface_init(W, H) != 0) { printf("GuiLite surface init 失败\n"); return 1; }

    /* 背景深灰 */
    vus_gui_surface_fill_rect(0, 0, W, H, 0xFF202020);
    /* 上半：GuiLite 文字（青色） */
    vus_gui_surface_draw_text(20, 30, "VUS GUI Test", 0xFF00FFFF);
    /* 下半标注还是上半标注都可，这里一并画上一行对比文字区说明 */
    vus_gui_surface_draw_text(20, 60, "0123456789", 0xFF00FFFF);

    unsigned int* fb = vus_gui_surface_framebuffer();
    if (!fb) { printf("无帧缓冲\n"); return 1; }

    /* X 窗口 */
    Display* dpy = XOpenDisplay(NULL);
    if (!dpy) { printf("无法打开 DISPLAY\n"); return 1; }
    int scr = DefaultScreen(dpy);
    Window win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr),
                                     0, 0, W, H, 1,
                                     BlackPixel(dpy, scr), BlackPixel(dpy, scr));
    XStoreName(dpy, win, "Compare");
    XSelectInput(dpy, win, ExposureMask | StructureNotifyMask | KeyPressMask);
    XMapWindow(dpy, win);

    GC gc = XCreateGC(dpy, win, 0, NULL);
    XSetForeground(dpy, gc, WhitePixel(dpy, scr));
    XFontStruct* font = XLoadQueryFont(dpy, "fixed");
    if (!font) font = XLoadQueryFont(dpy, "6x13");
    if (font) XSetFont(dpy, gc, font->fid);

    /* 创建与显示匹配的 XImage，XPutPixel 填入 */
    XImage* img = XCreateImage(dpy, DefaultVisual(dpy, scr),
                               (unsigned int)DefaultDepth(dpy, scr),
                               ZPixmap, 0, NULL, W, H, 32, 0);
    img->data = (char*)calloc((size_t)img->bytes_per_line * (size_t)img->height, 1);

    printf("窗口已创建：上半=GuiLite(Image)，下半=XDrawString(fixed)。按键退出\n");
    int ev_done = 0;
    while (!ev_done)
    {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.type == Expose)
        {
            /* 上半部 subwindow: 用 GuiLite fb 填充整窗 */
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++)
                {
                    unsigned int px = fb[(size_t)y * W + (size_t)x];
                    unsigned long val = 0;
                    if (img->red_mask)
                        val |= ((unsigned long)((px >> 16) & 0xFF) << ffs_pos(img->red_mask)) & img->red_mask;
                    if (img->green_mask)
                        val |= ((unsigned long)((px >> 8) & 0xFF) << ffs_pos(img->green_mask)) & img->green_mask;
                    if (img->blue_mask)
                        val |= ((unsigned long)(px & 0xFF) << ffs_pos(img->blue_mask)) & img->blue_mask;
                    XPutPixel(img, x, y, val);
                }
            XPutImage(dpy, win, gc, img, 0, 0, 0, 0, W, H);

            /* 下半：XDrawString 白色，画在窗口下半部 */
            XSetForeground(dpy, gc, (unsigned long)0x00FFFFFF);
            if (font)
                XDrawString(dpy, win, gc, 20, 200, "VUS GUI Test", 12);
        }
        else if (ev.type == KeyPress)
            ev_done = 1;
        else if (ev.type == ClientMessage)
            ev_done = 1;
    }

    XDestroyImage(img);
    if (font) XFreeFont(dpy, font);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    vus_gui_surface_free();
    return 0;
}