#ifndef CANBOOT_HAL_CONSOLE_H
#define CANBOOT_HAL_CONSOLE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Early console used during bring-up before the framebuffer/text console
 * subsystems are wired up. Backed by a platform serial port (16550 UART on
 * x86_64, PL011 on aarch64). Later milestones add a richer hal_console_*
 * surface for cursor/colour/raw-input/mouse that CanDo's `console` module
 * binds to.
 */

void hal_console_init(void);
void hal_console_putc(char c);
void hal_console_write(const char *s);
void hal_console_write_n(const char *s, size_t n);

#endif /* CANBOOT_HAL_CONSOLE_H */
