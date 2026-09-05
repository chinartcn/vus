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

/* 启用 strdup 等 POSIX/GNU 声明（须在含 string.h 前定义） */
#define _GNU_SOURCE

#include "guilite_bridge.h"

/* dlsym 需要 <dlfcn.h> 与链接期 -rdynamic/-ldl（见 src/generator.c GUI 链接参数） */
#include <dlfcn.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* 图形_背景图：PNG 解码用 libpng（链接 -lpng -lz，见 src/generator.c GUI 链接参数） */
#include <png.h>

/* 图形_图片(.gif)/GIF 动画：gifdec 单驱动 GIF 解码（头在 rt/gifdec 子目录，
 * 已由 Makefile 编译进 libvus_rt.a，这里仅用到它的头，实现无需重复编译）。 */
#include "gifdec/gifdec.h"

/* 图形_图片(.svg)：nanosvg 纯 C 单头解析 + 栅格化。
 * NANOSVG_IMPLEMENTATION / NANOSVGRAST_IMPLEMENTATION 各只能定义一次，
 * 只在本文件展开一次实现，避免与其它源文件的宏冲突。 */
#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvgrast.h"

/* 图形_字体：外部 TTF/OTF 字体加载与栅格化用 FreeType（链接 -lfreetype，
 * 见 src/generator.c GUI 链接参数）。 */
#include <ft2build.h>
#include FT_FREETYPE_H

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
    int first = 1;
    while (name[i] && o < out_size - 1)
    {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x80 && (isalnum(c) || c == '_'))
        {
            if (first && c >= '0' && c <= '9')
            {
                /* 数字开头的名字加下划线前缀（与编译器 gen_sanitize_name 逐位一致，
                 * 否则超长/数字开头名反查符号会失配）。 */
                if (o + 1 < out_size) out[o++] = '_';
            }
            out[o++] = (char)c;
            first = 0;
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
            first = 0;
        }
        else
        {
            if (o + 1 < out_size) out[o++] = '_';
            first = 0;
            i++;
        }
    }
    out[o] = '\0';
}

/* 最近一次点击坐标（-1 = 尚无点击），供命中检测使用。 */
static int s_click_x = -1;
static int s_click_y = -1;
/* 点击消费标记：棋盘格等需要“仅在本次点击生效一次”的控件读取后置位，
 * 避免主循环每帧读到同一点击而反复切换状态。poll / emit 时清零。 */
static int s_click_consumed = 0;

/* ============ 阶段4：X11 多输入状态 ============ */
static int       s_valid_key    = 0;   /* 是否已有按键记录 */
static char      s_last_key[16] = {0}; /* 最近一次可打印按键 UTF-8 字符串 */
static unsigned  s_last_keycode = 0;   /* 最近一次按键 keycode（KeySym） */
static unsigned  s_last_keymask = 0;   /* 最近一次按键修饰键掩码 */
static int       s_motion_valid = 0;   /* 是否已有指针移动记录 */
static int       s_mouse_x = -1;       /* 最近指针 X */
static int       s_mouse_y = -1;       /* 最近指针 Y */
static unsigned  s_mouse_mask = 0;     /* 指针移动时修饰键掩码 */
static int       s_wheel_dy  = 0;      /* 滚轮增量：+1 上 / -1 下，0 无滚动 */
static int       s_wheel_x = -1, s_wheel_y = -1; /* 滚轮时机标 */
static int       s_pressed[8] = {0};   /* 各鼠标键按下计数（索引=button 1..4/5），仅 X11 真实事件更新 */

/* ============ 键盘回调约定名：命中后回调 <fn>(字符, keycode) ============ */
#define VUS_CALLBACK_KEY "事件_按键"

/* ============ 阶段B：样式/主题模板 ============ */
/* 全局主题色（0xRRGGBB）。默认值对应现有控件配色，保证既有示例不改变外观；
 * 调用 图形_主题(...) 可整体切换配色风格。 */
typedef struct {
    unsigned int bg;         /* 控件背景/填充色 */
    unsigned int border;     /* 边框色 */
    unsigned int highlight;  /* 高亮/选中色（按钮底、复选框勾选） */
    unsigned int fg;         /* 正文/文字主色 */
    unsigned int text;       /* 控件文字色（按钮白字） */
} VusGuiTheme;
static VusGuiTheme s_theme = { 0xF5F5F5, 0x333333, 0x3399CC, 0x000000, 0xFFFFFF };

/* 全局控件圆角半径（像素），0=直角。由 图形_外观 设置，供 图形_按钮 等控件复用。 */
static int s_global_radius = 0;

/* ============ 阶段B：样式/主题模板 —— API ============ */

/* 图形_主题(背景, 边框, 高亮, 正文, 文字)：设置全局主题色（0xRRGGBB）。
 * 任一传 -1 表示保持该通道不变。返回 "1"。 */
VusString* vus_gui_set_theme(int bg, int border, int highlight, int fg, int text)
{
    if (!s_initialized) return vus_string_new("0");
    if (bg       >= 0) s_theme.bg       = (unsigned int)bg;
    if (border   >= 0) s_theme.border   = (unsigned int)border;
    if (highlight>= 0) s_theme.highlight= (unsigned int)highlight;
    if (fg       >= 0) s_theme.fg       = (unsigned int)fg;
    if (text     >= 0) s_theme.text     = (unsigned int)text;
    return vus_string_new("1");
}

/* ===== 统一控件表（阶段3：控件库） =====
 * 按钮/标签/文本框/复选框/进度条/列表/画布 共用同一张表与同一套命中检测。 */
#define VUS_CTRL_MAX    128
#define VUS_LIST_LINES  32
#define VUS_LINE_MAX    96
typedef enum {
    CTRL_BUTTON, CTRL_LABEL, CTRL_TEXTBOX,
    CTRL_CHECKBOX, CTRL_PROGRESS, CTRL_LIST, CTRL_CANVAS,
    CTRL_SLIDER, CTRL_SWITCH, CTRL_SPIN, CTRL_RADIO
} VusCtrlType;
typedef struct {
    char name[64];
    VusCtrlType type;
    int x, y, w, h;
    int row_h;                          /* 列表每行像素高 */
    char lines[VUS_LIST_LINES][VUS_LINE_MAX]; /* 列表每行文本 */
    int line_cnt;
    int checked;                        /* 复选框 */
    int touched;                        /* 复选框：是否已被点击过（此后内部 checked 为权威） */
    int progress;                       /* 进度条 0-100 */
    int rel_x, rel_y;                   /* 画布最近命中相对坐标 */
    int slider_value, slider_min, slider_max; /* 滑块：当前值/最小值/最大值 */
    int switch_state;                   /* 开关：1 开 / 0 关 */
    int spin_value, spin_step;          /* 微调：当前值/步长 */
    int radio_sel, radio_n;             /* 单选：当前选中索引/选项个数 */
    char radio_opts[16][40];            /* 单选每项文本 */
} VusControl;
static VusControl s_ctrls[VUS_CTRL_MAX];
static int        s_ctrl_cnt = 0;

/* 查找控件：未找到返回 NULL。 */
static VusControl* find_ctrl(const char* name)
{
    if (!name) return NULL;
    for (int i = 0; i < s_ctrl_cnt; i++)
        if (strcmp(s_ctrls[i].name, name) == 0) return &s_ctrls[i];
    return NULL;
}

/* 登记控件：同名复用（更新类型/几何），否则追加。满表返回 NULL。 */
static VusControl* register_ctrl(const char* name, VusCtrlType type)
{
    if (!name || !*name) return NULL;
    VusControl* c = find_ctrl(name);
    if (c) { c->type = type; return c; }
    if (s_ctrl_cnt >= VUS_CTRL_MAX) return NULL;
    VusControl* n = &s_ctrls[s_ctrl_cnt++];
    memset(n, 0, sizeof(*n));
    strncpy(n->name, name, sizeof(n->name) - 1);
    n->name[sizeof(n->name) - 1] = '\0';
    n->type = type;
    return n;
}

/* 点是否落在控件矩形内。 */
static int point_in(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

/* ============ 阶段6：脏标记（按需刷新 / 省电） ============
 * 任何绘制/控件写入帧缓冲或文字队列后置位，redraw 仅在置位时真正上屏并
 * 清零；否则短路返回。避免主循环每帧“无条件刷新”（即使画面未变化）导致
 * CPU 持续工作、无法休眠，进而被 Android 判定异常耗电而杀进程。
 * G8：脏区升级为矩形 —— 绘制写入的像素包围盒（半开区间 [x1,x2)×[y1,y2)），
 * redraw 只增量提交/上屏这块区域，不再整帧 memcpy。像素型绘制
 * （write_scrolled_pixel）自动归并；矩形型绘制在调用点 mark_dirty_area。
 */
static int  s_dirty = 1; /* 初始为 1，保证首帧必刷 */
static int  s_dirty_x1 = 0, s_dirty_y1 = 0, s_dirty_x2 = 0, s_dirty_y2 = 0;
static void vus_gui_mark_dirty_area(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    int x2 = x + w, y2 = y + h;
    if (!s_dirty)
    {
        s_dirty = 1;
        s_dirty_x1 = x; s_dirty_y1 = y; s_dirty_x2 = x2; s_dirty_y2 = y2;
    }
    else
    {
        if (x < s_dirty_x1) s_dirty_x1 = x;
        if (y < s_dirty_y1) s_dirty_y1 = y;
        if (x2 > s_dirty_x2) s_dirty_x2 = x2;
        if (y2 > s_dirty_y2) s_dirty_y2 = y2;
    }
}
static void vus_gui_mark_dirty(void)
{
    /* 全屏脏：以当前帧缓冲尺寸归并 */
    vus_gui_mark_dirty_area(0, 0, vus_gui_surface_width(), vus_gui_surface_height());
}

/* ============ 阶段F：外部字体（FreeType） ============
 * 图形_字体_加载("路径", 字号)：用 FreeType 加载外部 TTF/OTF 字体到全局活动字体，
 * 后续 draw_text/图形_MD 的文字栅格化为字形写进 ARGB 帧缓冲。 */
static FT_Library   s_ftlib = 0;
static FT_Face      s_ftface = 0;
static int          s_ft_size = 16;   /* 当前字号（像素） */
static int          s_ft_loaded = 0;

/* ============ G1：FreeType 字形缓存（(码点,字号) → 渲染位图 + 度量） ============
 * 原实现每帧每字符都 FT_Load_Char(FT_LOAD_RENDER) 重栅格化；这里把栅格化
 * 结果按 (字号, 码点) 键缓存为拷贝（glyph slot 缓冲会被后续 load 覆盖），
 * 下次同字型直接复用位图与 advance。命中更新 last_used，满员淘汰最久未用。 */
#define VUS_GLYPH_CACHE_MAX 512
typedef struct {
    uint32_t       key;        /* (s_ft_size << 21) | (cp & 0x1FFFFF) */
    unsigned char* buf;        /* 位图像素拷贝（宽*rows, pitch 行距） */
    int            width, rows, pitch;
    int            left, top;  /* bitmap_left / bitmap_top */
    int            advance;    /* advance.x >> 6 */
    int            last_used;
} VusGlyphSlot;
static VusGlyphSlot s_glyphs[VUS_GLYPH_CACHE_MAX];
static int s_glyph_count = 0;
static int s_glyph_clock = 0;

static void vus_glyph_cache_clear(void)
{
    for (int i = 0; i < s_glyph_count; i++) { free(s_glyphs[i].buf); s_glyphs[i].buf = 0; }
    s_glyph_count = 0;
}

/* 取字形缓存条目：命中直接返回；未命中用当前活动字体栅格化（拷贝位图）。 */
static VusGlyphSlot* vus_glyph_get(unsigned int cp)
{
    if (!s_ftface || !s_ftlib) return 0;
    uint32_t key = ((uint32_t)s_ft_size << 21) | (cp & 0x1FFFFFu);
    VusGlyphSlot* evict = 0;
    int min_use = 0x7FFFFFFF;
    for (int i = 0; i < s_glyph_count; i++)
    {
        if (s_glyphs[i].key == key)
        {
            s_glyphs[i].last_used = ++s_glyph_clock;
            return &s_glyphs[i];
        }
        if (s_glyphs[i].last_used < min_use) { min_use = s_glyphs[i].last_used; evict = &s_glyphs[i]; }
    }
    if (s_glyph_count >= VUS_GLYPH_CACHE_MAX)
    {
        /* 满员：淘汰最久未用 */
        free(evict->buf); evict->buf = 0;
        *evict = s_glyphs[s_glyph_count - 1];
        s_glyph_count--;
    }
    if (FT_Load_Char(s_ftface, cp, FT_LOAD_RENDER) != 0) return 0;
    FT_GlyphSlot g = s_ftface->glyph;
    FT_Bitmap* bmp = &g->bitmap;
    VusGlyphSlot* sl = &s_glyphs[s_glyph_count];
    size_t total = (size_t)bmp->rows * (size_t)(bmp->pitch > 0 ? bmp->pitch : bmp->width);
    unsigned char* copy = (unsigned char*)malloc(total ? total : 1);
    if (!copy) return 0;
    if (total) memcpy(copy, bmp->buffer, total);
    sl->key = key;
    sl->buf = copy;
    sl->width = bmp->width;
    sl->rows = bmp->rows;
    sl->pitch = bmp->pitch > 0 ? bmp->pitch : bmp->width;
    sl->left = g->bitmap_left;
    sl->top = g->bitmap_top;
    sl->advance = (int)(g->advance.x >> 6);
    sl->last_used = ++s_glyph_clock;
    s_glyph_count++;
    return sl;
}

/* ============ G2：图片解码缓存（路径 → RGBA 像素 LRU） ============
 * PNG/SVG 每次绘制都整文件重解码；这里把解码结果按路径缓存
 * （LRU，上限 VUS_IMG_CACHE_MAX 条），重复绘制/滚动复用时直接复用像素，
 * 只保留一次解码成本。GIF/动画帧不走此缓存（由播放器槽表管理）。 */
#define VUS_IMG_CACHE_MAX 16
typedef struct {
    char*         path;
    unsigned char* rgba;
    int           w, h;
    int           last_used;
} VusImgSlot;
static VusImgSlot s_imgs[VUS_IMG_CACHE_MAX];
static int s_img_count = 0;

static const unsigned char* vus_img_cache_get(const char* path, int* w, int* h)
{
    if (!path) return 0;
    for (int i = 0; i < s_img_count; i++)
    {
        if (strcmp(s_imgs[i].path, path) == 0)
        {
            s_imgs[i].last_used = ++s_glyph_clock;
            if (w) *w = s_imgs[i].w;
            if (h) *h = s_imgs[i].h;
            return s_imgs[i].rgba;
        }
    }
    return 0;
}

/* 接管解码产出的 rgba（所有权归缓存）；满员淘汰最久未用条目。 */
static void vus_img_cache_put(const char* path, unsigned char* rgba, int w, int h)
{
    if (!path || !rgba || w <= 0 || h <= 0) return;
    if (s_img_count >= VUS_IMG_CACHE_MAX)
    {
        int min_use = 0x7FFFFFFF, ev = 0;
        for (int i = 0; i < s_img_count; i++)
            if (s_imgs[i].last_used < min_use) { min_use = s_imgs[i].last_used; ev = i; }
        free(s_imgs[ev].path); free(s_imgs[ev].rgba);
        s_imgs[ev] = s_imgs[--s_img_count];
    }
    s_imgs[s_img_count].path = strdup(path);
    if (!s_imgs[s_img_count].path) { free(rgba); return; }
    s_imgs[s_img_count].rgba = rgba;
    s_imgs[s_img_count].w = w;
    s_imgs[s_img_count].h = h;
    s_imgs[s_img_count].last_used = ++s_glyph_clock;
    s_img_count++;
}

/* PNG/SVG 解码实现定义在后方（阶段C/G），此处供带缓存辅助前置引用。 */
static int png_load_rgba(const char* path, int* pw, int* ph, unsigned char** data);
static int svg_load_rgba(const char* path, int* pw, int* ph, unsigned char** data);

/* PNG 解码 + LRU 缓存：命中直接返回缓存指针（调用方不得 free）；
 * 未命中解码一次并接管入缓存。失败返回 NULL。 */
static const unsigned char* png_cached_rgba(const char* path, int* w, int* h)
{
    const unsigned char* hit = vus_img_cache_get(path, w, h);
    if (hit) return hit;
    int sw = 0, sh = 0;
    const unsigned char* rgba = NULL;
    if (!png_load_rgba(path, &sw, &sh, (unsigned char**)&rgba) || !rgba || sw <= 0 || sh <= 0)
    {
        free((void*)rgba);
        return 0;
    }
    vus_img_cache_put(path, (unsigned char*)rgba, sw, sh);
    if (w) *w = sw;
    if (h) *h = sh;
    return rgba;   /* 所有权已移交缓存 */
}

/* SVG 解码 + LRU 缓存（同上约定）。 */
static const unsigned char* svg_cached_rgba(const char* path, int* w, int* h)
{
    const unsigned char* hit = vus_img_cache_get(path, w, h);
    if (hit) return hit;
    int sw = 0, sh = 0;
    const unsigned char* rgba = NULL;
    if (!svg_load_rgba(path, &sw, &sh, (unsigned char**)&rgba) || !rgba || sw <= 0 || sh <= 0)
    {
        free((void*)rgba);
        return 0;
    }
    vus_img_cache_put(path, (unsigned char*)rgba, sw, sh);
    if (w) *w = sw;
    if (h) *h = sh;
    return rgba;   /* 所有权已移交缓存 */
}

/* ===== 阶段D前置：滚动容器表（画线/MD 平移裁剪用到） ===== */
#define VUS_SCROLL_MAX 16
typedef struct {
    char name[64];
    int x, y, w, h, content_h;
    int offset;
} VusScroll;
static VusScroll  s_scrolls[VUS_SCROLL_MAX];
static int        s_scroll_cnt = 0;
static VusScroll* s_act_scroll = NULL;   /* 当前开启平移/裁剪的活动容器 */
static VusScroll* find_scroll(const char* name);

/* 帧缓冲像素写入（应用滚动平移 + 裁剪），定义在阶段D，此处供字体渲染前置引用。 */
static void write_scrolled_pixel(int x, int y, unsigned int argb);

/* RGBA 源像素与目标 ARGB 的 alpha over 合成，返回 ARGB。 */
static unsigned int argb_alpha_over(unsigned int dst, int r, int g, int b, int a)
{
    if (a <= 0) return dst;
    if (a >= 255) return 0xFF000000u | ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
    int inv = 255 - a;
    unsigned int nr = (((unsigned)r * a) + ((((dst >> 16) & 0xFF)) * inv)) / 255;
    unsigned int ng = (((unsigned)g * a) + ((((dst >> 8) & 0xFF)) * inv)) / 255;
    unsigned int nb = (((unsigned)b * a) + ((dst & 0xFF) * inv)) / 255;
    return 0xFF000000u | (nr << 16) | (ng << 8) | nb;
}

/* UTF-8 解码单个码点：返回码点，*adv 为该字符字节数（<=4）。非法字节按 0xFFFD。 */
static unsigned int ft_utf8_next(const char* s, int* adv)
{
    unsigned char c = (unsigned char)*s;
    *adv = 1;
    if (c < 0x80) return c;
    int n;
    unsigned int cp;
    if ((c & 0xE0) == 0xC0)      { cp = c & 0x1F; n = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
    else return 0xFFFD;
    for (int i = 1; i < n; i++)
    {
        if ((c = (unsigned char)s[i]) < 0x80 || c >= 0xC0) return 0xFFFD;
        cp = (cp << 6) | (unsigned int)(c & 0x3F);
    }
    *adv = n;
    return cp;
}

/* 用当前外部字体栅格化文本到帧缓冲；逐字形取位图灰度，经 write_scrolled_pixel
 * 写像素（自动滚动平移/裁剪），颜色用给定 ARGB。返回 1（已绘制）或 0（无字体）。
 * G1：字形从 LRU 缓存取（命中复用位图与度量，不再反复 FT_Load_Char 重栅格化）。 */
static void vus_ft_draw_text(int x, int y, const char* text, unsigned int argb)
{
    if (!text || !s_ftface || !s_ftlib) return;
    int pen_x = x;
    int line_y = y;
    const char* p = text;
    while (*p)
    {
        if ((unsigned char)*p == '\n')
        {
            line_y += s_ft_size + 4;
            pen_x = x;
            p++;
            continue;
        }
        int adv;
        unsigned int cp = ft_utf8_next(p, &adv);
        p += adv;
        VusGlyphSlot* g = vus_glyph_get(cp);
        if (!g) { pen_x += s_ft_size; continue; }
        int gx = pen_x + g->left;
        int gy = line_y + s_ft_size - g->top;
        for (int row = 0; row < g->rows; row++)
        {
            for (int col = 0; col < g->width; col++)
            {
                unsigned char a = g->buf[row * g->pitch + col];
                if (a == 0) continue;
                unsigned int out;
                if (a >= 255)
                    out = 0xFF000000u | (argb & 0x00FFFFFFu);
                else
                    out = argb_alpha_over(0x00000000u, (argb >> 16) & 0xFF,
                                          (argb >> 8) & 0xFF, argb & 0xFF, a);
                write_scrolled_pixel(gx + col, gy + row, out);
            }
        }
        pen_x += g->advance;
    }
}

/* 图形_字体_加载：加载外部字体并设置字号。 */
VusString* vus_gui_font(const char* path, int size_px)
{
    if (!path || !*path || size_px < 4 || size_px > 256)
    {
        return vus_string_new("0");
    }
    if (!s_ftlib)
    {
        if (FT_Init_FreeType(&s_ftlib) != 0) return vus_string_new("0");
    }
    if (s_ftface) { FT_Done_Face(s_ftface); s_ftface = 0; }
    if (FT_New_Face(s_ftlib, path, 0, &s_ftface) != 0)
    {
        s_ft_loaded = 0;
        return vus_string_new("0");
    }
    FT_Set_Pixel_Sizes(s_ftface, 0, (unsigned)size_px);
    /* 换字体（或同字体重建）：字形度量/位图随 face 变化，缓存全部作废 */
    vus_glyph_cache_clear();
    s_ft_size = size_px;
    s_ft_loaded = 1;
    return vus_string_new("1");
}

/* 查询外部字体是否已加载：返回 "1"/"0"。 */
VusString* vus_gui_font_loaded(void)
{
    return vus_string_new(s_ft_loaded ? "1" : "0");
}

/* ============ 阶段E：多页导航（页面栈） ============
 * 页面概念：脚本对每页定义约定式函数 `页_<名>()`，编译为 `vus_页_<sanitized名>`
 * 全局符号（配合语言 导入 即可接入外部 .vus 页面）。这里维护一个页面栈，
 * 切换/绘制通过 dlsym 反查当前页函数并调用（与 事件_点击 回调同一调用约定：
 * VUS 函数 `X` -> `void vus_<sanitized X>(void*)`，其 void* 为 VusString**，[0] 返回槽）。 */

#define VUS_PAGE_STACK_MAX 32

/* 页面栈：每层登记页名（strdup）；s_page_top 为栈顶索引，-1 表示空栈。 */
static char* s_pages[VUS_PAGE_STACK_MAX];
static int   s_page_top = -1;

/* 页函数约定名前缀：当前页 `<名>` 对应函数 `页_<名>`。 */
#define VUS_PAGE_FN_PREFIX "页_"

/* 页面名深拷贝：避免依赖 strdup 的 POSIX 特性宏声明。 */
static char* page_dup(const char* s)
{
    if (!s) return 0;
    size_t n = strlen(s) + 1;
    char* c = (char*)malloc(n);
    if (c) memcpy(c, s, n);
    return c;
}

/* 依据当前栈顶页名，构造其对应编译符号名就地写入 sym。
 * 返回 0 表示无当前页可查；否则返回 1 并把完整符号名写入 sym。 */
static int page_current_symbol(char* sym, size_t sym_size)
{
    if (s_page_top < 0 || !s_pages[s_page_top]) return 0;
    /* name 缓冲区须足够容纳「页_」前缀+页名，避免提前截断：编译器对完整函数名
     * sanitize，若这里先把名字截短，会与编译器产生的符号不一致导致 dlsym 失败。
     * san 保持与编译器 gen_function 的 san[256] 相同，超长符号两端在同一点截断，
     * 从而符号保持一致。 */
    char name[512];
    char san[256];
    snprintf(name, sizeof(name), VUS_PAGE_FN_PREFIX "%s", s_pages[s_page_top]);
    vus_gui_sanitize_name(name, san, sizeof(san));
    snprintf(sym, sym_size, "vus_%s", san);
    return 1;
}

/* 图形_页面_打开(名)：把名为"名"的页置为当前页。
 * 已在栈中则弹出其上方所有层（回到该页）；否则压入栈顶。返回 "1"/"0"。 */
VusString* vus_gui_page_open(const char* name)
{
    if (!name || !*name) return vus_string_new("0");
    /* 已存在：弹出该页及其上方的所有层，令其成为当前页 */
    for (int i = 0; i <= s_page_top; i++)
    {
        if (strcmp(s_pages[i], name) == 0)
        {
            for (int j = s_page_top; j > i; j--) { free(s_pages[j]); s_pages[j] = 0; }
            s_page_top = i;
            return vus_string_new("1");
        }
    }
    /* 不存在：压栈（超上限则忽略，返回失败） */
    if (s_page_top + 1 >= VUS_PAGE_STACK_MAX) return vus_string_new("0");
    char* copy = page_dup(name);
    if (!copy) return vus_string_new("0");
    s_page_top++;
    s_pages[s_page_top] = copy;
    return vus_string_new("1");
}

/* 图形_页面_返回()：弹栈回到上一页。有上一页返回 "1"；已在首页返回 "0"。 */
VusString* vus_gui_page_back(void)
{
    if (s_page_top <= 0) return vus_string_new("0");
    free(s_pages[s_page_top]);
    s_pages[s_page_top] = 0;
    s_page_top--;
    return vus_string_new("1");
}

/* 图形_页面_当前()：返回当前页名字符串；无页返回空串。 */
VusString* vus_gui_page_current(void)
{
    if (s_page_top < 0 || !s_pages[s_page_top]) return vus_string_new("");
    return vus_string_new(s_pages[s_page_top]);
}

/* 图形_页面_绘制()：dlsym 当前页对应 `页_<名>` 函数并调用（无参绘制）。
 * 找到并调用返回 "1"；无当前页或函数缺失返回 "0"（不崩溃）。 */
VusString* vus_gui_page_draw(void)
{
    if (s_page_top < 0) return vus_string_new("0");
    char sym[512];
    if (!page_current_symbol(sym, sizeof(sym))) return vus_string_new("0");
    void (*fn)(void*) = (void(*)(void*))dlsym(RTLD_DEFAULT, sym);
    if (!fn) return vus_string_new("0");
    VusString* args[1] = { 0 }; /* 仅返回槽，无参数 */
    fn(args);
    return vus_string_new("1");
}

/* 文本绘制辅助：外部字体已加载时优先 FreeType 栅格化；否则优先 X11，失败回退 8x8。 */
static void draw_text_xy(int x, int y, const char* text, unsigned int rgb)
{
    if (!text) return;
    unsigned int argb = argb_from_rgb(rgb);
    if (s_ft_loaded && s_ftface)
    {
        /* 滚动容器内：垂直平移后绘制。FreeType 路径经 write_scrolled_pixel 会二次平移，
         * 故这里统一采用「内容坐标 + write_scrolled_pixel 平移」，不再预减 offset。 */
        vus_ft_draw_text(x, y, text, argb);
        return;
    }
    if (s_act_scroll) y -= s_act_scroll->offset;
    if (vus_gui_platform_draw_text(x, y, text, rgb) != 1)
        vus_gui_surface_draw_text(x, y, text, argb);
}

/* ============ 阶段D：Markdown 最小集 / 画线增强 / 滚动容器 ============ */

/* 直接写 ARGB 帧缓冲：应用滚动平移 + 裁剪后写像素。画线/滚动容器像素级写入
 * 统一走此入口，保证在滚动容器内坐标正确平移且超可视区部分被裁掉。 */
static void write_scrolled_pixel(int x, int y, unsigned int argb)
{
    if (!s_initialized) return;
    if (s_act_scroll)
    {
        y -= s_act_scroll->offset;
        if (x < s_act_scroll->x || x >= s_act_scroll->x + s_act_scroll->w) return;
        if (y < s_act_scroll->y || y >= s_act_scroll->y + s_act_scroll->h) return;
    }
    unsigned int* fb = vus_gui_surface_backbuffer();
    if (!fb) return;
    int W = vus_gui_surface_width();
    int H = vus_gui_surface_height();
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    fb[y * W + x] = argb;
    /* G8：像素型绘制的脏区在最终写像素处归并（滚动平移/裁剪后即为实际落点） */
    vus_gui_mark_dirty_area(x, y, 1, 1);
}

/* 读取 ARGB 像素（同上平移/裁剪语义）：供 PNG alpha 合成读当前底色。
 * 无活动容器/越界返回 0。读后台缓冲（绘制累积在此）。 */
static unsigned int read_scrolled_pixel(int x, int y)
{
    if (!s_initialized) return 0;
    if (s_act_scroll)
    {
        y -= s_act_scroll->offset;
        if (x < s_act_scroll->x || x >= s_act_scroll->x + s_act_scroll->w) return 0;
        if (y < s_act_scroll->y || y >= s_act_scroll->y + s_act_scroll->h) return 0;
    }
    unsigned int* fb = vus_gui_surface_backbuffer();
    if (!fb) return 0;
    int W = vus_gui_surface_width();
    int H = vus_gui_surface_height();
    if (x < 0 || y < 0 || x >= W || y >= H) return 0;
    return fb[y * W + x];
}

/* ============ 阶段I 前置：像素级圆角 / 圆 / 圆弧绘制原语 ============
 * 全部经 write_scrolled_pixel 逐像素写入（自动应用滚动平移 + 裁剪），
 * 供阶段I公开函数与 图形_按钮 的圆角外观复用。 */

/* 判断像素是否落在圆角矩形 (x,y,w,h) 内部（r 为四角圆角半径，像素）。
 * r<=0 时退化为普通矩形判定。 */
static int in_round_rect(int px, int py, int x, int y, int w, int h, int r)
{
    if (px < x || px >= x + w || py < y || py >= y + h) return 0;
    if (r <= 0) return 1;
    int l = px - x, rt = x + w - 1 - px, t = py - y, b = y + h - 1 - py;
    int cap = (r - 1) * (r - 1);
    if (l < r && t < r)
        if ((r - 1 - l) * (r - 1 - l) + (r - 1 - t) * (r - 1 - t) > cap) return 0;
    if (l < r && b < r)
        if ((r - 1 - l) * (r - 1 - l) + (r - 1 - b) * (r - 1 - b) > cap) return 0;
    if (rt < r && t < r)
        if ((r - 1 - rt) * (r - 1 - rt) + (r - 1 - t) * (r - 1 - t) > cap) return 0;
    if (rt < r && b < r)
        if ((r - 1 - rt) * (r - 1 - rt) + (r - 1 - b) * (r - 1 - b) > cap) return 0;
    return 1;
}

/* 圆角实心填充：遍历本矩形逐像素判定并写入。 */
static void fill_round_rect_px(int x, int y, int w, int h, int r, unsigned int argb)
{
    for (int py = y; py < y + h; py++)
        for (int px = x; px < x + w; px++)
            if (in_round_rect(px, py, x, y, w, h, r))
                write_scrolled_pixel(px, py, argb);
}

/* 圆角外框（1px）：外圆角矩形减去向内缩 1 的内圆角矩形得到边框。 */
static void round_rect_outline_px(int x, int y, int w, int h, int r, unsigned int argb)
{
    if (w <= 0 || h <= 0) return;
    int ir = (r - 1 > 0) ? r - 1 : 0;
    for (int py = y; py < y + h; py++)
        for (int px = x; px < x + w; px++)
            if (in_round_rect(px, py, x, y, w, h, r) &&
                !in_round_rect(px, py, x + 1, y + 1, w - 2, h - 2, ir))
                write_scrolled_pixel(px, py, argb);
}

/* 实心圆。 */
static void fill_circle_px(int cx, int cy, int r, unsigned int argb)
{
    int r2 = r * r;
    for (int py = cy - r; py <= cy + r; py++)
        for (int px = cx - r; px <= cx + r; px++)
        {
            int dx = px - cx, dy = py - cy;
            if (dx * dx + dy * dy <= r2) write_scrolled_pixel(px, py, argb);
        }
}

/* 圆外框（半径 ~1px 环带）。 */
static void draw_circle_px(int cx, int cy, int r, unsigned int argb)
{
    if (r <= 0) { write_scrolled_pixel(cx, cy, argb); return; }
    int r2 = r * r, r1 = (r - 1) * (r - 1);
    for (int py = cy - r; py <= cy + r; py++)
        for (int px = cx - r; px <= cx + r; px++)
        {
            int dx = px - cx, dy = py - cy;
            int dd = dx * dx + dy * dy;
            if (dd <= r2 && dd >= r1) write_scrolled_pixel(px, py, argb);
        }
}

/* 圆弧（线宽 1）：起始角度 0=正东(3点钟)顺时针，跨角度可为负（逆时针）。
 * 逐角度落点（1° 步进），y 用 cy + r*sin（屏幕坐标 y 向下 → 顺时针）。 */
static void draw_arc_px(int cx, int cy, int r, int start_deg, int sweep_deg, unsigned int argb)
{
    if (r <= 0) { write_scrolled_pixel(cx, cy, argb); return; }
    int n = (sweep_deg >= 0) ? sweep_deg : -sweep_deg;
    int step = (sweep_deg >= 0) ? 1 : -1;
    if (n > 7200) n = 7200; /* 防御异常输入 */
    for (int k = 0; k <= n; k++)
    {
        double a = (double)(start_deg + step * k) * 3.14159265358979323846 / 180.0;
        int px = cx + (int)lround(r * cos(a));
        int py = cy + (int)lround(r * sin(a));
        write_scrolled_pixel(px, py, argb);
    }
}

/* 像素级 Bresenham 画线（经 write_scrolled_pixel），供 draw_line_ex 与箭头复用。 */
static void pixel_line_ex(int x1, int y1, int x2, int y2, unsigned int argb,
                          int wpx, int dashed)
{
    int dx = (x2 > x1) ? (x2 - x1) : (x1 - x2);
    int sx = (x1 < x2) ? 1 : -1;
    int dy = (y2 > y1) ? -(y2 - y1) : -(y1 - y2);
    int sy = (y1 < y2) ? 1 : -1;
    int err = dx + dy;
    int px = x1, py = y1;
    const int dash_on = 6, dash_off = 4;
    int step = 0;
    int half = wpx / 2;
    for (;;)
    {
        if (!dashed || (step % (dash_on + dash_off)) < dash_on)
        {
            for (int ox = -half; ox <= half; ox++)
                for (int oy = -half; oy <= half; oy++)
                    write_scrolled_pixel(px + ox, py + oy, argb);
        }
        if (px == x2 && py == y2) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; px += sx; }
        if (e2 <= dx) { err += dx; py += sy; }
        step++;
    }
}

/* find_scroll 实现（前置声明见阶段D前置）。 */
static VusScroll* find_scroll(const char* name)
{
    if (!name) return NULL;
    for (int i = 0; i < s_scroll_cnt; i++)
        if (strcmp(s_scrolls[i].name, name) == 0) return &s_scrolls[i];
    return NULL;
}

/* 滚动容器：登记可视区 + 滚动范围，并设为活动容器。 */
VusString* vus_gui_scroll_begin(const char* name, int x, int y, int w, int h, int content_h)
{
    if (!s_initialized || !name || !*name) return vus_string_new("0");
    VusScroll* s = find_scroll(name);
    if (!s)
    {
        if (s_scroll_cnt >= VUS_SCROLL_MAX) return vus_string_new("0");
        s = &s_scrolls[s_scroll_cnt++];
        memset(s, 0, sizeof(*s));
        strncpy(s->name, name, sizeof(s->name) - 1);
    }
    s->x = x; s->y = y; s->w = w; s->h = h; s->content_h = content_h;
    if (s->offset > content_h - h) s->offset = content_h - h;
    if (s->offset < 0) s->offset = 0;
    s_act_scroll = s;   /* 后续绘制（画线/MD分片）在容器内平移裁剪 */
    return vus_string_new("1");
}

VusString* vus_gui_scroll_offset(const char* name)
{
    if (!s_initialized) return vus_string_new("0");
    VusScroll* s = find_scroll(name);
    char buf[32];
    if (!s) return vus_string_new("0");
    snprintf(buf, sizeof(buf), "%d", s->offset);
    return vus_string_new(buf);
}

VusString* vus_gui_scroll_delta(const char* name, int dy)
{
    if (!s_initialized) return vus_string_new("0");
    VusScroll* s = find_scroll(name);
    if (!s) return vus_string_new("0");
    s->offset += dy;
    if (s->offset > s->content_h - s->h) s->offset = s->content_h - s->h;
    if (s->offset < 0) s->offset = 0;
    vus_gui_mark_dirty();
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", s->offset);
    return vus_string_new(buf);
}

/* 画线增强：支持线宽/虚线/箭头，兼容旧 5 参（后 3 参默认 1/0/0）。 */
VusString* vus_gui_draw_line_ex(int x1, int y1, int x2, int y2,
    unsigned int color, int width, int dashed, int arrow)
{
    if (!s_initialized) return vus_string_new("0");
    int wpx = (width < 1) ? 1 : width;
    unsigned int argb = argb_from_rgb(color);
    pixel_line_ex(x1, y1, x2, y2, argb, wpx, dashed);
    /* 箭头：终点端补两条翼线成三角头（整数近似，不限方向）。 */
    if (arrow && (x1 != x2 || y1 != y2))
    {
        int al = (wpx > 3) ? (wpx + 2) : 5;
        int dx = (x2 > x1) ? 1 : (x2 < x1 ? -1 : 0);
        int dy = (y2 > y1) ? 1 : (y2 < y1 ? -1 : 0);
        pixel_line_ex(x2, y2, x2 - dx * al, y2 + dy * al, argb, 1, 0);
        pixel_line_ex(x2, y2, x2 + dx * al, y2 - dy * al, argb, 1, 0);
    }
    vus_gui_mark_dirty();
    return vus_string_new("1");
}

/* ===== 阶段C+：控件/区域 PNG 背景（图形_背景图） ===== */

/* 用 libpng 解码 PNG 为 RGBA（8bit×4）像素数组。
 * 统一转为 RGBA8 便于采样：调色板→RGB、灰度→RGB、tRNS→alpha、RGB 无 alpha 补 255。
 * 成功返回 1 并写出 *pw、*ph、*data（data 需 free）；失败返回 0。 */
static int png_load_rgba(const char* path, int* pw, int* ph, unsigned char** data)
{
    FILE* f = fopen(path, "rb");
    png_structp png = NULL;
    png_infop info = NULL;
    if (!f) return 0;
    unsigned char sig[8];
    if (fread(sig, 1, 8, f) != 8 || png_sig_cmp(sig, 0, 8) != 0)
    {
        fclose(f);
        return 0;
    }
    png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { fclose(f); return 0; }
    info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, NULL, NULL); fclose(f); return 0; }
    if (setjmp(png_jmpbuf(png)))
    {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(f);
        return 0;
    }
    png_init_io(png, f);
    png_set_sig_bytes(png, 8);
    png_read_info(png, info);
    /* 统一解码为 8bit RGBA：调色板/灰度转 RGB，tRNS 补 alpha，RGB 补 alpha=255。 */
    png_set_palette_to_rgb(png);
    png_set_tRNS_to_alpha(png);
    png_set_gray_to_rgb(png);
    png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    png_read_update_info(png, info);

    int W = (int)png_get_image_width(png, info);
    int H = (int)png_get_image_height(png, info);
    int rowbytes = (int)png_get_rowbytes(png, info);
    png_bytepp rows = (png_bytepp)malloc(sizeof(png_bytep) * (size_t)H);
    unsigned char* buf = (unsigned char*)malloc((size_t)rowbytes * (size_t)H);
    if (!rows || !buf)
    {
        free(rows); free(buf);
        png_destroy_read_struct(&png, &info, NULL); fclose(f); return 0;
    }
    for (int i = 0; i < H; i++) rows[i] = buf + (size_t)i * (size_t)rowbytes;
    png_read_image(png, rows);
    png_read_end(png, info);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(f);
    *pw = W; *ph = H; *data = buf;
    free(rows);
    return 1;
}

/* 图形_背景图(x, y, 宽, 高, PNG路径)：解码 PNG 按最近邻拉伸铺到矩形，
 * 受滚动容器平移/裁剪，带 alpha 合成。 */
VusString* vus_gui_draw_png(int x, int y, int w, int h, const char* path)
{
    if (!s_initialized || w <= 0 || h <= 0 || !path || !*path)
    {
        return vus_string_new("0");
    }
    int sw = 0, sh = 0;
    /* G2：解码结果按路径 LRU 缓存，重复绘制复用像素，不再整文件重解码 */
    const unsigned char* rgba = png_cached_rgba(path, &sw, &sh);
    if (!rgba || sw <= 0 || sh <= 0)
    {
        return vus_string_new("0");
    }
    for (int ty = y; ty < y + h; ty++)
    {
        int sy = ((ty - y) * sh) / h;
        if (sy < 0) sy = 0; else if (sy >= sh) sy = sh - 1;
        for (int tx = x; tx < x + w; tx++)
        {
            int sx = ((tx - x) * sw) / w;
            if (sx < 0) sx = 0; else if (sx >= sw) sx = sw - 1;
            const unsigned char* p = rgba + ((size_t)sy * (size_t)sw + (size_t)sx) * 4;
            unsigned int a = p[3];
            if (a >= 255)
            {
                unsigned int argb = 0xFF000000u | ((unsigned)p[0] << 16) |
                                    ((unsigned)p[1] << 8) | (unsigned)p[2];
                write_scrolled_pixel(tx, ty, argb);
            }
            else
            {
                unsigned int dst = read_scrolled_pixel(tx, ty);
                unsigned int argb = argb_alpha_over(dst, p[0], p[1], p[2], (int)a);
                write_scrolled_pixel(tx, ty, argb);
            }
        }
    }
    /* 脏区已由 write_scrolled_pixel 逐像素归并 */
    return vus_string_new("1");
}

/* ===== 阶段D：图形_MD 最小集渲染 ===== */
#define MD_LINEH 16   /* 行距 px（对应 X 6x13 字体上略加间距） */

/* 单字符近似宽度：ASCII=6px，UTF-8 多字节（中文等）=12px。
 * 若已加载外部字体，则用 FreeType 真实 advance 度量（随字号缩放）。
 * 返回该字符消耗的字节数与像素宽。 */
static void md_next_char(const char* p, int* bytes, int* px)
{
    unsigned char c = (unsigned char)*p;
    if (c < 0x80) { *bytes = 1; }
    else
    {
        *bytes = 1;
        while ((unsigned char)*(p + *bytes) >= 0x80 &&
               ((unsigned char)*(p + *bytes) & 0xC0) == 0x80 && *bytes < 4) (*bytes)++;
    }
    if (s_ft_loaded && s_ftface)
    {
        int adv;
        unsigned int cp = ft_utf8_next(p, &adv);
        (void)adv;
        /* G1：度量走字形缓存（命中不再 FT_Load_Char 栅格化） */
        VusGlyphSlot* g = vus_glyph_get(cp);
        if (g) *px = g->advance;
        else   *px = s_ft_size;
        return;
    }
    if (c < 0x80) *px = 6;
    else *px = 12;
}

/* 把文本 s 按给定宽度折行，逐片用 draw_text_xy 绘制，每多一行下移 MD_LINEH。
 * 返回实际绘制的行数。 */
static int md_wrap_draw(int x, int y, int width, int indent, const char* s, unsigned int rgb)
{
    int usable = width - indent;
    if (usable < 6) usable = 6;
    int cnt = 0;
    while (s && *s)
    {
        const char* end = s;
        int used = 0;
        while (*end)
        {
            int nb, np;
            md_next_char(end, &nb, &np);
            if (used + np > usable) break;
            used += np;
            end += nb;
        }
        if (end == s)  /* 单个字符比可用区还宽时，至少推进一个字符 */
        {
            int nb, np; md_next_char(s, &nb, &np); (void)np;
            end = s + nb;
        }
        char seg[512];
        size_t sl = (size_t)(end - s);
        if (sl > 511) sl = 511;
        memcpy(seg, s, sl);
        seg[sl] = '\0';
        draw_text_xy(x + indent, y, seg, rgb);
        cnt++;
        s = end;
        y += MD_LINEH;
        if (!*end) break;
    }
    return cnt;
}

/* 图形_MD(x, y, 宽度, 文本)：最小集 Markdown 渲染。返回占用行数。 */
VusString* vus_gui_md(int x, int y, int width, const char* text)
{
    if (!s_initialized) { return vus_string_new("0"); }
    if (!text || !*text) { return vus_string_new("0"); }

    const char* p = text;
    int total_lines = 0;
    int cy = y;
    int in_code = 0;
    const unsigned int code_col = 0x875F00; /* 代码块强调色 */

    while (p && *p)
    {
        const char* eol = strchr(p, '\n');
        int len = eol ? (int)(eol - p) : (int)strlen(p);
        while (len > 0 && (p[len - 1] == '\r' || p[len - 1] == '\n')) len--;
        if (len <= 0)
        {
            total_lines++;
            cy += MD_LINEH;
            p = eol ? eol + 1 : NULL;
            continue;
        }

        char line[512];
        if (len > 511) len = 511;
        memcpy(line, p, (size_t)len);
        line[len] = '\0';
        if (eol) p = eol + 1; else p = NULL;

        /* 去掉前导空白。 */
        const char* t = line;
        while (*t == ' ' || *t == '\t') t++;

        int indent = 0;
        unsigned int col = s_theme.fg;
        const char* body = t;

        if (in_code)
        {
            if (strncmp(t, "```", 3) == 0) { in_code = 0; continue; }
            indent = 8; col = code_col;
        }
        else if (strncmp(t, "```", 3) == 0)
        {
            in_code = 1;  /* 代码块起始行，忽略内容与语言说明 */
            continue;
        }
        else if (*t == '#')
        {
            int lev = 0; while (t[lev] == '#') lev++;
            body = t + lev;
            while (*body == ' ') body++;
            col = s_theme.highlight;   /* 标题用高亮色 */
        }
        else if (*t == '-' || *t == '*')
        {
            body = t + 2;              /* 去 "- " / "* " 前缀 */
            indent = 16;
        }
        else if (*t == '>')
        {
            body = t + 1;
            while (*body == ' ') body++;
            indent = 8;
            col = s_theme.border;      /* 引用用边框色 */
        }
        else if (*t >= '0' && *t <= '9')
        {
            const char* dd = t;
            while (*dd >= '0' && *dd <= '9') dd++;
            if ((*dd == '.' || *dd == ')')) { body = dd + 1; while (*body == ' ') body++; indent = 16; }
        }
        else if (strncmp(t, "---", 3) == 0)
        {
            /* 分隔线：折行绘制一条下划线式占位线。 */
            draw_text_xy(x, cy, "────────", s_theme.border);
            total_lines++;
            cy += MD_LINEH;
            continue;
        }

        int n = md_wrap_draw(x, cy, width, indent, body, col);
        total_lines += n;
        cy += MD_LINEH * n;
    }

    return vus_to_string(total_lines);
}

/* 记录最近点击坐标，并触发约定回调 事件_点击(x,y)（可选，见上）。 */
void vus_gui_platform_emit_click(int x, int y)
{
    /* 无论是否有回调，都更新点击状态，供轮询式 图形_按钮点击 命中检测 */
    s_click_x = x;
    s_click_y = y;
    s_click_consumed = 0; /* 新点击到来，重新允许一次消费 */

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

/* ============ 阶段4：X11 多输入 实现 ============ */

/* 按键事件：记录最近一次按键状态，并触发约定回调 事件_按键(字符, keycode)。 */
void vus_gui_platform_emit_key(const char* symstr, unsigned int keycode, unsigned int state)
{
    s_valid_key = symstr && symstr[0];
    if (symstr && symstr[0])
    {
        strncpy(s_last_key, symstr, sizeof(s_last_key) - 1);
        s_last_key[sizeof(s_last_key) - 1] = '\0';
    }
    else
    {
        s_last_key[0] = '\0';
    }
    s_last_keycode = keycode;
    s_last_keymask = state;

    char san[256];
    char sym[512];
    vus_gui_sanitize_name(VUS_CALLBACK_KEY, san, sizeof(san));
    snprintf(sym, sizeof(sym), "vus_%s", san);
    void (*fn)(void*) = (void(*)(void*))dlsym(RTLD_DEFAULT, sym);
    if (!fn) return;

    /* 回调参数：字符字符串 + keycode 字符串 */
    VusString* ks = s_last_key[0] ? vus_string_new(s_last_key) : vus_string_new("");
    VusString* kc = vus_to_string((int)keycode);
    if (!ks || !kc) { if (ks) vus_unref(ks); if (kc) vus_unref(kc); return; }
    VusString* args[3] = { 0, ks, kc };
    fn(args);
    vus_unref(ks);
    vus_unref(kc);
}

/* 指针移动：记录鼠标位置（供 图形_鼠标x/y、悬停检测）。 */
void vus_gui_platform_emit_motion(int x, int y, unsigned int state)
{
    s_motion_valid = 1;
    s_mouse_x = x;
    s_mouse_y = y;
    s_mouse_mask = state;
}

/* 滚轮：记录滚动增量（供 图形_滚轮 轮询读取），并自动滚动指针下的容器。 */
void vus_gui_platform_emit_wheel(int x, int y, int dy)
{
    s_wheel_dy += dy;   /* 多档滚动累计，读取时清零 */
    s_wheel_x = x;
    s_wheel_y = y;
    /* 滚轮自动：命中指针下容器则滚动。上滚(+1)→offset-1；下滚(-1)→offset+1。 */
    for (int i = 0; i < s_scroll_cnt; i++)
    {
        VusScroll* sc = &s_scrolls[i];
        if (point_in(x, y, sc->x, sc->y, sc->w, sc->h))
        {
            sc->offset += (dy < 0) ? 1 : -1;
            if (sc->offset > sc->content_h - sc->h) sc->offset = sc->content_h - sc->h;
            if (sc->offset < 0) sc->offset = 0;
            vus_gui_mark_dirty();
            break;
        }
    }
}

/* 按钮按下（非滚轮 button）：记录到 s_pressed + 触发 事件_点击。 */
void vus_gui_platform_emit_button(int x, int y, int button)
{
    if (button >= 1 && button < 8) { s_pressed[button]++; }
    /* 普通点击（左/中/右键）也纳入 事件_点击 回调语义 */
    (void)x; (void)y;
}

/* 轮询读取内建：按键字符（无可打印字符返回空串）。
 * 映射 图形_按键() -> 返回 VusString*。 */
VusString* vus_gui_last_key(void)
{
    if (!s_initialized || !s_valid_key) return vus_string_new("");
    return vus_string_new(s_last_key);
}

/* 轮询读取内建：按键 keycode（KeySym 值），无按键返回 -1。 */
VusString* vus_gui_last_keycode(void)
{
    if (!s_initialized || !s_valid_key || s_last_keycode == 0)
        return vus_string_new("-1");
    return vus_to_string((int)s_last_keycode);
}

/* 轮询读取内建：鼠标位置，返回 "x,y"；尚无移动返回 "-1,-1"。 */
VusString* vus_gui_mouse_pos(void)
{
    if (!s_initialized || !s_motion_valid || s_mouse_x < 0)
        return vus_string_new("-1,-1");
    char buf[64];
    snprintf(buf, sizeof(buf), "%d,%d", s_mouse_x, s_mouse_y);
    return vus_string_new(buf);
}

/* 轮询读取内建：鼠标 X，返回整数或 -1。 */
VusString* vus_gui_mouse_x(void)
{
    if (!s_initialized || !s_motion_valid || s_mouse_x < 0)
        return vus_string_new("-1");
    return vus_to_string(s_mouse_x);
}

/* 轮询读取内建：鼠标 Y，返回整数或 -1。 */
VusString* vus_gui_mouse_y(void)
{
    if (!s_initialized || !s_motion_valid || s_mouse_y < 0)
        return vus_string_new("-1");
    return vus_to_string(s_mouse_y);
}

/* 轮询读取内建：滚轮增量（读取后清零），无滚动返回 0。 */
VusString* vus_gui_wheel(void)
{
    if (!s_initialized) return vus_string_new("0");
    int dy = s_wheel_dy;
    s_wheel_dy = 0;
    return vus_to_string(dy);
}

/* 轮询读取内建：指定鼠标键是否本次事件按下并消费，返回 "true"/"false"。 */
VusString* vus_gui_button_pressed(int button)
{
    if (!s_initialized || button < 1 || button >= 8) return vus_string_new("false");
    if (s_pressed[button] > 0)
    {
        s_pressed[button]--;
        return vus_string_new("true");
    }
    return vus_string_new("false");
}

/* 悬停检测：鼠标当前是否悬停在名为 name 的控件上。返回 "true"/"false"。 */
VusString* vus_gui_hover(const char* name)
{
    VusControl* c;
    if (!s_initialized || !name || !s_motion_valid || s_mouse_x < 0 ||
        !(c = find_ctrl(name)))
    {
        return vus_string_new("false");
    }
    return vus_string_new(point_in(s_mouse_x, s_mouse_y, c->x, c->y, c->w, c->h)
                          ? "true" : "false");
}

/* ============ 阶段C：控件组合模板 ============ */

/* 画一个带圆角矩形（radius为圆角半径像素）。圆角仅在 X11 文字/粗画布上不适用，
 * 这里用表面层逐像素走样；为简单起见用直角加四角点采样近似，非关键路径。
 * 说明：GuiLite surface 只有 fill_rect/draw_rect，圆角通过组合绘制视觉近似。 */
static void theme_fill_rounded(int x, int y, int w, int h, int r, unsigned int rgb)
{
    vus_gui_surface_fill_rect(x, y, w, h, argb_from_rgb(rgb));
    (void)r; /* 留作扩展：圆角裁剪 */
}

/* 图形_卡片(名,x,y,宽,高,标题)：绘制一张带标题栏的卡片，登记为可命中区域（CTRL_CANVAS 语义）。
 * 卡片往内 x+8,y+24 起为内容区，脚本在其内放置其它控件。
 * 配色取主题：背景=bg、边框=border、标题文字=highlight。返回 "1"。 */
VusString* vus_gui_card(const char* name, int x, int y, int w, int h, const char* title)
{
    if (!s_initialized || !name || !*name || w <= 0 || h <= 0)
    {
        return vus_string_new("0");
    }
    VusControl* c = register_ctrl(name, CTRL_CANVAS);
    if (!c)
    {
        return vus_string_new("0");
    }
    c->x = x; c->y = y; c->w = w; c->h = h;
    vus_gui_mark_dirty();
    /* 卡片主体（浅背景）+ 边框 */
    theme_fill_rounded(x, y, w, h, 4, s_theme.bg);
    vus_gui_surface_draw_rect(x, y, w, h, argb_from_rgb(s_theme.border));
    /* 标题栏：底部一条分隔线 + 标题文字 */
    vus_gui_surface_draw_rect(x, y + 22, w, 1, argb_from_rgb(s_theme.border));
    draw_text_xy(x + 6, y + 6, title ? title : "", s_theme.highlight);
    return vus_string_new("1");
}

/* 图形_面板(名,x,y,宽,高,标题)：卡片别名（同卡片绘制），供语义化使用。 */
VusString* vus_gui_panel(const char* name, int x, int y, int w, int h, const char* title)
{
    return vus_gui_card(name, x, y, w, h, title);
}

/* 图形_表单行(名,行名,x,y,宽,标签)：绘制"标签 + 右侧空白输入框"的组合行。
 * 标签色=fg，输入框白底+主题边框。行高 24。返回 "1"。 */
VusString* vus_gui_form_row(const char* name, const char* label, int x, int y, int w, const char* text)
{
    if (!s_initialized || !name || !*name || w <= 0)
    {
        return vus_string_new("0");
    }
    /* 文本框控件：登记输入框区域（名 = name，加 "_in" 后缀避免冲突） */
    char iname[72];
    snprintf(iname, sizeof(iname), "%s_in", name);
    VusControl* c = register_ctrl(iname, CTRL_TEXTBOX);
    if (!c)
    {
        return vus_string_new("0");
    }
    int input_x = x + 96;                 /* 标签区宽 96 */
    int input_w = w - 96; if (input_w < 10) input_w = 10;
    c->x = input_x; c->y = y; c->w = input_w; c->h = 22;
    vus_gui_mark_dirty();
    vus_gui_surface_fill_rect(input_x, y, input_w, 22, argb_from_rgb(0xFFFFFF));
    vus_gui_surface_draw_rect(input_x, y, input_w, 22, argb_from_rgb(s_theme.border));
    draw_text_xy(x, y + 5, label ? label : "", s_theme.fg);
    draw_text_xy(input_x + 4, y + 5, text ? text : "", s_theme.fg);
    /* 登记行主体（用于 图形_行点击 命中整行） */
    VusControl* row = register_ctrl(name, CTRL_BUTTON);
    if (row) { row->x = x; row->y = y; row->w = w; row->h = 22; }
    return vus_string_new("1");
}

/* 图形_行点击(名)：最近一次点击是否落在 name 组合行（表单行/面板区）内。 */
VusString* vus_gui_row_clicked(const char* name)
{
    VusControl* c;
    if (!s_initialized || !name || s_click_x < 0 || s_click_y < 0 ||
        !(c = find_ctrl(name)))
    {
        return vus_string_new("false");
    }
    if (point_in(s_click_x, s_click_y, c->x, c->y, c->w, c->h))
    {
        s_click_consumed = 1;
        return vus_string_new("true");
    }
    return vus_string_new("false");
}

/* 图形_圆环(名,x,y,半径,比例,颜色)：绘制一个环形进度（外圆边框 + 按比例填充的圆弧）。
 * 比例 pct 为 0-100；颜色取传入（-1 则用主题 highlight）。返回 "1"。 */
VusString* vus_gui_ring(const char* name, int x, int y, int radius, int pct, int color)
{
    if (!s_initialized || !name || !*name || radius < 2)
    {
        return vus_string_new("0");
    }
    VusControl* c = register_ctrl(name, CTRL_CANVAS);
    if (!c)
    {
        return vus_string_new("0");
    }
    unsigned int col = (color < 0) ? s_theme.highlight : (unsigned int)color;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    /* 外圆描边：用表面 draw_rect 近似为正方形外框 + 内部扇形用像素填充。
     * 简易版本：按比例在矩形区域内填充一个"进度块"，视觉表示为圆环的朴素近似。 */
    c->x = x - radius; c->y = y - radius; c->w = radius * 2; c->h = radius * 2;
    vus_gui_mark_dirty();
    vus_gui_surface_draw_rect(x - radius, y - radius, radius * 2, radius * 2,
                              argb_from_rgb(col));
    /* 按比例填充的进度条（作为圆环的线性近似显示） */
    int fill_w = (int)((long)radius * 2 * pct / 100);
    if (fill_w > 2)
    {
        vus_gui_surface_fill_rect(x - radius, y + radius - 3, fill_w, 3, argb_from_rgb(col));
    }
    return vus_string_new("1");
}

/* 创建并绘制一个按钮：填充底色 + 边框 + 文本（文本优先 X11 叠加，方向正常），
 * 并登记命中矩形。同名按钮重复调用时更新位置/尺寸。 */
VusString* vus_gui_button(const char* name, int x, int y, int w, int h, const char* text)
{
    if (!s_initialized || !name || !*name || w <= 0 || h <= 0)
    {
        return vus_string_new("0");
    }
    VusControl* c = register_ctrl(name, CTRL_BUTTON);
    if (!c)
    {
        return vus_string_new("0");
    }
    c->x = x; c->y = y; c->w = w; c->h = h;
    vus_gui_mark_dirty();

    /* 配色取自主题：底=highlight、边框=border、文字=text。
     * 图形_外观 设置全局圆角半径后，改为圆角填充 + 圆角外框。 */
    if (s_global_radius > 0)
    {
        fill_round_rect_px(x, y, w, h, s_global_radius, argb_from_rgb(s_theme.highlight));
        round_rect_outline_px(x, y, w, h, s_global_radius, argb_from_rgb(s_theme.border));
    }
    else
    {
        vus_gui_surface_fill_rect(x, y, w, h, argb_from_rgb(s_theme.highlight));
        vus_gui_surface_draw_rect(x, y, w, h, argb_from_rgb(s_theme.border));
    }
    int tw = text ? (int)(strlen(text) * 6) : 0; /* 估宽：6px/字符（X 6x13 字体） */
    int tx = x + (tw < w ? (w - tw) / 2 : 0);
    int ty = y + (h - 13) / 2;
    draw_text_xy(tx, ty, text ? text : "", s_theme.text);
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

/* 命中检测：最近一次点击是否落在名为 name 的按钮矩形内。
 * 命中且本次点击尚未被任何控件消费时返回 true 并立即消费（置
 * s_click_consumed），防止主循环每帧重复读到同一点击而反复触发
 * （此前坐标直到下次点击才更新，导致同一按钮被无限次命中）。 */
VusString* vus_gui_button_clicked(const char* name)
{
    VusControl* c;
    if (!s_initialized || !name || s_click_x < 0 || s_click_y < 0 ||
        !(c = find_ctrl(name)) || c->type != CTRL_BUTTON)
    {
        return vus_string_new("false");
    }
    if (!point_in(s_click_x, s_click_y, c->x, c->y, c->w, c->h))
    {
        return vus_string_new("false");
    }
    if (s_click_consumed)
    {
        /* 本次点击已被其它控件消费（如本帧前面的复选框/按钮） */
        return vus_string_new("false");
    }
    s_click_consumed = 1;
    return vus_string_new("true");
}

/* ============ 阶段3：控件库实现 ============ */

/* 标签：绘制一行文本，登记为可命中控件（用估宽的矩形）。 */
VusString* vus_gui_label(const char* name, int x, int y, const char* text, unsigned int color)
{
    if (!s_initialized || !name || !*name)
    {
        return vus_string_new("0");
    }
    VusControl* c = register_ctrl(name, CTRL_LABEL);
    if (!c)
    {
        return vus_string_new("0");
    }
    int tw = text ? (int)(strlen(text) * 6) : 0;
    c->x = x; c->y = y; c->w = tw ? tw : 1; c->h = 13;
    vus_gui_mark_dirty();
    draw_text_xy(x, y, text ? text : "", color);
    return vus_string_new("1");
}

/* 文本框：白底 + 边框 + 文本，登记矩形。 */
VusString* vus_gui_textbox(const char* name, int x, int y, int w, int h, const char* text)
{
    if (!s_initialized || !name || !*name || w <= 0 || h <= 0)
    {
        return vus_string_new("0");
    }
    VusControl* c = register_ctrl(name, CTRL_TEXTBOX);
    if (!c)
    {
        return vus_string_new("0");
    }
    c->x = x; c->y = y; c->w = w; c->h = h;
    vus_gui_mark_dirty();
    vus_gui_surface_fill_rect(x, y, w, h, argb_from_rgb(0xFFFFFF));
    vus_gui_surface_draw_rect(x, y, w, h, argb_from_rgb(0x555555));
    int tw = text ? (int)(strlen(text) * 6) : 0;
    draw_text_xy(x + 4, y + (h - 13) / 2, text ? text : "", 0x000000);
    (void)tw;
    return vus_string_new("1");
}

/* 复选框：方格 + 勾选标记 + 文本。点击（未消费）切换勾选状态。 */
VusString* vus_gui_checkbox(const char* name, int x, int y, const char* text, int checked)
{
    if (!s_initialized || !name || !*name)
    {
        return vus_string_new("false");
    }
    VusControl* c = register_ctrl(name, CTRL_CHECKBOX);
    if (!c)
    {
        return vus_string_new("false");
    }
    const int box = 13; /* 方格边长 */
    c->x = x; c->y = y; c->w = box + (text ? (int)strlen(text) * 6 : 0); c->h = box;
    vus_gui_mark_dirty();
    /* 点击命中方格/文本区且本次点击未消费 → 切换勾选状态。
     * 切换后标记 touched，此后以内部 c->checked 为权威绘制并返回，
     * 避免主循环每帧用外部变量旧值把状态覆盖回未勾选（“开→关”反复）。 */
    if (s_click_x >= 0 && s_click_y >= 0 && !s_click_consumed &&
        point_in(s_click_x, s_click_y, x, y, c->w, box))
    {
        c->checked = !c->checked;
        c->touched = 1;
        s_click_consumed = 1;
    }
    else if (!c->touched)
    {
        /* 从未被点击过：以调用方传入值作为初始/外部同步状态 */
        c->checked = checked;
    }
    const int shown = c->checked;
    /* 绘制：方格边框 +（勾选时）内部填充 + 文本（配色取主题） */
    vus_gui_surface_draw_rect(x, y, box, box, argb_from_rgb(s_theme.border));
    if (shown)
    {
        vus_gui_surface_fill_rect(x + 2, y + 2, box - 4, box - 4, argb_from_rgb(s_theme.highlight));
    }
    if (text)
    {
        draw_text_xy(x + box + 4, y + (box - 13) / 2, text, s_theme.fg);
    }
    return vus_string_new(shown ? "true" : "false");
}

/* 进度条：填充底色 + 按 value(0-100) 画比例的进度 + 边框。 */
VusString* vus_gui_progress(const char* name, int x, int y, int w, int h, int value)
{
    if (!s_initialized || !name || !*name || w <= 0 || h <= 0)
    {
        return vus_string_new("0");
    }
    VusControl* c = register_ctrl(name, CTRL_PROGRESS);
    if (!c)
    {
        return vus_string_new("0");
    }
    c->x = x; c->y = y; c->w = w; c->h = h;
    c->progress = (value < 0) ? 0 : (value > 100 ? 100 : value);
    vus_gui_mark_dirty();
    vus_gui_surface_fill_rect(x, y, w, h, argb_from_rgb(0xE8E8E8));          /* 底色 */
    int fw = c->progress * w / 100;
    if (fw > 0)
    {
        vus_gui_surface_fill_rect(x, y, fw, h, argb_from_rgb(0x44BB44));      /* 进度 */
    }
    vus_gui_surface_draw_rect(x, y, w, h, argb_from_rgb(0x666666));           /* 边框 */
    return vus_string_new("1");
}

/* 列表：声明列表区域，登记行列信息。 */
VusString* vus_gui_list(const char* name, int x, int y, int w, int h, int row_h)
{
    if (!s_initialized || !name || !*name || w <= 0 || h <= 0 || row_h <= 0)
    {
        return vus_string_new("0");
    }
    VusControl* c = register_ctrl(name, CTRL_LIST);
    if (!c)
    {
        return vus_string_new("0");
    }
    c->x = x; c->y = y; c->w = w; c->h = h; c->row_h = row_h; c->line_cnt = 0;
    vus_gui_mark_dirty();
    vus_gui_surface_draw_rect(x, y, w, h, argb_from_rgb(0x888888)); /* 列表外框 */
    return vus_string_new("1");
}

/* 列表行：写入并绘制第 line 行文本（高亮最近选中的行）。 */
VusString* vus_gui_list_row(const char* name, int line, const char* text)
{
    if (!s_initialized || line < 0 || line >= VUS_LIST_LINES)
    {
        return vus_string_new("0");
    }
    VusControl* c = find_ctrl(name);
    if (!c || c->type != CTRL_LIST) return vus_string_new("0");
    strncpy(c->lines[line], text ? text : "", VUS_LINE_MAX - 1);
    c->lines[line][VUS_LINE_MAX - 1] = '\0';
    if (line >= c->line_cnt) c->line_cnt = line + 1;
    vus_gui_mark_dirty();

    int ry = c->y + line * c->row_h;
    /* 选中行（最近点击落在该行）高亮浅蓝底 */
    int sel = (s_click_x >= 0 && s_click_y >= 0) && c->type == CTRL_LIST &&
              point_in(s_click_x, s_click_y, c->x + 1, ry, c->w - 2, c->row_h)
              ? 1 : 0;
    if (sel)
    {
        vus_gui_surface_fill_rect(c->x + 1, ry + 1, c->w - 2, c->row_h - 2,
                                  argb_from_rgb(0xB8E0FF));
    }
    else
    {
        vus_gui_surface_fill_rect(c->x + 1, ry + 1, c->w - 2, c->row_h - 2,
                                  argb_from_rgb(0xFFFFFF));
    }
    draw_text_xy(c->x + 4, ry + (c->row_h - 13) / 2, c->lines[line],
                 sel ? 0x0044AA : 0x000000);
    return vus_string_new("1");
}

/* 列表选中行：最近一次点击命中的行索引，未命中返回 "-1"。 */
VusString* vus_gui_list_selected(const char* name)
{
    VusControl* c = find_ctrl(name);
    if (!s_initialized || !c || c->type != CTRL_LIST ||
        s_click_x < 0 || s_click_y < 0)
    {
        return vus_string_new("-1");
    }
    if (!point_in(s_click_x, s_click_y, c->x, c->y, c->w, c->h))
    {
        return vus_string_new("-1");
    }
    int line = (s_click_y - c->y) / c->row_h;
    return vus_to_string(line);
}

/* 列表行命中：最近一次点击是否落在 name 列表的第 line 行内。
 * 命中且未消费时允许触发一次并立即消费，避免主循环每帧重复触发跳页等逻辑。 */
VusString* vus_gui_list_row_clicked(const char* name, int line)
{
    VusControl* c = find_ctrl(name);
    if (!s_initialized || !c || c->type != CTRL_LIST || line < 0 ||
        s_click_x < 0 || s_click_y < 0)
    {
        return vus_string_new("false");
    }
    if (s_click_consumed)
    {
        return vus_string_new("false");
    }
    int ry = c->y + line * c->row_h;
    int in = point_in(s_click_x, s_click_y, c->x, ry, c->w, c->row_h);
    if (in)
    {
        s_click_consumed = 1;
        return vus_string_new("true");
    }
    return vus_string_new("false");
}

/* 画布：登记一个可命中的绘图区域（可选描边）。 */
VusString* vus_gui_canvas(const char* name, int x, int y, int w, int h)
{
    if (!s_initialized || !name || !*name || w <= 0 || h <= 0)
    {
        return vus_string_new("0");
    }
    VusControl* c = register_ctrl(name, CTRL_CANVAS);
    if (!c)
    {
        return vus_string_new("0");
    }
    c->x = x; c->y = y; c->w = w; c->h = h;
    c->rel_x = c->rel_y = -1;
    vus_gui_mark_dirty();
    vus_gui_surface_fill_rect(x, y, w, h, argb_from_rgb(0x1E1E1E)); /* 画布底 */
    vus_gui_surface_draw_rect(x, y, w, h, argb_from_rgb(0x333333)); /* 描边 */
    return vus_string_new("1");
}

/* 画布命中：最近一次点击是否落在画布内，记录相对坐标。 */
VusString* vus_gui_canvas_hit(const char* name)
{
    VusControl* c = find_ctrl(name);
    if (!s_initialized || !c || c->type != CTRL_CANVAS ||
        s_click_x < 0 || s_click_y < 0)
    {
        return vus_string_new("false");
    }
    if (point_in(s_click_x, s_click_y, c->x, c->y, c->w, c->h))
    {
        c->rel_x = s_click_x - c->x;
        c->rel_y = s_click_y - c->y;
        return vus_string_new("true");
    }
    return vus_string_new("false");
}

/* 画布相对坐标：返回最近命中的相对位置 "x,y"；未命中返回 "-1,-1"。 */
VusString* vus_gui_canvas_pos(const char* name)
{
    VusControl* c = find_ctrl(name);
    if (!s_initialized || !c || c->type != CTRL_CANVAS || c->rel_x < 0)
    {
        return vus_string_new("-1,-1");
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%d,%d", c->rel_x, c->rel_y);
    return vus_string_new(buf);
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
    vus_gui_mark_dirty_area(x, y, 1, 1);
    return vus_string_new("1");
}

VusString* vus_gui_draw_line(int x1, int y1, int x2, int y2, unsigned int color)
{
    if (!s_initialized) { return vus_string_new("0"); }
    vus_gui_surface_draw_line(x1, y1, x2, y2, argb_from_rgb(color));
    int ax = x1 < x2 ? x1 : x2;
    int ay = y1 < y2 ? y1 : y2;
    int bw = (x1 < x2 ? x2 - x1 : x1 - x2) + 1;
    int bh = (y1 < y2 ? y2 - y1 : y1 - y2) + 1;
    vus_gui_mark_dirty_area(ax, ay, bw, bh);
    return vus_string_new("1");
}

VusString* vus_gui_draw_rect(int x, int y, int width, int height, unsigned int color)
{
    if (!s_initialized) { return vus_string_new("0"); }
    vus_gui_surface_draw_rect(x, y, width, height, argb_from_rgb(color));
    vus_gui_mark_dirty_area(x, y, width, height);
    return vus_string_new("1");
}

VusString* vus_gui_fill_rect(int x, int y, int width, int height, unsigned int color)
{
    if (!s_initialized) { return vus_string_new("0"); }
    vus_gui_surface_fill_rect(x, y, width, height, argb_from_rgb(color));
    vus_gui_mark_dirty_area(x, y, width, height);
    return vus_string_new("1");
}

VusString* vus_gui_draw_text(int x, int y, const char* text, unsigned int color)
{
    if (!s_initialized) { return vus_string_new("0"); }
    /* 优先用 X11 文字（XDrawString，方向正常）；X11 不可用时回退 GuiLite */
    if (vus_gui_platform_draw_text(x, y, text, color) == 1)
    {
        vus_gui_mark_dirty(); /* X11 文字已入队，同样视为有绘制 */
        return vus_string_new("1");
    }
    vus_gui_surface_draw_text(x, y, text, argb_from_rgb(color));
    vus_gui_mark_dirty();
    return vus_string_new("1");
}

VusString* vus_gui_redraw(void)
{
    if (!s_initialized)
    {
        return vus_string_new("0");
    }
    /* 本帧无任何新绘制：直接短路，不做上屏，避免无条件刷新的 CPU 空转。
     * 文字队列由 platform_redraw 在每次成功后清空，因此跳过时不残留。 */
    if (!s_dirty)
    {
        return vus_string_new("1");
    }
    int W = vus_gui_surface_width();
    int H = vus_gui_surface_height();
    int rx1 = s_dirty_x1, ry1 = s_dirty_y1, rx2 = s_dirty_x2, ry2 = s_dirty_y2;
    if (rx1 < 0) rx1 = 0; if (ry1 < 0) ry1 = 0;
    if (rx2 > W) rx2 = W; if (ry2 > H) ry2 = H;
    s_dirty = 0;
    /* G8：双缓冲增量提交 + 脏矩形上屏：仅拷贝/上屏本帧实际写过的区域，
     * 取代整帧 memcpy 与整帧上传；全屏脏（如大文本/首帧）时区域自动=全屏。 */
    if (rx2 > rx1 && ry2 > ry1)
    {
        vus_gui_surface_present_area(rx1, ry1, rx2, ry2);
        vus_gui_platform_redraw(W, H, vus_gui_surface_framebuffer(), rx1, ry1, rx2, ry2);
    }
    else
    {
        /* 脏区为空（理论不达）：保底整帧提交，保证语义正确 */
        vus_gui_surface_present();
        vus_gui_platform_redraw(W, H, vus_gui_surface_framebuffer(), 0, 0, W, H);
    }
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

/* =====================================================================
 * 阶段G：图片 API + GIF 播放器
 * ===================================================================== */

/* RGBA（4 字节/像素）近邻拉伸绘制到 (x,y,w,h)，带 alpha 合成（与 draw_png 同法）。 */
static void blit_rgba_stretch(int x, int y, int w, int h,
                              const unsigned char* rgba, int sw, int sh)
{
    for (int ty = y; ty < y + h; ty++)
    {
        int sy = ((ty - y) * sh) / h;
        if (sy < 0) sy = 0; else if (sy >= sh) sy = sh - 1;
        for (int tx = x; tx < x + w; tx++)
        {
            int sx = ((tx - x) * sw) / w;
            if (sx < 0) sx = 0; else if (sx >= sw) sx = sw - 1;
            const unsigned char* p = rgba + ((size_t)sy * (size_t)sw + (size_t)sx) * 4;
            unsigned int a = p[3];
            if (a >= 255)
            {
                unsigned int argb = 0xFF000000u | ((unsigned)p[0] << 16) |
                                    ((unsigned)p[1] << 8) | (unsigned)p[2];
                write_scrolled_pixel(tx, ty, argb);
            }
            else
            {
                unsigned int dst = read_scrolled_pixel(tx, ty);
                unsigned int argb = argb_alpha_over(dst, p[0], p[1], p[2], (int)a);
                write_scrolled_pixel(tx, ty, argb);
            }
        }
    }
}

/* RGB（3 字节/像素，opaque）近邻拉伸绘制到 (x,y,w,h)（GIF 帧输出为 RGB）。 */
static void blit_rgb_stretch(int x, int y, int w, int h,
                             const unsigned char* rgb, int sw, int sh)
{
    for (int ty = y; ty < y + h; ty++)
    {
        int sy = ((ty - y) * sh) / h;
        if (sy < 0) sy = 0; else if (sy >= sh) sy = sh - 1;
        for (int tx = x; tx < x + w; tx++)
        {
            int sx = ((tx - x) * sw) / w;
            if (sx < 0) sx = 0; else if (sx >= sw) sx = sw - 1;
            const unsigned char* p = rgb + ((size_t)sy * (size_t)sw + (size_t)sx) * 3;
            unsigned int argb = 0xFF000000u | ((unsigned)p[0] << 16) |
                                ((unsigned)p[1] << 8) | (unsigned)p[2];
            write_scrolled_pixel(tx, ty, argb);
        }
    }
}

/* 用 nanosvg 解析 SVG 并栅格化到其自身 viewport 大小，得到 RGBA（non-premultiplied）。
 * 成功返回 1 并写出 *pw/*ph/*data（需 free）；失败返回 0。 */
static int svg_load_rgba(const char* path, int* pw, int* ph, unsigned char** data)
{
    NSVGimage* img = nsvgParseFromFile(path, "px", 96.0f);
    if (!img) return 0;
    int w = (int)img->width, h = (int)img->height;
    if (w <= 0 || h <= 0) { nsvgDelete(img); return 0; }
    unsigned char* dst = (unsigned char*)malloc((size_t)w * (size_t)h * 4);
    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (!dst || !rast)
    {
        free(dst);
        if (rast) nsvgDeleteRasterizer(rast);
        nsvgDelete(img);
        return 0;
    }
    nsvgRasterize(rast, img, 0.0f, 0.0f, 1.0f, dst, w, h, w * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(img);
    *pw = w; *ph = h; *data = dst;
    return 1;
}

/* 图形_图片(x, y, 宽, 高, 路径)：按扩展名（.png/.svg/.gif，其它按 PNG）
 * 解码并最近邻拉伸绘制到矩形。返回 "1" 成功 / "0" 失败。 */
VusString* vus_gui_draw_image(int x, int y, int w, int h, const char* path)
{
    if (!s_initialized || w <= 0 || h <= 0 || !path || !*path)
    {
        return vus_string_new("0");
    }
    /* 取小写扩展名 */
    char ext[8] = {0};
    const char* dot = strrchr(path, '.');
    if (dot && dot[1])
    {
        size_t k = 0;
        while (dot[1 + k] && k < sizeof(ext) - 1)
        {
            ext[k] = (char)tolower((unsigned char)dot[1 + k]);
            k++;
        }
    }

    if (strcmp(ext, "svg") == 0)
    {
        int sw = 0, sh = 0;
        /* G2：解码结果 LRU 缓存，动画循环重复绘制同一 SVG 不再反复解析栅格化 */
        const unsigned char* rgba = svg_cached_rgba(path, &sw, &sh);
        if (!rgba || sw <= 0 || sh <= 0)
        {
            return vus_string_new("0");
        }
        blit_rgba_stretch(x, y, w, h, rgba, sw, sh);
        return vus_string_new("1");
    }
    if (strcmp(ext, "gif") == 0)
    {
        gd_GIF* g = gd_open_gif(path);
        if (!g) return vus_string_new("0");
        unsigned char* rgb = (unsigned char*)malloc((size_t)g->width * (size_t)g->height * 3);
        if (!rgb) { gd_close_gif(g); return vus_string_new("0"); }
        gd_rewind(g);
        gd_get_frame(g);              /* 加载第 0 帧到 canvas */
        gd_render_frame(g, rgb);
        blit_rgb_stretch(x, y, w, h, rgb, (int)g->width, (int)g->height);
        free(rgb);
        gd_close_gif(g);
        vus_gui_mark_dirty();
        return vus_string_new("1");
    }
    /* 默认按 PNG 处理 */
    {
        int sw2 = 0, sh2 = 0;
        /* G2：解码结果 LRU 缓存 */
        const unsigned char* rgba2 = png_cached_rgba(path, &sw2, &sh2);
        if (!rgba2 || sw2 <= 0 || sh2 <= 0)
        {
            return vus_string_new("0");
        }
        blit_rgba_stretch(x, y, w, h, rgba2, sw2, sh2);
        /* 脏区由 write_scrolled_pixel 逐像素归并 */
        return vus_string_new("1");
    }
}

/* ---- GIF 播放器：静态槽表 ---- */
#define VUS_GIF_MAX 8
typedef struct {
    char name[64];
    gd_GIF* gif;
    int nframes;     /* 总帧数 */
    int frame_idx;   /* 当前帧索引（下一步推进） */
    int used;
} VusGifSlot;
static VusGifSlot s_gifs[VUS_GIF_MAX];

static VusGifSlot* find_gif(const char* name)
{
    if (!name) return NULL;
    for (int i = 0; i < VUS_GIF_MAX; i++)
        if (s_gifs[i].used && strcmp(s_gifs[i].name, name) == 0) return &s_gifs[i];
    return NULL;
}

/* 统计总帧数：从动画起点迭代至 GIF trailer，并回到起点（加载第 0 帧）。 */
static int gif_frame_count(gd_GIF* gif)
{
    gd_rewind(gif);
    int n = 0;
    while (gd_get_frame(gif) == 1) n++;
    gd_rewind(gif);
    gd_get_frame(gif);  /* 预载第 0 帧到 canvas，供 render 使用 */
    return n;
}

/* 把第 idx 帧渲染进 rgb（RGB，width*height*3）。越界时渲染最后可用帧。 */
static void gif_render_index(gd_GIF* gif, int idx, unsigned char* rgb)
{
    gd_rewind(gif);
    if (idx < 0) idx = 0;
    for (int i = 0; i <= idx; i++)
    {
        if (gd_get_frame(gif) != 1) break; /* 已到 trailer */
    }
    gd_render_frame(gif, rgb);
}

/* 图形_动画_打开(名, 路径)：gif_open 存入槽表。返回 "1" / "0"。 */
VusString* vus_gui_anim_open(const char* name, const char* path)
{
    if (!name || !*name || !path || !*path) return vus_string_new("0");
    VusGifSlot* slot = find_gif(name);
    if (!slot)
    {
        for (int i = 0; i < VUS_GIF_MAX; i++)
        {
            if (!s_gifs[i].used) { slot = &s_gifs[i]; break; }
        }
    }
    if (!slot) return vus_string_new("0"); /* 槽表满 */
    if (slot->gif)
    {
        gd_close_gif(slot->gif);
        slot->gif = NULL;
    }
    gd_GIF* g = gd_open_gif(path);
    if (!g) return vus_string_new("0");
    memset(slot, 0, sizeof(*slot));
    strncpy(slot->name, name, sizeof(slot->name) - 1);
    slot->name[sizeof(slot->name) - 1] = '\0';
    slot->gif = g;
    slot->nframes = gif_frame_count(g);
    slot->frame_idx = 0;
    slot->used = 1;
    return vus_string_new("1");
}

/* 图形_动画_下一步(名, x, y)：推进一帧并在 (x,y) 以 GIF 原始尺寸绘制。 */
VusString* vus_gui_anim_next(const char* name, int x, int y)
{
    if (!s_initialized || !name) return vus_string_new("0");
    VusGifSlot* slot = find_gif(name);
    if (!slot || !slot->gif || slot->nframes <= 0) return vus_string_new("0");
    slot->frame_idx = (slot->frame_idx + 1) % slot->nframes;
    int w = (int)slot->gif->width, h = (int)slot->gif->height;
    unsigned char* rgb = (unsigned char*)malloc((size_t)w * (size_t)h * 3);
    if (!rgb) return vus_string_new("0");
    gif_render_index(slot->gif, slot->frame_idx, rgb);
    blit_rgb_stretch(x, y, w, h, rgb, w, h);
    free(rgb);
    vus_gui_mark_dirty();
    return vus_string_new("1");
}

/* 图形_动画_帧数(名)：返回总帧数整数字符串。 */
VusString* vus_gui_anim_frames(const char* name)
{
    if (!name) return vus_string_new("0");
    VusGifSlot* slot = find_gif(name);
    if (!slot || !slot->gif) return vus_string_new("0");
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", slot->nframes);
    return vus_string_new(buf);
}

/* 图形_动画_关闭(名)：关闭并释放槽。 */
VusString* vus_gui_anim_close(const char* name)
{
    VusGifSlot* slot = find_gif(name);
    if (!slot || !slot->gif) return vus_string_new("0");
    gd_close_gif(slot->gif);
    slot->gif = NULL;
    slot->used = 0;
    return vus_string_new("1");
}

/* =====================================================================
 * 阶段H：高级交互控件（统一控件表）
 * ===================================================================== */

/* 图形_滑块(名, x, y, 宽, 值[, 最小值, 最大值])：水平轨道 + 滑块块。
 * 点击（未消费）落在轨道矩形内按 x 换算新值写入控件。返回当前值整数串。 */
VusString* vus_gui_slider(const char* name, int x, int y, int w, int value, int minv, int maxv)
{
    if (!s_initialized || !name || !*name || w <= 0)
    {
        return vus_string_new("0");
    }
    VusControl* c = register_ctrl(name, CTRL_SLIDER);
    if (!c)
    {
        return vus_string_new("0");
    }
    const int sh = 16; /* 滑块控件高（固定） */
    c->x = x; c->y = y; c->w = w; c->h = sh;
    int mn = minv, mx = maxv;
    if (mn >= mx) mx = mn + 1;
    c->slider_min = mn; c->slider_max = mx;
    /* 首次被点击前以传入 value 为权威（touched 后内部为准，与复选框同法） */
    if (!c->touched)
    {
        c->slider_value = value;
    }
    if (c->slider_value < mn) c->slider_value = mn;
    if (c->slider_value > mx) c->slider_value = mx;
    /* 命中轨道 → 按 x 换算新值 */
    if (s_click_x >= 0 && s_click_y >= 0 && !s_click_consumed &&
        point_in(s_click_x, s_click_y, x, y, w, sh))
    {
        int nx = s_click_x - x;
        if (nx < 0) nx = 0;
        if (nx > w) nx = w;
        c->slider_value = mn + nx * (mx - mn) / w;
        c->touched = 1;
        s_click_consumed = 1;
        vus_gui_mark_dirty();
    }
    /* 绘制：轨道 + 滑块块 */
    vus_gui_surface_fill_rect(x, y, w, sh, argb_from_rgb(0xE0E0E0));
    vus_gui_surface_draw_rect(x, y, w, sh, argb_from_rgb(s_theme.border));
    int knob = 12;
    int kx = x + (c->slider_value - mn) * (w - knob) / (mx - mn);
    vus_gui_surface_fill_rect(kx, y + 2, knob, sh - 4, argb_from_rgb(s_theme.highlight));
    vus_gui_surface_draw_rect(kx, y + 2, knob, sh - 4, argb_from_rgb(s_theme.border));
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", c->slider_value);
    return vus_string_new(buf);
}

/* 图形_滑块值(名)：返回滑块当前值整数串。 */
VusString* vus_gui_slider_value(const char* name)
{
    VusControl* c;
    if (!s_initialized || !name || !(c = find_ctrl(name)) || c->type != CTRL_SLIDER)
    {
        return vus_string_new("0");
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", c->slider_value);
    return vus_string_new(buf);
}

/* 图形_开关(名, x, y[, 初始状态])：圆角底 + 圆形旋钮，点击切换。返回 "true"/"false"。 */
VusString* vus_gui_switch(const char* name, int x, int y, int state)
{
    if (!s_initialized || !name || !*name)
    {
        return vus_string_new("false");
    }
    VusControl* c = register_ctrl(name, CTRL_SWITCH);
    if (!c)
    {
        return vus_string_new("false");
    }
    const int sww = 40, swh = 20;
    c->x = x; c->y = y; c->w = sww; c->h = swh;
    if (!c->touched)
    {
        c->switch_state = (state != 0);
    }
    if (s_click_x >= 0 && s_click_y >= 0 && !s_click_consumed &&
        point_in(s_click_x, s_click_y, x, y, sww, swh))
    {
        c->switch_state = !c->switch_state;
        c->touched = 1;
        s_click_consumed = 1;
        vus_gui_mark_dirty();
    }
    /* 圆角底（开=绿 / 关=边框灰）+ 白色圆形旋钮 */
    fill_round_rect_px(x, y, sww, swh, swh / 2,
                       argb_from_rgb(c->switch_state ? 0x34B84E : 0xB0B0B0));
    int kx = c->switch_state ? (x + sww - swh) : x;
    fill_circle_px(kx + swh / 2, y + swh / 2, swh / 2 - 2, 0xFFFFFFFF);
    return vus_string_new(c->switch_state ? "true" : "false");
}

/* 图形_开关值(名)：返回 "true"/"false"。 */
VusString* vus_gui_switch_value(const char* name)
{
    VusControl* c;
    if (!s_initialized || !name || !(c = find_ctrl(name)) || c->type != CTRL_SWITCH)
    {
        return vus_string_new("false");
    }
    return vus_string_new(c->switch_state ? "true" : "false");
}

/* 图形_微调(名, x, y, 值[, 步长])：左减右加两个按钮 + 中间数值。返回当前值整数串。 */
VusString* vus_gui_spin(const char* name, int x, int y, int value, int step)
{
    if (!s_initialized || !name || !*name)
    {
        return vus_string_new("0");
    }
    VusControl* c = register_ctrl(name, CTRL_SPIN);
    if (!c)
    {
        return vus_string_new("0");
    }
    const int btn = 24, mid = 42, hgt = 20, wd = btn * 2 + mid;
    c->x = x; c->y = y; c->w = wd; c->h = hgt;
    c->spin_step = (step < 1) ? 1 : step;
    if (!c->touched)
    {
        c->spin_value = value;
    }
    if (s_click_x >= 0 && s_click_y >= 0 && !s_click_consumed)
    {
        if (point_in(s_click_x, s_click_y, x, y, btn, hgt))
        {
            c->spin_value -= c->spin_step;   /* 左半：减 */
            c->touched = 1;
            s_click_consumed = 1;
            vus_gui_mark_dirty();
        }
        else if (point_in(s_click_x, s_click_y, x + btn + mid, y, btn, hgt))
        {
            c->spin_value += c->spin_step;   /* 右半：加 */
            c->touched = 1;
            s_click_consumed = 1;
            vus_gui_mark_dirty();
        }
    }
    /* 绘制：左减钮 + 中间数值 + 右加钮 */
    vus_gui_surface_fill_rect(x, y, btn, hgt, argb_from_rgb(s_theme.highlight));
    vus_gui_surface_draw_rect(x, y, btn, hgt, argb_from_rgb(s_theme.border));
    draw_text_xy(x + (btn - 6) / 2, y + (hgt - 13) / 2, "-", s_theme.text);
    vus_gui_surface_fill_rect(x + btn + mid, y, btn, hgt, argb_from_rgb(s_theme.highlight));
    vus_gui_surface_draw_rect(x + btn + mid, y, btn, hgt, argb_from_rgb(s_theme.border));
    draw_text_xy(x + btn + mid + (btn - 6) / 2, y + (hgt - 13) / 2, "+", s_theme.text);
    vus_gui_surface_draw_rect(x + btn, y, mid, hgt, argb_from_rgb(s_theme.border));
    char vb[32];
    snprintf(vb, sizeof(vb), "%d", c->spin_value);
    draw_text_xy(x + btn + (mid - (int)strlen(vb) * 6) / 2, y + (hgt - 13) / 2, vb, s_theme.fg);
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", c->spin_value);
    return vus_string_new(buf);
}

/* 图形_微调值(名)：返回整数串。 */
VusString* vus_gui_spin_value(const char* name)
{
    VusControl* c;
    if (!s_initialized || !name || !(c = find_ctrl(name)) || c->type != CTRL_SPIN)
    {
        return vus_string_new("0");
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", c->spin_value);
    return vus_string_new(buf);
}

/* 图形_单选(名, x, y, 项高, 选项串, 选中索引)：选项用分号 ";" 分隔。
 * 绘制一列单选钮（圆圈 + 文字）。返回当前选中索引整数串。 */
VusString* vus_gui_radio(const char* name, int x, int y, int item_h,
                         const char* options, int sel)
{
    if (!s_initialized || !name || !*name || item_h < 10 || !options || !*options)
    {
        return vus_string_new("-1");
    }
    VusControl* c = register_ctrl(name, CTRL_RADIO);
    if (!c)
    {
        return vus_string_new("-1");
    }
    /* 用分号拆分为多选项 */
    c->radio_n = 0;
    const char* p = options;
    char tmp[256];
    while (*p && c->radio_n < 16)
    {
        while (*p == ';') p++;
        if (!*p) break;
        size_t k = 0;
        while (*p && *p != ';' && k < sizeof(tmp) - 1) tmp[k++] = *p++;
        tmp[k] = '\0';
        strncpy(c->radio_opts[c->radio_n], tmp, sizeof(c->radio_opts[0]) - 1);
        c->radio_opts[c->radio_n][sizeof(c->radio_opts[0]) - 1] = '\0';
        c->radio_n++;
    }
    int rows = (c->radio_n > 0) ? c->radio_n : 1;
    c->x = x; c->y = y;
    int w = 120; /* 控件登记宽（用于命中范围） */
    for (int i = 0; i < rows; i++) { int tw = (int)strlen(c->radio_opts[i]) * 6 + 22; if (tw > w) w = tw; }
    c->w = w; c->h = rows * item_h;
    if (!c->touched)
    {
        c->radio_sel = (sel >= 0) ? sel : 0;
    }
    if (c->radio_sel < 0) c->radio_sel = 0;
    if (c->radio_sel >= c->radio_n) c->radio_sel = c->radio_n - 1;
    /* 命中某选项行 → 更新选中（最大 48 行防御） */
    if (s_click_x >= 0 && s_click_y >= 0 && !s_click_consumed)
    {
        for (int i = 0; i < rows && i < 48; i++)
        {
            int ry = y + i * item_h;
            if (point_in(s_click_x, s_click_y, x, ry, w, item_h) &&
                point_in(s_click_x, s_click_y, c->x, c->y, c->w, c->h))
            {
                c->radio_sel = i;
                c->touched = 1;
                s_click_consumed = 1;
                vus_gui_mark_dirty();
                break;
            }
        }
    }
    /* 绘制：每行一个圆圈 + 选中填充 + 文字 */
    for (int i = 0; i < rows; i++)
    {
        int ry = y + i * item_h;
        int cyc = ry + item_h / 2;
        int sr = (item_h > 16) ? 6 : 5;
        draw_circle_px(x + sr, cyc, sr, argb_from_rgb(s_theme.border));
        if (i == c->radio_sel)
        {
            fill_circle_px(x + sr, cyc, sr - 2, argb_from_rgb(s_theme.highlight));
        }
        draw_text_xy(x + sr * 2 + 6, ry + (item_h - 13) / 2, c->radio_opts[i], s_theme.fg);
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", c->radio_sel);
    return vus_string_new(buf);
}

/* 图形_单选值(名)：返回选中索引整数串。 */
VusString* vus_gui_radio_value(const char* name)
{
    VusControl* c;
    if (!s_initialized || !name || !(c = find_ctrl(name)) || c->type != CTRL_RADIO)
    {
        return vus_string_new("-1");
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", c->radio_sel);
    return vus_string_new(buf);
}

/* =====================================================================
 * 阶段I：高级外观
 * ===================================================================== */

/* 图形_圆角矩形(…)：圆角外框。 */
VusString* vus_gui_round_rect(int x, int y, int w, int h, int radius, unsigned int color)
{
    if (!s_initialized || w <= 0 || h <= 0) return vus_string_new("0");
    if (radius < 0) radius = 0;
    round_rect_outline_px(x, y, w, h, radius, argb_from_rgb(color));
    vus_gui_mark_dirty();
    return vus_string_new("1");
}

/* 图形_圆角填充(…)：圆角实心。 */
VusString* vus_gui_round_fill(int x, int y, int w, int h, int radius, unsigned int color)
{
    if (!s_initialized || w <= 0 || h <= 0) return vus_string_new("0");
    if (radius < 0) radius = 0;
    fill_round_rect_px(x, y, w, h, radius, argb_from_rgb(color));
    vus_gui_mark_dirty();
    return vus_string_new("1");
}

/* 图形_画圆(圆心x, 圆心y, 半径, 颜色)：圆外框。 */
VusString* vus_gui_draw_circle(int cx, int cy, int r, unsigned int color)
{
    if (!s_initialized || r < 1) return vus_string_new("0");
    draw_circle_px(cx, cy, r, argb_from_rgb(color));
    vus_gui_mark_dirty();
    return vus_string_new("1");
}

/* 图形_填充圆(圆心x, 圆心y, 半径, 颜色)：实心圆。 */
VusString* vus_gui_fill_circle(int cx, int cy, int r, unsigned int color)
{
    if (!s_initialized || r < 1) return vus_string_new("0");
    fill_circle_px(cx, cy, r, argb_from_rgb(color));
    vus_gui_mark_dirty();
    return vus_string_new("1");
}

/* 图形_圆弧(圆心x, 圆心y, 半径, 起始角度, 跨角度, 颜色)：线宽 1。 */
VusString* vus_gui_draw_arc(int cx, int cy, int r, int start_deg, int sweep_deg,
                            unsigned int color)
{
    if (!s_initialized || r < 1) return vus_string_new("0");
    draw_arc_px(cx, cy, r, start_deg, sweep_deg, argb_from_rgb(color));
    vus_gui_mark_dirty();
    return vus_string_new("1");
}

/* 图形_外观(圆角半径[, 抗锯齿])：设置全局控件圆角半径（默认 0=直角）。
 * 抗锯齿参数接受但忽略。返回 "1"。 */
VusString* vus_gui_appearance(int radius, int aa)
{
    (void)aa;
    if (radius < 0) radius = 0;
    s_global_radius = radius;
    return vus_string_new("1");
}