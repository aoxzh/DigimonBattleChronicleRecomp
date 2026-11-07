/* Player settings and disc validation; see settings.h. */
#include "settings.h"
#include "../../../hal/hal_boot.h"
#include <SDL.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define make_dir(p) mkdir((p), 0755)
#endif

/* The generated code was lifted from this main.dol: FNV-1a 64 over the bytes
 * up to the end of its last section (the extracted file adds 224 bytes of
 * padding, which hal_boot_read_disc_dol does not read). */
#define GAME_ID "G2DJB2"
#define GAME_DOL_SIZE 3591200u
#define GAME_DOL_FNV 0x5FD8E0C4B410A2D2ull

static const char *const input_keys[GC_IN_COUNT] = {
    "A", "B", "X", "Y", "Z", "L", "R", "Start", "DUp", "DDown", "DLeft", "DRight",
    "StickUp", "StickDown", "StickLeft", "StickRight", "CUp", "CDown", "CLeft", "CRight"};
static const char *const input_labels[GC_IN_COUNT] = {
    "A", "B", "X", "Y", "Z", "L", "R", "Start", "D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right",
    "Stick Up", "Stick Down", "Stick Left", "Stick Right", "C-Stick Up", "C-Stick Down", "C-Stick Left", "C-Stick Right"};

void settings_defaults(GameSettings *s)
{
    memset(s, 0, sizeof(*s));
    s->window_width = 1280; s->window_height = 960;
    s->render_scale = 2;
    s->smooth = 1;
    s->audio = 1; s->volume = 100;
    s->pause_unfocused = 1;
    s->memcard = 1;
    s->touch_opacity = 55; s->touch_size = 100; s->touch_haptics = 1;
#ifdef __ANDROID__
    /* The GameCube frame is 640x480. A phone that cannot hold 60 Hz at 2x
     * should use the native resolution; the launcher can still raise it. */
    s->render_scale = 1;
    s->pause_unfocused = 1;
#endif
    upstream_gx_input_defaults(&s->input);
}

const char *settings_data_dir(void)
{
    static char dir[1024];
    if (dir[0]) return dir;
    char *base = SDL_GetBasePath();
    if (base) {
        char marker[1024];
        snprintf(marker, sizeof(marker), "%sportable.txt", base);
        FILE *f = fopen(marker, "rb");
        if (f) {
            fclose(f);
            snprintf(dir, sizeof(dir), "%suser", base);
            make_dir(dir);
        }
        SDL_free(base);
    }
    if (!dir[0]) {
        char *pref = SDL_GetPrefPath("", "DigimonBattleGC");
        snprintf(dir, sizeof(dir), "%s", pref ? pref : ".");
        SDL_free(pref);
        size_t n = strlen(dir);
        if (n > 1 && (dir[n - 1] == '/' || dir[n - 1] == '\\')) dir[n - 1] = 0;
    }
    return dir;
}

static void settings_path(char *out, size_t size)
{
    snprintf(out, size, "%s/settings.ini", settings_data_dir());
}

const char *settings_input_label(int input)
{
    return input >= 0 && input < GC_IN_COUNT ? input_labels[input] : "?";
}

const char *settings_pad_binding_name(int b)
{
    const char *name = NULL;
    if (b >= GC_BIND_AXIS) name = SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)(b - GC_BIND_AXIS));
    else if (b >= 0) name = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)b);
    return name ? name : "none";
}

int settings_pad_binding_parse(const char *name)
{
    SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(name);
    if (b != SDL_CONTROLLER_BUTTON_INVALID) return (int)b;
    SDL_GameControllerAxis a = SDL_GameControllerGetAxisFromString(name);
    if (a == SDL_CONTROLLER_AXIS_TRIGGERLEFT || a == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) return GC_BIND_AXIS + (int)a;
    return GC_BIND_NONE;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) ++s;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

int settings_load(GameSettings *s)
{
    char path[1100], line[1400];
    settings_path(path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        char *k = trim(line);
        if (!eq || *k == '#' || *k == ';') continue;
        *eq = 0;
        k = trim(k);
        char *v = trim(eq + 1);
        int n = atoi(v);
        if (!strcmp(k, "disc")) snprintf(s->disc, sizeof(s->disc), "%s", v);
        else if (!strcmp(k, "window_width")) s->window_width = clampi(n, 320, 16384);
        else if (!strcmp(k, "window_height")) s->window_height = clampi(n, 240, 16384);
        else if (!strcmp(k, "fullscreen")) s->fullscreen = n != 0;
        else if (!strcmp(k, "render_scale")) s->render_scale = clampi(n, 1, 4);
        else if (!strcmp(k, "stretch")) s->stretch = n != 0;
        else if (!strcmp(k, "smooth")) s->smooth = n != 0;
        else if (!strcmp(k, "vsync")) s->vsync = n != 0;
        else if (!strcmp(k, "audio")) s->audio = n != 0;
        else if (!strcmp(k, "volume")) s->volume = clampi(n, 0, 100);
        else if (!strcmp(k, "audio_latency")) s->audio_latency = n <= 0 ? 0 : clampi(n, 20, 350);
        else if (!strcmp(k, "pause_unfocused")) s->pause_unfocused = n != 0;
        else if (!strcmp(k, "skip_launcher")) s->skip_launcher = n != 0;
        else if (!strcmp(k, "memcard")) s->memcard = n != 0;
        else if (!strcmp(k, "show_fps")) s->show_fps = n != 0;
        else if (!strcmp(k, "touch_mode")) s->touch_mode = clampi(n, 0, 2);
        else if (!strcmp(k, "touch_opacity")) s->touch_opacity = clampi(n, 10, 100);
        else if (!strcmp(k, "touch_size")) s->touch_size = clampi(n, 50, 200);
        else if (!strcmp(k, "touch_haptics")) s->touch_haptics = n != 0;
        else if (!strcmp(k, "deadzone")) s->input.deadzone = clampi(n, 0, 90);
        else if (!strcmp(k, "keyboard_port")) s->input.keyboard_port = clampi(n, 1, 4) - 1;
        else if (!strcmp(k, "rumble")) s->input.rumble = n != 0;
        else if (!strncmp(k, "pad.", 4) || !strncmp(k, "key.", 4)) {
            for (int i = 0; i < GC_IN_COUNT; i++) {
                if (strcmp(k + 4, input_keys[i])) continue;
                if (k[0] == 'k') s->input.key[i] = strcmp(v, "none") ? (int)SDL_GetScancodeFromName(v) : 0;
                else if (i < GC_IN_PAD_COUNT) s->input.pad[i] = settings_pad_binding_parse(v);
            }
        }
    }
    fclose(f);
    return 1;
}

int settings_save(const GameSettings *s)
{
    char path[1100];
    settings_path(path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "# Digimon Battle Chronicle desktop settings. Delete this file to restore defaults.\n");
    fprintf(f, "disc=%s\nwindow_width=%d\nwindow_height=%d\nfullscreen=%d\nrender_scale=%d\n",
            s->disc, s->window_width, s->window_height, s->fullscreen, s->render_scale);
    fprintf(f, "stretch=%d\nsmooth=%d\nvsync=%d\naudio=%d\nvolume=%d\naudio_latency=%d\npause_unfocused=%d\nskip_launcher=%d\n",
            s->stretch, s->smooth, s->vsync, s->audio, s->volume, s->audio_latency, s->pause_unfocused, s->skip_launcher);
    fprintf(f, "memcard=%d\nshow_fps=%d\ntouch_mode=%d\ntouch_opacity=%d\ntouch_size=%d\ntouch_haptics=%d\n",
            s->memcard, s->show_fps, s->touch_mode, s->touch_opacity, s->touch_size, s->touch_haptics);
    fprintf(f, "deadzone=%d\nkeyboard_port=%d\nrumble=%d\n",
            s->input.deadzone, s->input.keyboard_port + 1, s->input.rumble);
    for (int i = 0; i < GC_IN_PAD_COUNT; i++)
        fprintf(f, "pad.%s=%s\n", input_keys[i], settings_pad_binding_name(s->input.pad[i]));
    for (int i = 0; i < GC_IN_COUNT; i++) {
        const char *name = s->input.key[i] > 0 ? SDL_GetScancodeName((SDL_Scancode)s->input.key[i]) : "";
        fprintf(f, "key.%s=%s\n", input_keys[i], *name ? name : "none");
    }
    return fclose(f) == 0 ? 0 : -1;
}

static int has_ext(const char *path, const char *ext)
{
    size_t n = strlen(path), e = strlen(ext);
    return n >= e && !SDL_strcasecmp(path + n - e, ext);
}

static int is_compressed_image(const char *path)
{
    static const char *const compressed[] = {".rvz", ".wia", ".gcz", ".ciso", ".wbfs", ".nfs"};
    for (unsigned i = 0; i < sizeof(compressed) / sizeof(compressed[0]); i++)
        if (has_ext(path, compressed[i])) return 1;
    return 0;
}

static int has_gc_magic(const char *path)
{
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) return 0;
    unsigned char h[0x20];
    const size_t got = fread(h, 1, sizeof(h), f);
    fclose(f);
    return got == sizeof(h) && h[0x1C] == 0xC2 && h[0x1D] == 0x33 &&
           h[0x1E] == 0x9F && h[0x1F] == 0x3D;
}

/* "dir/name.rvz" -> "name"; works with both host separators. */
static void image_stem(const char *path, char *out, size_t size)
{
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    const char *dot = strrchr(base, '.');
    const size_t n = dot && dot != base ? (size_t)(dot - base) : strlen(base);
    if (n >= size) {
        memcpy(out, base, size - 1);
        out[size - 1] = 0;
    } else {
        memcpy(out, base, n);
        out[n] = 0;
    }
}

static int sibling_iso(const char *path, char *out, size_t size)
{
    const size_t n = strlen(path);
    if (n + 5 >= size) return 0;
    snprintf(out, size, "%s", path);
    char *dot = strrchr(out, '.');
    char *slash = strrchr(out, '/');
    char *bslash = strrchr(out, '\\');
    if (slash && bslash && bslash > slash) slash = bslash;
    if (dot && (!slash || dot > slash)) *dot = 0;
    strcat(out, ".iso");
    return has_gc_magic(out);
}

int settings_resolve_disc(const char *path, char *resolved, size_t resolved_size,
                          char *msg, size_t msg_size)
{
    if (!path || !*path) {
        snprintf(msg, msg_size, "Choose your Digimon Battle Chronicle disc image (.iso or .gcm).");
        return 0;
    }
    if (resolved_size) snprintf(resolved, resolved_size, "%s", path);
    if (!is_compressed_image(path)) return 1;

    if (sibling_iso(path, resolved, resolved_size)) return 1;

    char stem[256], cache_dir[1100], cache[1200];
    image_stem(path, stem, sizeof(stem));
    snprintf(cache_dir, sizeof(cache_dir), "%s/cache", settings_data_dir());
    make_dir(cache_dir);
    snprintf(cache, sizeof(cache), "%s/%s.iso", cache_dir, stem);
    if (has_gc_magic(cache)) {
        snprintf(resolved, resolved_size, "%s", cache);
        return 1;
    }

    snprintf(msg, msg_size,
             "Compressed image is not imported yet. Run: python tools/prepare_disc.py \"%s\" "
             "(RVZ/GCZ/CISO; WIA: convert to .iso with Dolphin), then choose it again.", path);
    return 0;
}

int settings_check_disc(const char *path, char *msg, size_t size)
{
    char resolved[1200], reason[512];
    if (!settings_resolve_disc(path, resolved, sizeof(resolved), reason, sizeof(reason))) {
        snprintf(msg, size, "%s", reason);
        return 0;
    }
    const int imported = strcmp(resolved, path) != 0;
    path = resolved;
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(msg, size, "Cannot open the file."); return 0; }
    unsigned char h[0x40];
    size_t got = fread(h, 1, sizeof(h), f);
    fclose(f);
    if (got != sizeof(h) || h[0x1C] != 0xC2 || h[0x1D] != 0x33 || h[0x1E] != 0x9F || h[0x1F] != 0x3D) {
        snprintf(msg, size, imported ? "Imported ISO is invalid; delete it and re-run tools/prepare_disc.py."
                                     : "Not a GameCube disc image.");
        return 0;
    }
    char id[7], name[33];
    memcpy(id, h, 6); id[6] = 0;
    for (int i = 0; i < 32; i++) name[i] = h[0x20 + i] >= 0x20 && h[0x20 + i] < 0x7F ? (char)h[0x20 + i] : (h[0x20 + i] ? '?' : 0);
    name[32] = 0;
    if (strcmp(id, GAME_ID)) {
        snprintf(msg, size, "This disc is %s (%s). The port needs Digimon Battle Chronicle (" GAME_ID ").", name, id);
        return 0;
    }
    if (h[7] != 0) { snprintf(msg, size, "Disc revision %u found; the port was built from revision 0.", h[7]); return 0; }
    size_t n = 0;
    unsigned char *dol = hal_boot_read_disc_dol(path, &n);
    if (!dol) { snprintf(msg, size, "Cannot read main.dol from the disc image (truncated file?)."); return 0; }
    unsigned long long fnv = 14695981039346656037ull;
    for (size_t i = 0; i < n; i++) { fnv ^= dol[i]; fnv *= 1099511628211ull; }
    free(dol);
    if (n != GAME_DOL_SIZE || fnv != GAME_DOL_FNV) {
        snprintf(msg, size, "main.dol on this disc differs from the recompiled build (modified or bad dump).");
        return 0;
    }
    snprintf(msg, size, "Digimon Battle Chronicle (" GAME_ID ", rev 0) - ready%s.",
             imported ? " (imported ISO)" : "");
    return 1;
}
