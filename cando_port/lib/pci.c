/*
 * cando pci module - read-only PCI introspection.
 *
 *   pci.count()         number of enumerated functions
 *   pci.vendor(i)       hex vendor id ("0x1af4" etc.)
 *   pci.device(i)       hex device id
 *   pci.class(i)        class:subclass:prog_if ("01:00:00")
 *   pci.address(i)      "00:01.0" style bus:dev.func
 *   pci.list()          one device per line: "<addr> <ven>:<dev> class=<cs>"
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include "hal/pci.h"

#include "core/value.h"
#include "vm/vm.h"
#include "vm/bridge.h"
#include "object/object.h"
#include "object/string.h"
#include "lib/libutil.h"
#include "lib/object.h"

static const struct canboot_pci_dev *dev_at(uint32_t i) {
    if (i >= hal_pci_devcount()) return NULL;
    return &hal_pci_devs()[i];
}

static int p_count(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    cando_vm_push(vm, cando_number((f64)hal_pci_devcount()));
    return 1;
}

static int hex16_push(CandoVM *vm, uint16_t v) {
    char buf[8];
    int n = snprintf(buf, sizeof(buf), "0x%04x", v);
    if (n < 0) n = 0;
    CandoString *s = cando_string_new(buf, (uint32_t)n);
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static int p_vendor(CandoVM *vm, int argc, CandoValue *args) {
    const struct canboot_pci_dev *d = dev_at((uint32_t)libutil_arg_num_at(args, argc, 0, 0));
    if (!d) { cando_vm_push(vm, cando_null()); return 1; }
    return hex16_push(vm, d->vendor);
}

static int p_device(CandoVM *vm, int argc, CandoValue *args) {
    const struct canboot_pci_dev *d = dev_at((uint32_t)libutil_arg_num_at(args, argc, 0, 0));
    if (!d) { cando_vm_push(vm, cando_null()); return 1; }
    return hex16_push(vm, d->device);
}

static int p_class(CandoVM *vm, int argc, CandoValue *args) {
    const struct canboot_pci_dev *d = dev_at((uint32_t)libutil_arg_num_at(args, argc, 0, 0));
    if (!d) { cando_vm_push(vm, cando_null()); return 1; }
    char buf[12];
    int n = snprintf(buf, sizeof(buf), "%02x:%02x:%02x",
                     d->class_code, d->subclass, d->prog_if);
    if (n < 0) n = 0;
    CandoString *s = cando_string_new(buf, (uint32_t)n);
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static int p_address(CandoVM *vm, int argc, CandoValue *args) {
    const struct canboot_pci_dev *d = dev_at((uint32_t)libutil_arg_num_at(args, argc, 0, 0));
    if (!d) { cando_vm_push(vm, cando_null()); return 1; }
    char buf[12];
    int n = snprintf(buf, sizeof(buf), "%02x:%02x.%x",
                     d->addr.bus, d->addr.dev, d->addr.func);
    if (n < 0) n = 0;
    CandoString *s = cando_string_new(buf, (uint32_t)n);
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static int p_list(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    static char out[4096];
    int o = 0;
    uint32_t n = hal_pci_devcount();
    const struct canboot_pci_dev *list = hal_pci_devs();
    for (uint32_t i = 0; i < n && (size_t)o < sizeof(out) - 64; i++) {
        const struct canboot_pci_dev *d = &list[i];
        int w = snprintf(out + o, sizeof(out) - o,
                         "%02x:%02x.%x %04x:%04x class=%02x:%02x:%02x\n",
                         d->addr.bus, d->addr.dev, d->addr.func,
                         d->vendor, d->device,
                         d->class_code, d->subclass, d->prog_if);
        if (w < 0) break;
        o += w;
    }
    if (o > 0 && out[o - 1] == '\n') o--;
    CandoString *s = cando_string_new(out, (uint32_t)o);
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static const LibutilMethodEntry pci_methods[] = {
    { "count",   p_count   },
    { "vendor",  p_vendor  },
    { "device",  p_device  },
    { "class",   p_class   },
    { "address", p_address },
    { "list",    p_list    },
};

void canboot_cando_open_pcilib(CandoVM *vm) {
    CandoValue obj_val = cando_bridge_new_object(vm);
    CdoObject *obj     = cando_bridge_resolve(vm, cando_as_handle(obj_val));
    libutil_register_methods(vm, obj, pci_methods,
                             sizeof(pci_methods) / sizeof(pci_methods[0]));
    cando_vm_set_global(vm, "pci", obj_val, true);
}
