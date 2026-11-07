/* Player settings: settings.ini (KEY=VALUE) in the per-user data folder.
 * Data folder: <exe dir>/user when portable.txt sits beside the executable,
 * otherwise SDL's pref path (%APPDATA%\DigimonBattleGC on Windows). */
#ifndef DIGIMON_SETTINGS_H
#define DIGIMON_SETTINGS_H
#include "upstream_gx.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct GameSettings {
    char disc[1024];
    int window_width, window_height;
    int fullscreen;
    int render_scale;      /* 1..4 x 640x480 */
    int stretch, smooth, vsync;
    int audio, volume;     /* volume 0..100 */
    int audio_latency;     /* ms; 0 = auto (adaptive) */
    int pause_unfocused;
    int skip_launcher;
    int memcard;           /* slot A card: <data dir>/MemoryCardA.raw */
    int show_fps;
    int touch_mode;        /* 0 = auto (shown while no controller is connected), 1 = always, 2 = never */
    int touch_opacity;     /* 10..100 % */
    int touch_size;        /* 50..200 % */
    int touch_haptics;     /* vibrate on press; game rumble goes to the phone */
    UpstreamGxInput input;
} GameSettings;

void settings_defaults(GameSettings *s);
/* Folder for settings, logs, screenshots and controller mappings; created on
 * first use, always ends without a separator. */
const char *settings_data_dir(void);
int settings_load(GameSettings *s);        /* 1 when a settings file was read */
int settings_save(const GameSettings *s);  /* 0 on success */

const char *settings_input_label(int input);          /* "A", "Stick Up", ... */
const char *settings_pad_binding_name(int binding);   /* SDL name, "none" */
int settings_pad_binding_parse(const char *name);

/* Resolves a user-selected image to a readable ISO path. Raw .iso/.gcm paths
 * pass through. For .rvz/.wia/.gcz/.ciso the function looks for an imported
 * ISO beside the image or in <data dir>/cache (written by tools/prepare_disc.py)
 * and fails with an actionable message when no import exists. */
int settings_resolve_disc(const char *path, char *resolved, size_t resolved_size,
                          char *message, size_t message_size);

/* Checks a disc image against the recompiled game (Digimon Battle Chronicle,
 * G2DJB2 rev 0). Returns 1 if usable; writes a one-line reason either way.
 * Compressed images are checked through settings_resolve_disc(). */
int settings_check_disc(const char *path, char *message, size_t size);

#ifdef __cplusplus
}
#endif
#endif
