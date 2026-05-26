#ifndef CANBOOT_SHIM_OPENSSL_ERR_H
#define CANBOOT_SHIM_OPENSSL_ERR_H
/* Bare-metal shim: the error-string surface cando's lib/sockutil.c uses
 * to render a TLS handshake failure reason. */

#include <stddef.h>

unsigned long ERR_peek_last_error(void);
void          ERR_error_string_n(unsigned long e, char *buf, size_t len);

#endif
