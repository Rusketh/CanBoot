/*
 * cando image module - decode and blit PNG / JPEG / BMP via the
 * vendored stb_image. Decoded images live in a small slot table; the
 * raw pixel buffer is owned by stb_image's allocator (picolibc malloc
 * under the hood) and freed by image.free or when the slot is reused.
 *
 *   h = image.decode(bytes)        opaque handle (-1 on failure)
 *   image.width(h)                 pixels
 *   image.height(h)                pixels
 *   image.draw(h, x, y)            blit RGBA -> framebuffer at (x, y)
 *   image.draw(h, x, y, w, h_)     scaled blit (nearest-neighbour)
 *   image.pixel(h, x, y)           sample an RGBA pixel (0xAARRGGBB)
 *   image.free(h)                  release backing buffer
 *
 * Pixel format on disk: stb_image returns 4-byte RGBA in row-major
 * order. We convert to canboot's 0x00RRGGBB at blit time.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "core/value.h"
#include "vm/vm.h"
#include "vm/bridge.h"
#include "object/object.h"
#include "object/string.h"
#include "lib/libutil.h"
#include "lib/object.h"

#include "hal/display.h"

/* stb_image declarations - we don't include the whole header here to
 * avoid pulling its big body into this TU. */
extern unsigned char *stbi_load_from_memory(const unsigned char *buf, int len,
                                             int *x, int *y, int *channels,
                                             int desired_channels);
extern void stbi_image_free(void *retval_from_stbi_load);

#define CANBOOT_IMAGE_SLOTS 8

struct canboot_image {
    unsigned char *rgba;
    int width;
    int height;
};

static struct canboot_image g_images[CANBOOT_IMAGE_SLOTS];

static int alloc_slot(void) {
    for (int i = 0; i < CANBOOT_IMAGE_SLOTS; i++) {
        if (g_images[i].rgba == NULL) return i;
    }
    return -1;
}

static struct canboot_image *get_slot(int handle) {
    if (handle < 0 || handle >= CANBOOT_IMAGE_SLOTS) return NULL;
    if (g_images[handle].rgba == NULL) return NULL;
    return &g_images[handle];
}

static uint32_t rgba_to_0rgb(uint32_t rgba_le) {
    /* rgba_le is the little-endian uint32_t reading of bytes
     * [R, G, B, A]: bit 0-7 = R, 8-15 = G, 16-23 = B, 24-31 = A.
     * canboot HAL wants 0x00RRGGBB. */
    uint32_t r = (rgba_le >>  0) & 0xff;
    uint32_t g = (rgba_le >>  8) & 0xff;
    uint32_t b = (rgba_le >> 16) & 0xff;
    return (r << 16) | (g << 8) | b;
}

static int f_decode(CandoVM *vm, int argc, CandoValue *args) {
    /* PNG / JPEG / BMP data is binary with embedded NULs, so we must
     * use the CandoString length field instead of strlen. */
    CandoString *str = libutil_arg_str_at(args, argc, 0);
    if (!str) { cando_vm_push(vm, cando_number(-1)); return 1; }
    int len = (int)str->length;
    int w = 0, h = 0, ch = 0;
    unsigned char *pix = stbi_load_from_memory((const unsigned char *)str->data,
                                               len, &w, &h, &ch, 4);
    if (!pix) { cando_vm_push(vm, cando_number(-1)); return 1; }
    int handle = alloc_slot();
    if (handle < 0) {
        stbi_image_free(pix);
        cando_vm_push(vm, cando_number(-1));
        return 1;
    }
    g_images[handle].rgba   = pix;
    g_images[handle].width  = w;
    g_images[handle].height = h;
    cando_vm_push(vm, cando_number((double)handle));
    return 1;
}

static int f_width(CandoVM *vm, int argc, CandoValue *args) {
    int h = (int)libutil_arg_num_at(args, argc, 0, -1);
    struct canboot_image *img = get_slot(h);
    cando_vm_push(vm, cando_number(img ? (double)img->width : 0.0));
    return 1;
}

static int f_height(CandoVM *vm, int argc, CandoValue *args) {
    int h = (int)libutil_arg_num_at(args, argc, 0, -1);
    struct canboot_image *img = get_slot(h);
    cando_vm_push(vm, cando_number(img ? (double)img->height : 0.0));
    return 1;
}

static int f_pixel(CandoVM *vm, int argc, CandoValue *args) {
    int h  = (int)libutil_arg_num_at(args, argc, 0, -1);
    int px = (int)libutil_arg_num_at(args, argc, 1, 0);
    int py = (int)libutil_arg_num_at(args, argc, 2, 0);
    struct canboot_image *img = get_slot(h);
    if (!img || px < 0 || py < 0 || px >= img->width || py >= img->height) {
        cando_vm_push(vm, cando_number(0.0));
        return 1;
    }
    const unsigned char *p = img->rgba + ((size_t)py * img->width + px) * 4;
    uint32_t rgba = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                  | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    cando_vm_push(vm, cando_number((double)rgba));
    return 1;
}

static int f_draw(CandoVM *vm, int argc, CandoValue *args) {
    int h  = (int)libutil_arg_num_at(args, argc, 0, -1);
    int dx = (int)libutil_arg_num_at(args, argc, 1, 0);
    int dy = (int)libutil_arg_num_at(args, argc, 2, 0);
    int dw = (int)libutil_arg_num_at(args, argc, 3, 0);
    int dh = (int)libutil_arg_num_at(args, argc, 4, 0);
    struct canboot_image *img = get_slot(h);
    if (!img) { cando_vm_push(vm, cando_bool(false)); return 1; }

    if (dw <= 0) dw = img->width;
    if (dh <= 0) dh = img->height;

    /* Convert RGBA->0RGB into a scratch line buffer and blit row by
     * row. Avoids allocating w*h*4 bytes for the entire destination
     * when scaling. */
    enum { LINE_MAX = 4096 };
    static uint32_t line[LINE_MAX];
    if (dw > LINE_MAX) dw = LINE_MAX;

    for (int row = 0; row < dh; row++) {
        int sy = (row * img->height) / dh;
        const unsigned char *src_row = img->rgba + (size_t)sy * img->width * 4;
        for (int col = 0; col < dw; col++) {
            int sx = (col * img->width) / dw;
            const unsigned char *p = src_row + sx * 4;
            uint32_t rgba = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                          | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
            line[col] = rgba_to_0rgb(rgba);
        }
        hal_display_image(dx, dy + row, dw, 1, line);
    }
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

static int f_free(CandoVM *vm, int argc, CandoValue *args) {
    int h = (int)libutil_arg_num_at(args, argc, 0, -1);
    struct canboot_image *img = get_slot(h);
    if (img) {
        stbi_image_free(img->rgba);
        img->rgba = NULL;
        img->width = 0;
        img->height = 0;
    }
    cando_vm_push(vm, cando_bool(img != NULL));
    return 1;
}

static const LibutilMethodEntry image_methods[] = {
    { "decode", f_decode },
    { "width",  f_width  },
    { "height", f_height },
    { "pixel",  f_pixel  },
    { "draw",   f_draw   },
    { "free",   f_free   },
};

void canboot_cando_open_imagelib(CandoVM *vm) {
    CandoValue obj_val = cando_bridge_new_object(vm);
    CdoObject *obj     = cando_bridge_resolve(vm, cando_as_handle(obj_val));
    libutil_register_methods(vm, obj, image_methods,
                             sizeof(image_methods) / sizeof(image_methods[0]));
    cando_vm_set_global(vm, "image", obj_val, true);
}
