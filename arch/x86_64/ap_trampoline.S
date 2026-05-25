/*
 * Application-Processor trampoline (M3) — HIGH RISK, UNVERIFIED.
 *
 * APs start in 16-bit real mode at physical 0x8000 (SIPI vector 0x08).
 * This blob walks them 16 -> 32 -> 64 bit, loads the BSP's page tables
 * (CR3 patched in by smp.c), then jumps to canboot_ap_main on a per-AP
 * stack the BSP staged in canboot_ap_stack.
 *
 * It is copied verbatim to 0x8000 at runtime, so the early (paging-off)
 * code addresses everything relative to its load base:
 *   - real-mode operands use DS = CS (base 0x8000) + (label - start),
 *   - the far-jump targets and GDT base are (TRAMP_BASE + (label-start)).
 * The 64-bit tail runs with the kernel's identity map, so it can use
 * movabs against real kernel symbols (canboot_ap_stack / _ap_main).
 *
 * The 5 GDT descriptors mirror bootstrap.S's long-mode layout.
 */

#define TRAMP_BASE 0x8000

    .section .text
    .code16
    .global canboot_ap_trampoline_start
    .global canboot_ap_trampoline_end
    .global canboot_ap_trampoline_cr3

canboot_ap_trampoline_start:
    cli
    cld
    movw    %cs, %ax              /* CS = 0x0800 after SIPI; base = 0x8000 */
    movw    %ax, %ds              /* DS base = 0x8000 for label-relative ops */

    lgdtl   (gdt_desc - canboot_ap_trampoline_start)

    movl    %cr0, %eax
    orl     $1, %eax              /* CR0.PE */
    movl    %eax, %cr0
    ljmpl   $0x08, $(prot32 - canboot_ap_trampoline_start + TRAMP_BASE)

    .code32
prot32:
    movw    $0x10, %ax           /* 32-bit data selector */
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movw    %ax, %fs
    movw    %ax, %gs

    movl    %cr4, %eax
    orl     $(1 << 5), %eax      /* CR4.PAE */
    movl    %eax, %cr4

    movl    (canboot_ap_trampoline_cr3 - canboot_ap_trampoline_start + TRAMP_BASE), %eax
    movl    %eax, %cr3           /* PML4 phys (< 4 GiB), patched by smp.c */

    movl    $0xC0000080, %ecx    /* EFER */
    rdmsr
    orl     $(1 << 8), %eax      /* EFER.LME */
    wrmsr

    movl    %cr0, %eax
    orl     $(1 << 31), %eax     /* CR0.PG (PE still set) */
    movl    %eax, %cr0
    ljmpl   $0x18, $(long64 - canboot_ap_trampoline_start + TRAMP_BASE)

    .code64
long64:
    movw    $0x10, %ax           /* data descriptors are ignored in 64-bit */
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movw    %ax, %fs
    movw    %ax, %gs

    movabsq $canboot_ap_stack, %rax
    movq    (%rax), %rsp         /* per-AP stack top staged by the BSP */
    xorq    %rbp, %rbp

    movabsq $canboot_ap_main, %rax
    call    *%rax                /* never returns */
1:  hlt
    jmp     1b

    .align 16
gdt:
    .quad   0                          /* 0x00 null */
    .quad   0x00CF9A000000FFFF         /* 0x08 32-bit code */
    .quad   0x00CF92000000FFFF         /* 0x10 32-bit data */
    .quad   0x00AF9A000000FFFF         /* 0x18 64-bit code (L=1) */
    .quad   0x00AF92000000FFFF         /* 0x20 64-bit data */
gdt_end:

gdt_desc:
    .word   gdt_end - gdt - 1
    .long   gdt - canboot_ap_trampoline_start + TRAMP_BASE  /* linear GDT base */

canboot_ap_trampoline_cr3:
    .quad   0                          /* patched: BSP PML4 physical addr */

canboot_ap_trampoline_end:
