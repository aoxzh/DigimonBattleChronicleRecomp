/* Optional host sink: guest AX/AI remains owned by the audio adapter and HAL.
 * The guest thread only copies PCM into a bounded queue; this file's audio
 * thread owns SDL's device, adaptive buffering, WAV capture and underrun
 * recovery. A render/loader stall therefore cannot block the guest on SDL. */
#include "../upstream_audio/upstream_audio.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static SDL_AudioDeviceID device;
static FILE *wav;
static uint32_t rate, wav_bytes;
static uint64_t frames_seen, nonzero_frames, cleared_bytes_estimate;
static uint32_t queue_peak, queue_resets, queue_empty;
static int16_t peak;
static int playback, realtime_playback;
/* Queue target for paced playback. A fixed 350ms covered the measured 309ms
 * synchronous GX/resource stall but delayed every sound by that much. Measured
 * play on the dev host stalls 50-90ms most seconds and 150-600ms at loads, so
 * auto mode starts at 120ms. An isolated dropout (one load) adds 20ms; another
 * within 15s means play itself stalls, so the target grows x1.5+20ms (max
 * 350ms). Empty queues within 1s add 10ms each. After 15s without a dropout
 * it shrinks 10ms every 5s down to 80ms. A fixed target comes from
 * host_audio_configure or GC_AUDIO_BUFFER_MS. */
#define BUFFER_MAX_MS 350u
#ifdef __ANDROID__
/* The recompiled game on a phone can stall longer than a desktop, and Android
 * audio HALs add their own scheduling jitter. The Snapdragon 8 Gen 2 test phone still underran at
 * 160-220 ms during boot, so start at 220 and never shrink below 200; the
 * adaptive path can still grow to 350 ms. */
#define BUFFER_AUTO_MIN_MS 200u
#define BUFFER_AUTO_START_MS 220u
#else
#define BUFFER_AUTO_MIN_MS 80u
#define BUFFER_AUTO_START_MS 120u
#endif
static volatile unsigned buffer_ms=BUFFER_AUTO_START_MS, fixed_buffer_ms;
static uint64_t last_underrun_ns, last_shrink_ns, buffer_stable_since_ns;
static unsigned underruns;
static int started, rebuffering;
static uint64_t initial_buffer_frames, rebuffer_frames, empty_since_ns;
static unsigned rebuffers;
/* SDL may retain a hardware period after its software queue reaches zero.
 * An isolated zero estimate does not justify a disruptive pause. Repeated
 * zero availability for 20ms does: refill the target while paused so recovery
 * restores actual headroom even when production only returns to real time. */
#define REBUFFER_EMPTY_NS 20000000ull
static uint64_t queue_waits, queue_wait_ms, queue_stalls;
static uint64_t submitted_bytes, queue_errors;
/* Guest -> audio thread queue. Blocks carry their own rate so a sample-rate
 * change is still ordered. The cap is a hard bound; the guest is paced by its
 * own clock, so normal production never reaches it. */
typedef struct PcmBlock {
    struct PcmBlock *next;
    uint32_t frames, rate;
    int16_t samples[];
} PcmBlock;
static SDL_mutex *queue_lock;
static SDL_cond *queue_cv;
static PcmBlock *queue_head, *queue_tail;
static size_t queue_frames;
static uint32_t queue_rate;
static int queue_quit;
static SDL_Thread *audio_thread;
static SDL_atomic_t pause_request;
static void pcm_consume(void *user, const int16_t *samples, uint32_t frames, uint32_t sample_rate);
static void apply_device_pause(void);
/* Player builds recover from device loss (unplugged headset, driver switch):
 * the device is closed and reopened after a short delay, restarting the
 * prebuffer. GC_AUDIO_STRICT=1 keeps the fail-fast diagnostic behaviour. */
static int strict, enabled=1;
static volatile int volume=100;
static unsigned reopen_count;
static uint64_t reopen_at;
static uint64_t monotonic_ns(void);
static void audio_fatal(const char *reason){
    fprintf(stderr,"[host-audio] %s: %s; SDL=%s queue_waits=%llu queue_wait_ms=%llu queue_stalls=%llu queue_errors=%llu submitted_bytes=%llu resets=%u\n",
        strict?"fatal":"device lost",reason,SDL_GetError(),(unsigned long long)queue_waits,
        (unsigned long long)queue_wait_ms,(unsigned long long)queue_stalls,
        (unsigned long long)queue_errors,(unsigned long long)submitted_bytes,queue_resets);
    if(strict)abort();
    if(device){SDL_CloseAudioDevice(device);device=0;}
    started=0;
    reopen_at=monotonic_ns()+2000000000ull;
}
/* latency_ms: 0 = auto (adaptive), otherwise a fixed queue target.
 * GC_AUDIO_BUFFER_MS / GC_AUDIO_VOLUME override both for measurement runs. */
void host_audio_configure(int on,int vol,int latency_ms){
    const unsigned was_fixed=fixed_buffer_ms;
    enabled=on!=0;volume=vol<0?0:vol>100?100:vol;
    fixed_buffer_ms=latency_ms<=0?0u:latency_ms<20?20u:latency_ms>(int)BUFFER_MAX_MS?BUFFER_MAX_MS:(unsigned)latency_ms;
    /* Live change from the in-game menu (enabling sound still needs a restart). */
    if(fixed_buffer_ms)buffer_ms=fixed_buffer_ms;
    else if(was_fixed)buffer_ms=BUFFER_AUTO_START_MS;
}
static void buffer_underrun(void){
    uint64_t now=monotonic_ns();
    const int seen=last_underrun_ns!=0;
    const uint64_t since=now-last_underrun_ns;
    ++underruns;
    last_underrun_ns=last_shrink_ns=buffer_stable_since_ns=now;
    if(fixed_buffer_ms)return;
    /* A burst of underruns means the device is starving continuously, not just
     * one load spike. Keep growing on every one of those too (10 ms at a time);
     * the old early-return froze the target at 185 ms while the phone was still
     * producing audio below real time. Latency is bounded by BUFFER_MAX_MS. */
    unsigned next;
    if(seen&&since<1000000000ull)next=buffer_ms+10u;
    else if(seen&&since<15000000000ull)next=buffer_ms*3u/2u+20u;
    else next=buffer_ms+20u;
    if(next>BUFFER_MAX_MS)next=BUFFER_MAX_MS;
    if(next!=buffer_ms)fprintf(stderr,"[host-audio] underrun #%u: buffer %ums -> %ums\n",underruns,buffer_ms,next);
    buffer_ms=next;
}
static void buffer_relax(void){
    if(fixed_buffer_ms||buffer_ms<=BUFFER_AUTO_MIN_MS)return;
    uint64_t now=monotonic_ns();
    if(now-buffer_stable_since_ns<15000000000ull||now-last_shrink_ns<5000000000ull)return;
    last_shrink_ns=now;
    buffer_ms=buffer_ms-10u<BUFFER_AUTO_MIN_MS?BUFFER_AUTO_MIN_MS:buffer_ms-10u;
}
void host_audio_pause(int pause){
    SDL_AtomicSet(&pause_request,pause?1:0);
    if(queue_lock)SDL_CondSignal(queue_cv);
}
static void apply_device_pause(void){
    if(!device)return;
    const int want=SDL_AtomicGet(&pause_request)||(realtime_playback&&!started);
    SDL_PauseAudioDevice(device,want);
}
/* GC_AUDIO_TRACE=1: at most 128 observations, once per second plus shutdown.
 * SDL's queue is software availability, not a hardware playback cursor. In
 * sdl2-compat it is resampled output availability scaled back to source bytes,
 * so even stereo S16 queue sizes need not be multiples of four. */
static int trace;
static unsigned trace_lines;
static uint64_t trace_start, trace_first, trace_last, trace_frames;
static uint64_t trace_submitted, trace_dropped, trace_gap, pcm_last;
static uint32_t trace_queue;
static unsigned trace_zero_lines;
static uint64_t trace_zero_last;
/* A zero SDL software-availability estimate is not a hardware underrun proof.
 * Keep these observations separate from the 1Hz trace so startup bursts remain
 * visible even after the periodic trace budget has been exhausted. */
static void trace_zero_queue(uint64_t previous_pcm,SDL_AudioStatus status){
    if(!trace||trace_zero_lines>=32)return;
    uint64_t now=monotonic_ns();
    fprintf(stderr,"[host-audio-zero] event=%u wall_s=%.6f pcm_wall_s=%.6f zero_interval_ms=%.3f callback_gap_ms=%.3f pcm_frames=%llu submitted_bytes=%llu status=%d started=%d initial_buffer_frames=%llu queue_waits=%llu queue_wait_ms=%llu queue_stalls=%llu queue_errors=%llu empty_events=%u\n",
        ++trace_zero_lines,(double)(now-trace_start)/1e9,(double)(now-trace_first)/1e9,
        trace_zero_last?(double)(now-trace_zero_last)/1e6:0,
        previous_pcm?(double)(now-previous_pcm)/1e6:0,
        (unsigned long long)frames_seen,(unsigned long long)submitted_bytes,
        (int)status,started,(unsigned long long)initial_buffer_frames,
        (unsigned long long)queue_waits,(unsigned long long)queue_wait_ms,
        (unsigned long long)queue_stalls,(unsigned long long)queue_errors,queue_empty);
    trace_zero_last=now;
}
static uint64_t monotonic_ns(void){
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
    return (uint64_t)now.tv_sec*1000000000ull+(uint64_t)now.tv_nsec;
}
/* Preserve queued PCM in paced playback. The target uses SDL's source-byte
 * estimate; one incoming DMA chunk may take the queue just above it. Both the
 * monotonic deadline and delay budget bound a device that stops draining. */
static void wait_for_audio(uint32_t queued){
    uint32_t target=rate*4u*buffer_ms/1000u;
    unsigned waited=0;
    uint64_t start=0;
    if(queued>target){++queue_waits;start=monotonic_ns();}
    for(;;){
        /* The pause menu deliberately stops the device; that is not a loss. */
        if(SDL_AtomicGet(&pause_request))return;
        if(SDL_GetAudioDeviceStatus(device)!=SDL_AUDIO_PLAYING){
            ++queue_stalls;audio_fatal("realtime audio device is not playing");return;
        }
        if(queued<=target)return;
        if(waited>=500u||monotonic_ns()-start>=500000000ull){
            ++queue_stalls;audio_fatal("realtime audio queue did not drain within 500ms");return;
        }
        SDL_Delay(2);
        waited+=2;queue_wait_ms+=2;
        queued=SDL_GetQueuedAudioSize(device);
    }
}
static void trace_pcm(int final){
    if(!trace||trace_lines>=(final?128u:127u)||!rate)return;
    uint64_t now=monotonic_ns();
    if(!final&&trace_last&&now-trace_last<1000000000ull)return;
    uint32_t queued=device?SDL_GetQueuedAudioSize(device):0;
    double wall=trace_last?(double)(now-trace_last)/1e9:0;
    /* Signed because SDL resampling can round its estimate either way. */
    int64_t removed=(int64_t)(submitted_bytes-trace_submitted)
        -(int64_t)(cleared_bytes_estimate-trace_dropped)-(int64_t)queued+trace_queue;
    fprintf(stderr,"[host-audio-time] final=%d wall_s=%.6f pcm_wall_s=%.6f pcm_frames=%llu pcm_s=%.6f interval_s=%.6f received_fps=%.3f queue_bytes=%u queue_s=%.6f submitted_bytes=%llu api_removed_fps=%.3f max_gap_ms=%.3f resets=%u cleared_bytes_estimate=%llu queue_errors=%llu status=%d queue_waits=%llu queue_wait_ms=%llu queue_stalls=%llu initial_buffer_frames=%llu started=%d rebuffers=%u rebuffer_frames=%llu rebuffering=%d\n",
        final,(double)(now-trace_start)/1e9,(double)(now-trace_first)/1e9,
        (unsigned long long)frames_seen,(double)frames_seen/rate,wall,
        wall?(double)(frames_seen-trace_frames)/wall:0,queued,(double)queued/(rate*4),
        (unsigned long long)submitted_bytes,wall?(double)removed/4/wall:0,
        (double)trace_gap/1e6,queue_resets,(unsigned long long)cleared_bytes_estimate,
        (unsigned long long)queue_errors,device?(int)SDL_GetAudioDeviceStatus(device):0,
        (unsigned long long)queue_waits,(unsigned long long)queue_wait_ms,(unsigned long long)queue_stalls,(unsigned long long)initial_buffer_frames,started,rebuffers,(unsigned long long)rebuffer_frames,rebuffering);
    ++trace_lines;trace_last=now;trace_frames=frames_seen;
    trace_submitted=submitted_bytes;trace_dropped=cleared_bytes_estimate;
    trace_queue=queued;trace_gap=0;
}
static void le16(FILE *f,uint16_t v){fputc(v&255,f);fputc(v>>8,f);}
static void le32(FILE *f,uint32_t v){le16(f,v&65535);le16(f,v>>16);}
static void header(FILE*f,uint32_t n){
    rewind(f);fwrite("RIFF",1,4,f);le32(f,36+n);fwrite("WAVEfmt ",1,8,f);le32(f,16);le16(f,1);le16(f,2);le32(f,rate);le32(f,rate*4);le16(f,4);le16(f,16);fwrite("data",1,4,f);le32(f,n);
}
static void open_device(void){
    SDL_AudioSpec want={0},got={0};want.freq=(int)rate;want.format=AUDIO_S16SYS;want.channels=2;want.samples=512;
    device=SDL_OpenAudioDevice(NULL,0,&want,&got,0);
    if(device){
        started=0;rebuffering=0;empty_since_ns=0;
        if(!realtime_playback){started=1;}
        apply_device_pause();
        if(trace||reopen_count){
            SDL_version version;SDL_GetVersion(&version);
            fprintf(stderr,"[host-audio-time] SDL=%u.%u.%u driver=%s rate=%d format=%04x channels=%u samples=%u reopen=%u\n",
                version.major,version.minor,version.patch,SDL_GetCurrentAudioDriver(),
                got.freq,got.format,got.channels,got.samples,reopen_count);
        }
    }else{
        if(realtime_playback)audio_fatal("cannot open realtime audio device");
        else fprintf(stderr,"[host-audio] %s\n",SDL_GetError());
    }
}
static void pcm_consume(void *user,const int16_t *samples,uint32_t frames,uint32_t sample_rate){
    (void)user;
    uint64_t previous_pcm=pcm_last;
    if(trace){
        uint64_t now=monotonic_ns();
        if(!trace_first)trace_first=now;
        if(pcm_last&&now-pcm_last>trace_gap)trace_gap=now-pcm_last;
        pcm_last=now;
    }
    if(!rate){
        rate=sample_rate;
        const char*path=getenv("GC_AUDIO_WAV");
        if(path&&*path){wav=fopen(path,"wb");if(wav)header(wav,0);}
        if(playback)open_device();
    }else if(playback&&!device&&!strict&&monotonic_ns()>=reopen_at){
        if(reopen_count<1000){++reopen_count;open_device();}
    }
    if(rate!=sample_rate){if(realtime_playback&&strict)audio_fatal("realtime PCM sample-rate changed");fprintf(stderr,"[host-audio] sample-rate changed %u->%u; dropping block\n",rate,sample_rate);return;}
    for(uint32_t i=0;i<frames;++i){int l=samples[i*2],r=samples[i*2+1];nonzero_frames+=(l!=0||r!=0);int p=abs(l)>abs(r)?abs(l):abs(r);if(p>peak)peak=p>32767?32767:p;}
    frames_seen+=frames;
    if(wav&&wav_bytes<0xffff0000u-frames*4){for(uint32_t i=0;i<frames*2;++i)le16(wav,(uint16_t)samples[i]);wav_bytes+=frames*4;}
    if(volume<100&&frames&&device){
        static int16_t *scaled;static uint32_t scaled_frames;
        if(frames>scaled_frames){int16_t *p=(int16_t*)realloc(scaled,frames*4u);if(!p)return;scaled=p;scaled_frames=frames;}
        for(uint32_t i=0;i<frames*2;++i)scaled[i]=(int16_t)(samples[i]*volume/100);
        samples=scaled;
    }
    if(device){
        uint32_t queued=SDL_GetQueuedAudioSize(device);
        if(queued>queue_peak)queue_peak=queued;
        if(realtime_playback&&started)buffer_relax();
        if(!queued){
            SDL_AudioStatus status=SDL_GetAudioDeviceStatus(device);
            if(frames_seen>frames&&status==SDL_AUDIO_PLAYING){++queue_empty;if(realtime_playback&&started)buffer_underrun();}
            trace_zero_queue(previous_pcm,status);
            if(realtime_playback&&started&&status==SDL_AUDIO_PLAYING){
                uint64_t now=monotonic_ns();
                if(!empty_since_ns)empty_since_ns=now;
                else if(now-empty_since_ns>=REBUFFER_EMPTY_NS){
                    SDL_PauseAudioDevice(device,1);
                    started=0;rebuffering=1;++rebuffers;
                    fprintf(stderr,"[host-audio] rebuffer #%u: software queue empty for %.3fms, target=%ums\n",
                        rebuffers,(double)(now-empty_since_ns)/1e6,buffer_ms);
                    empty_since_ns=0;
                }
            }else empty_since_ns=0;
        }else empty_since_ns=0;
        if(realtime_playback){if(started)wait_for_audio(queued);}
        else if(queued>rate*4/2){cleared_bytes_estimate+=queued;++queue_resets;SDL_ClearQueuedAudio(device);}
    }
    if(device){
        if(SDL_QueueAudio(device,samples,frames*4)==0){
            submitted_bytes+=(uint64_t)frames*4;
            if(realtime_playback&&!started){
                if(rebuffering)rebuffer_frames+=frames;
                else initial_buffer_frames+=frames;
                if(SDL_GetQueuedAudioSize(device)>=rate*4u*buffer_ms/1000u){
                    started=1;rebuffering=0;apply_device_pause();
                    /* Stability starts at real playback, not at OS boot time. */
                    buffer_stable_since_ns=last_shrink_ns=monotonic_ns();
                }
            }
        }
        else{
            ++queue_errors;
            if(queue_errors<=4)fprintf(stderr,"[host-audio] queue failed: %s\n",SDL_GetError());
            if(realtime_playback)audio_fatal("realtime audio enqueue failed");
        }
    }
    trace_pcm(0);
}
/* Guest-thread producer: copy and return immediately. The only wait is a hard
 * queue bound so a stalled audio thread cannot grow memory without limit. */
static void pcm(void *user,const int16_t *samples,uint32_t frames,uint32_t sample_rate){
    (void)user;
    if(!frames)return;
    if(!audio_thread){pcm_consume(NULL,samples,frames,sample_rate);return;}
    PcmBlock *b=(PcmBlock*)malloc(sizeof(*b)+(size_t)frames*4u);
    if(!b)return;
    b->next=NULL;b->frames=frames;b->rate=sample_rate;
    memcpy(b->samples,samples,(size_t)frames*4u);
    SDL_LockMutex(queue_lock);
    if(queue_quit){SDL_UnlockMutex(queue_lock);free(b);return;}
    if(!queue_rate)queue_rate=sample_rate;
    const size_t cap=(size_t)(queue_rate?queue_rate:48000u)*3u/2u;
    while(realtime_playback&&queue_frames>cap&&!queue_quit)
        SDL_CondWaitTimeout(queue_cv,queue_lock,20);
    if(queue_quit){SDL_UnlockMutex(queue_lock);free(b);return;}
    if(queue_tail)queue_tail->next=b;else queue_head=b;
    queue_tail=b;
    queue_frames+=frames;
    SDL_CondSignal(queue_cv);
    SDL_UnlockMutex(queue_lock);
}
static int SDLCALL audio_thread_main(void *unused){
    (void)unused;
    for(;;){
        PcmBlock *b=NULL;
        SDL_LockMutex(queue_lock);
        while(!queue_head&&!queue_quit)
            SDL_CondWaitTimeout(queue_cv,queue_lock,50);
        if(!queue_head&&queue_quit){SDL_UnlockMutex(queue_lock);break;}
        b=queue_head;
        queue_head=b->next;
        if(!queue_head)queue_tail=NULL;
        queue_frames-=b->frames;
        SDL_CondBroadcast(queue_cv);
        SDL_UnlockMutex(queue_lock);
        apply_device_pause();
        pcm_consume(NULL,b->samples,b->frames,b->rate);
        apply_device_pause();
        free(b);
    }
    return 0;
}
void host_audio_init(int realtime){
    realtime_playback=realtime!=0;
    trace=getenv("GC_AUDIO_TRACE")&&strcmp(getenv("GC_AUDIO_TRACE"),"1")==0;
    if(trace)trace_start=monotonic_ns();
    strict=getenv("GC_AUDIO_STRICT")&&strcmp(getenv("GC_AUDIO_STRICT"),"1")==0;
    if(getenv("GC_AUDIO_BUFFER_MS"))host_audio_configure(enabled,volume,atoi(getenv("GC_AUDIO_BUFFER_MS")));
    if(getenv("GC_AUDIO_VOLUME"))host_audio_configure(enabled,atoi(getenv("GC_AUDIO_VOLUME")),(int)fixed_buffer_ms);
    buffer_ms=fixed_buffer_ms?fixed_buffer_ms:BUFFER_AUTO_START_MS;
    playback=enabled&&!getenv("GC_AUDIO_MUTE");
    if(playback&&SDL_InitSubSystem(SDL_INIT_AUDIO)!=0){if(realtime_playback&&strict)audio_fatal("cannot initialize realtime audio");fprintf(stderr,"[host-audio] %s\n",SDL_GetError());playback=0;}
    if(!queue_lock){queue_lock=SDL_CreateMutex();queue_cv=SDL_CreateCond();}
    queue_quit=0;queue_head=queue_tail=NULL;queue_frames=0;queue_rate=0;
    SDL_AtomicSet(&pause_request,0);
    if(queue_lock&&queue_cv){
        audio_thread=SDL_CreateThreadWithStackSize(audio_thread_main,"audio",1024u*1024u,NULL);
        if(!audio_thread)fprintf(stderr,"[host-audio] audio thread unavailable: %s\n",SDL_GetError());
    }
    upstream_audio_set_pcm_sink(pcm,NULL);
}
void host_audio_shutdown(void){
    upstream_audio_set_pcm_sink(NULL,NULL);
    if(audio_thread){
        SDL_LockMutex(queue_lock);
        queue_quit=1;
        SDL_CondBroadcast(queue_cv);
        SDL_UnlockMutex(queue_lock);
        SDL_WaitThread(audio_thread,NULL);
        audio_thread=NULL;
    }
    trace_pcm(1);
    if(wav){header(wav,wav_bytes);fclose(wav);wav=NULL;}
    if(device){SDL_CloseAudioDevice(device);device=0;}
    if(playback)SDL_QuitSubSystem(SDL_INIT_AUDIO);
    fprintf(stderr,"[host-audio] buffer_ms=%u%s underruns=%u\n",buffer_ms,fixed_buffer_ms?" (fixed)":" (auto)",underruns);
    fprintf(stderr,"[host-audio] queue_peak_bytes=%u queue_empty_events=%u resets=%u cleared_bytes_estimate=%llu queue_errors=%llu submitted_bytes=%llu queue_waits=%llu queue_wait_ms=%llu queue_stalls=%llu initial_buffer_frames=%llu started=%d rebuffers=%u rebuffer_frames=%llu rebuffering=%d\n",queue_peak,queue_empty,queue_resets,(unsigned long long)cleared_bytes_estimate,(unsigned long long)queue_errors,(unsigned long long)submitted_bytes,(unsigned long long)queue_waits,(unsigned long long)queue_wait_ms,(unsigned long long)queue_stalls,(unsigned long long)initial_buffer_frames,started,rebuffers,(unsigned long long)rebuffer_frames,rebuffering);
    UpstreamAudioStats s=upstream_audio_stats();
    fprintf(stderr,"[host-audio] pcm_frames=%llu nonzero=%llu peak=%d rate=%u ucode=%08x lists=%llu mixed=%llu dma=%llu nonzero_dma=%llu limits=%08x\n",
      (unsigned long long)frames_seen,(unsigned long long)nonzero_frames,peak,rate,s.ucode_hash,
      (unsigned long long)s.command_lists,(unsigned long long)s.mixed_blocks,(unsigned long long)s.dma_frames,(unsigned long long)s.nonzero_dma_frames,s.limitations);
}
