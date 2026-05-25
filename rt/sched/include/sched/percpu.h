#ifndef CANBOOT_SCHED_PERCPU_H
#define CANBOOT_SCHED_PERCPU_H

/*
 * Per-CPU scheduler state. M1 only ever touches index 0; the array is
 * sized for SMP so M3 can populate secondary CPUs without a layout
 * change. Each CPU owns a run queue; the single g_sched lock in sched.c
 * still serialises access in M1 (per-CPU rq locks arrive with M3).
 */

#include "sync/spinlock.h"
#include "sched/sched.h"

#ifndef CANBOOT_NR_CPUS
#define CANBOOT_NR_CPUS 8
#endif

struct canboot_cpu {
    struct canboot_thread *current;     /* thread running on this CPU   */
    struct canboot_thread *idle;        /* per-CPU idle thread          */
    struct canboot_thread *rq_head;     /* run queue (FIFO) head        */
    struct canboot_thread *rq_tail;     /* run queue tail               */
    int                    id;
    int                    online;
};

extern struct canboot_cpu canboot_cpus[CANBOOT_NR_CPUS];

static inline struct canboot_cpu *this_cpu(void) {
    return &canboot_cpus[canboot_cpu_id()];
}

#endif /* CANBOOT_SCHED_PERCPU_H */
