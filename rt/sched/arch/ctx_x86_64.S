/*
 * x86_64 thread context switch.
 *
 *   void canboot_arch_ctx_switch(struct canboot_arch_ctx *prev,  // rdi
 *                                struct canboot_arch_ctx *next); // rsi
 *
 * struct canboot_arch_ctx is { void *sp; } — a single saved stack
 * pointer. We push the SysV callee-saved registers (rbp, rbx, r12-r15)
 * onto the current stack, stash rsp into prev->sp, load next->sp, pop
 * the incoming thread's saved registers, and ret into wherever it was
 * parked (a prior call site, or thread_trampoline for a new thread).
 *
 * Caller-saved registers and x87/SSE state are handled by the C ABI
 * around the call site (SysV has no callee-saved XMM regs), which is
 * why a plain function-call context switch is correct for the
 * cooperative (M1) switch points. The preemptive IRQ path (M2) will
 * save a full trap frame instead and not route through here.
 *
 * The push order below MUST stay in sync with arch_ctx_init() in
 * rt/sched/sched.c, which fabricates this exact frame for new threads.
 */

.section .text
.code64

.global canboot_arch_ctx_switch
canboot_arch_ctx_switch:
    pushq   %rbp
    pushq   %rbx
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15

    movq    %rsp, (%rdi)        /* prev->sp = current rsp */
    movq    (%rsi), %rsp        /* rsp = next->sp         */

    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    popq    %rbp
    ret
