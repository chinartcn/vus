/*
 * 保持显示探针：创建一个纯红色 200x200 窗口，进入事件循环保持不退出。
 * 用途：判断 Termux-X11 屏幕上是否真的能显示 X 窗口。
 *   - 屏幕上出现红色窗口 → Termux-X11 显示正常，问题在 vus 程序内容
 *   - 屏幕无窗口 / 黑窗  → Termux-X11 会话或窗口显示有问题
 * 用法：gcc -O2 -o probe_show probe_show.c -lX11 && DISPLAY=:0 ./probe_show
 * 任意按键或关闭窗口退出。
 */
#include <stdio.h>
#include <stdlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#define W 200
#define H 200

int main(void)
{
    Display* dpy = XOpenDisplay(NULL);
    if (!dpy) { printf("无法打开 DISPLAY\n"); return 1; }
    int scr = DefaultScreen(dpy);

    Window win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr),
                                     0, 0, W, H, 1,
                                     BlackPixel(dpy, scr), BlackPixel(dpy, scr));
    XStoreName(dpy, win, "probe_show (red)");
    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);
    XSelectInput(dpy, win, ExposureMask | StructureNotifyMask | KeyPressMask);
    XMapWindow(dpy, win);
    XSync(dpy, False);

    GC gc = XCreateGC(dpy, win, 0, NULL);
    XImage* img = XCreateImage(dpy, DefaultVisual(dpy, scr),
                               (unsigned)DefaultDepth(dpy, scr),
                               ZPixmap, 0, NULL, W, H, 32, 0);
    img->data = (char*)calloc((size_t)img->bytes_per_line * img->height, 1);
    if (!img->data) { printf("alloc fail\n"); return 1; }

    /* 全屏填纯红 */
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            XPutPixel(img, x, y, (unsigned long)0xFF0000);

    XPutImage(dpy, win, gc, img, 0, 0, 0, 0, W, H);
    XFlush(dpy);
    printf("已绘制红色窗口，保持显示中... 按键或关闭窗口退出\n");

    int running = 1;
    while (running)
    {
        XEvent ev;
        XNextEvent(dpy, &ev);
        switch (ev.type)
        {
        case Expose:
            XPutImage(dpy, win, gc, img, 0, 0, 0, 0, W, H);
            XFlush(dpy);
            break;
        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == wm_delete) running = 0;
            break;
        case KeyPress:
            running = 0;
            break;
        default:
            break;
        }
    }
    printf("退出\n");
    XDestroyImage(img);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}