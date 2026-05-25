/*
 * POSIX pthread shim over the CanBoot scheduler (rt/sched/).
 *
 * This replaces the former setjmp/longjmp cooperative-fiber stub: the
 * threads are now real, with a full register context switch and a
 * blocking run-queue scheduler. The POSIX surface is unchanged so
 * CanDo's thread runtime keeps compiling and linking against it.
 *
 * Mutex / cond are implemented directly against the scheduler's global
 * lock + wait queues rather than a userspace fast path: it is simpler
 * to reason about and, on a single CPU (M1), the uncontended cost is a
 * cli/sti pair. M3's fine-grained scheduler keeps the same surface.
 *
 * UNVERIFIED: not yet compiled or booted in this environment.
 */

#include <errno.h>
#include <stddef.h>

#include "pthread.h"
#include "sched/sched.h"

void canboot_pthread_init(void) {
    canboot_sched_init();
}

/* ---- Lifecycle ------------------------------------------------------- */

pthread_t pthread_self(void) {
    struct canboot_thread *t = canboot_thread_current();
    return t ? (pthread_t)t->id : 0;
}

int pthread_equal(pthread_t a, pthread_t b) {
    return a == b;
}

int pthread_create(pthread_t *out,
                   const pthread_attr_t *attr,
                   void *(*start_routine)(void *),
                   void *arg) {
    (void)attr;
    struct canboot_thread *t = canboot_thread_create(start_routine, arg);
    if (!t)
        return EAGAIN;
    if (out)
        *out = (pthread_t)t->id;
    return 0;
}

int pthread_yield(void) {
    canboot_sched_yield();
    return 0;
}

__attribute__((noreturn)) void pthread_exit(void *retval) {
    canboot_thread_exit(retval);
}

int pthread_join(pthread_t tid, void **retval) {
    struct canboot_thread *t = canboot_thread_by_id((int)tid);
    if (!t)
        return ESRCH;
    return canboot_thread_join(t, retval) == 0 ? 0 : ESRCH;
}

int pthread_detach(pthread_t tid) {
    struct canboot_thread *t = canboot_thread_by_id((int)tid);
    if (!t)
        return ESRCH;
    canboot_thread_detach(t);
    return 0;
}

/* ---- Mutex ----------------------------------------------------------- */

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) {
    (void)a;
    if (!m) return EINVAL;
    m->locked       = 0;
    m->owner        = -1;
    m->waiters.head = NULL;
    m->waiters.tail = NULL;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m) { (void)m; return 0; }

int pthread_mutex_trylock(pthread_mutex_t *m) {
    if (!m) return EINVAL;
    canboot_irqflags_t f;
    canboot_sched_lock(&f);
    int rc;
    if (m->locked) {
        rc = EBUSY;
    } else {
        m->locked = 1;
        m->owner  = pthread_self();
        rc = 0;
    }
    canboot_sched_unlock(f);
    return rc;
}

int pthread_mutex_lock(pthread_mutex_t *m) {
    if (!m) return EINVAL;
    canboot_irqflags_t f;
    canboot_sched_lock(&f);
    while (m->locked)
        canboot_sched_block_on(&m->waiters);
    m->locked = 1;
    m->owner  = pthread_self();
    canboot_sched_unlock(f);
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *m) {
    if (!m) return EINVAL;
    canboot_irqflags_t f;
    canboot_sched_lock(&f);
    m->locked = 0;
    m->owner  = -1;
    canboot_sched_wake_one(&m->waiters);
    canboot_sched_unlock(f);
    return 0;
}

/* ---- Condition variable ---------------------------------------------- */

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a) {
    (void)a;
    if (!c) return EINVAL;
    c->waiters.head = NULL;
    c->waiters.tail = NULL;
    c->generation   = 0;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *c) { (void)c; return 0; }

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    if (!c || !m) return EINVAL;
    canboot_irqflags_t f;
    canboot_sched_lock(&f);

    /* Release the mutex while holding the scheduler lock so the unlock
     * and the enqueue-on-cond are atomic w.r.t. a concurrent signal. */
    m->locked = 0;
    m->owner  = -1;
    canboot_sched_wake_one(&m->waiters);

    canboot_sched_block_on(&c->waiters);

    /* Reacquire the mutex before returning to the caller. */
    while (m->locked)
        canboot_sched_block_on(&m->waiters);
    m->locked = 1;
    m->owner  = pthread_self();

    canboot_sched_unlock(f);
    return 0;
}

int pthread_cond_signal(pthread_cond_t *c) {
    if (!c) return EINVAL;
    canboot_irqflags_t f;
    canboot_sched_lock(&f);
    c->generation++;
    canboot_sched_wake_one(&c->waiters);
    canboot_sched_unlock(f);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c) {
    if (!c) return EINVAL;
    canboot_irqflags_t f;
    canboot_sched_lock(&f);
    c->generation++;
    canboot_sched_wake_all(&c->waiters);
    canboot_sched_unlock(f);
    return 0;
}

/* ---- Once ------------------------------------------------------------ */

int pthread_once(pthread_once_t *once, void (*init)(void)) {
    if (!once || !init) return EINVAL;
    canboot_irqflags_t f;
    canboot_sched_lock(&f);
    int run = 0;
    if (!once->done) {
        once->done = 1;
        run = 1;
    }
    canboot_sched_unlock(f);
    /* M1 is non-preemptive, so a racing caller cannot observe done==1
     * before init() finishes. M2 promotes this to a 3-state wait. */
    if (run)
        init();
    return 0;
}
