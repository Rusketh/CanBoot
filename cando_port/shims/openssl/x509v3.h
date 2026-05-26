#ifndef CANBOOT_SHIM_OPENSSL_X509V3_H
#define CANBOOT_SHIM_OPENSSL_X509V3_H
/* Bare-metal shim: hostname-verification parameter surface used by
 * cando's lib/sockutil.c for client-side SNI / name checking. */

#include <openssl/ssl.h>

#define X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS 0x4

void X509_VERIFY_PARAM_set_hostflags(X509_VERIFY_PARAM *param,
                                     unsigned int flags);
int  X509_VERIFY_PARAM_set1_host(X509_VERIFY_PARAM *param,
                                 const char *name, size_t namelen);

#endif
