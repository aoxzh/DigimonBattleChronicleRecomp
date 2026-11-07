/* Separate desktop runner; uses only the existing generated Gekko CPU and HAL. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L   /* setenv, clock_gettime, nanosleep under strict C11 */
#endif
#include "upstream_gx.h"
#include "settings.h"
#include "launcher.h"
#include "../../../hal/hal_boot.h"
#include "../../../hal/gc_abi.h"
#include "../../../hal/mem.h"
#include "../../../hal/gx.h"
#include "../../../hal/vi.h"
#include "../../../hal/hal.h"
#include "../../../hal/irq.h"
#include "../../../hal/si.h"
#include "../../../hal/di.h"
#include "../../../hal/exi.h"
#include "../../../hal/ai.h"
/* Raw host state uses the same SI report encoding as scripted input. */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <sys/stat.h>
#endif
#include <time.h>
#include <string.h>
#ifndef __ANDROID__
#define SDL_MAIN_HANDLED   /* plain main() on desktop (see CMakeLists.txt); Android enters via SDL_main */
#endif
#include <SDL.h>
#ifdef _WIN32
/* MinGW supplies unistd.h/clock_gettime but no alarm/SIGALRM; a watchdog thread
 * stands in for it, and a high-resolution timer replaces ~15ms nanosleep. */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
static GameSettings gs;   /* edited live by the in-game menu */
#ifdef GC_UPSTREAM_AUDIO
void host_audio_configure(int enabled, int volume, int latency_ms);
void host_audio_init(int realtime);
void host_audio_pause(int pause);
void host_audio_shutdown(void);
#endif
/* SDL_setenv() is not reliable before SDL's video subsystem exists (the
 * headed runner sets --input-script before SDL_Init). Use the host API so the
 * HAL sees GC_INPUT_SCRIPT during boot. */
static void set_process_env(const char *name, const char *value)
{
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    setenv(name, value ? value : "", 1);
#endif
}

static unsigned target_frames;
static const char *capture_path;
static volatile sig_atomic_t timed_out;
static unsigned virtual_frames;
static unsigned capture_every, last_captured;
static const char *capture_dir;
static int script_controls, realtime, player_mode;
static uint64_t wall_start_ns, pace_offset_ns;
static uint64_t cycle_start;
/* The recompiled CPU runs on its own thread so a long GL submit, swap or driver
 * stall cannot stop the guest from producing audio. SDL, GL, the overlay and
 * input live on the main thread. */
static SDL_Thread *guest_thread;
static SDL_atomic_t guest_done;
static SDL_atomic_t guest_exit_requested;
static SDL_atomic_t guest_pause_ack;
static jmp_buf guest_jmp;
static int guest_jmp_ready;
static uint64_t monotonic_ns(void) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec*1000000000ull + (uint64_t)now.tv_nsec;
}
static uint64_t elapsed_ns(void) { return monotonic_ns()-wall_start_ns; }
static void sleep_ns(uint64_t delay) {
#ifdef _WIN32
    static HANDLE timer;
    if (!timer) timer=CreateWaitableTimerExW(NULL,NULL,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
    LARGE_INTEGER due; due.QuadPart=-(LONGLONG)((delay+99)/100);
    if (timer && SetWaitableTimer(timer,&due,0,NULL,NULL,FALSE)) WaitForSingleObject(timer,INFINITE);
    else Sleep((DWORD)(delay/1000000ull));
#else
    struct timespec wait={(time_t)(delay/1000000000ull),(long)(delay%1000000000ull)};
    while (nanosleep(&wait,&wait)!=0 && !timed_out) {}
#endif
}
static uint64_t pace_sleep_ns;   /* GC_THREAD_DIAG: guest time spent ahead of real time */
static volatile uint64_t guest_cpu_ns;   /* guest thread CPU time, sampled per frame */
static void pace_guest(void) {
    { struct timespec c; if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c) == 0) guest_cpu_ns = (uint64_t)c.tv_sec*1000000000ull + (uint64_t)c.tv_nsec; }
    if (!realtime) return;
    uint64_t cycles=hal_cpu_cycles()-cycle_start;
    uint64_t target=cycles/486000000ull*1000000000ull + cycles%486000000ull*1000000000ull/486000000ull;
    uint64_t elapsed=elapsed_ns()-pace_offset_ns;
    /* Short loading stalls are caught up so audio stays in sync; a host stall
     * over a second (window drag, debugger, sleep) rebases instead of running
     * the game at full speed until it has caught up. */
    if (elapsed > target+1000000000ull) { pace_offset_ns += elapsed-target; return; }
    if (target>elapsed) {
        if (getenv("GC_THREAD_DIAG") && target-elapsed > 100000000ull) {
            static unsigned n;
            if (n++ < 20u)
                fprintf(stderr, "[pace] sleep_ms=%.3f cycles=%llu target_ns=%llu elapsed_ns=%llu offset_ns=%llu\n",
                        (double)(target-elapsed)/1e6, (unsigned long long)cycles,
                        (unsigned long long)target, (unsigned long long)elapsed,
                        (unsigned long long)pace_offset_ns);
        }
        pace_sleep_ns += target-elapsed;
        sleep_ns(target-elapsed);
    }
}

static void timeout_handler(int sig) {
    (void)sig;
    if (timed_out) {
        static const char message[] = "[main-gx] hard timeout: guest stopped servicing VI callbacks\n";
        (void)write(STDERR_FILENO,message,sizeof(message)-1);
        _exit(124);
    }
    timed_out = 1;
#ifndef _WIN32
    alarm(2); /* Give the normal VI callback time to capture and clean up. */
#endif
}
#ifdef _WIN32
static DWORD WINAPI timeout_thread(LPVOID arg) {
    Sleep((DWORD)(uintptr_t)arg*1000u); timeout_handler(0);
    Sleep(2000); timeout_handler(0);
    return 0;
}
#endif
static void finish(void *user) { (void)user; hal_gx_signal_finish(); }
static void token(uint16_t value, int interrupt, void *user) { (void)user; hal_gx_signal_token(value, interrupt); }
static uint32_t display_height(void *user) {
    (void)user;
    uint32_t acv = (hal_vi_reg(0) >> 4) & 0x3ff;
    return acv * ((hal_vi_reg(2) & 4) ? 1 : 2);
}
/* Player builds have no console: errors also go to a dialog. */
static void report_error(const char *title, const char *message) {
    fprintf(stderr, "[main-gx] %s: %s\n", title, message);
    if (player_mode) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, message, NULL);
}
static void at_exit(void) {
    const char *dump = getenv("GC_RAM_DUMP");
    (void)hal_exi_flush();
    fprintf(stderr,"[main-gx] timing wall_ns=%llu guest_cycles=%llu\n",(unsigned long long)elapsed_ns(),(unsigned long long)(hal_cpu_cycles()-cycle_start));
    if (dump && *dump) {
        FILE *f = fopen(dump,"wb");
        size_t written=0; if(f) { written=fwrite(gc_ram,1,GC_MEM1_SIZE,f); fclose(f); }
        fprintf(stderr,"[main-gx] RAM dump %s bytes=%zu\n",dump,written);
    }
    fprintf(stderr,"[main-gx] cpu pc=%08x lr=%08x ctr=%08x msr=%08x srr0=%08x srr1=%08x\n",gc.pc,gc.lr,gc.ctr,gc.msr,gc.srr0,gc.srr1);
    fprintf(stderr,"[main-gx] gpr");
    for(int r=0;r<32;++r) fprintf(stderr," r%d=%08x",r,gc.r[r]);
    fprintf(stderr,"\n[main-gx] DI reads=%llu bytes=%llu xfb=%08x fifo=%llu consumed=%llu display=%u\n",
      (unsigned long long)hal_di_read_count(),(unsigned long long)hal_di_read_bytes(),hal_vi_xfb_top(),
      (unsigned long long)hal_gx_bytes_fed(),(unsigned long long)hal_gx_bytes_consumed(),upstream_gx_frame_count());
#ifdef GC_UPSTREAM_AUDIO
    host_audio_shutdown();
#endif
    upstream_gx_shutdown(); hal_di_close_iso();
}
/* In-game menu edited gs: apply what can change live, then persist. */
static void settings_changed(void *user) {
    (void)user;
    upstream_gx_set_display(gs.stretch, gs.smooth, gs.show_fps);
    upstream_gx_set_input(&gs.input);
#ifdef GC_UPSTREAM_AUDIO
    host_audio_configure(gs.audio, gs.volume, gs.audio_latency);
#endif
    if (settings_save(&gs) != 0) fprintf(stderr, "[main-gx] cannot save settings to %s\n", settings_data_dir());
}
/* Backgrounded (Android may kill the app without further notice): saves to disk now. */
static void on_suspend(void *user) {
    (void)user;
    (void)hal_exi_flush();
    fflush(stderr);
}
static void guest_exit_now(int code) {
    SDL_AtomicSet(&guest_exit_requested, code);
    if (guest_jmp_ready) longjmp(guest_jmp, 1);
}
extern volatile int hal_guest_phase;
static void frame(void *user) {
    (void)user;
    hal_guest_phase = 30;
    ++virtual_frames;
    pace_guest();
    if (SDL_AtomicGet(&guest_exit_requested) || timed_out)
        guest_exit_now(timed_out ? 3 : SDL_AtomicGet(&guest_exit_requested));
    /* The render thread owns the pause menu. Stop the guest between frames so
     * audio no longer advances while paused, and so Android can flush the
     * memory card without racing a guest write. */
    if (upstream_gx_paused()) {
        SDL_AtomicSet(&guest_pause_ack, 1);
        const uint64_t t0 = monotonic_ns();
        while (upstream_gx_paused() && !SDL_AtomicGet(&guest_exit_requested) && !timed_out)
            sleep_ns(2000000);
        pace_offset_ns += monotonic_ns()-t0;
        SDL_AtomicSet(&guest_pause_ack, 0);
        if (SDL_AtomicGet(&guest_exit_requested) || timed_out)
            guest_exit_now(timed_out ? 3 : SDL_AtomicGet(&guest_exit_requested));
    }
    /* A scripted replay drives the pads, but rumble still reaches the host so a device
     * test can verify it; touch and controllers are ignored while a script is loaded. */
    if (script_controls)
        for (unsigned i = 0; i < 4; ++i) upstream_gx_rumble(i, hal_si_rumble(i));
    if (!script_controls) {
        UpstreamGxPad p[4];
        unsigned present_mask = upstream_gx_snapshot_pads(p);
        for (unsigned i = 0; i < 4; ++i) {
            hal_si_set_connected(i, i == 0 || ((present_mask >> i) & 1u));
            hal_si_set_pad_raw(i,p[i].buttons,p[i].stick_x,p[i].stick_y,p[i].sub_x,p[i].sub_y,p[i].trigger_l,p[i].trigger_r);
            upstream_gx_rumble(i, hal_si_rumble(i));
        }
    }
    const unsigned n = upstream_gx_frame_count();
    if (target_frames && n >= target_frames) guest_exit_now(0);
    hal_guest_phase = 23;
}
static int SDLCALL guest_main(void *unused) {
    (void)unused;
    if (!setjmp(guest_jmp)) {
        guest_jmp_ready = 1;
        gc_entry();
    }
    guest_jmp_ready = 0;
    SDL_AtomicSet(&guest_done, 1);
    return 0;
}
#ifdef _WIN32
/* GUI-subsystem executable: keep redirected stderr; otherwise attach to the
 * parent console with --console, or write logs/session-*.log (newest 8 kept). */
static int cmp_names(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }
static void windows_stdio(int console) {
    if (console && (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole())) {
        freopen("CONOUT$", "w", stderr); freopen("CONOUT$", "w", stdout);
        return;
    }
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN) return;
    char dir[1100], path[1200];
    snprintf(dir, sizeof(dir), "%s\\logs", settings_data_dir());
    CreateDirectoryA(dir, NULL);
    static char names[64][64];
    int count = 0;
    WIN32_FIND_DATAA fd;
    snprintf(path, sizeof(path), "%s\\session-*.log", dir);
    HANDLE find = FindFirstFileA(path, &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do if (count < 64 && strlen(fd.cFileName) < 64) strcpy(names[count++], fd.cFileName);
        while (FindNextFileA(find, &fd));
        FindClose(find);
    }
    qsort(names, (size_t)count, sizeof(names[0]), cmp_names);
    for (int i = 0; i + 7 < count; ++i) { snprintf(path, sizeof(path), "%s\\%s", dir, names[i]); DeleteFileA(path); }
    char stamp[32]; time_t now = time(NULL);
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&now));
    snprintf(path, sizeof(path), "%s\\session-%s.log", dir, stamp);
    if (freopen(path, "w", stderr)) setvbuf(stderr, NULL, _IONBF, 0);
}
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *info) {
    char text[512];
    snprintf(text, sizeof(text), "The game stopped unexpectedly (exception %08lx at %p, guest pc %08x).\n\nLogs: %s\\logs",
             info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress, gc.pc, settings_data_dir());
    fprintf(stderr, "[main-gx] crash: %s\n", text);
    fflush(stderr);
    if (player_mode) MessageBoxA(NULL, text, "Digimon Battle Chronicle", MB_ICONERROR | MB_OK);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif
static void usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s [disc.iso] [--setup] [--fullscreen|--windowed] [--scale N] [--window WxH] [--console] [--no-settings]\n"
        "       %s main.dol disc.iso [display-frames=0] [capture.png] [seconds=0] [--hidden]\n"
        "No arguments opens the launcher (or starts directly when chosen there). 0 disables the\n"
        "display/time limit; close the window to exit. --hidden runs ignore settings.ini.\n", argv0, argv0);
}
#ifdef __ANDROID__
/* No console on Android: stderr goes to <data>/logs/session.log (previous run kept). */
static void android_stdio(void) {
    char dir[1100], path[1200], prev[1200];
    snprintf(dir, sizeof(dir), "%s/logs", settings_data_dir());
    mkdir(dir, 0755);
    snprintf(path, sizeof(path), "%s/session.log", dir);
    snprintf(prev, sizeof(prev), "%s/session.prev.log", dir);
    rename(path, prev);
    if (freopen(path, "w", stderr)) setvbuf(stderr, NULL, _IOLBF, 0);
}
#endif
int main(int argc, char **argv) {
    SDL_SetMainReady();
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    android_stdio();
#endif
    const char *pos[8]; int npos = 0;
    const char *disc_arg = NULL;
    int setup = 0, hidden = 0, console = 0, no_settings = 0, fullscreen = -1, scale = 0, win_w = 0, win_h = 0;
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (!strcmp(a, "--setup")) setup = 1;
        else if (!strcmp(a, "--disc") && i + 1 < argc) disc_arg = argv[++i];   /* this run's disc, launcher still shown */
        else if (!strcmp(a, "--input-script") && i + 1 < argc) set_process_env("GC_INPUT_SCRIPT", argv[++i]);
        else if (!strcmp(a, "--hidden")) hidden = 1;
        else if (!strcmp(a, "--console")) console = 1;
        else if (!strcmp(a, "--no-settings")) no_settings = 1;
        else if (!strcmp(a, "--fullscreen")) fullscreen = 1;
        else if (!strcmp(a, "--windowed")) fullscreen = 0;
        else if (!strcmp(a, "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(a, "--window") && i + 1 < argc) { if (sscanf(argv[++i], "%dx%d", &win_w, &win_h) != 2) win_w = win_h = 0; }
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(argv[0]); return 0; }
        else if (a[0] == '-' && a[1] == '-') { usage(argv[0]); return 2; }
        else if (npos < 8) pos[npos++] = a;
    }
    (void)console;   /* used by the Windows log setup only */
    hidden = hidden || npos > 5;   /* original ABI: any sixth positional argument */
    player_mode = npos <= 1 && !hidden;
#ifdef _WIN32
    windows_stdio(console);
    SetUnhandledExceptionFilter(crash_filter);
#endif
    settings_defaults(&gs);
    const int use_settings = !hidden && !no_settings;
    if (use_settings) settings_load(&gs);
    if (disc_arg) snprintf(gs.disc, sizeof(gs.disc), "%s", disc_arg);
    if (npos == 1) snprintf(gs.disc, sizeof(gs.disc), "%s", pos[0]);
    const char *dol_path = npos >= 2 ? pos[0] : NULL;
    const char *iso_path = npos >= 2 ? pos[1] : gs.disc;
    if (player_mode) {
        char reason[256];
        int ok = settings_check_disc(gs.disc, reason, sizeof(reason));
#ifdef __ANDROID__
        /* Android uses MainActivity (Material 3) as its launcher; never open the
           desktop ImGui window here. A missing/invalid image is an error dialog. */
        if (!ok) { report_error("Disc image", reason); return 2; }
#else
        if (setup || (npos == 0 && !gs.skip_launcher) || !ok) {
            if (npos == 1 && !ok) report_error("Disc image", reason);
            if (!launcher_run(&gs)) return 0;
        }
        if (!settings_check_disc(gs.disc, reason, sizeof(reason))) { report_error("Disc image", reason); return 2; }
#endif
        iso_path = gs.disc;
    }
    target_frames = npos > 2 ? (unsigned)strtoul(pos[2], NULL, 0) : 0;
#ifdef _WIN32
    static char default_capture[MAX_PATH];
    snprintf(default_capture,sizeof(default_capture),"%s\\digimon-main-gx.png",getenv("TEMP") ? getenv("TEMP") : ".");
    capture_path = npos > 3 ? pos[3] : player_mode ? NULL : default_capture;
#else
    capture_path = npos > 3 ? pos[3] : player_mode ? NULL : "/tmp/digimon-main-gx.png";
#endif
    unsigned seconds = npos > 4 ? (unsigned)strtoul(pos[4], NULL, 0) : 0;
    script_controls = getenv("GC_INPUT_SCRIPT") != NULL;
    capture_dir = getenv("GC_CAPTURE_DIR");
    capture_every = getenv("GC_CAPTURE_EVERY") ? (unsigned)strtoul(getenv("GC_CAPTURE_EVERY"), NULL, 0) : 60;
    /* Slot A memory card: settings (player runs) or GC_MEMCARD; otherwise the slot stays empty. */
    static char memcard[1100];
    if (getenv("GC_MEMCARD")) snprintf(memcard, sizeof(memcard), "%s", getenv("GC_MEMCARD"));
#ifdef _WIN32
    else if (use_settings && gs.memcard) snprintf(memcard, sizeof(memcard), "%s\\MemoryCardA.raw", settings_data_dir());
#else
    else if (use_settings && gs.memcard) snprintf(memcard, sizeof(memcard), "%s/MemoryCardA.raw", settings_data_dir());
#endif
    hal_exi_set_memcard(memcard[0] ? memcard : NULL);
    /* A .rvz/.gcz/.ciso selected in the launcher is backed by an imported ISO. */
    static char resolved_disc[1200];
    char resolve_reason[512];
    if (!settings_resolve_disc(iso_path, resolved_disc, sizeof(resolved_disc),
                               resolve_reason, sizeof(resolve_reason))) {
        report_error("Disc image", resolve_reason);
        return 2;
    }
    iso_path = resolved_disc;
    if (!hal_boot_prepare(dol_path, iso_path)) { report_error("Boot failed", "Could not load the game from the disc image. See the log for details."); return 2; }
    UpstreamGxConfig config = {0};
    config.ram = gc_ram; config.ram_size = GC_MEM1_SIZE;
    config.width = 640; config.height = 480; config.internal_scale = 1;
    config.hidden = hidden;
    if (use_settings) {
        if (scale) gs.render_scale = scale < 1 ? 1 : scale > 4 ? 4 : scale;
        if (win_w > 0 && win_h > 0) { gs.window_width = win_w; gs.window_height = win_h; }
        if (fullscreen >= 0) gs.fullscreen = fullscreen;
        config.internal_scale = gs.render_scale;
        config.width = 640 * gs.render_scale; config.height = 480 * gs.render_scale;
        config.window_width = gs.window_width; config.window_height = gs.window_height;
        config.resizable = 1; config.fullscreen = gs.fullscreen;
        config.stretch = gs.stretch; config.smooth = gs.smooth; config.vsync = gs.vsync;
        config.pause_unfocused = gs.pause_unfocused;
        config.title = "Digimon Battle Chronicle";
        config.data_dir = settings_data_dir();
        config.settings = &gs; config.settings_changed = settings_changed;
#ifdef __ANDROID__
        config.fullscreen = 1;
#endif
    }
    config.on_suspend = on_suspend;
    realtime = !config.hidden || (getenv("GC_REALTIME") && strcmp(getenv("GC_REALTIME"),"1")==0);
    config.pe_finish = finish; config.pe_token = token; config.vi_height = display_height;
    if (upstream_gx_init(&config)) {
        char message[512];
        snprintf(message, sizeof(message), "Could not start graphics.\n\n%s", upstream_gx_error());
        report_error("Graphics", message);
        return 2;
    }
    if (use_settings) {
        upstream_gx_set_input(&gs.input);
        upstream_gx_set_display(gs.stretch, gs.smooth, gs.show_fps);
    }
#ifdef GC_UPSTREAM_AUDIO
    if (use_settings) host_audio_configure(gs.audio, gs.volume, gs.audio_latency);
    host_audio_init(realtime);
#endif
    wall_start_ns=monotonic_ns(); cycle_start=hal_cpu_cycles();
    fprintf(stderr, "[main-gx] clock monotonic_epoch_ns=%llu cpu_epoch_cycles=%llu\n",
            (unsigned long long)wall_start_ns, (unsigned long long)cycle_start);
#ifdef _WIN32
    atexit(at_exit);
    if (seconds) CloseHandle(CreateThread(NULL,0,timeout_thread,(LPVOID)(uintptr_t)seconds,0,NULL));
#else
    atexit(at_exit); signal(SIGALRM, timeout_handler); alarm(seconds);
#endif
    hal_set_frame_callback(frame, NULL);
    fprintf(stderr, "[main-gx] boot %08x limit=%u display frames / %u seconds scale=%d data=%s\n", hal_boot_entry(), target_frames, seconds,
            config.internal_scale, use_settings ? settings_data_dir() : "(settings off)");
    /* Prime the input snapshot/event pump before the guest can ask for it. */
    (void)upstream_gx_present();
    SDL_AtomicSet(&guest_done, 0);
    SDL_AtomicSet(&guest_exit_requested, 0);
    SDL_AtomicSet(&guest_pause_ack, 0);
    guest_thread = SDL_CreateThreadWithStackSize(guest_main, "guest", 16u * 1024u * 1024u, NULL);
    if (!guest_thread) {
        char message[512];
        snprintf(message, sizeof(message), "Could not start the guest thread.\n\n%s", SDL_GetError());
        report_error("Startup", message);
        return 2;
    }
    int exit_code = 0, guest_abandoned = 0, audio_was_paused = 0;
    uint64_t stop_deadline = 0, last_progress = monotonic_ns();
    unsigned last_display = 0;
    for (;;) {
        upstream_gx_set_guest_paused(SDL_AtomicGet(&guest_pause_ack));
        const int presented = upstream_gx_present();
        if (presented < 0) {
            SDL_AtomicSet(&guest_exit_requested, 1);
            if (!stop_deadline) stop_deadline = monotonic_ns() + 3000000000ull;
        }
        if (timed_out) {
            SDL_AtomicSet(&guest_exit_requested, 3);
            if (!stop_deadline) stop_deadline = monotonic_ns() + 3000000000ull;
        }
        const int paused_now = upstream_gx_paused();
        if (paused_now != audio_was_paused) {
#ifdef GC_UPSTREAM_AUDIO
            host_audio_pause(paused_now);
#endif
            audio_was_paused = paused_now;
        }
        if (presented > 0) {
            const unsigned n = upstream_gx_frame_count();
            last_progress = monotonic_ns();
            last_display = n;
            if (capture_dir && capture_every && n != last_captured && n % capture_every == 0) {
                char path[1024]; snprintf(path,sizeof(path),"%s/frame_%05u.png",capture_dir,n);
                if (upstream_gx_capture_png(path)) fprintf(stderr,"[main-gx] capture failed %s\n",path);
                last_captured=n;
            }
            if (n == 60) {
                fprintf(stderr, "[main-gx] VI registers");
                for (unsigned off = 0; off < 0x78; off += 2)
                    fprintf(stderr, " %02x=%04x", off, hal_vi_reg(off));
                fputc('\n', stderr);
            }
            if (n % 600 == 0 && getenv("GC_THREAD_DIAG")) {
                static uint64_t last_sleep, last_cpu, last_wall;
                const uint64_t wall = monotonic_ns(), cpu = guest_cpu_ns;
                fprintf(stderr, "[pace] display=%u wall_ms=%.1f guest_cpu_ms=%.1f pace_sleep_ms=%.1f (last 600)\n", n,
                        last_wall ? (double)(wall-last_wall)/1e6 : 0.0, (double)(cpu-last_cpu)/1e6, (double)(pace_sleep_ns-last_sleep)/1e6);
                last_sleep = pace_sleep_ns; last_cpu = cpu; last_wall = wall;
            }
            if (n <= 10 || n % 60 == 0)
                fprintf(stderr, "[main-gx] display=%u virtual=%u pc=%08x fifo=%llu copies=%u wall_ns=%llu guest_cycles=%llu\n", n, virtual_frames, gc.pc,
                        (unsigned long long)hal_gx_bytes_fed(), hal_gx_efb_copy_count(),
                        (unsigned long long)elapsed_ns(), (unsigned long long)(hal_cpu_cycles()-cycle_start));
        }
        if (SDL_AtomicGet(&guest_done)) break;
        const uint64_t now = monotonic_ns();
        if (now - last_progress > 2000000000ull) {
            fprintf(stderr, "[main-gx] stall display=%u virtual=%u pc=%08x icount=%llu fifo=%llu consumed=%llu paused=%d phase=%d cpu_cycles=%llu polls=%llu frames=%llu ai=%u\n",
                    last_display, virtual_frames, gc.pc, (unsigned long long)gc.icount,
                    (unsigned long long)hal_gx_bytes_fed(), (unsigned long long)hal_gx_bytes_consumed(),
                    (int)SDL_AtomicGet(&guest_pause_ack), (int)hal_guest_phase,
                    (unsigned long long)hal_cpu_cycles(), (unsigned long long)hal_poll_count(),
                    (unsigned long long)hal_frame_count(), hal_ai_samples());
            last_progress = now;
        }
        if (stop_deadline && monotonic_ns() > stop_deadline) {
            fprintf(stderr, "[main-gx] guest did not stop within 3s; exiting without it\n");
            guest_abandoned = 1;
            break;
        }
    }
    int guest_result = 0;
    if (!guest_abandoned) SDL_WaitThread(guest_thread, &guest_result);
    else { fflush(stderr); _Exit(2); }
    const unsigned n = upstream_gx_frame_count();
    /* Closing the window is a normal exit; only explicit captures can fail it. */
    int captured = capture_path ? upstream_gx_capture_png(capture_path) : 0;
    fprintf(stderr, "[main-gx] exit display=%u virtual=%u pc=%08x capture=%s result=%d timeout=%d\n",
            n, virtual_frames, gc.pc, capture_path ? capture_path : "none", captured, (int)timed_out);
    exit_code = timed_out ? 3 : captured ? 4 : 0;
    (void)guest_result;
    return exit_code;
}
