/*
 * aarch64 EL1 exception vector table (VBAR_EL1).
 *
 * We run at EL1h (SP_EL1) after bootstrap, so the timer IRQ arrives at
 * the "current EL, SP_ELx -> IRQ" slot (offset 0x280). That slot saves
 * the full integer register frame plus ELR/SPSR, calls the C IRQ
 * dispatcher (GIC ack -> generic-timer tick -> scheduler), then restores
 * and eret's. The scheduler may context-switch from inside the C
 * handler; the saved frame on the interrupted thread's stack is what
 * gets restored when it is later switched back in (mirrors the x86_64
 * timer stub in arch/x86_64/idt_stubs.S).
 *
 * Every other vector funnels into a fault reporter that dumps ESR/ELR
 * and halts, so an unexpected synchronous exception or abort is visible
 * on serial instead of silently looping.
 */

.section .text
.macro SAVE_FRAME
    sub     sp, sp, #272
    stp     x0,  x1,  [sp, #0]
    stp     x2,  x3,  [sp, #16]
    stp     x4,  x5,  [sp, #32]
    stp     x6,  x7,  [sp, #48]
    stp     x8,  x9,  [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    mrs     x9,  elr_el1
    mrs     x10, spsr_el1
    stp     x30, x9,  [sp, #240]
    str     x10, [sp, #256]
.endm

.macro RESTORE_FRAME
    ldr     x10, [sp, #256]
    ldp     x30, x9,  [sp, #240]
    msr     spsr_el1, x10
    msr     elr_el1,  x9
    ldp     x0,  x1,  [sp, #0]
    ldp     x2,  x3,  [sp, #16]
    ldp     x4,  x5,  [sp, #32]
    ldp     x6,  x7,  [sp, #48]
    ldp     x8,  x9,  [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    add     sp, sp, #272
.endm

/* The real timer IRQ path. */
irq_entry:
    SAVE_FRAME
    bl      aarch64_irq_handler
    RESTORE_FRAME
    eret

/* Unexpected exception: report vector index (x0) + ESR/ELR and halt. */
.macro FAULT idx
    SAVE_FRAME
    mov     x0, #\idx
    mrs     x1, esr_el1
    mrs     x2, elr_el1
    bl      aarch64_fault_handler
    RESTORE_FRAME
    eret
.endm

fault_0:  FAULT 0
fault_1:  FAULT 1
fault_2:  FAULT 2
fault_3:  FAULT 3
fault_4:  FAULT 4
fault_5:  FAULT 5
fault_6:  FAULT 6
fault_7:  FAULT 7

.align 11
.global canboot_aarch64_vectors
canboot_aarch64_vectors:
    /* Current EL, SP_EL0 */
    .align 7
    b       fault_0          /* Synchronous */
    .align 7
    b       fault_1          /* IRQ         */
    .align 7
    b       fault_2          /* FIQ         */
    .align 7
    b       fault_3          /* SError      */

    /* Current EL, SP_ELx (where we run: EL1h) */
    .align 7
    b       fault_4          /* Synchronous */
    .align 7
    b       irq_entry        /* IRQ         */
    .align 7
    b       fault_5          /* FIQ         */
    .align 7
    b       fault_6          /* SError      */

    /* Lower EL, AArch64 (no EL0 userspace, so unexpected) */
    .align 7
    b       fault_7
    .align 7
    b       fault_7
    .align 7
    b       fault_7
    .align 7
    b       fault_7

    /* Lower EL, AArch32 (unused) */
    .align 7
    b       fault_7
    .align 7
    b       fault_7
    .align 7
    b       fault_7
    .align 7
    b       fault_7
