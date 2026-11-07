#!/usr/bin/env python3
"""Compile the real host sink against deterministic SDL/clock doubles.

No SDL installation, audio device, guest executable, or game data is required.
All generated headers, C sources, and executables live in a temporary directory.
"""
import argparse
import os
from pathlib import Path
import shlex
import signal
import subprocess
import tempfile


SDL_HEADER = r"""
#ifndef TEST_SDL_H
#define TEST_SDL_H
#include <stdint.h>
#include <stddef.h>
typedef uint8_t Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;
typedef Uint32 SDL_AudioDeviceID;
typedef enum { SDL_AUDIO_STOPPED, SDL_AUDIO_PLAYING, SDL_AUDIO_PAUSED } SDL_AudioStatus;
typedef struct { Uint8 major, minor, patch; } SDL_version;
typedef struct {
    int freq;
    Uint16 format;
    Uint8 channels;
    Uint16 samples;
} SDL_AudioSpec;
#define AUDIO_S16SYS 0x8010
#define SDL_INIT_AUDIO 0x10u
int SDL_InitSubSystem(Uint32 flags);
void SDL_QuitSubSystem(Uint32 flags);
SDL_AudioDeviceID SDL_OpenAudioDevice(const char *, int, const SDL_AudioSpec *, SDL_AudioSpec *, int);
void SDL_CloseAudioDevice(SDL_AudioDeviceID device);
void SDL_PauseAudioDevice(SDL_AudioDeviceID device, int pause);
const char *SDL_GetError(void);
const char *SDL_GetCurrentAudioDriver(void);
void SDL_GetVersion(SDL_version *version);
Uint32 SDL_GetQueuedAudioSize(SDL_AudioDeviceID device);
SDL_AudioStatus SDL_GetAudioDeviceStatus(SDL_AudioDeviceID device);
void SDL_ClearQueuedAudio(SDL_AudioDeviceID device);
void SDL_Delay(Uint32 ms);
int SDL_QueueAudio(SDL_AudioDeviceID device, const void *data, Uint32 bytes);
/* Threading API used by host_audio.c's audio thread. The double creates no thread,
   so host_audio.c consumes PCM synchronously on the calling thread, which is what
   these deterministic tests drive. */
#define SDLCALL
typedef struct SDL_mutex SDL_mutex;
typedef struct SDL_cond SDL_cond;
typedef struct SDL_Thread SDL_Thread;
typedef struct { int value; } SDL_atomic_t;
typedef int (SDLCALL *SDL_ThreadFunction)(void *data);
SDL_mutex *SDL_CreateMutex(void);
SDL_cond *SDL_CreateCond(void);
int SDL_LockMutex(SDL_mutex *m);
int SDL_UnlockMutex(SDL_mutex *m);
int SDL_CondSignal(SDL_cond *c);
int SDL_CondBroadcast(SDL_cond *c);
int SDL_CondWaitTimeout(SDL_cond *c, SDL_mutex *m, Uint32 ms);
SDL_Thread *SDL_CreateThreadWithStackSize(SDL_ThreadFunction fn, const char *name, size_t stack, void *data);
void SDL_WaitThread(SDL_Thread *t, int *status);
int SDL_AtomicSet(SDL_atomic_t *a, int v);
int SDL_AtomicGet(SDL_atomic_t *a);
#endif
"""

TEST_SOURCE = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <SDL.h>

static uint64_t now_ns = 1000000000ull;
static uint32_t mock_queue;
static unsigned fail_queue, clear_calls, submissions, opened, closed;
static uint64_t accepted_bytes;
static Uint32 drain_per_delay;
static unsigned delay_calls;
static SDL_AudioStatus mock_status = SDL_AUDIO_PAUSED;
static unsigned resume_calls, pause_calls;
static Uint32 hidden_queue_bytes;
static int16_t expected_samples[320];
static int mock_clock_gettime(clockid_t id, struct timespec *t) {
    assert(id == CLOCK_MONOTONIC);
    t->tv_sec = (time_t)(now_ns / 1000000000ull);
    t->tv_nsec = (long)(now_ns % 1000000000ull);
    return 0;
}
#define clock_gettime mock_clock_gettime
#include HOST_AUDIO_SOURCE
#undef clock_gettime

static UpstreamAudioPcm registered_sink;
static void *registered_user;
int SDL_InitSubSystem(Uint32 flags) { assert(flags == SDL_INIT_AUDIO); return 0; }
void SDL_QuitSubSystem(Uint32 flags) { assert(flags == SDL_INIT_AUDIO); }
SDL_AudioDeviceID SDL_OpenAudioDevice(const char *name, int capture,
        const SDL_AudioSpec *want, SDL_AudioSpec *got, int changes) {
    assert(!name && !capture && !changes);
    assert(want->freq == 32000 && want->channels == 2);
    assert(want->format == AUDIO_S16SYS && want->samples == 512);
    *got = *want;
    ++opened;
    now_ns += 135000000ull; /* Deterministic device-open stall. */
    return 2;
}
void SDL_CloseAudioDevice(SDL_AudioDeviceID d) { assert(d == 2); ++closed; }
void SDL_PauseAudioDevice(SDL_AudioDeviceID d, int pause) {
    assert(d == 2);
    /* Count pauses of a playing device: host_audio.c also pauses the freshly opened
       (already paused) device explicitly, which is not a disruption. */
    if(pause) { if (mock_status == SDL_AUDIO_PLAYING) ++pause_calls; mock_status = SDL_AUDIO_PAUSED; }
    else { ++resume_calls;mock_status = SDL_AUDIO_PLAYING; }
}
const char *SDL_GetError(void) { return "injected queue error"; }
const char *SDL_GetCurrentAudioDriver(void) { return "mock"; }
void SDL_GetVersion(SDL_version *v) { v->major = 2; v->minor = 32; v->patch = 74; }
Uint32 SDL_GetQueuedAudioSize(SDL_AudioDeviceID d) {
    assert(d == 2);
    return mock_queue > hidden_queue_bytes ? mock_queue - hidden_queue_bytes : 0;
}
SDL_AudioStatus SDL_GetAudioDeviceStatus(SDL_AudioDeviceID d) {
    assert(d == 2); return mock_status;
}
void SDL_Delay(Uint32 ms) {
    assert(ms == 2);
    ++delay_calls;
    assert(delay_calls <= 250);
    now_ns += (uint64_t)ms * 1000000ull;
    mock_queue = mock_queue > drain_per_delay ? mock_queue - drain_per_delay : 0;
}
void SDL_ClearQueuedAudio(SDL_AudioDeviceID d) { assert(d == 2); mock_queue = 0; ++clear_calls; }
int SDL_QueueAudio(SDL_AudioDeviceID d, const void *data, Uint32 bytes) {
    assert(d == 2 && bytes == sizeof(expected_samples));
    assert(memcmp(data, expected_samples, bytes) == 0);
    ++submissions;
    if (fail_queue) return -1;
    accepted_bytes += bytes;
    mock_queue += bytes;
    return 0;
}
static int mock_lock_object;
SDL_mutex *SDL_CreateMutex(void) { return (SDL_mutex *)&mock_lock_object; }
SDL_cond *SDL_CreateCond(void) { return (SDL_cond *)&mock_lock_object; }
int SDL_LockMutex(SDL_mutex *m) { (void)m; return 0; }
int SDL_UnlockMutex(SDL_mutex *m) { (void)m; return 0; }
int SDL_CondSignal(SDL_cond *c) { (void)c; return 0; }
int SDL_CondBroadcast(SDL_cond *c) { (void)c; return 0; }
int SDL_CondWaitTimeout(SDL_cond *c, SDL_mutex *m, Uint32 ms) { (void)c; (void)m; (void)ms; return 1; }
SDL_Thread *SDL_CreateThreadWithStackSize(SDL_ThreadFunction fn, const char *name, size_t stack, void *data) {
    (void)fn; (void)name; (void)stack; (void)data; return NULL;
}
void SDL_WaitThread(SDL_Thread *t, int *status) { (void)t; (void)status; }
int SDL_AtomicSet(SDL_atomic_t *a, int v) { int old = a->value; a->value = v; return old; }
int SDL_AtomicGet(SDL_atomic_t *a) { return a->value; }
void upstream_audio_set_pcm_sink(UpstreamAudioPcm cb, void *user) {
    registered_sink = cb;
    registered_user = user;
}
UpstreamAudioStats upstream_audio_stats(void) {
    UpstreamAudioStats s = {0}; return s;
}
static void deliver(void) {
    assert(registered_sink);
    registered_sink(registered_user, expected_samples, 160, 32000);
}
static void finish_test(void) {
    host_audio_shutdown();
    assert(!registered_sink && opened == 1 && closed == 1);
    assert(submitted_bytes == accepted_bytes);
}
static void accounting(void) {
    deliver();
    assert(frames_seen == 160 && submitted_bytes == 640);
    assert(nonzero_frames == 160 && peak == 1234);
    mock_queue = 64000; /* The existing overflow boundary is strictly greater. */
    deliver();
    assert(clear_calls == 0 && queue_resets == 0 && submitted_bytes == 1280);
    mock_queue = 65001; /* SDL compatibility estimates can be unaligned. */
    fail_queue = 1;
    deliver();
    assert(clear_calls == 1 && queue_resets == 1 && cleared_bytes_estimate == 65001);
    assert(queue_peak == 65001 && queue_errors == 1);
    assert(frames_seen == 480 && submitted_bytes == 1280 && mock_queue == 0);
    for (int i = 0; i < 5; ++i) deliver();
    assert(queue_errors == 6 && submitted_bytes == 1280);
    fail_queue = 0;
    deliver();
    assert(frames_seen == 9 * 160 && submissions == 9 && submitted_bytes == 1920);
    assert(nonzero_frames == frames_seen && queue_empty == 6);
    finish_test();
    assert(trace_lines == 0);
    puts("PASS accounting: payload, accepted bytes, failed queues, approximate overflow");
}
static void trace_bound(void) {
    deliver();
    assert(trace_lines == 1);
    /* Hundreds of callbacks in one instant must not bypass the 1Hz bound. */
    for (int i = 0; i < 300; ++i) { mock_queue = 640; deliver(); }
    assert(trace_lines == 1);
    now_ns += 999999999ull;
    mock_queue = 640;
    deliver();
    assert(trace_lines == 1);
    ++now_ns;
    mock_queue = 640;
    deliver();
    assert(trace_lines == 2);
    /* Rounded API queue estimates can grow more than newly accepted bytes.
     * Verify signed inferred removal, without calling that a hardware cursor. */
    now_ns += 1000000000ull;
    mock_queue = 10001;
    deliver();
    assert(trace_lines == 3);
    for (int i = 0; i < 140; ++i) {
        now_ns += 1000000000ull;
        mock_queue = 640;
        deliver();
    }
    assert(trace_lines == 127 && queue_resets == 0);
    finish_test();
    assert(trace_lines == 128 && queue_errors == 0);
    puts("PASS trace: opt-in, 1Hz throttle, signed estimate, 127 periodic plus final");
}
static void prime(unsigned chunks) {
    for (unsigned i = 0; i < chunks - 1; ++i) {
        deliver();
        assert(!started && !resume_calls && !delay_calls && !queue_empty);
        assert(mock_status == SDL_AUDIO_PAUSED);
    }
    deliver();
    assert(started && resume_calls == 1 && mock_status == SDL_AUDIO_PLAYING);
    assert(initial_buffer_frames == chunks * 160 && !delay_calls && !queue_empty);
}
static void backpressure(unsigned prime_chunks) {
    prime(prime_chunks);
    mock_queue = 44800;
    deliver(); /* Exactly 350ms: no wait. */
    assert(delay_calls == 0);
    mock_queue = 65001; /* Exceeds legacy clear bound, still preserve in realtime. */
    drain_per_delay = 256; /* 32kHz stereo S16, 2ms of playback. */
    deliver();
    assert(delay_calls == 79 && queue_wait_ms == 158 && queue_waits == 1);
    assert(mock_queue == 45417 && clear_calls == 0 && queue_resets == 0);
    assert(submitted_bytes == (prime_chunks + 2) * 640);
    assert(accepted_bytes == submitted_bytes && submissions == prime_chunks + 2);
    assert(queue_stalls == 0 && !queue_empty);
    mock_queue = 0;
    deliver(); /* A real empty queue after start must remain visible. */
    assert(queue_empty == 1 && initial_buffer_frames == prime_chunks * 160);
    finish_test();
    puts("PASS realtime: paused prebuffer, 350ms threshold, drain, real underrun visibility");
}
int main(int argc, char **argv) {
    assert(argc == 2);
    for (unsigned i = 0; i < 320; ++i) expected_samples[i] = i & 1 ? -1234 : 1234;
    int realtime = strcmp(argv[1], "accounting") && strcmp(argv[1], "trace");
    host_audio_init(realtime);
    assert(registered_sink && !registered_user && !opened);
    if (!strcmp(argv[1], "accounting")) accounting();
    else if (!strcmp(argv[1], "trace")) trace_bound();
    else if (!strcmp(argv[1], "startup-trace")) {
        deliver();now_ns += 1000000000ull;deliver();
        assert(!started && !resume_calls && !delay_calls && !queue_empty);
        assert(initial_buffer_frames == 320 && mock_status == SDL_AUDIO_PAUSED);
        finish_test();puts("PASS realtime: paused priming status and counters are explicit in trace");
    }
    else if (!strcmp(argv[1], "zero-trace")) {
        deliver(); /* Zero reported while paused: trace it, do not count underrun. */
        assert(!queue_empty && trace_zero_lines == 1 && !started);
        for (int i = 1; i < 70; ++i) deliver();
        assert(started);
        for (int i = 0; i < 40; ++i) {
            now_ns += 10000000ull;mock_queue = 0;deliver();
            mock_queue = 640;deliver(); /* Independent, isolated zero observations. */
        }
        assert(queue_empty == 40 && trace_zero_lines == 32 && !rebuffers);
        assert(!queue_stalls && !queue_resets);
        finish_test();puts("PASS zero trace: paused status, playing count, 32 record bound without suppression");
    }
    else if (!strcmp(argv[1], "drain")) backpressure(70);
    else if (!strcmp(argv[1], "adaptive")) {
        /* Isolated zeros grow the target without forcing rebuffer. Bursts add
         * 10ms; another within 15s grows x1.5+20ms. Then relax after 15s quiet. */
        prime(BUFFER_AUTO_START_MS/5);
        assert(buffer_ms == BUFFER_AUTO_START_MS);
        mock_queue = 0; deliver();
        assert(queue_empty == 1 && underruns == 1 && buffer_ms == BUFFER_AUTO_START_MS+20);
        mock_queue = 640;deliver();
        mock_queue = 0; now_ns += 500000000ull; deliver();
        assert(underruns == 2 && buffer_ms == BUFFER_AUTO_START_MS+30);
        mock_queue = 640;deliver();
        mock_queue = 0; now_ns += 5000000000ull; deliver();
        unsigned grown=(BUFFER_AUTO_START_MS+30)*3/2+20;
        if(grown>BUFFER_MAX_MS)grown=BUFFER_MAX_MS;
        assert(underruns == 3 && buffer_ms == grown);
        mock_queue = 640; now_ns += 14000000000ull; deliver();
        assert(buffer_ms == grown);
        mock_queue = 640; now_ns += 2000000000ull; deliver();
        assert(buffer_ms == grown-10);
        mock_queue = 640; now_ns += 3000000000ull; deliver();
        assert(buffer_ms == grown-10);
        mock_queue = 640; now_ns += 2000000000ull; deliver();
        assert(buffer_ms == grown-20 && !rebuffers);
        finish_test();puts("PASS realtime: adaptive +20ms, burst +10ms, repeated x1.5, 15s/5s relax");
    }
    else if (!strcmp(argv[1], "startup-stability")) {
        now_ns=100000000000000ull; /* Large host uptime must not shorten startup. */
        prime(BUFFER_AUTO_START_MS/5);
        mock_queue=640;deliver();
        assert(buffer_ms==BUFFER_AUTO_START_MS);
        now_ns+=14999999999ull;mock_queue=640;deliver();
        assert(buffer_ms==BUFFER_AUTO_START_MS);
        ++now_ns;mock_queue=640;deliver();
        assert(buffer_ms==BUFFER_AUTO_START_MS-10);
        finish_test();puts("PASS realtime: full 15s stability measured from playback start");
    }
    else if (!strcmp(argv[1], "rebuffer")) {
        prime(70);
        /* A zero estimate followed by available audio is harmless. */
        mock_queue=0;deliver();
        now_ns+=30000000ull;mock_queue=640;deliver();
        assert(!pause_calls && !rebuffers);
        /* Production at exactly 1x after starvation cannot refill an active
         * device. Five empty observations 5ms apart trigger a paused refill. */
        for(unsigned i=0;i<5;++i) {
            now_ns+=5000000ull;mock_queue=0;deliver();
            if(i<4)assert(started && !rebuffering && !pause_calls);
        }
        assert(!started && rebuffering && pause_calls==1 && rebuffers==1);
        assert(mock_status==SDL_AUDIO_PAUSED && initial_buffer_frames==11200);
        assert(rebuffer_frames==160 && resume_calls==1 && queue_empty==6);
        for(unsigned i=0;i<68;++i) {
            now_ns+=5000000ull;deliver();
            assert(!started && resume_calls==1 && queue_empty==6);
        }
        now_ns+=5000000ull;deliver();
        assert(started && !rebuffering && resume_calls==2 && mock_queue==44800);
        assert(rebuffer_frames==11200 && initial_buffer_frames==11200);
        for(unsigned i=0;i<20;++i) {
            now_ns+=5000000ull;mock_queue-=640;deliver();
            assert(mock_queue==44800 && queue_empty==6);
        }
        assert(!clear_calls && !queue_resets && !queue_stalls);
        finish_test();puts("PASS realtime: sustained starvation rebuffer restores 350ms headroom at 1x production");
    }
    else if (!strcmp(argv[1], "recover")) {
        /* Without GC_AUDIO_STRICT a stopped device is closed, then reopened 2s later. */
        prime(BUFFER_AUTO_START_MS/5);
        mock_status = SDL_AUDIO_STOPPED;
        deliver();
        assert(closed == 1 && !device && !started && queue_stalls == 1);
        now_ns += 1000000000ull; deliver();
        assert(opened == 1);
        now_ns += 1500000000ull; mock_status = SDL_AUDIO_PAUSED; mock_queue = 0; deliver();
        assert(opened == 2 && device && !started && reopen_count == 1);
        host_audio_shutdown();
        assert(closed == 2 && !registered_sink);
        puts("PASS realtime: lost device is reopened instead of aborting");
    }
    else if (!strcmp(argv[1], "held-samples")) {
        hidden_queue_bytes = 28;prime(71);hidden_queue_bytes = 0;
        assert(initial_buffer_frames == 11360 && submitted_bytes == 45440);
        finish_test();puts("PASS realtime: resampler-held samples postpone resume to next chunk");
    }
    else {
        prime(70);
        mock_queue = 640;
        if (!strcmp(argv[1], "stalled")) mock_queue = 65001;
        else if (!strcmp(argv[1], "paused")) mock_status = SDL_AUDIO_PAUSED;
        else if (!strcmp(argv[1], "stopped")) mock_status = SDL_AUDIO_STOPPED;
        else { assert(!strcmp(argv[1], "queue-failed")); fail_queue = 1; }
        deliver();
        assert(!"realtime failure must abort");
    }
    return 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--android-defaults", action="store_true", help="exercise Android buffering constants with the same SDL double")
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1] / "platform/desktop/upstream_gx/host_audio.c"
    with tempfile.TemporaryDirectory(prefix="gc-host-audio-test-") as directory:
        work = Path(directory)
        (work / "SDL.h").write_text(SDL_HEADER)
        test_source = work / "test_host_audio.c"
        test_source.write_text(TEST_SOURCE)
        binary = work / "test_host_audio"
        flags = ["-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g",
                 "-Wall", "-Wextra", "-Werror", "-pedantic", "-fno-omit-frame-pointer"]
        if args.android_defaults:
            flags += ["-D__ANDROID__"]
        if args.sanitize:
            flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        subprocess.run(shlex.split(args.cc) + flags + [f'-DHOST_AUDIO_SOURCE="{source}"',
                       "-I", str(work), str(test_source), "-o", str(binary)], check=True)
        env = os.environ.copy()
        compiler_dir = str(Path(shlex.split(args.cc)[0]).resolve().parent)
        env["PATH"] = compiler_dir + os.pathsep + env.get("PATH", "")
        for key in ("GC_AUDIO_TRACE", "GC_AUDIO_MUTE", "GC_AUDIO_WAV", "GC_AUDIO_STRICT", "GC_AUDIO_BUFFER_MS", "GC_AUDIO_VOLUME"):
            env.pop(key, None)
        fatal = ("stalled", "paused", "stopped", "queue-failed")
        for scenario in ("accounting", "trace", "startup-trace", "zero-trace", "drain", "held-samples",
                         "adaptive", "startup-stability", "rebuffer", "recover") + fatal:
            scenario_env = dict(env)
            if scenario in ("trace", "startup-trace", "zero-trace"):
                scenario_env["GC_AUDIO_TRACE"] = "1"
            # The measured fixed 350ms target; "adaptive"/"recover" exercise the defaults.
            if scenario not in ("adaptive", "startup-stability", "recover"):
                scenario_env["GC_AUDIO_BUFFER_MS"] = "350"
            if scenario in fatal:
                scenario_env["GC_AUDIO_STRICT"] = "1"
            result = subprocess.run([str(binary), scenario], env=scenario_env,
                                    capture_output=True, text=True, timeout=15, cwd=work)
            if scenario in fatal:
                # abort(): SIGABRT on POSIX; exit code 3 (msvcrt) or fast-fail 0xC0000409 (UCRT) on Windows.
                assert result.returncode in (-signal.SIGABRT, 3, 0xC0000409), (scenario, result.returncode, result.stderr)
                assert "[host-audio] fatal:" in result.stderr, result.stderr
                assert "submitted_bytes=44800 resets=0" in result.stderr, result.stderr
                if scenario == "stalled":
                    assert "did not drain within 500ms" in result.stderr
                    assert "queue_waits=1 queue_wait_ms=500 queue_stalls=1 queue_errors=0" in result.stderr
                elif scenario == "queue-failed":
                    assert "realtime audio enqueue failed" in result.stderr
                    assert "queue_waits=0 queue_wait_ms=0 queue_stalls=0 queue_errors=1" in result.stderr
                else:
                    assert "device is not playing" in result.stderr
                    assert "queue_waits=0 queue_wait_ms=0 queue_stalls=1 queue_errors=0" in result.stderr
                print("PASS realtime fatal: " + scenario)
                continue
            if result.returncode:
                raise AssertionError(f"{scenario} exited {result.returncode}\n{result.stdout}\n{result.stderr}")
            observations = [line for line in result.stderr.splitlines()
                            if line.startswith("[host-audio-time] final=")]
            if scenario == "accounting":
                assert not observations, result.stderr
                assert "[host-audio-time]" not in result.stderr, result.stderr
                assert result.stderr.count("queue failed: injected queue error") == 4, result.stderr
                assert "cleared_bytes_estimate=65001 queue_errors=6 submitted_bytes=1920" in result.stderr, result.stderr
            elif scenario == "zero-trace":
                zeros = [line for line in result.stderr.splitlines() if line.startswith("[host-audio-zero]")]
                assert len(zeros) == 32, result.stderr
                assert "status=2 started=0" in zeros[0] and "empty_events=0" in zeros[0]
                assert "event=32 " in zeros[-1] and "empty_events=31" in zeros[-1]
                assert "status=1 started=1" in zeros[1] and "callback_gap_ms=10.000" in zeros[1]
                assert "zero_interval_ms=10.000" in zeros[2]
                assert "queue_empty_events=40" in result.stderr
            elif scenario == "startup-trace":
                assert len(observations) == 3, result.stderr
                assert all("status=2 " in line and "started=0" in line for line in observations)
                assert "initial_buffer_frames=160" in observations[0]
                assert "initial_buffer_frames=320" in observations[-1]
                assert "queue_empty_events=0" in result.stderr
            elif scenario == "trace":
                assert len(observations) == 128, len(observations)
                assert sum("final=0 " in line for line in observations) == 127
                assert "final=1 " in observations[-1], observations[-1]
                assert "pcm_wall_s=0.135000" in observations[0], observations[0]
                assert "interval_s=1.000000" in observations[1], observations[1]
                assert "api_removed_fps=-2180.250" in observations[2], observations[2]
                assert "cleared_bytes_estimate=0 queue_errors=0" in observations[-1]
                assert "driver=mock rate=32000 format=8010 channels=2 samples=512" in result.stderr
            print(result.stdout.strip())
        print("PASS host sink regression (" + ("ASan + UBSan" if args.sanitize else "strict C11") + ")")


if __name__ == "__main__":
    main()
