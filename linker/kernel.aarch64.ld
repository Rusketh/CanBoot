/*
 * canboot kernel link map for aarch64.
 * QEMU virt machine loads ELF kernels via -kernel; the entry point is
 * picked up from the ELF header and the binary is copied into RAM
 * starting at the load address. We use 0x40080000 (QEMU virt's
 * standard kernel offset, 512 KiB above the 1 GiB RAM start).
 */

ENTRY(_start)
OUTPUT_FORMAT(elf64-littleaarch64)
OUTPUT_ARCH(aarch64)

KERNEL_LOAD_ADDR = 0x40080000;

SECTIONS
{
    . = KERNEL_LOAD_ADDR;

    __kernel_start = .;

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
        *(.gnu.build.attributes*)
    }
}
