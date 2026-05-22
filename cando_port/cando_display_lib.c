/*
 * cando display module - binds the HAL display surface as
 * `display.clear`, `display.fillRect`, `display.pixel`, `display.line`,
 * `display.text`, `display.image`, `display.copyRect`, `display.width`,
 * `display.height`. Registered alongside cando_openlibs by
 * canboot_cando_open_displaylib() called from kmain after the VM is
 * brought up.
 */

#include <stdint.h>
#include <stddef.h>

#include "hal/display.h"

/* Cando types pulled in via its source tree; we use the iquote include
 * path from CMakeLists. */
#include "core/value.h"
#include "vm/vm.h"
#include "vm/bridge.h"
#include "object/object.h"
#include "lib/libutil.h"
#include "lib/object.h"

static int disp_clear(CandoVM *vm, int argc, CandoValue *args) {
    (void)vm;
    uint32_t col = (uint32_t)libutil_arg_num_at(args, argc, 0, 0);
    hal_display_clear(col);
    return 0;
}

static int disp_fill_rect(CandoVM *vm, int argc, CandoValue *args) {
    (void)vm;
    int32_t  x = (int32_t)libutil_arg_num_at(args, argc, 0, 0);
    int32_t  y = (int32_t)libutil_arg_num_at(args, argc, 1, 0);
    int32_t  w = (int32_t)libutil_arg_num_at(args, argc, 2, 0);
    int32_t  h = (int32_t)libutil_arg_num_at(args, argc, 3, 0);
    uint32_t c = (uint32_t)libutil_arg_num_at(args, argc, 4, 0);
    hal_display_fill_rect(x, y, w, h, c);
    return 0;
}

static int disp_pixel(CandoVM *vm, int argc, CandoValue *args) {
    (void)vm;
    int32_t  x = (int32_t)libutil_arg_num_at(args, argc, 0, 0);
    int32_t  y = (int32_t)libutil_arg_num_at(args, argc, 1, 0);
    uint32_t c = (uint32_t)libutil_arg_num_at(args, argc, 2, 0);
    hal_display_pixel(x, y, c);
    return 0;
}

static int disp_line(CandoVM *vm, int argc, CandoValue *args) {
    (void)vm;
    int32_t  x0 = (int32_t)libutil_arg_num_at(args, argc, 0, 0);
    int32_t  y0 = (int32_t)libutil_arg_num_at(args, argc, 1, 0);
    int32_t  x1 = (int32_t)libutil_arg_num_at(args, argc, 2, 0);
    int32_t  y1 = (int32_t)libutil_arg_num_at(args, argc, 3, 0);
    uint32_t c  = (uint32_t)libutil_arg_num_at(args, argc, 4, 0);
    hal_display_line(x0, y0, x1, y1, c);
    return 0;
}

static int disp_text(CandoVM *vm, int argc, CandoValue *args) {
    (void)vm;
    int32_t  x  = (int32_t)libutil_arg_num_at(args, argc, 0, 0);
    int32_t  y  = (int32_t)libutil_arg_num_at(args, argc, 1, 0);
    const char *s = libutil_arg_cstr_at(args, argc, 2);
    uint32_t fg = (uint32_t)libutil_arg_num_at(args, argc, 3, 0xFFFFFFu);
    uint32_t bg = (uint32_t)libutil_arg_num_at(args, argc, 4, 0u);
    if (s) hal_display_text(x, y, s, fg, bg);
    return 0;
}

static int disp_copy_rect(CandoVM *vm, int argc, CandoValue *args) {
    (void)vm;
    int32_t sx = (int32_t)libutil_arg_num_at(args, argc, 0, 0);
    int32_t sy = (int32_t)libutil_arg_num_at(args, argc, 1, 0);
    int32_t dx = (int32_t)libutil_arg_num_at(args, argc, 2, 0);
    int32_t dy = (int32_t)libutil_arg_num_at(args, argc, 3, 0);
    int32_t w  = (int32_t)libutil_arg_num_at(args, argc, 4, 0);
    int32_t h  = (int32_t)libutil_arg_num_at(args, argc, 5, 0);
    hal_display_copy_rect(sx, sy, dx, dy, w, h);
    return 0;
}

static int disp_get_pixel(CandoVM *vm, int argc, CandoValue *args) {
    int32_t x = (int32_t)libutil_arg_num_at(args, argc, 0, 0);
    int32_t y = (int32_t)libutil_arg_num_at(args, argc, 1, 0);
    cando_vm_push(vm, cando_number((f64)hal_display_get_pixel(x, y)));
    return 1;
}

static int disp_width(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    cando_vm_push(vm, cando_number((f64)hal_display_width()));
    return 1;
}

static int disp_height(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    cando_vm_push(vm, cando_number((f64)hal_display_height()));
    return 1;
}

static const LibutilMethodEntry display_methods[] = {
    { "clear",    disp_clear     },
    { "fillRect", disp_fill_rect },
    { "pixel",    disp_pixel     },
    { "line",     disp_line      },
    { "text",     disp_text      },
    { "image",    disp_copy_rect }, /* image-from-buffer lands when we have arrays-of-numbers wired */
    { "copyRect", disp_copy_rect },
    { "getPixel", disp_get_pixel },
    { "width",    disp_width     },
    { "height",   disp_height    },
};

void canboot_cando_open_displaylib(CandoVM *vm) {
    CandoValue obj_val = cando_bridge_new_object(vm);
    CdoObject *obj     = cando_bridge_resolve(vm, cando_as_handle(obj_val));
    libutil_register_methods(vm, obj, display_methods,
                             sizeof(display_methods) / sizeof(display_methods[0]));
    cando_vm_set_global(vm, "display", obj_val, true);
}
