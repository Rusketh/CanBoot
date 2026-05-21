/*
 * UEFI entry point. Milestone 2 scope: prove the PE/COFF image loads under
 * OVMF, print a banner via SimpleTextOutput, mirror "ok" to COM1 so the
 * smoke test (which only watches the serial port) can detect success, and
 * spin. The unified handoff to kmain() lands in milestone 3.
 */

#include <efi.h>
#include <efilib.h>

#define COM1 0x3F8u

static inline void outb(unsigned short port, unsigned char val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline unsigned char inb(unsigned short port) {
    unsigned char ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

static void serial_putc(char c) {
    while ((inb(COM1 + 5) & 0x20) == 0) { }
    outb(COM1, (unsigned char)c);
}

static void serial_write(const char *s) {
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}

static void serial_write_wide(const CHAR16 *s) {
    while (s && *s) {
        CHAR16 c = *s++;
        serial_putc((char)(c & 0x7F));
    }
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    InitializeLib(image, st);

    serial_init();
    serial_write("canboot: uefi entry reached\n");

    Print(L"canboot: uefi hello\n");

    serial_write("canboot: firmware vendor = ");
    serial_write_wide(st->FirmwareVendor);
    serial_write("\n");

    serial_write("canboot: uefi revision   = ");
    {
        char buf[16];
        unsigned int rev = (unsigned int)st->Hdr.Revision;
        unsigned int major = rev >> 16;
        unsigned int minor = rev & 0xFFFF;
        int i = 0;
        if (major == 0) buf[i++] = '0';
        else {
            char tmp[8]; int n = 0;
            while (major) { tmp[n++] = '0' + (major % 10); major /= 10; }
            while (n--) buf[i++] = tmp[n];
        }
        buf[i++] = '.';
        if (minor == 0) buf[i++] = '0';
        else {
            char tmp[8]; int n = 0;
            while (minor) { tmp[n++] = '0' + (minor % 10); minor /= 10; }
            while (n--) buf[i++] = tmp[n];
        }
        buf[i] = '\0';
        serial_write(buf);
    }
    serial_write("\n");

    serial_write("canboot: uefi handshake confirmed\n");
    serial_write("ok\n");

    for (;;) {
        __asm__ volatile ("hlt");
    }
    return EFI_SUCCESS;
}
