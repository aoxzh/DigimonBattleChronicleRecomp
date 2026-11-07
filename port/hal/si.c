/* SI controller protocol: communication RAM transfers and frame-latched polling.
 * Protocol references and bounded timing differences are in
 * docs/analysis/si-pad-protocol.md. */
#include "si.h"
#include "hal.h"
#include "irq.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t out, in_hi, in_lo;
    uint8_t mode;
} SiChan;

static SiChan ch[4];
static uint32_t si_poll, com_csr, sisr, exi_clock;
static uint8_t comm_ram[128];

#define CSR_TSTART    0x00000001u
#define CSR_RDST_MASK 0x08000000u
#define CSR_RDST      0x10000000u
#define CSR_COMERR    0x20000000u
#define CSR_TC_MASK   0x40000000u
#define CSR_TC        0x80000000u
#define CSR_WRITABLE  0x487F7F06u
#define SR_NOREP      0x08u
#define SR_RDST       0x20u
#define PAD_BUTTONS   0x1F7Fu

static struct {
    uint16_t btn;  /* active high internally and on the wire */
    uint8_t sx, sy, subx, suby, tl, tr;
    int connected;
} pad[4];

static void si_poll_tick(void);
static void si_update_interrupts(void);

/* A bounded, host-side replay lane. Events use SDK button bit positions. */
typedef enum {
    INPUT_BUTTON,
    INPUT_STICK_X,
    INPUT_STICK_Y,
    INPUT_SUB_X,
    INPUT_SUB_Y,
    INPUT_TRIGGER_L,
    INPUT_TRIGGER_R,
    INPUT_CONNECTED,
} InputKind;

typedef struct {
    uint64_t frame;
    unsigned chan;
    InputKind kind;
    uint16_t button_mask;
    uint8_t value;
    int pressed;
    int applied;
} InputEvent;

#define INPUT_EVENT_MAX 256u
#define INPUT_SCRIPT_MAX (1024u * 1024u)
static InputEvent input_events[INPUT_EVENT_MAX];
static unsigned input_event_count;
static char *input_script_storage;

static void input_script_apply(void);

static const char *skip_ws(const char *p)
{
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static int json_number(const char *begin, const char *end,
                       const char *key, uint64_t *out)
{
    char needle[64];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n <= 0 || (size_t)n >= sizeof(needle)) return 0;
    const char *p = begin;
    while (p < end) {
        const char *hit = strstr(p, needle);
        if (!hit || hit >= end) return 0;
        const char *colon = strchr(hit + n, ':');
        if (!colon || colon >= end) return 0;
        colon = skip_ws(colon + 1);
        if (colon >= end || !isdigit((unsigned char)*colon)) return 0;
        char *ep = NULL;
        unsigned long long v = strtoull(colon, &ep, 0);
        if (ep == colon || ep > end) return 0;
        *out = (uint64_t)v;
        return 1;
    }
    return 0;
}

static int json_string(const char *begin, const char *end,
                       const char *key, char *out, size_t out_n)
{
    char needle[64];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n <= 0 || (size_t)n >= sizeof(needle) || out_n == 0) return 0;
    const char *hit = strstr(begin, needle);
    if (!hit || hit >= end) return 0;
    const char *colon = strchr(hit + n, ':');
    if (!colon || colon >= end) return 0;
    const char *p = skip_ws(colon + 1);
    if (p >= end || *p != '\"') return 0;
    p++;
    size_t i = 0;
    while (p < end && *p && *p != '\"') {
        if (*p == '\\' && p + 1 < end) p++;
        if (i + 1 < out_n) out[i++] = *p;
        p++;
    }
    if (p >= end || *p != '\"') return 0;
    out[i] = '\0';
    return 1;
}

static int button_mask(const char *name, uint16_t *mask)
{
    struct ButtonName { const char *name; uint16_t mask; };
    static const struct ButtonName names[] = {
        {"LEFT", 0x0001u}, {"RIGHT", 0x0002u}, {"DOWN", 0x0004u},
        {"UP", 0x0008u}, {"Z", 0x0010u}, {"R", 0x0020u},
        {"L", 0x0040u}, {"A", 0x0100u}, {"B", 0x0200u},
        {"X", 0x0400u}, {"Y", 0x0800u}, {"START", 0x1000u},
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(name, names[i].name) == 0) {
            *mask = names[i].mask;
            return 1;
        }
    }
    return 0;
}

static int input_kind(const char *name, InputKind *kind)
{
    struct AxisName { const char *name; InputKind kind; };
    static const struct AxisName names[] = {
        {"STICK_X", INPUT_STICK_X}, {"STICK_Y", INPUT_STICK_Y},
        {"SUB_X", INPUT_SUB_X}, {"SUB_Y", INPUT_SUB_Y},
        {"TRIG_L", INPUT_TRIGGER_L}, {"TRIG_R", INPUT_TRIGGER_R},
        {"SX", INPUT_STICK_X}, {"SY", INPUT_STICK_Y},
        {"SUBX", INPUT_SUB_X}, {"SUBY", INPUT_SUB_Y},
        {"L_ANALOG", INPUT_TRIGGER_L}, {"R_ANALOG", INPUT_TRIGGER_R},
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(name, names[i].name) == 0) {
            *kind = names[i].kind;
            return 1;
        }
    }
    return 0;
}

static void input_script_reset(void)
{
    free(input_script_storage);
    input_script_storage = NULL;
    input_event_count = 0;
    memset(input_events, 0, sizeof(input_events));
}

static void input_script_load(void)
{
    input_script_reset();
    const char *path = getenv("GC_INPUT_SCRIPT");
    if (!path || !*path) return;

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[input] cannot open script %s\n", path);
        return;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    long size = ftell(f);
    if (size < 0 || (unsigned long)size > INPUT_SCRIPT_MAX ||
        fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "[input] script too large or unreadable: %s\n", path);
        fclose(f);
        return;
    }
    input_script_storage = (char *)malloc((size_t)size + 1u);
    if (!input_script_storage ||
        fread(input_script_storage, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "[input] cannot read script %s\n", path);
        fclose(f);
        input_script_reset();
        return;
    }
    fclose(f);
    input_script_storage[size] = '\0';

    const char *p = input_script_storage;
    while (*p && input_event_count < INPUT_EVENT_MAX) {
        const char *open = strchr(p, '{');
        if (!open) break;
        const char *close = strchr(open + 1, '}');
        if (!close) break;
        InputEvent ev;
        memset(&ev, 0, sizeof(ev));
        uint64_t frame = 0, chan = 0, value = 0;
        char name[32], state[16];
        if (!json_number(open, close, "frame", &frame)) {
            p = close + 1;
            continue;
        }
        if (json_number(open, close, "channel", &chan) && chan > 3) {
            p = close + 1;
            continue;
        }
        ev.frame = frame;
        ev.chan = (unsigned)chan;
        ev.value = 0x80;
        if (json_string(open, close, "button", name, sizeof(name))) {
            if (!button_mask(name, &ev.button_mask) ||
                !json_string(open, close, "state", state, sizeof(state))) {
                p = close + 1;
                continue;
            }
            ev.kind = INPUT_BUTTON;
            ev.pressed = strcmp(state, "release") != 0;
        } else if (json_string(open, close, "axis", name, sizeof(name)) &&
                   input_kind(name, &ev.kind) &&
                   json_number(open, close, "value", &value) && value <= 255) {
            ev.value = (uint8_t)value;
        } else if (json_number(open, close, "connected", &value) && value <= 1) {
            ev.kind = INPUT_CONNECTED;
            ev.value = (uint8_t)value;
        } else {
            p = close + 1;
            continue;
        }
        input_events[input_event_count++] = ev;
        p = close + 1;
    }
    fprintf(stderr, "[input] loaded %u events from %s\n", input_event_count, path);
}

void hal_si_frame_tick(void)
{
    input_script_apply();
    si_poll_tick();
}

static void input_script_apply(void)
{
    uint64_t frame = hal_frame_count();
    for (unsigned i = 0; i < input_event_count; i++) {
        InputEvent *ev = &input_events[i];
        if (ev->applied || ev->frame > frame) continue;
        if (ev->kind == INPUT_BUTTON) {
            if (ev->pressed) pad[ev->chan].btn |= ev->button_mask;
            else             pad[ev->chan].btn &= (uint16_t)~ev->button_mask;
        } else if (ev->kind == INPUT_STICK_X) {
            pad[ev->chan].sx = ev->value;
        } else if (ev->kind == INPUT_STICK_Y) {
            pad[ev->chan].sy = ev->value;
        } else if (ev->kind == INPUT_SUB_X) {
            pad[ev->chan].subx = ev->value;
        } else if (ev->kind == INPUT_SUB_Y) {
            pad[ev->chan].suby = ev->value;
        } else if (ev->kind == INPUT_TRIGGER_L) {
            pad[ev->chan].tl = ev->value;
        } else if (ev->kind == INPUT_TRIGGER_R) {
            pad[ev->chan].tr = ev->value;
        } else if (ev->kind == INPUT_CONNECTED) {
            pad[ev->chan].connected = ev->value != 0;
        }
        ev->applied = 1;
    }
}

void hal_si_init(void)
{
    memset(ch, 0, sizeof(ch));
    memset(pad, 0, sizeof(pad));
    memset(comm_ram, 0, sizeof(comm_ram));
    si_poll = com_csr = sisr = exi_clock = 0;
    for (unsigned i = 0; i < 4; i++) {
        ch[i].mode = 3;
        hal_si_set_pad_raw(i, 0, 0x80, 0x80, 0x80, 0x80, 0, 0);
    }
    pad[0].connected = 1;
    input_script_load();
    si_update_interrupts();
}

void hal_si_set_pad(unsigned chan, uint16_t button16,
                    uint8_t stickx, uint8_t sticky,
                    uint8_t subx, uint8_t suby,
                    uint8_t trigL, uint8_t trigR)
{
    /* Preserve the original host ABI, including its 0x1ff0 neutral value.
     * That old mask omitted the D-pad: XOR this value, not all 16 bits. */
    hal_si_set_pad_raw(chan, button16 ^ 0x1FF0u, stickx, sticky,
                       subx, suby, trigL, trigR);
}

void hal_si_set_pad_raw(unsigned chan, uint16_t buttons,
                        uint8_t stickx, uint8_t sticky,
                        uint8_t subx, uint8_t suby,
                        uint8_t trigL, uint8_t trigR)
{
    if (chan > 3) return;
    pad[chan].btn = buttons & PAD_BUTTONS;
    pad[chan].sx = stickx;   pad[chan].sy = sticky;
    pad[chan].subx = subx;   pad[chan].suby = suby;
    pad[chan].tl = trigL;    pad[chan].tr = trigR;
}

void hal_si_set_connected(unsigned chan, int connected)
{
    if (chan > 3) return;
    pad[chan].connected = connected != 0;
}

int hal_si_rumble(unsigned chan)
{
    if (chan > 3 || !pad[chan].connected) return 0;
    return ((ch[chan].out >> 16) & 0xFF) == 0x40 && (ch[chan].out & 3u) == 1u;
}

static uint32_t channel_status(unsigned chan, uint32_t bits)
{
    return bits << (24 - 8 * chan);
}

static void si_update_interrupts(void)
{
    /* RDSTINT is derived, so writing its W1C bit cannot hide unread data. */
    if (sisr & 0x20202020u) com_csr |= CSR_RDST;
    else                    com_csr &= ~CSR_RDST;
    bool set = ((com_csr & CSR_TC) && (com_csr & CSR_TC_MASK)) ||
               ((com_csr & CSR_RDST) && (com_csr & CSR_RDST_MASK));
    if (set) hal_pi_set_interrupt(PI_INT_SI);
    else     hal_pi_clear_interrupt(PI_INT_SI);
}

static void poll_response(unsigned c, uint32_t *hi, uint32_t *lo)
{
    *hi = ((uint32_t)pad[c].btn << 16) | 0x00800000u |
          ((uint32_t)pad[c].sx << 8) | pad[c].sy;
    uint32_t cx = pad[c].subx, cy = pad[c].suby;
    uint32_t l = pad[c].tl, r = pad[c].tr;
    /* Dolphin GCController::GetData: A/B analog pressure is unsupported (0). */
    switch (ch[c].mode) {
    case 1:
        *lo = ((cx >> 4) << 28) | ((cy >> 4) << 24) | (l << 16) | (r << 8);
        break;
    case 2:
        *lo = ((cx >> 4) << 28) | ((cy >> 4) << 24) |
              ((l >> 4) << 20) | ((r >> 4) << 16);
        break;
    case 3:
        *lo = (cx << 24) | (cy << 16) | (l << 8) | r;
        break;
    case 4:
        *lo = (cx << 24) | (cy << 16);
        break;
    default:  /* modes 0, 5, 6, 7 */
        *lo = (cx << 24) | (cy << 16) | ((l >> 4) << 12) | ((r >> 4) << 8);
        break;
    }
}

static void si_poll_tick(void)
{
    for (unsigned c = 0; c < 4; c++) {
        if (!(si_poll & (0x80u >> c))) continue;
        if (pad[c].connected) {
            uint32_t errlatch = ch[c].in_hi & 0x40000000u;
            poll_response(c, &ch[c].in_hi, &ch[c].in_lo);
            ch[c].in_hi |= errlatch;
            sisr |= channel_status(c, SR_RDST);
        } else {
            ch[c].in_hi = 0xC0000000u;  /* ERRSTAT and ERRLATCH */
            ch[c].in_lo = 0;
            sisr |= channel_status(c, SR_NOREP);
        }
    }
    si_update_interrupts();
}

static void store_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static void si_transfer(void)
{
    unsigned c = (com_csr >> 1) & 3;
    unsigned outlen = (com_csr >> 16) & 0x7F;
    unsigned inlen = (com_csr >> 8) & 0x7F;
    if (!outlen) outlen = 128;
    if (!inlen) inlen = 128;
    uint8_t response[128] = {0};
    unsigned response_len = 0;
    uint8_t cmd = comm_ram[0];
    uint32_t hi, lo;
    com_csr &= ~(CSR_TSTART | CSR_COMERR);
    com_csr |= CSR_TC;
    if (pad[c].connected) {
        switch (cmd) {
        case 0x00: case 0xFF:
            response[0] = 0x09; /* standard wired GameCube controller ID */
            response_len = 3;
            break;
        case 0x41: case 0x42:
            /* Fixed neutral calibration, independent of held host input.
             * Use the pinned runtime's origin marker and center values. */
            response[1] = 0x80;
            response[2] = response[3] = response[4] = response[5] = 0x80;
            response_len = 10;
            break;
        case 0x40:
            if (outlen < 3) break;
            poll_response(c, &hi, &lo);
            store_be32(response, hi);
            store_be32(response + 4, lo);
            response_len = 8;
            break;
        default: break;
        }
    }
    if (!response_len) {
        /* Unsupported requests time out deterministically like an absent pad. */
        com_csr |= CSR_COMERR;
        sisr |= channel_status(c, SR_NOREP);
    } else {
        /* Only clock the requested bytes; pad an overlong receive with zero. */
        memcpy(comm_ram, response, inlen);
    }
    si_update_interrupts();
}

uint32_t hal_si_read32(uint32_t reg)
{
    if (reg & 3u) return 0;
    if (reg >= 0x80 && reg <= 0xFC) {
        const uint8_t *p = comm_ram + (reg - 0x80);
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
               ((uint32_t)p[2] << 8) | p[3];
    }
    if (reg < 0x30) {
        unsigned c = reg / 0x0C;
        uint32_t off = reg % 0x0C;
        if (off == 0) return ch[c].out;
        /* Both input-word reads acknowledge RDST, as in Dolphin SI.cpp. */
        sisr &= ~channel_status(c, SR_RDST);
        si_update_interrupts();
        return off == 4 ? ch[c].in_hi : ch[c].in_lo;
    }
    switch (reg) {
    case 0x30: return si_poll;
    case 0x34: return com_csr;
    case 0x38: return sisr;
    case 0x3C: return exi_clock;
    default:   return 0;
    }
}

void hal_si_write32(uint32_t reg, uint32_t v)
{
    if (reg & 3u) return;
    if (reg >= 0x80 && reg <= 0xFC) {
        store_be32(comm_ram + (reg - 0x80), v);
        return;
    }
    if (reg < 0x30) {
        unsigned c = reg / 0x0C;
        uint32_t off = reg % 0x0C;
        if (off == 0) ch[c].out = v;
        else if (off == 4) ch[c].in_hi = v;
        else ch[c].in_lo = v;
        return;
    }
    switch (reg) {
    case 0x30: si_poll = v; break;
    case 0x34:
        /* Preserve completion/error flags across mask/length writes. */
        com_csr = (com_csr & ~CSR_WRITABLE) | (v & CSR_WRITABLE);
        if (v & CSR_TC) com_csr &= ~CSR_TC;
        if (v & CSR_RDST) com_csr &= ~CSR_RDST;
        if (v & CSR_TSTART) si_transfer();
        si_update_interrupts();
        break;
    case 0x38:
        sisr &= ~(v & 0x0F0F0F0Fu);  /* only errors are W1C */
        if (v & 0x80000000u) {
            for (unsigned c = 0; c < 4; c++) {
                /* SDK changes analog mode with polling temporarily disabled. */
                if (((ch[c].out >> 16) & 0xFF) == 0x40 &&
                    !(si_poll & (0x80u >> c)))
                    ch[c].mode = (uint8_t)((ch[c].out >> 8) & 7);
            }
            sisr &= ~0x90101010u; /* WR and WRST clear on immediate copy */
        }
        si_update_interrupts();
        break;
    case 0x3C: exi_clock = v; break;
    default: break;
    }
}
