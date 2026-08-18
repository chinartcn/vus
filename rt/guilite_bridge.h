#ifndef VUS_GUILITE_BRIDGE_H
#define VUS_GUILITE_BRIDGE_H

/*
 * VUS 集成 GuiLite 绘图基础层 —— C 桥接层头文件
 *
 * 本文件对 VUS 语言编译器生成的 C 代码暴露 图形_* 内建函数的 C 实现。
 * 桥接层采用纯 C API，返回 VusString*（"1" 成功 / "0" 失败），
 * 与 VUS 运行时库其它内建函数（如 日志_*）的约定保持一致。
 *
 * 颜色统一使用 0xRRGGBB 整数（红/绿/蓝各 8 bit）。
 */

#include "libvus_rt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化图形环境：创建 GuiLite surface 与显示后端（X11 或 headless）。
 * 返回 VusString*："1" 成功 / "0" 失败。 */
VusString* vus_gui_init(int width, int height, const char* title);

/* 基础绘图内建函数（颜色 color 为 0xRRGGBB） */
VusString* vus_gui_draw_pixel(int x, int y, unsigned int color);
VusString* vus_gui_draw_line(int x1, int y1, int x2, int y2, unsigned int color);
VusString* vus_gui_draw_rect(int x, int y, int width, int height, unsigned int color);
VusString* vus_gui_fill_rect(int x, int y, int width, int height, unsigned int color);
VusString* vus_gui_draw_text(int x, int y, const char* text, unsigned int color);

/* 外部字体：加载 TTF/OTF 到全局活动字体并设置字号（像素）。返回 "1" / "0"。 */
VusString* vus_gui_font(const char* path, int size_px);
/* 外部字体是否已加载：返回 "1" / "0"。 */
VusString* vus_gui_font_loaded(void);

/* ============ 阶段E：多页导航（页面栈） ============
 * 页面概念：脚本对每页定义约定式函数 `页_<名>()`，编译为 `vus_页_<sanitized名>`
 * 全局符号（与语言 导入 结合即可接入外部 .vus 页面）。桥接层维护一个页面栈，
 * 切换/绘制通过 dlsym 反查当前页函数并调用。 */

/* 图形_页面_打开(名)：把名为"名"的页置为当前页。若该页已在栈中，则弹出其上方
 * 所有层（回到该页，避免 tab 往返无限增栈）；否则压栈。返回 "1" / "0"。 */
VusString* vus_gui_page_open(const char* name);
/* 图形_页面_返回()：弹栈回到上一页。有上一页返回 "1"；已在首页返回 "0"。 */
VusString* vus_gui_page_back(void);
/* 图形_页面_当前()：返回当前页名字符串；无页返回空串。 */
VusString* vus_gui_page_current(void);
/* 图形_页面_绘制()：dlsym 当前页对应 `页_<名>` 函数并调用（无参绘制）。找到并
 * 调用返回 "1"；无当前页或函数缺失返回 "0"（不崩溃）。 */
VusString* vus_gui_page_draw(void);

/* 刷新：把帧缓冲送到显示后端（X11 送窗 / headless 导出 PPM）。 */
VusString* vus_gui_redraw(void);

/* 保持：进入显示后端事件循环，保持窗口存活。 */
VusString* vus_gui_run(void);

/* 内部接口：供桥接实现与平台层使用的 C++ 包装（在 guilite_wrapper.cpp 中实现） */
int vus_gui_surface_init(int width, int height);          /* 0 成功 / -1 失败 */
void vus_gui_surface_free(void);
void vus_gui_surface_draw_pixel(int x, int y, unsigned int argb);
void vus_gui_surface_draw_line(int x1, int y1, int x2, int y2, unsigned int argb);
void vus_gui_surface_draw_rect(int x, int y, int width, int height, unsigned int argb);
void vus_gui_surface_fill_rect(int x, int y, int width, int height, unsigned int argb);
void vus_gui_surface_draw_text(int x, int y, const char* text, unsigned int argb);
unsigned int* vus_gui_surface_framebuffer(void);           /* 返回前台 ARGB8888 帧缓冲指针（显示目标） */
unsigned int* vus_gui_surface_backbuffer(void);            /* 返回后台 ARGB8888 帧缓冲指针（绘制目标） */
void vus_gui_surface_present(void);                        /* 双缓冲提交：后台整块拷贝到前台 */
int vus_gui_surface_width(void);
int vus_gui_surface_height(void);

/* 平台层接口（guilite_platform.c 实现） */
int  vus_gui_platform_init(int width, int height, const char* title);
void vus_gui_platform_redraw(int width, int height, const unsigned int* fb);
void vus_gui_platform_run(int width, int height, const unsigned int* fb);
/* 平台层文字绘制：X11 可用且加载了 X 字体时用 XDrawString 叠加入队，返回 1；
 * 否则返回 0，由桥接层回退到 GuiLite 帧缓冲绘制。 */
int  vus_gui_platform_draw_text(int x, int y, const char* text, unsigned int color);
/* 平台层非阻塞取事件：处理当前 X 事件队列（含点击/重绘/退出），供轮询式交互。 */
void vus_gui_platform_poll(int width, int height, const unsigned int* fb);

/* 点击事件派发：平台层（X11 事件循环）捕获鼠标点击后回调本接口，
 * 本接口负责按约定函数名反查 VUS 回调（事件_点击）并调用（见 guilite_bridge.c）。
 * x / y 为窗口内坐标（像素，左上为原点）。
 * button 为 X 按钮号：1=左键 2=中键 3=右键 4=滚轮上 5=滚轮下。滚轮事件的
 * x/y 也一并传入（用于区分是点击还是滚动）。 */
void vus_gui_platform_emit_click(int x, int y);
void vus_gui_platform_emit_button(int x, int y, int button);

/* ============ 阶段4：X11 多输入支持 ============ */

/* 按键事件派发：平台层收到 KeyPress 后回调本接口，记录最近一次按键。
 * symstr 为 KeySym 对应的可打印 UTF-8 字符串（无法打印时为 NULL）；
 * keycode 为 X 键码（物理键）；state 为修饰键掩码（Shift/Ctrl/Alt 位）。
 * 本接口存储键盘状态供 图形_按键/图形_按键码 轮询读取，并触发 事件_按键 回调。 */
void vus_gui_platform_emit_key(const char* symstr, unsigned int keycode, unsigned int state);

/* 指针移动派发：平台层收到 MotionNotify 后回调本接口，记录鼠标位置。
 * x / y 为窗口内坐标；state 为修饰键掩码。 */
void vus_gui_platform_emit_motion(int x, int y, unsigned int state);

/* 滚轮派发：滚轮作为 ButtonPress 的 button 4/5 到达，平台层分离后回调本接口。
 * dy 为滚动增量：+1 上滚、-1 下滚；x/y 为滚动时机标位置。 */
void vus_gui_platform_emit_wheel(int x, int y, int dy);

/* 轮询读取 API：供 图形_* 内建映射（阶段4，X11 多输入）。
 * 全部返回 VusString*，headless 下返回合理默认值。 */
VusString* vus_gui_last_key(void);             /* 图形_按键：最近按键字符 */
VusString* vus_gui_last_keycode(void);         /* 图形_按键码：最近按键 KeySym（无则 -1） */
VusString* vus_gui_mouse_pos(void);            /* 图形_鼠标位置："x,y" / "-1,-1" */
VusString* vus_gui_mouse_x(void);              /* 图形_鼠标x：鼠标 X / -1 */
VusString* vus_gui_mouse_y(void);              /* 图形_鼠标y：鼠标 Y / -1 */
VusString* vus_gui_wheel(void);                /* 图形_滚轮：滚轮增量（读取后清零） */
VusString* vus_gui_button_pressed(int button); /* 图形_键按下(btn)：某键是否按下并消费 */
VusString* vus_gui_hover(const char* name);    /* 图形_悬停(名)：鼠标是否悬停控件上 */

/* ============ 阶段B：样式/主题模板 ============ */
/* 图形_主题(背景, 边框, 高亮, 正文, 文字)：设置全局主题色（0xRRGGBB）。
 * 任一传 -1 表示保持该通道不变。返回 VusString*："1" 成功 / "0" 失败。 */
VusString* vus_gui_set_theme(int bg, int border, int highlight, int fg, int text);

/* ============ 阶段C：控件组合模板 ============ */
VusString* vus_gui_card(const char* name, int x, int y, int w, int h, const char* title); /* 图形_卡片 */
VusString* vus_gui_panel(const char* name, int x, int y, int w, int h, const char* title);/* 图形_面板 */
VusString* vus_gui_form_row(const char* name, const char* label, int x, int y, int w, const char* text); /* 图形_表单行 */
VusString* vus_gui_row_clicked(const char* name);  /* 图形_行点击 */
VusString* vus_gui_ring(const char* name, int x, int y, int radius, int pct, int color); /* 图形_圆环 */

/* ============ 阶段D：Markdown 最小集 / 画线增强 / 滚动容器 ============ */

/* 画线增强（图形_画线，后 3 参可省略，默认 线宽=1/虚线=0/箭头=0）。
 * 直接写 ARGB 帧缓冲，支持线宽、虚线、终点箭头。 */
VusString* vus_gui_draw_line_ex(int x1, int y1, int x2, int y2,
    unsigned int color, int width, int dashed, int arrow);

/* 图形_MD(x, y, 宽度, 文本)：Markdown 最小集渲染（标题/列表/引用/代码块/段落与分隔线，
 * 行内标记当普通文本），按宽度折行，复用 draw_text_xy 文字通道。
 * 返回占用行数（VusString* 为整数字符串），空文本返回 "0"。 */
VusString* vus_gui_md(int x, int y, int width, const char* text);

/* 滚动容器（图形_滚动容器）：声明可视区 + 滚动范围，开启内容平移 + 裁剪。
 * 返回 "1" 成功 / "0" 失败。 */
VusString* vus_gui_scroll_begin(const char* name, int x, int y, int w, int h, int content_h);
/* 图形_滚动容器滚(name, dy)：令容器偏移增减 dy（夹在合法范围），返回新偏移整数串。 */
VusString* vus_gui_scroll_delta(const char* name, int dy);
/* 图形_滚动容器偏移(name)：返回当前偏移整数串；未登记返回 "0"。 */
VusString* vus_gui_scroll_offset(const char* name);

/* 图形_背景图(x, y, 宽, 高, PNG路径)：用 libpng 解码 PNG（RGBA），按最近邻
 * 拉伸填充到目标矩形，作为控件/区域的背景底图。受滚动容器平移与裁剪约束，
 * 直接写 ARGB 帧缓冲并置脏。返回 "1" 成功 / "0" 失败（文件缺失/非 PNG/解码错）。
 * PNG 带 alpha 时与帧缓冲当前像素做 alpha 合成。 */
VusString* vus_gui_draw_png(int x, int y, int w, int h, const char* path);

/* ============ 阶段2：控件与轮询交互 API ============ */

/* 创建并绘制一个按钮，登记命中矩形（供 图形_按钮点击 命中检测）。
 * name：控件唯一名；x/y：左上角；w/h：宽高；text：按钮文本。
 * 返回 VusString*："1" 成功 / "0" 失败。 */
VusString* vus_gui_button(const char* name, int x, int y, int w, int h, const char* text);

/* 非阻塞处理 X 事件队列（轮询式交互模型的核心），更新最近点击坐标。
 * 返回 VusString*："1" 成功 / "0" 失败。 */
VusString* vus_gui_poll(void);

/* 命中检测：最近一次点击是否落在名为 name 的按钮矩形内。
 * 返回 VusString*："true" / "false"，可直接用作 如果 条件。 */
VusString* vus_gui_button_clicked(const char* name);

/* 模拟一次点击（x/y 为窗口内坐标），等价于平台层收到一次鼠标按下。
 * 用于 headless 自动化测试注入点击，验证命中检测与 如果 条件分支。
 * 返回 VusString*："1" 成功 / "0" 失败。 */
VusString* vus_gui_sim_click(int x, int y);

/* ============ 阶段3：控件库 ============
 * 统一控件表：按钮/标签/文本框/复选框/进度条/列表/画布 共享命中检测。
 * 文本绘制优先 X11（方向正常），失败回退 GuiLite 帧缓冲。 */

/* 标签：绘制一行文本，登记为可命中控件（估宽矩形）。
 * 返回 VusString*："1" 成功 / "0" 失败。 */
VusString* vus_gui_label(const char* name, int x, int y, const char* text, unsigned int color);

/* 文本框：白底 + 边框 + 文本，登记矩形。
 * 返回 VusString*："1" 成功 / "0" 失败。 */
VusString* vus_gui_textbox(const char* name, int x, int y, int w, int h, const char* text);

/* 复选框：方格 + 勾选标记 + 文本。点击（未消费）切换勾选状态。
 * 返回 VusString*：切换后状态 "true"/"false"，可作 如果 条件。 */
VusString* vus_gui_checkbox(const char* name, int x, int y, const char* text, int checked);

/* 进度条：填充底色 + 按 value(0-100) 画比例的进度 + 边框。
 * 返回 VusString*："1" 成功 / "0" 失败。 */
VusString* vus_gui_progress(const char* name, int x, int y, int w, int h, int value);

/* 列表：声明列表区域（void），返回 "1" 成功。rows_h 为每行像素高。 */
VusString* vus_gui_list(const char* name, int x, int y, int w, int h, int row_h);

/* 列表行：在第 line 行（0 起）写入并绘制文本（选中行高亮）。
 * 返回 VusString*："1" 成功 / "0" 失败（越界/未创建列表）。 */
VusString* vus_gui_list_row(const char* name, int line, const char* text);

/* 列表选中行：最近一次点击命中的行索引，未命中/无列表返回 "-1"。
 * 返回 VusString*：整数字符串，脚本用 vus_to_int 或与数字比较。 */
VusString* vus_gui_list_selected(const char* name);

/* 列表行命中：最近一次点击是否落在 name 列表的第 line 行内。
 * 返回 VusString*："true"/"false"。 */
VusString* vus_gui_list_row_clicked(const char* name, int line);

/* 画布：声明一个可命中区域，脚本自行在其内绘制。可选描边。
 * 返回 VusString*："1" 成功 / "0" 失败。 */
VusString* vus_gui_canvas(const char* name, int x, int y, int w, int h);

/* 画布命中：最近一次点击是否落在 name 画布内（仅当相对坐标在范围内时）。
 * 返回 VusString*："true" 命中 / "false" 未命中，可作 如果 条件。 */
VusString* vus_gui_canvas_hit(const char* name);

/* 画布相对坐标：最近一次画布命中的相对位置，返回 "x,y"；未命中返回 "-1,-1"。 */
VusString* vus_gui_canvas_pos(const char* name);

/* ============ 阶段G：图片 / GIF 动画 ============
 * 图形_图片(x, y, 宽, 高, 路径)：按扩展名（.png/.svg/.gif，其它按 PNG）解码
 * 图像并最近邻拉伸绘制到矩形 (x,y,w,h)。返回 "1" 成功 / "0" 失败
 * （文件缺失/未知格式/解码失败）。 */
VusString* vus_gui_draw_image(int x, int y, int w, int h, const char* path);

/* GIF 播放器：静态槽表（VUS_GIF_MAX 个），每条用一个 gd_GIF 句柄。 */
VusString* vus_gui_anim_open(const char* name, const char* path);  /* 图形_动画_打开 */
VusString* vus_gui_anim_next(const char* name, int x, int y);      /* 图形_动画_下一步 */
VusString* vus_gui_anim_frames(const char* name);                  /* 图形_动画_帧数 */
VusString* vus_gui_anim_close(const char* name);                   /* 图形_动画_关闭 */

/* ============ 阶段H：高级交互控件 ============ */
/* 滑块（图形_滑块）：水平轨道 + 滑块块，min/max 缺省 0/100。
 * 点击轨道按 x 换算新值写入控件并返回当前值整数字符串。 */
VusString* vus_gui_slider(const char* name, int x, int y, int w, int value, int minv, int maxv);
VusString* vus_gui_slider_value(const char* name);   /* 图形_滑块值 */

/* 开关（图形_开关）：圆角底 + 圆形旋钮，点击切换。返回 "true"/"false"。 */
VusString* vus_gui_switch(const char* name, int x, int y, int state);
VusString* vus_gui_switch_value(const char* name);   /* 图形_开关值 */

/* 微调（图形_微调）：左减右加两个按钮 + 中间数值，步长缺省 1。 */
VusString* vus_gui_spin(const char* name, int x, int y, int value, int step);
VusString* vus_gui_spin_value(const char* name);     /* 图形_微调值 */

/* 单选（图形_单选）：options 用分号 ";" 分隔的多选项，绘制一列单选钮。 */
VusString* vus_gui_radio(const char* name, int x, int y, int item_h, const char* options, int sel);
VusString* vus_gui_radio_value(const char* name);    /* 图形_单选值 */

/* ============ 阶段I：高级外观 ============ */
VusString* vus_gui_round_rect(int x, int y, int w, int h, int radius, unsigned int color); /* 图形_圆角矩形 */
VusString* vus_gui_round_fill(int x, int y, int w, int h, int radius, unsigned int color); /* 图形_圆角填充 */
VusString* vus_gui_draw_circle(int cx, int cy, int r, unsigned int color);  /* 图形_画圆 */
VusString* vus_gui_fill_circle(int cx, int cy, int r, unsigned int color);  /* 图形_填充圆 */
VusString* vus_gui_draw_arc(int cx, int cy, int r, int start_deg, int sweep_deg, unsigned int color); /* 图形_圆弧 */
/* 图形_外观(圆角半径[, 抗锯齿])：设置全局控件圆角半径 s_global_radius（默认 0=直角）。
 * 半径>0 时 图形_按钮 改为圆角外观。返回 "1"。抗锯齿参数可忽略。 */
VusString* vus_gui_appearance(int radius, int aa);

#ifdef __cplusplus
}
#endif

#endif /* VUS_GUILITE_BRIDGE_H */