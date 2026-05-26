#ifndef CANBOOT_HAL_POWER_H
#define CANBOOT_HAL_POWER_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Power control. x86_64 parses the firmware ACPI tables (RSDP -> RSDT/XSDT
 * -> FADT, plus the \_S5 object in the DSDT) to drive an ACPI poweroff (S5)
 * and reboot, with an 8042 pulse / triple-fault reboot fallback. Arches
 * without an implementation fall back to halting (weak defaults).
 */

/* Power the machine off (ACPI S5). Does not return on success; halts if no
 * poweroff path is available. */
void canboot_power_off(void) __attribute__((noreturn));

/* Reset the machine (ACPI reset register, else 8042, else triple fault). */
void canboot_reboot(void) __attribute__((noreturn));

/* Non-destructive discovery for diagnostics/selftests: parse the ACPI power
 * registers without acting on them. Returns true if a usable poweroff path
 * (PM1a control + an S5 sleep type) was found. */
struct canboot_power_info {
    uint16_t pm1a_cnt;       /* PM1a control register I/O port */
    uint16_t pm1b_cnt;       /* PM1b control (0 if none)        */
    uint8_t  slp_typa;       /* SLP_TYP for S5 (PM1a)           */
    uint8_t  slp_typb;       /* SLP_TYP for S5 (PM1b)           */
    bool     slp_typ_known;
    bool     reset_supported;
    uint8_t  reset_space;    /* GAS space id: 1=I/O, 0=memory   */
    uint64_t reset_addr;
    uint8_t  reset_value;
};
bool canboot_power_probe(struct canboot_power_info *out);

#endif /* CANBOOT_HAL_POWER_H */
