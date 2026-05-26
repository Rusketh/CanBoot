/*
 * CanBoot thread scheduler core (Milestone 1).
 *
 * A real preemptive-capable scheduler that replaces the old
 * setjmp/longjmp cooperative fiber stub. Threads are switched via a
 * full callee-saved register context switch (rt/sched/arch/ctx_*.S),
 * blocked threads park on intrusive wait queues, and all scheduler
 * state is serialised by a single global lock (g_sched).
 *
 * What M1 deliberately does NOT do yet:
 *   - No timer interrupt drives preemption, so in practice threads only
 *     switch at yield / block points. M2 wires the LAPIC timer to call
 *     into reschedule() from IRQ context.
 *   - g_sched is one big lock. M3 shards it per-CPU and adds reschedule
 *     IPIs + work stealing for true SMP parallelism.
 *   - The idle thread spins (cpu_relax) instead of halting, because with
 *     interrupts effectively masked there is nothing to wake a hlt/wfi.
 *     M2 switches it to halt-until-interrupt.
 *   - The thread pool is static (no heap dependency). The picolibc
 *     allocator used by threads is serialised in M5
 *     (rt/picolibc_port/syscalls.c) so forced preemption is safe to
 *     enable.
 *
 * UNVERIFIED: this has not been compiled or booted (the build
 * environment has no cross-toolchain, QEMU, or the cando submodule).
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "sync/spinlock.h"
#include "sched/sched.h"
#include "sched/percpu.h"

#define CANBOOT_THREAD_POOL   16u
#define CANBOOT_THREAD_STACK  (32u * 1024u)
#define CANBOOT_IDLE_STACK    (8u * 1024u)  /* idle does almost nothing */

struct canboot_cpu canboot_cpus[CANBOOT_NR_CPUS];

/* Global scheduler lock. Protects every run queue, every wait queue,
 * and all thread.state transitions. Held across context switches: the
 * thread that resumes is responsible for releasing it (see the
 * resume-side spin_unlock in reschedule() / the trampoline). */
static spinlock_t g_sched = SPINLOCK_INITIALIZER;

/* Preemption gates (see canboot_sched_on_tick docs in sched.h). */
static volatile int   g_preempt_enabled; /* timer may force a switch    */
static volatile int   g_irqs_armed;       /* arch IRQ path configured    */
static unsigned long  g_ticks;            /* monotonic timer ticks        */

/* Global run queue (FIFO) of runnable threads, shared by all CPUs and
 * protected by g_sched. Idle threads are never enqueued here. */
static struct {
    struct canboot_thread *head;
    struct canboot_thread *tail;
} g_runq;

/* Weak default: single logical CPU. The x86 SMP layer (arch/x86_64/
 * smp.c) provides a strong override that maps the LAPIC ID to a logical
 * index once APs are up. */
__attribute__((weak)) unsigned canboot_arch_cpu_id(void) {
    return 0u;
}

/* Thread pool. Slot 0 is the adopted boot/main thread (no owned stack).
 * Remaining slots back pthread_create. Per-CPU idle threads live in
 * their own array so the pool stays available for user threads. */
static struct canboot_thread g_threads[CANBOOT_THREAD_POOL];
static __attribute__((aligned(16)))
    unsigned char g_stacks[CANBOOT_THREAD_POOL][CANBOOT_THREAD_STACK];

static struct canboot_thread g_idle[CANBOOT_NR_CPUS];
static __attribute__((aligned(16)))
    unsigned char g_idle_stacks[CANBOOT_NR_CPUS][CANBOOT_IDLE_STACK];

/* ------------------------------------------------------------------ */
/* Per-thread TLS. cando keeps a handful of _Thread_local pointers (the  */
/* current VM / thread / sort ctx / memctrl); a server that runs a child */
/* VM per accepted connection needs each worker thread to see its own    */
/* copy. On x86_64 the thread pointer is FS_BASE and the toolchain emits  */
/* %fs:NEG-offset accesses, so a zeroed block with FS_BASE at its top is  */
/* a valid per-thread TLS area (PT_TLS is .tbss-only — FileSiz 0 — so no  */
/* init image to copy). On aarch64 _Thread_local is compiled to a plain   */
/* global (see cmake/uefi_aarch64.cmake), so tls_base is unused there.    */
#define CANBOOT_THREAD_TLS_SIZE 2048u
static __attribute__((aligned(64)))
    unsigned char g_thread_tls[CANBOOT_THREAD_POOL][CANBOOT_THREAD_TLS_SIZE];

#if defined(__x86_64__)
static inline void tls_set_base(void *p) {
    uint64_t v = (uint64_t)(uintptr_t)p;
    __asm__ volatile ("wrmsr" : :
        "c"(0xC0000100u),                 /* IA32_FS_BASE */
        "a"((uint32_t)(v & 0xFFFFFFFFu)),
        "d"((uint32_t)(v >> 32)));
}
static inline void *tls_get_base(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000100u));
    return (void *)(uintptr_t)(((uint64_t)hi << 32) | lo);
}
/* x86_64 variant II: thread pointer sits at the END of the block;
 * variables live at negative offsets below it. */
static inline void *tls_top(unsigned char *area, size_t size) {
    return area + size;
}
#else
static inline void tls_set_base(void *p) { (void)p; }
static inline void *tls_get_base(void) { return NULL; }
static inline void *tls_top(unsigned char *area, size_t size) {
    (void)size; return area;
}
#endif

/* ------------------------------------------------------------------ */
/* Arch context bootstrap: lay down a fake callee-saved frame so the    */
/* first switch into a new thread "returns" into the trampoline. The    */
/* layout MUST match the pop/ldp order in rt/sched/arch/ctx_<arch>.S.   */
/* ------------------------------------------------------------------ */

static void thread_trampoline(void);

static void arch_ctx_init(struct canboot_arch_ctx *ctx,
                          void *stack_top_in) {
    uintptr_t top = ((uintptr_t)stack_top_in) & ~(uintptr_t)0xF;

#if defined(__x86_64__)
    /* 8 slots: [0..5]=r15,r14,r13,r12,rbx,rbp  [6]=return addr  [7]=pad.
     * Picking sp = top-64 leaves rsp = top-8 on entry to the trampoline,
     * i.e. (rsp % 16) == 8 — the SysV state right after a `call`. */
    uint64_t *s = (uint64_t *)(top - 64u);
    for (int i = 0; i < 8; i++) s[i] = 0;
    s[6] = (uint64_t)(uintptr_t)&thread_trampoline; /* ret target */
    ctx->sp = (void *)s;
#elif defined(__aarch64__)
    /* 20 slots: [0..9]=x19..x28, [10]=x29(fp), [11]=x30(lr),
     * [12..19]=d8..d15. sp must stay 16-aligned; 160 bytes keeps it so. */
    uint64_t *s = (uint64_t *)(top - 160u);
    for (int i = 0; i < 20; i++) s[i] = 0;
    s[11] = (uint64_t)(uintptr_t)&thread_trampoline; /* x30 -> ret */
    ctx->sp = (void *)s;
#else
#error "unsupported architecture for context switch"
#endif
}

/* ------------------------------------------------------------------ */
/* Run-queue + wait-queue primitives. All callers hold g_sched.         */
/* ------------------------------------------------------------------ */

static void rq_push(struct canboot_thread *t) {
    t->q_next = NULL;
    if (g_runq.tail)
        g_runq.tail->q_next = t;
    else
        g_runq.head = t;
    g_runq.tail = t;
}

static struct canboot_thread *rq_pop(void) {
    struct canboot_thread *t = g_runq.head;
    if (t) {
        g_runq.head = t->q_next;
        if (!g_runq.head)
            g_runq.tail = NULL;
        t->q_next = NULL;
    }
    return t;
}

static void wq_push(struct canboot_wait_queue *wq, struct canboot_thread *t) {
    t->q_next = NULL;
    if (wq->tail)
        wq->tail->q_next = t;
    else
        wq->head = t;
    wq->tail = t;
}

static struct canboot_thread *wq_pop(struct canboot_wait_queue *wq) {
    struct canboot_thread *t = wq->head;
    if (t) {
        wq->head = t->q_next;
        if (!wq->head)
            wq->tail = NULL;
        t->q_next = NULL;
    }
    return t;
}

/* ------------------------------------------------------------------ */
/* Core switch. Caller holds g_sched and has saved/disabled IRQs. On     */
/* return (when 'prev' is later resumed) g_sched is STILL held — the     */
/* resumed context's own caller releases it.                            */
/* ------------------------------------------------------------------ */

static void reschedule(void) {
    struct canboot_cpu *cpu = this_cpu();
    struct canboot_thread *prev = cpu->current;
    struct canboot_thread *next = rq_pop();

    if (!next)
        next = cpu->idle;

    if (next == prev) {
        next->state = CANBOOT_TH_RUNNING;
        return;
    }

    next->state = CANBOOT_TH_RUNNING;
    next->cpu   = cpu;
    cpu->current = next;

    /* Point the CPU's thread pointer at the incoming thread's TLS block
     * before we resume it, so its _Thread_local accesses are private.
     * Every switch sets the in-thread's base, so prev's is restored when
     * something later switches back to it. */
    if (next->tls_base)
        tls_set_base(next->tls_base);

    canboot_arch_ctx_switch(&prev->ctx, &next->ctx);
    /* Resumed: 'prev' is current again. g_sched is held. */
}

static void thread_trampoline(void) {
    /* First instruction of every freshly created thread. The switch
     * that landed us here was performed with g_sched held; release it
     * before running user code (mirrors reschedule()'s resume side). */
    spin_unlock(&g_sched);
    /* Once the arch IRQ path is armed (LAPIC timer up on x86_64), new
     * threads start with interrupts enabled so the timer can preempt
     * them. Before that they run masked, exactly as in M1. */
    if (g_irqs_armed)
        canboot_irq_enable();

    struct canboot_thread *self = canboot_thread_current();
    void *ret = self->entry(self->arg);
    canboot_thread_exit(ret);
}

/* ------------------------------------------------------------------ */
/* Public lock / block API                                             */
/* ------------------------------------------------------------------ */

void canboot_sched_lock(canboot_irqflags_t *saved_flags) {
    canboot_irqflags_t f = canboot_irq_save();
    spin_lock(&g_sched);
    if (saved_flags)
        *saved_flags = f;
}

void canboot_sched_unlock(canboot_irqflags_t saved_flags) {
    spin_unlock(&g_sched);
    canboot_irq_restore(saved_flags);
}

void canboot_sched_block_on(struct canboot_wait_queue *wq) {
    /* g_sched held on entry and on return. */
    struct canboot_thread *self = this_cpu()->current;
    self->state = CANBOOT_TH_BLOCKED;
    wq_push(wq, self);
    reschedule();
}

void canboot_sched_wake_one(struct canboot_wait_queue *wq) {
    struct canboot_thread *t = wq_pop(wq);
    if (t) {
        t->state = CANBOOT_TH_RUNNABLE;
        rq_push(t);
    }
}

void canboot_sched_wake_all(struct canboot_wait_queue *wq) {
    struct canboot_thread *t;
    while ((t = wq_pop(wq)) != NULL) {
        t->state = CANBOOT_TH_RUNNABLE;
        rq_push(t);
    }
}

void canboot_sched_yield(void) {
    canboot_irqflags_t f;
    canboot_sched_lock(&f);
    struct canboot_thread *self = this_cpu()->current;
    if (self->state == CANBOOT_TH_RUNNING) {
        self->state = CANBOOT_TH_RUNNABLE;
        rq_push(self);
    }
    reschedule();
    canboot_sched_unlock(f);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */

struct canboot_thread *canboot_thread_current(void) {
    return this_cpu()->current;
}

struct canboot_thread *canboot_thread_by_id(int id) {
    if (id < 0 || (unsigned)id >= CANBOOT_THREAD_POOL)
        return NULL;
    return &g_threads[id];
}

static void *idle_entry(void *arg) {
    (void)arg;
    /* The idle thread is never placed on a run queue: reschedule() falls
     * back to cpu->idle only when nothing else is runnable. So we just
     * re-run the picker (without enqueuing ourselves) and spin. M2 makes
     * this halt-until-interrupt once a timer IRQ can wake a blocked
     * thread; until then a blocked-everywhere system would wedge on a
     * hlt, so we busy-relax instead. */
    for (;;) {
        canboot_irqflags_t f;
        canboot_sched_lock(&f);
        reschedule();
        canboot_sched_unlock(f);
        canboot_cpu_relax();
    }
    return NULL; /* unreachable; silences -Wreturn-type at -O0 */
}

void canboot_sched_init(void) {
    memset(canboot_cpus, 0, sizeof(canboot_cpus));
    memset(g_threads, 0, sizeof(g_threads));
    memset(g_idle, 0, sizeof(g_idle));
    spin_init(&g_sched);

    struct canboot_cpu *cpu = &canboot_cpus[0];
    cpu->id = 0;
    cpu->online = 1;

    /* Slot 0: adopt whatever stack/context is running right now. Its
     * ctx.sp is filled in by the first switch away from it. */
    struct canboot_thread *main_th = &g_threads[0];
    main_th->id    = 0;
    main_th->state = CANBOOT_TH_RUNNING;
    main_th->cpu   = cpu;
    main_th->stack = NULL; /* not owned */
    /* Adopt the boot CPU's already-configured TLS area (kmain/setup_tls
     * on x86_64; NULL elsewhere). New threads get private blocks. */
    main_th->tls_base = tls_get_base();
    cpu->current   = main_th;

    /* Per-CPU idle thread (CPU 0 only in M1). */
    struct canboot_thread *idle = &g_idle[0];
    idle->id         = -1;
    idle->entry      = idle_entry;
    idle->arg        = NULL;
    idle->state      = CANBOOT_TH_RUNNABLE;
    idle->cpu        = cpu;
    idle->stack      = g_idle_stacks[0];
    idle->stack_size = CANBOOT_IDLE_STACK;
    /* Idle never runs cando, but keep a valid TLS base so a switch to it
     * still points FS_BASE somewhere mapped. Share the boot area. */
    idle->tls_base   = main_th->tls_base;
    arch_ctx_init(&idle->ctx, g_idle_stacks[0] + CANBOOT_IDLE_STACK);
    cpu->idle = idle;
}

/* Called by each Application Processor once the arch SMP layer has
 * brought it into long mode with its per-CPU LAPIC enabled and the
 * APIC-ID -> logical-index map populated (so canboot_cpu_id() resolves
 * to `cpu_index` here). The AP adopts its startup stack as its idle
 * thread and joins the scheduler; this never returns. Real threads are
 * pulled from the shared global run queue, giving true parallelism. */
__attribute__((noreturn)) void canboot_sched_ap_online(unsigned cpu_index) {
    canboot_irqflags_t f;
    canboot_sched_lock(&f);

    struct canboot_cpu *cpu = &canboot_cpus[cpu_index];
    cpu->id     = (int)cpu_index;
    cpu->online = 1;

    struct canboot_thread *idle = &g_idle[cpu_index];
    idle->id    = -1 - (int)cpu_index; /* unique, never a valid pool id */
    idle->state = CANBOOT_TH_RUNNING;
    idle->cpu   = cpu;
    idle->stack = NULL;                /* adopt the AP's startup stack  */
    idle->tls_base = tls_get_base();   /* AP's shared boot TLS area     */
    cpu->idle    = idle;
    cpu->current = idle;

    canboot_sched_unlock(f);

    idle_entry(NULL); /* becomes this CPU's idle loop; never returns */
    __builtin_unreachable();
}

struct canboot_thread *canboot_thread_create(void *(*fn)(void *), void *arg) {
    canboot_irqflags_t f;
    canboot_sched_lock(&f);

    struct canboot_thread *t = NULL;
    for (unsigned i = 1; i < CANBOOT_THREAD_POOL; i++) {
        if (g_threads[i].state == CANBOOT_TH_FREE) {
            t = &g_threads[i];
            break;
        }
    }
    if (!t) {
        canboot_sched_unlock(f);
        return NULL; /* pool exhausted */
    }

    int id = (int)(t - g_threads);
    memset(t, 0, sizeof(*t));
    t->id         = id;
    t->entry      = fn;
    t->arg        = arg;
    t->state      = CANBOOT_TH_RUNNABLE;
    t->stack      = g_stacks[id];
    t->stack_size = CANBOOT_THREAD_STACK;
    /* Fresh, zeroed private TLS block (re-zeroed on pool-slot reuse so a
     * prior thread's _Thread_local values don't leak in). */
    memset(g_thread_tls[id], 0, CANBOOT_THREAD_TLS_SIZE);
    t->tls_base   = tls_top(g_thread_tls[id], CANBOOT_THREAD_TLS_SIZE);
    arch_ctx_init(&t->ctx, g_stacks[id] + CANBOOT_THREAD_STACK);

    rq_push(t);

    canboot_sched_unlock(f);
    return t;
}

void canboot_thread_detach(struct canboot_thread *t) {
    canboot_irqflags_t f;
    canboot_sched_lock(&f);
    if (t->state == CANBOOT_TH_EXITED) {
        /* Already finished and waiting to be reaped: free it now. */
        t->state = CANBOOT_TH_FREE;
    } else {
        t->detached = 1;
    }
    canboot_sched_unlock(f);
}

__attribute__((noreturn)) void canboot_thread_exit(void *retval) {
    canboot_irqflags_t f;
    canboot_sched_lock(&f);

    struct canboot_thread *self = this_cpu()->current;
    self->retval = retval;
    canboot_sched_wake_all(&self->join_q);

    if (self->detached && self->id >= 0) {
        self->state = CANBOOT_TH_FREE; /* nobody will join; reap now */
    } else {
        self->state = CANBOOT_TH_EXITED;
    }

    /* Switch away forever. We never re-enter this thread, so the thread
     * we switch into is the one that releases g_sched (resume side). */
    reschedule();

    /* Unreachable: an EXITED/FREE thread is never put back on a run
     * queue. Guard anyway so the noreturn contract holds. */
    for (;;)
        canboot_cpu_relax();
}

int canboot_thread_join(struct canboot_thread *t, void **retval) {
    if (!t)
        return -1;

    canboot_irqflags_t f;
    canboot_sched_lock(&f);

    while (t->state != CANBOOT_TH_EXITED && t->state != CANBOOT_TH_FREE)
        canboot_sched_block_on(&t->join_q);

    if (retval)
        *retval = t->retval;

    if (t->id >= 0)
        t->state = CANBOOT_TH_FREE; /* reap the slot */

    canboot_sched_unlock(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Preemption (M2)                                                      */
/* ------------------------------------------------------------------ */

void canboot_sched_arm_irqs(void) {
    g_irqs_armed = 1;
}

void canboot_sched_set_preemption(int enabled) {
    g_preempt_enabled = enabled ? 1 : 0;
}

unsigned long canboot_sched_ticks(void) {
    return __atomic_load_n(&g_ticks, __ATOMIC_RELAXED);
}

void canboot_preempt_disable(void) {
    canboot_irqflags_t f = canboot_irq_save();
    this_cpu()->preempt_count++;
    canboot_irq_restore(f);
}

void canboot_preempt_enable(void) {
    canboot_irqflags_t f = canboot_irq_save();
    struct canboot_cpu *c = this_cpu();
    int resched = 0;
    if (c->preempt_count)
        c->preempt_count--;
    if (c->preempt_count == 0 && c->need_resched)
        resched = 1;
    canboot_irq_restore(f);
    if (resched)
        canboot_sched_yield();
}

/* Force a round-robin switch away from the current thread if another is
 * runnable. Called from the timer IRQ (canboot_sched_on_tick); the
 * context switch parks the interrupted thread mid-ISR. */
void canboot_sched_preempt(void) {
    canboot_irqflags_t f;
    canboot_sched_lock(&f);
    struct canboot_cpu *c = this_cpu();
    struct canboot_thread *self = c->current;
    c->need_resched = 0;
    if (g_runq.head != NULL) {
        /* The idle thread is never placed on a run queue (reschedule()
         * falls back to it). So when the timer preempts idle, switch to
         * the runnable thread but do NOT enqueue idle. A real thread is
         * round-robined back onto the queue as usual. */
        if (self->state == CANBOOT_TH_RUNNING && self != c->idle) {
            self->state = CANBOOT_TH_RUNNABLE;
            rq_push(self);
        }
        reschedule();
    }
    canboot_sched_unlock(f);
}

/* Timer-tick entry point. Runs in IRQ context with interrupts masked. */
void canboot_sched_on_tick(void) {
    __atomic_add_fetch(&g_ticks, 1, __ATOMIC_RELAXED);

    if (!g_preempt_enabled)
        return;

    struct canboot_cpu *c = this_cpu();
    if (c->preempt_count) {
        c->need_resched = 1; /* honoured by canboot_preempt_enable() */
        return;
    }
    canboot_sched_preempt();
}
