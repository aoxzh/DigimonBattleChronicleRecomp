#include "upstream_runtime.h"
#include "../../../third_party/gcn-recomp/audio/ax.h"
#include <array>
#include <deque>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
UpstreamAudioConfig config{};
UpstreamAudioStats stats{};
UpstreamAudioPcm sink;
void *sink_user;
std::string error;
enum class Mode { Rom, Ax, Unsupported };
Mode mode = Mode::Rom;
struct Mail { uint32_t value; bool irq; };
std::deque<Mail> replies;
uint32_t boot_key, boot_addr, boot_bytes, boot_vector;
uint32_t init_vector;
bool have_vector, want_list, boot_value;
uint16_t list_words;
std::array<uint32_t, 10> upload;
unsigned upload_count;
bool uploading;

uint32_t physical(uint32_t a) {
    // MEM1 physical or its cached/uncached aliases only. Never wrap MMIO into RAM.
    if (a >= 0x80000000u && a < 0x81800000u) return a - 0x80000000u;
    if (a >= 0xc0000000u && a < 0xc1800000u) return a - 0xc0000000u;
    return a;
}
uint8_t *memory(uint32_t a, size_t n) {
    a = physical(a);
    if (!config.ram || a > config.ram_size || n > config.ram_size - a)
        audio_fail("audio access outside borrowed MEM1");
    return config.ram + a;
}
void queue(uint32_t value, bool irq) {
    if (replies.size() >= 64) audio_fail("DSP reply queue overflow");
    bool head = replies.empty();
    replies.push_back({value, irq});
    if (head && irq && config.raise_dsp_irq) config.raise_dsp_irq(config.user);
}
int result() { return stats.limitations ? UPSTREAM_AUDIO_PARTIAL : UPSTREAM_AUDIO_OK; }
int failure(const std::exception& e) { error = e.what(); return UPSTREAM_AUDIO_ERROR; }
uint32_t fingerprint(uint32_t a, uint32_t bytes) {
    if (!bytes || bytes > 0x2000) audio_fail("invalid DSP IRAM byte count");
    const uint8_t *p = memory(a, bytes);
    uint32_t h = 0;
    for (uint32_t i = 0; i < bytes; ++i) { h ^= p[i]; h = (h << 3) | (h >> 29); }
    return h;
}
int boot(uint32_t a, uint32_t bytes, uint32_t vector) {
    const uint32_t h = fingerprint(a, bytes);
    stats.ucode_hash = h;
    want_list = false;
    if (h != 0x07f88145u || bytes != 0x1f20) {
        mode = Mode::Unsupported;
        error = "unsupported DSP ucode (only measured Digimon AX 07f88145/1f20 is enabled)";
        std::fprintf(stderr, "[upstream-audio] unsupported ucode hash=%08x bytes=%x vector=%x\n", h, bytes, vector);
        return UPSTREAM_AUDIO_UNSUPPORTED_UCODE;
    }
    bool resume = have_vector && vector != init_vector;
    if (!have_vector) { have_vector = true; init_vector = vector; }
    mode = Mode::Ax;
    if (!resume) ax_init();
    std::fprintf(stderr, "[upstream-audio] AX hash=%08x bytes=%x vector=%x %s; diagnostic mixer\n", h, bytes, vector, resume ? "resume" : "boot");
    queue(resume ? 0xdcd10001u : 0xdcd10000u, true);
    return result();
}
int process(uint32_t a, uint16_t words) {
    if (mode != Mode::Ax) return UPSTREAM_AUDIO_UNSUPPORTED_UCODE;
    if (!words || words > 0x4000) audio_fail("invalid AX command list word count");
    memory(a, uint32_t(words) * 2);
    ax_process_cmdlist(a, words);
    ++stats.command_lists;
    return result();
}
void rom(uint32_t mail) {
    if (!boot_value) {
        if ((mail & 0xffff0000u) != 0x80f30000u) audio_fail("ROM expects 80F3 boot key");
        boot_key = mail & 0xffff;
        boot_value = true;
        return;
    }
    boot_value = false;
    switch (boot_key) {
    case 0xa001: boot_addr = mail; break;
    case 0xc002: if (mail) audio_fail("nonzero DSP IRAM destination unsupported"); break;
    case 0xa002: boot_bytes = mail; break;
    case 0xb002: if (mail) audio_fail("DSP ROM DRAM upload unsupported"); break;
    case 0xd001: boot_vector = mail; boot(boot_addr, boot_bytes, boot_vector); break;
    default: audio_fail("unknown DSP ROM key");
    }
}
}

[[noreturn]] void audio_fail(const char *why) { throw std::runtime_error(why); }
void audio_limit(uint32_t bit) {
    if (!(stats.limitations & bit))
        std::fprintf(stderr, "[upstream-audio] unimplemented DSP behavior flag=%08x\n", bit);
    stats.limitations |= bit;
}
void audio_mixed_block() { ++stats.mixed_blocks; }
uint16_t audio_mem_r16(uint32_t a) {
    const uint8_t *p = memory(a, 2);
    return uint16_t(uint16_t(p[0]) << 8 | p[1]);
}
uint32_t audio_mem_r32(uint32_t a) {
    const uint8_t *p = memory(a, 4);
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void audio_mem_w16(uint32_t a, uint16_t v) {
    uint8_t *p = memory(a, 2); p[0] = uint8_t(v >> 8); p[1] = uint8_t(v);
}
void audio_mem_w32(uint32_t a, uint32_t v) {
    uint8_t *p = memory(a, 4);
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
uint8_t audio_aram_byte(uint32_t a) {
    if (!config.aram_read) audio_fail("no borrowed ARAM callback");
    return config.aram_read(config.user, a);
}
extern "C" int upstream_audio_bind(const UpstreamAudioConfig *c) {
    if (!c || !c->ram || c->ram_size != 0x01800000 || !c->aram_read) {
        error = "bind requires borrowed 24 MiB MEM1 and ARAM read callback";
        return UPSTREAM_AUDIO_ERROR;
    }
    config = *c;
    stats = {};
    // Dpop is omitted by this upstream implementation even if all active voice
    // formats happen to be exact. Keep the overall capability explicitly partial.
    stats.limitations = UPSTREAM_AUDIO_LIMIT_DPOP;
    have_vector = false;
    error.clear();
    upstream_audio_reset();
    return result();
}
extern "C" void upstream_audio_unbind() {
    replies.clear(); config = {}; mode = Mode::Unsupported;
    sink = nullptr; sink_user = nullptr; have_vector = false;
}
extern "C" void upstream_audio_reset() {
    replies.clear(); mode = Mode::Rom;
    boot_key = boot_addr = boot_bytes = boot_vector = 0;
    boot_value = want_list = uploading = false; upload_count = 0;
    ax_init();
    replies.push_back({0x8071feedu, false});
}
extern "C" void upstream_audio_ipl_ready() {
    replies.clear(); replies.push_back({0x80544348u, false});
}
extern "C" int upstream_audio_boot(uint32_t a, uint32_t bytes, uint32_t vector) {
    try { return boot(a, bytes, vector); }
    catch (const std::exception& e) { return failure(e); }
}
extern "C" int upstream_audio_process(uint32_t a, uint16_t words) {
    try { return process(a, words); }
    catch (const std::exception& e) { return failure(e); }
}
extern "C" int upstream_audio_mail(uint32_t mail) {
    try {
        if (uploading) {
            upload[upload_count++] = mail;
            if (upload_count == upload.size()) {
                uploading = false;
                if (upload[1] || upload[8]) audio_fail("DSP task DRAM transfer unsupported");
                return boot(upload[3], upload[4] & 0xffff, upload[6] & 0xffff);
            }
        } else if (mode == Mode::Rom) {
            rom(mail);
        } else if (mode == Mode::Unsupported) {
            return UPSTREAM_AUDIO_UNSUPPORTED_UCODE;
        } else if (want_list) {
            want_list = false;
            int rc = process(mail, list_words);
            if (rc >= 0) queue(0xdcd10002u, true); // AX yield only after actual work
            return rc;
        } else if ((mail & 0xffff0000u) == 0xbabe0000u) {
            list_words = uint16_t(mail); want_list = true;
        } else {
            switch (mail) {
            case 0xcdd10000: queue(0xdcd10001u, true); break;
            case 0xcdd10001: uploading = true; upload_count = 0; break;
            case 0xcdd10002: upstream_audio_reset(); break;
            case 0xcdd10003: break;
            default: audio_fail("unexpected AX mailbox word");
            }
        }
        return mode == Mode::Unsupported ? UPSTREAM_AUDIO_UNSUPPORTED_UCODE : result();
    } catch (const std::exception& e) { return failure(e); }
}
extern "C" uint16_t upstream_audio_mail_hi() {
    return replies.empty() ? 0 : uint16_t(replies.front().value >> 16) | 0x8000u;
}
extern "C" uint16_t upstream_audio_mail_lo() {
    if (replies.empty()) return 0;
    uint16_t lo = uint16_t(replies.front().value);
    replies.pop_front();
    if (!replies.empty() && replies.front().irq && config.raise_dsp_irq)
        config.raise_dsp_irq(config.user);
    return lo;
}
extern "C" void upstream_audio_set_pcm_sink(UpstreamAudioPcm cb, void *user) {
    sink = cb; sink_user = user;
}
extern "C" int upstream_audio_dma(uint32_t a, uint32_t bytes, uint32_t rate) {
    try {
        if ((bytes & 3) || (rate != 32000 && rate != 48000)) audio_fail("invalid AI DMA shape/rate");
        const uint8_t *p = memory(a, bytes);
        // Bounded stack chunks; no alignment assumptions or whole-RAM copies.
        std::array<int16_t, 512> lr;
        for (uint32_t at = 0; at < bytes / 4;) {
            uint32_t frames = std::min<uint32_t>(256, bytes / 4 - at);
            for (uint32_t i = 0; i < frames; ++i) {
                const uint8_t *s = p + (at + i) * 4;
                lr[i * 2] = int16_t(uint16_t(s[2]) << 8 | s[3]);
                lr[i * 2 + 1] = int16_t(uint16_t(s[0]) << 8 | s[1]);
                if (lr[i * 2] || lr[i * 2 + 1]) ++stats.nonzero_dma_frames;
            }
            stats.dma_frames += frames;
            if (sink) sink(sink_user, lr.data(), frames, rate);
            at += frames;
        }
        return result();
    } catch (const std::exception& e) { return failure(e); }
}
extern "C" UpstreamAudioStats upstream_audio_stats() { return stats; }
extern "C" const char *upstream_audio_last_error() { return error.c_str(); }
