/*
 * VUS GUI —— EGL + OpenGL ES 渲染后端
 *
 * 把 GuiLite 的内存帧缓冲(ARGB8888)作为 BGRA 纹理整帧上传到 GPU，
 * 用一个全屏三角形平铺绘制，颜色序(NV21 无关)与翻转(VUS_X11_FLIP)
 * 在着色器与纹理坐标中完成，替代 XPutImage 逐像素 CPU 拷贝。
 *
 * 目标平台：Termux + termux-x11（EGL 走 mesa/virgl/llvmpipe，LLVM
 * SIMD 加速远快于逐像素 XPutImage）。沙箱无渲染环境时 init 失败回退软路径。
 *
 * 编译：仅 `VUS_GUI_GLES` 定义时编译；链接 -lEGL -lGLESv2。
 * 许可：MIT。
 */

#ifdef VUS_GUI_GLES

#include "guilite_gles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

/* ---- 全局资源 ---- */
static EGLDisplay  g_egl_dpy = EGL_NO_DISPLAY;
static EGLConfig   g_egl_cfg = 0;
static EGLContext  g_egl_ctx = EGL_NO_CONTEXT;
static EGLSurface  g_egl_surf = EGL_NO_SURFACE;

static GLuint   g_prog  = 0;
static GLuint   g_vao   = 0;
static GLuint   g_vbo   = 0;
static GLuint   g_tex   = 0;
static int      g_w = 0;
static int      g_h = 0;
static int      g_flip_v = 0;
static int      g_flip_h = 0;
static int      g_active = 0;

/* ---- 着色器（GLSL ES 3.00）
 * 帧缓冲内存序为 ARGB8888 小端 -> 字节序 [B,G,R,A]，经 GL_RGBA 上传后
 * 通道到 R=byte0(B)、G=byte1(G)、B=byte2(R)。故在片元里把 R/B 交换，
 * 并忽略 alpha（保持原 XPutImage 只取 RGB 的行为）。 */
static const char* g_vert_src =
    "#version 300 es\n"
    "layout(location=0) in vec2 aPos;\n"
    "layout(location=1) in vec2 aTex;\n"
    "out vec2 vTex;\n"
    "void main(){ vTex=aTex; gl_Position=vec4(aPos,0.0,1.0); }\n";

static const char* g_frag_src =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 vTex;\n"
    "uniform sampler2D uTex;\n"
    "out vec4 frag;\n"
    "void main(){ vec4 t=texture(uTex,vTex); frag=vec4(t.b,t.g,t.r,1.0); }\n";

static GLuint compile_shader(GLenum type, const char* src)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[1024];
        GLsizei len = 0;
        glGetShaderInfoLog(sh, (GLsizei)sizeof(log), &len, log);
        fprintf(stderr, "[gles] 着色器编译失败(%d): %.*s\n", (int)type, (int)len, log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static int build_program(void)
{
    GLuint vs = compile_shader(GL_VERTEX_SHADER, g_vert_src);
    if (!vs) return 0;
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, g_frag_src);
    if (!fs) { glDeleteShader(vs); return 0; }
    g_prog = glCreateProgram();
    glAttachShader(g_prog, vs);
    glAttachShader(g_prog, fs);
    glLinkProgram(g_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[1024];
        GLsizei len = 0;
        glGetProgramInfoLog(g_prog, (GLsizei)sizeof(log), &len, log);
        fprintf(stderr, "[gles] 着色器链接失败: %.*s\n", (int)len, log);
        glDeleteProgram(g_prog);
        g_prog = 0;
        return 0;
    }
    return 1;
}

/* 构建全屏四边形（两个三角形），纹理坐标按翻转开关取值：
 *   默认：左上 对应 fb 首行/首列(纹理 v=0,s=0)
 *   flip_v: 上下颠倒(首行画到屏幕底部)
 *   flip_h: 左右镜像 */
static void build_quad(void)
{
    /* 纹理坐标默认: 左上=(0,0) 右下=(1,1)。翻转即交换端点。 */
    float s_tl = g_flip_h ? 1.0f : 0.0f;
    float s_tr = g_flip_h ? 0.0f : 1.0f;
    float t_tl = g_flip_v ? 1.0f : 0.0f;
    float t_br = g_flip_v ? 0.0f : 1.0f;
    /* 顶点: 屏幕左上(-1,1) 应与 fb 首行(纹理 t=t_tl)对应；
     *      屏幕左下(-1,-1) 应与 fb 末行(纹理 t=t_br)对应。 */
    /* 6 个顶点(两个三角形)，每顶点 [x,y,s,t] 共 24 个 float；
     * 默认: 屏幕左上(-1,1) 对应 fb 首行(纹理 v=0=s_tl)、首列(s=0)。 */
    float v[24] = {
        /* 左下 */ -1, -1, s_tl, t_br,
        /* 右下 */  1, -1, s_tr, t_br,
        /* 左上 */ -1,  1, s_tl, t_tl,
        /* 左上副本 */ -1,  1, s_tl, t_tl,
        /* 右下副本 */  1, -1, s_tr, t_br,
        /* 右上 */  1,  1, s_tr, t_tl,
    };
    glGenVertexArrays(1, &g_vao);
    glBindVertexArray(g_vao);
    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); /* aPos */
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1); /* aTex */
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glBindVertexArray(0);
}

int vus_gles_init(Display* dpy, Window win, int width, int height)
{
    if (!dpy) return 0;
    g_flip_v = 0;
    g_flip_h = 0;
    const char* fl = getenv("VUS_X11_FLIP");
    if (fl && *fl && !(strcmp(fl, "none") == 0 || strcmp(fl, "0") == 0 || strcmp(fl, "off") == 0))
    {
        if (strchr(fl, 'v') || strchr(fl, 'V')) g_flip_v = 1;
        if (strchr(fl, 'h') || strchr(fl, 'H')) g_flip_h = 1;
    }
    fprintf(stderr, "[gles] 初始化 EGL...\n");

    g_egl_dpy = eglGetDisplay((EGLNativeDisplayType)dpy);
    if (g_egl_dpy == EGL_NO_DISPLAY)
    {
        fprintf(stderr, "[gles] eglGetDisplay 失败\n"); return 0;
    }
    EGLint maj = 0, min = 0;
    if (!eglInitialize(g_egl_dpy, &maj, &min))
    {
        fprintf(stderr, "[gles] eglInitialize 失败\n"); return 0;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API))
    {
        fprintf(stderr, "[gles] eglBindAPI 失败\n"); return 0;
    }
    fprintf(stderr, "[gles] EGL v%d.%d OK\n", (int)maj, (int)min);

    /* 首选 ES3，退而求其次 ES2（保证 virgl/llvmpipe 均可用） */
    EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_COLOR_BUFFER_TYPE, EGL_RGB_BUFFER,
        EGL_NONE
    };
    EGLConfig cfg3 = 0, cfg2 = 0;
    EGLint n = 0;
    if (eglChooseConfig(g_egl_dpy, cfg_attr, &cfg3, 1, &n) && n > 0)
    {
        g_egl_cfg = cfg3;
    }
    else
    {
        EGLint cfg2_attr[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
            EGL_COLOR_BUFFER_TYPE, EGL_RGB_BUFFER,
            EGL_NONE
        };
        if (eglChooseConfig(g_egl_dpy, cfg2_attr, &cfg2, 1, &n) && n > 0)
        {
            g_egl_cfg = cfg2;
        }
    }
    if (!g_egl_cfg)
    {
        fprintf(stderr, "[gles] eglChooseConfig 失败\n"); return 0;
    }

    /* 上下文：仅 ES3（先 3.2 再 3.0）。GLES2 不支持 #version 300 es，与其
     * 着色器/属性 API 错配，不如直接软回退 XImage 路径（功能不损失）。 */
    EGLint ctx_attr32[] = { EGL_CONTEXT_MAJOR_VERSION, 3,
                            EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE };
    EGLint ctx_attr30[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
    g_egl_ctx = eglCreateContext(g_egl_dpy, g_egl_cfg, EGL_NO_CONTEXT, ctx_attr32);
    if (g_egl_ctx == EGL_NO_CONTEXT)
    {
        g_egl_ctx = eglCreateContext(g_egl_dpy, g_egl_cfg, EGL_NO_CONTEXT, ctx_attr30);
    }
    if (g_egl_ctx == EGL_NO_CONTEXT)
    {
        fprintf(stderr, "[gles] eglCreateContext 失败\n"); return 0;
    }

    g_egl_surf = eglCreateWindowSurface(g_egl_dpy, g_egl_cfg,
                                        (EGLNativeWindowType)win, NULL);
    if (g_egl_surf == EGL_NO_SURFACE)
    {
        EGLint e = eglGetError();
        fprintf(stderr, "[gles] eglCreateWindowSurface 失败 (err=%d)\n", (int)e);
        return 0;
    }
    if (!eglMakeCurrent(g_egl_dpy, g_egl_surf, g_egl_surf, g_egl_ctx))
    {
        fprintf(stderr, "[gles] eglMakeCurrent 失败\n"); return 0;
    }

    if (!build_program())
    {
        return 0;
    }
    fprintf(stderr, "[gles] GL 渲染器: %s / %s\n",
            (const char*)glGetString(GL_VENDOR),
            (const char*)glGetString(GL_RENDERER));

    build_quad();

    glGenTextures(1, &g_tex);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)width, (GLsizei)height,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);

    g_w = width;
    g_h = height;
    g_active = 1;
    glViewport(0, 0, (GLsizei)width, (GLsizei)height);
    fprintf(stderr, "[gles] 已启用 GL 上屏 (%dx%d, flip_v=%d flip_h=%d)\n",
            (int)width, (int)height, g_flip_v, g_flip_h);
    return 1;
}

int vus_gles_active(void)
{
    return g_active;
}

void vus_gles_redraw(int width, int height, const unsigned int* fb)
{
    if (!g_active || !fb) return;
    if (width != g_w || height != g_h)
    {
        /* 尺寸变化：重建纹理 */
        g_w = width;
        g_h = height;
        glBindTexture(GL_TEXTURE_2D, g_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)width, (GLsizei)height,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindTexture(GL_TEXTURE_2D, 0);
        glViewport(0, 0, (GLsizei)width, (GLsizei)height);
    }

    /* 帧缓冲即 ARGB8888 小端，内存字节 = [B,G,R,A] —— 直接按 GL_RGBA 上传，
     * 片元着色器会把 R/B 交换还原成正确颜色。 */
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)width, (GLsizei)height,
                    GL_RGBA, GL_UNSIGNED_BYTE, fb);

    glUseProgram(g_prog);
    glUniform1i(glGetUniformLocation(g_prog, "uTex"), 0);
    glBindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    eglSwapBuffers(g_egl_dpy, g_egl_surf);
}

#endif /* VUS_GUI_GLES */