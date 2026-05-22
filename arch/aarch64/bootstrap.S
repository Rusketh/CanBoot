/*
 * aarch64 bootstrap entry. QEMU virt + -kernel loads us in EL2 (with
 * KVM/HVF off) or EL1 (with most accel). We don't enter the EFI path
 * here; this is the direct -kernel entry. Drop to EL1 if needed,
 * zero .bss, set up SP, jump to kmain.
 *
 * Boot info is synthesised on entry: a tiny struct boot_info that
 * advertises CANBOOT_FB_NONE and no memory map (we discover it later
 * via FDT once we wire that in). For "boots to ok on serial" this is
 * enough for kmain's existing handshake-confirmation flow.
 */

.section .text.boot, "ax", @progbits
.global _start
.extern aarch64_kmain_entry

_start:
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
    adr     x0, __stack_top
    msr     sp_el1, x0
    eret

1:
    // EL1 path - set SP, zero BSS, jump to C.
    adr     x0, __stack_top
    mov     sp, x0

    // Zero out .bss.
    adr     x0, __bss_start
    adr     x1, __bss_end
zero_bss:
    cmp     x0, x1
    b.eq    bss_done
    str     xzr, [x0], #8
    b       zero_bss
bss_done:

    bl      aarch64_kmain_entry

hang:
    wfe
    b       hang

.section .bss
.align 16
.skip 65536
__stack_top:
