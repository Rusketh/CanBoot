#ifndef CANBOOT_ARCH_X86_64_IDT_H
#define CANBOOT_ARCH_X86_64_IDT_H

#include <stdint.h>

/*
 * x86_64 IDT install + exception handler.
 *
 * Until we have a preemptive scheduler we never enable hardware
 * interrupts (sti); the IDT exists purely so any CPU exception
 * (#PF, #GP, #UD, etc.) gets a chance to print a trap frame to
 * serial instead of triple-faulting.
 */

void canboot_idt_install(void);

/* Load the shared IDT on the current CPU (the SMP APs call this). */
void canboot_idt_load(void);

/* Install an interrupt gate for `vec` pointing at an asm stub. Used by
 * the LAPIC timer bring-up (M2) to wire vectors 32+ after the base IDT
 * is loaded. Present, ring 0, 64-bit interrupt gate. */
void canboot_idt_set_gate(int vec, void (*handler)(void));

/* Trap frame pushed by our exception stubs. */
struct canboot_trap_frame {
    /* Pushed by stubs (saved general-purpose registers in
     * pushal-style order). */
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rsi, rdi, rdx, rcx, rbx, rax;
    /* Pushed by stub (vector + error code if any). */
    uint64_t vector;
    uint64_t error_code;
    /* Pushed by CPU on entry. */
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

void canboot_exception_handler(struct canboot_trap_frame *f);

#endif
