/*
 * Cooperative pthread implementation. setjmp/longjmp-based fibers with a
 * round-robin scheduler. Designed for single-CPU bare-metal: no atomics,
 * no preemption, no SMP. Threads must call pthread_yield() (or a
 * blocking primitive that yields internally) to give up the CPU.
 *
 * One slot reserved for the "main" thread (tid 0) which is whatever was
 * already running when canboot_pthread_init() was called.
 */

#include <errno.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#include "pthread.h"

#define CANBOOT_PTHREAD_MAX           8u
#define CANBOOT_PTHREAD_STACK_SIZE    (32u * 1024u)

enum pth_state {
    PTH_UNUSED = 0,
    PTH_NEW,
    PTH_RUNNING,
    PTH_RUNNABLE,
    PTH_WAITING_MUTEX,
    PTH_WAITING_COND,
    PTH_EXITED,
    PTH_JOINED,
};

struct pth {
    jmp_buf            ctx;
    void              *(*entry)(void *);
    void              *arg;
    void              *retval;
    enum pth_state     state;
    pthread_mutex_t   *waiting_mutex;
    pthread_cond_t    *waiting_cond;
    int                cond_generation;
    __attribute__((aligned(16))) unsigned char stack[CANBOOT_PTHREAD_STACK_SIZE];
};

static struct pth g_threads[CANBOOT_PTHREAD_MAX];
static int        g_current;

static void thread_trampoline(void);

void canboot_pthread_init(void) {
    memset(g_threads, 0, sizeof(g_threads));
    g_threads[0].state = PTH_RUNNING;
    g_current = 0;
}

pthread_t pthread_self(void) {
    return (pthread_t)g_current;
}

int pthread_equal(pthread_t a, pthread_t b) {
    return a == b;
}

int pthread_create(pthread_t *out,
                   const pthread_attr_t *attr,
                   void *(*start_routine)(void *),
                   void *arg) {
    (void)attr;
    for (unsigned i = 1; i < CANBOOT_PTHREAD_MAX; i++) {
        if (g_threads[i].state == PTH_UNUSED || g_threads[i].state == PTH_JOINED) {
            g_threads[i].entry = start_routine;
            g_threads[i].arg   = arg;
            g_threads[i].state = PTH_NEW;
            if (out) *out = (pthread_t)i;
            return 0;
        }
    }
    return EAGAIN;
}

/* Switch to a freshly-allocated stack and call thread_trampoline. The
 * trampoline calls the user entry function and then pthread_exit; we
 * never return through this asm block. */
static __attribute__((noreturn)) void first_run(int tid) {
    g_current = tid;
    g_threads[tid].state = PTH_RUNNING;

    uintptr_t sp = (uintptr_t)&g_threads[tid].stack[CANBOOT_PTHREAD_STACK_SIZE];
    sp &= ~(uintptr_t)0xF;   /* 16-byte align; call pushes 8 -> ABI-correct */

#if defined(__x86_64__)
    __asm__ volatile (
        "movq %0, %%rsp\n\t"
        "xorq %%rbp, %%rbp\n\t"
        "callq *%1\n\t"
        "ud2\n\t"
        : : "r"(sp), "r"((void *)&thread_trampoline)
        : "memory"
    );
#elif defined(__aarch64__)
    /* AAPCS64: x29 = fp, x30 = lr. Zero the frame pointer so the
     * unwinder stops at this synthetic root, then branch to the
     * trampoline with sp set as if it were just called. brk #0
     * forces a fault if the trampoline ever returns. */
    __asm__ volatile (
        "mov sp, %0\n\t"
        "mov x29, xzr\n\t"
        "blr %1\n\t"
        "brk #0\n\t"
        : : "r"(sp), "r"((void *)&thread_trampoline)
        : "memory", "x29", "x30"
    );
#endif
    __builtin_unreachable();
}

int pthread_yield(void) {
    int prev = g_current;

    enum pth_state next_state =
        (g_threads[prev].state == PTH_RUNNING) ? PTH_RUNNABLE
                                               : g_threads[prev].state;

    if (g_threads[prev].state != PTH_EXITED) {
        if (setjmp(g_threads[prev].ctx) != 0) {
            return 0; /* resumed */
        }
    }
    g_threads[prev].state = next_state;

    /* Round-robin pick of next runnable. */
    int n = prev;
    for (unsigned tries = 0; tries < CANBOOT_PTHREAD_MAX * 2u; tries++) {
        n = (n + 1) % (int)CANBOOT_PTHREAD_MAX;
        if (g_threads[n].state == PTH_NEW) {
            first_run(n);
            __builtin_unreachable();
        }
        if (g_threads[n].state == PTH_RUNNABLE) {
            g_current = n;
            g_threads[n].state = PTH_RUNNING;
            longjmp(g_threads[n].ctx, 1);
        }
    }

    /* No runnable thread anywhere. Resume self if possible so the caller
     * doesn't deadlock on a yield that has no target. */
    if (g_threads[prev].state == PTH_RUNNABLE) {
        g_threads[prev].state = PTH_RUNNING;
        g_current = prev;
    }
    return 0;
}

static void thread_trampoline(void) {
    int t = g_current;
    void *ret = g_threads[t].entry(g_threads[t].arg);
    pthread_exit(ret);
}

void pthread_exit(void *retval) {
    g_threads[g_current].retval = retval;
    g_threads[g_current].state  = PTH_EXITED;
    pthread_yield();
    for (;;) {
#if defined(__x86_64__)
        __asm__ volatile ("hlt");
#elif defined(__aarch64__)
        __asm__ volatile ("wfe");
#endif
    }
}

int pthread_join(pthread_t t, void **retval) {
    if (t < 0 || (unsigned)t >= CANBOOT_PTHREAD_MAX) return ESRCH;
    while (g_threads[t].state != PTH_EXITED) {
        pthread_yield();
    }
    if (retval) *retval = g_threads[t].retval;
    g_threads[t].state = PTH_JOINED;
    return 0;
}

/* Mutex --------------------------------------------------------------- */

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) {
    (void)a;
    if (!m) return EINVAL;
    m->locked = 0;
    m->owner  = -1;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m) { (void)m; return 0; }

int pthread_mutex_trylock(pthread_mutex_t *m) {
    if (!m) return EINVAL;
    if (m->locked) return EBUSY;
    m->locked = 1;
    m->owner  = g_current;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *m) {
    if (!m) return EINVAL;
    while (m->locked) {
        g_threads[g_current].state         = PTH_WAITING_MUTEX;
        g_threads[g_current].waiting_mutex = m;
        pthread_yield();
    }
    m->locked = 1;
    m->owner  = g_current;
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *m) {
    if (!m) return EINVAL;
    m->locked = 0;
    m->owner  = -1;
    /* Wake every thread waiting on this mutex; whichever runs first
     * grabs it. Cooperative scheduling means no real race. */
    for (unsigned i = 0; i < CANBOOT_PTHREAD_MAX; i++) {
        if (g_threads[i].state == PTH_WAITING_MUTEX &&
            g_threads[i].waiting_mutex == m) {
            g_threads[i].state         = PTH_RUNNABLE;
            g_threads[i].waiting_mutex = 0;
        }
    }
    return 0;
}

/* Condition variable -------------------------------------------------- */

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a) {
    (void)a;
    if (!c) return EINVAL;
    c->generation = 0;
    return 0;
}
int pthread_cond_destroy(pthread_cond_t *c) { (void)c; return 0; }

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    if (!c || !m) return EINVAL;
    int gen = c->generation;
    g_threads[g_current].waiting_cond    = c;
    g_threads[g_current].cond_generation = gen;
    g_threads[g_current].state           = PTH_WAITING_COND;
    pthread_mutex_unlock(m);
    pthread_yield();
    pthread_mutex_lock(m);
    return 0;
}

int pthread_cond_signal(pthread_cond_t *c) {
    if (!c) return EINVAL;
    c->generation++;
    for (unsigned i = 0; i < CANBOOT_PTHREAD_MAX; i++) {
        if (g_threads[i].state == PTH_WAITING_COND &&
            g_threads[i].waiting_cond == c) {
            g_threads[i].state        = PTH_RUNNABLE;
            g_threads[i].waiting_cond = 0;
            break;
        }
    }
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c) {
    if (!c) return EINVAL;
    c->generation++;
    for (unsigned i = 0; i < CANBOOT_PTHREAD_MAX; i++) {
        if (g_threads[i].state == PTH_WAITING_COND &&
            g_threads[i].waiting_cond == c) {
            g_threads[i].state        = PTH_RUNNABLE;
            g_threads[i].waiting_cond = 0;
        }
    }
    return 0;
}

/* Once ---------------------------------------------------------------- */

int pthread_once(pthread_once_t *once, void (*init)(void)) {
    if (!once || !init) return EINVAL;
    if (!once->done) {
        once->done = 1;
        init();
    }
    return 0;
}
