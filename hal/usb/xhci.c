/*
 * Polled xHCI (USB 3) host-controller driver + universal USB class
 * drivers:
 *
 *   - USB-HID boot keyboard / mouse (interrupt IN), and
 *   - USB Mass Storage class plumbing (bulk IN + bulk OUT), consumed by
 *     hal/disk/usb_storage.c which layers SCSI Bulk-Only Transport on top.
 *
 * Both follow the "works on anything" universal contract a firmware/OS
 * basic driver relies on: HID via the boot protocol, mass storage via the
 * standard bulk-only class with SCSI transparent commands - so any
 * compliant keyboard, mouse/touchpad, or USB disk binds with one code
 * path regardless of vendor.
 *
 * The controller is brought up once (idempotently) and *every* connected
 * root-hub port is enumerated (Enable Slot -> Address Device -> Get/Set
 * descriptors -> Configure Endpoint), each device classified from its
 * interface class/protocol. Up to MAX_DEV devices bind simultaneously, so
 * a laptop exposing a keyboard + touchpad, or a keyboard + a USB stick,
 * gets all of them. No hubs, no hot-plug.
 *
 * No interrupts: the single shared event ring is polled. Two flows touch
 * it after boot - the HAL input pump and synchronous disk bulk transfers -
 * so all event-ring access is serialised under g_evt_lock (irqsave: the
 * lock is held with interrupts off so a ticket lock can't be stranded by
 * preemption, and it is SMP-safe). A queued HID report that completes
 * while a bulk transfer is waiting is routed to the keyboard/mouse path by
 * the shared dispatcher, so input keeps flowing during disk I/O.
 *
 * All rings / contexts / buffers live in this file's BSS, which the kernel
 * identity-maps in the first 4 GiB, so virtual == physical for DMA.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "hal/pci.h"
#include "hal/input.h"
#include "sync/cpu.h"
#include "sync/spinlock.h"

#include "xhci.h"

#define XHCI_CLASS    0x0Cu
#define XHCI_SUBCLASS 0x03u
#define XHCI_PROGIF   0x30u

/* ---- Capability registers (BAR0 + 0) ---------------------------------- */
#define CAP_CAPLENGTH   0x00
#define CAP_HCSPARAMS1  0x04
#define CAP_HCSPARAMS2  0x08
#define CAP_HCCPARAMS1  0x10
#define CAP_DBOFF       0x14
#define CAP_RTSOFF      0x18

/* ---- Operational registers (BAR0 + CAPLENGTH) ------------------------- */
#define OP_USBCMD   0x00
#define OP_USBSTS   0x04
#define OP_CRCR     0x18   /* 64-bit */
#define OP_DCBAAP   0x30   /* 64-bit */
#define OP_CONFIG   0x38
#define OP_PORTSC(p) (0x400 + ((p) - 1) * 0x10)

#define USBCMD_RUN   (1u << 0)
#define USBCMD_HCRST (1u << 1)
#define USBSTS_HCH   (1u << 0)
#define USBSTS_CNR   (1u << 11)

#define PORTSC_CCS  (1u << 0)
#define PORTSC_PED  (1u << 1)
#define PORTSC_PR   (1u << 4)
#define PORTSC_PP   (1u << 9)
#define PORTSC_CSC  (1u << 17)
#define PORTSC_PRC  (1u << 21)
#define PORTSC_SPEED_SHIFT 10
#define PORTSC_SPEED_MASK  0xF

/* ---- Runtime: interrupter 0 (BAR0 + RTSOFF + 0x20) -------------------- */
#define IR0_IMAN    0x00
#define IR0_IMOD    0x04
#define IR0_ERSTSZ  0x08
#define IR0_ERSTBA  0x10   /* 64-bit */
#define IR0_ERDP    0x18   /* 64-bit */

/* ---- TRB ------------------------------------------------------------- */
struct __attribute__((packed)) trb {
    uint64_t param;
    uint32_t status;
    uint32_t control;
};

#define TRB_TYPE(t)   (((t) & 0x3Fu) << 10)
#define TRB_GET_TYPE(c) (((c) >> 10) & 0x3Fu)
#define TRB_CYCLE     (1u << 0)

#define TRB_NORMAL        1
#define TRB_SETUP         2
#define TRB_DATA          3
#define TRB_STATUS        4
#define TRB_LINK          6
#define TRB_ENABLE_SLOT   9
#define TRB_ADDRESS_DEV   11
#define TRB_CONFIG_EP     12
#define TRB_EV_TRANSFER   32
#define TRB_EV_CMD_COMPL  33
#define TRB_EV_PORT_CHG   34

#define TRB_IOC   (1u << 5)
#define TRB_IDT   (1u << 6)   /* immediate data (setup) */
#define TRB_LINK_TC (1u << 1) /* toggle cycle */

#define CC_SUCCESS    1
#define CC_SHORT_PKT  13

#define RING_SZ   16
#define EVT_SZ    64
#define MAX_SLOTS 16
#define MAX_DEV   4           /* concurrent devices we bind */

/* Context buffers are sized for the 64-byte (CSZ=1) layout so a controller
 * that advertises large contexts can't overrun them; ctx_entry() uses the
 * runtime stride. */
#define CTX_ENTRY_MAX 64

enum usb_kind { USB_NONE = 0, USB_KEYBOARD, USB_MOUSE, USB_MSC };

/* Per-device state. The rings / device context / report buffers are
 * DMA-resident, so they live in dedicated aligned arrays below indexed by
 * the device's slot in g_dev[]; everything else is plain bookkeeping. */
struct usb_dev {
    bool     used;
    uint8_t  kind;          /* enum usb_kind */
    uint8_t  slot;          /* xHCI slot id (DCBAA index)        */
    uint32_t port;          /* root-hub port                     */
    uint32_t speed;

    /* HID interrupt IN endpoint. */
    uint8_t  int_epaddr;
    uint8_t  int_dci;
    uint16_t int_mps;
    bool     int_pending;
    uint32_t int_idx, int_cycle;
    uint8_t  last[8];       /* last keyboard report, for make/break diffing */

    /* EP0 control ring. */
    uint32_t ep0_idx, ep0_cycle;

    /* Mass-storage bulk endpoints. */
    uint8_t  bulk_in_dci,  bulk_out_dci;
    uint16_t bulk_in_mps,  bulk_out_mps;
    uint32_t bin_idx,  bin_cycle;
    uint32_t bout_idx, bout_cycle;
};

/* Controller-global DMA-resident structures (identity-mapped BSS). */
static __attribute__((aligned(64))) uint64_t   g_dcbaa[256];
static __attribute__((aligned(64))) struct trb g_cmd_ring[RING_SZ];
static __attribute__((aligned(64))) struct trb g_evt_ring[EVT_SZ];
static __attribute__((aligned(64))) uint8_t    g_erst[64];
static __attribute__((aligned(4096))) uint64_t g_scratch_arr[64];
static __attribute__((aligned(4096))) uint8_t  g_scratch_bufs[8][4096];
static __attribute__((aligned(64))) uint8_t    g_descbuf[512];
static __attribute__((aligned(64))) uint8_t    g_input_ctx[33 * CTX_ENTRY_MAX];
static __attribute__((aligned(64))) uint8_t    g_setupbuf[8];

/* Per-device DMA-resident structures. Each row is naturally aligned: a
 * RING_SZ TRB ring is 256 bytes, a device context 2 KiB, both multiples of
 * 64, so row i inherits the array's 64-byte alignment. */
static __attribute__((aligned(64))) struct trb g_ep0_ring[MAX_DEV][RING_SZ];
static __attribute__((aligned(64))) struct trb g_int_ring[MAX_DEV][RING_SZ];
static __attribute__((aligned(64))) struct trb g_bin_ring[MAX_DEV][RING_SZ];
static __attribute__((aligned(64))) struct trb g_bout_ring[MAX_DEV][RING_SZ];
static __attribute__((aligned(64))) uint8_t    g_dev_ctx[MAX_DEV][32 * CTX_ENTRY_MAX];
static __attribute__((aligned(64))) uint8_t    g_report[MAX_DEV][64];

static struct usb_dev g_dev[MAX_DEV];

static volatile uint8_t *g_cap;
static volatile uint8_t *g_op;
static volatile uint8_t *g_rt;
static volatile uint32_t *g_db;
static uint32_t g_ctx_bytes = 32;

static uint32_t g_cmd_idx, g_cmd_cycle = 1;
static uint32_t g_evt_idx, g_evt_cycle = 1;

static bool      g_initialized;       /* controller brought up + enumerated */
static bool      g_hid_present;       /* at least one HID device bound      */
static spinlock_t g_evt_lock = SPINLOCK_INITIALIZER;

/* ---- MMIO helpers ----------------------------------------------------- */
static inline uint32_t rd32(volatile uint8_t *b, uint32_t o) {
    return *(volatile uint32_t *)(b + o);
}
static inline void wr32(volatile uint8_t *b, uint32_t o, uint32_t v) {
    *(volatile uint32_t *)(b + o) = v;
}
static inline void wr64(volatile uint8_t *b, uint32_t o, uint64_t v) {
    *(volatile uint32_t *)(b + o)     = (uint32_t)v;
    *(volatile uint32_t *)(b + o + 4) = (uint32_t)(v >> 32);
}

/* ---- Ring helpers ----------------------------------------------------- */
static void ring_link(struct trb *ring, uint32_t cycle) {
    struct trb *l = &ring[RING_SZ - 1];
    memset(l, 0, sizeof(*l));
    l->param   = (uint64_t)(uintptr_t)ring;
    l->control = TRB_TYPE(TRB_LINK) | TRB_LINK_TC | (cycle ? TRB_CYCLE : 0);
}

static void push_trb(struct trb *ring, uint32_t *idx, uint32_t *cycle,
                     uint64_t param, uint32_t status, uint32_t control) {
    struct trb *t = &ring[*idx];
    t->param  = param;
    t->status = status;
    t->control = control | (*cycle ? TRB_CYCLE : 0);
    (*idx)++;
    if (*idx == RING_SZ - 1) {           /* hit the link TRB */
        ring[RING_SZ - 1].control =
            (ring[RING_SZ - 1].control & ~TRB_CYCLE) | (*cycle ? TRB_CYCLE : 0);
        *idx = 0;
        *cycle ^= 1;
    }
}

/* Poll the event ring once; if an event TRB is ready, copy it to *out,
 * advance ERDP, and return 1. Returns 0 if no event pending. Caller holds
 * g_evt_lock (or runs pre-SMP during enumeration). */
static int poll_event(struct trb *out) {
    struct trb *e = &g_evt_ring[g_evt_idx];
    if ((e->control & TRB_CYCLE) != (g_evt_cycle ? TRB_CYCLE : 0))
        return 0;
    *out = *e;
    g_evt_idx++;
    if (g_evt_idx == EVT_SZ) { g_evt_idx = 0; g_evt_cycle ^= 1; }
    /* Update ERDP to the next dequeue pointer (with EHB clear). */
    wr64(g_rt, 0x20 + IR0_ERDP,
         (uint64_t)(uintptr_t)&g_evt_ring[g_evt_idx] | (1u << 3));
    return 1;
}

static struct usb_dev *dev_by_slot(uint8_t slot) {
    for (int i = 0; i < MAX_DEV; i++)
        if (g_dev[i].used && g_dev[i].slot == slot) return &g_dev[i];
    return NULL;
}

static void queue_report(struct usb_dev *d);
static void emit_keys(struct usb_dev *d);
static void emit_mouse(struct usb_dev *d);

/* Route one HID interrupt-transfer completion to the keyboard/mouse path
 * and re-arm the endpoint. Called for events that aren't the bulk
 * completion a disk transfer is waiting on, so input keeps flowing during
 * I/O. Mass-storage completions have no async consumer here (a bulk
 * transfer is always awaited synchronously) and are ignored. */
static void route_hid_event(const struct trb *ev) {
    struct usb_dev *d = dev_by_slot((uint8_t)((ev->control >> 24) & 0xFFu));
    if (!d || d->kind == USB_MSC) return;
    uint32_t cc = (ev->status >> 24) & 0xFFu;
    if (cc == CC_SUCCESS || cc == CC_SHORT_PKT) {
        if (d->kind == USB_MOUSE) emit_mouse(d);
        else                      emit_keys(d);
    }
    d->int_pending = false;
    queue_report(d);
}

/* Wait (bounded) for a command-completion event; returns completion code,
 * or 0 on timeout. Runs only during enumeration (pre-SMP, single flow), so
 * no locking; other event types seen meanwhile are discarded - safe
 * because no transfers are queued until every device is configured. */
static uint32_t wait_cmd(struct trb *out) {
    for (uint64_t spin = 0; spin < 50000000ull; spin++) {
        struct trb ev;
        if (poll_event(&ev)) {
            if (TRB_GET_TYPE(ev.control) == TRB_EV_CMD_COMPL) {
                if (out) *out = ev;
                return (ev.status >> 24) & 0xFFu;
            }
        } else {
            canboot_cpu_relax();
        }
    }
    return 0;
}

/* Wait (bounded) for a transfer event on EP0 during enumeration. */
static uint32_t wait_xfer(void) {
    for (uint64_t spin = 0; spin < 50000000ull; spin++) {
        struct trb ev;
        if (poll_event(&ev)) {
            if (TRB_GET_TYPE(ev.control) == TRB_EV_TRANSFER)
                return (ev.status >> 24) & 0xFFu;
        } else {
            canboot_cpu_relax();
        }
    }
    return 0;
}

static void ring_cmd_doorbell(void) { g_db[0] = 0; }
static void ring_slot_doorbell(uint8_t slot, uint8_t dci) { g_db[slot] = dci; }

/* Issue a command TRB and wait for its Command Completion Event. Returns
 * completion code; *out_slot (if non-NULL) gets the event's slot id. */
static uint32_t do_command(uint64_t param, uint32_t control, uint8_t *out_slot) {
    push_trb(g_cmd_ring, &g_cmd_idx, &g_cmd_cycle, param, 0, control);
    ring_cmd_doorbell();
    struct trb ev;
    uint32_t cc = wait_cmd(&ev);
    if (out_slot) *out_slot = (uint8_t)((ev.control >> 24) & 0xFFu);
    return cc;
}

/* ---- Context accessors (32-byte entries; CSZ=1 -> 64 handled by stride) */
static uint8_t *ctx_entry(uint8_t *base, int i) { return base + i * g_ctx_bytes; }

/* ---- Control transfer on a device's EP0 ------------------------------- *
 * Standard SETUP/DATA/STATUS sequence. `data` length up to 512. Returns
 * completion code of the status stage (1 = success). Enumeration-only. */
static uint32_t control_xfer(struct usb_dev *d,
                             uint8_t bmRequestType, uint8_t bRequest,
                             uint16_t wValue, uint16_t wIndex,
                             uint16_t wLength, void *data) {
    int di = (int)(d - g_dev);
    struct trb *ring = g_ep0_ring[di];
    uint64_t setup;
    uint8_t *s = g_setupbuf;
    s[0] = bmRequestType; s[1] = bRequest;
    s[2] = (uint8_t)wValue; s[3] = (uint8_t)(wValue >> 8);
    s[4] = (uint8_t)wIndex; s[5] = (uint8_t)(wIndex >> 8);
    s[6] = (uint8_t)wLength; s[7] = (uint8_t)(wLength >> 8);
    memcpy(&setup, s, 8);

    int in = (bmRequestType & 0x80) != 0;
    uint32_t trt = wLength ? (in ? 3u : 2u) : 0u;
    push_trb(ring, &d->ep0_idx, &d->ep0_cycle, setup, 8,
             TRB_TYPE(TRB_SETUP) | TRB_IDT | (trt << 16));
    if (wLength) {
        push_trb(ring, &d->ep0_idx, &d->ep0_cycle,
                 (uint64_t)(uintptr_t)data, wLength,
                 TRB_TYPE(TRB_DATA) | (in ? (1u << 16) : 0));
    }
    push_trb(ring, &d->ep0_idx, &d->ep0_cycle, 0, 0,
             TRB_TYPE(TRB_STATUS) | ((wLength && in) ? 0 : (1u << 16)) | TRB_IOC);

    ring_slot_doorbell(d->slot, 1);    /* DCI 1 = EP0 */
    return wait_xfer();
}

/* ---- HID keyboard usage -> CanBoot key -------------------------------- */
static uint32_t hid_to_code(uint8_t usage, int shift) {
    if (usage >= 0x04 && usage <= 0x1D) {       /* a..z */
        char c = (char)('a' + (usage - 0x04));
        if (shift) c = (char)(c - 32);
        return (uint32_t)c;
    }
    if (usage >= 0x1E && usage <= 0x27) {       /* 1..0 */
        static const char num[]  = "1234567890";
        static const char sym[]  = "!@#$%^&*()";
        int i = usage - 0x1E;
        return (uint32_t)(shift ? sym[i] : num[i]);
    }
    switch (usage) {
        case 0x28: return CANBOOT_KEY_ENTER;
        case 0x29: return CANBOOT_KEY_ESC;
        case 0x2A: return CANBOOT_KEY_BACKSP;
        case 0x2B: return CANBOOT_KEY_TAB;
        case 0x2C: return ' ';
        case 0x2D: return shift ? '_' : '-';
        case 0x2E: return shift ? '+' : '=';
        case 0x4F: return CANBOOT_KEY_RIGHT;
        case 0x50: return CANBOOT_KEY_LEFT;
        case 0x51: return CANBOOT_KEY_DOWN;
        case 0x52: return CANBOOT_KEY_UP;
        default:   return 0;
    }
}

static int report_has(const uint8_t *rep, uint8_t usage) {
    for (int i = 2; i < 8; i++) if (rep[i] == usage) return 1;
    return 0;
}

static void emit_keys(struct usb_dev *d) {
    const uint8_t *rep = g_report[(int)(d - g_dev)];
    int shift = (rep[0] & 0x22) != 0;   /* L/R shift */
    for (int i = 2; i < 8; i++) {
        uint8_t u = rep[i];
        if (u == 0) continue;
        if (report_has(d->last, u)) continue;
        uint32_t code = hid_to_code(u, shift);
        if (!code) continue;
        struct canboot_event ev = {
            .type = CANBOOT_EV_KEY_DOWN, .source = CANBOOT_EV_SRC_USB_HID,
            .code = code, .raw = u,
        };
        canboot_input_push(&ev);
    }
    for (int i = 2; i < 8; i++) {
        uint8_t u = d->last[i];
        if (u == 0) continue;
        if (report_has(rep, u)) continue;
        uint32_t code = hid_to_code(u, shift);
        if (!code) continue;
        struct canboot_event ev = {
            .type = CANBOOT_EV_KEY_UP, .source = CANBOOT_EV_SRC_USB_HID,
            .code = code, .raw = u,
        };
        canboot_input_push(&ev);
    }
    memcpy(d->last, rep, 8);
}

/* Decode a HID boot-protocol mouse report: byte0 = button bitmap (bit0
 * left, bit1 right, bit2 middle), byte1 = dX, byte2 = dY (both 8-bit
 * signed), optional byte3 = wheel. Unlike PS/2, HID Y grows downward, so
 * the delta maps straight onto the framebuffer's top-left origin. */
static void emit_mouse(struct usb_dev *d) {
    const uint8_t *rep = g_report[(int)(d - g_dev)];
    uint8_t buttons = rep[0];
    int32_t dx = (int8_t)rep[1];
    int32_t dy = (int8_t)rep[2];
    int32_t wheel = (d->int_mps >= 4) ? (int8_t)rep[3] : 0;

    if (dx || dy) canboot_input_mouse_move_rel(dx, dy);
    canboot_input_mouse_button(CANBOOT_MOUSE_LEFT,   (buttons & 0x1) != 0);
    canboot_input_mouse_button(CANBOOT_MOUSE_RIGHT,  (buttons & 0x2) != 0);
    canboot_input_mouse_button(CANBOOT_MOUSE_MIDDLE, (buttons & 0x4) != 0);
    if (wheel) canboot_input_mouse_wheel(wheel);
}

/* Queue one interrupt-IN transfer for the next report on device d. */
static void queue_report(struct usb_dev *d) {
    int di = (int)(d - g_dev);
    /* Clear the report so a short packet (e.g. a 3-byte boot mouse) can't
     * leave a stale trailing byte that fakes wheel motion. */
    memset(g_report[di], 0, 8);
    push_trb(g_int_ring[di], &d->int_idx, &d->int_cycle,
             (uint64_t)(uintptr_t)g_report[di], d->int_mps,
             TRB_TYPE(TRB_NORMAL) | TRB_IOC);
    ring_slot_doorbell(d->slot, d->int_dci);
    d->int_pending = true;
}

/* ---- HAL input pump --------------------------------------------------- */
static void usb_hid_pump(void) {
    if (!g_hid_present) return;
    canboot_irqflags_t f = spin_lock_irqsave(&g_evt_lock);
    struct trb ev;
    while (poll_event(&ev)) {
        if (TRB_GET_TYPE(ev.control) == TRB_EV_TRANSFER) route_hid_event(&ev);
    }
    for (int i = 0; i < MAX_DEV; i++)
        if (g_dev[i].used && g_dev[i].kind != USB_MSC && !g_dev[i].int_pending)
            queue_report(&g_dev[i]);
    spin_unlock_irqrestore(&g_evt_lock, f);
}

/* ---- Bulk transfer (mass storage) ------------------------------------- *
 * Synchronous single bulk transfer on the given device/DCI. Routes any HID
 * completions seen while waiting so the keyboard keeps working. Returns 0
 * on success (or short packet), -1 on error/timeout; *residue (if non-NULL)
 * gets the untransferred byte count from the event. */
int canboot_xhci_bulk(uint8_t slot, uint8_t dci, int in,
                      void *buf, uint32_t len, uint32_t *residue) {
    struct usb_dev *d = dev_by_slot(slot);
    if (!d || d->kind != USB_MSC) return -1;
    int di = (int)(d - g_dev);
    struct trb *ring; uint32_t *idx, *cyc;
    if (dci == d->bulk_in_dci)  { ring = g_bin_ring[di];  idx = &d->bin_idx;  cyc = &d->bin_cycle; }
    else                        { ring = g_bout_ring[di]; idx = &d->bout_idx; cyc = &d->bout_cycle; }
    (void)in;

    canboot_irqflags_t f = spin_lock_irqsave(&g_evt_lock);
    push_trb(ring, idx, cyc, (uint64_t)(uintptr_t)buf, len,
             TRB_TYPE(TRB_NORMAL) | TRB_IOC);
    ring_slot_doorbell(slot, dci);

    int rc = -1;
    for (uint64_t spin = 0; spin < 200000000ull; spin++) {
        struct trb ev;
        if (!poll_event(&ev)) { canboot_cpu_relax(); continue; }
        if (TRB_GET_TYPE(ev.control) != TRB_EV_TRANSFER) continue;
        uint8_t ev_slot = (uint8_t)((ev.control >> 24) & 0xFFu);
        uint8_t ev_ep   = (uint8_t)((ev.control >> 16) & 0x1Fu);
        if (ev_slot == slot && ev_ep == dci) {
            uint32_t cc = (ev.status >> 24) & 0xFFu;
            if (residue) *residue = ev.status & 0xFFFFFFu;
            rc = (cc == CC_SUCCESS || cc == CC_SHORT_PKT) ? 0 : -1;
            break;
        }
        route_hid_event(&ev);          /* a HID report landed mid-transfer */
    }
    spin_unlock_irqrestore(&g_evt_lock, f);
    return rc;
}

/* ---- Enumeration ------------------------------------------------------ */
static int reset_port(uint32_t port) {
    uint32_t sc = rd32(g_op, OP_PORTSC(port));
    if (!(sc & PORTSC_CCS)) return 0;            /* nothing connected */
    sc = rd32(g_op, OP_PORTSC(port)) & ~((1u << 17) | (1u << 21) | (1u << 18) |
                                         (1u << 20) | (1u << 22));
    wr32(g_op, OP_PORTSC(port), sc | PORTSC_PR | PORTSC_PP);
    for (uint64_t s = 0; s < 5000000ull; s++) {
        if (rd32(g_op, OP_PORTSC(port)) & PORTSC_PRC) break;
        canboot_cpu_relax();
    }
    sc = rd32(g_op, OP_PORTSC(port));
    wr32(g_op, OP_PORTSC(port), (sc & ~((1u << 17) | (1u << 22))) | PORTSC_PRC);
    return (rd32(g_op, OP_PORTSC(port)) & PORTSC_PED) ? 1 : 0;
}

static uint16_t mps_for_speed(uint32_t speed) {
    switch (speed) {
        case 1: return 8;     /* full   */
        case 2: return 8;     /* low    */
        case 3: return 64;    /* high   */
        case 4: return 512;   /* super  */
        default: return 8;
    }
}

/* Address the device on `port` into slot `d` and read its config
 * descriptor into g_descbuf (returns total length, 0 on failure). */
static uint16_t address_and_get_config(struct usb_dev *d, uint32_t port) {
    int di = (int)(d - g_dev);
    uint32_t speed = (rd32(g_op, OP_PORTSC(port)) >> PORTSC_SPEED_SHIFT)
                     & PORTSC_SPEED_MASK;
    d->speed = speed;

    uint8_t slot = 0;
    if (do_command(0, TRB_TYPE(TRB_ENABLE_SLOT), &slot) != CC_SUCCESS || !slot) {
        printf("canboot: xhci enable-slot failed\n");
        return 0;
    }
    d->slot = slot;

    memset(g_dev_ctx[di], 0, sizeof(g_dev_ctx[di]));
    g_dcbaa[slot] = (uint64_t)(uintptr_t)g_dev_ctx[di];

    memset(g_input_ctx, 0, sizeof(g_input_ctx));
    uint32_t *icc = (uint32_t *)ctx_entry(g_input_ctx, 0);
    icc[1] = (1u << 0) | (1u << 1);              /* add slot ctx + EP0 ctx */
    uint32_t *slotc = (uint32_t *)ctx_entry(g_input_ctx, 1);
    slotc[0] = (1u << 27) | (speed << 20);
    slotc[1] = (port << 16);

    uint16_t mps0 = mps_for_speed(speed);
    uint32_t *ep0 = (uint32_t *)ctx_entry(g_input_ctx, 2);
    ep0[1] = (4u << 3) | (mps0 << 16) | (3u << 1);
    d->ep0_idx = 0; d->ep0_cycle = 1;
    ring_link(g_ep0_ring[di], 1);
    ep0[2] = (uint32_t)(uintptr_t)g_ep0_ring[di] | 1;
    ep0[3] = (uint32_t)((uint64_t)(uintptr_t)g_ep0_ring[di] >> 32);

    if (do_command((uint64_t)(uintptr_t)g_input_ctx,
                   TRB_TYPE(TRB_ADDRESS_DEV) | (slot << 24), NULL) != CC_SUCCESS) {
        printf("canboot: xhci address-device failed\n");
        return 0;
    }

    memset(g_descbuf, 0, sizeof(g_descbuf));
    if (control_xfer(d, 0x80, 0x06, 0x0200, 0, 9, g_descbuf) != CC_SUCCESS)
        return 0;
    uint16_t total = (uint16_t)(g_descbuf[2] | (g_descbuf[3] << 8));
    if (total > sizeof(g_descbuf)) total = sizeof(g_descbuf);
    if (control_xfer(d, 0x80, 0x06, 0x0200, 0, total, g_descbuf) != CC_SUCCESS)
        return 0;
    return total;
}

/* Write one endpoint context entry into g_input_ctx for DCI `dci`. */
static void set_ep_ctx(uint8_t dci, uint32_t eptype, uint16_t mps,
                       struct trb *ring) {
    uint32_t *epc = (uint32_t *)ctx_entry(g_input_ctx, dci + 1);
    epc[0] = (eptype == 7u /*intr*/) ? (3u << 16) : 0u;  /* interval */
    epc[1] = (eptype << 3) | ((uint32_t)mps << 16) | (3u << 1);
    epc[2] = (uint32_t)(uintptr_t)ring | 1;              /* DCS=1 */
    epc[3] = (uint32_t)((uint64_t)(uintptr_t)ring >> 32);
    epc[4] = mps;                                        /* avg TRB length */
}

/* Configure a HID device's interrupt IN endpoint and arm it later. */
static bool configure_hid(struct usb_dev *d, uint8_t cfg_value,
                          uint8_t iface_no, uint8_t proto) {
    int di = (int)(d - g_dev);
    if (control_xfer(d, 0x00, 0x09, cfg_value, 0, 0, NULL) != CC_SUCCESS)
        return false;
    control_xfer(d, 0x21, 0x0B, 0, iface_no, 0, NULL);   /* SET_PROTOCOL(boot) */

    uint8_t epnum = d->int_epaddr & 0x0F;
    d->int_dci = (uint8_t)(epnum * 2 + 1);
    memset(g_input_ctx, 0, sizeof(g_input_ctx));
    uint32_t *icc = (uint32_t *)ctx_entry(g_input_ctx, 0);
    icc[1] = (1u << 0) | (1u << d->int_dci);
    uint32_t *slotc = (uint32_t *)ctx_entry(g_input_ctx, 1);
    slotc[0] = ((uint32_t)d->int_dci << 27) | (d->speed << 20);
    slotc[1] = (d->port << 16);

    d->int_idx = 0; d->int_cycle = 1;
    ring_link(g_int_ring[di], 1);
    set_ep_ctx(d->int_dci, 7u /* intr IN */, d->int_mps, g_int_ring[di]);

    if (do_command((uint64_t)(uintptr_t)g_input_ctx,
                   TRB_TYPE(TRB_CONFIG_EP) | (d->slot << 24), NULL) != CC_SUCCESS) {
        printf("canboot: xhci configure-endpoint (hid) failed\n");
        return false;
    }
    d->kind = (proto == 2) ? USB_MOUSE : USB_KEYBOARD;
    return true;
}

/* Configure a mass-storage device's bulk IN + bulk OUT endpoints. */
static bool configure_msc(struct usb_dev *d, uint8_t cfg_value,
                          uint8_t in_ep, uint16_t in_mps,
                          uint8_t out_ep, uint16_t out_mps) {
    int di = (int)(d - g_dev);
    if (control_xfer(d, 0x00, 0x09, cfg_value, 0, 0, NULL) != CC_SUCCESS)
        return false;

    d->bulk_in_dci  = (uint8_t)((in_ep  & 0x0F) * 2 + 1);
    d->bulk_out_dci = (uint8_t)((out_ep & 0x0F) * 2);
    d->bulk_in_mps  = in_mps;
    d->bulk_out_mps = out_mps;
    uint8_t max_dci = d->bulk_in_dci > d->bulk_out_dci ? d->bulk_in_dci
                                                       : d->bulk_out_dci;

    memset(g_input_ctx, 0, sizeof(g_input_ctx));
    uint32_t *icc = (uint32_t *)ctx_entry(g_input_ctx, 0);
    icc[1] = (1u << 0) | (1u << d->bulk_in_dci) | (1u << d->bulk_out_dci);
    uint32_t *slotc = (uint32_t *)ctx_entry(g_input_ctx, 1);
    slotc[0] = ((uint32_t)max_dci << 27) | (d->speed << 20);
    slotc[1] = (d->port << 16);

    d->bin_idx = 0;  d->bin_cycle = 1;  ring_link(g_bin_ring[di], 1);
    d->bout_idx = 0; d->bout_cycle = 1; ring_link(g_bout_ring[di], 1);
    set_ep_ctx(d->bulk_in_dci,  6u /* bulk IN  */, in_mps,  g_bin_ring[di]);
    set_ep_ctx(d->bulk_out_dci, 2u /* bulk OUT */, out_mps, g_bout_ring[di]);

    if (do_command((uint64_t)(uintptr_t)g_input_ctx,
                   TRB_TYPE(TRB_CONFIG_EP) | (d->slot << 24), NULL) != CC_SUCCESS) {
        printf("canboot: xhci configure-endpoint (msc) failed\n");
        return false;
    }
    d->kind = USB_MSC;
    return true;
}

/* Enumerate and configure the device on `port` into slot `d`. On success
 * d->used/kind/... are populated (HID endpoints not yet armed). */
static bool enumerate(struct usb_dev *d, uint32_t port) {
    uint16_t total = address_and_get_config(d, port);
    if (!total) return false;

    uint8_t cfg_value = g_descbuf[5];

    /* Walk descriptors, tracking the interface we're inside, and collect
     * the endpoints we care about per class. */
    uint8_t iface_no = 0, cur_class = 0, cur_proto = 0;
    uint8_t hid_iface = 0, hid_proto = 0;
    bool    hid_found = false;
    uint8_t msc_in_ep = 0, msc_out_ep = 0;
    uint16_t msc_in_mps = 512, msc_out_mps = 512;
    bool    msc_in = false, msc_out = false;

    for (int i = 0; i + 1 < total; ) {
        uint8_t len = g_descbuf[i], typ = g_descbuf[i + 1];
        if (len == 0) break;
        if (typ == 0x04) {                       /* INTERFACE */
            iface_no  = g_descbuf[i + 2];
            cur_class = g_descbuf[i + 5];
            cur_proto = g_descbuf[i + 7];
        } else if (typ == 0x05) {                /* ENDPOINT */
            uint8_t addr = g_descbuf[i + 2];
            uint8_t attr = g_descbuf[i + 3];
            uint16_t mps = (uint16_t)(g_descbuf[i + 4] | (g_descbuf[i + 5] << 8));
            if (cur_class == 0x03 && (addr & 0x80) && (attr & 0x3) == 0x3
                && !hid_found) {                 /* HID interrupt IN */
                d->int_epaddr = addr;
                d->int_mps = (mps == 0 || mps > 8) ? 8 : mps;
                hid_iface = iface_no; hid_proto = cur_proto;
                hid_found = true;
            } else if (cur_class == 0x08 && (attr & 0x3) == 0x2) {  /* MSC bulk */
                if (addr & 0x80) { msc_in_ep = addr;  msc_in_mps = mps ? mps : 512; msc_in = true; }
                else             { msc_out_ep = addr; msc_out_mps = mps ? mps : 512; msc_out = true; }
            }
        }
        i += len;
    }

    if (msc_in && msc_out) {
        if (!configure_msc(d, cfg_value, msc_in_ep, msc_in_mps,
                           msc_out_ep, msc_out_mps))
            return false;
        d->port = port;
        d->used = true;
        printf("canboot: usb-storage on port %u slot %u in 0x%02x out 0x%02x\n",
               port, d->slot, msc_in_ep, msc_out_ep);
        return true;
    }
    if (hid_found) {
        if (!configure_hid(d, cfg_value, hid_iface, hid_proto)) return false;
        d->port = port;
        d->used = true;
        memset(d->last, 0, sizeof(d->last));
        printf("canboot: usb-hid %s on port %u slot %u ep 0x%02x mps %u\n",
               d->kind == USB_MOUSE ? "mouse" : "keyboard",
               port, d->slot, d->int_epaddr, d->int_mps);
        return true;
    }

    printf("canboot: xhci device on port %u has no supported class\n", port);
    return false;
}

static struct usb_dev *alloc_dev(void) {
    for (int i = 0; i < MAX_DEV; i++)
        if (!g_dev[i].used) return &g_dev[i];
    return NULL;
}

/* ---- Controller bring-up (idempotent) --------------------------------- */
bool canboot_xhci_ensure_init(void) {
    if (g_initialized) return true;

    uint32_t n = hal_pci_devcount();
    const struct canboot_pci_dev *devs = hal_pci_devs();
    const struct canboot_pci_dev *pd = NULL;
    for (uint32_t i = 0; i < n; i++) {
        if (devs[i].class_code == XHCI_CLASS &&
            devs[i].subclass   == XHCI_SUBCLASS &&
            devs[i].prog_if    == XHCI_PROGIF) { pd = &devs[i]; break; }
    }
    if (!pd) return false;

    memset(g_dev, 0, sizeof(g_dev));

    hal_pci_enable_bus_master(pd->addr);
    uint64_t bar = hal_pci_bar_addr(pd->addr, 0);
    if (!bar) return false;
    g_cap = (volatile uint8_t *)(uintptr_t)bar;

    uint8_t caplen = *(volatile uint8_t *)(g_cap + CAP_CAPLENGTH);
    g_op = g_cap + caplen;
    g_rt = g_cap + (rd32(g_cap, CAP_RTSOFF) & ~0x1Fu);
    g_db = (volatile uint32_t *)(g_cap + (rd32(g_cap, CAP_DBOFF) & ~0x3u));
    uint32_t hcc = rd32(g_cap, CAP_HCCPARAMS1);
    g_ctx_bytes = (hcc & (1u << 2)) ? 64 : 32;
    uint32_t hcs1 = rd32(g_cap, CAP_HCSPARAMS1);
    uint32_t maxslots = hcs1 & 0xFFu;
    uint32_t maxports = (hcs1 >> 24) & 0xFFu;

    wr32(g_op, OP_USBCMD, 0);
    for (uint64_t s = 0; s < 10000000ull; s++) {
        if (!(rd32(g_op, OP_USBSTS) & USBSTS_HCH)) continue;
        break;
    }
    wr32(g_op, OP_USBCMD, USBCMD_HCRST);
    for (uint64_t s = 0; s < 50000000ull; s++) {
        if (!(rd32(g_op, OP_USBCMD) & USBCMD_HCRST) &&
            !(rd32(g_op, OP_USBSTS) & USBSTS_CNR)) break;
        canboot_cpu_relax();
    }

    wr32(g_op, OP_CONFIG, maxslots < MAX_SLOTS ? maxslots : MAX_SLOTS);

    memset(g_dcbaa, 0, sizeof(g_dcbaa));
    uint32_t hcs2 = rd32(g_cap, CAP_HCSPARAMS2);
    uint32_t spb = ((hcs2 >> 27) & 0x1F) | (((hcs2 >> 21) & 0x1F) << 5);
    if (spb > 8) spb = 8;
    if (spb) {
        for (uint32_t i = 0; i < spb; i++)
            g_scratch_arr[i] = (uint64_t)(uintptr_t)g_scratch_bufs[i];
        g_dcbaa[0] = (uint64_t)(uintptr_t)g_scratch_arr;
    }
    wr64(g_op, OP_DCBAAP, (uint64_t)(uintptr_t)g_dcbaa);

    memset(g_cmd_ring, 0, sizeof(g_cmd_ring));
    g_cmd_idx = 0; g_cmd_cycle = 1;
    ring_link(g_cmd_ring, 1);
    wr64(g_op, OP_CRCR, (uint64_t)(uintptr_t)g_cmd_ring | 1);

    memset(g_evt_ring, 0, sizeof(g_evt_ring));
    g_evt_idx = 0; g_evt_cycle = 1;
    uint64_t *erst = (uint64_t *)g_erst;
    erst[0] = (uint64_t)(uintptr_t)g_evt_ring;
    ((uint32_t *)g_erst)[2] = EVT_SZ;
    ((uint32_t *)g_erst)[3] = 0;
    wr32(g_rt, 0x20 + IR0_ERSTSZ, 1);
    wr64(g_rt, 0x20 + IR0_ERDP, (uint64_t)(uintptr_t)g_evt_ring);
    wr64(g_rt, 0x20 + IR0_ERSTBA, (uint64_t)(uintptr_t)g_erst);
    wr32(g_rt, 0x20 + IR0_IMAN, 0);

    wr32(g_op, OP_USBCMD, USBCMD_RUN);
    for (uint64_t s = 0; s < 10000000ull; s++) {
        if (!(rd32(g_op, OP_USBSTS) & USBSTS_HCH)) break;
        canboot_cpu_relax();
    }

    /* Enumerate every connected root-hub port, binding up to MAX_DEV
     * devices (keyboard + mouse + a USB disk, in any mix). No transfers are
     * queued during this loop so the event ring carries only command/port
     * events; this runs before preemption, so no locking is needed. */
    for (uint32_t p = 1; p <= maxports; p++) {
        if (!(rd32(g_op, OP_PORTSC(p)) & PORTSC_CCS)) continue;
        if (!reset_port(p)) continue;
        struct usb_dev *d = alloc_dev();
        if (!d) break;
        if (!enumerate(d, p)) memset(d, 0, sizeof(*d));
    }

    /* Arm every bound HID device and register the single shared pump. */
    bool any_hid = false;
    for (int i = 0; i < MAX_DEV; i++) {
        if (!g_dev[i].used || g_dev[i].kind == USB_MSC) continue;
        queue_report(&g_dev[i]);
        any_hid = true;
    }
    if (any_hid) {
        g_hid_present = true;
        canboot_input_register_pump(usb_hid_pump);
    }

    g_initialized = true;
    return true;
}

/* ---- HID HAL entry point ---------------------------------------------- */
bool canboot_usb_hid_init(void) {
    canboot_xhci_ensure_init();
    return g_hid_present;
}

bool canboot_usb_hid_present(void) { return g_hid_present; }

/* ---- Mass-storage device accessors (for hal/disk/usb_storage.c) ------- */
int canboot_xhci_msc_count(void) {
    int c = 0;
    for (int i = 0; i < MAX_DEV; i++)
        if (g_dev[i].used && g_dev[i].kind == USB_MSC) c++;
    return c;
}

bool canboot_xhci_msc_get(int idx, uint8_t *slot,
                          uint8_t *bulk_in_dci, uint8_t *bulk_out_dci) {
    int c = 0;
    for (int i = 0; i < MAX_DEV; i++) {
        if (!g_dev[i].used || g_dev[i].kind != USB_MSC) continue;
        if (c++ != idx) continue;
        if (slot)         *slot         = g_dev[i].slot;
        if (bulk_in_dci)  *bulk_in_dci  = g_dev[i].bulk_in_dci;
        if (bulk_out_dci) *bulk_out_dci = g_dev[i].bulk_out_dci;
        return true;
    }
    return false;
}
