#include "launcher.h"
#include "ui_style.h"
#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"
#include <SDL.h>
#ifdef __ANDROID__
#include <GLES3/gl3.h>
#include "android_glue.h"
#else
#include <SDL_opengl.h>
#endif
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <SDL_syswm.h>
#endif

#ifndef __ANDROID__
// Background art, embedded by CMake (launcher_bg.jpg -> launcher_bg.c) and decoded once.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"
extern "C" const unsigned char launcher_bg_jpg[];
extern "C" const unsigned long launcher_bg_jpg_size;
#endif

namespace {

#ifndef __ANDROID__
struct Background { GLuint tex = 0; int w = 0, h = 0; };

Background load_background() {
    Background bg;
    int n = 0;
    unsigned char *px = stbi_load_from_memory(launcher_bg_jpg, (int)launcher_bg_jpg_size, &bg.w, &bg.h, &n, 4);
    if (!px) { fprintf(stderr, "[launcher] background: %s\n", stbi_failure_reason()); return bg; }
    glGenTextures(1, &bg.tex);
    glBindTexture(GL_TEXTURE_2D, bg.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, bg.w, bg.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    stbi_image_free(px);
    return bg;
}

// Fills the window, cropping the art (cover), then darkens it so the UI stays readable.
void draw_background(const Background &bg, ImVec2 size) {
    if (!bg.tex || size.x <= 0 || size.y <= 0) return;
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    const float img = (float)bg.w / bg.h, win = size.x / size.y;
    ImVec2 uv0(0, 0), uv1(1, 1);
    if (win > img) { const float v = img / win; uv0.y = (1 - v) * 0.5f; uv1.y = uv0.y + v; }
    else { const float u = win / img; uv0.x = (1 - u) * 0.5f; uv1.x = uv0.x + u; }
    dl->AddImage((ImTextureID)(intptr_t)bg.tex, ImVec2(0, 0), size, uv0, uv1);
    dl->AddRectFilledMultiColor(ImVec2(0, 0), size, IM_COL32(8, 10, 20, 110), IM_COL32(8, 10, 20, 110),
                                IM_COL32(8, 10, 20, 200), IM_COL32(8, 10, 20, 200));
}
#endif

struct Preset { int w, h; const char *note; };
const Preset kWindowSizes[] = {
    {640, 480, "1x"}, {960, 720, "1.5x"}, {1280, 960, "2x"}, {1440, 1080, "fits 1080p"},
    {1600, 1200, "2.5x"}, {1920, 1440, "3x"}, {1280, 720, "16:9 window"}, {1920, 1080, "16:9 window"},
    {2560, 1440, "16:9 window"}};
const int kPresetCount = int(sizeof(kWindowSizes) / sizeof(kWindowSizes[0]));

struct State {
    GameSettings *s;
    std::string disc_label;   // Android: picked file's name (the path is /proc/self/fd/N)
    std::string checked_disc;
    bool checked = false, disc_ok = false;
    char disc_msg[256] = "";
    int capture_kind = 0;   // 1 = controller, 2 = keyboard
    int capture_index = -1;
    std::vector<SDL_GameController *> pads;
    bool saved_flash = false;
    uint32_t saved_at = 0;
};

void check_disc(State &st) {
    if (st.checked && st.checked_disc == st.s->disc) return;
    st.checked = true;
    st.checked_disc = st.s->disc;
    st.disc_ok = settings_check_disc(st.s->disc, st.disc_msg, sizeof(st.disc_msg)) != 0;
}

void set_disc(State &st, const char *path) {
    snprintf(st.s->disc, sizeof(st.s->disc), "%s", path);
}

#ifdef _WIN32
std::wstring widen(const std::string &s) {
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), int(w.size()));
    if (!w.empty()) w.pop_back();
    return w;
}
bool browse_disc(SDL_Window *owner, State &st) {
    wchar_t file[1024] = L"";
    std::wstring dir = widen(st.s->disc);
    size_t slash = dir.find_last_of(L"\\/");
    dir = slash == std::wstring::npos ? L"" : dir.substr(0, slash);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    SDL_SysWMinfo wm; SDL_VERSION(&wm.version);
    if (SDL_GetWindowWMInfo(owner, &wm)) ofn.hwndOwner = wm.info.win.window;
    ofn.lpstrFilter = L"GameCube disc image (*.iso;*.gcm;*.rvz;*.gcz;*.ciso)\0*.iso;*.gcm;*.rvz;*.gcz;*.ciso\0All files\0*.*\0";
    ofn.lpstrFile = file; ofn.nMaxFile = 1024;
    ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    ofn.lpstrTitle = L"Choose your Digimon Battle Chronicle disc image";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;
    char utf8[4096];
    WideCharToMultiByte(CP_UTF8, 0, file, -1, utf8, sizeof(utf8), nullptr, nullptr);
    set_disc(st, utf8);
    return true;
}
#endif

void autodetect_disc(State &st) {
    if (st.s->disc[0]) return;
    std::vector<std::string> candidates = {"disc.iso"};
    if (char *base = SDL_GetBasePath()) {
        candidates.push_back(std::string(base) + "disc.iso");
        candidates.push_back(std::string(base) + "../../disc.iso");   // port/build/windows/
        SDL_free(base);
    }
    char msg[256], full[4096];   // >= PATH_MAX for realpath
    for (const auto &c : candidates) {
        if (!settings_check_disc(c.c_str(), msg, sizeof(msg))) continue;
#ifdef _WIN32
        set_disc(st, _fullpath(full, c.c_str(), sizeof(full)) ? full : c.c_str());
#else
        set_disc(st, realpath(c.c_str(), full) ? full : c.c_str());
#endif
        return;
    }
}

void open_pad(State &st, int index) {
    if (!SDL_IsGameController(index)) return;
    SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(index);
    for (auto *p : st.pads) if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(p)) == id) return;
    if (SDL_GameController *c = SDL_GameControllerOpen(index)) st.pads.push_back(c);
}

void close_pad(State &st, SDL_JoystickID id) {
    for (size_t i = 0; i < st.pads.size(); ++i)
        if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(st.pads[i])) == id) {
            SDL_GameControllerClose(st.pads[i]); st.pads.erase(st.pads.begin() + long(i)); return;
        }
}

// Binding capture consumes the event so it never also drives the UI.
bool capture_event(State &st, const SDL_Event &e) {
    if (!st.capture_kind) return false;
    UpstreamGxInput &in = st.s->input;
    if (e.type == SDL_KEYDOWN) {
        const SDL_Scancode sc = e.key.keysym.scancode;
        if (sc == SDL_SCANCODE_ESCAPE) st.capture_kind = 0;
        else if (sc == SDL_SCANCODE_BACKSPACE || sc == SDL_SCANCODE_DELETE) {
            if (st.capture_kind == 1) in.pad[st.capture_index] = GC_BIND_NONE; else in.key[st.capture_index] = 0;
            st.capture_kind = 0;
        } else if (st.capture_kind == 2) { in.key[st.capture_index] = sc; st.capture_kind = 0; }
        return true;
    }
    if (st.capture_kind == 1 && e.type == SDL_CONTROLLERBUTTONDOWN) {
        in.pad[st.capture_index] = e.cbutton.button; st.capture_kind = 0; return true;
    }
    if (st.capture_kind == 1 && e.type == SDL_CONTROLLERAXISMOTION && e.caxis.value > 16000 &&
        (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) {
        in.pad[st.capture_index] = GC_BIND_AXIS + e.caxis.axis; st.capture_kind = 0; return true;
    }
    return e.type == SDL_KEYUP || e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP ||
           e.type == SDL_CONTROLLERAXISMOTION;
}

void help(const char *text) {
    ImGui::SameLine(); ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

void section(const char *title) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.62f, 0.22f, 1.0f));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::Separator();
}

void tab_game(State &st, SDL_Window *window) {
    GameSettings &s = *st.s;
    section("Disc image");
#ifdef __ANDROID__
    (void)window;
    ImGui::TextWrapped("%s", st.disc_label.empty() ? (s.disc[0] ? "Disc chosen earlier" : "No disc chosen yet") : st.disc_label.c_str());
    if (ImGui::Button("Choose disc image...", ImVec2(-1, ImGui::GetFrameHeight() * 1.5f))) android_pick_disc();
#else
    ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 7);
    ImGui::InputTextWithHint("##disc", "Path to Digimon Battle Chronicle (.iso / .gcm / .rvz / .gcz / .ciso)", s.disc, sizeof(s.disc));
#ifdef _WIN32
    ImGui::SameLine();
    if (ImGui::Button("Browse...", ImVec2(-1, 0))) browse_disc(window, st);
#else
    (void)window;
#endif
#endif
    check_disc(st);
    ImGui::PushStyleColor(ImGuiCol_Text, st.disc_ok ? ImVec4(0.45f, 0.85f, 0.45f, 1) : ImVec4(0.95f, 0.45f, 0.4f, 1));
    ImGui::TextWrapped("%s %s", st.disc_ok ? "OK:" : "!", st.disc_msg);
    ImGui::PopStyleColor();
#ifdef __ANDROID__
    ImGui::TextDisabled("Copy your own Japanese disc image (G2DJB2, .iso) to the phone, e.g. Downloads, then choose it here.");
#else
    ImGui::TextDisabled("Tip: drag and drop the disc image onto this window. Your own Japanese disc (G2DJB2) is required.");
#endif

    section("Startup");
    bool skip = s.skip_launcher;
    if (ImGui::Checkbox("Start the game directly next time", &skip)) s.skip_launcher = skip;
#ifdef __ANDROID__
    help("Skips this screen when the disc is valid. Clear the app's data to see it again.");
#else
    help("Skips this launcher when the disc is valid. Run main_gx --setup to open it again.");
#endif

    section("Saving");
    bool card = s.memcard;
    if (ImGui::Checkbox("Memory card in slot A", &card)) s.memcard = card;
    help("A 251-block card stored as MemoryCardA.raw in the data folder. It is created (already formatted) "
         "the first time you play, and written back about a second after the game saves.");
    ImGui::BeginDisabled(!s.memcard);
#ifdef _WIN32
    ImGui::TextDisabled("%s\\MemoryCardA.raw", settings_data_dir());
#else
    ImGui::TextDisabled("%s/MemoryCardA.raw", settings_data_dir());
#endif
#ifndef __ANDROID__
    if (ImGui::Button("Open saves folder")) {
        std::string url = std::string("file:///") + settings_data_dir();
        SDL_OpenURL(url.c_str());
    }
#endif
    ImGui::EndDisabled();
#ifndef __ANDROID__
    ImGui::TextDisabled("Back up saves by copying this file. Dolphin .raw cards can be dropped in as MemoryCardA.raw.");
#endif
}

void tab_display(State &st) {
    GameSettings &s = *st.s;
    bool b;
#ifndef __ANDROID__
    section("Window");
    int current = -1;
    for (int i = 0; i < kPresetCount; ++i)
        if (kWindowSizes[i].w == s.window_width && kWindowSizes[i].h == s.window_height) current = i;
    char label[64];
    if (current >= 0) snprintf(label, sizeof(label), "%d x %d  (%s)", s.window_width, s.window_height, kWindowSizes[current].note);
    else snprintf(label, sizeof(label), "%d x %d  (custom)", s.window_width, s.window_height);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
    if (ImGui::BeginCombo("Window size", label)) {
        for (int i = 0; i < kPresetCount; ++i) {
            snprintf(label, sizeof(label), "%d x %d  (%s)", kWindowSizes[i].w, kWindowSizes[i].h, kWindowSizes[i].note);
            if (ImGui::Selectable(label, i == current)) { s.window_width = kWindowSizes[i].w; s.window_height = kWindowSizes[i].h; }
        }
        ImGui::EndCombo();
    }
    int wh[2] = {s.window_width, s.window_height};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
    if (ImGui::InputInt2("Custom size", wh)) {
        s.window_width = std::clamp(wh[0], 320, 16384); s.window_height = std::clamp(wh[1], 240, 16384);
    }
    b = s.fullscreen;
    if (ImGui::Checkbox("Fullscreen", &b)) s.fullscreen = b;
    help("Borderless fullscreen at the desktop resolution. Toggle in game with F11 or Alt+Enter.");
#endif

    section("Rendering");
    static const char *scales[] = {"1x  native (640 x 480)", "2x  (1280 x 960)", "3x  (1920 x 1440)", "4x  (2560 x 1920)"};
    int scale = std::clamp(s.render_scale, 1, 4) - 1;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
    if (ImGui::Combo("Internal resolution", &scale, scales, 4)) s.render_scale = scale + 1;
    help("Renders the GameCube's 640x480 frame at a higher resolution. Higher values need a faster GPU.");
    static const char *aspects[] = {"4:3  original (black bars)", "Stretch to window"};
    int aspect = s.stretch ? 1 : 0;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
    if (ImGui::Combo("Aspect ratio", &aspect, aspects, 2)) s.stretch = aspect;
    b = s.smooth;
    if (ImGui::Checkbox("Smooth scaling", &b)) s.smooth = b;
    help("Linear filtering when the frame is scaled to the window. Off = sharp nearest-neighbour.");
    b = s.vsync;
    if (ImGui::Checkbox("VSync", &b)) s.vsync = b;
    help("Waits for the monitor refresh when presenting. Reduces tearing; game speed is still paced by the emulated clock.");

    section("Behaviour");
    b = s.pause_unfocused;
    if (ImGui::Checkbox("Pause when the window loses focus", &b)) s.pause_unfocused = b;
}

void tab_audio(State &st) {
    GameSettings &s = *st.s;
    section("Sound");
    bool b = s.audio;
    if (ImGui::Checkbox("Enable sound", &b)) s.audio = b;
    ImGui::BeginDisabled(!s.audio);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
    ImGui::SliderInt("Volume", &s.volume, 0, 100, "%d%%");
    static const int latencies[] = {0, 50, 80, 120, 200, 350};
    static const char *latency_names[] = {"Auto (adaptive, 80-350 ms)", "50 ms", "80 ms", "120 ms", "200 ms", "350 ms (most stable)"};
    int li = 0;
    for (int i = 0; i < 6; ++i) if (latencies[i] == s.audio_latency) li = i;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
    if (ImGui::Combo("Audio latency", &li, latency_names, 6)) s.audio_latency = latencies[li];
    help("How much sound is buffered ahead. Lower = sound closer to the action. Auto starts low and only "
         "grows if this PC's loading stalls make the sound drop out; lower fixed values may crackle on slow PCs.");
    ImGui::EndDisabled();
}

// Xbox-style labels for SDL's positional controller names.
const char *pad_label(int b) {
    if (b == GC_BIND_AXIS + SDL_CONTROLLER_AXIS_TRIGGERLEFT) return "LT / L2";
    if (b == GC_BIND_AXIS + SDL_CONTROLLER_AXIS_TRIGGERRIGHT) return "RT / R2";
    switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return "A (bottom)";
    case SDL_CONTROLLER_BUTTON_B: return "B (right)";
    case SDL_CONTROLLER_BUTTON_X: return "X (left)";
    case SDL_CONTROLLER_BUTTON_Y: return "Y (top)";
    case SDL_CONTROLLER_BUTTON_BACK: return "Back / Select";
    case SDL_CONTROLLER_BUTTON_GUIDE: return "Guide";
    case SDL_CONTROLLER_BUTTON_START: return "Start";
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return "L3";
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "R3";
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "LB / L1";
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "RB / R1";
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return "D-Pad Up";
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "D-Pad Down";
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "D-Pad Left";
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "D-Pad Right";
    case GC_BIND_NONE: return "-";
    default: return settings_pad_binding_name(b);
    }
}

void binding_button(State &st, int kind, int index) {
    const UpstreamGxInput &in = st.s->input;
    std::string text;
    if (st.capture_kind == kind && st.capture_index == index) text = "Press... (Esc cancel)";
    else if (kind == 1) text = pad_label(in.pad[index]);
    else text = in.key[index] > 0 ? SDL_GetScancodeName(SDL_Scancode(in.key[index])) : "-";
    ImGui::PushID(kind * 100 + index);
    if (ImGui::Button(text.c_str(), ImVec2(-1, 0))) { st.capture_kind = kind; st.capture_index = index; }
    ImGui::PopID();
}

void tab_controls(State &st) {
    GameSettings &s = *st.s;
    UpstreamGxInput &in = s.input;
    section("Players");
    if (st.pads.empty()) ImGui::TextDisabled("No controller connected. Xbox, PlayStation, Switch Pro and most SDL pads work.");
    for (size_t i = 0; i < st.pads.size() && i < 4; ++i)
        ImGui::BulletText("Player %d: %s", int(i) + 1, SDL_GameControllerName(st.pads[i]));
    int kb = in.keyboard_port;
    static const char *ports[] = {"Player 1", "Player 2", "Player 3", "Player 4"};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    if (ImGui::Combo("Keyboard controls", &kb, ports, 4)) in.keyboard_port = kb;
    help("Controllers take players 1-4 in the order they are connected. The keyboard adds to the chosen player.");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    ImGui::SliderInt("Stick deadzone", &in.deadzone, 0, 50, "%d%%");
    bool b = in.rumble;
    if (ImGui::Checkbox("Rumble", &b)) in.rumble = b;

    section("Button mapping  (click, then press a button or key; Backspace clears)");
    if (ImGui::BeginTable("bind", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("GameCube", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Controller", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("Keyboard", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < GC_IN_COUNT; ++i) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(settings_input_label(i));
            ImGui::TableNextColumn();
            if (i < GC_IN_PAD_COUNT) binding_button(st, 1, i);
            else ImGui::TextDisabled(i < GC_IN_C_UP ? "Left stick" : "Right stick");
            ImGui::TableNextColumn(); binding_button(st, 2, i);
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Reset controls to defaults")) { upstream_gx_input_defaults(&in); st.capture_kind = 0; }

    section("Touch controls");
    static const char *modes[] = {"Auto (hide when a controller is connected)", "Always show", "Never show"};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
    ImGui::Combo("On-screen buttons", &s.touch_mode, modes, 3);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
    ImGui::SliderInt("Opacity", &s.touch_opacity, 10, 100, "%d%%");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
    ImGui::SliderInt("Size", &s.touch_size, 50, 200, "%d%%");
    b = s.touch_haptics;
    if (ImGui::Checkbox("Vibrate on press and for game rumble", &b)) s.touch_haptics = b;

    section("In game");
#ifdef __ANDROID__
    ImGui::TextWrapped("Tap the menu button at the top or press Back to pause and change settings. Bluetooth "
                       "controllers paired in the system settings work right away and take players 1-4.");
#else
    ImGui::TextUnformatted("F1 / Esc  pause menu & settings      F11 / Alt+Enter  fullscreen\n"
                           "F8  pause      F9  FPS      F12  screenshot");
#endif
}

void tab_about() {
    section("Digimon Battle Chronicle");
    ImGui::TextWrapped("The GameCube program is statically recompiled to native code; graphics use the pinned gcn-recomp "
                       "renderer over OpenGL and audio the gcn-recomp AX mixer. No game data ships with this program.");
    section("Files");
    ImGui::TextWrapped("Settings, saves, logs and screenshots: %s", settings_data_dir());
#ifndef __ANDROID__
    if (ImGui::Button("Open data folder")) {
        std::string url = std::string("file:///") + settings_data_dir();
        SDL_OpenURL(url.c_str());
    }
    ImGui::TextDisabled("Portable mode: put an empty portable.txt next to the program to keep data in a user folder there.");
#endif
    ImGui::TextDisabled("Extra controller mappings: put gamecontrollerdb.txt in the data folder.");
    section("Third-party");
    ImGui::TextWrapped("gcn-recomp (MIT), Dear ImGui (MIT), SDL2 (zlib), glad (MIT/Apache-2.0).");
}

// Touch-drag scrolling inside the content area (ImGui has no kinetic scrolling).
void drag_scroll() {
    ImGuiIO &io = ImGui::GetIO();
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseDragging(0, 8.0f) && !ImGui::IsAnyItemActive())
        ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
}

}  // namespace

extern "C" int launcher_run(GameSettings *settings) {
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "[launcher] SDL: %s\n", SDL_GetError());
        return settings->disc[0] ? 1 : 0;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
#ifdef __ANDROID__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    const Uint32 window_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    const Uint32 window_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#endif
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    const float scale = ui_display_scale();
    SDL_Window *window = SDL_CreateWindow("Digimon Battle Chronicle - Launcher", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          int(820 * scale), int(700 * scale), window_flags);
    SDL_GLContext gl = window ? SDL_GL_CreateContext(window) : nullptr;
    if (!gl) {
        fprintf(stderr, "[launcher] window: %s\n", SDL_GetError());
        if (window) SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER);
        return settings->disc[0] ? 1 : 0;
    }
    SDL_GL_MakeCurrent(window, gl);
    SDL_GL_SetSwapInterval(1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ui_apply_style(scale);
    ImGui::GetStyle().WindowRounding = 0;
    ui_load_fonts(scale);
    ImGui_ImplSDL2_InitForOpenGL(window, gl);
#ifdef __ANDROID__
    ImGui_ImplOpenGL3_Init("#version 300 es");
#else
    ImGui_ImplOpenGL3_Init("#version 330");
#endif

    State st; st.s = settings;
#ifndef __ANDROID__
    autodetect_disc(st);
    const Background bg = load_background();
#endif
    for (int i = 0; i < SDL_NumJoysticks(); ++i) open_pad(st, i);
    int result = -1;
    while (result < 0) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_CONTROLLERDEVICEADDED) open_pad(st, e.cdevice.which);
            else if (e.type == SDL_CONTROLLERDEVICEREMOVED) close_pad(st, e.cdevice.which);
            else if (e.type == SDL_DROPFILE) { set_disc(st, e.drop.file); SDL_free(e.drop.file); continue; }
            if (capture_event(st, e)) continue;
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT || (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE)) result = 0;
        }
#ifdef __ANDROID__
        char picked[1024], name[512];
        if (android_take_picked_disc(picked, sizeof(picked), name, sizeof(name))) { set_disc(st, picked); st.disc_label = name; }
#endif
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
#ifndef __ANDROID__
        draw_background(bg, io.DisplaySize);
        // The art shows through the window; panels keep a dark glass so text stays legible.
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, bg.tex ? 0.0f : 0.97f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.07f, 0.11f, bg.tex ? 0.72f : 1.0f));
#endif
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::SetWindowFontScale(1.45f);
        ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.22f, 1), "Digimon Battle Chronicle");
        ImGui::SetWindowFontScale(1.0f);
#ifdef __ANDROID__
        ImGui::SameLine(); ImGui::TextDisabled("  GameCube recompilation for Android");
#else
        ImGui::SameLine(); ImGui::TextDisabled("  GameCube recompilation for desktop");
#endif
        ImGui::Spacing();
        const float footer = ImGui::GetFrameHeightWithSpacing() * 1.6f + ImGui::GetStyle().ItemSpacing.y * 2;
        ImGui::BeginChild("content", ImVec2(0, -footer), ImGuiChildFlags_AlwaysUseWindowPadding);
        drag_scroll();
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("Game")) { tab_game(st, window); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Display")) { tab_display(st); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Audio")) { tab_audio(st); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Controls")) { tab_controls(st); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("About")) { tab_about(); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();
        ImGui::Separator();
        check_disc(st);
        const float h = ImGui::GetFrameHeight() * 1.5f;
        ImGui::BeginDisabled(!st.disc_ok);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.43f, 0.10f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.97f, 0.55f, 0.18f, 1));
        if (ImGui::Button("  Play  ", ImVec2(ImGui::GetFontSize() * 9, h))) result = 1;
        ImGui::PopStyleColor(2);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Save settings", ImVec2(0, h))) {
            st.saved_flash = settings_save(settings) == 0; st.saved_at = SDL_GetTicks();
        }
        ImGui::SameLine();
        if (ImGui::Button("Quit", ImVec2(0, h))) result = 0;
        if (st.saved_flash && SDL_GetTicks() - st.saved_at < 2000) { ImGui::SameLine(); ImGui::TextDisabled("Saved."); }
        ImGui::End();
#ifndef __ANDROID__
        ImGui::PopStyleColor(2);
#endif
        ImGui::Render();
        int w = 0, hgt = 0; SDL_GL_GetDrawableSize(window, &w, &hgt);
        glViewport(0, 0, w, hgt);
        glClearColor(0.075f, 0.085f, 0.11f, 1); glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }
    if (result == 1 && settings_save(settings) != 0)
        fprintf(stderr, "[launcher] cannot save settings to %s\n", settings_data_dir());
#ifndef __ANDROID__
    if (bg.tex) glDeleteTextures(1, &bg.tex);
#endif
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    for (auto *p : st.pads) SDL_GameControllerClose(p);
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER);
    return result;
}
