#ifndef CANBOOT_SCHED_PERCPU_H
#define CANBOOT_SCHED_PERCPU_H

/*
 * Per-CPU scheduler state. Each online CPU has a current + idle thread.
 * The run queue itself is global (shared, in sched.c) and protected by
 * the global scheduler lock: the simplest correct SMP design — every
 * CPU pulls runnable threads from the one queue. Per-CPU run queues with
 * sharded locks + work stealing are the M3b scalability follow-up.
 */

#include "sync/spinlock.h"
#include "sched/sched.h"

#ifndef CANBOOT_NR_CPUS
#define CANBOOT_NR_CPUS 8
#endif

struct canboot_cpu {
    struct canboot_thread *current;     /* thread running on this CPU   */
    struct canboot_thread *idle;        /* per-CPU idle thread          */
    unsigned               preempt_count; /* >0 => preemption deferred  */
    volatile int           need_resched;  /* tick wanted a switch        */
    int                    id;
    int                    online;
};

extern struct canboot_cpu canboot_cpus[CANBOOT_NR_CPUS];

static inline struct canboot_cpu *this_cpu(void) {
    return &canboot_cpus[canboot_cpu_id()];
}

#endif /* CANBOOT_SCHED_PERCPU_H */
