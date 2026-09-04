/*
 * gen_builtin_gui.c — 图形/GUI + 传感器 + 音频 + TUI 内置函数映射
 *
 * 处理：图形_xx/传感器_xx/时钟/音频_xx/tui_xx
 * 从 generator.c 的 gen_expr_call 中拆分而来。
 */

#define _GNU_SOURCE
#include "generator.h"
#include "gen_builtin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *gen_builtin_gui(GenBuf *buf, VusAstCall *call) {
    /* ============= GuiLite 图形内置函数 ============= */
    if (strcmp(call->func_name, "图形_初始化") == 0) {
        if (call->args && call->args->count >= 3) {
            char *w = gen_expr(buf, call->args->items[0]);
            char *h = gen_expr(buf, call->args->items[1]);
            char *t = gen_expr(buf, call->args->items[2]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_init((int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), vus_string_cstr(%s))",
                w, h, t);
            free(w); free(h); free(t);
            g_uses_gui = 1;
            return strdup(result);
        }
    }

    /* ============= VUS XYZ 体感音游内建 ============= */
    if (strcmp(call->func_name, "传感器_读") == 0) {
        if (call->args && call->args->count >= 1) {
            char *a = gen_expr(buf, call->args->items[0]);
            char result[512];
            snprintf(result, sizeof(result), "vus_sensor_read(vus_string_cstr(%s))", a);
            free(a);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "时钟") == 0) {
        return strdup("vus_clock_ms()");
    }
    if (strcmp(call->func_name, "音频_打开") == 0) {
        if (call->args && call->args->count >= 1) {
            char *a = gen_expr(buf, call->args->items[0]);
            char result[512];
            snprintf(result, sizeof(result), "vus_audio_open(vus_string_cstr(%s))", a);
            free(a);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "音频_播放") == 0) {
        return strdup("vus_audio_play()");
    }
    if (strcmp(call->func_name, "音频_暂停") == 0) {
        return strdup("vus_audio_pause()");
    }
    if (strcmp(call->func_name, "音频_续") == 0) {
        return strdup("vus_audio_resume()");
    }
    if (strcmp(call->func_name, "音频_跳转") == 0) {
        if (call->args && call->args->count >= 1) {
            char *a = gen_expr(buf, call->args->items[0]);
            char result[512];
            snprintf(result, sizeof(result), "vus_audio_seek((int)vus_to_int(%s, &_err))", a);
            free(a);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "音频_进度") == 0) {
        return strdup("vus_audio_position()");
    }
    if (strcmp(call->func_name, "音频_时长") == 0) {
        return strdup("vus_audio_duration()");
    }

    /* ===== 基础绘图 ===== */
    if (strcmp(call->func_name, "图形_画点") == 0) {
        if (call->args && call->args->count >= 3) {
            char *x = gen_expr(buf, call->args->items[0]);
            char *y = gen_expr(buf, call->args->items[1]);
            char *c = gen_expr(buf, call->args->items[2]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_draw_pixel((int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (unsigned int)vus_to_int(%s, &_err))",
                x, y, c);
            free(x); free(y); free(c);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_画线") == 0) {
        if (call->args && call->args->count >= 5) {
            char *x1 = gen_expr(buf, call->args->items[0]);
            char *y1 = gen_expr(buf, call->args->items[1]);
            char *x2 = gen_expr(buf, call->args->items[2]);
            char *y2 = gen_expr(buf, call->args->items[3]);
            char *c  = gen_expr(buf, call->args->items[4]);
            char *wd  = (call->args->count > 5) ? gen_expr(buf, call->args->items[5]) : strdup("vus_to_string(1)");
            char *ds  = (call->args->count > 6) ? gen_expr(buf, call->args->items[6]) : strdup("vus_to_string(0)");
            char *ar  = (call->args->count > 7) ? gen_expr(buf, call->args->items[7]) : strdup("vus_to_string(0)");
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_draw_line_ex((int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (unsigned int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err))",
                x1, y1, x2, y2, c, wd, ds, ar);
            free(x1); free(y1); free(x2); free(y2); free(c);
            free(wd); free(ds); free(ar);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_矩形") == 0) {
        if (call->args && call->args->count >= 5) {
            char *x = gen_expr(buf, call->args->items[0]);
            char *y = gen_expr(buf, call->args->items[1]);
            char *wd = gen_expr(buf, call->args->items[2]);
            char *ht = gen_expr(buf, call->args->items[3]);
            char *c  = gen_expr(buf, call->args->items[4]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_draw_rect((int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (unsigned int)vus_to_int(%s, &_err))",
                x, y, wd, ht, c);
            free(x); free(y); free(wd); free(ht); free(c);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_填充") == 0) {
        if (call->args && call->args->count >= 5) {
            char *x = gen_expr(buf, call->args->items[0]);
            char *y = gen_expr(buf, call->args->items[1]);
            char *wd = gen_expr(buf, call->args->items[2]);
            char *ht = gen_expr(buf, call->args->items[3]);
            char *c  = gen_expr(buf, call->args->items[4]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_fill_rect((int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (unsigned int)vus_to_int(%s, &_err))",
                x, y, wd, ht, c);
            free(x); free(y); free(wd); free(ht); free(c);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_文字") == 0) {
        if (call->args && call->args->count >= 4) {
            char *x = gen_expr(buf, call->args->items[0]);
            char *y = gen_expr(buf, call->args->items[1]);
            char *txt = gen_expr(buf, call->args->items[2]);
            char *c  = gen_expr(buf, call->args->items[3]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_draw_text((int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), vus_string_cstr(%s), (unsigned int)vus_to_int(%s, &_err))",
                x, y, txt, c);
            free(x); free(y); free(txt); free(c);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_刷新") == 0) {
        g_uses_gui = 1;
        return strdup("vus_gui_redraw()");
    }
    if (strcmp(call->func_name, "图形_保持") == 0) {
        g_uses_gui = 1;
        return strdup("vus_gui_run()");
    }

    /* 图形_MD */
    if (strcmp(call->func_name, "图形_MD") == 0) {
        if (call->args && call->args->count >= 4) {
            char *x = gen_expr(buf, call->args->items[0]);
            char *y = gen_expr(buf, call->args->items[1]);
            char *w = gen_expr(buf, call->args->items[2]);
            char *txt = gen_expr(buf, call->args->items[3]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_md((int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), vus_string_cstr(%s))",
                x, y, w, txt);
            free(x); free(y); free(w); free(txt);
            g_uses_gui = 1;
            return strdup(result);
        }
    }

    /* 滚动容器 */
    if (strcmp(call->func_name, "图形_滚动容器") == 0) {
        if (call->args && call->args->count >= 6) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char *x  = gen_expr(buf, call->args->items[1]);
            char *y  = gen_expr(buf, call->args->items[2]);
            char *w  = gen_expr(buf, call->args->items[3]);
            char *h  = gen_expr(buf, call->args->items[4]);
            char *ch = gen_expr(buf, call->args->items[5]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_scroll_begin(vus_string_cstr(%s), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err))",
                nm, x, y, w, h, ch);
            free(nm); free(x); free(y); free(w); free(h); free(ch);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_滚动容器滚") == 0) {
        if (call->args && call->args->count >= 2) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char *dy = gen_expr(buf, call->args->items[1]);
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_scroll_delta(vus_string_cstr(%s), (int)vus_to_int(%s, &_err))",
                nm, dy);
            free(nm); free(dy);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_滚动容器偏移") == 0) {
        if (call->args && call->args->count >= 1) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_scroll_offset(vus_string_cstr(%s))", nm);
            free(nm);
            g_uses_gui = 1;
            return strdup(result);
        }
    }

    /* 背景图 */
    if (strcmp(call->func_name, "图形_背景图") == 0) {
        if (call->args && call->args->count >= 5) {
            char *x = gen_expr(buf, call->args->items[0]);
            char *y = gen_expr(buf, call->args->items[1]);
            char *w = gen_expr(buf, call->args->items[2]);
            char *h = gen_expr(buf, call->args->items[3]);
            char *path = gen_expr(buf, call->args->items[4]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_draw_png((int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), vus_string_cstr(%s))",
                x, y, w, h, path);
            free(x); free(y); free(w); free(h); free(path);
            g_uses_gui = 1;
            g_uses_png = 1;
            return strdup(result);
        }
    }

    /* 字体 */
    if (strcmp(call->func_name, "图形_字体_加载") == 0) {
        if (call->args && call->args->count >= 2) {
            char *path = gen_expr(buf, call->args->items[0]);
            char *sz   = gen_expr(buf, call->args->items[1]);
            char result[2048];
            snprintf(result, sizeof(result),
                "vus_gui_font(vus_string_cstr(%s), (int)vus_to_int(%s, &_err))",
                path, sz);
            free(path); free(sz);
            g_uses_gui = 1;
            g_uses_freetype = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_字体已加载") == 0) {
        g_uses_gui = 1;
        g_uses_freetype = 1;
        return strdup("vus_gui_font_loaded()");
    }

    /* ===== 页面导航 ===== */
    if (strcmp(call->func_name, "图形_页面_打开") == 0) {
        if (call->args && call->args->count >= 1) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char result[2048];
            snprintf(result, sizeof(result), "vus_gui_page_open(vus_string_cstr(%s))", nm);
            free(nm);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_页面_返回") == 0) {
        g_uses_gui = 1;
        return strdup("vus_gui_page_back()");
    }
    if (strcmp(call->func_name, "图形_页面_当前") == 0) {
        g_uses_gui = 1;
        return strdup("vus_gui_page_current()");
    }
    if (strcmp(call->func_name, "图形_页面_绘制") == 0) {
        g_uses_gui = 1;
        return strdup("vus_gui_page_draw()");
    }

    /* ===== 基本控件 ===== */
    if (strcmp(call->func_name, "图形_按钮") == 0) {
        if (call->args && call->args->count >= 6) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char *x  = gen_expr(buf, call->args->items[1]);
            char *y  = gen_expr(buf, call->args->items[2]);
            char *wd = gen_expr(buf, call->args->items[3]);
            char *ht = gen_expr(buf, call->args->items[4]);
            char *tx = gen_expr(buf, call->args->items[5]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_button(vus_string_cstr(%s), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), vus_string_cstr(%s))",
                nm, x, y, wd, ht, tx);
            free(nm); free(x); free(y); free(wd); free(ht); free(tx);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_取事件") == 0) {
        g_uses_gui = 1;
        return strdup("vus_gui_poll()");
    }

    /* ===== X11 多输入 ===== */
    if (strcmp(call->func_name, "图形_按键") == 0) {
        g_uses_gui = 1;
        return strdup("vus_gui_last_key()");
    }
    if (strcmp(call->func_name, "图形_按键码") == 0) {
        g_uses_gui = 1;
        return strdup("vus_gui_last_keycode()");
    }
    if (strcmp(call->func_name, "图形_鼠标位置") == 0) {
        g_uses_gui = 1;
        return strdup("vus_gui_mouse_pos()");
    }
    if (strcmp(call->func_name, "图形_鼠标x") == 0) {
        g_uses_gui = 1;
        return strdup("vus_gui_mouse_x()");
    }
    if (strcmp(call->func_name, "图形_鼠标y") == 0) {
        g_uses_gui = 1;
        return strdup("vus_gui_mouse_y()");
    }
    if (strcmp(call->func_name, "图形_滚轮") == 0) {
        g_uses_gui = 1;
        return strdup("vus_gui_wheel()");
    }
    if (strcmp(call->func_name, "图形_键按下") == 0) {
        if (call->args && call->args->count >= 1) {
            char *b = gen_expr(buf, call->args->items[0]);
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_button_pressed((int)vus_to_int(%s, &_err))", b);
            free(b);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_悬停") == 0) {
        if (call->args && call->args->count >= 1) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_hover(vus_string_cstr(%s))", nm);
            free(nm);
            g_uses_gui = 1;
            return strdup(result);
        }
    }

    /* ===== 主题 ===== */
    if (strcmp(call->func_name, "图形_主题") == 0) {
        int n = call->args ? call->args->count : 0;
        char* a[5] = { 0 };
        for (int i = 0; i < 5 && i < n; i++)
            a[i] = gen_expr(buf, call->args->items[i]);
        char t[5][128] = { {0} };
        for (int i = 0; i < 5; i++) {
            if (a[i])
                snprintf(t[i], sizeof(t[i]), "(int)vus_to_int(%s, &_err)", a[i]);
            else
                snprintf(t[i], sizeof(t[i]), "-1");
        }
        char result[4096];
        snprintf(result, sizeof(result),
            "vus_gui_set_theme(%s, %s, %s, %s, %s)",
            t[0], t[1], t[2], t[3], t[4]);
        for (int i = 0; i < 5; i++) if (a[i]) free(a[i]);
        g_uses_gui = 1;
        return strdup(result);
    }

    /* ===== 组合模板 ===== */
    if (strcmp(call->func_name, "图形_卡片") == 0 || strcmp(call->func_name, "图形_面板") == 0) {
        if (call->args && call->args->count >= 6) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char *x  = gen_expr(buf, call->args->items[1]);
            char *y  = gen_expr(buf, call->args->items[2]);
            char *wd = gen_expr(buf, call->args->items[3]);
            char *ht = gen_expr(buf, call->args->items[4]);
            char *ti = gen_expr(buf, call->args->items[5]);
            char result[4096];
            const char* fn = (strcmp(call->func_name, "图形_卡片") == 0) ? "vus_gui_card" : "vus_gui_panel";
            snprintf(result, sizeof(result),
                "%s(vus_string_cstr(%s), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), vus_string_cstr(%s))",
                fn, nm, x, y, wd, ht, ti);
            free(nm); free(x); free(y); free(wd); free(ht); free(ti);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_表单行") == 0) {
        if (call->args && call->args->count >= 6) {
            char *nm  = gen_expr(buf, call->args->items[0]);
            char *lb  = gen_expr(buf, call->args->items[1]);
            char *x   = gen_expr(buf, call->args->items[2]);
            char *y   = gen_expr(buf, call->args->items[3]);
            char *wd  = gen_expr(buf, call->args->items[4]);
            char *tx  = gen_expr(buf, call->args->items[5]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_form_row(vus_string_cstr(%s), vus_string_cstr(%s), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), vus_string_cstr(%s))",
                nm, lb, x, y, wd, tx);
            free(nm); free(lb); free(x); free(y); free(wd); free(tx);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_行点击") == 0) {
        if (call->args && call->args->count >= 1) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_row_clicked(vus_string_cstr(%s))", nm);
            free(nm);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_圆环") == 0) {
        if (call->args && call->args->count >= 5) {
            char *nm  = gen_expr(buf, call->args->items[0]);
            char *x   = gen_expr(buf, call->args->items[1]);
            char *y   = gen_expr(buf, call->args->items[2]);
            char *r   = gen_expr(buf, call->args->items[3]);
            char *pct = gen_expr(buf, call->args->items[4]);
            char *col = (call->args->count >= 6) ? gen_expr(buf, call->args->items[5]) : NULL;
            char result[4096];
            if (col) {
                snprintf(result, sizeof(result),
                    "vus_gui_ring(vus_string_cstr(%s), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err))",
                    nm, x, y, r, pct, col);
            } else {
                snprintf(result, sizeof(result),
                    "vus_gui_ring(vus_string_cstr(%s), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), -1)",
                    nm, x, y, r, pct);
            }
            free(nm); free(x); free(y); free(r); free(pct); if (col) free(col);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_按钮点击") == 0 || strcmp(call->func_name, "按钮被点击") == 0) {
        if (call->args && call->args->count >= 1) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_button_clicked(vus_string_cstr(%s))", nm);
            free(nm);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_模拟点击") == 0) {
        if (call->args && call->args->count >= 2) {
            char *x = gen_expr(buf, call->args->items[0]);
            char *y = gen_expr(buf, call->args->items[1]);
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_sim_click((int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err))",
                x, y);
            free(x); free(y);
            g_uses_gui = 1;
            return strdup(result);
        }
    }

    /* ===== 控件库 ===== */
    if (strcmp(call->func_name, "图形_标签") == 0) {
        if (call->args && call->args->count >= 5) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char *x  = gen_expr(buf, call->args->items[1]);
            char *y  = gen_expr(buf, call->args->items[2]);
            char *tx = gen_expr(buf, call->args->items[3]);
            char *c  = gen_expr(buf, call->args->items[4]);
            char result[2048];
            snprintf(result, sizeof(result),
                "vus_gui_label(vus_string_cstr(%s), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), vus_string_cstr(%s), (unsigned int)vus_to_int(%s, &_err))",
                nm, x, y, tx, c);
            free(nm); free(x); free(y); free(tx); free(c);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_文本框") == 0) {
        if (call->args && call->args->count >= 6) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char *x  = gen_expr(buf, call->args->items[1]);
            char *y  = gen_expr(buf, call->args->items[2]);
            char *wd = gen_expr(buf, call->args->items[3]);
            char *ht = gen_expr(buf, call->args->items[4]);
            char *tx = gen_expr(buf, call->args->items[5]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_textbox(vus_string_cstr(%s), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), vus_string_cstr(%s))",
                nm, x, y, wd, ht, tx);
            free(nm); free(x); free(y); free(wd); free(ht); free(tx);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_复选框") == 0) {
        if (call->args && call->args->count >= 5) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char *x  = gen_expr(buf, call->args->items[1]);
            char *y  = gen_expr(buf, call->args->items[2]);
            char *tx = gen_expr(buf, call->args->items[3]);
            char *ck = gen_expr(buf, call->args->items[4]);
            char result[2048];
            snprintf(result, sizeof(result),
                "vus_gui_checkbox(vus_string_cstr(%s), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), vus_string_cstr(%s), (int)vus_to_int(%s, &_err))",
                nm, x, y, tx, ck);
            free(nm); free(x); free(y); free(tx); free(ck);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_进度") == 0) {
        if (call->args && call->args->count >= 6) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char *x  = gen_expr(buf, call->args->items[1]);
            char *y  = gen_expr(buf, call->args->items[2]);
            char *wd = gen_expr(buf, call->args->items[3]);
            char *ht = gen_expr(buf, call->args->items[4]);
            char *vl = gen_expr(buf, call->args->items[5]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_progress(vus_string_cstr(%s), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err))",
                nm, x, y, wd, ht, vl);
            free(nm); free(x); free(y); free(wd); free(ht); free(vl);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_列表") == 0) {
        if (call->args && call->args->count >= 6) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char *x  = gen_expr(buf, call->args->items[1]);
            char *y  = gen_expr(buf, call->args->items[2]);
            char *wd = gen_expr(buf, call->args->items[3]);
            char *ht = gen_expr(buf, call->args->items[4]);
            char *rh = gen_expr(buf, call->args->items[5]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_list(vus_string_cstr(%s), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err))",
                nm, x, y, wd, ht, rh);
            free(nm); free(x); free(y); free(wd); free(ht); free(rh);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_列表行") == 0) {
        if (call->args && call->args->count >= 3) {
            char *nm   = gen_expr(buf, call->args->items[0]);
            char *line = gen_expr(buf, call->args->items[1]);
            char *tx   = gen_expr(buf, call->args->items[2]);
            char result[2048];
            snprintf(result, sizeof(result),
                "vus_gui_list_row(vus_string_cstr(%s), (int)vus_to_int(%s, &_err), vus_string_cstr(%s))",
                nm, line, tx);
            free(nm); free(line); free(tx);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_列表选中") == 0) {
        if (call->args && call->args->count >= 1) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_list_selected(vus_string_cstr(%s))", nm);
            free(nm);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_列表行点击") == 0) {
        if (call->args && call->args->count >= 2) {
            char *nm   = gen_expr(buf, call->args->items[0]);
            char *line = gen_expr(buf, call->args->items[1]);
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_list_row_clicked(vus_string_cstr(%s), (int)vus_to_int(%s, &_err))",
                nm, line);
            free(nm); free(line);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_画布") == 0) {
        if (call->args && call->args->count >= 5) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char *x  = gen_expr(buf, call->args->items[1]);
            char *y  = gen_expr(buf, call->args->items[2]);
            char *wd = gen_expr(buf, call->args->items[3]);
            char *ht = gen_expr(buf, call->args->items[4]);
            char result[2048];
            snprintf(result, sizeof(result),
                "vus_gui_canvas(vus_string_cstr(%s), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err))",
                nm, x, y, wd, ht);
            free(nm); free(x); free(y); free(wd); free(ht);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_画布命中") == 0) {
        if (call->args && call->args->count >= 1) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_canvas_hit(vus_string_cstr(%s))", nm);
            free(nm);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_画布点") == 0) {
        if (call->args && call->args->count >= 1) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_canvas_pos(vus_string_cstr(%s))", nm);
            free(nm);
            g_uses_gui = 1;
            return strdup(result);
        }
    }

    /* ===== 图片 API + GIF 动画 ===== */
    if (strcmp(call->func_name, "图形_图片") == 0) {
        if (call->args && call->args->count >= 5) {
            char *x = gen_expr(buf, call->args->items[0]);
            char *y = gen_expr(buf, call->args->items[1]);
            char *wd = gen_expr(buf, call->args->items[2]);
            char *ht = gen_expr(buf, call->args->items[3]);
            char *path = gen_expr(buf, call->args->items[4]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_draw_image((int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), vus_string_cstr(%s))",
                x, y, wd, ht, path);
            free(x); free(y); free(wd); free(ht); free(path);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_动画_打开") == 0) {
        if (call->args && call->args->count >= 2) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char *path = gen_expr(buf, call->args->items[1]);
            char result[2048];
            snprintf(result, sizeof(result),
                "vus_gui_anim_open(vus_string_cstr(%s), vus_string_cstr(%s))", nm, path);
            free(nm); free(path);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_动画_下一步") == 0) {
        if (call->args && call->args->count >= 3) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char *x = gen_expr(buf, call->args->items[1]);
            char *y = gen_expr(buf, call->args->items[2]);
            char result[2048];
            snprintf(result, sizeof(result),
                "vus_gui_anim_next(vus_string_cstr(%s), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err))",
                nm, x, y);
            free(nm); free(x); free(y);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_动画_帧数") == 0) {
        if (call->args && call->args->count >= 1) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_anim_frames(vus_string_cstr(%s))", nm);
            free(nm);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_动画_关闭") == 0) {
        if (call->args && call->args->count >= 1) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_anim_close(vus_string_cstr(%s))", nm);
            free(nm);
            g_uses_gui = 1;
            return strdup(result);
        }
    }

    /* ===== 高级交互控件 ===== */
    if (strcmp(call->func_name, "图形_滑块") == 0) {
        if (call->args && call->args->count >= 5) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char *x = gen_expr(buf, call->args->items[1]);
            char *y = gen_expr(buf, call->args->items[2]);
            char *wd = gen_expr(buf, call->args->items[3]);
            char *vl = gen_expr(buf, call->args->items[4]);
            char *mn = (call->args->count > 5) ? gen_expr(buf, call->args->items[5]) : strdup("vus_to_string(0)");
            char *mx = (call->args->count > 6) ? gen_expr(buf, call->args->items[6]) : strdup("vus_to_string(100)");
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_slider(vus_string_cstr(%s), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err))",
                nm, x, y, wd, vl, mn, mx);
            free(nm); free(x); free(y); free(wd); free(vl); free(mn); free(mx);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_滑块值") == 0) {
        if (call->args && call->args->count >= 1) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_slider_value(vus_string_cstr(%s))", nm);
            free(nm);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_开关") == 0) {
        if (call->args && call->args->count >= 3) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char *x = gen_expr(buf, call->args->items[1]);
            char *y = gen_expr(buf, call->args->items[2]);
            char *st = (call->args->count > 3) ? gen_expr(buf, call->args->items[3]) : strdup("vus_to_string(0)");
            char result[2048];
            snprintf(result, sizeof(result),
                "vus_gui_switch(vus_string_cstr(%s), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err))",
                nm, x, y, st);
            free(nm); free(x); free(y); free(st);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_开关值") == 0) {
        if (call->args && call->args->count >= 1) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_switch_value(vus_string_cstr(%s))", nm);
            free(nm);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_微调") == 0) {
        if (call->args && call->args->count >= 4) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char *x = gen_expr(buf, call->args->items[1]);
            char *y = gen_expr(buf, call->args->items[2]);
            char *vl = gen_expr(buf, call->args->items[3]);
            char *st = (call->args->count > 4) ? gen_expr(buf, call->args->items[4]) : strdup("vus_to_string(1)");
            char result[2048];
            snprintf(result, sizeof(result),
                "vus_gui_spin(vus_string_cstr(%s), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err))",
                nm, x, y, vl, st);
            free(nm); free(x); free(y); free(vl); free(st);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_微调值") == 0) {
        if (call->args && call->args->count >= 1) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_spin_value(vus_string_cstr(%s))", nm);
            free(nm);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_单选") == 0) {
        if (call->args && call->args->count >= 6) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char *x = gen_expr(buf, call->args->items[1]);
            char *y = gen_expr(buf, call->args->items[2]);
            char *ih = gen_expr(buf, call->args->items[3]);
            char *opts = gen_expr(buf, call->args->items[4]);
            char *sl = gen_expr(buf, call->args->items[5]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_radio(vus_string_cstr(%s), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), vus_string_cstr(%s), (int)vus_to_int(%s, &_err))",
                nm, x, y, ih, opts, sl);
            free(nm); free(x); free(y); free(ih); free(opts); free(sl);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_单选值") == 0) {
        if (call->args && call->args->count >= 1) {
            char *nm = gen_expr(buf, call->args->items[0]);
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_radio_value(vus_string_cstr(%s))", nm);
            free(nm);
            g_uses_gui = 1;
            return strdup(result);
        }
    }

    /* ===== 高级外观 ===== */
    if (strcmp(call->func_name, "图形_圆角矩形") == 0) {
        if (call->args && call->args->count >= 6) {
            char *x = gen_expr(buf, call->args->items[0]);
            char *y = gen_expr(buf, call->args->items[1]);
            char *wd = gen_expr(buf, call->args->items[2]);
            char *ht = gen_expr(buf, call->args->items[3]);
            char *rd = gen_expr(buf, call->args->items[4]);
            char *c = gen_expr(buf, call->args->items[5]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_round_rect((int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (unsigned int)vus_to_int(%s, &_err))",
                x, y, wd, ht, rd, c);
            free(x); free(y); free(wd); free(ht); free(rd); free(c);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_圆角填充") == 0) {
        if (call->args && call->args->count >= 6) {
            char *x = gen_expr(buf, call->args->items[0]);
            char *y = gen_expr(buf, call->args->items[1]);
            char *wd = gen_expr(buf, call->args->items[2]);
            char *ht = gen_expr(buf, call->args->items[3]);
            char *rd = gen_expr(buf, call->args->items[4]);
            char *c = gen_expr(buf, call->args->items[5]);
            char result[4096];
            snprintf(result, sizeof(result),
                "vus_gui_round_fill((int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (unsigned int)vus_to_int(%s, &_err))",
                x, y, wd, ht, rd, c);
            free(x); free(y); free(wd); free(ht); free(rd); free(c);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_画圆") == 0) {
        if (call->args && call->args->count >= 4) {
            char *cx = gen_expr(buf, call->args->items[0]);
            char *cy = gen_expr(buf, call->args->items[1]);
            char *r = gen_expr(buf, call->args->items[2]);
            char *c = gen_expr(buf, call->args->items[3]);
            char result[2048];
            snprintf(result, sizeof(result),
                "vus_gui_draw_circle((int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (unsigned int)vus_to_int(%s, &_err))",
                cx, cy, r, c);
            free(cx); free(cy); free(r); free(c);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_填充圆") == 0) {
        if (call->args && call->args->count >= 4) {
            char *cx = gen_expr(buf, call->args->items[0]);
            char *cy = gen_expr(buf, call->args->items[1]);
            char *r = gen_expr(buf, call->args->items[2]);
            char *c = gen_expr(buf, call->args->items[3]);
            char result[2048];
            snprintf(result, sizeof(result),
                "vus_gui_fill_circle((int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (unsigned int)vus_to_int(%s, &_err))",
                cx, cy, r, c);
            free(cx); free(cy); free(r); free(c);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_圆弧") == 0) {
        if (call->args && call->args->count >= 6) {
            char *cx = gen_expr(buf, call->args->items[0]);
            char *cy = gen_expr(buf, call->args->items[1]);
            char *r = gen_expr(buf, call->args->items[2]);
            char *sd = gen_expr(buf, call->args->items[3]);
            char *sw = gen_expr(buf, call->args->items[4]);
            char *c = gen_expr(buf, call->args->items[5]);
            char result[2048];
            snprintf(result, sizeof(result),
                "vus_gui_draw_arc((int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err), (unsigned int)vus_to_int(%s, &_err))",
                cx, cy, r, sd, sw, c);
            free(cx); free(cy); free(r); free(sd); free(sw); free(c);
            g_uses_gui = 1;
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "图形_外观") == 0) {
        if (call->args && call->args->count >= 1) {
            char *rd = gen_expr(buf, call->args->items[0]);
            char *aa = (call->args->count > 1) ? gen_expr(buf, call->args->items[1]) : strdup("vus_to_string(0)");
            char result[1024];
            snprintf(result, sizeof(result),
                "vus_gui_appearance((int)vus_to_int(%s, &_err), (int)vus_to_int(%s, &_err))",
                rd, aa);
            free(rd); free(aa);
            g_uses_gui = 1;
            return strdup(result);
        }
    }

    /* ===== TUI ===== */
    if (strcmp(call->func_name, "tui_清屏") == 0) {
        return strdup("vus_plugin_tui_clear(NULL)");
    }
    if (strcmp(call->func_name, "tui_重置") == 0) {
        return strdup("vus_plugin_tui_reset(NULL)");
    }
    if (strcmp(call->func_name, "tui_设置颜色") == 0) {
        if (call->args && call->args->count >= 2) {
            char *fg = gen_expr(buf, call->args->items[0]);
            char *bg = gen_expr(buf, call->args->items[1]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_plugin_tui_set_color(%s, %s)", fg, bg);
            free(fg); free(bg);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "tui_定位") == 0) {
        if (call->args && call->args->count >= 2) {
            char *row = gen_expr(buf, call->args->items[0]);
            char *col = gen_expr(buf, call->args->items[1]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_plugin_tui_locate(%s, %s)", row, col);
            free(row); free(col);
            return strdup(result);
        }
    }
    if (strcmp(call->func_name, "tui_进度条") == 0) {
        if (call->args && call->args->count >= 3) {
            char *cur = gen_expr(buf, call->args->items[0]);
            char *tot = gen_expr(buf, call->args->items[1]);
            char *wid = gen_expr(buf, call->args->items[2]);
            char result[4096];
            snprintf(result, sizeof(result), "vus_plugin_tui_progress(%s, %s, %s)", cur, tot, wid);
            free(cur); free(tot); free(wid);
            return strdup(result);
        }
    }

    return NULL;
}