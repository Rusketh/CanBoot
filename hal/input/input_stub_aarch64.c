/*
 * Stub hal_input surface for aarch64 m5. The picolibc syscalls glue
 * (rt/picolibc_port/syscalls.c) calls hal_input_getc() and
 * hal_input_pump() from its read() implementation, so those symbols
 * have to resolve even on the aarch64 build before the m4-equivalent
 * input drivers land.
 *
 * hal_input_getc() returns -1 to signal "no character available". The
 * milestone-5 self-test doesn't read stdin, so this stub is enough to
 * link and run.
 */

#include <stdbool.h>

#include "hal/input.h"

void hal_input_init(void) { }
void hal_input_pump(void) { }
bool hal_input_poll(struct canboot_event *out) { (void)out; return false; }
int  hal_input_getc(void) { return -1; }
