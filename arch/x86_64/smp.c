/*
 * x86_64 SMP bring-up (M3) — DORMANT, UNVERIFIED. See smp.h.
 *
 * Flow:
 *   1. Locate the ACPI MADT from the RSDP the loader handed us, collect
 *      every enabled Local APIC's ID, and build apicid -> logical-index.
 *   2. Copy the real-mode AP trampoline (ap_trampoline.S) into a fixed
 *      low page and patch in CR3 (the BSP's PML4) so APs share our
 *      address space.
 *   3. For each AP: INIT, then two STARTUP IPIs pointing at the
 *      trampoline page; hand it a stack via g_ap_stack; wait for the AP
 *      to bump g_ap_online from its C entry (canboot_ap_main).
 *
 * Everything here assumes xAPIC and the identity map established by
 * arch/x86_64/bootstrap.S (first 4 GiB), which covers the LAPIC MMIO and
 * the low trampoline page.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "smp.h"
#include "lapic.h"
#include "sync/cpu.h"
#include "sched/sched.h"
#include "sched/percpu.h"

#define MAX_CPUS        CANBOOT_NR_CPUS
#define TRAMPOLINE_ADDR 0x8000u   /* page-aligned, < 1 MiB (SIPI vector 0x08) */

/* PML4 physical address from bootstrap.S (identity-mapped => phys==virt). */
extern uint8_t pml4[];

/* Trampoline blob bounds + patch slots (ap_trampoline.S). */
extern uint8_t canboot_ap_trampoline_start[];
extern uint8_t canboot_ap_trampoline_end[];
extern uint64_t canboot_ap_trampoline_cr3;  /* patched before SIPI */

/* AP <-> BSP handoff, referenced by the trampoline's 64-bit tail. */
volatile uint64_t canboot_ap_stack;          /* stack top for the next AP */
volatile unsigned canboot_ap_online;         /* APs bump this when alive  */

static uint8_t  g_apic_ids[MAX_CPUS];
static unsigned g_cpu_count = 1;             /* BSP only until discovery  */
static uint8_t  g_apicid_to_index[256];      /* APIC ID -> logical index  */
static int      g_smp_active = 0;

/* Dedicated stacks for APs (the BSP keeps its bootstrap.S stack). */
static __attribute__((aligned(16)))
    uint8_t g_ap_stacks[MAX_CPUS][32u * 1024u];

/* ---- canboot_cpu_id() strong override -------------------------------- */

unsigned canboot_arch_cpu_id(void) {
    if (!g_smp_active)
        return 0u;
    return g_apicid_to_index[canboot_lapic_id() & 0xFFu];
}

unsigned canboot_smp_cpu_count(void) {
    return g_cpu_count;
}

/* ---- ACPI MADT discovery --------------------------------------------- */

struct acpi_sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

struct acpi_rsdp {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_addr;
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

struct madt {
    struct acpi_sdt_header header;
    uint32_t local_apic_addr;
    uint32_t flags;
    uint8_t  entries[];
} __attribute__((packed));

static const struct acpi_sdt_header *find_madt(uint64_t rsdp_addr) {
    if (!rsdp_addr)
        return NULL;
    const struct acpi_rsdp *rsdp = (const struct acpi_rsdp *)(uintptr_t)rsdp_addr;

    /* Prefer the XSDT (64-bit pointers) when ACPI >= 2.0. */
    if (rsdp->revision >= 2 && rsdp->xsdt_addr) {
        const struct acpi_sdt_header *xsdt =
            (const struct acpi_sdt_header *)(uintptr_t)rsdp->xsdt_addr;
        unsigned n = (xsdt->length - sizeof(*xsdt)) / sizeof(uint64_t);
        const uint64_t *ptrs = (const uint64_t *)(xsdt + 1);
        for (unsigned i = 0; i < n; i++) {
            const struct acpi_sdt_header *h =
                (const struct acpi_sdt_header *)(uintptr_t)ptrs[i];
            if (memcmp(h->signature, "APIC", 4) == 0)
                return h;
        }
        return NULL;
    }

    const struct acpi_sdt_header *rsdt =
        (const struct acpi_sdt_header *)(uintptr_t)rsdp->rsdt_addr;
    unsigned n = (rsdt->length - sizeof(*rsdt)) / sizeof(uint32_t);
    const uint32_t *ptrs = (const uint32_t *)(rsdt + 1);
    for (unsigned i = 0; i < n; i++) {
        const struct acpi_sdt_header *h =
            (const struct acpi_sdt_header *)(uintptr_t)ptrs[i];
        if (memcmp(h->signature, "APIC", 4) == 0)
            return h;
    }
    return NULL;
}

static void discover_cpus(uint64_t rsdp_addr) {
    const struct acpi_sdt_header *h = find_madt(rsdp_addr);
    g_cpu_count = 0;
    memset(g_apicid_to_index, 0, sizeof(g_apicid_to_index));

    if (!h) {
        /* No MADT: assume uniprocessor. */
        g_apic_ids[0]  = (uint8_t)canboot_lapic_id();
        g_cpu_count    = 1;
        g_apicid_to_index[g_apic_ids[0]] = 0;
        return;
    }

    const struct madt *m = (const struct madt *)h;
    const uint8_t *p   = m->entries;
    const uint8_t *end = (const uint8_t *)m + m->header.length;

    /* List the BSP first so it always gets logical index 0. */
    uint8_t bsp = (uint8_t)canboot_lapic_id();
    g_apic_ids[g_cpu_count++] = bsp;

    while (p + 2 <= end) {
        uint8_t type = p[0];
        uint8_t len  = p[1];
        if (len < 2)
            break;
        if (type == 0 /* Processor Local APIC */ && len >= 8) {
            uint8_t  apic_id = p[3];
            uint32_t flags;
            memcpy(&flags, p + 4, sizeof(flags));
            int enabled = (flags & 0x1) != 0; /* bit0: enabled */
            if (enabled && apic_id != bsp && g_cpu_count < MAX_CPUS)
                g_apic_ids[g_cpu_count++] = apic_id;
        }
        p += len;
    }

    for (unsigned i = 0; i < g_cpu_count; i++)
        g_apicid_to_index[g_apic_ids[i]] = (uint8_t)i;
}

/* ---- AP C entry ------------------------------------------------------ */

/* Called (64-bit) from the trampoline tail with a private stack. */
__attribute__((noreturn)) void canboot_ap_main(void) {
    canboot_lapic_enable_this_cpu();
    canboot_lapic_timer_start_this_cpu();

    unsigned idx = g_apicid_to_index[canboot_lapic_id() & 0xFFu];

    __atomic_add_fetch(&canboot_ap_online, 1, __ATOMIC_RELEASE);

    canboot_irq_enable();        /* timer can now preempt this CPU */
    canboot_sched_ap_online(idx); /* never returns */
}

/* ---- Bring-up -------------------------------------------------------- */

static void udelay_spin(unsigned loops) {
    for (volatile unsigned i = 0; i < loops * 100000u; i++)
        __asm__ volatile ("pause");
}

void canboot_smp_boot_aps(uint64_t acpi_rsdp) {
    discover_cpus(acpi_rsdp);
    g_smp_active = 1; /* canboot_cpu_id() now uses the APIC-ID map */

    printf("canboot: smp discovered %u cpu(s)\n", g_cpu_count);
    if (g_cpu_count <= 1)
        return;

    /* Copy the trampoline into the low page, then patch CR3 *in the
     * copy* (the kernel-resident symbol is not what the AP executes). */
    size_t tramp_len =
        (size_t)(canboot_ap_trampoline_end - canboot_ap_trampoline_start);
    memcpy((void *)(uintptr_t)TRAMPOLINE_ADDR,
           canboot_ap_trampoline_start, tramp_len);

    size_t cr3_off = (size_t)((uint8_t *)&canboot_ap_trampoline_cr3 -
                              canboot_ap_trampoline_start);
    *(volatile uint64_t *)(uintptr_t)(TRAMPOLINE_ADDR + cr3_off) =
        (uint64_t)(uintptr_t)pml4;

    uint8_t sipi_vector = (uint8_t)(TRAMPOLINE_ADDR >> 12); /* 0x08 */

    for (unsigned i = 1; i < g_cpu_count; i++) {
        uint32_t apic = g_apic_ids[i];
        canboot_ap_stack =
            (uint64_t)(uintptr_t)(g_ap_stacks[i] + sizeof(g_ap_stacks[i]));

        unsigned before = __atomic_load_n(&canboot_ap_online, __ATOMIC_ACQUIRE);

        canboot_lapic_send_init(apic);
        udelay_spin(10);                       /* ~10 ms */
        canboot_lapic_send_sipi(apic, sipi_vector);
        udelay_spin(1);
        canboot_lapic_send_sipi(apic, sipi_vector); /* SDM: send twice */

        /* Wait (bounded) for the AP to signal it reached canboot_ap_main. */
        for (unsigned spin = 0; spin < 100u; spin++) {
            if (__atomic_load_n(&canboot_ap_online, __ATOMIC_ACQUIRE) > before)
                break;
            udelay_spin(1);
        }
        if (__atomic_load_n(&canboot_ap_online, __ATOMIC_ACQUIRE) <= before)
            printf("canboot: smp AP apic=%u did not come online\n",
                   (unsigned)apic);
        else
            printf("canboot: smp AP apic=%u online (cpu %u)\n",
                   (unsigned)apic, i);
    }
}

void canboot_smp_reschedule(unsigned cpu_index) {
    if (!g_smp_active || cpu_index >= g_cpu_count)
        return;
    if (cpu_index == canboot_cpu_id())
        return;
    canboot_lapic_send_ipi(g_apic_ids[cpu_index], 0x20u);
}
