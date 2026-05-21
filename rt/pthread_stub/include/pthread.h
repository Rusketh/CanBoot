#ifndef CANBOOT_PTHREAD_H
#define CANBOOT_PTHREAD_H

#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>

/*
 * Cooperative pthread stub. Single-CPU, no preemption: threads run
 * until they call pthread_yield(), block on a mutex/cond, or exit. The
 * surface mirrors POSIX so CanDo's existing pthread call sites compile
 * unchanged once we vendor it.
 *
 * Promoted to a preemptive scheduler in a later milestone once the LAPIC
 * timer is wired and we have an IDT.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef int pthread_t;

typedef struct { int unused; } pthread_attr_t;

typedef struct {
    int      locked;
    int      owner;   /* tid of holder, or -1 */
} pthread_mutex_t;

typedef struct { int unused; } pthread_mutexattr_t;

typedef struct {
    int      generation;   /* incremented by every signal/broadcast */
} pthread_cond_t;

typedef struct { int unused; } pthread_condattr_t;

typedef struct {
    int      done;
} pthread_once_t;

#define PTHREAD_MUTEX_INITIALIZER  { 0, -1 }
#define PTHREAD_COND_INITIALIZER   { 0 }
#define PTHREAD_ONCE_INIT          { 0 }

/* Lifecycle ------------------------------------------------------------- */

void canboot_pthread_init(void);

int       pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                         void *(*start_routine)(void *), void *arg);
int       pthread_join(pthread_t thread, void **retval);
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
