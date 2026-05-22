/*
 * virtio-gpu driver for aarch64 UEFI (and any other firmware that
 * doesn't produce GOP itself). Initialises the device, allocates a
 * 32-bit RGB scanout framebuffer in kernel memory, and exposes its
 * descriptor through canboot_virtio_gpu_init so kmain can patch
 * boot_info.fb when the firmware-supplied fb is absent.
 *
 * Synchronous command path on controlq (idx 0). Two-descriptor chain
 * per command: desc[0] = readable request, desc[1] = writable response.
 * We poll the used ring after each kick.
 *
 * Resource model: one host-side 2D resource (id=1), backed by guest
 * memory we own, presented on scanout 0. Each call to
 * canboot_virtio_gpu_flush() issues TRANSFER_TO_HOST_2D +
 * RESOURCE_FLUSH so anything painted into the buffer becomes visible.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "canboot/boot_info.h"
#include "hal/virtio.h"
#include "hal/console.h"

#define VIRTIO_PCI_GPU 0x1050u

#define CTRLQ_IDX 0u
#define CTRLQ_DEPTH 8u

#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO     0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D   0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF       0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT          0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH       0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D  0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106

#define VIRTIO_GPU_RESP_OK_NODATA           0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO     0x1101

#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM    2

#define VIRTIO_GPU_MAX_SCANOUTS 16

struct __attribute__((packed)) gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
};

struct __attribute__((packed)) gpu_rect {
    uint32_t x, y, width, height;
};

struct __attribute__((packed)) gpu_resp_display_info {
    struct gpu_ctrl_hdr hdr;
    struct {
        struct gpu_rect r;
        uint32_t enabled;
        uint32_t flags;
    } pmodes[VIRTIO_GPU_MAX_SCANOUTS];
};

struct __attribute__((packed)) gpu_resource_create_2d {
    struct gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
};

struct __attribute__((packed)) gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
};

struct __attribute__((packed)) gpu_resource_attach_backing {
    struct gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    /* followed by nr_entries gpu_mem_entry */
    struct gpu_mem_entry entry0;
};

struct __attribute__((packed)) gpu_set_scanout {
    struct gpu_ctrl_hdr hdr;
    struct gpu_rect     r;
    uint32_t            scanout_id;
    uint32_t            resource_id;
};

struct __attribute__((packed)) gpu_resource_flush {
    struct gpu_ctrl_hdr hdr;
    struct gpu_rect     r;
    uint32_t            resource_id;
    uint32_t            padding;
};

struct __attribute__((packed)) gpu_transfer_to_host_2d {
    struct gpu_ctrl_hdr hdr;
    struct gpu_rect     r;
    uint64_t            offset;
    uint32_t            resource_id;
    uint32_t            padding;
};

static struct canboot_virtio_dev g_dev;
static struct canboot_virtq      g_ctrlq;

static struct canboot_virtq_desc  g_desc [CANBOOT_VIRTQ_SIZE]
    __attribute__((aligned(16)));
static struct canboot_virtq_avail g_avail
    __attribute__((aligned(2)));
static struct canboot_virtq_used  g_used
    __attribute__((aligned(4)));

/* Command staging buffers. Largest request is gpu_resource_attach_backing,
 * largest response is gpu_resp_display_info. */
static uint8_t g_cmdbuf [256]  __attribute__((aligned(16)));
static uint8_t g_respbuf[512]  __attribute__((aligned(16)));

/* Default framebuffer: 1024x768x4 = 3 MiB. .bss-resident so it's
 * already mapped by AAVMF's identity page tables. */
#define GPU_FB_W 1024u
#define GPU_FB_H 768u
static uint8_t g_fb[GPU_FB_W * GPU_FB_H * 4u] __attribute__((aligned(4096)));

static bool g_ready;
static uint32_t g_w, g_h;

static int submit_cmd(uint32_t cmd_len, uint32_t resp_len) {
    /* desc[0] = readable command, desc[1] = writable response. */
    g_desc[0].addr  = (uint64_t)(uintptr_t)g_cmdbuf;
    g_desc[0].len   = cmd_len;
    g_desc[0].flags = CANBOOT_VIRTQ_DESC_F_NEXT;
    g_desc[0].next  = 1;
    g_desc[1].addr  = (uint64_t)(uintptr_t)g_respbuf;
    g_desc[1].len   = resp_len;
    g_desc[1].flags = CANBOOT_VIRTQ_DESC_F_WRITE;
    g_desc[1].next  = 0;

    uint16_t avail_idx = g_avail.idx;
    g_avail.ring[avail_idx % g_ctrlq.size] = 0;   /* head desc id */
    __asm__ volatile ("dmb sy" ::: "memory");
    g_avail.idx = avail_idx + 1;
    __asm__ volatile ("dmb sy" ::: "memory");

    canboot_virtq_kick(&g_ctrlq, CTRLQ_IDX);

    /* Poll used until our submission completes. ~1 second timeout
     * via a generous spin count; virtio-gpu commands are tiny so
     * this should resolve in microseconds. */
    for (uint64_t spins = 0; spins < 10ull * 1000ull * 1000ull; spins++) {
        __asm__ volatile ("dmb sy" ::: "memory");
        if (g_used.idx != g_ctrlq.last_used_idx) {
            g_ctrlq.last_used_idx = g_used.idx;
            return 0;
        }
        __asm__ volatile ("yield");
    }
    hal_console_write("virtio-gpu: command timeout\n");
    return -1;
}

static bool resp_ok(uint32_t expected_type) {
    struct gpu_ctrl_hdr *h = (struct gpu_ctrl_hdr *)g_respbuf;
    return h->type == expected_type;
}

bool canboot_virtio_gpu_init(struct canboot_fb *out_fb) {
    if (!canboot_virtio_find(VIRTIO_PCI_GPU, &g_dev)) {
        return false;
    }
    if (!canboot_virtio_negotiate(&g_dev, 0)) return false;
    if (!canboot_virtio_queue_setup(&g_dev, CTRLQ_IDX, &g_ctrlq,
                                    g_desc, &g_avail, &g_used)) {
        return false;
    }
    if (!canboot_virtio_run(&g_dev)) return false;

    /* GET_DISPLAY_INFO */
    memset(g_cmdbuf, 0, sizeof(g_cmdbuf));
    struct gpu_ctrl_hdr *get = (struct gpu_ctrl_hdr *)g_cmdbuf;
    get->type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
    if (submit_cmd(sizeof(*get),
                   sizeof(struct gpu_resp_display_info)) != 0) return false;
    if (!resp_ok(VIRTIO_GPU_RESP_OK_DISPLAY_INFO)) {
        hal_console_write("virtio-gpu: GET_DISPLAY_INFO unexpected resp\n");
        return false;
    }
    struct gpu_resp_display_info *info =
        (struct gpu_resp_display_info *)g_respbuf;
    g_w = info->pmodes[0].r.width  ? info->pmodes[0].r.width  : GPU_FB_W;
    g_h = info->pmodes[0].r.height ? info->pmodes[0].r.height : GPU_FB_H;
    if (g_w > GPU_FB_W) g_w = GPU_FB_W;
    if (g_h > GPU_FB_H) g_h = GPU_FB_H;

    /* RESOURCE_CREATE_2D resource_id=1 */
    memset(g_cmdbuf, 0, sizeof(g_cmdbuf));
    struct gpu_resource_create_2d *cr =
        (struct gpu_resource_create_2d *)g_cmdbuf;
    cr->hdr.type     = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    cr->resource_id  = 1;
    cr->format       = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    cr->width        = g_w;
    cr->height       = g_h;
    if (submit_cmd(sizeof(*cr), sizeof(struct gpu_ctrl_hdr)) != 0) return false;
    if (!resp_ok(VIRTIO_GPU_RESP_OK_NODATA)) {
        hal_console_write("virtio-gpu: CREATE_2D failed\n");
        return false;
    }

    /* RESOURCE_ATTACH_BACKING with our g_fb pages. */
    memset(g_cmdbuf, 0, sizeof(g_cmdbuf));
    struct gpu_resource_attach_backing *ab =
        (struct gpu_resource_attach_backing *)g_cmdbuf;
    ab->hdr.type      = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    ab->resource_id   = 1;
    ab->nr_entries    = 1;
    ab->entry0.addr   = (uint64_t)(uintptr_t)g_fb;
    ab->entry0.length = g_w * g_h * 4u;
    if (submit_cmd(sizeof(*ab), sizeof(struct gpu_ctrl_hdr)) != 0) return false;
    if (!resp_ok(VIRTIO_GPU_RESP_OK_NODATA)) {
        hal_console_write("virtio-gpu: ATTACH_BACKING failed\n");
        return false;
    }

    /* SET_SCANOUT scanout_id=0 -> resource_id=1 */
    memset(g_cmdbuf, 0, sizeof(g_cmdbuf));
    struct gpu_set_scanout *ss = (struct gpu_set_scanout *)g_cmdbuf;
    ss->hdr.type     = VIRTIO_GPU_CMD_SET_SCANOUT;
    ss->r.width      = g_w;
    ss->r.height     = g_h;
    ss->scanout_id   = 0;
    ss->resource_id  = 1;
    if (submit_cmd(sizeof(*ss), sizeof(struct gpu_ctrl_hdr)) != 0) return false;
    if (!resp_ok(VIRTIO_GPU_RESP_OK_NODATA)) {
        hal_console_write("virtio-gpu: SET_SCANOUT failed\n");
        return false;
    }

    out_fb->addr             = (uint64_t)(uintptr_t)g_fb;
    out_fb->width            = g_w;
    out_fb->height           = g_h;
    out_fb->pitch            = g_w * 4u;
    out_fb->bpp              = 32;
    out_fb->format           = CANBOOT_FB_RGB;
    /* B8G8R8X8: blue at byte 0 -> shift 0, green at byte 1 -> shift 8,
     * red at byte 2 -> shift 16. */
    out_fb->blue_mask_shift  = 0;  out_fb->blue_mask_size  = 8;
    out_fb->green_mask_shift = 8;  out_fb->green_mask_size = 8;
    out_fb->red_mask_shift   = 16; out_fb->red_mask_size   = 8;

    g_ready = true;
    return true;
}

void canboot_virtio_gpu_flush(void) {
    if (!g_ready) return;

    memset(g_cmdbuf, 0, sizeof(g_cmdbuf));
    struct gpu_transfer_to_host_2d *t =
        (struct gpu_transfer_to_host_2d *)g_cmdbuf;
    t->hdr.type    = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    t->r.width     = g_w;
    t->r.height    = g_h;
    t->resource_id = 1;
    (void)submit_cmd(sizeof(*t), sizeof(struct gpu_ctrl_hdr));

    memset(g_cmdbuf, 0, sizeof(g_cmdbuf));
    struct gpu_resource_flush *f = (struct gpu_resource_flush *)g_cmdbuf;
    f->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    f->r.width     = g_w;
    f->r.height    = g_h;
    f->resource_id = 1;
    (void)submit_cmd(sizeof(*f), sizeof(struct gpu_ctrl_hdr));
}
