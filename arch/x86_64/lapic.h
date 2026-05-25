#ifndef CANBOOT_ARCH_X86_64_LAPIC_H
#define CANBOOT_ARCH_X86_64_LAPIC_H

#include <stdint.h>

/*
 * x86_64 Local APIC (xAPIC, MMIO) timer support for preemptive
 * scheduling (M2). The LAPIC MMIO window (0xFEE00000) sits inside the
 * first 4 GiB that arch/x86_64/bootstrap.S identity-maps, so it is
 * reachable without extra page-table work.
 *
 * SMP / x2APIC / multi-CPU IPIs are M3 — this is single-CPU xAPIC only.
 */

/* End-of-interrupt: acknowledge the in-service interrupt. Call from
 * every IRQ handler before returning (never for the spurious vector). */
void canboot_lapic_eoi(void);

/* This CPU's LAPIC ID (physical APIC ID, bits 24-31 of the ID reg). */
uint32_t canboot_lapic_id(void);

/* Per-CPU LAPIC bring-up, callable from the BSP and every AP:
 *   _enable_this_cpu  software-enables the LAPIC (APIC base MSR, TPR,
 *                     spurious vector). Does NOT touch the 8259 PIC.
 *   _timer_start_this_cpu  programs this CPU's periodic timer using the
 *                     count calibrated once by canboot_lapic_timer_setup. */
void canboot_lapic_enable_this_cpu(void);
void canboot_lapic_timer_start_this_cpu(void);

/* AP bring-up IPIs (xAPIC ICR). dest = target physical APIC ID. */
void canboot_lapic_send_init(uint32_t dest);
void canboot_lapic_send_sipi(uint32_t dest, uint8_t vector);
/* Fixed inter-processor interrupt (e.g. reschedule). */
void canboot_lapic_send_ipi(uint32_t dest, uint8_t vector);

/*
 * One-shot bring-up of the LAPIC periodic timer:
 *   - masks the legacy 8259 PIC so only LAPIC IRQs reach the CPU,
 *   - enables the LAPIC (spurious vector 0xFF),
 *   - installs the timer + spurious IDT gates,
 *   - calibrates the timer against canboot_tsc_hz() and programs it
 *     periodic at `hz` (e.g. 100 => a 10 ms tick).
 *
 * Does NOT execute `sti`; the caller enables interrupts once the
 * scheduler is ready. Requires canboot_idt_install() to have run.
 */
void canboot_lapic_timer_setup(unsigned hz);

#endif /* CANBOOT_ARCH_X86_64_LAPIC_H */
