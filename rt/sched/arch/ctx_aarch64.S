/*
 * aarch64 thread context switch.
 *
 *   void canboot_arch_ctx_switch(struct canboot_arch_ctx *prev,  // x0
 *                                struct canboot_arch_ctx *next); // x1
 *
 * struct canboot_arch_ctx is { void *sp; }. We save the AAPCS64
 * callee-saved state — x19..x28, x29 (fp), x30 (lr), and the lower 64
 * bits of v8..v15 (d8..d15, which AAPCS64 also makes callee-saved) —
 * onto the current stack, record sp into prev->sp, load next->sp,
 * reload the incoming thread's saved state, and ret (branch to the
 * restored x30: a prior call site, or thread_trampoline for a new
 * thread).
 *
 * 160 bytes = 12 GP regs (96) + 8 FP regs (64); a multiple of 16, so sp
 * stays 16-aligned throughout. The store order MUST match
 * arch_ctx_init() in rt/sched/sched.c.
 */

.section .text

.global canboot_arch_ctx_switch
canboot_arch_ctx_switch:
    sub     sp, sp, #160

    stp     x19, x20, [sp, #0]
    stp     x21, x22, [sp, #16]
    stp     x23, x24, [sp, #32]
    stp     x25, x26, [sp, #48]
    stp     x27, x28, [sp, #64]
    stp     x29, x30, [sp, #80]
    stp     d8,  d9,  [sp, #96]
    stp     d10, d11, [sp, #112]
    stp     d12, d13, [sp, #128]
    stp     d14, d15, [sp, #144]

    mov     x9, sp
    str     x9, [x0]            /* prev->sp = current sp */
    ldr     x9, [x1]            /* next sp               */
    mov     sp, x9

    ldp     x19, x20, [sp, #0]
    ldp     x21, x22, [sp, #16]
    ldp     x23, x24, [sp, #32]
    ldp     x25, x26, [sp, #48]
    ldp     x27, x28, [sp, #64]
    ldp     x29, x30, [sp, #80]
    ldp     d8,  d9,  [sp, #96]
    ldp     d10, d11, [sp, #112]
    ldp     d12, d13, [sp, #128]
    ldp     d14, d15, [sp, #144]

    add     sp, sp, #160
    ret
