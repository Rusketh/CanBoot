/*
 * canboot kernel link map for x86_64 Multiboot2.
 * Loaded at 1 MiB physical; identity-mapped during bootstrap so the same
 * virtual addresses are usable after long-mode paging is enabled.
 */

ENTRY(_start)
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)

KERNEL_LOAD_ADDR = 0x100000;

SECTIONS
{
    . = KERNEL_LOAD_ADDR;

    __kernel_start = .;

    .multiboot2 ALIGN(8) : {
        KEEP(*(.multiboot2))
    }

    .text ALIGN(4K) : {
        *(.text.boot)
        *(.text .text.*)
    }

    .rodata ALIGN(4K) : {
        *(.rodata .rodata.*)
    }

    .data ALIGN(4K) : {
        *(.data .data.*)
    }

    .bss ALIGN(4K) (NOLOAD) : {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        . = ALIGN(4K);
        __bss_end = .;
    }

    __kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame)
        *(.eh_frame_hdr)
    }
}
