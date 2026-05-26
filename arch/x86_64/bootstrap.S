/*
 * x86_64 bootstrap: enter from GRUB in 32-bit protected mode, transition to
 * long mode with the first 4 GiB identity-mapped via 2 MiB pages, then call
 * bios_kmain_entry(magic, info_ptr) in 64-bit code. The 4 GiB window covers
 * RAM plus the high-MMIO regions where QEMU and most chipsets map the
 * framebuffer BAR.
 */

.code32
.section .text.boot, "ax", @progbits
.global _start
.extern bios_kmain_entry

_start:
    cli
    cld
    movl    %eax, mb2_magic_save
    movl    %ebx, mb2_info_save

    movl    $stack_top, %esp
    xorl    %ebp, %ebp

    /* PML4[0] -> PDPT */
    movl    $pdpt, %eax
    orl     $0x3, %eax
    movl    %eax, pml4
    movl    $0,   pml4 + 4

    /* PDPT[0..3] -> PD0..PD3 (one PD per GiB) */
    movl    $pd, %eax
    orl     $0x3, %eax
    movl    %eax, pdpt
    movl    $0,   pdpt + 4
    addl    $4096, %eax
    movl    %eax, pdpt + 8
    movl    $0,   pdpt + 12
    addl    $4096, %eax
    movl    %eax, pdpt + 16
    movl    $0,   pdpt + 20
    addl    $4096, %eax
    movl    %eax, pdpt + 24
    movl    $0,   pdpt + 28

    /* Fill 2048 PD entries mapping 0..4 GiB as 2 MiB pages */
    xorl    %ecx, %ecx
1:
    movl    %ecx, %eax
    shll    $21, %eax          /* offset = i * 2 MiB */
    orl     $0x83, %eax        /* present | writable | PS (2 MiB) */
    movl    %eax, pd(, %ecx, 8)
    movl    $0,   pd + 4(, %ecx, 8)
    incl    %ecx
    cmpl    $2048, %ecx
    jb      1b

    /* CR3 = PML4 */
    movl    $pml4, %eax
    movl    %eax, %cr3

    /* CR4.PAE = 1 */
    movl    %cr4, %eax
    orl     $(1 << 5), %eax
    movl    %eax, %cr4

    /* EFER.LME = 1 */
    movl    $0xC0000080, %ecx
    rdmsr
    orl     $(1 << 8), %eax
    wrmsr

    /* CR0.PG = 1, CR0.PE already set by GRUB */
    movl    %cr0, %eax
    orl     $(1 << 31), %eax
    movl    %eax, %cr0

    lgdt    gdt64_ptr
    ljmp    $0x08, $long_mode_entry

.code64
long_mode_entry:
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %fs
    movw    %ax, %gs
    movw    %ax, %ss

    movq    $stack_top, %rsp
    xorq    %rbp, %rbp

    /* bios_kmain_entry(uint32_t magic, uint32_t info_ptr) per SysV ABI */
    movl    mb2_magic_save(%rip), %edi
    movl    mb2_info_save(%rip),  %esi
    call    bios_kmain_entry

hang:
    cli
    hlt
    jmp     hang

/* GDT is global so the SMP AP bring-up (arch/x86_64/smp.c) can reload it
 * on each AP, giving the same CS=0x08 / SS=0x10 the IDT gates expect. */
.section .rodata
.align 8
.global gdt64
gdt64:
    .quad   0                       /* null */
    .quad   0x00AF9A000000FFFF      /* 0x08: 64-bit code, DPL=0 */
    .quad   0x00AF92000000FFFF      /* 0x10: 64-bit data, DPL=0 */
gdt64_end:

.global gdt64_ptr
gdt64_ptr:
    .word   gdt64_end - gdt64 - 1
    .quad   gdt64

.section .bss
.align 4096
.global pml4               /* APs load this as CR3 (smp.c, M3) */
pml4:
    .skip 4096
pdpt:
    .skip 4096
pd:
    .skip 16384       /* 4 page directories, one per GiB */

.align 16
stack_bottom:
    .skip 262144              /* 256 KiB - mbedtls handshake stack frames */
stack_top:

.align 4
mb2_magic_save:
    .long 0
mb2_info_save:
    .long 0
