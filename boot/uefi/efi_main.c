/*
 * UEFI entry. Locates GOP for the framebuffer, harvests the memory map,
 * walks the configuration table for an ACPI RSDP, calls ExitBootServices,
 * builds a normalised struct boot_info, and dispatches the unified kmain.
 *
 * After ExitBootServices we lose Print() and BootServices entirely; the
 * COM1 driver inside the kernel takes over for further diagnostics.
 */

#include <efi.h>
#include <efilib.h>

#include "canboot/boot_info.h"

#define COM1 0x3F8u

static inline void outb(unsigned short port, unsigned char val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline unsigned char inb(unsigned short port) {
    unsigned char ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static void serial_init_early(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}
static void serial_putc_early(char c) {
    while ((inb(COM1 + 5) & 0x20) == 0) { }
    outb(COM1, (unsigned char)c);
}
static void serial_write_early(const char *s) {
    while (*s) {
        if (*s == '\n') serial_putc_early('\r');
        serial_putc_early(*s++);
    }
}

void kmain(struct boot_info *bi);

static struct boot_info g_boot_info;

static EFI_GUID acpi_20_guid = ACPI_20_TABLE_GUID;
static EFI_GUID acpi_10_guid = ACPI_TABLE_GUID;

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
            /* BLT-only or unknown bitmask; treat as unavailable for now. */
            bi->fb.format = CANBOOT_FB_NONE;
            break;
    }
}

static void populate_acpi(EFI_SYSTEM_TABLE *st, struct boot_info *bi) {
    for (UINTN i = 0; i < st->NumberOfTableEntries; i++) {
        EFI_GUID *g = &st->ConfigurationTable[i].VendorGuid;
        if (guid_eq(g, &acpi_20_guid)) {
            bi->acpi_rsdp = (uint64_t)(uintptr_t)st->ConfigurationTable[i].VendorTable;
            return;
        }
    }
    for (UINTN i = 0; i < st->NumberOfTableEntries; i++) {
        EFI_GUID *g = &st->ConfigurationTable[i].VendorGuid;
        if (guid_eq(g, &acpi_10_guid)) {
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
            /* Genuinely free RAM the kernel heap may claim. EfiLoaderCode/
             * Data are deliberately NOT usable: they hold the running image,
             * the boot_info struct, and the boot files (init.cdo, ...), which
             * the kernel keeps reading and the heap must never overwrite. */
            case EfiConventionalMemory:
            case EfiBootServicesCode:
            case EfiBootServicesData:
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

/*
 * Best-effort: read a small file off the boot volume into freshly
 * AllocatePages'd memory and record it in boot_info. Any failure simply
 * skips the file (the kernel then falls back to scanning real disks), so
 * a problem here can never break the boot.
 */
/* Returns 1 if the file was opened, read, and recorded; 0 otherwise. */
static int load_one_boot_file(EFI_FILE_HANDLE root, CHAR16 *wname,
                              const char *aname, struct boot_info *bi) {
    if (bi->file_count >= CANBOOT_BOOT_FILE_MAX) return 0;

    EFI_FILE_HANDLE fh = NULL;
    EFI_STATUS s = uefi_call_wrapper(root->Open, 5, root, &fh, wname,
                                     EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(s) || fh == NULL) return 0;

    UINT64 size = 0;
    s = uefi_call_wrapper(fh->SetPosition, 2, fh, 0xFFFFFFFFFFFFFFFFULL);
    if (!EFI_ERROR(s))
        s = uefi_call_wrapper(fh->GetPosition, 2, fh, &size);
    if (EFI_ERROR(s) || size == 0 || size > (16ull * 1024 * 1024)) {
        uefi_call_wrapper(fh->Close, 1, fh);
        return 0;
    }
    (void)uefi_call_wrapper(fh->SetPosition, 2, fh, 0);

    UINTN pages = (UINTN)((size + 4095ull) / 4096ull);
    EFI_PHYSICAL_ADDRESS addr = 0;
    s = uefi_call_wrapper(BS->AllocatePages, 4,
                          AllocateAnyPages, EfiLoaderData, pages, &addr);
    if (EFI_ERROR(s) || addr == 0) {
        uefi_call_wrapper(fh->Close, 1, fh);
        return 0;
    }

    UINTN rd = (UINTN)size;
    s = uefi_call_wrapper(fh->Read, 3, fh, &rd, (void *)(uintptr_t)addr);
    uefi_call_wrapper(fh->Close, 1, fh);
    if (EFI_ERROR(s) || rd == 0) {
        uefi_call_wrapper(BS->FreePages, 2, addr, pages);
        return 0;
    }

    struct canboot_boot_file *f = &bi->files[bi->file_count];
    int i = 0;
    for (; aname[i] && i < (int)CANBOOT_BOOT_FILE_NAME - 1; i++)
        f->name[i] = aname[i];
    f->name[i] = '\0';
    f->addr = (uint64_t)addr;
    f->size = (uint64_t)rd;
    bi->file_count++;

    serial_write_early("canboot: boot file loaded: ");
    serial_write_early(aname);
    serial_write_early("\n");
    return 1;
}

/* Try the lowercase name first, then the UPPERCASE 8.3 form. ISO9660
 * stores names uppercased (INIT.CDO) and some firmware filesystem drivers
 * do a case-sensitive Open, so the lowercase attempt alone misses. The
 * canonical lowercase basename (aname) is what gets recorded either way. */
static void load_named_boot_file(EFI_FILE_HANDLE root, const char *aname,
                                 CHAR16 *lower, CHAR16 *upper,
                                 struct boot_info *bi) {
    if (load_one_boot_file(root, lower, aname, bi)) return;
    if (load_one_boot_file(root, upper, aname, bi)) return;
    serial_write_early("canboot: boot file absent: ");
    serial_write_early(aname);
    serial_write_early("\n");
}

static void load_boot_files(EFI_HANDLE image, EFI_SYSTEM_TABLE *st,
                            struct boot_info *bi) {
    bi->file_count = 0;
    serial_write_early("canboot: reading boot files from boot volume\n");

    EFI_GUID li_guid = LOADED_IMAGE_PROTOCOL;
    EFI_LOADED_IMAGE *li = NULL;
    EFI_STATUS s = uefi_call_wrapper(st->BootServices->HandleProtocol, 3,
                                     image, &li_guid, (void **)&li);
    if (EFI_ERROR(s) || li == NULL || li->DeviceHandle == NULL) {
        serial_write_early("canboot: boot files: no loaded-image device\n");
        return;
    }

    EFI_FILE_HANDLE root = LibOpenRoot(li->DeviceHandle);
    if (root == NULL) {
        serial_write_early("canboot: boot files: cannot open boot volume\n");
        return;
    }

    load_named_boot_file(root, "init.cdo",  L"init.cdo",  L"INIT.CDO",  bi);
    load_named_boot_file(root, "probe.png", L"probe.png", L"PROBE.PNG", bi);
    load_named_boot_file(root, "gui.cdo",   L"gui.cdo",   L"GUI.CDO",   bi);

    uefi_call_wrapper(root->Close, 1, root);
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    InitializeLib(image, st);

    serial_init_early();
    serial_write_early("canboot: uefi entry reached\n");

    Print(L"canboot: uefi loader; collecting boot_info\n");

    g_boot_info.magic = CANBOOT_BOOT_INFO_MAGIC;
    g_boot_info.version = CANBOOT_BOOT_INFO_VERSION;
    g_boot_info.boot_source = CANBOOT_BOOT_UEFI;

    populate_fb(st, &g_boot_info);
    populate_acpi(st, &g_boot_info);

    /* Read /init.cdo (+ probe.png, gui.cdo) off the boot volume now, while
     * BootServices + the firmware filesystem are still available. Done
     * before the memory map is fetched so the AllocatePages it does are
     * reflected in map_key. Best-effort: failures just leave file_count
     * at 0 and the kernel falls back to scanning real disks. */
    load_boot_files(image, st, &g_boot_info);

    /* Acquire memory map: probe, allocate, fetch. */
    UINTN map_size = 0, map_key = 0, desc_size = 0;
    UINT32 desc_version = 0;
    EFI_MEMORY_DESCRIPTOR *map = NULL;
    EFI_STATUS s = uefi_call_wrapper(st->BootServices->GetMemoryMap, 5,
                                     &map_size, map, &map_key,
                                     &desc_size, &desc_version);
    /* Expect EFI_BUFFER_TOO_SMALL. Add headroom for AllocatePool churn. */
    map_size += 8 * desc_size;
    s = uefi_call_wrapper(st->BootServices->AllocatePool, 3,
                          EfiLoaderData, map_size, (void **)&map);
    if (EFI_ERROR(s) || map == NULL) {
        serial_write_early("canboot: FATAL AllocatePool for mmap\n");
        for (;;) __asm__ volatile ("hlt");
    }
    s = uefi_call_wrapper(st->BootServices->GetMemoryMap, 5,
                          &map_size, map, &map_key, &desc_size, &desc_version);
    if (EFI_ERROR(s)) {
        serial_write_early("canboot: FATAL GetMemoryMap\n");
        for (;;) __asm__ volatile ("hlt");
    }

    populate_mmap(map, map_size, desc_size, &g_boot_info);

    serial_write_early("canboot: calling ExitBootServices\n");

    s = uefi_call_wrapper(st->BootServices->ExitBootServices, 2,
                          image, map_key);
    if (EFI_ERROR(s)) {
        /* Memory map may have shifted between GetMemoryMap and the
         * ExitBootServices call (UEFI spec allows this once). Retry. */
        s = uefi_call_wrapper(st->BootServices->GetMemoryMap, 5,
                              &map_size, map, &map_key,
                              &desc_size, &desc_version);
        if (!EFI_ERROR(s)) {
            populate_mmap(map, map_size, desc_size, &g_boot_info);
            s = uefi_call_wrapper(st->BootServices->ExitBootServices, 2,
                                  image, map_key);
        }
        if (EFI_ERROR(s)) {
            serial_write_early("canboot: FATAL ExitBootServices failed twice\n");
            for (;;) __asm__ volatile ("hlt");
        }
    }

    /* We own the machine now. Hand off to the unified kmain. */
    kmain(&g_boot_info);

    for (;;) __asm__ volatile ("hlt");
    return EFI_SUCCESS;
}
