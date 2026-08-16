/*
 * 方向观测探针：画出四个象限异色，保持显示，请肉眼观察屏幕各角颜色。
 * 通过四角颜色位置即可反推出 Termux-X11 显示层到底做了何种翻转。
 *
 * 布局（360x240，每象限 180x120）：
 *   左上=红(0xFF0000)   右上=绿(0x00FF00)
 *   左下=蓝(0x0000FF)   右下=青(0x00FFFF)
 *   中心=白色十字
 *
 * 用法：gcc -O2 -o probe_orient probe_orient.c -lX11 && DISPLAY=:0 ./probe_orient
 * 任意按键或关闭窗口退出。
 */
#include <stdio.h>
#include <stdlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#define W 360
#define H 240

int main(void)
{
    Display* dpy = XOpenDisplay(NULL);
    if (!dpy) { printf("无法打开 DISPLAY\n"); return 1; }
    int scr = DefaultScreen(dpy);

    Window win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr),
                                     0, 0, W, H, 1,
                                     BlackPixel(dpy, scr), BlackPixel(dpy, scr));
    XStoreName(dpy, win, "probe_orient");
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

    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
        {
            unsigned long c;
            if (x == W/2 || y == H/2) c = 0xFFFFFF;             /* 中心十字白 */
            else if (x < W/2 && y < H/2) c = 0xFF0000;           /* 左上红 */
            else if (x >= W/2 && y < H/2) c = 0x00FF00;          /* 右上绿 */
            else if (x < W/2 && y >= H/2) c = 0x0000FF;          /* 左下蓝 */
            else c = 0x00FFFF;                                    /* 右下青 */
            XPutPixel(img, x, y, c);
        }

    XPutImage(dpy, win, gc, img, 0, 0, 0, 0, W, H);
    XFlush(dpy);
    printf("已绘制四角异色窗口。请观察屏幕并告诉我四个角的颜色。\n");

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
    XDestroyImage(img);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}