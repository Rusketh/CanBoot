#ifndef CANBOOT_SYNC_SPINLOCK_H
#define CANBOOT_SYNC_SPINLOCK_H

/*
 * Ticket spinlock. FIFO-fair, SMP-safe via the GCC/Clang __atomic
 * builtins (no libatomic dependency). Use the _irqsave variants when a
 * lock may be taken both from thread context and from an interrupt
 * handler on the same CPU — otherwise an IRQ that lands mid-critical
 * section and tries to take the same lock self-deadlocks.
 */

#include "sync/cpu.h"

typedef struct {
    unsigned int next;   /* next ticket to hand out  */
    unsigned int owner;  /* ticket currently served  */
} spinlock_t;

#define SPINLOCK_INITIALIZER { 0u, 0u }

static inline void spin_init(spinlock_t *l) {
    __atomic_store_n(&l->next,  0u, __ATOMIC_RELAXED);
    __atomic_store_n(&l->owner, 0u, __ATOMIC_RELAXED);
}

static inline void spin_lock(spinlock_t *l) {
    unsigned int ticket = __atomic_fetch_add(&l->next, 1u, __ATOMIC_RELAXED);
    while (__atomic_load_n(&l->owner, __ATOMIC_ACQUIRE) != ticket)
        canboot_cpu_relax();
}

static inline int spin_trylock(spinlock_t *l) {
    unsigned int owner = __atomic_load_n(&l->owner, __ATOMIC_RELAXED);
    unsigned int next  = __atomic_load_n(&l->next,  __ATOMIC_RELAXED);
    if (owner != next)
        return 0; /* contended */
    unsigned int expect = next;
    if (__atomic_compare_exchange_n(&l->next, &expect, next + 1u, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return 1;
    return 0;
}

static inline void spin_unlock(spinlock_t *l) {
    __atomic_add_fetch(&l->owner, 1u, __ATOMIC_RELEASE);
}

static inline canboot_irqflags_t spin_lock_irqsave(spinlock_t *l) {
    canboot_irqflags_t flags = canboot_irq_save();
    spin_lock(l);
    return flags;
}

static inline void spin_unlock_irqrestore(spinlock_t *l,
                                          canboot_irqflags_t flags) {
    spin_unlock(l);
    canboot_irq_restore(flags);
}

#endif /* CANBOOT_SYNC_SPINLOCK_H */
