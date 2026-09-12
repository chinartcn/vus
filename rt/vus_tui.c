/*
 * vus_tui.c — VUS 终端 UI 画布子系统（帧缓冲 + 差分刷新 + 输入）
 *
 * 并入 libvus_rt 编译（Makefile 增加 vus_tui.o）。
 * 无终端环境安全：isatty 判定，绘制静默、读取返回空串。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "vus_tui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <time.h>

/* 画布单元格：UTF-8 序列（≤7 字节 + NUL）+ 颜色 + 样式位 */
typedef struct {
    char ch[8];
    int  fg;    /* -1 默认 */
    int  bg;    /* -1 默认 */
    int  style; /* bit0=加粗 bit1=下划线 bit2=反显 */
} TuiCell;

static TuiCell s_cur[VUS_TUI_MAX_ROWS][VUS_TUI_MAX_COLS];
static TuiCell s_prev[VUS_TUI_MAX_ROWS][VUS_TUI_MAX_COLS];
static int s_rows = 24;
static int s_cols = 80;
static int s_has_prev = 0;   /* 首帧差分基准不存在 → 全量输出 */

static int tui_is_tty_out(void) { return isatty(STDOUT_FILENO); }
static int tui_is_tty_in(void)  { return isatty(STDIN_FILENO); }

/* ============ 参数钳制 ============ */

void vus_tui_color_clamp(int *fg, int *bg) {
    if (fg && *fg < 0) *fg = -1;
    else if (fg && *fg > 255) *fg = 255;
    if (bg && *bg < 0) *bg = -1;
    else if (bg && *bg > 255) *bg = 255;
}

/* ============ 画布 ============ */

int vus_tui_canvas_resize(int rows, int cols) {
    if (rows > 0 && rows <= VUS_TUI_MAX_ROWS) s_rows = rows;
    if (cols > 0 && cols <= VUS_TUI_MAX_COLS) s_cols = cols;
    s_has_prev = 0;   /* 尺寸变化后旧差分基准失效 */
    return 0;
}

void vus_tui_canvas_clear(void) {
    for (int r = 0; r < s_rows; r++) {
        for (int c = 0; c < s_cols; c++) {
            TuiCell *cell = &s_cur[r][c];
            cell->ch[0] = ' ';
            cell->ch[1] = '\0';
            cell->fg = -1;
            cell->bg = -1;
            cell->style = 0;
        }
    }
}

/* UTF-8 序列长度（不含 NUL）；非法字节按 1 处理 */
static int utf8_len(const char *p) {
    unsigned char b = (unsigned char)*p;
    if (b >= 0xF0) return 4;
    if (b >= 0xE0) return 3;
    if (b >= 0xC0) return 2;
    return 1;
}

void vus_tui_canvas_put(int row, int col, const char *utf8, int fg, int bg, int style) {
    if (!utf8 || row < 0 || row >= s_rows) return;
    vus_tui_color_clamp(&fg, &bg);
    int c = col;
    const char *p = utf8;
    while (*p && c < s_cols) {
        int len = utf8_len(p);
        if (c >= 0) {
            TuiCell *cell = &s_cur[row][c];
            int i;
            for (i = 0; i < len && p[i]; i++) cell->ch[i] = p[i];
            cell->ch[i] = '\0';
            cell->fg = fg;
            cell->bg = bg;
            cell->style = style;
        }
        p += len;
        c++;
    }
}

void vus_tui_canvas_line(int row, int col, int len, char ch) {
    if (row < 0 || row >= s_rows) return;
    for (int i = 0; i < len; i++) {
        int c = col + i;
        if (c < 0 || c >= s_cols) continue;
        TuiCell *cell = &s_cur[row][c];
        cell->ch[0] = ch;
        cell->ch[1] = '\0';
        cell->fg = -1;
        cell->bg = -1;
        cell->style = 0;
    }
}

void vus_tui_canvas_box(int row, int col, int h, int w, const char *title) {
    if (h < 2 || w < 2) return;
    const char *tl = "\xE2\x94\x8C";  /* ┌ */
    const char *tr = "\xE2\x94\x90";  /* ┐ */
    const char *bl = "\xE2\x94\x94";  /* └ */
    const char *br = "\xE2\x94\x98";  /* ┘ */
    const char *hz = "\xE2\x94\x80";  /* ─ */
    const char *vt = "\xE2\x94\x82";  /* │ */
    vus_tui_canvas_put(row, col, tl, -1, -1, 0);
    vus_tui_canvas_put(row, col + w - 1, tr, -1, -1, 0);
    vus_tui_canvas_put(row + h - 1, col, bl, -1, -1, 0);
    vus_tui_canvas_put(row + h - 1, col + w - 1, br, -1, -1, 0);
    for (int i = 1; i < w - 1; i++) {
        vus_tui_canvas_put(row, col + i, hz, -1, -1, 0);
        vus_tui_canvas_put(row + h - 1, col + i, hz, -1, -1, 0);
    }
    for (int r = 1; r < h - 1; r++) {
        vus_tui_canvas_put(row + r, col, vt, -1, -1, 0);
        vus_tui_canvas_put(row + r, col + w - 1, vt, -1, -1, 0);
    }
    /* 标题居中于顶线（标题覆盖顶线中段） */
    if (title && title[0]) {
        int tlen = 0;
        for (const char *q = title; *q; q += utf8_len(q)) tlen++;
        int tcol = col + (w - tlen) / 2;
        vus_tui_canvas_put(row, tcol, title, 33, -1, 1);  /* 蓝色加粗 */
    }
}

/* 单元格终端显示宽度（列）：全角字符（中文/假名/谚文/全角符号）占 2 列，其余占 1 列。
 * 差分刷新的定位必须按「终端列」而非「画布列」计算：画布每个单元格记 1 列，但全角
 * 字符在终端实际占 2 列；仅按画布列定位会在全角字符之后错位，把变更内容写到前一
 * 字符上面（覆盖其右半）。边框/制表符（U+2500 区，如 ┌─┐│）在终端为 1 列，不判为全角。 */
static int tui_cell_width(const char *ch) {
    if (!ch || !ch[0]) return 1;
    unsigned char b0 = (unsigned char)ch[0];
    if (b0 < 0x80) return 1;                    /* ASCII */
    if ((b0 & 0xE0) == 0xC0) return 1;          /* 2 字节：无全角 */
    unsigned cp;
    if ((b0 & 0xF0) == 0xE0) {                  /* 3 字节 */
        cp = ((unsigned)(b0 & 0x0F) << 12) | ((unsigned)((unsigned char)ch[1] & 0x3F) << 6)
           | (unsigned)((unsigned char)ch[2] & 0x3F);
    } else if ((b0 & 0xF8) == 0xF0) {           /* 4 字节：emoji 等按近似全角 */
        return 2;
    } else {
        return 1;
    }
    if (cp >= 0x3000 && cp <= 0x30FF) return 2; /* CJK 标点/假名 */
    if (cp >= 0x3400 && cp <= 0x9FFF) return 2; /* CJK 统一表意（含扩展 A） */
    if (cp >= 0xAC00 && cp <= 0xD7A3) return 2; /* 谚文音节 */
    if (cp >= 0xF900 && cp <= 0xFAFF) return 2; /* CJK 兼容表意 */
    if (cp >= 0xFE30 && cp <= 0xFE4F) return 2; /* 竖排变体 */
    if (cp >= 0xFF00 && cp <= 0xFF60) return 2; /* 全角 ASCII */
    if (cp >= 0xFFE0 && cp <= 0xFFE6) return 2; /* 全角符号 */
    return 1;
}

/* 差分上屏：只输出与显示缓冲不同的单元格；颜色/样式变化才发 ANSI。
 * 定位按「终端列」累计：光标初始在 (0,0)（\033[H 后）。每行逐格累计已占用终端列
 * （被跳过的未变更单元格同样占宽，计入列位置），变更单元格若非恰在当前光标处则发
 * \033[%d;%dH 定位；连续单元格不重复定位保持差分输出紧凑。全角字符占 2 列，由
 * tui_cell_width 计入，避免在全角字符之后的变更落到错误列（覆盖前一字符）。 */
void vus_tui_flush(void) {
    if (!tui_is_tty_out()) {
        /* 非终端：仅推进差分基准（输出被管道/文件吞掉无意义） */
        memcpy(s_prev, s_cur, sizeof(s_cur));
        s_has_prev = 1;
        return;
    }
    int fg = -1, bg = -1, st = -1;
    int cur_r = 0, cur_c = 0;   /* 逻辑光标（0 基）：\033[H 归位后位于 (0,0) */
    fputs("\033[?25l\033[H", stdout);   /* 隐藏光标 + 归位 */
    for (int r = 0; r < s_rows; r++) {
        int term_col = 0;       /* 本行已占用终端列：下一变更单元格的应处位置 */
        for (int c = 0; c < s_cols; c++) {
            TuiCell *n = &s_cur[r][c];
            if (s_has_prev && memcmp(n, &s_prev[r][c], sizeof(*n)) == 0) {
                term_col += tui_cell_width(n->ch);   /* 未变更格上一帧已上屏，占宽照计 */
                continue;
            }
            if (r != cur_r || term_col != cur_c)
                fprintf(stdout, "\033[%d;%dH", r + 1, term_col + 1);
            /* 颜色/样式切换优化 */
            if (n->fg != fg || n->bg != bg || n->style != st) {
                fputs("\033[0m", stdout);
                if (n->style & 1) fputs("\033[1m", stdout);
                if (n->style & 2) fputs("\033[4m", stdout);
                if (n->style & 4) fputs("\033[7m", stdout);
                if (n->style & 8) fputs("\033[5m", stdout);
                if (n->fg >= 0) fprintf(stdout, "\033[38;5;%dm", n->fg);
                if (n->bg >= 0) fprintf(stdout, "\033[48;5;%dm", n->bg);
                fg = n->fg; bg = n->bg; st = n->style;
            }
            fputs(n->ch, stdout);
            cur_r = r;
            cur_c = term_col + tui_cell_width(n->ch);
            term_col = cur_c;
        }
    }
    fputs("\033[0m\033[?25h", stdout);  /* 复位 + 显示光标 */
    fflush(stdout);
    memcpy(s_prev, s_cur, sizeof(s_cur));
    s_has_prev = 1;
}

/* ============ 终端交互 ============ */

int vus_tui_rows(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) return ws.ws_row;
    return 24;
}

int vus_tui_cols(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
    return 80;
}

void vus_tui_cursor(int show) {
    fputs(show ? "\033[?25h" : "\033[?25l", stdout);
    fflush(stdout);
}

/* 流式样式：立即输出 ANSI（供 tui_文本样式），style 位与画布一致 */
void vus_tui_style(int style) {
    fputs("\033[0m", stdout);
    if (style & 1) fputs("\033[1m", stdout);
    if (style & 2) fputs("\033[4m", stdout);
    if (style & 4) fputs("\033[7m", stdout);
    if (style & 8) fputs("\033[5m", stdout);
    fflush(stdout);
}

void vus_tui_clear_line(int row) {
    /* 画布行清为空格 */
    if (row >= 0 && row < s_rows) {
        for (int c = 0; c < s_cols; c++) {
            TuiCell *cell = &s_cur[row][c];
            cell->ch[0] = ' ';
            cell->ch[1] = '\0';
            cell->fg = -1;
            cell->bg = -1;
            cell->style = 0;
        }
    }
    /* 终端行立即清除 */
    if (tui_is_tty_out()) {
        if (row >= 0) fprintf(stdout, "\033[%d;1H\033[K", row + 1);
        fflush(stdout);
    }
}

/* ============ 输入 ============ */

static int tui_raw_on(struct termios *save) {
    struct termios raw;
    if (tcgetattr(STDIN_FILENO, save) != 0) return -1;
    raw = *save;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return -1;
    return 0;
}

static void tui_raw_off(const struct termios *save) {
    tcsetattr(STDIN_FILENO, TCSANOW, save);
}

/* 非阻塞读键：组合 ESC 前缀序列（方向键等），返回 UTF-8 字符串 */
VusString *vus_tui_read_key(void) {
    if (!tui_is_tty_in()) return vus_string_new("");
    struct termios save;
    if (tui_raw_on(&save) != 0) return vus_string_new("");
    unsigned char buf[17];   /* 16 字节数据 + NUL（buf[16]='\0' 需在界内） */
    int n = (int)read(STDIN_FILENO, buf, 16);
    tui_raw_off(&save);
    if (n <= 0) return vus_string_new("");
    /* ESC 前缀：补齐剩余序列（如 "\033[A"），微等待下一字节 */
    if (buf[0] == 0x1B && n < 16) {
        struct termios save2;
        if (tui_raw_on(&save2) == 0) {
            int more = (int)read(STDIN_FILENO, buf + n, 16 - n);
            tui_raw_off(&save2);
            if (more > 0) n += more;
        }
    }
    buf[n] = '\0';
    return vus_string_new((const char *)buf);
}

/* 行编辑：提示 + 逐字符回显 + 退格 + 回车结束；CTRL-C 放弃 */
VusString *vus_tui_input(const char *prompt, int maxlen) {
    if (!tui_is_tty_in()) return vus_string_new("");
    if (maxlen <= 0) maxlen = 64;
    if (maxlen > 512) maxlen = 512;
    struct termios save;
    if (tui_raw_on(&save) != 0) return vus_string_new("");
    if (prompt) { fputs(prompt, stdout); fflush(stdout); }
    char *buf = (char *)malloc((size_t)maxlen + 1);
    if (!buf) { tui_raw_off(&save); return vus_string_new(""); }
    int len = 0;
    for (;;) {
        unsigned char b;
        ssize_t n = read(STDIN_FILENO, &b, 1);
        if (n <= 0) {
            if (len == 0) { free(buf); tui_raw_off(&save); return vus_string_new(""); }
            /* 非阻塞读无输入：让出 CPU 1ms，避免已输入字符时忙等空转烧满单核 */
            struct timespec ts = {0, 1000000};
            nanosleep(&ts, NULL);
            continue;
        }
        if (b == '\r' || b == '\n') break;
        if (b == 3) {  /* CTRL-C：放弃 */
            len = 0;
            break;
        }
        if (b == 127 || b == 8) {  /* 退格 */
            if (len > 0) {
                /* 回退一个 UTF-8 字符 */
                int i = len - 1;
                while (i > 0 && ((unsigned char)buf[i] & 0xC0) == 0x80) i--;
                len = i;
                fputs("\b \b", stdout);
                fflush(stdout);
            }
            continue;
        }
        if (b < 0x20 && b != 0x1B) continue;  /* 忽略其他控制符（ESC 走下方） */
        if (b == 0x1B) continue;              /* 方向键等序列暂忽略 */
        if (len < maxlen) {
            buf[len++] = (char)b;
            putchar(b);
            fflush(stdout);
        }
    }
    tui_raw_off(&save);
    if (prompt) fputs("\n", stdout);
    buf[len] = '\0';
    VusString *out = vus_string_new(buf);
    free(buf);
    return out;
}
