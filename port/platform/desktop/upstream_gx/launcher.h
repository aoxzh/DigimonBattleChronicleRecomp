/* Desktop launcher (Dear ImGui): disc image, display, audio and controls.
 * Runs in its own SDL window before the game window exists. */
#ifndef DIGIMON_LAUNCHER_H
#define DIGIMON_LAUNCHER_H
#include "settings.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Returns 1 to start the game (settings saved), 0 to quit. */
int launcher_run(GameSettings *settings);
#ifdef __cplusplus
}
#endif
#endif
