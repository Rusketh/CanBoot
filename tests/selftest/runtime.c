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

#define WORKERS         4
#define INCS_PER_WORKER 1000

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int    g_counter;
static volatile int    g_alloc_fail;   /* set if a heap readback is wrong */

static void *worker(void *arg) {
    int id = (int)(intptr_t)arg;
    for (int i = 0; i < INCS_PER_WORKER; i++) {
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

    hal_console_write("selftest: self-test ok\n");
}
