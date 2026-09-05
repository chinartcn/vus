/*
 * vus_coro.h — VUS 运行时轻量级协程
 *
 * 设计目标：
 *   1. 跨平台（x86 / x86_64 / ARM32 / ARM64 / Android Termux）
 *   2. 不依赖 ucontext（Android/Bionic 不提供）
 *   3. 纯 C 实现 + 少量平台特定汇编，可被 C11 编译器直接编译
 *
 * 原理：
 *   基于 setjmp / longjmp + 手工切换栈指针（SP）/ 寄存器的轻量级协程。
 *   每个协程拥有独立的 64KB 栈，调度在主协程与子协程之间进行。
 *
 * 接口：
 *   vus_coro_create  - 创建协程（分配栈，挂起启动函数）
 *   vus_coro_resume  - 恢复 / 启动协程，首次调用即进入入口函数
 *   vus_coro_yield   - 当前协程让出，切回主协程
 *   vus_coro_is_done - 协程入口函数是否已返回
 *   vus_coro_free    - 释放协程对象与栈
 */

#ifndef VUS_CORO_H
#define VUS_CORO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#define VUS_CORO_STACK_SIZE  (128 * 1024)   /* 128KB 协程栈，足够日常使用 */

typedef struct VusCoroutine VusCoroutine;

VusCoroutine* vus_coro_create(void (*func)(void*), void* arg);
void          vus_coro_resume(VusCoroutine* coro);
void          vus_coro_yield(void);
int           vus_coro_is_done(VusCoroutine* coro);
void          vus_coro_free(VusCoroutine* coro);
void          vus_coro_store_result(void* result);
VusCoroutine* vus_coro_current(void);

#ifdef __cplusplus
}
#endif

#endif /* VUS_CORO_H */
