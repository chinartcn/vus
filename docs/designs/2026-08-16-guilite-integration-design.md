# VUS 集成 GuiLite 绘图基础层 —— 设计文档

日期：2026-08-16
状态：已批准

## 1. 目标

为 VUS 语言提供原生 GUI 绘图能力（阶段1：绘图基础层），让 VUS 用户能用中文内建函数直接绘制并显示图形界面，无需学习第二门语言。采用 GuiLite（不造轮子），显示后端为 X11（Termux X11 / PC X11 / Xvfb 无头测试通用）。

## 2. 背景与决策

| 决策点 | 结论 |
|--------|------|
| 阶段目标 | 绘图基础层：画点/线/矩形/文字/填充 + 显示 |
| 图形库 | GuiLite（官方 C++ 单头文件 GuiLite.h，C 桥接） |
| 显示后端 | X11（XPutImage 送帧缓冲），Termux 与 PC 通用 |
| 交互 | 仅显示 + 窗口保持（X11 事件循环），不映射 VUS 回调 |
| 测试 | Xvfb 无头自动化测试 + 像素断言 |

## 3. 总体架构

```
VUS 脚本 (图形_初始化/图形_画点/图形_刷新/...)
   ↓ generator.c 内建函数映射（复用日志_* 模式）
C 桥接层 rt/guilite_bridge.c  —— 纯 C API + 静态状态
   ↓
GuiLite (C++ 单头文件 GuiLite.h)  —— surface 绘制 + 字体渲染
   ↓
X11 显示后端 rt/guilite_platform_x11.c  —— XPutImage 送帧缓冲 + 事件循环
   ↓
Termux X11 (DISPLAY=:0) / PC X11 / Xvfb 无头测试
```

## 4. VUS 内建函数集（阶段1）

| 内建函数 | 桥接函数 | 作用 |
|----------|----------|------|
| `图形_初始化(宽, 高, 标题)` | `vus_gui_init` | 创建 X11 窗口 + GuiLite surface |
| `图形_画点(x, y, 颜色)` | `vus_gui_draw_pixel` | 画点 |
| `图形_画线(x1,y1, x2,y2, 颜色)` | `vus_gui_draw_line` | 画线 |
| `图形_矩形(x, y, 宽, 高, 颜色)` | `vus_gui_draw_rect` | 空心矩形 |
| `图形_填充(x, y, 宽, 高, 颜色)` | `vus_gui_fill_rect` | 实心矩形 |
| `图形_文字(x, y, 文本, 颜色)` | `vus_gui_draw_text` | 文字（GuiLite 字体） |
| `图形_刷新()` | `vus_gui_redraw` | XPutImage 送屏 |
| `图形_保持()` | `vus_gui_run` | 进入事件循环保持窗口 |

颜色用 `0xRRGGBB` 整数。

## 5. 显示后端（X11）

- `guilite_platform_x11.c`：XOpenDisplay / XCreateSimpleWindow / XPutImage 将 GuiLite 的 RGB 帧缓冲送到窗口。
- 像素格式：GuiLite 默认 ARGB8888；X11 用 ZPixmap 送 24/32 位。
- 事件循环：`XNextEvent` 处理 Expose/ClientMessage(WMshell) 等，保持窗口存活；`图形_保持()` 进入循环。
- 同一套代码：Termux 设 `DISPLAY=:0`，PC 默认 display，Xvfb 无头。

## 6. 构建集成

- Makefile 增 GuiLite 规则：`GuiLite.cpp`(C++) 编译为 `libGuiLite` 目标 + `guilite_bridge.c`(C) + `guilite_platform_x11.c`。
- `vus run` 生成代码的 gcc 链接命令追加 `-lX11` 与 GuiLite/bridge/platform 目标文件。
- 因 GuiLite 为 C++，编译 GuiLite 部分需 g++（Termux 有 g++/clang + x11-repo）。
- 桥接层用 `extern "C"` 保护，VUS 生成的 C 直接调用。

## 7. 测试策略

- `tests/test_gui.vus`：初始化 → 画各图元 → 刷新，断言桥接函数被调用、返回成功。
- Xvfb 下跑真实 X11 后端，验证无崩溃、像素正确（读回帧缓冲断言）。
- 回归：make test 全量通过。

## 8. 验收标准

- [ ] `make clean && make` 全量重编通过（含 GuiLite/bridge/X11 后端）
- [ ] `tests/test_gui.vus` 在 Xvfb 下运行无崩溃，像素断言通过
- [ ] 内建函数 `图形_*` 在 VUS 中可编译调用
- [ ] 提交推送 Gitee，走分支 + PR