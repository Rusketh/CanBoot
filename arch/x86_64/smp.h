#ifndef CANBOOT_ARCH_X86_64_SMP_H
#define CANBOOT_ARCH_X86_64_SMP_H

#include <stdint.h>

/*
 * x86_64 SMP bring-up (M3). DORMANT BY DEFAULT — kmain does not call
 * canboot_smp_boot_aps() yet. The single-CPU boot is unchanged; this is
 * here so the AP path can be enabled and boot-debugged on real hardware
 * / QEMU without first writing it.
 *
 * !!! HIGH RISK / UNVERIFIED !!!
 * The Application-Processor trampoline (arch/x86_64/ap_trampoline.S) and
 * the INIT-SIPI-SIPI sequence below cannot be validated by inspection.
 * Expect to iterate on this with a live multi-core boot. Until then the
 * scheduler runs CPU 0 only.
 *
 * Scope: xAPIC + ACPI MADT discovery only. x2APIC, per-CPU GDT/TSS, and
 * a GS-base per-CPU pointer (to make canboot_cpu_id() O(1) instead of a
 * LAPIC-ID read + table lookup) are follow-ups.
 */

/* Parse the ACPI MADT for CPU APIC IDs, build the APIC-ID -> logical
 * index map, then INIT-SIPI-SIPI every Application Processor and wait
 * for each to join the scheduler. acpi_rsdp is boot_info->acpi_rsdp.
 * Must run on the BSP after the LAPIC timer is calibrated. */
void canboot_smp_boot_aps(uint64_t acpi_rsdp);

/* Number of CPUs discovered (1 until canboot_smp_boot_aps runs). */
unsigned canboot_smp_cpu_count(void);

/* Send a reschedule IPI to a logical CPU (no-op for self / offline). */
void canboot_smp_reschedule(unsigned cpu_index);

#endif /* CANBOOT_ARCH_X86_64_SMP_H */
