/*
 * 32 exception entry stubs. Each pushes its vector number, normalises
 * the error-code slot (zero for vectors that don't push one), saves
 * GPRs, calls the common C handler with the trap frame pointer in
 * rdi, then halts. We do not return from exceptions - they're fatal.
 *
 * SysV ABI: caller's GPRs are at known offsets above the trap frame,
 * which is built bottom-up matching canboot_trap_frame in idt.h.
 */

.section .text
.code64

.macro EX_STUB_NOERR vec
.global canboot_isr_\vec
canboot_isr_\vec:
    pushq   $0                      /* synthetic error code */
    pushq   $\vec                   /* vector */
    jmp     canboot_isr_common
.endm

.macro EX_STUB_ERR vec
.global canboot_isr_\vec
canboot_isr_\vec:
    /* CPU pushed error code already */
    pushq   $\vec
    jmp     canboot_isr_common
.endm

EX_STUB_NOERR  0
EX_STUB_NOERR  1
EX_STUB_NOERR  2
EX_STUB_NOERR  3
EX_STUB_NOERR  4
EX_STUB_NOERR  5
EX_STUB_NOERR  6
EX_STUB_NOERR  7
EX_STUB_ERR    8
EX_STUB_NOERR  9
EX_STUB_ERR   10
EX_STUB_ERR   11
EX_STUB_ERR   12
EX_STUB_ERR   13
EX_STUB_ERR   14
EX_STUB_NOERR 15
EX_STUB_NOERR 16
EX_STUB_ERR   17
EX_STUB_NOERR 18
EX_STUB_NOERR 19
EX_STUB_NOERR 20
EX_STUB_ERR   21
EX_STUB_NOERR 22
EX_STUB_NOERR 23
EX_STUB_NOERR 24
EX_STUB_NOERR 25
EX_STUB_NOERR 26
EX_STUB_NOERR 27
EX_STUB_NOERR 28
EX_STUB_ERR   29
EX_STUB_ERR   30
EX_STUB_NOERR 31

canboot_isr_common:
    /* Save GPRs in canboot_trap_frame order. */
    pushq   %rax
    pushq   %rbx
    pushq   %rcx
    pushq   %rdx
    pushq   %rdi
    pushq   %rsi
    pushq   %rbp
    pushq   %r8
    pushq   %r9
    pushq   %r10
    pushq   %r11
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15

    /* Frame pointer = rsp. */
    movq    %rsp, %rdi
    /* Align stack for SysV (16-byte at call) - we already pushed an
     * even number of 8-byte quads since the CPU push, so we're at
     * 8 mod 16 entering the handler call. Subtract 8 to fix. */
    subq    $8, %rsp
    call    canboot_exception_handler
    addq    $8, %rsp

    /* Should not return. Hang. */
1:  cli
    hlt
    jmp     1b

/*
 * LAPIC timer IRQ stub (vector 0x20). Unlike the exception stubs this
 * one returns: it saves every general-purpose register on the
 * interrupted thread's stack, calls the C dispatcher, then restores and
 * iretq's back to the interrupted instruction.
 *
 * The C dispatcher may context-switch away (preemption). That is safe:
 * the cooperative switch (canboot_arch_ctx_switch) preserves the
 * callee-saved registers across the switch, and the caller-saved
 * registers live in the trap frame pushed here — restored by the pop
 * sequence once this thread is rescheduled and the call returns. rbp is
 * used as an alignment anchor and is itself callee-saved, so it survives
 * the switch and lets us undo the call-alignment padding on return.
 */
.global canboot_irq_timer_stub
canboot_irq_timer_stub:
    pushq   %rax
    pushq   %rcx
    pushq   %rdx
    pushq   %rbx
    pushq   %rbp
    pushq   %rsi
    pushq   %rdi
    pushq   %r8
    pushq   %r9
    pushq   %r10
    pushq   %r11
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15

    movq    %rsp, %rbp          /* anchor (original rbp already saved) */
    andq    $-16, %rsp          /* 16-byte align for the SysV call */
    call    canboot_irq_timer
    movq    %rbp, %rsp          /* undo alignment padding */

    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %r11
    popq    %r10
    popq    %r9
    popq    %r8
    popq    %rdi
    popq    %rsi
    popq    %rbp
    popq    %rbx
    popq    %rdx
    popq    %rcx
    popq    %rax
    iretq

/*
 * Spurious-interrupt vector (0xFF). Per the Intel SDM a spurious
 * interrupt must NOT be acknowledged with an EOI, so this just returns.
 */
.global canboot_irq_spurious_stub
canboot_irq_spurious_stub:
    iretq
