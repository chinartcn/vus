/*
 * vus_cordis.h — Cordis 式运行时上下文元框架（路线 A：C 元层）
 *
 * 提供：上下文（服务容器）、依赖注入（DAG 拓扑序激活/逆序回退）、
 * 可逆副作用（effect 栈）、事件四模式（EMIT/PARALLEL/SERIAL/WATERFALL）。
 * 与既有系统接线：VUA 事件（VusClosure）复用、FFI 桥域可登记为服务。
 *
 * 生命周期约定：
 *   - 所有注册函数返回一个 disposer 句柄，可提前撤销，也可交给
 *     vus_ctx_dispose 在销毁时按注册逆序统一回退（"拔得干净"）。
 *   - 服务回调不持有引用，回调内涉及 VUS 对象需自行 vus_ref/vus_unref。
 */
#ifndef VUS_CORDIS_H
#define VUS_CORDIS_H

#include "libvus_rt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 事件分发模式（每个事件一次声明一个模式，公开契约） */
enum {
    VUS_EV_EMIT = 0,      /* 观察者广播：按注册顺序，不等待，无短路 */
    VUS_EV_PARALLEL,      /* 并行观察：按注册顺序逐个调用（单线程模拟并发） */
    VUS_EV_SERIAL,        /* 顺序执行：按注册顺序调用，回调可写 args 传递；可 stop 短路 */
    VUS_EV_WATERFALL,     /* 中间件链：同 SERIAL，回调可修改 args 给下游；可 stop 短路 */
};

/* 上下文（服务容器） */
typedef struct VusCtx VusCtx;

/* 服务描述符 */
typedef struct VusCtxService {
    const char *key;                  /* 服务键（ctx 内唯一） */
    const char **deps;                /* 依赖的服务键列表，NULL 结尾；驱动 DI 拓扑激活 */
    void (*init)(void *impl);         /* 激活（依赖就绪后调用，可为 NULL） */
    void (*dispose)(void *impl);      /* 逆序卸载（可为 NULL） */
    void *impl;                       /* 实现数据 */
} VusCtxService;

/* ---- 上下文生命周期 ---- */
VusCtx *vus_ctx_create(void);
/* 逆序跑 effect 栈 + 逆序调用服务 dispose（服务按激活序倒序），随后释放容器 */
void    vus_ctx_dispose(VusCtx *ctx);

/* ---- 服务容器 + DI ---- */
/* 注册服务：依赖全部就绪时立即按拓扑序激活；返回 disposer（NULL=失败，如环依赖）
 * 环依赖检测：一轮扫描无新进展即拒绝，错误信息入 ctx。 */
void *vus_ctx_register(VusCtx *ctx, VusCtxService *svc);
/* 按键取已激活服务的 impl（未激活/不存在返回 NULL，borrow 语义） */
void *vus_ctx_get(VusCtx *ctx, const char *key);
/* 撤销单个服务：执行其 dispose 并移除（effect 由调用方另行撤销） */
void  vus_ctx_unregister(VusCtx *ctx, void *disposer);

/* ---- effect 栈（可逆副作用） ---- */
/* 登记副作用；undo 在 vus_ctx_dispose 或显式撤销时被调用（栈序，后注册先撤销） */
void *vus_ctx_effect(VusCtx *ctx, void (*undo)(void *data), void *data);
void  vus_ctx_cancel_effect(VusCtx *ctx, void *disposer);

/* ---- 事件四模式 ---- */
/* 登记监听器：ev 为事件名，mode 声明分发模式；cb 为 VusClosure（env, args），
 * args 由调用方持有转发；返回 disposer（NULL=失败） */
void *vus_ctx_on(VusCtx *ctx, const char *ev, int mode, VusClosure *cb);
/* 广播（EMIT/PARALLEL）：按注册顺序调用全部监听器，args 原样透传 */
void  vus_ctx_fire(VusCtx *ctx, const char *ev, void *args);
/* 管线（SERIAL/WATERFALL）：按注册顺序调用；每个回调可经 args 修改传给下游；
 * 回调可用 vus_ctx_set_stop 短路后续（stop 后立即返回） */
void  vus_ctx_pipeline(VusCtx *ctx, const char *ev, void *args, int mode);
/* 短路标志（WATERFALL/SERIAL 用） */
void  vus_ctx_set_stop(VusCtx *ctx, int stop);
int   vus_ctx_stopped(VusCtx *ctx);

/* 事件模式字符串 → 枚举（"观察|中间件|并行|顺序" 或英文别名；未知归 EMIT） */
int   vus_ev_mode(const char *name);

/* ---- 诊断 ---- */
const char *vus_ctx_last_error(VusCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* VUS_CORDIS_H */