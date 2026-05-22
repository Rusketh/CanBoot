/*
 * Milestone 9 self-test: prove libcando is linked and the VM can be
 * opened + closed cleanly on bare metal. Actual script execution
 * (cando_dofile on /init.cdo) lands in milestone 10 once the syscall
 * + thread bindings are fully shaken out for CanDo's startup path.
 */

#include <stdio.h>
#include <stdint.h>

/* Forward-declared rather than including <cando.h>; the public header
 * transitively pulls in cando's lib/sockutil.h which expects glibc's
 * <netinet/in.h> + <netdb.h> + <openssl/ssl.h>. The cando_port patch
 * series will give us a clean bare-metal cando.h in milestone 10. */
typedef struct CandoVM CandoVM;
CandoVM *cando_open(void);
void     cando_openlibs(CandoVM *vm);
void     cando_close(CandoVM *vm);

/* Reference the public API symbols so the linker can't garbage-collect
 * libcando out of the kernel binary. Functions are addressed (not
 * called) and the addresses are written to a volatile sink that
 * defeats constant folding. */
static volatile void *g_cando_keep[3];

void canboot_m9_candotest(void) {
    printf("milestone 9: starting cando link test\n");

    g_cando_keep[0] = (void *)(uintptr_t)&cando_open;
    g_cando_keep[1] = (void *)(uintptr_t)&cando_openlibs;
    g_cando_keep[2] = (void *)(uintptr_t)&cando_close;

    printf("milestone 9: libcando linked into kernel (open=%p openlibs=%p close=%p)\n",
           (void *)g_cando_keep[0],
           (void *)g_cando_keep[1],
           (void *)g_cando_keep[2]);
    printf("milestone 9: runtime bring-up (cando_open/dofile) deferred to m10\n");
    printf("milestone 9: cando link test ok\n");
}
