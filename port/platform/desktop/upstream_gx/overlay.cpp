#include "overlay.h"
#include "settings.h"
#include "ui_style.h"
#include "gl.h"
#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"
#include <algorithm>
#include <cmath>
#include <map>

namespace {

// GC SI button bits (see si.h).
enum : uint16_t { B_L = 0x0040, B_R = 0x0020, B_Z = 0x0010, B_A = 0x0100, B_B = 0x0200,
                  B_X = 0x0400, B_Y = 0x0800, B_START = 0x1000 };
enum Kind { K_BUTTON, K_STICK, K_MENU, K_SHOULDER };
struct Control { const char *label; Kind kind; uint16_t mask; float x, y, r; };
enum { C_STICK, C_A, C_B, C_X, C_Y, C_Z, C_L, C_R, C_START, C_MENU, C_COUNT };
struct Finger { int ctrl; float ox, oy, x, y; };

SDL_Window *window;
OverlayHost host;
ImGuiContext *ctx;
bool ready, menu, menu_was_open;
int action;
float ui_scale = 1.0f;
float dpi = 160.0f;
int W = 1, H = 1;
Control ctl[C_COUNT];
std::map<SDL_FingerID, Finger> fingers;
// A tap can go down and up between two input samples (the render thread samples once per
// present, the guest takes that snapshot once per field), and the game then never sees it.
// Each pressed button is held for at least this long after its finger lands.
constexpr uint32_t kMinPressMs = 60;
uint32_t press_until[C_COUNT];
SDL_Haptic *haptic;
bool rumbling;
uint32_t rumble_refresh;

const ImU32 kAccent = IM_COL32(238, 128, 33, 255);

GameSettings *gs() { return host.settings; }
int opacity() { return gs() ? gs()->touch_opacity : 55; }
float size_factor() { return gs() ? gs()->touch_size / 100.0f : 1.0f; }

bool any_controller() {
    for (int p = 0; p < 4; ++p) if (host.port_name && host.port_name(p)) return true;
    return false;
}

bool touch_wanted() {
    const int mode = gs() ? gs()->touch_mode : 0;
    if (mode == 2) return false;
    if (mode == 1) return true;
#ifdef __ANDROID__
    return !any_controller();
#else
    return SDL_GetNumTouchDevices() > 0 && !any_controller();
#endif
}

// Finger-sized controls from the panel density, capped so everything fits the screen.
void layout(int w, int h) {
    W = w; H = h;
    float u = dpi * 0.30f * size_factor();
    u = std::min(u, std::min(W, H) * 0.13f);
    u = std::max(u, 24.0f);
    /* Keep clear of Android's system gesture strips. The old layout put the
     * menu under the status-bar pull area, L/R under the cutout, and START on
     * the navigation gesture: tapping START could send the app Home. */
    const float top = 1.6f * u;
    const float ax = W - 2.5f * u, ay = H - 2.8f * u;
    ctl[C_STICK] = {"", K_STICK, 0, 2.4f * u, H - 2.6f * u, 1.55f * u};
    ctl[C_A] = {"A", K_BUTTON, B_A, ax, ay, 0.95f * u};
    ctl[C_B] = {"B", K_BUTTON, B_B, ax - 1.6f * u, ay + 0.75f * u, 0.62f * u};
    ctl[C_X] = {"X", K_BUTTON, B_X, ax + 1.5f * u, ay - 0.45f * u, 0.62f * u};
    ctl[C_Y] = {"Y", K_BUTTON, B_Y, ax - 0.5f * u, ay - 1.6f * u, 0.62f * u};
    ctl[C_Z] = {"Z", K_BUTTON, B_Z, ax + 1.5f * u, ay - 1.85f * u, 0.5f * u};
    ctl[C_L] = {"L", K_SHOULDER, B_L, 1.6f * u, top, 0.75f * u};
    ctl[C_R] = {"R", K_SHOULDER, B_R, W - 1.6f * u, top, 0.75f * u};
    ctl[C_START] = {"START", K_BUTTON, B_START, W * 0.5f, H - 1.6f * u, 0.5f * u};
    ctl[C_MENU] = {"", K_MENU, 0, W * 0.5f, top, 0.45f * u};
}

int hit(float x, float y, bool face_only) {
    int best = -1;
    float best_d = 1e9f;
    for (int i = 0; i < C_COUNT; ++i) {
        if (i == C_STICK) continue;
        if (face_only && ctl[i].kind != K_BUTTON) continue;
        const float d = std::hypot(x - ctl[i].x, y - ctl[i].y);
        if (d < ctl[i].r * 1.35f && d < best_d) { best = i; best_d = d; }
    }
    return best;
}

bool pressed(int c) {
    for (auto &kv : fingers) if (kv.second.ctrl == c) return true;
    return false;
}

void buzz() {
    if (haptic && gs() && gs()->touch_haptics) SDL_HapticRumblePlay(haptic, 0.35f, 18);
}

void draw_controls(ImDrawList *dl) {
    const float a = opacity() / 100.0f;
    const ImU32 fill = IM_COL32(255, 255, 255, int(40 * a)), line = IM_COL32(255, 255, 255, int(170 * a));
    const ImU32 on = IM_COL32(238, 128, 33, int(200 * a)), text = IM_COL32(255, 255, 255, int(230 * a));
    ImFont *font = ImGui::GetFont();
    for (int i = 0; i < C_COUNT; ++i) {
        const Control &c = ctl[i];
        const bool down = pressed(i);
        if (c.kind == K_STICK) {
            float bx = c.x, by = c.y, kx = c.x, ky = c.y;
            for (auto &kv : fingers)
                if (kv.second.ctrl == C_STICK) {
                    bx = kv.second.ox; by = kv.second.oy;
                    float dx = kv.second.x - bx, dy = kv.second.y - by, m = std::hypot(dx, dy);
                    if (m > c.r) { dx *= c.r / m; dy *= c.r / m; }
                    kx = bx + dx; ky = by + dy;
                }
            dl->AddCircleFilled(ImVec2(bx, by), c.r, fill, 48);
            dl->AddCircle(ImVec2(bx, by), c.r, line, 48, 2.5f * ui_scale);
            dl->AddCircleFilled(ImVec2(kx, ky), c.r * 0.42f, down ? on : IM_COL32(255, 255, 255, int(110 * a)), 32);
            continue;
        }
        if (c.kind == K_MENU) {
            dl->AddCircleFilled(ImVec2(c.x, c.y), c.r, down ? on : fill, 32);
            dl->AddCircle(ImVec2(c.x, c.y), c.r, line, 32, 2.0f * ui_scale);
            for (int k = -1; k <= 1; ++k)
                dl->AddLine(ImVec2(c.x - c.r * 0.45f, c.y + k * c.r * 0.32f), ImVec2(c.x + c.r * 0.45f, c.y + k * c.r * 0.32f),
                            text, 2.5f * ui_scale);
            continue;
        }
        if (c.kind == K_SHOULDER) {
            const ImVec2 p0(c.x - c.r * 1.3f, c.y - c.r * 0.6f), p1(c.x + c.r * 1.3f, c.y + c.r * 0.6f);
            dl->AddRectFilled(p0, p1, down ? on : fill, c.r * 0.6f);
            dl->AddRect(p0, p1, line, c.r * 0.6f, 0, 2.5f * ui_scale);
        } else {
            dl->AddCircleFilled(ImVec2(c.x, c.y), c.r, down ? on : fill, 40);
            dl->AddCircle(ImVec2(c.x, c.y), c.r, line, 40, 2.5f * ui_scale);
        }
        const float fs = c.kind == K_BUTTON && c.mask == B_START ? c.r * 0.55f : c.r * 0.85f;
        const ImVec2 ts = font->CalcTextSizeA(fs, 1e9f, 0, c.label);
        dl->AddText(font, fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), text, c.label);
    }
}

void changed() {
    if (host.settings_changed) host.settings_changed(host.user);
}

// Touch-drag scrolling for the current child window (ImGui has no kinetic scrolling).
void drag_scroll() {
    ImGuiIO &io = ImGui::GetIO();
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseDragging(0, 6.0f * ui_scale) &&
        !ImGui::IsAnyItemActive())
        ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
}

void draw_menu() {
    ImGuiIO &io = ImGui::GetIO();
    const ImVec2 size(std::min(io.DisplaySize.x * 0.9f, 620.0f * ui_scale), std::min(io.DisplaySize.y * 0.9f, 560.0f * ui_scale));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::Begin("Paused", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                    ImGuiWindowFlags_NoSavedSettings);
    const float bh = ImGui::GetFrameHeight() * 1.6f;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.43f, 0.10f, 1));
    if (ImGui::Button("Resume", ImVec2(ImGui::GetContentRegionAvail().x * 0.6f, bh))) action = OVERLAY_RESUME;
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::Button("Quit game", ImVec2(-1, bh))) ImGui::OpenPopup("quit?");
    if (ImGui::BeginPopupModal("quit?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextUnformatted("Quit the game? Unsaved progress since the last in-game save is lost.");
        if (ImGui::Button("Quit", ImVec2(8 * ImGui::GetFontSize(), bh))) { action = OVERLAY_QUIT; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(8 * ImGui::GetFontSize(), bh))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    GameSettings *s = gs();
    ImGui::BeginChild("settings", ImVec2(0, 0), ImGuiChildFlags_None);
    drag_scroll();
    if (s && ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Display")) {
            bool b = s->stretch;
            if (ImGui::Checkbox("Stretch to screen (instead of 4:3)", &b)) { s->stretch = b; changed(); }
            b = s->smooth;
            if (ImGui::Checkbox("Smooth scaling", &b)) { s->smooth = b; changed(); }
            b = s->show_fps;
            if (ImGui::Checkbox("Show FPS", &b)) { s->show_fps = b; changed(); }
            ImGui::TextDisabled("Internal resolution: %dx (change it in the launcher; applies on restart)", s->render_scale);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Audio")) {
            bool b = s->audio;
            ImGui::BeginDisabled();
            ImGui::Checkbox("Sound (set in the launcher)", &b);
            ImGui::EndDisabled();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("##vol", &s->volume, 0, 100, "Volume %d%%")) changed();
            static const int lat[] = {0, 50, 80, 120, 200, 350};
            static const char *names[] = {"Latency: Auto", "Latency: 50 ms", "Latency: 80 ms", "Latency: 120 ms",
                                          "Latency: 200 ms", "Latency: 350 ms"};
            int li = 0;
            for (int i = 0; i < 6; ++i) if (lat[i] == s->audio_latency) li = i;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##lat", &li, names, 6)) { s->audio_latency = lat[li]; changed(); }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Touch")) {
            static const char *modes[] = {"Auto (hide when a controller is connected)", "Always show", "Never show"};
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##mode", &s->touch_mode, modes, 3)) changed();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("##opacity", &s->touch_opacity, 10, 100, "Opacity %d%%")) changed();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("##size", &s->touch_size, 50, 200, "Size %d%%")) changed();
            bool b = s->touch_haptics;
            if (ImGui::Checkbox("Vibrate (buttons and game rumble)", &b)) { s->touch_haptics = b; changed(); }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Controllers")) {
            bool none = true;
            for (int p = 0; p < 4; ++p)
                if (const char *n = host.port_name ? host.port_name(p) : nullptr) { ImGui::BulletText("Player %d: %s", p + 1, n); none = false; }
            if (none) ImGui::TextWrapped("No controller connected. Pair a Bluetooth controller (Xbox, PlayStation, "
                                         "Switch Pro, 8BitDo...) in the system settings; it is picked up immediately.");
            bool b = s->input.rumble;
            if (ImGui::Checkbox("Controller rumble", &b)) { s->input.rumble = b; changed(); }
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("##dz", &s->input.deadzone, 0, 50, "Stick deadzone %d%%")) changed();
            static const char *ports[] = {"Touch/keyboard: Player 1", "Touch/keyboard: Player 2", "Touch/keyboard: Player 3",
                                          "Touch/keyboard: Player 4"};
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##kp", &s->input.keyboard_port, ports, 4)) changed();
            ImGui::TextDisabled("Button mapping is in the launcher's Controls tab.");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::End();
}

}  // namespace

bool overlay_init(SDL_Window *w, const OverlayHost &h) {
    window = w; host = h;
    float ddpi = 0;
    if (SDL_GetDisplayDPI(SDL_GetWindowDisplayIndex(w), &ddpi, nullptr, nullptr) == 0 && ddpi > 0) dpi = ddpi;
    ui_scale = ui_display_scale();
    IMGUI_CHECKVERSION();
    ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    ui_apply_style(ui_scale);
    ui_load_fonts(ui_scale);
    ImGui_ImplSDL2_InitForOpenGL(w, SDL_GL_GetCurrentContext());
#ifdef __ANDROID__
    ImGui_ImplOpenGL3_Init("#version 300 es");
#else
    ImGui_ImplOpenGL3_Init("#version 330");
#endif
    if (SDL_InitSubSystem(SDL_INIT_HAPTIC) == 0) {
        // On Android SDL lists the phone's own vibrator as a haptic device.
        for (int i = 0; i < SDL_NumHaptics() && !haptic; ++i) {
            const char *n = SDL_HapticName(i);
            if (SDL_NumHaptics() == 1 || (n && SDL_strcasestr(n, "vibrator"))) haptic = SDL_HapticOpen(i);
        }
        if (haptic && SDL_HapticRumbleInit(haptic) != 0) { SDL_HapticClose(haptic); haptic = nullptr; }
    }
    fprintf(stderr, "[touch] haptic %s (%d device(s)), touch_haptics=%d\n",
            haptic ? SDL_HapticName(SDL_HapticIndex(haptic)) : "none", SDL_NumHaptics(), gs() ? gs()->touch_haptics : -1);
    ready = true;
    return true;
}

void overlay_shutdown() {
    if (!ready) return;
    ImGui::SetCurrentContext(ctx);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext(ctx);
    ctx = nullptr;
    if (haptic) { SDL_HapticClose(haptic); haptic = nullptr; }
    fingers.clear();
    ready = menu = false;
}

bool overlay_menu_open() { return menu; }
void overlay_set_menu(bool open) {
    menu = open;
    fingers.clear();
}
int overlay_take_action() { const int a = action; action = OVERLAY_NONE; return a; }
bool overlay_touch_visible() { return ready && !menu && touch_wanted(); }

bool overlay_event(const SDL_Event &e) {
    if (!ready) return false;
    if (menu) {
        ImGui::SetCurrentContext(ctx);
        ImGui_ImplSDL2_ProcessEvent(&e);
        return e.type != SDL_QUIT && e.type != SDL_WINDOWEVENT && e.type != SDL_APP_WILLENTERBACKGROUND &&
               e.type != SDL_APP_TERMINATING && e.type != SDL_CONTROLLERDEVICEADDED && e.type != SDL_CONTROLLERDEVICEREMOVED;
    }
    if (!touch_wanted()) { fingers.clear(); return false; }
    if (e.type != SDL_FINGERDOWN && e.type != SDL_FINGERMOTION && e.type != SDL_FINGERUP) return false;
    const float x = e.tfinger.x * W, y = e.tfinger.y * H;
    if (e.type == SDL_FINGERDOWN) {
        int c = hit(x, y, false);
        if (c < 0 && x < W * 0.45f && y > H * 0.3f) c = C_STICK;
        if (c < 0) return false;
        Finger f{c, x, y, x, y};
        if (c == C_STICK) {   // the stick centres under the thumb, kept fully on screen
            const float r = ctl[C_STICK].r;
            f.ox = std::clamp(x, r, W - r); f.oy = std::clamp(y, r, H - r);
        } else buzz();
        fingers[e.tfinger.fingerId] = f;
        if (c != C_STICK) press_until[c] = SDL_GetTicks() + kMinPressMs;
        return true;
    }
    auto it = fingers.find(e.tfinger.fingerId);
    if (it == fingers.end()) return false;
    Finger &f = it->second;
    if (e.type == SDL_FINGERMOTION) {
        f.x = x; f.y = y;
        if (ctl[f.ctrl].kind == K_BUTTON && ctl[f.ctrl].mask != B_START) {   // slide across face buttons
            const int c = hit(x, y, true);
            if (c >= 0 && c != f.ctrl && ctl[c].mask != B_START) { f.ctrl = c; buzz(); }
        }
        return true;
    }
    const int c = f.ctrl;
    fingers.erase(it);
    if (c == C_MENU) overlay_set_menu(true);
    return true;
}

void overlay_pad(UpstreamGxPad &pad) {
    if (!ready || menu || !touch_wanted()) return;
    const uint32_t now = SDL_GetTicks();
    for (int i = 0; i < C_COUNT; ++i) {   // taps already released, still within their minimum
        if (!press_until[i] || (int32_t)(press_until[i] - now) <= 0) { press_until[i] = 0; continue; }
        if (ctl[i].kind == K_BUTTON && ctl[i].mask) {
            pad.buttons |= ctl[i].mask;
            if (ctl[i].mask == B_L) pad.trigger_l = 255;
            if (ctl[i].mask == B_R) pad.trigger_r = 255;
        }
    }
    for (auto &kv : fingers) {
        const Finger &f = kv.second;
        const Control &c = ctl[f.ctrl];
        if (c.kind == K_STICK) {
            float dx = (f.x - f.ox) / c.r, dy = (f.y - f.oy) / c.r;
            const float m = std::hypot(dx, dy);
            if (m > 1) { dx /= m; dy /= m; }
            if (m > 0.12f) {
                pad.stick_x = uint8_t(std::clamp(128 + int(std::lround(dx * 127)), 1, 255));
                pad.stick_y = uint8_t(std::clamp(128 - int(std::lround(dy * 127)), 1, 255));
            }
        } else if (c.mask) {
            pad.buttons |= c.mask;
            if (c.mask == B_L) pad.trigger_l = 255;
            if (c.mask == B_R) pad.trigger_r = 255;
        }
    }
}

void overlay_rumble(bool on) {
    if (!haptic || !gs() || !gs()->touch_haptics || !touch_wanted()) on = false;
    const uint32_t now = SDL_GetTicks();
    if (on && (!rumbling || now - rumble_refresh > 400)) { SDL_HapticRumblePlay(haptic, 0.6f, 600); rumble_refresh = now; }
    else if (!on && rumbling && haptic) SDL_HapticRumbleStop(haptic);
    rumbling = on;
}

void overlay_draw(int dw, int dh, int fps, bool show_fps) {
    if (!ready) return;
    layout(dw, dh);
    const bool touch = touch_wanted() && !menu;
    if (!touch && !menu && !show_fps) {
        if (menu_was_open) menu_was_open = false;
        return;
    }
    ImGui::SetCurrentContext(ctx);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    if (touch) draw_controls(dl);
    if (show_fps) {
        char t[32]; snprintf(t, sizeof(t), "%d FPS", fps);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.1f, ImVec2(12 * ui_scale, 8 * ui_scale), kAccent, t);
    }
    if (menu) {
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImGui::GetIO().DisplaySize, IM_COL32(0, 0, 0, 140));
        draw_menu();
    }
    menu_was_open = menu;
    ImGui::Render();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}
