/*
 * cando fb module - framebuffer / virtio-gpu helpers that don't fit
 * cleanly under display.* (which is the painter API).
 *
 *   fb.flush()       push the current backing buffer to the host
 *                    scanout. No-op when running on firmware-supplied
 *                    GOP framebuffers (which the firmware refreshes
 *                    on its own); useful on the aarch64 path where we
 *                    drive virtio-gpu directly and have to issue
 *                    TRANSFER_TO_HOST_2D + RESOURCE_FLUSH explicitly.
 *   fb.present()     alias for flush()
 *
 * Wired only when the virtio-gpu driver has been initialised; the
 * stub case (firmware fb) makes flush a cheap no-op.
 */

#include <stdint.h>
#include <stddef.h>

#include "core/value.h"
#include "vm/vm.h"
#include "vm/bridge.h"
#include "object/object.h"
#include "lib/libutil.h"
#include "lib/object.h"

#if CANBOOT_AARCH64_EFI_BUILD
extern void canboot_virtio_gpu_flush(void);
#endif

static int f_flush(CandoVM *vm, int argc, CandoValue *args) {
    (void)vm; (void)argc; (void)args;
#if CANBOOT_AARCH64_EFI_BUILD
    canboot_virtio_gpu_flush();
#endif
    return 0;
}

static const LibutilMethodEntry fb_methods[] = {
    { "flush",   f_flush },
    { "present", f_flush },
};

void canboot_cando_open_fblib(CandoVM *vm) {
    CandoValue obj_val = cando_bridge_new_object(vm);
    CdoObject *obj     = cando_bridge_resolve(vm, cando_as_handle(obj_val));
    libutil_register_methods(vm, obj, fb_methods,
                             sizeof(fb_methods) / sizeof(fb_methods[0]));
    cando_vm_set_global(vm, "fb", obj_val, true);
}
