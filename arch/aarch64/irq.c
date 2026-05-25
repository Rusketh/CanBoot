/*
 * aarch64 interrupt + generic-timer bring-up (the "M4" scheduler item).
 *
 * Mirrors the x86_64 LAPIC preemption path: install the EL1 vector table,
 * bring up the GICv2 distributor + CPU interface, arm the EL1 virtual
 * generic timer (CNTV) as a periodic source on PPI 27, and route its IRQ
 * to canboot_sched_on_tick(). With this in place the aarch64 kmain turns
 * on forced preemption exactly as x86_64 does.
 *
 * GICv2 MMIO addresses are the QEMU `virt` machine's fixed layout; the
 * runners pin gic-version=2 so this matches both the -kernel and AAVMF
 * UEFI boots.
 */

#include <stdint.h>
#include <stdio.h>

#include "sched/sched.h"

#define GICD_BASE   0x08000000UL
#define GICC_BASE   0x08010000UL

#define GICD_CTLR        (*(volatile uint32_t *)(GICD_BASE + 0x000))
#define GICD_ISENABLER0  (*(volatile uint32_t *)(GICD_BASE + 0x100))
#define GICD_IPRIORITY   ((volatile uint8_t  *)(GICD_BASE + 0x400))
#define GICD_ICFGR1      (*(volatile uint32_t *)(GICD_BASE + 0xC04))

#define GICC_CTLR        (*(volatile uint32_t *)(GICC_BASE + 0x000))
#define GICC_PMR         (*(volatile uint32_t *)(GICC_BASE + 0x004))
#define GICC_IAR         (*(volatile uint32_t *)(GICC_BASE + 0x00C))
#define GICC_EOIR        (*(volatile uint32_t *)(GICC_BASE + 0x010))

/* EL1 virtual timer private peripheral interrupt on the QEMU virt GIC. */
#define TIMER_INTID 27u

extern char canboot_aarch64_vectors[];

static uint64_t g_tick_interval;

static inline void timer_rearm(void) {
    __asm__ volatile ("msr cntv_tval_el0, %0" :: "r"(g_tick_interval));
}

void canboot_aarch64_irq_init(unsigned hz) {
    if (hz == 0) hz = 100u;

    /* Vector table. */
    __asm__ volatile ("msr vbar_el1, %0" :: "r"(canboot_aarch64_vectors));
    __asm__ volatile ("isb");

    /* GIC distributor: enable, route PPI 27 as level-sensitive, mid prio. */
    GICD_CTLR = 1u;
    GICD_IPRIORITY[TIMER_INTID] = 0xA0;     /* below the PMR cutoff       */
    GICD_ICFGR1 &= ~(3u << ((TIMER_INTID % 16) * 2)); /* level-triggered  */
    GICD_ISENABLER0 = (1u << TIMER_INTID);

    /* GIC CPU interface: allow all priorities, enable signalling. */
    GICC_PMR  = 0xFFu;
    GICC_CTLR = 1u;

    /* Generic timer: program the down-counter interval and enable it. */
    uint64_t frq;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(frq));
    g_tick_interval = frq / hz;
    timer_rearm();
    __asm__ volatile ("msr cntv_ctl_el0, %0" :: "r"((uint64_t)1)); /* enable */
    __asm__ volatile ("isb");

    printf("canboot: aarch64 gic+timer up (%u Hz, cntfrq=%lu)\n",
           hz, (unsigned long)frq);
}

/* Called from the IRQ vector (vectors.S) with IRQs masked. */
void aarch64_irq_handler(void) {
    uint32_t iar   = GICC_IAR;
    uint32_t intid = iar & 0x3FFu;

    if (intid == TIMER_INTID) {
        timer_rearm();          /* schedule the next tick                 */
        GICC_EOIR = iar;        /* EOI before any context switch          */
        canboot_sched_on_tick();/* may park us mid-ISR and switch threads */
        return;
    }
    if (intid < 1020u)          /* a real but unexpected INTID */
        GICC_EOIR = iar;
}

void aarch64_fault_handler(unsigned vec, uint64_t esr, uint64_t elr) {
    printf("canboot: FATAL aarch64 exception vec=%u esr=0x%lx elr=0x%lx\n",
           vec, (unsigned long)esr, (unsigned long)elr);
    for (;;)
        __asm__ volatile ("wfe");
}
