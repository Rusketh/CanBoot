#ifndef CANBOOT_SYNC_CPU_H
#define CANBOOT_SYNC_CPU_H

/*
 * Per-CPU primitives the scheduler and locks build on: local interrupt
 * masking, a relax hint for spin loops, and the logical CPU index.
 *
 * Milestone status (M1): single logical CPU. canboot_cpu_id() always
 * returns 0. SMP (M3) replaces the body with a LAPIC-id / MPIDR_EL1
 * lookup against a registration map populated during AP bring-up.
 */

#include <stdint.h>

typedef unsigned long canboot_irqflags_t;

/* Save the current interrupt-enable state and disable local interrupts.
 * Returns an opaque token to hand back to canboot_irq_restore(). */
static inline canboot_irqflags_t canboot_irq_save(void) {
    canboot_irqflags_t flags;
#if defined(__x86_64__)
    __asm__ volatile ("pushfq\n\t"
                      "popq %0\n\t"
                      "cli"
                      : "=r"(flags) : : "memory");
#elif defined(__aarch64__)
    /* DAIF bit 1 (I) masks IRQ. Save the whole DAIF then set I. */
    __asm__ volatile ("mrs %0, daif\n\t"
                      "msr daifset, #2"
                      : "=r"(flags) : : "memory");
#else
    flags = 0;
#endif
    return flags;
}

/* Unconditionally enable local interrupts. */
static inline void canboot_irq_enable(void) {
#if defined(__x86_64__)
    __asm__ volatile ("sti" : : : "memory");
#elif defined(__aarch64__)
    __asm__ volatile ("msr daifclr, #2" : : : "memory");
#endif
}

/* Unconditionally disable local interrupts. */
static inline void canboot_irq_disable(void) {
#if defined(__x86_64__)
    __asm__ volatile ("cli" : : : "memory");
#elif defined(__aarch64__)
    __asm__ volatile ("msr daifset, #2" : : : "memory");
#endif
}

/* Restore a previously saved interrupt-enable state. */
static inline void canboot_irq_restore(canboot_irqflags_t flags) {
#if defined(__x86_64__)
    __asm__ volatile ("pushq %0\n\t"
                      "popfq"
                      : : "r"(flags) : "memory", "cc");
#elif defined(__aarch64__)
    __asm__ volatile ("msr daif, %0" : : "r"(flags) : "memory");
#else
    (void)flags;
#endif
}

/* Spin-loop relaxation hint (PAUSE / YIELD). */
static inline void canboot_cpu_relax(void) {
#if defined(__x86_64__)
    __asm__ volatile ("pause" : : : "memory");
#elif defined(__aarch64__)
    __asm__ volatile ("yield" : : : "memory");
#endif
}

/* Logical CPU index in [0, CANBOOT_NR_CPUS). Backed by an arch hook so
 * the SMP layer (x86 APIC-ID map) can override it; the weak default
 * returns 0, which is correct for single-CPU and pre-SMP boot. */
unsigned canboot_arch_cpu_id(void);
static inline unsigned canboot_cpu_id(void) {
    return canboot_arch_cpu_id();
}

#endif /* CANBOOT_SYNC_CPU_H */
