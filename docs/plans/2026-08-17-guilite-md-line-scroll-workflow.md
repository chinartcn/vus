---
intent: 为 VUS GUI 增加三块单页绘制能力：图形_MD 最小集渲染、图形_画线增强（线宽/虚线/箭头）、滚动容器（内容平移+裁剪）
success_criteria: 三个新内建函数实现并在 headless 用例通过；旧 GUI 测试全部通过（tests/run_tests.sh 全绿）
risk_level: low
auto_approve: true
branch: master
worktree: false
dirty_worktree: allow
---

## Steps

- [ ] **Step 1: 头文件声明本批 API**
action: 编辑 rt/guilite_bridge.h，在既有声明块末尾新增三组声明：
 1) `VusString* vus_gui_draw_line_ex(int x1,int y1,int x2,int y2,unsigned int color,int width,int dashed,int arrow);`
 2) `VusString* vus_gui_md(int x,int y,int width,const char* text);`
 3) `VusString* vus_gui_scroll_begin(const char* name,int x,int y,int w,int h,int content_h);`
    `VusString* vus_gui_scroll_delta(const char* name,int dy);`
    `VusString* vus_gui_scroll_offset(const char* name);`
 用中文注释标注「阶段D：Markdown 最小集 / 画线增强 / 滚动容器」。
verify: grep -q 'vus_gui_draw_line_ex' rt/guilite_bridge.h && grep -q 'vus_gui_md' rt/guilite_bridge.h && grep -q 'vus_gui_scroll_begin' rt/guilite_bridge.h
loop: false

- [ ] **Step 2: 实现画线增强**
action: 在 rt/guilite_bridge.c 中新增 `VusString* vus_gui_draw_line_ex(...)`：
 - 未初始化返回 "0"。
 - 用 Bresenham 生成 (x1,y1)→(x2,y2) 像素点序；每点以 width×width 方形笔调用 `write_fb_pixel`（新增内部辅助：直接写 `vus_gui_surface_framebuffer()` 的 ARGB 像素，写前做滚动裁剪/平移，见 Step4；本步先用临时直写）。
 - dashed 非 0 时按固定占空比（如 6 实 4 空）跳过部分像素。
 - arrow 非 0 时在终点端沿线段方向补两条短线成三角头。
 - 完成后调用 `vus_gui_mark_dirty()`，返回 "1"。
 需注意「直接写帧缓冲」要转为 ARGB（复用已有 argb_from_rgb）。
verify: grep -q 'vus_gui_draw_line_ex' rt/guilite_bridge.c && make -C /workspace/vus
loop: until [编译通过且 grep 命中]
max_iterations: 3

- [ ] **Step 3: 实现图形_MD 最小集**
action: 在 rt/guilite_bridge.c 新增 `VusString* vus_gui_md(int x,int y,int width,const char* text)`：
 - 未初始化返回 "0"。
 - 从 text 逐行解析，行类型：标题(#…)、无序列表("- "/"* ")、有序列表(数字. )、引用("> ")、代码块("```"，含可能的语言行忽略)、空行(段落)、分隔线("---")、普通行。
 - 每行按顺序绘制到 (cx,cy)，cy 从 y 累加固定行距（取现有 draw_text_xy 的行高约 16px）。标题去 # 前缀用 `s_theme.highlight` 色，引用用 `s_theme.border` 色，代码块整行用固定强调色（如 0x875F00）并加缩进，列表加缩进与符号。
 - 按 width 折行（借助现有文字估宽；折行即换行并 cy+=行距）。
 - 行内标记 * ** ` /* ] 一律保留为普通文本。
 - 复用现有内部 helper draw_text_xy(x,y,text,color) 逐行输出（平台文字会置脏），最后返回占用行数（VusString* 为整数字符串），空文本返回 "0"。
verify: grep -q 'vus_gui_md' rt/guilite_bridge.c && make -C /workspace/vus
loop: until [编译通过且 grep 命中]
max_iterations: 3

- [ ] **Step 4: 实现滚动容器（状态 + 平移裁剪 + 滚轮命中）**
action: 在 rt/guilite_bridge.c 新增滚动容器支持：
 - 新增容器状态结构并登记表（最多 16 个），每项含 name、可视矩形(x,y,w,h)、content_h、offset，及一个内部辅助 `static int scroll_translate(int* y_inout)`：若当前存在"活动容器"，则在绘制坐标上 y -= offset 并裁剪到可视矩形，返回 1=落在可见区可画 / 0=裁剪掉。
 - 新增 `vus_gui_scroll_begin(name,x,y,w,h,content_h)`：登记容器并将其置为"活动容器"（记全局 s_act_scroll 指针），返回 "1"；若已登记则刷新参数。
 - 新增 `vus_gui_scroll_delta(name,dy)`：令同名容器 offset += dy（夹在 [0, content_h-h]），标记脏，返回新偏移整数字符串；未登记返回 "0"。
 - 新增 `vus_gui_scroll_offset(name)`：返回同名容器当前 offset 整数字符串；未登记返回 "0"。
 - 在 vus_gui_platform_emit_wheel 中：滚轮(x,y)若落在某容器可视矩形内，则对该容器滚动（dy 符号经换算：上滚 dy<0 视作向上一个方向），并 mark_dirty → 实现「滚轮自动滚动」。
 - 让像素级写入（供 draw_line_ex 及后续裁剪用）通过一个 `static void write_scrolled_pixel(int x,int y,unsigned int argb)`：应用滚动平移+裁剪后写帧缓冲。本批先让画线用此辅助；其余既有绘制函数不做强制裁剪（保持现状），滚动容器本批以脚本显式调用为主。
verify: grep -q 'vus_gui_scroll_offset' rt/guilite_bridge.c && grep -q 'scroll_translate' rt/guilite_bridge.c && make -C /workspace/vus
loop: until [编译通过且 grep 命中]
max_iterations: 4

- [ ] **Step 5: generator 映射**
action: 编辑 src/generator.c：
 - 「图形_画线」分支：先判断总参个数 items_count；>=8 时生成 vus_gui_draw_line_ex(x1,y1,x2,y2,color,width,dashed,arrow)；5..7 个时用默认字面量（width=1/dashed=0/arrow=0）补齐再生成；仅当 >=5 才合法，否则保持原行为。逐参判断避免越界。
 - 新增「图形_MD」映射 → `vus_gui_md(x, y, width, vus_string_cstr(text))`。
 - 新增「图形_滚动容器」映射 → `vus_gui_scroll_begin(...)`；「图形_滚动容器滚(nm,dy)」→ `vus_gui_scroll_delta(...)`；「图形_滚动容器偏移(nm)」→ `vus_gui_scroll_offset(vus_string_cstr(nm))`。
verify: grep -q '图形_MD' src/generator.c && grep -q '图形_滚动容器' src/generator.c && make -C /workspace/vus
loop: until [编译通过且 grep 命中]
max_iterations: 3

- [ ] **Step 6: 新增验证脚本 + 全量回归**
action:
 1) 新建 examples/gui_md_line_scroll_verify.vus：调用 图形_初始化(320,240,"v")；再 图形_画线(0,0,100,50,0xFF0000,3,0,1) 打印返回值、图形_MD(5,5,300,"# 标题\n- 项1\n1. 项2\n> 引用\n```\ncode\n```\n正文") 打印行数、图形_滚动容器("s",0,20,300,100,300) 与 图形_滚动容器滚("s",10) 打印新偏移、图形_滚动容器偏移("s") 打印当前偏移；用 日志_打印 输出各返回值。
 2) 运行：`cd /workspace/vus && ./vus run examples/gui_md_line_scroll_verify.vus`，目视返回值（画线/容器为 1、MD 行数为非 0、滚后偏移为 10）。
 3) 全量回归：`cd /workspace/vus && bash tests/run_tests.sh`，须全部通过（旧 GUI 测试无回归）。
verify: cd /workspace/vus && ./vus run examples/gui_md_line_scroll_verify.vus 2>&1 | grep -Eq '1|行数' && bash tests/run_tests.sh
loop: until [全量测试通过]
max_iterations: 5