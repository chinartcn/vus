/*
 * vus_cordis.c — Cordis 式运行时上下文元框架（路线 A：C 元层）
 *
 * 实现 vus_cordis.h 声明的上下文/服务容器/依赖注入/effect 栈/事件四模式。
 * 并入 libvus_rt 编译（Makefile 增加 vus_cordis.o）。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L   /* strdup（C11 严格模式下不可见） */
#endif
#include "vus_cordis.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VUS_CTX_MAX_SVCS   64
#define VUS_CTX_MAX_EFFECTS 128
#define VUS_CTX_MAX_LISTENERS 256

/* 可撤销句柄（disposer） */
typedef struct VusCtxDisc {
    int kind; /* 1=服务 2=effect 3=监听器 */
    VusCtx *ctx;
    void *target;
} VusCtxDisc;

typedef struct CtxEffect {
    void (*undo)(void *data);
    void *data;
} CtxEffect;

typedef struct CtxListener {
    char *ev;
    int mode;
    VusClosure *cb;
    struct CtxListener *next;
} CtxListener;

struct VusCtx {
    /* 服务容器 */
    VusCtxService *svcs[VUS_CTX_MAX_SVCS];
    int active[VUS_CTX_MAX_SVCS];       /* 是否已激活（DI 拓扑） */
    int svc_order[VUS_CTX_MAX_SVCS];    /* 激活顺序（倒序即逆拓扑卸载序） */
    int n_svcs;
    int n_order;

    /* effect 栈 */
    CtxEffect effects[VUS_CTX_MAX_EFFECTS];
    int n_effects;

    /* 事件监听器 */
    CtxListener *listeners;
    int n_listeners;

    int stopped;                        /* WATERFALL/SERIAL 短路标志 */
    char err_buf[192];
};

/* ============ 上下文生命周期 ============ */

VusCtx *vus_ctx_create(void) {
    VusCtx *ctx = (VusCtx *)calloc(1, sizeof(VusCtx));
    return ctx;
}

void vus_ctx_dispose(VusCtx *ctx) {
    if (!ctx) return;
    /* 1) 服务逆拓扑序卸载 */
    for (int i = ctx->n_order - 1; i >= 0; i--) {
        VusCtxService *s = ctx->svcs[ctx->svc_order[i]];
        if (s && s->dispose) s->dispose(s->impl);
    }
    /* 2) effect 栈逆序回退 */
    for (int i = ctx->n_effects - 1; i >= 0; i--) {
        if (ctx->effects[i].undo) ctx->effects[i].undo(ctx->effects[i].data);
    }
    /* 3) 监听器卸载（引用归还） */
    CtxListener *l = ctx->listeners;
    while (l) {
        CtxListener *nx = l->next;
        if (l->cb) vus_unref(l->cb);
        free(l->ev);
        free(l);
        l = nx;
    }
    free(ctx);
}

const char *vus_ctx_last_error(VusCtx *ctx) {
    return ctx ? ctx->err_buf : "ctx is NULL";
}

/* ============ 服务容器 + DI ============ */

/* 服务键是否已激活 */
static int svc_active(VusCtx *ctx, const char *key) {
    for (int i = 0; i < ctx->n_svcs; i++) {
        if (ctx->active[i] && ctx->svcs[i] && ctx->svcs[i]->key &&
            strcmp(ctx->svcs[i]->key, key) == 0) return 1;
    }
    return 0;
}

/* DI 激活推进：遍历未激活服务，依赖就绪者激活。返回本轮激活数。 */
static int svc_progress(VusCtx *ctx) {
    int activated = 0;
    for (int i = 0; i < ctx->n_svcs; i++) {
        if (ctx->active[i]) continue;
        VusCtxService *s = ctx->svcs[i];
        int ready = 1;
        if (s->deps) {
            for (int d = 0; s->deps[d]; d++) {
                if (!svc_active(ctx, s->deps[d])) { ready = 0; break; }
            }
        }
        if (ready) {
            ctx->active[i] = 1;
            ctx->svc_order[ctx->n_order++] = i;
            if (s->init) s->init(s->impl);
            activated++;
        }
    }
    return activated;
}

void *vus_ctx_register(VusCtx *ctx, VusCtxService *svc) {
    if (!ctx || !svc || !svc->key) return NULL;
    if (svc_active(ctx, svc->key)) {
        snprintf(ctx->err_buf, sizeof(ctx->err_buf), "服务已存在: %s", svc->key);
        return NULL;
    }
    if (ctx->n_svcs >= VUS_CTX_MAX_SVCS) {
        snprintf(ctx->err_buf, sizeof(ctx->err_buf), "服务数量超过上限 %d", VUS_CTX_MAX_SVCS);
        return NULL;
    }
    int idx = ctx->n_svcs++;
    ctx->svcs[idx] = svc;
    ctx->active[idx] = 0;

    /* 拓扑推进（新服务可能让一批依赖链陆续就绪） */
    while (svc_progress(ctx) > 0) {
        /* 反复推进直至无进展 */
    }
    if (!ctx->active[idx]) {
        /* 环依赖或缺少依赖方：拒绝本次注册 */
        snprintf(ctx->err_buf, sizeof(ctx->err_buf),
                 "服务 %s 无法激活（存在环依赖或依赖未就绪）", svc->key);
        ctx->n_svcs--;
        return NULL;
    }
    VusCtxDisc *disc = (VusCtxDisc *)malloc(sizeof(VusCtxDisc));
    if (!disc) return NULL;
    disc->kind = 1;
    disc->ctx = ctx;
    disc->target = ctx->svcs[idx];
    return disc;
}

void *vus_ctx_get(VusCtx *ctx, const char *key) {
    if (!ctx || !key) return NULL;
    for (int i = 0; i < ctx->n_svcs; i++) {
        if (ctx->active[i] && ctx->svcs[i] && ctx->svcs[i]->key &&
            strcmp(ctx->svcs[i]->key, key) == 0) {
            return ctx->svcs[i]->impl;
        }
    }
    return NULL;
}

void vus_ctx_unregister(VusCtx *ctx, void *disc_ptr) {
    VusCtxDisc *disc = (VusCtxDisc *)disc_ptr;
    if (!disc || disc->kind != 1 || disc->ctx != ctx) return;
    for (int i = 0; i < ctx->n_svcs; i++) {
        if (ctx->svcs[i] == disc->target) {
            if (ctx->active[i] && ctx->svcs[i]->dispose) ctx->svcs[i]->dispose(ctx->svcs[i]->impl);
            ctx->active[i] = 0;
            /* 从激活顺序表移除（保持其余顺序） */
            int removed = 0;
            for (int k = 0; k < ctx->n_order; k++) {
                if (ctx->svc_order[k] == i) { removed = 1; }
                if (removed && k + 1 < ctx->n_order) ctx->svc_order[k] = ctx->svc_order[k + 1];
            }
            if (removed) ctx->n_order--;
            ctx->svcs[i] = NULL;
            break;
        }
    }
    free(disc);
}

/* ============ effect 栈 ============ */

void *vus_ctx_effect(VusCtx *ctx, void (*undo)(void *data), void *data) {
    if (!ctx || ctx->n_effects >= VUS_CTX_MAX_EFFECTS) return NULL;
    ctx->effects[ctx->n_effects].undo = undo;
    ctx->effects[ctx->n_effects].data = data;
    ctx->n_effects++;
    VusCtxDisc *disc = (VusCtxDisc *)malloc(sizeof(VusCtxDisc));
    if (!disc) return NULL;
    disc->kind = 2;
    disc->ctx = ctx;
    disc->target = (void *)(size_t)(ctx->n_effects - 1);
    return disc;
}

void vus_ctx_cancel_effect(VusCtx *ctx, void *disc_ptr) {
    VusCtxDisc *disc = (VusCtxDisc *)disc_ptr;
    if (!disc || disc->kind != 2 || disc->ctx != ctx) return;
    size_t idx = (size_t)disc->target;
    if (idx < (size_t)ctx->n_effects) {
        if (ctx->effects[idx].undo) ctx->effects[idx].undo(ctx->effects[idx].data);
        /* 移出（保持剩余顺序） */
        for (size_t k = idx; k + 1 < (size_t)ctx->n_effects; k++) ctx->effects[k] = ctx->effects[k + 1];
        ctx->n_effects--;
    }
    free(disc);
}

/* ============ 事件四模式 ============ */

void *vus_ctx_on(VusCtx *ctx, const char *ev, int mode, VusClosure *cb) {
    if (!ctx || !ev || !cb || ctx->n_listeners >= VUS_CTX_MAX_LISTENERS) return NULL;
    CtxListener *l = (CtxListener *)calloc(1, sizeof(CtxListener));
    if (!l) return NULL;
    l->ev = strdup(ev);
    l->mode = mode;
    l->cb = cb;
    vus_ref(cb);
    /* 尾部追加：EMIT/SERIAL/WATERFALL 均按注册顺序分发 */
    if (!ctx->listeners) {
        ctx->listeners = l;
    } else {
        CtxListener *t = ctx->listeners;
        while (t->next) t = t->next;
        t->next = l;
    }
    ctx->n_listeners++;
    VusCtxDisc *disc = (VusCtxDisc *)malloc(sizeof(VusCtxDisc));
    if (!disc) { vus_unref(cb); free(l->ev); free(l); ctx->n_listeners--; return NULL; }
    disc->kind = 3;
    disc->ctx = ctx;
    disc->target = l;
    return disc;
}

void vus_ctx_fire(VusCtx *ctx, const char *ev, void *args) {
    if (!ctx || !ev) return;
    for (CtxListener *l = ctx->listeners; l; l = l->next) {
        if (strcmp(l->ev, ev) != 0) continue;
        if (l->mode == VUS_EV_EMIT || l->mode == VUS_EV_PARALLEL) {
            vus_closure_call(l->cb, args);
        }
    }
}

void vus_ctx_set_stop(VusCtx *ctx, int stop) {
    if (ctx) ctx->stopped = stop;
}

int vus_ctx_stopped(VusCtx *ctx) {
    return ctx ? ctx->stopped : 0;
}

int vus_ev_mode(const char *name) {
    if (!name) return VUS_EV_EMIT;
    if (strcmp(name, "中间件") == 0 || strcmp(name, "waterfall") == 0) return VUS_EV_WATERFALL;
    if (strcmp(name, "并行") == 0 || strcmp(name, "parallel") == 0) return VUS_EV_PARALLEL;
    if (strcmp(name, "顺序") == 0 || strcmp(name, "serial") == 0) return VUS_EV_SERIAL;
    return VUS_EV_EMIT;  /* "观察"/emit/未知 */
}

void vus_ctx_pipeline(VusCtx *ctx, const char *ev, void *args, int mode) {
    if (!ctx || !ev) return;
    ctx->stopped = 0;
    for (CtxListener *l = ctx->listeners; l; l = l->next) {
        if (strcmp(l->ev, ev) != 0) continue;
        if (mode == VUS_EV_SERIAL || mode == VUS_EV_WATERFALL) {
            if (ctx->stopped) break;
            vus_closure_call(l->cb, args);
        }
    }
    ctx->stopped = 0;
}