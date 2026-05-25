#ifndef CANBOOT_PTHREAD_H
#define CANBOOT_PTHREAD_H

#include <stddef.h>
#include <stdint.h>

#include "sched/waitq.h"

/*
 * POSIX pthread surface for CanBoot, layered over the CanBoot scheduler
 * (rt/sched/). CanDo's thread runtime (vendor/cando source/core/
 * thread_platform.c, source/lib/thread.c) calls these unchanged.
 *
 * These are real threads with a full register context switch — not the
 * old setjmp/longjmp fibers. As of M5 they are also genuinely preemptive
 * on x86_64: the LAPIC timer (M2) forces round-robin switches and the
 * allocator is serialised so that is safe. They still switch at yield /
 * blocking points too.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef int pthread_t;

typedef struct { int unused; } pthread_attr_t;

typedef struct {
    int                        locked;
    int                        owner;    /* tid of holder, or -1 */
    struct canboot_wait_queue  waiters;
} pthread_mutex_t;

typedef struct { int unused; } pthread_mutexattr_t;

typedef struct {
    struct canboot_wait_queue  waiters;
    int                        generation;
} pthread_cond_t;

typedef struct { int unused; } pthread_condattr_t;

/* 3-state so a racing caller blocks until the winner's init() returns,
 * rather than observing "done" while init() is still mid-flight (which a
 * 2-state flag allows the moment preemption is enabled). */
typedef struct {
    int                        state;   /* 0=fresh, 1=running, 2=done */
    struct canboot_wait_queue  waiters;
} pthread_once_t;

#define PTHREAD_MUTEX_INITIALIZER  { 0, -1, CANBOOT_WAIT_QUEUE_INITIALIZER }
#define PTHREAD_COND_INITIALIZER   { CANBOOT_WAIT_QUEUE_INITIALIZER, 0 }
#define PTHREAD_ONCE_INIT          { 0, CANBOOT_WAIT_QUEUE_INITIALIZER }

/* Lifecycle ------------------------------------------------------------- */

void canboot_pthread_init(void);

int       pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                         void *(*start_routine)(void *), void *arg);
int       pthread_join(pthread_t thread, void **retval);
int       pthread_detach(pthread_t thread);
__attribute__((noreturn)) void pthread_exit(void *retval);
pthread_t pthread_self(void);
int       pthread_yield(void);
int       pthread_equal(pthread_t a, pthread_t b);

/* Mutex ---------------------------------------------------------------- */

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);

/* Condition variable --------------------------------------------------- */

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);

/* Once ---------------------------------------------------------------- */

int pthread_once(pthread_once_t *once, void (*init)(void));

#ifdef __cplusplus
}
#endif

#endif /* CANBOOT_PTHREAD_H */
