/*
 * 纯 X11 文字显示探针：绕过 GuiLite，直接用 X 核心字体显示文字。
 * 用于判断"文字反向"是发生在 X 层，还是 GuiLite 字体渲染层。
 *
 * 画面（期望方向）：
 *   背景黑色
 *   纵横交错的彩色图形（带方向性，能看出是否被翻转）
 *   下方白色 XDrawString 文字 "VUS GUI Test"
 *   上方青色 XDrawString 文字 "0123456789"
 *
 * 若 XDrawString 文字在屏幕上也是反向的 => X/GL 层翻转（不是 GuiLite 问题）。
 * 若 XDrawString 文字正向，而 vus(GuiLite) 文字反向 => GuiLite 字体渲染层问题。
 *
 * 用法：
 *   gcc -O2 -o probe_text probe_text.c -lX11
 *   DISPLAY=:0 ./probe_text
 */
#include <stdio.h>
#include <stdlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

int main(void)
{
    Display* dpy = XOpenDisplay(NULL);
    if (!dpy) { printf("无法打开 DISPLAY\n"); return 1; }
    int scr = DefaultScreen(dpy);
    Window root = RootWindow(dpy, scr);
    int W = 460, H = 240;

    Window win = XCreateSimpleWindow(dpy, root, 0, 0, W, H, 1,
                                     BlackPixel(dpy, scr), BlackPixel(dpy, scr));
    XStoreName(dpy, win, "VUS Text Probe");
    XSelectInput(dpy, win, ExposureMask | StructureNotifyMask | KeyPressMask);
    XMapWindow(dpy, win);
    XFlush(dpy);

    GC gc = XCreateGC(dpy, win, 0, NULL);
    XSetForeground(dpy, gc, WhitePixel(dpy, scr));

    /* 选择一个 core 字体（X 自带 fixed / 6x13 等） */
    XFontStruct* font = XLoadQueryFont(dpy, "fixed");
    if (!font) font = XLoadQueryFont(dpy, "6x13");
    if (font)
        XSetFont(dpy, gc, font->fid);
    else
        printf("提示: fixed/6x13 字体均不可用，仍尝试绘制\n");

    printf("窗口已创建，保持显示中... 按键或关闭窗口退出\n");
    while (1)
    {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.type == Expose)
        {
            /* 背景填充深蓝 */
            XSetForeground(dpy, gc, 0x102030);
            XFillRectangle(dpy, win, gc, 0, 0, W, H);

            /* 方向性图形：黄色填充矩形（右半）+ 红色十字（中心）+ 绿线（左上对角线） */
            XSetForeground(dpy, gc, 0x00FFFF00); /* 黄 */
            XFillRectangle(dpy, win, gc, W/2, H/2, W/2, H/2);
            XSetForeground(dpy, gc, 0x00FF0000); /* 红 */
            XFillRectangle(dpy, win, gc, W/2-10, H/2-10, 20, 20);
            XSetForeground(dpy, gc, 0x0000FF00); /* 绿 */
            XFillRectangle(dpy, win, gc, 0, 0, 60, 2);

            /* 用 XDrawString 显示文字（X 核心字体） */
            XSetForeground(dpy, gc, 0x00FFFFFF); /* 白 */
            if (font)
                XDrawString(dpy, win, gc, 10, 30, "VUS GUI Test", 12);
            XSetForeground(dpy, gc, 0x0000FFFF); /* 青 */
            if (font)
                XDrawString(dpy, win, gc, 10, 60, "0123456789", 10);
        }
        else if (ev.type == KeyPress)
            break;
        else if (ev.type == ClientMessage)
            break;
    }
    if (font) XFreeFont(dpy, font);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}