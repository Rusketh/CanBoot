/*
 * Milestone 5 self-test: prove picolibc (printf, malloc/free, string
 * functions) and the thread scheduler (create, join, mutex-protected
 * counter, yield) work end-to-end. Doubles as the smoke test for the
 * rt/sched preemptive-capable scheduler that replaced the old
 * setjmp/longjmp fiber stub.
 *
 * Run from kmain after the input loop. On success the kernel prints
 * "selftest: self-test ok"; on any failure it prints the failure
 * with detail so the serial log surfaces what broke.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "pthread.h"
#include "hal/console.h"
#include "sync/cpu.h"

/* Bitmask of logical CPUs the workers were observed running on. With SMP
 * APs online and a shared run queue, work lands on more than one CPU. */
static volatile unsigned g_cpus_seen;

#define WORKERS         4
#define INCS_PER_WORKER 1000

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int    g_counter;
static volatile int    g_alloc_fail;   /* set if a heap readback is wrong */

static void *worker(void *arg) {
    int id = (int)(intptr_t)arg;
    for (int i = 0; i < INCS_PER_WORKER; i++) {
        __atomic_or_fetch(&g_cpus_seen, 1u << (canboot_cpu_id() & 31u),
                          __ATOMIC_RELAXED);
        /* Hammer the allocator OUTSIDE the mutex so all workers contend on
         * the heap concurrently. With preemption on, a timer switch landing
         * mid-malloc would corrupt the (single-thread-built) picolibc free
         * list — surfacing here as a NULL return or a bad readback unless
         * the M5 allocator guard serialises it. Sizes/patterns are derived
         * deterministically; rand() is itself non-reentrant. */
        size_t n = (size_t)(16 + ((id * 37 + i * 13) & 0xFF));
        unsigned char *p = (unsigned char *)malloc(n);
        if (!p) {
            g_alloc_fail = 1;
        } else {
            unsigned char tag = (unsigned char)(id * 31 + i);
            memset(p, tag, n);
            pthread_yield();           /* widen the preemption window */
            for (size_t k = 0; k < n; k++) {
                if (p[k] != tag) { g_alloc_fail = 1; break; }
            }
            free(p);
        }

        pthread_mutex_lock(&g_lock);
        int snap = g_counter;
        pthread_yield();           /* exercise the scheduler mid-section */
        g_counter = snap + 1;
        pthread_mutex_unlock(&g_lock);
        if ((i & 0xFF) == 0) pthread_yield();
    }
    return (void *)(intptr_t)id;
}

size_t canboot_heap_bytes_used(void);
size_t canboot_heap_bytes_total(void);
unsigned long canboot_sched_ticks(void);

/* Preemption proof: a worker that NEVER yields. With cooperative
 * scheduling it would starve every other thread; if it still makes
 * progress while the main thread also runs (and the tick counter
 * advances), the timer must be preempting. */
static volatile int           g_spin_stop;
static volatile unsigned long g_spin_count;

static void *spinner(void *arg) {
    (void)arg;
    while (!g_spin_stop) {
        g_spin_count++;
        __asm__ volatile ("" ::: "memory");
    }
    return NULL;
}

static int preemption_test(void) {
    g_spin_stop = 0;
    g_spin_count = 0;
    pthread_t st;
    if (pthread_create(&st, 0, spinner, 0) != 0) {
        printf("selftest: FAIL preemption spinner create\n");
        return 0;
    }
    /* Busy-wait ~5 timer ticks WITHOUT yielding. If preemption is on, the
     * spinner gets scheduled in the gaps; if the timer never ticks, the
     * guard bound trips and we report it rather than hanging forever. */
    unsigned long t0 = canboot_sched_ticks();
    unsigned long guard = 0;
    while (canboot_sched_ticks() - t0 < 5 && guard < 3000000000UL) {
        guard++;
        __asm__ volatile ("" ::: "memory");
    }
    int ticked = (canboot_sched_ticks() - t0 >= 5);
    g_spin_stop = 1;
    pthread_join(st, 0);

    if (!ticked) {
        printf("selftest: FAIL preemption timer not ticking (guard=%lu)\n", guard);
        return 0;
    }
    if (g_spin_count == 0) {
        printf("selftest: FAIL preemption: non-yielding worker never ran\n");
        return 0;
    }
    printf("selftest: preemption ok spinner=%lu over >=5 ticks\n", g_spin_count);
    return 1;
}

/* Stress the mmap-backed heap: allocate and free well past the old 16 MiB
 * static arena, across many block sizes, verifying each block's contents
 * survive intact. Proves the allocator hands out a large region without
 * corruption. Returns 1 on success. */
#define BIGHEAP_BLOCKS 512
static unsigned char *bh_p[BIGHEAP_BLOCKS];
static size_t         bh_sz[BIGHEAP_BLOCKS];

static int big_heap_test(void) {
    const size_t target = 34u * 1024u * 1024u;   /* comfortably > 32 MiB */
    size_t total = 0;
    int n = 0;

    for (; n < BIGHEAP_BLOCKS && total < target; n++) {
        /* Spread sizes across 8 KiB .. ~136 KiB so many free-list bins and
         * sbrk growth paths get exercised. */
        size_t sz = (size_t)(8u * 1024u + ((n * 2659u) & 0x1FFFFu));
        unsigned char *p = (unsigned char *)malloc(sz);
        if (!p) {
            printf("selftest: FAIL big-heap malloc n=%d sz=%zu total=%zu\n",
                   n, sz, total);
            for (int k = 0; k < n; k++) free(bh_p[k]);
            return 0;
        }
        memset(p, (unsigned char)(n * 131 + 7), sz);
        bh_p[n] = p;
        bh_sz[n] = sz;
        total += sz;
    }

    if (total < 32u * 1024u * 1024u) {
        printf("selftest: FAIL big-heap only reached %zu bytes (< 32 MiB)\n",
               total);
        for (int k = 0; k < n; k++) free(bh_p[k]);
        return 0;
    }

    /* Verify every block still holds its tag. */
    for (int i = 0; i < n; i++) {
        unsigned char tag = (unsigned char)(i * 131 + 7);
        for (size_t k = 0; k < bh_sz[i]; k += 509) {
            if (bh_p[i][k] != tag) {
                printf("selftest: FAIL big-heap corruption block=%d off=%zu\n",
                       i, k);
                for (int z = 0; z < n; z++) free(bh_p[z]);
                return 0;
            }
        }
    }

    /* Free the even blocks, reallocate into the freed space at new sizes,
     * verify, then release everything. */
    for (int i = 0; i < n; i += 2) { free(bh_p[i]); bh_p[i] = NULL; }
    for (int i = 0; i < n; i += 2) {
        size_t sz = (size_t)(4u * 1024u + ((i * 1471u) & 0xFFFFu));
        unsigned char *p = (unsigned char *)malloc(sz);
        if (!p) {
            printf("selftest: FAIL big-heap realloc-wave n=%d sz=%zu\n", i, sz);
            for (int z = 1; z < n; z += 2) free(bh_p[z]);
            return 0;
        }
        memset(p, (unsigned char)(i * 17 + 3), sz);
        bh_p[i] = p;
        bh_sz[i] = sz;
    }
    for (int i = 0; i < n; i++) {
        unsigned char tag = (i & 1) ? (unsigned char)(i * 131 + 7)
                                    : (unsigned char)(i * 17 + 3);
        size_t step = (i & 1) ? 509 : 251;
        for (size_t k = 0; k < bh_sz[i]; k += step) {
            if (bh_p[i][k] != tag) {
                printf("selftest: FAIL big-heap wave2 corruption block=%d\n", i);
                for (int z = 0; z < n; z++) free(bh_p[z]);
                return 0;
            }
        }
    }
    for (int i = 0; i < n; i++) free(bh_p[i]);

    printf("selftest: big-heap alloc/free %zu bytes across %d blocks (>32 MiB) ok\n",
           total, n);
    return 1;
}

void runtime_selftest(void) {
    hal_console_write("selftest: starting self-test\n");

    /* stdio + printf */
    printf("selftest: printf int=%d hex=0x%x str=%s\n", 42, 0xCAFE, "alive");

    /* malloc / free / strcpy */
    char *buf = (char *)malloc(64);
    if (!buf) {
        hal_console_write("selftest: FAIL malloc returned NULL\n");
        return;
    }
    strcpy(buf, "hello, picolibc");
    if (strcmp(buf, "hello, picolibc") != 0) {
        hal_console_write("selftest: FAIL strcpy/strcmp mismatch\n");
        free(buf);
        return;
    }
    printf("selftest: malloc(64)=%p strcpy ok '%s'\n", (void *)buf, buf);
    free(buf);

    /* large allocation to stress sbrk */
    void *big = malloc(64 * 1024);
    if (!big) {
        hal_console_write("selftest: FAIL malloc(64k) returned NULL\n");
        return;
    }
    memset(big, 0xA5, 64 * 1024);
    free(big);
    printf("selftest: heap bytes_used=%zu bytes_total=%zu\n",
           canboot_heap_bytes_used(), canboot_heap_bytes_total());

    /* mmap-backed heap stress: > 32 MiB across many sizes. */
    if (!big_heap_test()) {
        return;
    }

    /* Forced-preemption proof (timer-driven scheduling). */
    if (!preemption_test()) {
        return;
    }

    /* pthread: spawn workers that race a mutex-protected counter and
     * concurrently hammer the allocator under live preemption. */
    g_counter = 0;
    g_alloc_fail = 0;
    pthread_t t[WORKERS];
    for (int i = 0; i < WORKERS; i++) {
        int rc = pthread_create(&t[i], 0, worker, (void *)(intptr_t)i);
        if (rc != 0) {
            printf("selftest: FAIL pthread_create [%d] rc=%d\n", i, rc);
            return;
        }
    }
    for (int i = 0; i < WORKERS; i++) {
        void *ret = 0;
        int rc = pthread_join(t[i], &ret);
        if (rc != 0) {
            printf("selftest: FAIL pthread_join [%d] rc=%d\n", i, rc);
            return;
        }
        if ((intptr_t)ret != i) {
            printf("selftest: FAIL worker [%d] retval=%ld\n",
                   i, (long)(intptr_t)ret);
            return;
        }
    }
    int expected = WORKERS * INCS_PER_WORKER;
    if (g_counter != expected) {
        printf("selftest: FAIL counter=%d expected=%d\n",
               g_counter, expected);
        return;
    }
    if (g_alloc_fail) {
        hal_console_write("selftest: FAIL concurrent malloc/free readback\n");
        return;
    }
    printf("selftest: pthread counter=%d (expected %d) heap-race ok\n",
           g_counter, expected);

    unsigned mask = g_cpus_seen;
    unsigned ncpu = 0;
    for (unsigned m = mask; m; m &= m - 1) ncpu++;
    printf("selftest: smp observed %u cpu(s) (mask=0x%x)\n", ncpu, mask);

    hal_console_write("selftest: self-test ok\n");
}
