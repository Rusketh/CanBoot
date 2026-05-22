/*
 * Milestone 9 self-test: prove libcando is linked and the VM can be
 * opened + closed cleanly on bare metal. Actual script execution
 * (cando_dofile on /init.cdo) lands in milestone 10 once the syscall
 * + thread bindings are fully shaken out for CanDo's startup path.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "hal/disk.h"
#include "fs/fat32.h"
#include "fs/iso9660.h"

/* Forward-declared rather than including <cando.h>; the public header
 * transitively pulls in cando's lib/sockutil.h which expects glibc's
 * <netinet/in.h> + <netdb.h> + <openssl/ssl.h>. */
typedef struct CandoVM CandoVM;
CandoVM    *cando_open(void);
void        cando_openlibs(CandoVM *vm);
void        cando_close(CandoVM *vm);
int         cando_dostring(CandoVM *vm, const char *src, const char *name);
const char *cando_errmsg(CandoVM *vm);
void        canboot_cando_open_displaylib(CandoVM *vm);
void        canboot_cando_open_inputlib(CandoVM *vm);
void        canboot_cando_open_timelib(CandoVM *vm);
void        canboot_cando_open_filelib(CandoVM *vm);
void        canboot_cando_open_netlib(CandoVM *vm);
void        canboot_cando_open_tlslib(CandoVM *vm);

#include "hal/display.h"
#include "hal/input.h"

static int load_init_cdo(char *out, uint32_t out_size, uint32_t *out_len) {
    uint32_t nd = hal_disk_count();
    /* Prefer FAT32 on writable disk. */
    for (uint32_t i = 0; i < nd; i++) {
        struct canboot_disk *d = hal_disk_get(i);
        if (d->kind == CANBOOT_DISK_KIND_CDROM) continue;
        struct canboot_fat32 fs;
        if (!canboot_fat32_open(d, &fs)) continue;
        if (canboot_fat32_read_root_file(&fs, "init.cdo",
                                          out, out_size, out_len) > 0) {
            out[*out_len < out_size ? *out_len : out_size - 1] = '\0';
            return 0;
        }
    }
    /* ISO9660 fallback. */
    for (uint32_t i = 0; i < nd; i++) {
        struct canboot_disk *d = hal_disk_get(i);
        struct canboot_iso iso;
        if (!canboot_iso_open(d, &iso)) continue;
        uint32_t lba = 0, size = 0;
        if (canboot_iso_lookup(&iso, "init.cdo", &lba, &size) &&
            canboot_iso_read_file(&iso, lba, size, out, out_size) > 0) {
            *out_len = size;
            out[size < out_size ? size : out_size - 1] = '\0';
            return 0;
        }
    }
    return -1;
}

void canboot_m9_candotest(void) {
    printf("milestone 9: starting cando link test\n");

    printf("milestone 9: addr open=%p openlibs=%p close=%p\n",
           (void *)(uintptr_t)&cando_open,
           (void *)(uintptr_t)&cando_openlibs,
           (void *)(uintptr_t)&cando_close);

    printf("milestone 9: calling cando_open\n");
    CandoVM *vm = cando_open();
    if (!vm) {
        printf("milestone 9: FAIL cando_open returned NULL\n");
        return;
    }
    printf("milestone 9: cando_open ok vm=%p\n", (void *)vm);

    printf("milestone 9: calling cando_openlibs\n");
    cando_openlibs(vm);
    printf("milestone 9: cando_openlibs ok\n");

    canboot_cando_open_displaylib(vm);
    printf("milestone 11: display lib registered (%dx%d)\n",
           hal_display_width(), hal_display_height());

    canboot_cando_open_inputlib(vm);
    printf("milestone 12: input lib registered\n");

    /* Milestone 13: system libs - clock, file, net, tls. */
    canboot_cando_open_timelib(vm);
    canboot_cando_open_filelib(vm);
    canboot_cando_open_netlib(vm);
    canboot_cando_open_tlslib(vm);
    printf("milestone 13: system libs registered (time/file/net/tls)\n");

    /* Milestone 10: load /init.cdo from disk and run it through cando_dostring. */
    static char init_src[8192];
    uint32_t init_len = 0;
    if (load_init_cdo(init_src, sizeof(init_src), &init_len) != 0) {
        printf("milestone 10: FAIL could not load /init.cdo\n");
        cando_close(vm);
        return;
    }
    printf("milestone 10: loaded /init.cdo (%u bytes)\n", (unsigned)init_len);

    printf("milestone 10: --- init.cdo output begin ---\n");
    int rc = cando_dostring(vm, init_src, "init.cdo");
    printf("milestone 10: --- init.cdo output end ---\n");
    if (rc != 0) {
        const char *err = cando_errmsg(vm);
        printf("milestone 10: FAIL cando_dostring rc=%d err=%s\n",
               rc, err ? err : "(none)");
        cando_close(vm);
        return;
    }
    printf("milestone 10: cando_dostring ok rc=%d\n", rc);

    /* Milestone 11: assert known pixel colours match what init.cdo painted.
     * The script paints three rects + a line + a text label; we sample
     * three points and verify the colour matches. */
    struct {
        int32_t x, y;
        uint32_t want;
        const char *what;
    } probes[] = {
        {  10,   10, 0x00FF0000u, "red top-left rect"    },
        { 160,   60, 0x0000FF00u, "green middle rect"    },
        { 310,  110, 0x000000FFu, "blue bottom-right rect" },
    };
    int probe_fails = 0;
    for (size_t i = 0; i < sizeof(probes)/sizeof(probes[0]); i++) {
        uint32_t got = hal_display_get_pixel(probes[i].x, probes[i].y);
        if (got == probes[i].want) {
            printf("milestone 11: probe %s @ (%d,%d) ok = 0x%06x\n",
                   probes[i].what, probes[i].x, probes[i].y, (unsigned)got);
        } else {
            printf("milestone 11: FAIL probe %s @ (%d,%d) got=0x%06x want=0x%06x\n",
                   probes[i].what, probes[i].x, probes[i].y,
                   (unsigned)got, (unsigned)probes[i].want);
            probe_fails++;
        }
    }
    if (probe_fails == 0) {
        printf("milestone 11: display test ok\n");
    }

    printf("milestone 9: calling cando_close\n");
    cando_close(vm);
    printf("milestone 9: cando_close ok\n");

    printf("milestone 9: cando link test ok\n");
    printf("milestone 10: init.cdo executed ok\n");
}
