/*
 * Termux-X11 显示方向诊断探针
 *
 * 运行方式（Termux 中）：
 *   gcc -O2 -o /tmp/fbprobe fbprobe.c -lX11
 *   DISPLAY=:0 /tmp/fbprobe
 *
 * 输出：
 *   1) 当前 visual 的 depth / byte_order / 各颜色掩码
 *   2) 用与 VUS guilite_platform 相同的方式（XPutPixel + XPutImage）
 *      在窗口四角画 红/绿/蓝/青，再 XGetImage 读回窗口，
 *      对比实际颜色，判断屏幕方向是否颠倒/镜像。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#define W 64
#define H 64

static int ffs_pos(unsigned long mask)
{
    int pos = 0;
    while (mask && !(mask & 1)) { mask >>= 1; pos++; }
    return pos;
}

static void put_argb(XImage* img, int x, int y, unsigned int px)
{
    unsigned long val = 0;
    if (img->red_mask)
        val |= ((unsigned long)((px >> 16) & 0xFF) << ffs_pos(img->red_mask)) & img->red_mask;
    if (img->green_mask)
        val |= ((unsigned long)((px >> 8) & 0xFF) << ffs_pos(img->green_mask)) & img->green_mask;
    if (img->blue_mask)
        val |= ((unsigned long)(px & 0xFF) << ffs_pos(img->blue_mask)) & img->blue_mask;
    XPutPixel(img, x, y, val);
}

int main(void)
{
    Display* dpy = XOpenDisplay(NULL);
    if (!dpy) { printf("无法打开 DISPLAY=%s\n", getenv("DISPLAY")); return 1; }
    int scr = DefaultScreen(dpy);

    Visual* vis = DefaultVisual(dpy, scr);
    int depth = DefaultDepth(dpy, scr);
    printf("== Visual 信息 ==\n");
    printf("depth=%d bytes_per_rgb=%d\n", depth, vis->bits_per_rgb);
    printf("red_mask  =0x%08lX\n", (unsigned long)vis->red_mask);
    printf("green_mask=0x%08lX\n", (unsigned long)vis->green_mask);
    printf("blue_mask =0x%08lX\n", (unsigned long)vis->blue_mask);
    printf("screens[%d] imaging=%d\n", XScreenNumberOfScreen(DefaultScreenOfDisplay(dpy)),
           (int)XImageByteOrder(dpy));

    Window win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr),
                                     0, 0, W, H, 1,
                                     BlackPixel(dpy, scr), BlackPixel(dpy, scr));
    XMapWindow(dpy, win);
    XSync(dpy, False);

    GC gc = XCreateGC(dpy, win, 0, NULL);
    XImage* img = XCreateImage(dpy, vis, (unsigned)depth, ZPixmap, 0, NULL,
                               W, H, 32, 0);
    img->data = (char*)calloc((size_t)img->bytes_per_line * img->height, 1);
    printf("XImage: bytes_per_line=%lu byte_order=%s\n",
           (unsigned long)img->bytes_per_line,
           img->byte_order == LSBFirst ? "LSBFirst" : "MSBFirst");

    /* 四角：左上红 右上绿 左下蓝 右下青 */
    put_argb(img, 0, 0, 0xFFFF0000u);
    put_argb(img, W-1, 0, 0xFF00FF00u);
    put_argb(img, 0, H-1, 0xFF0000FFu);
    put_argb(img, W-1, H-1, 0xFF00FFFFu);

    XPutImage(dpy, win, gc, img, 0, 0, 0, 0, W, H);
    XSync(dpy, False);

    XImage* back = XGetImage(dpy, win, 0, 0, W, H, AllPlanes, ZPixmap);
    if (!back) { printf("XGetImage 失败\n"); return 1; }
    unsigned long tl = XGetPixel(back, 0, 0);
    unsigned long tr = XGetPixel(back, W-1, 0);
    unsigned long bl = XGetPixel(back, 0, H-1);
    unsigned long br = XGetPixel(back, W-1, H-1);
    printf("\n== 读回四角实际颜色 ==\n");
    printf("左上=0x%06lX (期望 FF0000 红)\n", tl & 0xFFFFFF);
    printf("右上=0x%06lX (期望 00FF00 绿)\n", tr & 0xFFFFFF);
    printf("左下=0x%06lX (期望 0000FF 蓝)\n", bl & 0xFFFFFF);
    printf("右下=0x%06lX (期望 00FFFF 青)\n", br & 0xFFFFFF);

    printf("\n== 方向判定 ==\n");
    int tl_r = (tl & 0xFF0000) == 0xFF0000;
    int tr_g = (tr & 0x00FF00) == 0x00FF00;
    int bl_b = (bl & 0x0000FF) == 0x0000FF;
    int br_c = (br & 0xFFFFFF) == 0x00FFFF;
    if (tl_r && tr_g && bl_b && br_c) printf("方向正确（无翻转）\n");
    else {
        /* 红找到了吗，在哪 */
        const char* where[4] = {0};
        unsigned int corners[4] = { tl, tr, bl, br };
        const char* names[4] = { "左上", "右上", "左下", "右下" };
        for (int i = 0; i < 4; i++)
            if ((corners[i] & 0xFF0000) == 0xFF0000) where[i] = "红";
        printf("红像素出现在: 左上=%s 右上=%s 左下=%s 右下=%s\n",
               where[0]?"是":"否", where[1]?"是":"否",
               where[2]?"是":"否", where[3]?"是":"否");
        printf("蓝像素出现在: 左上=%s 右上=%s 左下=%s 右下=%s\n",
               (tl&0xFF)==0xFF?"是":"否", (tr&0xFF)==0xFF?"是":"否",
               (bl&0xFF)==0xFF?"是":"否", (br&0xFF)==0xFF?"是":"否");
    }

    XDestroyImage(back);
    XDestroyImage(img);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}