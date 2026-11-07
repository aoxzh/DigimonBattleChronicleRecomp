#ifndef DIGIMON_UPSTREAM_AUDIO_H
#define DIGIMON_UPSTREAM_AUDIO_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Single instance, synchronous guest-thread API. Callbacks must not reenter.
 * RAM is borrowed big-endian MEM1; no second guest RAM/ARAM allocation. */
typedef uint8_t (*UpstreamAudioAramRead)(void *user, uint32_t byte_address);
typedef void (*UpstreamAudioIrq)(void *user);
typedef void (*UpstreamAudioPcm)(void *user, const int16_t *stereo_lr,
                                 uint32_t frames, uint32_t sample_rate);
typedef struct {
    uint8_t *ram;
    size_t ram_size;
    UpstreamAudioAramRead aram_read;
    UpstreamAudioIrq raise_dsp_irq;
    void *user;
} UpstreamAudioConfig;
enum {
    UPSTREAM_AUDIO_OK = 0,
    UPSTREAM_AUDIO_PARTIAL = 1,
    UPSTREAM_AUDIO_ERROR = -1,
    UPSTREAM_AUDIO_UNSUPPORTED_UCODE = -2
};
enum {
    UPSTREAM_AUDIO_LIMIT_COMPRESSOR = 1u << 0,
    UPSTREAM_AUDIO_LIMIT_POLYPHASE = 1u << 1,
    UPSTREAM_AUDIO_LIMIT_LPF = 1u << 2,
    UPSTREAM_AUDIO_LIMIT_ITD = 1u << 3,
    UPSTREAM_AUDIO_LIMIT_UNKNOWN_COMMAND = 1u << 4,
    UPSTREAM_AUDIO_LIMIT_DPOP = 1u << 5,
    UPSTREAM_AUDIO_LIMIT_DPL2 = 1u << 6
};
typedef struct {
    uint32_t ucode_hash;
    uint32_t limitations; /* Sticky since bind; PARTIAL means diagnostic output. */
    uint64_t command_lists;
    uint64_t mixed_blocks;
    uint64_t dma_frames;
    uint64_t nonzero_dma_frames;
} UpstreamAudioStats;
int upstream_audio_bind(const UpstreamAudioConfig *config);
void upstream_audio_unbind(void);
/* DSP reset queues ROM mail 8071FEED. IPL init mail is separate. */
void upstream_audio_reset(void);
void upstream_audio_ipl_ready(void);
int upstream_audio_mail(uint32_t mail);
uint16_t upstream_audio_mail_hi(void); /* peek, occupied bit included */
uint16_t upstream_audio_mail_lo(void); /* consume, raise next head's IRQ */
/* Identify by uploaded bytes, never guess AX from first-task size. */
int upstream_audio_boot(uint32_t address, uint32_t bytes, uint32_t vector);
int upstream_audio_process(uint32_t command_list, uint16_t words);
/* Optional sink sees only AI DMA bytes read from guest RAM (BE R,L -> host L,R).
 * Pointer is valid for the callback only. No host device/thread is created. */
void upstream_audio_set_pcm_sink(UpstreamAudioPcm sink, void *user);
int upstream_audio_dma(uint32_t address, uint32_t bytes, uint32_t sample_rate);
UpstreamAudioStats upstream_audio_stats(void);
const char *upstream_audio_last_error(void);
#ifdef __cplusplus
}
#endif
#endif
