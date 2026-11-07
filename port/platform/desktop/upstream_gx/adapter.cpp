#include "upstream_gx.h"
#include "overlay.h"
#include "upstream_runtime.h"
#include "gx.h"
#include "render_gl.h"
#include "gl.h"
#include <SDL.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

// Upstream graphics exclusively; the original port owns all guest state.
uint8_t *g_mem;
static UpstreamGxConfig cfg{};
static SDL_Window *window;
static SDL_GLContext context;
static SDL_GameController *ports[4];   // controllers in connection order = GC ports 1-4
static bool rumbling[4];
static uint32_t rumble_refresh[4];
static UpstreamGxInput input_cfg;
static bool input_ready;
static GLuint output_fbo, output_tex;
static std::vector<uint8_t> pending;
static std::string error_text;
static bool sdl_owned, new_frame;
static bool stream_failed;
static std::atomic<bool> paused{false};
static bool auto_paused, quit_requested, show_fps;
static uint32_t fps_t0, fps_frames, fps_value;
static std::atomic<uint32_t> presented_count{0};
static std::atomic<int> guest_paused{0};
static bool suspend_pending;
/* Input is sampled on the GL/render thread (SDL and the touch overlay live there)
 * and copied to the guest thread between frames. */
static std::mutex pad_mutex;
static UpstreamGxPad pad_snapshot[4];
static unsigned pad_mask_snapshot = 1;
static std::atomic<int> rumble_request[4];
static std::atomic<int> rumble_applied[4];
static unsigned read_pads(UpstreamGxPad out[4]);
static void apply_rumble_requests(void);
static bool owner() { return context && SDL_GL_GetCurrentContext() == context; }
bool write_png(const char *, const uint8_t *, int, int);
namespace gx {
void adapter_frontend_reset();
void adapter_texture_reset();
void adapter_render_reset();
void adapter_render_shutdown();
// GCN_STALLS=1: microseconds each pipeline wait cost, summed and printed every 300
// presented frames (upstream_gx_present). Off: one cached getenv per wait.
static std::atomic<uint64_t> stall_us[(int)Stall::Count];
static uint64_t stall_now_us() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool stalls_on() { static const bool on = getenv("GCN_STALLS") != nullptr; return on; }
StallTimer::StallTimer(Stall st) : s(st), t0(stalls_on() ? stall_now_us() : 0) {}
StallTimer::~StallTimer() { if (t0) stall_add(s, stall_now_us() - t0); }
void stall_add(Stall st, uint64_t us) { stall_us[(int)st].fetch_add(us, std::memory_order_relaxed); }
void adapter_execute_batch(Batch& b) { if (render_execute(b)) new_frame = true; }
}
[[noreturn]] void fatal(const char *fmt, ...) {
    char text[2048]; va_list ap; va_start(ap, fmt); vsnprintf(text, sizeof(text), fmt, ap); va_end(ap);
    throw std::runtime_error(text);
}
uint8_t *upstream_mem_ptr(uint32_t address) {
    uint32_t pa = address & 0x01ffffffu;
    if (!g_mem || pa >= RAM_SIZE) fatal("GX RAM address outside MEM1: %08x", address);
    return g_mem + pa;
}
uint32_t vi_display_height() { return cfg.vi_height ? cfg.vi_height(cfg.user) : 0; }
void debug_guest_check(const char *) { if (cfg.debug_frame) cfg.debug_frame(gx::g_frames_submitted.load(), cfg.user); }
void debug_guard_dma(const char *what, uint32_t address, uint32_t length) {
    if ((address & 0x01ffffffu) > RAM_SIZE || length > RAM_SIZE - (address & 0x01ffffffu)) fatal("GX DMA outside MEM1");
    if (cfg.debug_dma) cfg.debug_dma(what, address, length, cfg.user);
}
void heap_trace_frame(uint32_t) {}
void pe_signal_finish() { if (cfg.pe_finish) cfg.pe_finish(cfg.user); }
void pe_signal_token(uint16_t token, bool interrupt) { if (cfg.pe_token) cfg.pe_token(token, interrupt, cfg.user); }
const char *func_name(uint32_t) { return "guest"; }
static bool copy_writeback(uint32_t, uint32_t, uint32_t, uint32_t, bool) { return true; }
extern "C" const char *upstream_gx_error() { return error_text.c_str(); }
extern "C" uint32_t upstream_gx_frame_count() { return presented_count.load(std::memory_order_relaxed); }

static int port_of(SDL_JoystickID id) {
    for (int p = 0; p < 4; ++p)
        if (ports[p] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(ports[p])) == id) return p;
    return -1;
}
static void open_controller(int device_index) {
    if (!SDL_IsGameController(device_index) || port_of(SDL_JoystickGetDeviceInstanceID(device_index)) >= 0) return;
    int p = 0;
    while (p < 4 && ports[p]) ++p;
    if (p == 4) return;
    if (!(ports[p] = SDL_GameControllerOpen(device_index))) return;
    fprintf(stderr, "[pad] port %d: %s\n", p + 1, SDL_GameControllerName(ports[p]));
}
static void close_controller(SDL_JoystickID id) {
    int p = port_of(id);
    if (p < 0) return;
    fprintf(stderr, "[pad] port %d disconnected\n", p + 1);
    SDL_GameControllerClose(ports[p]); ports[p] = nullptr; rumbling[p] = false;
}
static const char *port_name(int p) {
    return p >= 0 && p < 4 && ports[p] ? SDL_GameControllerName(ports[p]) : nullptr;
}
static void set_menu(bool open) {
    if (overlay_menu_open() == open) return;
    overlay_set_menu(open);
    paused = open; auto_paused = false;
}
static void update_title() {
    if (!window || cfg.hidden) return;
    std::string t = cfg.title ? cfg.title : "Digimon Battle GC";
    if (show_fps) t += " | " + std::to_string(fps_value) + " FPS";
    if (paused) t += " | Paused (F8)";
    SDL_SetWindowTitle(window, t.c_str());
}
static void save_screenshot() {
    if (!cfg.data_dir) return;
    char name[64]; time_t now = time(nullptr);
    strftime(name, sizeof(name), "digimon-%Y%m%d-%H%M%S.png", localtime(&now));
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::u8path(cfg.data_dir) / "screenshots";
    std::filesystem::create_directories(dir, ec);
    std::string path = (dir / name).u8string();
    fprintf(stderr, "[upstream-gx] screenshot %s %s\n", path.c_str(), upstream_gx_capture_png(path.c_str()) ? "failed" : "saved");
}
// Scale the output framebuffer into the drawable: 4:3 with black bars, or stretched.
static void blit_window() {
    GLint viewport[4]; GLfloat clear[4];
    glGetIntegerv(GL_VIEWPORT, viewport); glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
    int dw = 0, dh = 0; SDL_GL_GetDrawableSize(window, &dw, &dh);
    int x = 0, y = 0, w = dw, h = dh;
    if (!cfg.stretch) {
        h = dw * 3 / 4;
        if (h > dh) { h = dh; w = dh * 4 / 3; }
        x = (dw - w) / 2; y = (dh - h) / 2;
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, dw, dh); glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, output_fbo);
    const bool scaled = w != cfg.width || h != cfg.height;
    glBlitFramebuffer(0, 0, cfg.width, cfg.height, x, y, x + w, y + h, GL_COLOR_BUFFER_BIT,
                      cfg.smooth && scaled ? GL_LINEAR : GL_NEAREST);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glClearColor(clear[0], clear[1], clear[2], clear[3]);
}
static void handle_event(const SDL_Event &e) {
    // App lifecycle first: Android may kill a backgrounded app without another event.
    if (e.type == SDL_APP_WILLENTERBACKGROUND || e.type == SDL_APP_TERMINATING) {
        // Pause first. On Android the guest may still be between frames; the
        // memory card is flushed from present() once it has acknowledged the pause.
        if (cfg.settings) set_menu(true);   // come back paused
        if (e.type == SDL_APP_TERMINATING) {
            if (cfg.on_suspend) cfg.on_suspend(cfg.user);
        } else {
            suspend_pending = true;
        }
    }
    if (e.type == SDL_KEYDOWN && !e.key.repeat &&
        (e.key.keysym.scancode == SDL_SCANCODE_AC_BACK || e.key.keysym.sym == SDLK_F1 ||
         (e.key.keysym.sym == SDLK_ESCAPE && !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP))) &&
        cfg.settings) {
        set_menu(!overlay_menu_open());
        return;
    }
    if (overlay_event(e)) {
        if (overlay_menu_open() && !paused) { paused = true; auto_paused = false; }   // touch menu button
        return;
    }
    switch (e.type) {
    case SDL_QUIT: quit_requested = true; break;
    case SDL_KEYDOWN: {
        if (e.key.repeat) break;
        const SDL_Keycode k = e.key.keysym.sym;
        const bool fs = SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
        if (k == SDLK_F11 || (k == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT)))
            SDL_SetWindowFullscreen(window, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
        else if (k == SDLK_ESCAPE && fs) SDL_SetWindowFullscreen(window, 0);
        else if (k == SDLK_F8) { paused = !paused; auto_paused = false; update_title(); }
        else if (k == SDLK_F9) { show_fps = !show_fps; update_title(); }
        else if (k == SDLK_F12) save_screenshot();
        break;
    }
    case SDL_WINDOWEVENT:
        if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST && cfg.pause_unfocused && !paused) { paused = auto_paused = true; update_title(); }
        else if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED && auto_paused) { paused = auto_paused = false; update_title(); }
        break;
    case SDL_CONTROLLERDEVICEADDED: open_controller(e.cdevice.which); break;
    case SDL_CONTROLLERDEVICEREMOVED: close_controller(e.cdevice.which); break;
    default: break;
    }
}
static void load_controller_db(const std::string &dir) {
    if (dir.empty()) return;
    std::string path = dir + "/gamecontrollerdb.txt";
    if (FILE *f = fopen(path.c_str(), "rb")) {
        fclose(f);
        int n = SDL_GameControllerAddMappingsFromFile(path.c_str());
        fprintf(stderr, "[pad] %d controller mappings from %s\n", n, path.c_str());
    }
}
extern "C" int upstream_gx_init(const UpstreamGxConfig *config) {
    if (context) { error_text = "GX already initialized"; return -1; }
    if (!config || !config->ram || config->ram_size != RAM_SIZE || config->width < 1 || config->height < 1) {
        error_text = "invalid GX configuration (24 MiB MEM1 required)"; return -1;
    }
    cfg = *config; g_mem = cfg.ram; error_text.clear();
    for (int p = 0; p < 4; ++p) pad_snapshot[p] = {0, 128, 128, 128, 128, 0, 0};
    pad_mask_snapshot = 1;
    presented_count.store(0);
    // The adapter feeds the GX front end from the guest thread and renders on this
    // thread. Deferred XF keeps the guest moving while the previous frame draws.
    // The upstream default is two transform threads; a device can override it.
#ifdef _WIN32
    if (!getenv("GCN_XF_THREADS")) _putenv_s("GCN_XF_THREADS", "2");
#else
    if (!getenv("GCN_XF_THREADS")) setenv("GCN_XF_THREADS", "2", 0);
#endif
    // Window sizes are physical pixels; controllers keep working while unfocused unless that pauses.
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, cfg.pause_unfocused ? "0" : "1");
    // SDL subsystem initialization is reference counted; release our reference only.
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) { error_text = SDL_GetError(); return -1; }
    sdl_owned = true;
    if (cfg.data_dir) load_controller_db(cfg.data_dir);
    if (char *base = SDL_GetBasePath()) { load_controller_db(base); SDL_free(base); }
    paused = auto_paused = quit_requested = show_fps = false;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) open_controller(i);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, GCN_GL_MAJOR);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, GCN_GL_MINOR);
#ifdef GCN_GL_ES
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8); SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8); SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#endif
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    Uint32 flags = SDL_WINDOW_OPENGL | (cfg.hidden ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN);
    if (cfg.resizable) flags |= SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (cfg.fullscreen && !cfg.hidden) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    window = SDL_CreateWindow(cfg.title ? cfg.title : "Digimon Battle GC", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              cfg.window_width > 0 ? cfg.window_width : cfg.width,
                              cfg.window_height > 0 ? cfg.window_height : cfg.height, flags);
    if (window && cfg.resizable) SDL_SetWindowMinimumSize(window, 320, 240);
    if (!window || !(context = SDL_GL_CreateContext(window))) {
        error_text = SDL_GetError(); upstream_gx_shutdown(); return -1;
    }
    try {
        if (gl_load_with(SDL_GL_GetProcAddress) < GCN_GL_VERSION_MIN) fatal("OpenGL %d.%d%s required", GCN_GL_MAJOR, GCN_GL_MINOR,
#ifdef GCN_GL_ES
                                                                           " ES"
#else
                                                                           ""
#endif
        );
        fprintf(stderr, "[upstream-gx] GL %s renderer %s; gcn-recomp 3fad2f2 graphics only\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
        SDL_GL_SetSwapInterval(cfg.vsync && !cfg.hidden ? 1 : 0);
        // Shader combinations seen in earlier runs are built at startup instead of
        // stalling the guest (and its audio) the first time a scene needs them.
        static std::string shader_cache;
        if (cfg.data_dir) shader_cache = (std::filesystem::u8path(cfg.data_dir) / "shader_cache.bin").u8string();
        else if (const char *path = getenv("GC_SHADER_CACHE")) shader_cache = path;   // measurement runs
        gx::render_set_shader_cache(shader_cache.empty() ? nullptr : shader_cache.c_str());
        // Frame-by-frame debugging (GCN_DUMP_RANGE selects the presented frames).
        gx::g_dump_dir = getenv("GC_DUMP_DIR");
        gx::g_dump_every = getenv("GC_DUMP_EVERY") ? atoi(getenv("GC_DUMP_EVERY")) : 1;
        gx::render_init(cfg.internal_scale);
        gx::render_set_window_size(cfg.width, cfg.height);
        glGenTextures(1, &output_tex); glBindTexture(GL_TEXTURE_2D, output_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, cfg.width, cfg.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenFramebuffers(1, &output_fbo); glBindFramebuffer(GL_FRAMEBUFFER, output_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output_tex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fatal("output framebuffer incomplete");
        gx::render_set_output_fbo(output_fbo);
        gx::set_copy_writeback(copy_writeback);
        upstream_gx_reset();
        if (!cfg.hidden && cfg.settings) {
            OverlayHost host{};
            host.settings = static_cast<GameSettings *>(cfg.settings);
            host.settings_changed = cfg.settings_changed; host.user = cfg.user; host.port_name = port_name;
            overlay_init(window, host);
        }
        return 0;
    } catch (const std::exception& e) { error_text = e.what(); upstream_gx_shutdown(); return -1; }
}
extern "C" int upstream_gx_consume(const void *bytes, size_t length) {
    if (!bytes && length) { error_text = "consume requires valid bytes"; return -1; }
    if (stream_failed) return -1; // retain the first failure; never retry/skip its command
    try {
        if (length > 16 * 1024 * 1024 || pending.size() + length > 16 * 1024 * 1024) fatal("GX pending command buffer exceeded 16 MiB");
        if (length) pending.insert(pending.end(), static_cast<const uint8_t *>(bytes), static_cast<const uint8_t *>(bytes) + length);
        const uint32_t used = pending.empty() ? 0 : gx::process(pending.data(), uint32_t(pending.size()), true);
        pending.erase(pending.begin(), pending.begin() + used);
        return 0;
    } catch (const std::exception& e) { error_text = e.what(); stream_failed = true; return -1; }
}
extern "C" int upstream_gx_present() {
    if (!owner()) return -1;
    SDL_Event event;
    while (SDL_PollEvent(&event)) handle_event(event);
    // Sample input here, where SDL and the touch overlay are owned, then hand the
    // guest a stable snapshot. Rumble is applied here too, never on the guest.
    {
        UpstreamGxPad p[4];
        const unsigned mask = read_pads(p);
        std::lock_guard<std::mutex> lk(pad_mutex);
        std::memcpy(pad_snapshot, p, sizeof(pad_snapshot));
        pad_mask_snapshot = mask;
    }
    apply_rumble_requests();
    if (quit_requested) return -1;
    bool presented = new_frame; new_frame = false;
#ifdef DIGIMON_GX_ASYNC
    // Wait briefly for the first frame so an idle render loop is not a spin loop;
    // then drain everything already finished (later strips of the same display).
    if (auto b = gx::take_batch(presented ? 0 : 8)) {
        presented |= gx::render_execute(*b);
        while (auto c = gx::take_batch(0)) presented |= gx::render_execute(*c);
    }
#else
    while (auto b = gx::take_batch(0)) presented |= gx::render_execute(*b);
#endif
    if (presented) {
        presented_count.store(gx::present_count(), std::memory_order_relaxed);
        if (gx::stalls_on()) {
            static uint32_t frames; static uint64_t t0;
            if (!t0) t0 = gx::stall_now_us();
            if (++frames % 300 == 0) {
                uint64_t v[(int)gx::Stall::Count];
                for (int i = 0; i < (int)gx::Stall::Count; ++i) v[i] = gx::stall_us[i].exchange(0);
                const uint64_t now = gx::stall_now_us();
                fprintf(stderr, "[stalls] %u frames %.0fms: fe_full %.0f finish %.0f token %.0f xf_full %.0f render_full %.0f ms\n",
                        300u, (now - t0) / 1e3, v[0] / 1e3, v[1] / 1e3, v[2] / 1e3, v[3] / 1e3, v[4] / 1e3);
                t0 = now;
            }
        }
        if (!cfg.hidden) {
            // The runner paces the shared guest clock, including retraces without a copy.
            blit_window();
            int dw = 0, dh = 0; SDL_GL_GetDrawableSize(window, &dw, &dh);
            overlay_draw(dw, dh, (int)fps_value, show_fps);
            SDL_GL_SwapWindow(window);
            ++fps_frames;
            const uint32_t now = SDL_GetTicks();
            if (now - fps_t0 >= 1000) {
                fps_value = (uint32_t)(fps_frames * 1000ull / (now - fps_t0)); fps_frames = 0; fps_t0 = now;
                if (show_fps) update_title();
            }
        }
    }
    if (suspend_pending && guest_paused.load(std::memory_order_acquire)) {
        suspend_pending = false;
        if (cfg.on_suspend) cfg.on_suspend(cfg.user);
    }
    return presented ? 1 : 0;
}
extern "C" int upstream_gx_paused() { return paused.load(std::memory_order_relaxed) ? 1 : 0; }
extern "C" void upstream_gx_set_guest_paused(int paused_now) {
    guest_paused.store(paused_now ? 1 : 0, std::memory_order_release);
}
extern "C" int upstream_gx_wait_paused() {
    update_title();
    while (paused.load(std::memory_order_relaxed) && !quit_requested && owner()) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, overlay_menu_open() ? 16 : 50)) { handle_event(e); while (SDL_PollEvent(&e)) handle_event(e); }
        switch (overlay_take_action()) {
        case OVERLAY_RESUME: set_menu(false); break;
        case OVERLAY_QUIT: quit_requested = true; break;
        default: break;
        }
        if (!cfg.hidden && paused.load(std::memory_order_relaxed)) {   // repaint after resize/expose; the menu draws over the frame
            blit_window();
            int dw = 0, dh = 0; SDL_GL_GetDrawableSize(window, &dw, &dh);
            overlay_draw(dw, dh, (int)fps_value, show_fps);
            SDL_GL_SwapWindow(window);
        }
    }
    if (overlay_menu_open()) overlay_set_menu(false);
    paused.store(false, std::memory_order_relaxed); auto_paused = false;
    update_title();
    fps_t0 = SDL_GetTicks(); fps_frames = 0;
    return quit_requested ? -1 : 0;
}

static const uint16_t kButtonMask[GC_IN_PAD_COUNT] = {
    0x0100, 0x0200, 0x0400, 0x0800, 0x0010, 0x0040, 0x0020, 0x1000, 0x0008, 0x0004, 0x0001, 0x0002};
extern "C" void upstream_gx_input_defaults(UpstreamGxInput *in) {
    if (!in) return;
    const int pad[GC_IN_PAD_COUNT] = {
        SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
        SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, GC_BIND_AXIS + SDL_CONTROLLER_AXIS_TRIGGERLEFT,
        GC_BIND_AXIS + SDL_CONTROLLER_AXIS_TRIGGERRIGHT, SDL_CONTROLLER_BUTTON_START,
        SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
        SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT};
    const int key[GC_IN_COUNT] = {
        SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_U, SDL_SCANCODE_I, SDL_SCANCODE_O,
        SDL_SCANCODE_Q, SDL_SCANCODE_E, SDL_SCANCODE_RETURN,
        SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_A, SDL_SCANCODE_D,
        SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
        SDL_SCANCODE_T, SDL_SCANCODE_G, SDL_SCANCODE_F, SDL_SCANCODE_H};
    std::copy(pad, pad + GC_IN_PAD_COUNT, in->pad);
    std::copy(key, key + GC_IN_COUNT, in->key);
    in->deadzone = 15; in->keyboard_port = 0; in->rumble = 1;
}
extern "C" void upstream_gx_set_input(const UpstreamGxInput *in) {
    if (!in) return;
    input_cfg = *in;
    input_cfg.deadzone = std::clamp(input_cfg.deadzone, 0, 90);
    input_cfg.keyboard_port &= 3;
    input_ready = true;
}
// Radial deadzone, rescaled so the full range stays reachable; GC Y is up-positive.
static void read_stick(SDL_GameController *c, SDL_GameControllerAxis ax, SDL_GameControllerAxis ay, uint8_t &ox, uint8_t &oy) {
    float x = std::clamp(SDL_GameControllerGetAxis(c, ax) / 32767.0f, -1.0f, 1.0f);
    float y = std::clamp(-SDL_GameControllerGetAxis(c, ay) / 32767.0f, -1.0f, 1.0f);
    const float m = std::sqrt(x * x + y * y), dz = input_cfg.deadzone / 100.0f;
    if (m <= dz) { ox = oy = 128; return; }
    const float s = std::min(1.0f, (m - dz) / (1.0f - dz)) / m;
    ox = (uint8_t)std::clamp(128 + (int)std::lround(x * s * 127), 1, 255);
    oy = (uint8_t)std::clamp(128 + (int)std::lround(y * s * 127), 1, 255);
}
static void read_controller(SDL_GameController *c, UpstreamGxPad &pad) {
    for (int i = 0; i < GC_IN_PAD_COUNT; ++i) {
        const int b = input_cfg.pad[i];
        int analog = 0;
        if (b >= GC_BIND_AXIS) {
            analog = std::max<int>(0, SDL_GameControllerGetAxis(c, (SDL_GameControllerAxis)(b - GC_BIND_AXIS))) * 255 / 32767;
            if (analog < 8) analog = 0;
        } else if (b >= 0 && SDL_GameControllerGetButton(c, (SDL_GameControllerButton)b)) analog = 255;
        if (analog > 217) pad.buttons |= kButtonMask[i];   // digital click at ~85% travel
        if (i == GC_IN_L) pad.trigger_l = (uint8_t)std::max<int>(pad.trigger_l, analog);
        if (i == GC_IN_R) pad.trigger_r = (uint8_t)std::max<int>(pad.trigger_r, analog);
    }
    read_stick(c, SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, pad.stick_x, pad.stick_y);
    read_stick(c, SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY, pad.sub_x, pad.sub_y);
}
// Keys override the stick only while held; diagonals stay inside the GC gate.
static void key_stick(bool l, bool r, bool u, bool d, uint8_t &ox, uint8_t &oy) {
    const int dx = (int)r - (int)l, dy = (int)u - (int)d;
    if (!dx && !dy) return;
    const int m = dx && dy ? 90 : 127;
    ox = (uint8_t)(128 + dx * m); oy = (uint8_t)(128 + dy * m);
}
static void read_keyboard(UpstreamGxPad &pad) {
    const uint8_t *k = SDL_GetKeyboardState(nullptr);
    const bool alt = SDL_GetModState() & KMOD_ALT;   // Alt+Enter is the fullscreen hotkey
    auto down = [&](int i) {
        const int sc = input_cfg.key[i];
        return sc > 0 && sc < SDL_NUM_SCANCODES && !(alt && sc == SDL_SCANCODE_RETURN) && k[sc];
    };
    for (int i = 0; i < GC_IN_PAD_COUNT; ++i) {
        if (!down(i)) continue;
        pad.buttons |= kButtonMask[i];
        if (i == GC_IN_L) pad.trigger_l = 255;
        if (i == GC_IN_R) pad.trigger_r = 255;
    }
    key_stick(down(GC_IN_STICK_LEFT), down(GC_IN_STICK_RIGHT), down(GC_IN_STICK_UP), down(GC_IN_STICK_DOWN), pad.stick_x, pad.stick_y);
    key_stick(down(GC_IN_C_LEFT), down(GC_IN_C_RIGHT), down(GC_IN_C_UP), down(GC_IN_C_DOWN), pad.sub_x, pad.sub_y);
}
static unsigned read_pads(UpstreamGxPad out[4]) {
    if (!input_ready) { upstream_gx_input_defaults(&input_cfg); input_ready = true; }
    for (int p = 0; p < 4; ++p) out[p] = {0, 128, 128, 128, 128, 0, 0};
    if (!owner()) return 0;
    unsigned mask = 1u << input_cfg.keyboard_port;
    for (int p = 0; p < 4; ++p)
        if (ports[p]) { read_controller(ports[p], out[p]); mask |= 1u << p; }
    read_keyboard(out[input_cfg.keyboard_port]);
    overlay_pad(out[input_cfg.keyboard_port]);   // touch controls drive the keyboard player
    return mask;
}
extern "C" unsigned upstream_gx_pads(UpstreamGxPad out[4]) {
    return read_pads(out);
}
extern "C" unsigned upstream_gx_snapshot_pads(UpstreamGxPad out[4]) {
    std::lock_guard<std::mutex> lk(pad_mutex);
    std::memcpy(out, pad_snapshot, sizeof(pad_snapshot));
    return pad_mask_snapshot;
}
static void apply_rumble_requests(void) {
    for (unsigned p = 0; p < 4; ++p) {
        const int req = rumble_request[p].load(std::memory_order_relaxed);
        if (req == rumble_applied[p].load(std::memory_order_relaxed)) continue;
        rumble_applied[p].store(req, std::memory_order_relaxed);
        const bool on = req && input_cfg.rumble;
        static unsigned logged;
        if (logged < 8) {
            ++logged;
            fprintf(stderr, "[rumble] port %u %s -> %s%s\n", p + 1, req ? "on" : "off",
                    ports[p] ? "controller" : (int)p == input_cfg.keyboard_port ? "phone/touch" : "nobody",
                    input_cfg.rumble ? "" : " (rumble disabled in settings)");
        }
        if (!ports[p]) {   // no pad on this port: the phone vibrates for the touch player
            if ((int)p == input_cfg.keyboard_port) overlay_rumble(on);
            continue;
        }
        const uint32_t now = SDL_GetTicks();
        if (on && (!rumbling[p] || now - rumble_refresh[p] > 500)) {
            SDL_GameControllerRumble(ports[p], 0xC000, 0xC000, 1000); rumble_refresh[p] = now;
        } else if (!on && rumbling[p]) SDL_GameControllerRumble(ports[p], 0, 0, 0);
        rumbling[p] = on;
    }
}
extern "C" void upstream_gx_pad(UpstreamGxPad *pad) {
    if (!pad) return;
    UpstreamGxPad all[4]; upstream_gx_snapshot_pads(all);
    *pad = all[0];
}
extern "C" void upstream_gx_rumble(unsigned port, int on) {
    if (port > 3) return;
    rumble_request[port].store(on ? 1 : 0, std::memory_order_relaxed);
}
extern "C" int upstream_gx_read_rgba(uint8_t *out, size_t capacity) {
    const size_t row = size_t(cfg.width) * 4;
    if (!owner() || !out || capacity < row * cfg.height) return -1;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, output_fbo); glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, cfg.width, cfg.height, GL_RGBA, GL_UNSIGNED_BYTE, out);
    for (int y = 0; y < cfg.height / 2; ++y) std::swap_ranges(out + y * row, out + (y + 1) * row, out + (cfg.height - y - 1) * row);
    return glGetError() == GL_NO_ERROR ? 0 : -1;
}
extern "C" int upstream_gx_capture_png(const char *path) {
    std::vector<uint8_t> pixels(size_t(cfg.width) * cfg.height * 4);
    return path && upstream_gx_read_rgba(pixels.data(), pixels.size()) == 0 && write_png(path, pixels.data(), cfg.width, cfg.height) ? 0 : -1;
}
extern "C" void upstream_gx_reset() {
    if (!owner()) return;
    pending.clear(); stream_failed = false; error_text.clear();
    gx::adapter_frontend_reset(); gx::adapter_texture_reset(); gx::adapter_render_reset();
    glBindFramebuffer(GL_FRAMEBUFFER, output_fbo); glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    new_frame = false;
}
extern "C" void upstream_gx_set_display(int stretch, int smooth, int fps) {
    cfg.stretch = stretch; cfg.smooth = smooth;
    if (show_fps != (fps != 0)) { show_fps = fps != 0; update_title(); }
}
extern "C" void upstream_gx_shutdown() {
    if (context) {
        SDL_GL_MakeCurrent(window, context);
        overlay_shutdown();
        gx::adapter_frontend_reset(); gx::adapter_texture_reset(); gx::adapter_render_shutdown();
        glDeleteTextures(1, &output_tex); glDeleteFramebuffers(1, &output_fbo);
        SDL_GL_DeleteContext(context); context = nullptr;
    }
    for (int p = 0; p < 4; ++p)
        if (ports[p]) { SDL_GameControllerClose(ports[p]); ports[p] = nullptr; rumbling[p] = false; }
    if (window) { SDL_DestroyWindow(window); window = nullptr; }
    if (sdl_owned) SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER);
    sdl_owned = false; output_fbo = output_tex = 0; g_mem = nullptr; pending.clear();
}
