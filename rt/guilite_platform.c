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

/* 启用 strdup 等 POSIX/GNU 声明（X11 文字入队用；须在含 string.h 前定义） */
#define _GNU_SOURCE

#include "guilite_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef VUS_GUI_X11
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>

#ifdef VUS_GUI_GLES
#include "guilite_gles.h"
#endif

static Display* s_dpy = 0;
static Window   s_win = 0;
static GC       s_gc = 0;
static XImage*  s_img = 0;
static Atom     s_wm_delete = 0;
static int      s_running = 0;

/* G3：32bpp 快速路径标记。XImage 与帧缓冲同为 32bpp 且各通道掩码/字节序与本机
 * 一致时（ARGB8888 直配），redraw 由逐像素 XPutPixel 位掩码换算优化为按行
 * memcpy（支持垂直翻转的行序重排；水平镜像需逐像素交换，禁用快路径）。 */
static int      s_fast32 = 0;

/* X11 文字叠加：优先用 Xft（FontConfig + FreeType）按 UTF-8 叠加文本，支持
 * 中英文混合与系统中文字体自动回退；Xft 不可用时回退 XDrawString（X 核心
 * 字体，仅 ASCII）。文字请求先入队，redraw 时在 XPutImage 之后重放，避免
 * 被帧缓冲覆盖。两者都不可用时返回 0，由桥接层回退到 GuiLite 帧缓冲。 */
static XftDraw*   s_xft = 0;
static XftFont*   s_xftfont = 0;
static XftFont*   s_fallback_font = 0;
static XFontStruct* s_xfont = 0;
#define VUS_TEXT_MAX 256
typedef struct { int x; int y; unsigned long color; char* text; } VUS_XText;
static VUS_XText     s_texts[VUS_TEXT_MAX];
static int           s_text_cnt = 0;

/* 显示方向补偿开关：部分后端（如 Termux-X11 的 GL 渲染）会把窗口内容
 * 翻转显示。用环境变量 VUS_X11_FLIP 控制：
 *   none / 0 / off    不翻转（默认）
 *   v                 垂直翻转（上下颠倒）
 *   h                 水平翻转（左右镜像）
 *   vh / hv           垂直+水平翻转（180度）
 * 在 vus_gui_platform_init 中读取一次，redraw 时按开关映射像素坐标。
 *
 * 说明：PPM / XGetImage 读回 X server 内部的像素始终是正向的，所以
 * 这里默认不翻转，保持 X 层正确。Termux-X11 的 GL 贴图层翻转应由其
 * 专用的 TERMUX_X11_FORCE_FLIP 来处理，避免两层叠加混乱。 */
static int s_flip_v = 0;
static int s_flip_h = 0;

/* 返回无符号整数中最低有效位 SET 的位置（用于把 8bit 颜色分量左移到掩码位） */
static int ffs_pos(unsigned long mask)
{
    int pos = 0;
    while (mask && !(mask & 1)) { mask >>= 1; pos++; }
    return pos;
}

#ifdef VUS_GUI_GLES
/* 是否启用 EGL/GLES 上屏（init 成功后置位；一旦启用 redraw 走 GL，不再走 XImage） */
static int s_gles = 0;
#endif
#endif

/* 调试开关：VUS_GUI_DEBUG=1 时每次 redraw 打印帧缓冲概要并导出 PPM。
 * 默认关闭，避免 60fps 下每帧写日志/写盘导致终端与 logcat 数据堆积、
 * CPU 持续空转无法休眠、进而触发 Android 异常耗电杀进程。 */
static int s_dbg = 0;

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
    /* 调试开关：仅当 VUS_GUI_DEBUG 非空且非 "0" 时输出绘制日志与 PPM */
    s_dbg = 0;
    {
        const char* dbg = getenv("VUS_GUI_DEBUG");
        if (dbg && dbg[0] && strcmp(dbg, "0") != 0) s_dbg = 1;
    }
#ifdef VUS_GUI_X11
    s_dpy = XOpenDisplay(NULL);
    if (!s_dpy)
    {
        /* X11 不可用则回退 headless（仅导出 PPM） */
        return 0;
    }
    int screen = DefaultScreen(s_dpy);

    /* 读取显示方向补偿开关（Termux-X11 等后端可能翻转）。
     * 默认不翻转（X server 内部像素始终正确，见 x11_read 验证）。
     * 仅当显式设置 VUS_X11_FLIP 时才翻转坐标：
     *   none / 0 / off    不翻转（默认）
     *   v                 垂直翻转
     *   h                 水平翻转
     *   vh / hv           180度翻转
     * 真正的 Termux GL 贴图翻转交给 TERMUX_X11_FORCE_FLIP 处理，
     * 这里保持 X 层干净，避免双重叠加。 */
    s_flip_v = 0;
    s_flip_h = 0;
    {
        const char* fl = getenv("VUS_X11_FLIP");
        if (fl && *fl && !(strcmp(fl, "none") == 0 || strcmp(fl, "0") == 0 || strcmp(fl, "off") == 0))
        {
            if (strchr(fl, 'v') || strchr(fl, 'V')) { s_flip_v = 1; }
            if (strchr(fl, 'h') || strchr(fl, 'H')) { s_flip_h = 1; }
        }
        fprintf(stderr, "[flip] VUS_X11_FLIP='%s' -> flip_v=%d flip_h=%d\n",
                fl ? fl : "(null)", s_flip_v, s_flip_h);
    }

    s_win = XCreateSimpleWindow(s_dpy, RootWindow(s_dpy, screen),
                                0, 0, (unsigned int)width, (unsigned int)height,
                                1, BlackPixel(s_dpy, screen), WhitePixel(s_dpy, screen));
    if (title)
    {
        XStoreName(s_dpy, s_win, title);
    }
    XSelectInput(s_dpy, s_win, ExposureMask | StructureNotifyMask | KeyPressMask |
                                    ButtonPressMask | PointerMotionMask);
    XMapWindow(s_dpy, s_win);

    s_gc = XCreateGC(s_dpy, s_win, 0, NULL);
    s_wm_delete = XInternAtom(s_dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(s_dpy, s_win, &s_wm_delete, 1);

    /* 加载文字渲染字体：
     * 优先 Xft（FontConfig + FreeType，按 UTF-8 显示中英文，自动回退系统中文字体）；
     * Xft 不可用时回退 X11 核心字体（XDrawString，仅 ASCII）。 */
    int screen_for_font = DefaultScreen(s_dpy);
    s_xft = XftDrawCreate(s_dpy, s_win,
                          DefaultVisual(s_dpy, screen_for_font),
                          DefaultColormap(s_dpy, screen_for_font));
    if (s_xft)
    {
        /* FontConfig pattern 候选队列：
         *  - 前置显式 CJK 字体族（Noto CJK / WenQuanYi 等常见名称），XftFontOpenName
         *    按族名精确匹配，命中即含中文字形。
         *  - "sans"/"monospace" 是通用族名，fontconfig 通常命中 DejaVu Sans，
         *    其不含 CJK 字形且 Xft 单字体不做跨文件回退，故仅作兜底。
         *  逐个尝试，加载成功且含中文字形(U+4E2D)者即采用。 */
        const char* candidates[] = {
            "Noto Sans CJK SC-16",
            "Noto Sans CJK-16",
            "Noto Sans SC-16",
            "WenQuanYi Zen Hei-16",
            "Noto Sans CJK TC-16",
            "Noto Sans CJK JP-16",
            "sans-16",
            "sans",
            "monospace-16",
        };
        const int cand_cnt = (int)(sizeof(candidates) / sizeof(candidates[0]));
        FcChar32 codepoint = 0x4E2D; /* "中" */
        for (int i = 0; i < cand_cnt && !s_xftfont; i++)
        {
            XftFont* f = XftFontOpenName(s_dpy, screen_for_font, candidates[i]);
            if (f && f->charset && FcCharSetHasChar(f->charset, codepoint))
            {
                s_xftfont = f;   /* 含中文，采用 */
                break;
            }
            /* 该候选缺中文字形：仍保留第一个能加载的供 ASCII 兜底，继续找中文款 */
            if (f && !s_fallback_font) { s_fallback_font = f; }
            else if (f) { XftFontClose(s_dpy, f); }
        }
        if (!s_xftfont) { s_xftfont = s_fallback_font; s_fallback_font = 0; }
    }
    if (!s_xftfont)
    {
        /* Xft 不可用 → 回退 X 核心字体（ASCII only） */
        if (s_xft) { XftDrawDestroy(s_xft); s_xft = 0; }
        s_xfont = XLoadQueryFont(s_dpy, "fixed");
        if (!s_xfont) { s_xfont = XLoadQueryFont(s_dpy, "6x13"); }
        fprintf(stderr, "[font] Xft 不可用，回退 X11 核心字体（中文乱码）——请确认已安装 xfontconfig + 中文字体\n");
    }
    else
    {
        /* 诊断：检查字体是否含中文字形（U+4E2D "中"）。缺字形时提示装中文 CJK 字体。 */
        FcChar32 codepoint = 0x4E2D;
        int has_cjk = s_xftfont->charset ? FcCharSetHasChar(s_xftfont->charset, codepoint) : 0;
        fprintf(stderr, "[font] Xft 字体已加载 (size=%d, ascent=%d, 含中文字形=%s)\n",
                s_xftfont->height, s_xftfont->ascent, has_cjk ? "是" : "否");
        if (!has_cjk)
        {
            fprintf(stderr, "[font] 警告：当前字体无中文字形，中文将显示为方块/乱码。"
                    "请安装中文字体并刷新字体缓存，例：\n"
                    "  Termux : pkg install font-noto-cjk && fc-cache -f\n"
                    "  Ubuntu : apt install fonts-noto-cjk && fc-cache -f\n");
        }
    }
    s_text_cnt = 0;

#ifdef VUS_GUI_GLES
    /* 尝试底层 GPU 上屏：EGL+GLES 上下文就绪后，redraw 走纹理上屏；
     * 任何一步失败 vus_gles_init 返回 0，回退下方 XImage 软路径。 */
    s_gles = vus_gles_init(s_dpy, s_win, width, height);
    if (s_gles)
    {
        s_running = 1;
        return 0; /* GL 路径定义上不需要 XImage */
    }
#endif

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
    /* G3：32bpp 直配检测（掩码/字节序/水平翻转），命中后 redraw 走按行 memcpy */
    s_fast32 = 0;
    if (s_img && s_img->bits_per_pixel == 32 &&
        s_img->red_mask == 0x00FF0000u && s_img->green_mask == 0x0000FF00u &&
        s_img->blue_mask == 0x000000FFu && !s_flip_h)
    {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        if (s_img->byte_order == LSBFirst) s_fast32 = 1;
#else
        if (s_img->byte_order == MSBFirst) s_fast32 = 1;
#endif
    }
    s_running = 1;
#endif /* VUS_GUI_X11 */
    return 0;
}

/* 平台层文字绘制：X11 可用且已加载 X 字体时，把文字请求入队（redraw 时
 * 用 XDrawString 叠加到窗口），返回 1；否则返回 0，由桥接层回退到
 * GuiLite 帧缓冲绘制（如 headless 导出 PPM）。 */
int vus_gui_platform_draw_text(int x, int y, const char* text, unsigned int color)
{
#ifdef VUS_GUI_X11
    if (!s_dpy || !s_gc || !(s_xftfont || s_xfont) || !text || !*text)
    {
        return 0;
    }
    if (s_text_cnt >= VUS_TEXT_MAX)
    {
        return 0;
    }
    VUS_XText* t = &s_texts[s_text_cnt++];
    t->x = x;
    t->y = y;
    t->color = (unsigned long)(color & 0x00FFFFFF);
    t->text = strdup(text);
    if (!t->text)
    {
        s_text_cnt--;
        return 0;
    }
    return 1;
#else
    (void)x; (void)y; (void)text; (void)color;
    return 0;
#endif
}

void vus_gui_platform_redraw(int width, int height, const unsigned int* fb,
                             int rx1, int ry1, int rx2, int ry2)
{
    if (!fb) { return; }

    /* G8：脏矩形规范化（半开区间，越界裁剪）；空/非法回退整帧（保底正确） */
    if (rx1 < 0) rx1 = 0;
    if (ry1 < 0) ry1 = 0;
    if (rx2 > width) rx2 = width;
    if (ry2 > height) ry2 = height;
    if (rx2 <= rx1 || ry2 <= ry1) { rx1 = 0; ry1 = 0; rx2 = width; ry2 = height; }

    /* 调试模式才导出 PPM 并打印每帧帧缓冲概要，默认关闭（避免高频写盘/日志耗电） */
    if (s_dbg)
    {
        export_ppm("gui_out.ppm", width, height, fb);
        fprintf(stderr, "[redraw] w=%d h=%d dirty=(%d,%d)-(%d,%d) fb[0]=0x%08X fb[%d]=0x%08X\n",
                width, height, rx1, ry1, rx2, ry2, fb[0], width * height - 1,
                fb[(size_t)(width / 2) * (size_t)width + (size_t)(height / 2)]);
    }

#ifdef VUS_GUI_X11
#ifdef VUS_GUI_GLES
    if (s_gles)
    {
        /* 底层 GPU 上屏：G8 增量 —— 帧缓冲脏矩形以 glTexSubImage2D 只上传
         * 子区域，取代整帧纹理上传；文字随后仍在 X11 上层用 Xft 叠加。 */
        vus_gles_redraw(width, height, fb, rx1, ry1, rx2, ry2);
        for (int i = 0; i < s_text_cnt; i++)
        {
            XRenderColor rc;
            rc.red   = (unsigned short)(((s_texts[i].color >> 16) & 0xFF) << 8);
            rc.green = (unsigned short)(((s_texts[i].color >> 8)  & 0xFF) << 8);
            rc.blue  = (unsigned short)((s_texts[i].color & 0xFF) << 8);
            rc.alpha = 0xFFFF;
            XftColor fc;
            if (s_xftfont && s_xft && XftColorAllocValue(
                    s_dpy, DefaultVisual(s_dpy, DefaultScreen(s_dpy)),
                    DefaultColormap(s_dpy, DefaultScreen(s_dpy)), &rc, &fc))
            {
                XftDrawStringUtf8(s_xft, &fc, s_xftfont,
                                  s_texts[i].x, s_texts[i].y + s_xftfont->ascent,
                                  (const FcChar8*)s_texts[i].text,
                                  (int)strlen(s_texts[i].text));
                XftColorFree(s_dpy, DefaultVisual(s_dpy, DefaultScreen(s_dpy)),
                             DefaultColormap(s_dpy, DefaultScreen(s_dpy)), &fc);
            }
        }
        XFlush(s_dpy);
        goto gles_done; /* 跳过下方 XImage 软路径与文字重放 */
    }
#endif
    if (!s_dpy || !s_img) { return; }

    int dw = rx2 - rx1;
    int dh = ry2 - ry1;
    if (s_fast32)
    {
        /* G3 快路径：32bpp 且掩码/字节序与本机帧缓冲一致 → 按行 memcpy。
         * 垂直翻转（VUS_X11_FLIP=v）时行序反转，逐行重排后写入 s_img。 */
        for (int y = ry1; y < ry2; y++)
        {
            int ty = s_flip_v ? (height - 1 - y) : y;
            memcpy(s_img->data + (size_t)ty * (size_t)s_img->bytes_per_line
                                 + (size_t)rx1 * 4u,
                   fb + (size_t)y * (size_t)width + (size_t)rx1,
                   (size_t)dw * 4u);
        }
    }
    else
    {
        /* 通用逐像素路径：Xlib 按 s_img 的 byte_order/bit_order/掩码自动换算
         * 像素值，适配任意 visual/深度/字节序；仅走脏区，支持双向翻转。 */
        for (int y = ry1; y < ry2; y++)
        {
            for (int x = rx1; x < rx2; x++)
            {
                unsigned int px = fb[(size_t)y * (size_t)width + (size_t)x];
                unsigned long val = 0;
                if (s_img->red_mask)
                    val |= ((unsigned long)((px >> 16) & 0xFF) << ffs_pos(s_img->red_mask)) & s_img->red_mask;
                if (s_img->green_mask)
                    val |= ((unsigned long)((px >> 8) & 0xFF) << ffs_pos(s_img->green_mask)) & s_img->green_mask;
                if (s_img->blue_mask)
                    val |= ((unsigned long)(px & 0xFF) << ffs_pos(s_img->blue_mask)) & s_img->blue_mask;
                int tx = s_flip_h ? (width - 1 - x) : x;
                int ty = s_flip_v ? (height - 1 - y) : y;
                XPutPixel(s_img, tx, ty, val);
            }
        }
    }
    /* G8：只把脏矩形对应的 XImage 子区域送窗（src 与 dst 同区域；
     * 翻转补偿在写 s_img 时已按行/像素完成，区域随之映射） */
    {
        int sx = s_flip_h ? (width - rx2) : rx1;
        int sy = s_flip_v ? (height - ry2) : ry1;
        XPutImage(s_dpy, s_win, s_gc, s_img, sx, sy, sx, sy,
                  (unsigned int)dw, (unsigned int)dh);
    }
    /* 在帧缓冲之上重放文字，避免被 XPutImage 覆盖：
     * 优先 Xft 按 UTF-8 绘制（支持中文，XftDrawStringUtf8 内部做字形回退）；
     * Xft 不可用时回退 XDrawString（X 核心字体，仅 ASCII）。 */
    if (s_xftfont && s_xft)
    {
        int ascent = s_xftfont->ascent;
        for (int i = 0; i < s_text_cnt; i++)
        {
            XRenderColor rc;
            rc.red   = (unsigned short)(((s_texts[i].color >> 16) & 0xFF) << 8);
            rc.green = (unsigned short)(((s_texts[i].color >> 8)  & 0xFF) << 8);
            rc.blue  = (unsigned short)((s_texts[i].color & 0xFF) << 8);
            rc.alpha = 0xFFFF;
            XftColor fc;
            if (XftColorAllocValue(s_dpy, DefaultVisual(s_dpy, DefaultScreen(s_dpy)),
                                   DefaultColormap(s_dpy, DefaultScreen(s_dpy)), &rc, &fc))
            {
                XftDrawStringUtf8(s_xft, &fc, s_xftfont,
                                  s_texts[i].x, s_texts[i].y + ascent,
                                  (const FcChar8*)s_texts[i].text,
                                  (int)strlen(s_texts[i].text));
                XftColorFree(s_dpy, DefaultVisual(s_dpy, DefaultScreen(s_dpy)),
                             DefaultColormap(s_dpy, DefaultScreen(s_dpy)), &fc);
            }
        }
    }
    else if (s_xfont)
    {
        int ascent = s_xfont->max_bounds.ascent;
        XSetFont(s_dpy, s_gc, s_xfont->fid);
        for (int i = 0; i < s_text_cnt; i++)
        {
            XSetForeground(s_dpy, s_gc, s_texts[i].color);
            XDrawString(s_dpy, s_win, s_gc, s_texts[i].x,
                        s_texts[i].y + ascent, s_texts[i].text,
                        (int)strlen(s_texts[i].text));
        }
    }
    XFlush(s_dpy);

    /* 文字列表是按帧快照：重放完本帧条目后立即清空（释放 strdup 内存），
     * 下一帧从空表重新入队。否则动画循环中各帧文字无限累积，出现
     * 重叠闪烁/模糊/黑块（缺失字形叠影）。Expose 触发重绘时同理只重放当前帧。 */
    for (int i = 0; i < s_text_cnt; i++)
    {
        free(s_texts[i].text);
        s_texts[i].text = 0;
    }
    s_text_cnt = 0;

#ifdef VUS_GUI_GLES
gles_done: ;
    /* GL 路径跳转来的收尾（复用文字内存清理语义）。 */
#endif
#endif
}

#ifdef VUS_GUI_X11
/* 单条 X 事件处理（阻塞 run 循环与非阻塞 poll 共用） */
static void vus_gui_platform_handle_event(XEvent *ev, int width, int height, const unsigned int* fb)
{
    switch (ev->type)
    {
    case Expose:
        vus_gui_platform_redraw(width, height, fb, 0, 0, width, height);
        break;
    case ClientMessage:
        if ((Atom)ev->xclient.data.l[0] == s_wm_delete)
        {
            s_running = 0;
        }
        break;
    case KeyPress:
        /* 键盘输入：记录按键（可打印字符 + keycode），并派发给桥接层。
         * Esc 键退出事件循环，便于测试；其余按键作为输入继续轮询。 */
        {
            KeySym keysym = XLookupKeysym(&ev->xkey, 0);
            if (keysym == XK_Escape)
            {
                s_running = 0;
                break;
            }
            /* 用 XKB/keysym 把按键转成可打印 UTF-8 字符串（大写/移位已由 keysym 处理） */
            char buf[16] = { 0 };
            const char* symstr = NULL;
            int len = XLookupString(&ev->xkey, buf, sizeof(buf) - 1, NULL, NULL);
            if (len > 0)
            {
                buf[len] = '\0';
                symstr = buf;
            }
            else if (keysym >= XK_space && keysym <= XK_asciitilde)
            {
                /* 单字符合法可打印区（ASCII 打印字符，含空格） */
                char one = (char)(keysym & 0x7F);
                buf[0] = one; buf[1] = '\0';
                symstr = buf;
            }
            vus_gui_platform_emit_key(symstr, (unsigned int)keysym,
                                      (unsigned int)ev->xkey.state);
        }
        break;
    case ButtonPress:
        /* 鼠标/触摸按下：记录坐标供 图形_按钮点击 命中。
         * X 滚轮作为 ButtonPress 的 button 4(上)/5(下) 到达，单独派发滚轮，
         * 不污染普通点击的命中检测。 */
        if (ev->xbutton.button == 4 || ev->xbutton.button == 5)
        {
            int dy = (ev->xbutton.button == 4) ? 1 : -1;
            vus_gui_platform_emit_wheel(ev->xbutton.x, ev->xbutton.y, dy);
            break;
        }
        vus_gui_platform_emit_button(ev->xbutton.x, ev->xbutton.y, ev->xbutton.button);
        vus_gui_platform_emit_click(ev->xbutton.x, ev->xbutton.y);
        break;
    case MotionNotify:
        /* 指针移动：记录鼠标位置（供 图形_鼠标x/y、悬停检测） */
        vus_gui_platform_emit_motion(ev->xmotion.x, ev->xmotion.y, (unsigned int)ev->xmotion.state);
        break;
    default:
        break;
    }
}
#endif /* VUS_GUI_X11 */

/* 非阻塞取事件：处理当前 X 事件队列（含点击/重绘/退出），供轮询式交互模型。
 * 返回不阻塞；处理事件数设上限，避免 Motion 洪泛导致忙等。 */
void vus_gui_platform_poll(int width, int height, const unsigned int* fb)
{
#ifdef VUS_GUI_X11
    if (!s_dpy)
    {
        return;
    }
    int guard = 128;
    while (guard-- > 0 && s_running && XPending(s_dpy))
    {
        XEvent ev;
        XNextEvent(s_dpy, &ev);
        vus_gui_platform_handle_event(&ev, width, height, fb);
    }
    XFlush(s_dpy);
#else
    (void)width; (void)height; (void)fb;
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
        vus_gui_platform_handle_event(&ev, width, height, fb);
    }
    if (s_img)
    {
        XDestroyImage(s_img);
        s_img = 0;
    }
    /* 释放文字请求缓冲 */
    for (int i = 0; i < s_text_cnt; i++)
    {
        free(s_texts[i].text);
        s_texts[i].text = 0;
    }
    s_text_cnt = 0;
    if (s_xftfont)
    {
        XftFontClose(s_dpy, s_xftfont);
        s_xftfont = 0;
    }
    if (s_fallback_font)
    {
        XftFontClose(s_dpy, s_fallback_font);
        s_fallback_font = 0;
    }
    if (s_xft)
    {
        XftDrawDestroy(s_xft);
        s_xft = 0;
    }
    if (s_xfont)
    {
        XFreeFont(s_dpy, s_xfont);
        s_xfont = 0;
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