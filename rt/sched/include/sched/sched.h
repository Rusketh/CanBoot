#ifndef CANBOOT_SCHED_SCHED_H
#define CANBOOT_SCHED_SCHED_H

/*
 * CanBoot thread scheduler core.
 *
 * Milestone roadmap (this file is M1):
 *   M1  arch-neutral scheduler: TCBs, run queue, blocking sync, full
 *       register context switch. Voluntary switch points only (yield /
 *       block) — no timer yet, so cooperatively scheduled in practice.
 *   M2  x86_64 LAPIC-timer preemption: the timer IRQ calls
 *       canboot_sched_preempt() from interrupt context.
 *   M3  SMP: per-CPU run queues, work stealing, reschedule IPIs; the
 *       single g_sched lock below is sharded.
 *   M4  aarch64 GIC + generic-timer parity (done: arch/aarch64/vectors.S
 *       + irq.c install VBAR_EL1, bring up GICv2 and the CNTV virtual
 *       timer, and route ticks to canboot_sched_on_tick like the LAPIC).
 *   M5  runtime hardening: serialise the picolibc allocator so forced
 *       timer preemption can be turned on (done for x86_64).
 *
 * The POSIX pthread surface (rt/pthread_stub/include/pthread.h) is a
 * thin shim over this API.
 */

#include <stddef.h>
#include <stdint.h>

#include "sync/spinlock.h"
#include "sched/waitq.h"

/* Saved machine context for a parked thread: just the stack pointer.
 * canboot_arch_ctx_switch pushes the callee-saved registers onto the
 * outgoing thread's stack and records %rsp / sp here; the incoming
 * thread's saved sp points at its own such frame. */
struct canboot_arch_ctx {
    void *sp;
};

/* Implemented in arch asm (rt/sched/arch/ctx_<arch>.S). Saves the
 * caller's callee-saved state to *prev and resumes *next. Returns (into
 * the *prev* timeline) only when prev is later switched back in. */
void canboot_arch_ctx_switch(struct canboot_arch_ctx *prev,
                             struct canboot_arch_ctx *next);

enum canboot_thread_state {
    CANBOOT_TH_FREE = 0, /* slot unused                     */
    CANBOOT_TH_NEW,      /* created, not yet first-run      */
    CANBOOT_TH_RUNNABLE, /* on a run queue                  */
    CANBOOT_TH_RUNNING,  /* currently on a CPU              */
    CANBOOT_TH_BLOCKED,  /* parked on a wait queue          */
    CANBOOT_TH_EXITED,   /* finished; awaiting join/reap    */
};

struct canboot_cpu;

struct canboot_thread {
    struct canboot_arch_ctx     ctx;
    enum canboot_thread_state   state;

    void                      *(*entry)(void *);
    void                       *arg;
    void                       *retval;

    int                         id;        /* index into the pool       */
    int                         detached;  /* reap on exit, no join      */

    struct canboot_thread      *q_next;    /* link for the one queue it  */
                                           /* currently sits on          */
    struct canboot_wait_queue   join_q;    /* threads blocked in join()  */
    struct canboot_cpu         *cpu;       /* CPU it last ran on          */

    unsigned char              *stack;     /* owned stack base, or NULL  */
    size_t                      stack_size;

    /* Per-thread TLS pointer (x86_64 FS_BASE). Each thread gets a private
     * zeroed block so cando's _Thread_local current-VM pointer is not
     * shared across concurrently-running threads. NULL on arches where
     * _Thread_local is compiled to a plain global (aarch64). */
    void                       *tls_base;
};

/* ---- Lifecycle ------------------------------------------------------- */

/* Adopt the current execution context as thread 0 and bring up CPU 0's
 * run queue + idle thread. Call once, early, on the boot CPU. */
void canboot_sched_init(void);

/* Entry point for a secondary CPU (M3). The arch SMP layer calls this on
 * each AP after long-mode + per-CPU LAPIC bring-up; the AP joins the
 * scheduler as an idle CPU and never returns. */
__attribute__((noreturn)) void canboot_sched_ap_online(unsigned cpu_index);

/* Backs canboot_cpu_id(); weak default returns 0, overridden by the SMP
 * layer once APs are running. Declared here for callers that need it. */
unsigned canboot_arch_cpu_id(void);

struct canboot_thread *canboot_thread_create(void *(*fn)(void *), void *arg);
struct canboot_thread *canboot_thread_current(void);
struct canboot_thread *canboot_thread_by_id(int id);

__attribute__((noreturn)) void canboot_thread_exit(void *retval);
int  canboot_thread_join(struct canboot_thread *t, void **retval);
void canboot_thread_detach(struct canboot_thread *t);

/* Voluntarily yield the CPU to another runnable thread. */
void canboot_sched_yield(void);

/* ---- Preemption (M2) ------------------------------------------------- */
/*
 * canboot_sched_on_tick() is called from the timer IRQ handler (IRQs
 * already masked by the gate). It advances the tick counter and, when
 * preemption is enabled and not deferred, forces a round-robin switch
 * via canboot_sched_preempt(). The preempt is performed by parking the
 * interrupted thread mid-ISR using the ordinary context switch; it
 * resumes (and the ISR iret's back to the interrupted instruction) once
 * it is rescheduled.
 *
 * Preemption is gated by canboot_sched_set_preemption(). As of M5 the
 * x86_64 kmain turns it ON: the one piece of non-reentrant runtime that
 * is actually shared across threads — the picolibc allocator — is now
 * serialised (rt/picolibc_port/syscalls.c, __wrap_malloc/free/calloc/
 * realloc behind a recursive preempt + SMP guard). lwIP, the HAL and the
 * Mbed TLS BIO are driven cooperatively from a single flow, so the heap
 * is their only concurrently-touched resource. aarch64 leaves preemption
 * off until M4 wires the generic timer.
 *
 * canboot_sched_arm_irqs() tells the thread trampoline that the arch IRQ
 * path is configured, so newly created threads should start with
 * interrupts enabled (and therefore be preemptible).
 */
void          canboot_sched_on_tick(void);
void          canboot_sched_preempt(void);
void          canboot_sched_set_preemption(int enabled);
void          canboot_sched_arm_irqs(void);
unsigned long canboot_sched_ticks(void);

/* Defer / allow preemption on the current CPU. Nest freely. */
void canboot_preempt_disable(void);
void canboot_preempt_enable(void);

/* ---- Locking + blocking primitives (for mutex / cond shims) ---------- */
/*
 * Usage contract:
 *   canboot_sched_lock(&flags);
 *     ... inspect / mutate shared state ...
 *     canboot_sched_block_on(&wq);   // optional, may be called repeatedly
 *     ... (lock is still held on return from block_on) ...
 *   canboot_sched_unlock(flags);
 *
 * block_on parks the current thread on wq and switches away; it returns
 * (with the scheduler lock still held) only once the thread is woken and
 * rescheduled. wake_one / wake_all must also be called under the lock.
 */
void canboot_sched_lock(canboot_irqflags_t *saved_flags);
void canboot_sched_unlock(canboot_irqflags_t saved_flags);
void canboot_sched_block_on(struct canboot_wait_queue *wq);
void canboot_sched_wake_one(struct canboot_wait_queue *wq);
void canboot_sched_wake_all(struct canboot_wait_queue *wq);

/* ---- VM "GIL" -------------------------------------------------------- */
/*
 * A single global lock that serialises CanDo VM execution. CanDo child
 * VMs (one per `thread {}` worker or per accepted server connection)
 * share the parent's heap, handle table and GC, none of which is
 * thread-safe, so only one thread may run VM bytecode at a time.
 *
 * The lock is held by whichever thread is executing VM code (taken when
 * a cando thread starts, held by the boot thread from sched_init) and
 * dropped at blocking points — thread join, condition waits, and socket
 * I/O pumps — so other VM threads (and I/O) make progress while one
 * blocks. acquire() is idempotent for the current owner. Held across the
 * blocking-primitive's own park; the woken thread reacquires before it
 * resumes VM work.
 */
void canboot_gil_acquire(void);
void canboot_gil_release(void);
int  canboot_gil_owned_by_current(void);

#endif /* CANBOOT_SCHED_SCHED_H */
