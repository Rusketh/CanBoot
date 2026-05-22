/*
 * aarch64 bootstrap entry. We're loaded as a raw image via QEMU's
 * -kernel; the 64-byte Linux ARM64 boot header (Documentation/arm64/
 * booting.rst) at the very start makes QEMU honour the Linux boot
 * protocol, which puts the FDT physical address in x0 at entry. We
 * stash it in x19 (callee-saved across the EL2->EL1 eret) and restore
 * it into x0 right before the C trampoline call so aarch64_kmain_entry
 * can parse the device tree.
 *
 * For the simpler "plain ELF -kernel" path QEMU doesn't run any
 * bootloader stub and would leave x0=0, which is why the raw .bin
 * (with header) is the canonical artefact for the direct boot path.
 */

.section .text.boot, "ax", @progbits
.global _start
.extern aarch64_kmain_entry

/* Linux/ARM64 boot header. code0 must be a valid AArch64 instruction;
 * we make it an unconditional branch past the header into the real
 * entry. text_offset = 0x80000 (the conventional kernel load offset
 * inside the lowest 512 MiB of RAM), flags = 0 (no PIE / 4K pages).
 * image_size = 0 tells the loader to keep the image where it was
 * placed (QEMU honours this). */
_start:
    b       _boot_entry              // code0
    .long   0                        // code1
    .quad   0x80000                  // text_offset
    .quad   0                        // image_size (0 = wherever loaded)
    .quad   0                        // flags
    .quad   0                        // res2
    .quad   0                        // res3
    .quad   0                        // res4
    .ascii  "ARM\x64"                // magic
    .long   0                        // res5 (PE header offset; not a PE)

_boot_entry:
    // Preserve FDT pointer across the EL2->EL1 transition. eret keeps
    // general-purpose regs intact, so x19 survives the switch.
    mov     x19, x0

    /* If we landed in EL2, configure HCR_EL2 for AArch64 EL1, then
     * eret to EL1. */
    mrs     x0, CurrentEL
    cmp     x0, #(2 << 2)            // CurrentEL == EL2 ?
    b.ne    1f

    // Set HCR_EL2.RW = 1 (EL1 runs in AArch64).
    mov     x0, #(1 << 31)
    msr     hcr_el2, x0

    // Set SPSR_EL2 = EL1h, masked.
    mov     x0, #0x3c5
    msr     spsr_el2, x0

    // Set ELR_EL2 to the EL1 continuation.
    adr     x0, 1f
    msr     elr_el2, x0

    // Set EL1 stack pointer before eret.
    adrp    x0, __stack_top
    add     x0, x0, #:lo12:__stack_top
    msr     sp_el1, x0
    eret

1:
    // EL1 path - enable FP/SIMD access, set SP, zero BSS, restore FDT,
    // jump to C.
    //
    // CPACR_EL1.FPEN bits [21:20] = 0b11 disables EL0/EL1 FP/SIMD traps.
    // We don't otherwise touch FP regs in the kernel (-mgeneral-regs-only),
    // but picolibc internally uses NEON/FP and would otherwise trap on
    // first FP instruction.
    mrs     x0, cpacr_el1
    orr     x0, x0, #(3 << 20)
    msr     cpacr_el1, x0
    isb

    adrp    x0, __stack_top
    add     x0, x0, #:lo12:__stack_top
    mov     sp, x0

    // Zero out .bss. Use adrp+add since the bss now spans more than the
    // ±1 MiB adr range once picolibc's static heap is linked in.
    adrp    x0, __bss_start
    add     x0, x0, #:lo12:__bss_start
    adrp    x1, __bss_end
    add     x1, x1, #:lo12:__bss_end
zero_bss:
    cmp     x0, x1
    b.eq    bss_done
    str     xzr, [x0], #8
    b       zero_bss
bss_done:

    mov     x0, x19                  // x0 = FDT phys ptr (QEMU's x0 at entry)
    bl      aarch64_kmain_entry

hang:
    wfe
    b       hang

.section .bss
.align 16
.skip 65536
__stack_top:
