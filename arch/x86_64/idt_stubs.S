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
