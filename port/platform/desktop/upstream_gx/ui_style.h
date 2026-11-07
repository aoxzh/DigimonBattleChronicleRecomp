/* Shared Dear ImGui look for the launcher and the in-game overlay. */
#ifndef DIGIMON_UI_STYLE_H
#define DIGIMON_UI_STYLE_H

/* UI scale for the display the window is on: 1 = 96 dpi desktop; phones scale by
 * physical density so touch targets stay finger-sized. */
float ui_display_scale(void);
void ui_apply_style(float scale);
/* Loads a system UI font (+ CJK glyphs for file and controller names) at 17 px * scale. */
void ui_load_fonts(float scale);

#endif
