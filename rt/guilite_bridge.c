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

/* 最近一次点击坐标（-1 = 尚无点击），供命中检测使用。 */
static int s_click_x = -1;
static int s_click_y = -1;
/* 点击消费标记：棋盘格等需要“仅在本次点击生效一次”的控件读取后置位，
 * 避免主循环每帧读到同一点击而反复切换状态。poll / emit 时清零。 */
static int s_click_consumed = 0;

/* ===== 统一控件表（阶段3：控件库） =====
 * 按钮/标签/文本框/复选框/进度条/列表/画布 共用同一张表与同一套命中检测。 */
#define VUS_CTRL_MAX    128
#define VUS_LIST_LINES  32
#define VUS_LINE_MAX    96
typedef enum {
    CTRL_BUTTON, CTRL_LABEL, CTRL_TEXTBOX,
    CTRL_CHECKBOX, CTRL_PROGRESS, CTRL_LIST, CTRL_CANVAS
} VusCtrlType;
typedef struct {
    char name[64];
    VusCtrlType type;
    int x, y, w, h;
    int row_h;                          /* 列表每行像素高 */
    char lines[VUS_LIST_LINES][VUS_LINE_MAX]; /* 列表每行文本 */
    int line_cnt;
    int checked;                        /* 复选框 */
    int progress;                       /* 进度条 0-100 */
    int rel_x, rel_y;                   /* 画布最近命中相对坐标 */
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

/* 文本绘制辅助：优先 X11（方向正常），失败回退 GuiLite 帧缓冲。 */
static void draw_text_xy(int x, int y, const char* text, unsigned int rgb)
{
    if (!text) return;
    if (vus_gui_platform_draw_text(x, y, text, rgb) != 1)
        vus_gui_surface_draw_text(x, y, text, argb_from_rgb(rgb));
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

    /* 默认配色：蓝底 + 深蓝边框 + 白字（居中） */
    vus_gui_surface_fill_rect(x, y, w, h, argb_from_rgb(0x3399CC));
    vus_gui_surface_draw_rect(x, y, w, h, argb_from_rgb(0x0A2A3A));
    int tw = text ? (int)(strlen(text) * 6) : 0; /* 估宽：6px/字符（X 6x13 字体） */
    int tx = x + (tw < w ? (w - tw) / 2 : 0);
    int ty = y + (h - 13) / 2;
    draw_text_xy(tx, ty, text ? text : "", 0xFFFFFF);
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
    VusControl* c;
    if (!s_initialized || !name || s_click_x < 0 || s_click_y < 0 ||
        !(c = find_ctrl(name)) || c->type != CTRL_BUTTON)
    {
        return vus_string_new("false");
    }
    return vus_string_new(point_in(s_click_x, s_click_y, c->x, c->y, c->w, c->h)
                          ? "true" : "false");
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
    /* 点击命中方格/文本区且本次点击未消费 → 切换 */
    if (s_click_x >= 0 && s_click_y >= 0 && !s_click_consumed &&
        point_in(s_click_x, s_click_y, x, y, c->w, box))
    {
        checked = !checked;
        c->checked = checked;
        s_click_consumed = 1;
    }
    else
    {
        c->checked = checked;
    }
    /* 绘制：方格边框 +（勾选时）内部填充 + 文本 */
    vus_gui_surface_draw_rect(x, y, box, box, argb_from_rgb(0x333333));
    if (checked)
    {
        vus_gui_surface_fill_rect(x + 2, y + 2, box - 4, box - 4, argb_from_rgb(0x3399CC));
    }
    if (text)
    {
        draw_text_xy(x + box + 4, y + (box - 13) / 2, text, 0x000000);
    }
    return vus_string_new(checked ? "true" : "false");
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

/* 列表行命中：最近一次点击是否落在 name 列表的第 line 行内。 */
VusString* vus_gui_list_row_clicked(const char* name, int line)
{
    VusControl* c = find_ctrl(name);
    if (!s_initialized || !c || c->type != CTRL_LIST || line < 0 ||
        s_click_x < 0 || s_click_y < 0)
    {
        return vus_string_new("false");
    }
    int ry = c->y + line * c->row_h;
    int in = point_in(s_click_x, s_click_y, c->x, ry, c->w, c->row_h);
    return vus_string_new(in ? "true" : "false");
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