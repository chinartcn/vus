/*
 * VUS GUI —— EGL + OpenGL ES 渲染后端（rt/guilite_gles.c 的实现声明）
 *
 * 目的：把 GuiLite 渲染到内存的 ARGB8888 帧缓冲，交给 GPU 直接上屏，
 * 替换纯 CPU 的 XPutImage 逐像素上传。virgl/llvmpipe 等加速的是 X 层
 * GL，只有把我们的上屏路径也走 EGL/GLES 才能真正吃到 GPU。
 *
 * 语义：
 *   - vus_gles_init 在 X11 窗口创建后调用；任何 EGL/GLES 步骤失败都返回 0，
 *     上层保持原有 XPutImage 软路径（功能不受损）。
 *   - vus_gles_redraw 每帧把帧缓冲作为纹理上传，用 GL 全屏三角形绘制，
 *     颜色序(内存 BGR→RGB)与翻转(VUS_X11_FLIP)在着色器/纹理坐标完成。
 *   - 文字叠加仍走 platform 层原有 Xft 路径（在 swap 之后 X11 上层绘制）。
 *
 * 编译条件：仅当 `VUS_GUI_GLES` 定义时编译本文件；链接需挂 -lEGL -lGLESv2。
 * 许可：MIT（与 VUS 保持一致）。
 */
#ifndef VUS_GUILITE_GLES_H
#define VUS_GUILITE_GLES_H

#ifdef VUS_GUI_GLES
#include <X11/Xlib.h>

/* 初始化 EGL + GLES 上下文与着色器。失败返回 0（上层回退软渲染）。 */
int vus_gles_init(Display* dpy, Window win, int width, int height);

/* 当前是否处于 GL 上屏模式（需 init 成功）。 */
int vus_gles_active(void);

/* 把 ARGB8888 帧缓冲以纹理方式经 GL 上屏；G8 增量：仅上传脏矩形
 * [rx1,rx2)×[ry1,ry2) 到对应纹理子区域（半开区间）。init 未成功时为 no-op。 */
void vus_gles_redraw(int width, int height, const unsigned int* fb,
                     int rx1, int ry1, int rx2, int ry2);

#endif /* VUS_GUI_GLES */
#endif /* VUS_GUILITE_GLES_H */