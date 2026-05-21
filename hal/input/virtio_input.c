/*
 * virtio-input device driver. Discovers the first virtio-input PCI
 * function, negotiates features (we ask for nothing), wires up the
 * eventq (idx 0) with 32 receive buffers, then polls the used ring on
 * every pump cycle and translates Linux input event codes to CanBoot
 * events.
 *
 * statusq (idx 1) is not used here - we only consume events.
 */

#include <stdbool.h>
#include <stdint.h>

#include "hal/input.h"
#include "hal/virtio.h"

#define EVENTQ_IDX  0u
#define NUM_BUFS    32u

struct __attribute__((packed)) virtio_input_event {
    uint16_t type;
    uint16_t code;
    uint32_t value;
};

/* Linux input event types. */
#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03

/* Linux keycodes we care about; rest fall through unchanged. */
#define KEY_ESC      1
#define KEY_BACKSP   14
#define KEY_TAB      15
#define KEY_ENTER    28
#define KEY_LCTRL    29
#define KEY_LSHIFT   42
#define KEY_RSHIFT   54
#define KEY_LALT     56
#define KEY_SPACE    57
#define KEY_CAPS     58
#define KEY_KPENTER  96
#define KEY_RCTRL    97
#define KEY_RALT     100
#define KEY_HOME     102
#define KEY_UP       103
#define KEY_LEFT     105
#define KEY_RIGHT    106
#define KEY_DOWN     108

/* Subset of the US-layout Linux keycode -> ASCII mapping. */
static const char ascii_lower[256] = {
    [2]  = '1', [3]  = '2', [4]  = '3', [5]  = '4', [6]  = '5',
    [7]  = '6', [8]  = '7', [9]  = '8', [10] = '9', [11] = '0',
    [12] = '-', [13] = '=',
    [16] = 'q', [17] = 'w', [18] = 'e', [19] = 'r', [20] = 't',
    [21] = 'y', [22] = 'u', [23] = 'i', [24] = 'o', [25] = 'p',
    [26] = '[', [27] = ']',
    [30] = 'a', [31] = 's', [32] = 'd', [33] = 'f', [34] = 'g',
    [35] = 'h', [36] = 'j', [37] = 'k', [38] = 'l', [39] = ';',
    [40] = '\'', [41] = '`', [43] = '\\',
    [44] = 'z', [45] = 'x', [46] = 'c', [47] = 'v', [48] = 'b',
    [49] = 'n', [50] = 'm', [51] = ',', [52] = '.', [53] = '/',
    [57] = ' ',
};

static const char ascii_shift_punct[128] = {
    ['1']  = '!', ['2'] = '@', ['3'] = '#', ['4'] = '$', ['5'] = '%',
    ['6']  = '^', ['7'] = '&', ['8'] = '*', ['9'] = '(', ['0'] = ')',
    ['-']  = '_', ['='] = '+', ['['] = '{', [']'] = '}', [';'] = ':',
    ['\''] = '"', ['`'] = '~', ['\\'] = '|',
    [',']  = '<', ['.'] = '>', ['/'] = '?',
};

static struct canboot_virtio_dev   g_dev;
static struct canboot_virtq        g_eventq;

static struct canboot_virtq_desc   g_desc [CANBOOT_VIRTQ_SIZE]
    __attribute__((aligned(16)));
static struct canboot_virtq_avail  g_avail
    __attribute__((aligned(2)));
static struct canboot_virtq_used   g_used
    __attribute__((aligned(4)));

static struct virtio_input_event   g_evbufs[NUM_BUFS]
    __attribute__((aligned(4)));

static bool g_present;
static bool g_shift;

static uint32_t translate(uint16_t keycode) {
    switch (keycode) {
        case KEY_ESC:     return CANBOOT_KEY_ESC;
        case KEY_BACKSP:  return CANBOOT_KEY_BACKSP;
        case KEY_TAB:     return CANBOOT_KEY_TAB;
        case KEY_ENTER:
        case KEY_KPENTER: return CANBOOT_KEY_ENTER;
        case KEY_LCTRL:   return CANBOOT_KEY_LCTRL;
        case KEY_RCTRL:   return CANBOOT_KEY_RCTRL;
        case KEY_LSHIFT:  return CANBOOT_KEY_LSHIFT;
        case KEY_RSHIFT:  return CANBOOT_KEY_RSHIFT;
        case KEY_LALT:    return CANBOOT_KEY_LALT;
        case KEY_RALT:    return CANBOOT_KEY_RALT;
        case KEY_CAPS:    return CANBOOT_KEY_CAPS;
        case KEY_UP:      return CANBOOT_KEY_UP;
        case KEY_DOWN:    return CANBOOT_KEY_DOWN;
        case KEY_LEFT:    return CANBOOT_KEY_LEFT;
        case KEY_RIGHT:   return CANBOOT_KEY_RIGHT;
        default:
            if (keycode < 256 && ascii_lower[keycode]) {
                char c = ascii_lower[keycode];
                if (g_shift) {
                    if (c >= 'a' && c <= 'z') return (uint32_t)(c - 'a' + 'A');
                    if (ascii_shift_punct[(int)c]) return (uint32_t)ascii_shift_punct[(int)c];
                }
                return (uint32_t)c;
            }
            return 0;
    }
}

static void emit(uint32_t code, uint16_t keycode, bool down) {
    struct canboot_event ev = {
        .type   = down ? CANBOOT_EV_KEY_DOWN : CANBOOT_EV_KEY_UP,
        .source = CANBOOT_EV_SRC_VIRTIO_INPUT,
        .code   = code,
        .raw    = keycode,
        .value  = down ? 1 : 0,
    };
    canboot_input_push(&ev);
}

static void handle_event(const struct virtio_input_event *e) {
    if (e->type == EV_SYN) return;
    if (e->type != EV_KEY) return;
    bool down = (e->value != 0);

    /* Track shift state via virtio key codes (Linux keycodes). */
    if (e->code == KEY_LSHIFT || e->code == KEY_RSHIFT) {
        g_shift = down;
    }

    uint32_t code = translate(e->code);
    if (code == 0) return;
    emit(code, e->code, down);
}

static void refill(uint16_t desc_id) {
    canboot_virtq_publish_writable(&g_eventq, desc_id,
                                   &g_evbufs[desc_id % NUM_BUFS],
                                   sizeof(struct virtio_input_event));
}

static void virtio_input_pump(void) {
    uint16_t completed = canboot_virtq_used_advance(&g_eventq);
    if (completed == 0) return;

    for (uint16_t i = 0; i < completed; i++) {
        uint16_t slot = (uint16_t)((g_eventq.last_used_idx + i) % g_eventq.size);
        uint32_t id   = g_eventq.used->ring[slot].id;
        if (id < g_eventq.size) {
            handle_event(&g_evbufs[id % NUM_BUFS]);
            refill((uint16_t)id);
        }
    }
    g_eventq.last_used_idx = (uint16_t)(g_eventq.last_used_idx + completed);
    canboot_virtq_kick(&g_eventq, EVENTQ_IDX);
}

bool canboot_virtio_input_init(void) {
    if (!canboot_virtio_find(CANBOOT_VIRTIO_PCI_INPUT, &g_dev)) return false;
    if (!canboot_virtio_negotiate(&g_dev, 0)) return false;

    uint16_t nq = g_dev.num_queues;
    if (nq < 1) return false;

    if (!canboot_virtio_queue_setup(&g_dev, EVENTQ_IDX, &g_eventq,
                                    g_desc, &g_avail, &g_used)) {
        return false;
    }

    /* Stash NUM_BUFS event buffers in the eventq so the device has
     * somewhere to write incoming events. */
    uint16_t to_publish = NUM_BUFS;
    if (to_publish > g_eventq.size) to_publish = g_eventq.size;
    for (uint16_t i = 0; i < to_publish; i++) {
        canboot_virtq_publish_writable(&g_eventq, i,
                                       &g_evbufs[i % NUM_BUFS],
                                       sizeof(struct virtio_input_event));
    }

    if (!canboot_virtio_run(&g_dev)) return false;

    canboot_virtq_kick(&g_eventq, EVENTQ_IDX);

    canboot_input_register_pump(virtio_input_pump);
    g_present = true;
    return true;
}

bool canboot_virtio_input_present(void) { return g_present; }
