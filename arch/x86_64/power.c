/*
 * x86_64 ACPI power control: poweroff (S5) and reboot, parsed from the
 * firmware ACPI tables handed to us in boot_info.acpi_rsdp.
 *
 * RSDP -> RSDT (or XSDT on ACPI 2.0+) -> FADT ("FACP") gives the PM1a/PM1b
 * control registers and the reset register; the FADT's DSDT is scanned for
 * the \_S5 package to recover the SLP_TYP value an S5 transition needs.
 * Poweroff writes (SLP_TYP << 10) | SLP_EN to the PM1 control register(s).
 * Reboot prefers the ACPI reset register, then an 8042 pulse, then a triple
 * fault. ACPI tables live in reclaimable low memory which the kernel
 * identity-maps, so physical addresses are dereferenced directly.
 *
 * This is intentionally a thin, no-AML-interpreter parser: enough to cover
 * the common firmware (QEMU, real PCs, the major hypervisors) the same way
 * a minimal boot environment does.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "hal/power.h"
#include "canboot/env.h"

#define SLP_EN (1u << 13)

static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline void outw(uint16_t port, uint16_t v) {
    __asm__ volatile ("outw %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v; __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port)); return v;
}

static inline const uint8_t *phys(uint64_t addr) {
    return (const uint8_t *)(uintptr_t)addr;
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static struct canboot_power_info g_info;
static bool g_probed;

/* Locate the \_S5 package in the DSDT AML and extract SLP_TYPa/b (the first
 * two integer elements). Returns true on success. */
static bool parse_s5(uint64_t dsdt_phys, uint8_t *typa, uint8_t *typb) {
    if (!dsdt_phys) return false;
    const uint8_t *h = phys(dsdt_phys);
    if (memcmp(h, "DSDT", 4) != 0) return false;
    uint32_t len = rd32(h + 4);
    if (len < 36 || len > 0x100000) return false;   /* sanity bound */
    const uint8_t *aml = h + 36;
    uint32_t n = len - 36;

    for (uint32_t i = 0; i + 4 < n; i++) {
        if (aml[i] != '_' || aml[i+1] != 'S' || aml[i+2] != '5' || aml[i+3] != '_')
            continue;
        const uint8_t *p = aml + i + 4;
        const uint8_t *end = aml + n;
        if (p >= end || *p != 0x12) return false;    /* expect PackageOp */
        p++;
        if (p >= end) return false;
        p += 1 + (*p >> 6);                           /* skip PkgLength */
        if (p >= end) return false;
        p++;                                          /* skip NumElements */
        /* First element: SLP_TYPa. */
        if (p < end && *p == 0x0A) p++;               /* BytePrefix */
        if (p >= end) return false;
        *typa = *p++;
        /* Second element: SLP_TYPb (best-effort). */
        if (p < end && *p == 0x0A) p++;
        *typb = (p < end) ? *p : 0;
        return true;
    }
    return false;
}

static const uint8_t *find_fadt(void) {
    const struct boot_info *bi = canboot_env_boot_info();
    if (!bi || !bi->acpi_rsdp) return NULL;
    const uint8_t *rsdp = phys(bi->acpi_rsdp);
    if (memcmp(rsdp, "RSD PTR ", 8) != 0) return NULL;

    uint8_t rev = rsdp[15];
    const uint8_t *sdt;
    int entry_sz;
    uint32_t count;
    if (rev >= 2) {
        uint64_t xsdt = rd64(rsdp + 24);
        if (!xsdt) return NULL;
        sdt = phys(xsdt);
        if (memcmp(sdt, "XSDT", 4) != 0) return NULL;
        entry_sz = 8;
    } else {
        uint32_t rsdt = rd32(rsdp + 16);
        if (!rsdt) return NULL;
        sdt = phys(rsdt);
        if (memcmp(sdt, "RSDT", 4) != 0) return NULL;
        entry_sz = 4;
    }
    count = (rd32(sdt + 4) - 36) / (uint32_t)entry_sz;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = sdt + 36 + i * entry_sz;
        uint64_t taddr = (entry_sz == 8) ? rd64(e) : rd32(e);
        const uint8_t *t = phys(taddr);
        if (memcmp(t, "FACP", 4) == 0) return t;
    }
    return NULL;
}

bool canboot_power_probe(struct canboot_power_info *out) {
    if (g_probed) { if (out) *out = g_info; return g_info.slp_typ_known; }
    g_probed = true;
    memset(&g_info, 0, sizeof(g_info));

    const uint8_t *fadt = find_fadt();
    if (fadt) {
        g_info.pm1a_cnt = (uint16_t)rd32(fadt + 64);
        g_info.pm1b_cnt = (uint16_t)rd32(fadt + 68);

        uint64_t dsdt = rd32(fadt + 40);
        uint32_t fadt_len = rd32(fadt + 4);
        if ((!dsdt) && fadt_len >= 148) dsdt = rd64(fadt + 140);  /* X_DSDT */
        if (parse_s5(dsdt, &g_info.slp_typa, &g_info.slp_typb))
            g_info.slp_typ_known = true;

        uint32_t flags = rd32(fadt + 112);
        if ((flags & (1u << 10)) && fadt_len >= 129) {  /* RESET_REG_SUP */
            g_info.reset_space = fadt[116];              /* GAS space id  */
            g_info.reset_addr  = rd64(fadt + 120);
            g_info.reset_value = fadt[128];
            if (g_info.reset_addr) g_info.reset_supported = true;
        }
    }

    if (out) *out = g_info;
    return g_info.slp_typ_known && g_info.pm1a_cnt != 0;
}

void canboot_power_off(void) {
    canboot_power_probe(NULL);
    if (g_info.pm1a_cnt && g_info.slp_typ_known) {
        outw(g_info.pm1a_cnt, (uint16_t)((g_info.slp_typa << 10) | SLP_EN));
        if (g_info.pm1b_cnt)
            outw(g_info.pm1b_cnt, (uint16_t)((g_info.slp_typb << 10) | SLP_EN));
    }
    /* Well-known hypervisor poweroff ports, in case ACPI parsing came up
     * short (QEMU q35 / piix, Bochs, VirtualBox). */
    outw(0x0604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);
    for (;;) __asm__ volatile ("cli; hlt");
}

void canboot_reboot(void) {
    canboot_power_probe(NULL);
    if (g_info.reset_supported) {
        if (g_info.reset_space == 1)            /* System I/O */
            outb((uint16_t)g_info.reset_addr, g_info.reset_value);
        else if (g_info.reset_space == 0)       /* System Memory */
            *(volatile uint8_t *)(uintptr_t)g_info.reset_addr = g_info.reset_value;
    }
    /* 8042 keyboard-controller pulse. */
    for (int i = 0; i < 100000; i++)
        if (!(inb(0x64) & 0x02)) break;
    outb(0x64, 0xFE);
    /* Last resort: triple fault via a null IDT. */
    struct __attribute__((packed)) { uint16_t limit; uint64_t base; } idtr = { 0, 0 };
    __asm__ volatile ("lidt %0; int3" : : "m"(idtr));
    for (;;) __asm__ volatile ("cli; hlt");
}
