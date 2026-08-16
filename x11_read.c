/*
 * 读回 vus 实际窗口的像素并打印字形方向 —— 终极验证工具
 *
 * 用途：在第二个终端运行，读取正在显示的 VUS 窗口（标题含 "VUS"）内部
 *       青色文字区(22..150, 180..189)的像素并 dump 成 ASCII。
 * 目的：
 *   - 若读回的窗口内容 == 正向 PPM（VUS GUI Test 正向）  => X server 内部完全正确，
 *     翻转发生在 X server -> 手机屏幕 的 GL 呈现层。
 *   - 若读回的窗口内容 == 镜像                           => X server 层就有问题（异常，需查 redraw）。
 *
 * 用法（先跑 demo，再开第二个终端）：
 *   gcc -O2 -o x11_read x11_read.c -lX11
 *   DISPLAY=:0 ./x11_read
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

static Window find_by_name(Display* dpy, Window root, const char* key)
{
    Window root_ret, parent_ret, *children = NULL;
    unsigned int n = 0;
    if (!XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &n))
        return 0;
    for (unsigned int i = 0; i < n; i++)
    {
        char* name = NULL;
        if (XFetchName(dpy, children[i], &name) && name)
        {
            if (strstr(name, key))
            {
                Window w = children[i];
                if (name) XFree(name);
                if (children) XFree(children);
                return w;
            }
            XFree(name);
        }
        Window sub = find_by_name(dpy, children[i], key);
        if (sub)
        {
            if (children) XFree(children);
            return sub;
        }
    }
    if (children) XFree(children);
    return 0;
}

int main(void)
{
    Display* dpy = XOpenDisplay(NULL);
    if (!dpy) { printf("无法打开 DISPLAY\n"); return 1; }
    int scr = DefaultScreen(dpy);
    Window root = RootWindow(dpy, scr);

    printf("查找标题含 \"VUS\" 的窗口...\n");
    Window win = find_by_name(dpy, root, "VUS");
    if (!win)
    {
        printf("未找到 VUS 窗口。请先运行 demo 并保持在事件循环中。\n");
        printf("列出所有顶层窗口名:\n");
        Window *ch=NULL,*rr,*pr; unsigned int n=0;
        if (XQueryTree(dpy, root, &rr, &pr, &ch, &n))
            for (unsigned int i=0;i<n;i++){
                char* nm=NULL; XFetchName(dpy,ch[i],&nm);
                printf("  0x%lx : %s\n",(unsigned long)ch[i], nm?nm:"(无)"); if(nm)XFree(nm);
            }
        if(ch)XFree(ch);
        XCloseDisplay(dpy);
        return 2;
    }
    printf("找到窗口 0x%lx\n", (unsigned long)win);

    XWindowAttributes attr;
    XGetWindowAttributes(dpy, win, &attr);
    int W = attr.width, H = attr.height;
    printf("窗口尺寸 %dx%d\n", W, H);

    /* 读回窗口左上方（含文字区） */
    XImage* img = XGetImage(dpy, win, 0, 0, W, H, AllPlanes, ZPixmap);
    if (!img) { printf("XGetImage 失败\n"); return 3; }

    printf("=== 青色文字区 y=175..189, x=15..150 ===\n");
    for (int yy = 175; yy < 190; yy++)
    {
        char row[512]; int p = 0;
        for (int xx = 15; xx < 150; xx++)
        {
            unsigned long c = XGetPixel(img, xx, yy);
            unsigned int b = (unsigned int)(c & 0xFF);
            unsigned int g = (unsigned int)((c >> 8) & 0xFF);
            unsigned int r = (unsigned int)((c >> 16) & 0xFF);
            row[p++] = (g > 150 && b > 150 && r < 150) ? '#' : '.';
            if (p >= 500) break;
        }
        row[p] = 0;
        printf("%s\n", row);
    }
    printf("=== 黄色块区域 y=110..150, x=160..270 ===\n");
    for (int yy = 110; yy < 160; yy += 8)
    {
        char row[512]; int p = 0;
        for (int xx = 160; xx < 275; xx++)
        {
            unsigned long c = XGetPixel(img, xx, yy);
            unsigned int b = (unsigned int)(c & 0xFF);
            unsigned int g = (unsigned int)((c >> 8) & 0xFF);
            unsigned int r = (unsigned int)((c >> 16) & 0xFF);
            row[p++] = (r > 200 && g > 200 && b < 120) ? 'Y' : '.';
        }
        row[p] = 0;
        printf("%s\n", row);
    }

    XDestroyImage(img);
    XCloseDisplay(dpy);
    return 0;
}