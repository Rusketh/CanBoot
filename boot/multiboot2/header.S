/*
 * Multiboot2 header for canboot.
 * Magic + i386 architecture + a single end tag. GRUB locates this in the
 * first 32 KiB of the kernel ELF and hands off in 32-bit protected mode.
 */

#define MB2_MAGIC        0xe85250d6
#define MB2_ARCH_I386    0

.section .multiboot2, "a", @progbits
.align 8

mb2_header_start:
    .long MB2_MAGIC
    .long MB2_ARCH_I386
    .long mb2_header_end - mb2_header_start
    .long -(MB2_MAGIC + MB2_ARCH_I386 + (mb2_header_end - mb2_header_start))

    /* End tag */
    .align 8
    .short 0
    .short 0
    .long  8
mb2_header_end:
