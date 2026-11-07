/* Graphics-only gcn-recomp 3fad2f2 adapter. GL calls stay on the render thread;
 * the guest thread may feed the command stream and read its input snapshot.
 * MEM1 is borrowed, never owned. CP/PI/MMIO remain the caller's responsibility. */
#ifndef DIGIMON_UPSTREAM_GX_H
#define DIGIMON_UPSTREAM_GX_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct UpstreamGxConfig {
    uint8_t *ram;
    size_t ram_size;                   /* must be exactly 24 MiB */
    int width, height;                /* output framebuffer (capture) dimensions */
    int hidden;                       /* hidden SDL window, offscreen FBO capture */
    int internal_scale;               /* 1 = native EFB resolution */
    void *user;
    void (*pe_finish)(void *user);
    void (*pe_token)(uint16_t token, int interrupt, void *user);
    uint32_t (*vi_height)(void *user);  /* zero = no configured display */
    void (*debug_frame)(uint32_t frame, void *user);
    void (*debug_dma)(const char *what, uint32_t address, uint32_t length, void *user);
    /* Player frontend. All-zero keeps the fixed width x height, 1:1 window. */
    int window_width, window_height;  /* initial window size; 0 = width x height */
    int resizable, fullscreen;        /* fullscreen = desktop resolution */
    int stretch;                      /* 0 = keep 4:3 with black bars */
    int smooth;                       /* linear filtering when scaling to the window */
    int vsync;
    int pause_unfocused;
    const char *title;                /* NULL = "Digimon Battle GC" */
    const char *data_dir;             /* gamecontrollerdb.txt, screenshots/ (F12); NULL = none */
    /* In-game menu (F1/Esc, Android back, touch menu button) edits these GameSettings
     * (settings.h) and calls settings_changed to apply and persist. NULL: no overlay. */
    void *settings;
    void (*settings_changed)(void *user);
    /* App about to be suspended or killed (Android background): flush saves now. */
    void (*on_suspend)(void *user);
} UpstreamGxConfig;

int upstream_gx_init(const UpstreamGxConfig *config);
/* Copies bytes, retains partial commands across calls and feeds the GX front end.
 * Safe on the guest thread: it never touches GL. Returns 0 on success, -1 on error.
 * Do not also expand CALL_DL in the caller. */
int upstream_gx_consume(const void *bytes, size_t length);
/* Drain submitted frames, service SDL events and hotkeys, blit to the window.
 * Must run on the GL owner thread. Waits up to a few milliseconds for a frame.
 * Returns 1 if any frame was presented, 0 if none, -1 if the user requested exit.
 * Hotkeys: F11 / Alt+Enter fullscreen, F8 pause, F9 FPS in title, F12 screenshot. */
int upstream_gx_present(void);
/* Paused by F8 or focus loss: block (redrawing the last frame) until resumed.
 * Returns 0 when resumed, -1 if the user requested exit. */
int upstream_gx_paused(void);
int upstream_gx_wait_paused(void);
/* Live display options (in-game menu). */
void upstream_gx_set_display(int stretch, int smooth, int show_fps);
int upstream_gx_capture_png(const char *path);
/* Top-down RGBA8 from the last presented output, capacity >= width*height*4. */
int upstream_gx_read_rgba(uint8_t *out, size_t capacity);
typedef struct UpstreamGxPad {
    uint16_t buttons;  /* active-high GC SI mask, defined bits 0x1f7f */
    uint8_t stick_x, stick_y, sub_x, sub_y, trigger_l, trigger_r;
} UpstreamGxPad;

/* Bindable GameCube inputs. Controllers bind the GC_IN_A..GC_IN_DRIGHT
 * buttons (sticks are fixed: left = main, right = C); the keyboard binds all. */
enum {
    GC_IN_A, GC_IN_B, GC_IN_X, GC_IN_Y, GC_IN_Z, GC_IN_L, GC_IN_R, GC_IN_START,
    GC_IN_DUP, GC_IN_DDOWN, GC_IN_DLEFT, GC_IN_DRIGHT,
    GC_IN_PAD_COUNT,
    GC_IN_STICK_UP = GC_IN_PAD_COUNT, GC_IN_STICK_DOWN, GC_IN_STICK_LEFT, GC_IN_STICK_RIGHT,
    GC_IN_C_UP, GC_IN_C_DOWN, GC_IN_C_LEFT, GC_IN_C_RIGHT,
    GC_IN_COUNT
};
/* Controller binding codes: SDL_GameControllerButton, GC_BIND_AXIS + trigger
 * SDL_GameControllerAxis, or GC_BIND_NONE. Keyboard bindings are SDL_Scancode. */
#define GC_BIND_NONE (-1)
#define GC_BIND_AXIS 100
typedef struct UpstreamGxInput {
    int pad[GC_IN_PAD_COUNT];
    int key[GC_IN_COUNT];
    int deadzone;        /* stick deadzone, percent */
    int keyboard_port;   /* 0..3 */
    int rumble;
} UpstreamGxInput;
void upstream_gx_input_defaults(UpstreamGxInput *input);
void upstream_gx_set_input(const UpstreamGxInput *input);
/* Controllers take ports 1-4 in connection order; the keyboard also drives
 * keyboard_port. Returns the bit mask of ports that have an input device.
 * Must run on the render thread (it also drives the touch overlay). */
unsigned upstream_gx_pads(UpstreamGxPad pads[4]);
/* Copy of the most recent render-thread snapshot. Safe on the guest thread. */
unsigned upstream_gx_snapshot_pads(UpstreamGxPad pads[4]);
/* The guest tells the render thread when it has stopped between frames, so an
 * Android background event can flush the memory card without racing it. */
void upstream_gx_set_guest_paused(int paused);
/* Port 1 only (original single-player ABI). */
void upstream_gx_pad(UpstreamGxPad *pad);
/* Safe on the guest thread: the render thread applies the request. */
void upstream_gx_rumble(unsigned port, int on);
/* Presented frames. Safe on either thread. */
uint32_t upstream_gx_frame_count(void);
const char *upstream_gx_error(void);
void upstream_gx_reset(void);           /* clears graphics state; keeps RAM/context */
void upstream_gx_shutdown(void);        /* releases renderer and SDL resources */
#ifdef __cplusplus
}
#endif
#endif
