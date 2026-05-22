/*
 * aarch64 UEFI entry. Mirrors boot/uefi/efi_main.c (x86_64) but targets
 * the QEMU virt platform: PL011 serial at 0x09000000 takes the role of
 * COM1, and the configuration table is searched for a flattened device
 * tree (DTB) instead of an x86 ACPI RSDP.
 *
 * After ExitBootServices we lose the firmware's services entirely; the
 * PL011 driver inside the kernel takes over for further diagnostics.
 */

#include <efi.h>
#include <efilib.h>

#include "canboot/boot_info.h"

#define PL011_BASE 0x09000000ull
#define PL011_DR   ((volatile uint32_t *)(PL011_BASE + 0x00))
#define PL011_FR   ((volatile uint32_t *)(PL011_BASE + 0x18))
#define PL011_FR_TXFF (1u << 5)

static void pl011_putc(char c) {
    while ((*PL011_FR) & PL011_FR_TXFF) { }
    *PL011_DR = (uint32_t)(unsigned char)c;
}
static void pl011_write(const char *s) {
    while (*s) {
        if (*s == '\n') pl011_putc('\r');
        pl011_putc(*s++);
    }
}

void kmain(struct boot_info *bi);

static struct boot_info g_boot_info;

/* DEVICE_TREE_GUID per UEFI spec (b1b621d5-f19c-41a5-830b-d9152c69aae0) */
static EFI_GUID dtb_guid = {
    0xb1b621d5, 0xf19c, 0x41a5,
    { 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0 }
};
static EFI_GUID acpi_20_guid = ACPI_20_TABLE_GUID;

static int guid_eq(const EFI_GUID *a, const EFI_GUID *b) {
    const UINT8 *pa = (const UINT8 *)a;
    const UINT8 *pb = (const UINT8 *)b;
    for (UINTN i = 0; i < sizeof(EFI_GUID); i++) {
        if (pa[i] != pb[i]) return 0;
    }
    return 1;
}

static void populate_fb(EFI_SYSTEM_TABLE *st, struct boot_info *bi) {
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_STATUS s = uefi_call_wrapper(st->BootServices->LocateProtocol, 3,
                                     &gop_guid, NULL, (void **)&gop);
    if (EFI_ERROR(s) || gop == NULL || gop->Mode == NULL || gop->Mode->Info == NULL) {
        bi->fb.format = CANBOOT_FB_NONE;
        return;
    }

    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = gop->Mode->Info;
    bi->fb.addr   = (uint64_t)gop->Mode->FrameBufferBase;
    bi->fb.width  = info->HorizontalResolution;
    bi->fb.height = info->VerticalResolution;
    bi->fb.pitch  = info->PixelsPerScanLine * 4u;
    bi->fb.bpp    = 32;

    switch (info->PixelFormat) {
        case PixelRedGreenBlueReserved8BitPerColor:
            bi->fb.format = CANBOOT_FB_RGB;
            bi->fb.red_mask_shift   = 0;  bi->fb.red_mask_size   = 8;
            bi->fb.green_mask_shift = 8;  bi->fb.green_mask_size = 8;
            bi->fb.blue_mask_shift  = 16; bi->fb.blue_mask_size  = 8;
            break;
        case PixelBlueGreenRedReserved8BitPerColor:
            bi->fb.format = CANBOOT_FB_RGB;
            bi->fb.blue_mask_shift  = 0;  bi->fb.blue_mask_size  = 8;
            bi->fb.green_mask_shift = 8;  bi->fb.green_mask_size = 8;
            bi->fb.red_mask_shift   = 16; bi->fb.red_mask_size   = 8;
            break;
        default:
            bi->fb.format = CANBOOT_FB_NONE;
            break;
    }
}

/* On aarch64 the firmware exposes a DTB (or, less commonly on QEMU virt,
 * an ACPI RSDP) via the configuration table. We stash whichever pointer
 * we find in boot_info.acpi_rsdp - it's just an opaque "platform tables"
 * handle as far as the kernel is concerned at this milestone. */
static void populate_platform_tables(EFI_SYSTEM_TABLE *st, struct boot_info *bi) {
    for (UINTN i = 0; i < st->NumberOfTableEntries; i++) {
        EFI_GUID *g = &st->ConfigurationTable[i].VendorGuid;
        if (guid_eq(g, &dtb_guid)) {
            bi->acpi_rsdp = (uint64_t)(uintptr_t)st->ConfigurationTable[i].VendorTable;
            return;
        }
    }
    for (UINTN i = 0; i < st->NumberOfTableEntries; i++) {
        EFI_GUID *g = &st->ConfigurationTable[i].VendorGuid;
        if (guid_eq(g, &acpi_20_guid)) {
            bi->acpi_rsdp = (uint64_t)(uintptr_t)st->ConfigurationTable[i].VendorTable;
            return;
        }
    }
}

static void populate_mmap(EFI_MEMORY_DESCRIPTOR *map, UINTN map_size,
                          UINTN desc_size, struct boot_info *bi) {
    UINTN n = desc_size ? (map_size / desc_size) : 0;
    uint32_t out_n = 0;
    for (UINTN i = 0; i < n && out_n < CANBOOT_MMAP_MAX; i++) {
        EFI_MEMORY_DESCRIPTOR *d =
            (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)map + i * desc_size);
        bi->mmap[out_n].base   = (uint64_t)d->PhysicalStart;
        bi->mmap[out_n].length = (uint64_t)d->NumberOfPages * 4096ull;
        switch (d->Type) {
            case EfiConventionalMemory:
            case EfiBootServicesCode:
            case EfiBootServicesData:
            case EfiLoaderCode:
            case EfiLoaderData:
                bi->mmap[out_n].type = CANBOOT_MMAP_USABLE; break;
            case EfiACPIReclaimMemory:
                bi->mmap[out_n].type = CANBOOT_MMAP_ACPI_RECL; break;
            case EfiACPIMemoryNVS:
                bi->mmap[out_n].type = CANBOOT_MMAP_ACPI_NVS; break;
            case EfiUnusableMemory:
                bi->mmap[out_n].type = CANBOOT_MMAP_BAD; break;
            default:
                bi->mmap[out_n].type = CANBOOT_MMAP_RESERVED; break;
        }
        out_n++;
    }
    bi->mmap_count = out_n;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    InitializeLib(image, st);

    Print(L"canboot: uefi entry reached (aarch64)\n");
    pl011_write("canboot: uefi entry reached (aarch64)\n");

    g_boot_info.magic = CANBOOT_BOOT_INFO_MAGIC;
    g_boot_info.version = CANBOOT_BOOT_INFO_VERSION;
    g_boot_info.boot_source = CANBOOT_BOOT_UEFI;

    populate_fb(st, &g_boot_info);
    populate_platform_tables(st, &g_boot_info);

    UINTN map_size = 0, map_key = 0, desc_size = 0;
    UINT32 desc_version = 0;
    EFI_MEMORY_DESCRIPTOR *map = NULL;
    EFI_STATUS s = uefi_call_wrapper(st->BootServices->GetMemoryMap, 5,
                                     &map_size, map, &map_key,
                                     &desc_size, &desc_version);
    map_size += 8 * desc_size;
    s = uefi_call_wrapper(st->BootServices->AllocatePool, 3,
                          EfiLoaderData, map_size, (void **)&map);
    if (EFI_ERROR(s) || map == NULL) {
        pl011_write("canboot: FATAL AllocatePool for mmap\n");
        for (;;) __asm__ volatile ("wfe");
    }
    s = uefi_call_wrapper(st->BootServices->GetMemoryMap, 5,
                          &map_size, map, &map_key, &desc_size, &desc_version);
    if (EFI_ERROR(s)) {
        pl011_write("canboot: FATAL GetMemoryMap\n");
        for (;;) __asm__ volatile ("wfe");
    }

    populate_mmap(map, map_size, desc_size, &g_boot_info);

    pl011_write("canboot: calling ExitBootServices (aarch64)\n");

    s = uefi_call_wrapper(st->BootServices->ExitBootServices, 2,
                          image, map_key);
    if (EFI_ERROR(s)) {
        s = uefi_call_wrapper(st->BootServices->GetMemoryMap, 5,
                              &map_size, map, &map_key,
                              &desc_size, &desc_version);
        if (!EFI_ERROR(s)) {
            populate_mmap(map, map_size, desc_size, &g_boot_info);
            s = uefi_call_wrapper(st->BootServices->ExitBootServices, 2,
                                  image, map_key);
        }
        if (EFI_ERROR(s)) {
            pl011_write("canboot: FATAL ExitBootServices failed twice\n");
            for (;;) __asm__ volatile ("wfe");
        }
    }

    kmain(&g_boot_info);

    for (;;) __asm__ volatile ("wfe");
    return EFI_SUCCESS;
}
