/*
 * x86_64 Local APIC timer driver + timer IRQ dispatch (M2).
 *
 * Drives preemptive scheduling: a periodic LAPIC timer interrupt on
 * vector 0x20 calls canboot_sched_on_tick(), which advances the tick
 * counter and (when preemption is enabled) preempts the running thread.
 *
 * UNVERIFIED: not compiled or booted here. LAPIC register offsets and
 * the divide/periodic encodings follow the Intel SDM; calibration uses
 * the TSC Hz already established by net/lwip_port/sys_arch.c.
 */

#include <stdint.h>
#include <stdio.h>

#include "lapic.h"
#include "idt.h"
#include "sched/sched.h"

/* TSC Hz, calibrated lazily against the PIT in sys_arch.c. */
extern uint64_t canboot_tsc_hz(void);

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"((uint32_t)v),
                                   "d"((uint32_t)(v >> 32)));
}

/* ---- LAPIC MMIO ------------------------------------------------------ */

#define LAPIC_BASE        0xFEE00000ull
#define IA32_APIC_BASE    0x1Bu
#define APIC_BASE_ENABLE  (1ull << 11)

#define LAPIC_REG_ID          0x020u
#define LAPIC_REG_EOI         0x0B0u
#define LAPIC_REG_SVR         0x0F0u
#define LAPIC_REG_TPR         0x080u
#define LAPIC_REG_LVT_TIMER   0x320u
#define LAPIC_REG_TIMER_INIT  0x380u
#define LAPIC_REG_TIMER_CUR   0x390u
#define LAPIC_REG_TIMER_DIV   0x3E0u

#define LVT_MASKED        (1u << 16)
#define LVT_TIMER_PERIODIC (1u << 17)
#define TIMER_DIV_16      0x3u   /* divide configuration register: /16 */

#define TIMER_VECTOR      0x20u
#define SPURIOUS_VECTOR   0xFFu

static inline uint32_t lapic_read(uint32_t reg) {
    return *(volatile uint32_t *)(LAPIC_BASE + reg);
}
static inline void lapic_write(uint32_t reg, uint32_t v) {
    *(volatile uint32_t *)(LAPIC_BASE + reg) = v;
}

void canboot_lapic_eoi(void) {
    lapic_write(LAPIC_REG_EOI, 0);
}

/* asm stubs in idt_stubs.S */
extern void canboot_irq_timer_stub(void);
extern void canboot_irq_spurious_stub(void);

/* Called from canboot_irq_timer_stub after the trap frame is built. */
void canboot_irq_timer(void) {
    canboot_lapic_eoi();
    canboot_sched_on_tick();
}

static void pic_mask_all(void) {
    /* Mask every line on both 8259s so legacy IRQs never reach the CPU
     * once we set IF; the LAPIC is the only interrupt source we want. */
    outb(0x21, 0xFFu); /* master PIC data */
    outb(0xA1, 0xFFu); /* slave  PIC data */
}

static uint32_t calibrate_lapic_hz(void) {
    /* Count LAPIC ticks (one-shot, masked) over a TSC-measured 10 ms. */
    lapic_write(LAPIC_REG_TIMER_DIV, TIMER_DIV_16);
    lapic_write(LAPIC_REG_LVT_TIMER, LVT_MASKED | TIMER_VECTOR); /* one-shot, masked */
    lapic_write(LAPIC_REG_TIMER_INIT, 0xFFFFFFFFu);

    uint64_t hz = canboot_tsc_hz();
    if (hz == 0)
        hz = 1000000000ull;
    uint64_t window = hz / 100ull; /* 10 ms in TSC cycles */

    uint64_t start = rdtsc();
    while (rdtsc() - start < window)
        __asm__ volatile ("pause");

    uint32_t remaining = lapic_read(LAPIC_REG_TIMER_CUR);
    uint32_t elapsed   = 0xFFFFFFFFu - remaining;
    lapic_write(LAPIC_REG_TIMER_INIT, 0); /* stop */

    return elapsed * 100u; /* ticks/sec */
}

void canboot_lapic_timer_setup(unsigned hz) {
    if (hz == 0)
        hz = 100u;

    pic_mask_all();

    /* Ensure the LAPIC is globally enabled in the APIC base MSR. */
    wrmsr(IA32_APIC_BASE, rdmsr(IA32_APIC_BASE) | APIC_BASE_ENABLE);

    /* Accept all interrupt priorities, and software-enable the LAPIC
     * with a spurious vector. */
    lapic_write(LAPIC_REG_TPR, 0);
    lapic_write(LAPIC_REG_SVR, 0x100u | SPURIOUS_VECTOR); /* bit 8 = enable */

    canboot_idt_set_gate(TIMER_VECTOR,    canboot_irq_timer_stub);
    canboot_idt_set_gate(SPURIOUS_VECTOR, canboot_irq_spurious_stub);

    uint32_t lapic_hz = calibrate_lapic_hz();
    uint32_t count    = lapic_hz / hz;
    if (count == 0)
        count = 1;

    lapic_write(LAPIC_REG_TIMER_DIV,  TIMER_DIV_16);
    lapic_write(LAPIC_REG_TIMER_INIT, count);
    lapic_write(LAPIC_REG_LVT_TIMER,  LVT_TIMER_PERIODIC | TIMER_VECTOR);

    printf("canboot: lapic timer %u Hz (lapic_hz=%u count=%u)\n",
           hz, (unsigned)lapic_hz, (unsigned)count);
}
