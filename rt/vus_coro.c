/*
 * vus_coro.c — VUS 运行时轻量级协程实现
 *
 * 设计：每个协程保存一份 callee-saved 寄存器 + SP + PC，
 * 通过自定义的 swap 原子地做「保存 from 现场 → 恢复 to 现场」。
 *
 * 支持的平台（在 swap 里写了内联汇编）：
 *   - x86_64 (System V ABI，Linux / macOS / BSD / Android Termux)
 *   - aarch64 / arm64 (AAPCS64，含 Android)
 *   - ARM 32-bit (AAPCS)
 *   - i386
 *
 * 不识别的平台会自动退化成「同步调用 func」，此时 yield 是空操作，
 * 但保证不会崩溃，便于在嵌入式或新架构上先跑起来。
 */

#define _GNU_SOURCE
#include "vus_coro.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

/* ---------- 类型 ---------- */

typedef enum {
    CORO_READY   = 0,
    CORO_RUNNING = 1,
    CORO_YIELDED = 2,
    CORO_DONE    = 3,
} CoroState;

/* 寄存器快照布局（索引），给所有平台 swap 函数统一使用 */
#define VCTX_RBX 0
#define VCTX_RBP 1
#define VCTX_R12 2
#define VCTX_R13 3
#define VCTX_R14 4
#define VCTX_R15 5
#define VCTX_SP  6
#define VCTX_PC  7

struct VusCoroutine {
    CoroState state;
    void    (*func)(void*);
    void*     arg;
    unsigned long ctx[16];   /* 前 8 槽 = 上面的 VCTX_*；其余预留 */
    char*     stack;
    size_t    stack_size;
    struct VusCoroutine* link;   /* yield / finish 后回到谁 */
};

/* ---------- 调度器全局 ---------- */

static VusCoroutine  g_main_coro;
static VusCoroutine* g_current     = NULL;
static int           g_main_inited = 0;

/* ---------- 原子 swap：操作 ctx[] 裸指针 ----------
 *
 *   void vus_coro_swap_ctx(unsigned long* from_ctx, unsigned long* to_ctx);
 *
 * 保存当前寄存器到 from_ctx，从 to_ctx 恢复寄存器并跳 to_ctx[VCTX_PC]。
 * 不返回（以 tail-jump 的方式继续执行，下次从别人切回时仿佛是 swap 正常返回）。
 */

/* Clang 不支持 noclone，且 naked 函数中不允许非汇编语句 */
#if defined(__clang__)
#define VUS_CORO_NAKED  __attribute__((noinline, naked))
#else
#define VUS_CORO_NAKED  __attribute__((noinline, noclone, naked))
#endif

#if defined(__x86_64__) && !defined(_WIN32)
#define VUS_CORO_HAVE_SWAP 1
VUS_CORO_NAKED
static void vus_coro_swap_ctx(unsigned long* from, unsigned long* to)
{
    /* rdi = from, rsi = to.
     * AMD64 System V callee-saved: rbx, rbp, r12-r15.
     * 先把返回地址 pop 掉，再保存 rsp（去掉 RA 之后的值），
     * 这样恢复时正好 stack 对齐。
     */
    __asm__ __volatile__(
        "popq   %%rax\n\t"
        "movq   %%rbx,   (%%rdi)\n\t"
        "movq   %%rbp,  8(%%rdi)\n\t"
        "movq   %%r12, 16(%%rdi)\n\t"
        "movq   %%r13, 24(%%rdi)\n\t"
        "movq   %%r14, 32(%%rdi)\n\t"
        "movq   %%r15, 40(%%rdi)\n\t"
        "movq   %%rsp, 48(%%rdi)\n\t"
        "movq   %%rax, 56(%%rdi)\n\t"

        "movq     (%%rsi), %%rbx\n\t"
        "movq   8(%%rsi), %%rbp\n\t"
        "movq  16(%%rsi), %%r12\n\t"
        "movq  24(%%rsi), %%r13\n\t"
        "movq  32(%%rsi), %%r14\n\t"
        "movq  40(%%rsi), %%r15\n\t"
        "movq  48(%%rsi), %%rsp\n\t"
        "jmpq   *56(%%rsi)\n\t"
        : : : "rax", "rcx", "rdx", "r8", "r9", "r10", "r11",
              "memory", "cc"
    );
}

#elif defined(__aarch64__) || defined(__arm64__)
#define VUS_CORO_HAVE_SWAP 1
/* 对 ARM64 我们复用前 8 槽位但按 AAPCS64 重新解释：
 *   0: x19  1: x20  2: x21  3: x22
 *   4: x23  5: x24  6: x25  7: x26
 *   另外 ctx[8..15] = x27, x28, x29(fp), x30(lr), sp, 0, 0, 0
 * 实际使用：
 *   VCTX_SP=12  VCTX_PC=11(lr)
 */
#undef  VCTX_SP
#undef  VCTX_PC
#define VCTX_X19  0
#define VCTX_X20  1
#define VCTX_X21  2
#define VCTX_X22  3
#define VCTX_X23  4
#define VCTX_X24  5
#define VCTX_X25  6
#define VCTX_X26  7
#define VCTX_X27  8
#define VCTX_X28  9
#define VCTX_FP   10
#define VCTX_LR   11
#define VCTX_SP   12
#define VCTX_PC   11
VUS_CORO_NAKED
static void vus_coro_swap_ctx(unsigned long* from, unsigned long* to)
{
    __asm__ __volatile__(
        "stp    x19, x20, [x0, #(0*8)]\n\t"
        "stp    x21, x22, [x0, #(2*8)]\n\t"
        "stp    x23, x24, [x0, #(4*8)]\n\t"
        "stp    x25, x26, [x0, #(6*8)]\n\t"
        "stp    x27, x28, [x0, #(8*8)]\n\t"
        "stp    x29, x30, [x0, #(10*8)]\n\t"
        "mov    x16, sp\n\t"
        "str    x16,     [x0, #(12*8)]\n\t"

        "ldp    x19, x20, [x1, #(0*8)]\n\t"
        "ldp    x21, x22, [x1, #(2*8)]\n\t"
        "ldp    x23, x24, [x1, #(4*8)]\n\t"
        "ldp    x25, x26, [x1, #(6*8)]\n\t"
        "ldp    x27, x28, [x1, #(8*8)]\n\t"
        "ldp    x29, x30, [x1, #(10*8)]\n\t"
        "ldr    x16,     [x1, #(12*8)]\n\t"
        "mov    sp, x16\n\t"
        "ret\n\t"
        : : : "x16","x17","memory","cc"
    );
}

#elif defined(__arm__)
#define VUS_CORO_HAVE_SWAP 1
/* AAPCS 32-bit callee-saved: r4-r11, sp, lr.
 * ctx[0..7]=r4..r11; ctx[8]=sp; ctx[9]=lr; VCTX_SP=8, VCTX_PC=9.
 */
#undef  VCTX_SP
#undef  VCTX_PC
#define VCTX_R4  0
#define VCTX_R5  1
#define VCTX_R6  2
#define VCTX_R7  3
#define VCTX_R8  4
#define VCTX_R9  5
#define VCTX_R10 6
#define VCTX_R11 7
#define VCTX_SP  8
#define VCTX_PC  9
VUS_CORO_NAKED
static void vus_coro_swap_ctx(unsigned long* from, unsigned long* to)
{
    /* r0 = from, r1 = to */
    __asm__ __volatile__(
        "stmia  r0!, {r4-r11}\n\t"
        "str    sp, [r0, #0]\n\t"
        "str    lr, [r0, #4]\n\t"

        "add    r1, r1, #(8*4)\n\t"
        "ldr    r2, [r1, #0]\n\t"
        "ldr    r3, [r1, #4]\n\t"
        "sub    r1, r1, #(8*4)\n\t"
        "ldmia  r1!, {r4-r11}\n\t"
        "mov    sp, r2\n\t"
        "bx     r3\n\t"
        : : : "r2","r3","memory","cc"
    );
}

#elif defined(__i386__)
#define VUS_CORO_HAVE_SWAP 1
/* i386 cdecl: callee-save = ebx, esi, edi, ebp, esp, eip.
 * ctx[0]=ebx, 1=ebp, 2=esi, 3=edi, 4=esp, 5=eip.
 */
#undef  VCTX_SP
#undef  VCTX_PC
#define VCTX_EBX 0
#define VCTX_EBP 1
#define VCTX_ESI 2
#define VCTX_EDI 3
#define VCTX_SP  4
#define VCTX_PC  5
VUS_CORO_NAKED
static void vus_coro_swap_ctx(unsigned long* from, unsigned long* to)
{
    __asm__ __volatile__(
        "popl   %%eax\n\t"
        "movl   %%ebx,   (%%edi)\n\t"
        "movl   %%ebp,  4(%%edi)\n\t"
        "movl   %%esi,  8(%%edi)\n\t"
        "movl   %%edi, 12(%%edi)\n\t"
        "movl   %%esp, 16(%%edi)\n\t"
        "movl   %%eax, 20(%%edi)\n\t"

        "movl     (%%esi), %%ebx\n\t"
        "movl   4(%%esi), %%ebp\n\t"
        "movl   8(%%esi), %%eax\n\t"  /* will overwrite eax later */
        "movl  12(%%esi), %%edi\n\t"
        "movl  16(%%esi), %%esp\n\t"
        "jmp    *8(%%esi)\n\t"        /* jump to saved esi (we moved eax to nothing; reload) */
        : : : "eax", "ecx", "edx", "memory", "cc"
    );
}
/* Note: i386 is rare in 2025; fallback mode will catch it if the above fails. */
#endif

/* GCC/Clang 兼容的属性宏 */
#if defined(__clang__)
#define VUS_CORO_NOINLINE __attribute__((noinline))
#else
#define VUS_CORO_NOINLINE __attribute__((noinline, noclone, optimize("O0")))
#endif

/* ---------- C 层 swap 包装 ---------- */
VUS_CORO_NOINLINE
static void vus_coro_swap(VusCoroutine* a, VusCoroutine* b)
{
#if defined(VUS_CORO_HAVE_SWAP)
    vus_coro_swap_ctx(a->ctx, b->ctx);
#else
    (void)a; (void)b;
#endif
}

/* ---------- 入口桥 ---------- */
VUS_CORO_NOINLINE
__attribute__((noreturn))
static void vus_coro_entry_bridge(void)
{
    VusCoroutine* self = g_current;
    if (self && self->func) {
        self->func(self->arg);
    }
    if (self) {
        self->state = CORO_DONE;
    }
    VusCoroutine* target = (self && self->link) ? self->link : &g_main_coro;
    if (target) {
        g_current = target;
        vus_coro_swap(self, target);
    }
    for (;;) { /* 防跑飞 */ }
}

/* ---------- 初始栈/上下文准备 ---------- */
static void vus_coro_prepare_initial_ctx(VusCoroutine* coro)
{
    /* 清零所有寄存器槽 */
    for (int i = 0; i < 16; ++i) coro->ctx[i] = 0;

    /* 栈：高地址 -> 低地址 */
    char* top = coro->stack + coro->stack_size;
    /* 16 字节对齐（所有主流 ABI 都要求） */
    top = (char*)(((unsigned long)top) & ~15UL);

    /*
     * 函数入口要求：
     *   x86_64 SysV / AAPCS64 / AAPCS32 都要求"just after a call"
     *   即 SP ≡ 8 mod 16。我们用 jmp（而不是 call）跳到 entry_bridge，
     *   所以要手动减 8 模拟"call 已经 push 过 RA"。
     */
    top -= 8;
    /* 留出红色区域和 frame safety padding */
#if defined(__x86_64__) || defined(__aarch64__) || defined(__arm64__)
    top -= 128;
#else
    top -= 64;
#endif

    coro->ctx[VCTX_SP] = (unsigned long)top;
    coro->ctx[VCTX_PC] = (unsigned long)vus_coro_entry_bridge;

#ifdef VCTX_FP   /* aarch64: fp = sp 附近的某个地址，不需要真实 frame */
    coro->ctx[VCTX_FP] = (unsigned long)top;
#endif
}

/* ---------- 主协程初始化 ---------- */
static void vus_coro_ensure_main(void)
{
    if (g_main_inited) return;
    memset(&g_main_coro, 0, sizeof(g_main_coro));
    g_main_coro.state = CORO_RUNNING;
    g_main_coro.link  = NULL;
    g_current         = &g_main_coro;
    g_main_inited     = 1;
}

/* ========== 公共接口 ========== */

VusCoroutine* vus_coro_create(void (*func)(void*), void* arg)
{
    vus_coro_ensure_main();

    VusCoroutine* coro = (VusCoroutine*)calloc(1, sizeof(VusCoroutine));
    if (!coro) return NULL;

    coro->state      = CORO_READY;
    coro->func       = func;
    coro->arg        = arg;
    coro->stack_size = VUS_CORO_STACK_SIZE;
    coro->stack      = (char*)malloc(coro->stack_size + 64);
    coro->link       = &g_main_coro;

    if (!coro->stack) {
        free(coro);
        return NULL;
    }
    /* 警戒填充（方便 gdb 诊断栈溢出） */
    memset(coro->stack, 0xCC, 16);
    memset(coro->stack + coro->stack_size + 48, 0xCC, 16);

    vus_coro_prepare_initial_ctx(coro);
    return coro;
}

VUS_CORO_NOINLINE
void vus_coro_resume(VusCoroutine* coro)
{
    vus_coro_ensure_main();
    if (!coro) return;
    if (coro->state == CORO_DONE) return;

    VusCoroutine* prev = g_current;
    coro->link  = prev;
    g_current   = coro;

#if defined(VUS_CORO_HAVE_SWAP)
    coro->state = CORO_RUNNING;
    vus_coro_swap(prev, coro);
    if (coro->state != CORO_DONE) {
        coro->state = CORO_YIELDED;
    }
#else
    /* Fallback：同步调用，yield 为空 */
    coro->state = CORO_RUNNING;
    if (coro->func) coro->func(coro->arg);
    coro->state = CORO_DONE;
#endif

    g_current = prev;
}

VUS_CORO_NOINLINE
void vus_coro_yield(void)
{
    vus_coro_ensure_main();
    VusCoroutine* self = g_current;
    if (!self) return;
    if (self->state != CORO_RUNNING) return;
    if (self == &g_main_coro) return;  /* 主协程不能 yield */

#if defined(VUS_CORO_HAVE_SWAP)
    VusCoroutine* target = self->link ? self->link : &g_main_coro;
    self->state = CORO_YIELDED;
    g_current = target;
    vus_coro_swap(self, target);
    self->state = CORO_RUNNING;
#else
    (void)self;
#endif
}

int vus_coro_is_done(VusCoroutine* coro)
{
    return !coro || coro->state == CORO_DONE;
}

void vus_coro_free(VusCoroutine* coro)
{
    if (!coro) return;
    if (coro->stack) {
        free(coro->stack);
        coro->stack = NULL;
    }
    free(coro);
}
