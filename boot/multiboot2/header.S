/*
 * Multiboot2 header for canboot.
 *
 * Magic + i386 architecture + a framebuffer tag (optional: GRUB falls
 * back to text mode if it can't satisfy 1024x768x32) + end tag. GRUB
 * locates this in the first 32 KiB of the kernel ELF and hands off in
 * 32-bit protected mode.
 */

#define MB2_MAGIC            0xe85250d6
#define MB2_ARCH_I386        0
#define MB2_TAG_FRAMEBUFFER  5
#define MB2_TAG_FLAG_OPT     1

.section .multiboot2, "a", @progbits
.align 8

mb2_header_start:
    .long MB2_MAGIC
    .long MB2_ARCH_I386
    .long mb2_header_end - mb2_header_start
    .long -(MB2_MAGIC + MB2_ARCH_I386 + (mb2_header_end - mb2_header_start))

    /* Framebuffer request: 1024x768x32, optional */
    .align 8
fb_tag_start:
    .short MB2_TAG_FRAMEBUFFER
    .short MB2_TAG_FLAG_OPT
    .long  fb_tag_end - fb_tag_start
    .long  1024
    .long  768
    .long  32
fb_tag_end:

    /* End tag */
    .align 8
    .short 0
    .short 0
    .long  8
mb2_header_end:
