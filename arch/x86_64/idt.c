/*
 * IDT install + C-level exception handler. Prints a trap frame on any
 * CPU exception (#PF, #GP, #UD, ...) and halts. Interrupts above
 * vector 31 are masked out (we install no IRQ handlers); hardware
 * interrupts are never enabled until the preemptive scheduler
 * milestone.
 */

#include <stdint.h>
#include <stdio.h>

#include "idt.h"

struct __attribute__((packed)) idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
};

struct __attribute__((packed)) idtr_desc {
    uint16_t limit;
    uint64_t base;
};

#define IDT_ENTRIES 256
static struct idt_entry idt[IDT_ENTRIES] __attribute__((aligned(16)));

extern void canboot_isr_0(void);
extern void canboot_isr_1(void);
extern void canboot_isr_2(void);
extern void canboot_isr_3(void);
extern void canboot_isr_4(void);
extern void canboot_isr_5(void);
extern void canboot_isr_6(void);
extern void canboot_isr_7(void);
extern void canboot_isr_8(void);
extern void canboot_isr_9(void);
extern void canboot_isr_10(void);
extern void canboot_isr_11(void);
extern void canboot_isr_12(void);
extern void canboot_isr_13(void);
extern void canboot_isr_14(void);
extern void canboot_isr_15(void);
extern void canboot_isr_16(void);
extern void canboot_isr_17(void);
extern void canboot_isr_18(void);
extern void canboot_isr_19(void);
extern void canboot_isr_20(void);
extern void canboot_isr_21(void);
extern void canboot_isr_22(void);
extern void canboot_isr_23(void);
extern void canboot_isr_24(void);
extern void canboot_isr_25(void);
extern void canboot_isr_26(void);
extern void canboot_isr_27(void);
extern void canboot_isr_28(void);
extern void canboot_isr_29(void);
extern void canboot_isr_30(void);
extern void canboot_isr_31(void);

static void set_gate(int n, uintptr_t handler) {
    uint16_t cs;
    __asm__ volatile ("mov %%cs, %0" : "=r"(cs));
    idt[n].offset_low  = (uint16_t)(handler & 0xFFFFu);
    idt[n].selector    = cs;
    idt[n].ist         = 0;
    idt[n].type_attr   = 0x8E;   /* present, ring 0, 64-bit interrupt gate */
    idt[n].offset_mid  = (uint16_t)((handler >> 16) & 0xFFFFu);
    idt[n].offset_high = (uint32_t)((handler >> 32) & 0xFFFFFFFFu);
    idt[n].reserved    = 0;
}

void canboot_idt_install(void) {
    uintptr_t stubs[32] = {
        (uintptr_t)canboot_isr_0,  (uintptr_t)canboot_isr_1,
        (uintptr_t)canboot_isr_2,  (uintptr_t)canboot_isr_3,
        (uintptr_t)canboot_isr_4,  (uintptr_t)canboot_isr_5,
        (uintptr_t)canboot_isr_6,  (uintptr_t)canboot_isr_7,
        (uintptr_t)canboot_isr_8,  (uintptr_t)canboot_isr_9,
        (uintptr_t)canboot_isr_10, (uintptr_t)canboot_isr_11,
        (uintptr_t)canboot_isr_12, (uintptr_t)canboot_isr_13,
        (uintptr_t)canboot_isr_14, (uintptr_t)canboot_isr_15,
        (uintptr_t)canboot_isr_16, (uintptr_t)canboot_isr_17,
        (uintptr_t)canboot_isr_18, (uintptr_t)canboot_isr_19,
        (uintptr_t)canboot_isr_20, (uintptr_t)canboot_isr_21,
        (uintptr_t)canboot_isr_22, (uintptr_t)canboot_isr_23,
        (uintptr_t)canboot_isr_24, (uintptr_t)canboot_isr_25,
        (uintptr_t)canboot_isr_26, (uintptr_t)canboot_isr_27,
        (uintptr_t)canboot_isr_28, (uintptr_t)canboot_isr_29,
        (uintptr_t)canboot_isr_30, (uintptr_t)canboot_isr_31,
    };

    for (int i = 0; i < 32; i++) {
        set_gate(i, stubs[i]);
    }
    /* Vectors 32..255 are not installed; we never enable interrupts so
     * hardware IRQs can't fire. */

    struct idtr_desc d = {
        .limit = sizeof(idt) - 1,
        .base  = (uint64_t)(uintptr_t)idt,
    };
    __asm__ volatile ("lidt %0" : : "m"(d));
}

static const char *vec_name(uint64_t v) {
    switch (v) {
        case 0:  return "#DE divide-by-zero";
        case 1:  return "#DB debug";
        case 2:  return "NMI";
        case 3:  return "#BP breakpoint";
        case 4:  return "#OF overflow";
        case 5:  return "#BR bound range";
        case 6:  return "#UD invalid opcode";
        case 7:  return "#NM device not available";
        case 8:  return "#DF double fault";
        case 10: return "#TS invalid TSS";
        case 11: return "#NP segment not present";
        case 12: return "#SS stack-segment fault";
        case 13: return "#GP general protection";
        case 14: return "#PF page fault";
        case 16: return "#MF x87 fp";
        case 17: return "#AC alignment check";
        case 18: return "#MC machine check";
        case 19: return "#XF simd fp";
        case 21: return "#CP control protection";
        default: return "?";
    }
}

void canboot_exception_handler(struct canboot_trap_frame *f) {
    uint64_t cr2 = 0;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

    printf("\n***** CPU EXCEPTION *****\n");
    printf("vector=%llu (%s) err=0x%llx\n",
           (unsigned long long)f->vector,
           vec_name(f->vector),
           (unsigned long long)f->error_code);
    printf("rip=0x%016llx cs=0x%llx rflags=0x%llx\n",
           (unsigned long long)f->rip,
           (unsigned long long)f->cs,
           (unsigned long long)f->rflags);
    printf("rsp=0x%016llx ss=0x%llx cr2=0x%016llx\n",
           (unsigned long long)f->rsp,
           (unsigned long long)f->ss,
           (unsigned long long)cr2);
    printf("rax=0x%016llx rbx=0x%016llx rcx=0x%016llx rdx=0x%016llx\n",
           (unsigned long long)f->rax, (unsigned long long)f->rbx,
           (unsigned long long)f->rcx, (unsigned long long)f->rdx);
    printf("rsi=0x%016llx rdi=0x%016llx rbp=0x%016llx\n",
           (unsigned long long)f->rsi, (unsigned long long)f->rdi,
           (unsigned long long)f->rbp);
    printf("r8 =0x%016llx r9 =0x%016llx r10=0x%016llx r11=0x%016llx\n",
           (unsigned long long)f->r8, (unsigned long long)f->r9,
           (unsigned long long)f->r10, (unsigned long long)f->r11);
    printf("r12=0x%016llx r13=0x%016llx r14=0x%016llx r15=0x%016llx\n",
           (unsigned long long)f->r12, (unsigned long long)f->r13,
           (unsigned long long)f->r14, (unsigned long long)f->r15);
    printf("***** halting *****\n");
    for (;;) { __asm__ volatile ("cli; hlt"); }
}
