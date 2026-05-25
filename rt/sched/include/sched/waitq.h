#ifndef CANBOOT_SCHED_WAITQ_H
#define CANBOOT_SCHED_WAITQ_H

/*
 * Intrusive wait queue: a FIFO list of blocked threads, linked through
 * each thread's q_next field. Embedded directly inside mutexes,
 * condition variables, and thread join slots.
 *
 * The queue carries no lock of its own — every queue operation runs
 * under the global scheduler lock (see sched.c). M3 shards that lock
 * per run-queue / per wait-object; the head/tail layout here does not
 * change when that happens.
 */

struct canboot_thread; /* forward; defined in sched/sched.h */

struct canboot_wait_queue {
    struct canboot_thread *head;
    struct canboot_thread *tail;
};

#define CANBOOT_WAIT_QUEUE_INITIALIZER { 0, 0 }

#endif /* CANBOOT_SCHED_WAITQ_H */
