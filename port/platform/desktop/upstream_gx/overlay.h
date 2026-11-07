/* In-game overlay (Dear ImGui on the game's GL context): touch controls and the pause /
 * settings menu. All calls on the GL owner thread, after upstream_gx_init. */
#ifndef DIGIMON_OVERLAY_H
#define DIGIMON_OVERLAY_H
#include <SDL.h>
#include "upstream_gx.h"

struct GameSettings;
struct OverlayHost {
    struct GameSettings *settings;          /* NULL: menu shows no settings */
    void (*settings_changed)(void *user);   /* apply + persist after an edit */
    void *user;
    const char *(*port_name)(int port);     /* controller on GC port 0..3, or NULL */
};

bool overlay_init(SDL_Window *window, const OverlayHost &host);
void overlay_shutdown();
/* True when the overlay used the event: a finger on a touch control, or menu input. */
bool overlay_event(const SDL_Event &e);
bool overlay_menu_open();
void overlay_set_menu(bool open);
enum { OVERLAY_NONE, OVERLAY_RESUME, OVERLAY_QUIT };
int overlay_take_action();
/* Draws touch controls / FPS / menu over the presented frame (default framebuffer). */
void overlay_draw(int drawable_w, int drawable_h, int fps, bool show_fps);
bool overlay_touch_visible();
/* Merges the touch controls into the pad of the player they drive. */
void overlay_pad(UpstreamGxPad &pad);
/* Game rumble for the touch player, played on the phone's vibrator. */
void overlay_rumble(bool on);

#endif
