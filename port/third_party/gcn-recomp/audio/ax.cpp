// High-level emulation of the AX audio ucode (the "old" 2001 GameCube variant):
// processes the command lists the AX library sends, mixes voices from ARAM and
// writes the final 5ms stereo block that the AI DMA plays.
// Pinned MIT source: ethannicholas/gcn-recomp 3fad2f2deea7e563755819cf6f44da8743c61c55.
// Local changes: borrowed-memory shim, newer AX channel bits, strict parser bounds,
// deterministic reset and explicit diagnostics for omitted DSP behavior. See PIN.json.
#include "../../../platform/desktop/upstream_audio/upstream_runtime.h"
#include "ax.h"
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string>

namespace {

constexpr int SPMS = 32;          // samples per millisecond (32 kHz)
constexpr int SPF = 5 * SPMS;     // samples per 5ms frame

int32_t g_left[SPF], g_right[SPF], g_surround[SPF];
int32_t g_auxa_l[SPF], g_auxa_r[SPF], g_auxa_s[SPF];
int32_t g_auxb_l[SPF], g_auxb_r[SPF], g_auxb_s[SPF];
uint16_t g_compressor_pos;

// Measured newer-AX command 0x12: the guest owns 11 attack and 10 release
// rows of 160 unsigned Q15 gains. Protocol/state reference: Dolphin AX
// RunCompressor (GPL reference only); this implementation is independently
// authored. No game table bytes or reference implementation are embedded.
void compress_main(uint16_t threshold, uint16_t release, uint32_t table) {
    if (release > 10) {
        audio_limit(UPSTREAM_AUDIO_LIMIT_COMPRESSOR);
        return;
    }
    bool attack = false;
    for (int i = 0; i < SPF; ++i) {
        const int64_t l = g_left[i], r = g_right[i];
        if (l > threshold || l < -int64_t(threshold) ||
            r > threshold || r < -int64_t(threshold)) {
            attack = true;
            break;
        }
    }
    if (!attack && !g_compressor_pos) return;
    const uint16_t next = attack ? release : uint16_t(g_compressor_pos - 1);
    const unsigned row = attack ? g_compressor_pos : 11u + next;
    const uint64_t address = uint64_t(table) + row * SPF * 2u;
    if (address + SPF * 2u > uint64_t(UINT32_MAX) + 1u)
        audio_fail("AX compressor table address overflow");
    uint16_t gains[SPF];
    for (int i = 0; i < SPF; ++i) {
        gains[i] = mem_r16(uint32_t(address) + 2u * i);
        // The measured table only attenuates. Keep other table domains
        // explicitly partial rather than guessing accumulator overflow rules.
        if (gains[i] > 0x8000) {
            audio_limit(UPSTREAM_AUDIO_LIMIT_COMPRESSOR);
            return;
        }
    }
    for (int i = 0; i < SPF; ++i) {
        g_left[i] = int32_t((int64_t(g_left[i]) * gains[i]) >> 15);
        g_right[i] = int32_t((int64_t(g_right[i]) * gains[i]) >> 15);
    }
    g_compressor_pos = next;
}

// ---- parameter block (word indices into the big-endian u16 array) ----
enum {
    PB_NEXT_HI = 0, PB_NEXT_LO = 1, PB_SRC_TYPE = 4, PB_COEF_SELECT = 5, PB_MIXER_CONTROL = 6,
    PB_RUNNING = 7, PB_IS_STREAM = 8,
    PB_MIXER = 9,           // 18 words: (vol, delta) pairs for L, R, AuxA L/R, AuxB L/R, AuxB S, S, AuxA S
    PB_UPDATES = 34,        // num_updates[5], data_hi, data_lo
    PB_VOL = 50, PB_VOL_DELTA = 51,
    PB_LOOPING = 55, PB_FORMAT = 56, PB_LOOP_HI = 57, PB_LOOP_LO = 58, PB_END_HI = 59, PB_END_LO = 60,
    PB_CUR_HI = 61, PB_CUR_LO = 62,
    PB_COEFS = 63, PB_GAIN = 79, PB_PRED_SCALE = 80, PB_YN1 = 81, PB_YN2 = 82,
    PB_RATIO_HI = 83, PB_RATIO_LO = 84, PB_FRAC = 85, PB_LAST = 86,
    PB_LOOP_PRED_SCALE = 90, PB_LOOP_YN1 = 91, PB_LOOP_YN2 = 92,
    PB_WORDS = 93,
};

struct PB {
    uint16_t w[PB_WORDS];
    uint32_t get32(int i) const { return ((uint32_t)w[i] << 16) | w[i + 1]; }
    void set32(int i, uint32_t v) { w[i] = (uint16_t)(v >> 16); w[i + 1] = (uint16_t)v; }
};

void read_pb(uint32_t addr, PB& pb) {
    for (int i = 0; i < PB_WORDS; i++) pb.w[i] = mem_r16(addr + 2 * i);
}
void write_pb(uint32_t addr, const PB& pb) {
    for (int i = 0; i < PB_WORDS; i++) mem_w16(addr + 2 * i, pb.w[i]);
}

inline uint8_t aram_byte(uint32_t a) { return audio_aram_byte(a & 0x00ffffffu); }

// The DSP "accelerator": fetches the next sample for a voice, handling ADPCM
// decoding, the end of the sample data and looping.
struct Accel {
    PB& pb;
    uint32_t cur, end, loop;
    uint16_t fmt;
    bool stopped = false;

    explicit Accel(PB& p) : pb(p), cur(p.get32(PB_CUR_HI)), end(p.get32(PB_END_HI)), loop(p.get32(PB_LOOP_HI)), fmt(p.w[PB_FORMAT]) {}

    int16_t sample() {
        if (stopped) return 0;
        int32_t val = 0;
        switch (fmt) {
        case 0x00: {  // 4-bit DSP ADPCM, addresses in nibbles
            if ((cur & 15) == 0) {
                pb.w[PB_PRED_SCALE] = aram_byte(cur >> 1);
                cur += 2;
            }
            uint16_t ps = pb.w[PB_PRED_SCALE];
            int scale = 1 << (ps & 0xF);
            int ci = (ps >> 4) & 7;
            int32_t c1 = (int16_t)pb.w[PB_COEFS + ci * 2], c2 = (int16_t)pb.w[PB_COEFS + ci * 2 + 1];
            uint8_t b = aram_byte(cur >> 1);
            int t = (cur & 1) ? (b & 0xF) : (b >> 4);
            if (t >= 8) t -= 16;
            int32_t yn1 = (int16_t)pb.w[PB_YN1], yn2 = (int16_t)pb.w[PB_YN2];
            val = scale * t + ((0x400 + c1 * yn1 + c2 * yn2) >> 11);
            val = std::clamp(val, -0x7FFF, 0x7FFF);
            pb.w[PB_YN2] = pb.w[PB_YN1];
            pb.w[PB_YN1] = (uint16_t)val;
            cur++;
            break;
        }
        case 0x0A:  // PCM16, addresses in samples
            val = (int16_t)((aram_byte(cur * 2) << 8) | aram_byte(cur * 2 + 1));
            pb.w[PB_YN2] = pb.w[PB_YN1];
            pb.w[PB_YN1] = (uint16_t)val;
            cur++;
            break;
        case 0x19:  // PCM8
            val = (int16_t)(aram_byte(cur) << 8);
            pb.w[PB_YN2] = pb.w[PB_YN1];
            pb.w[PB_YN1] = (uint16_t)val;
            cur++;
            break;
        default:
            stopped = true;
            return 0;
        }
        if (cur > end) {
            if (pb.w[PB_LOOPING]) {
                cur = loop;
                pb.w[PB_PRED_SCALE] = pb.w[PB_LOOP_PRED_SCALE];
                if (!pb.w[PB_IS_STREAM]) {
                    pb.w[PB_YN1] = pb.w[PB_LOOP_YN1];
                    pb.w[PB_YN2] = pb.w[PB_LOOP_YN2];
                }
            } else {
                pb.w[PB_RUNNING] = 0;
                stopped = true;
            }
        }
        return (int16_t)val;
    }

    void finish() { pb.set32(PB_CUR_HI, cur); }
};

void mix_add(int32_t* out, const int16_t* in, int n, PB& pb, int vol_idx, bool ramp,
             int dpop_idx) {
    uint16_t vol = pb.w[vol_idx];
    uint16_t delta = ramp ? pb.w[vol_idx + 1] : 0;
    for (int i = 0; i < n; i++) {
        int32_t s = (int32_t)(((int64_t)in[i] * vol) >> 15);
        const int32_t contribution = std::clamp(s, -32767, 32767);
        out[i] += contribution;
        // Guest 802a5264 collects this signed per-voice bus value on removal;
        // 802a4e3c generates the next setup ramp. Preserve disabled/stopped
        // fields by writing only when the destination actually mixes a sample.
        pb.w[dpop_idx] = uint16_t(contribution);
        vol += delta;
    }
    pb.w[vol_idx] = vol;
}

// Polyphase SRC (src_type 0): 4 taps over the last four input samples, 128 phases of
// the fractional position, one coefficient set per coef_select (the AX library picks
// a lower cutoff as the pitch ratio rises above 1). The DSP reads these from its
// coefficient ROM; the values here are generated, not copied: Lanczos-2 windowed sinc,
// interpolating between last[1] and last[2], each phase normalized to unity gain.
// Local change (2026-11-06): previously linear interpolation for every src_type.
struct PolyphaseTable {
    int16_t c[3][128][4];
    PolyphaseTable() {
        const double cutoff[3] = {0.97, 0.75, 0.55};
        for (int s = 0; s < 3; s++)
            for (int p = 0; p < 128; p++) {
                double h[4], sum = 0;
                for (int k = 0; k < 4; k++) {
                    const double x = (k - 1) - p / 128.0;
                    auto sinc = [](double v) {
                        constexpr double pi = 3.14159265358979323846;
                        return v == 0 ? 1.0 : std::sin(pi * v) / (pi * v);
                    };
                    h[k] = std::fabs(x) < 2 ? cutoff[s] * sinc(cutoff[s] * x) * sinc(x / 2) : 0;
                    sum += h[k];
                }
                int total = 0;
                for (int k = 0; k < 4; k++) total += c[s][p][k] = int16_t(std::lround(h[k] / sum * 32768.0));
                c[s][p][1] = int16_t(c[s][p][1] + (32768 - total));   // exact unity DC gain
            }
    }
};
const PolyphaseTable g_polyphase;

extern int g_stat_voices;
void process_voice(PB& pb, int offset) {
    if (!pb.w[PB_RUNNING]) return;
    if (offset == 0) g_stat_voices++;
    if (pb.w[27]) audio_limit(UPSTREAM_AUDIO_LIMIT_ITD);
    int16_t samples[SPMS];
    Accel acc(pb);
    // Sample rate conversion: polyphase (0), linear (1) or none (2).
    uint32_t ratio = pb.get32(PB_RATIO_HI);
    uint64_t pos = pb.w[PB_FRAC];
    int16_t last[4];
    for (int k = 0; k < 4; ++k) last[k] = int16_t(pb.w[PB_LAST + k]);
    const uint16_t src_type = pb.w[PB_SRC_TYPE];
    const int16_t (*poly)[4] = g_polyphase.c[std::min<uint16_t>(pb.w[PB_COEF_SELECT], 2)];
    if (src_type == 0) audio_limit(UPSTREAM_AUDIO_LIMIT_POLYPHASE);
    auto consume = [&]() {
        last[0] = last[1]; last[1] = last[2]; last[2] = last[3];
        last[3] = acc.sample();
    };
    for (int i = 0; i < SPMS; i++) {
        if (src_type == 2) {
            // No SRC bypasses the ratio and phase; it reads one fresh input.
            consume();
            samples[i] = last[3];
            continue;
        }
        // AX advances the input position before selecting the output phase.
        // A wide accumulator also retains carry for a large 16.16 ratio.
        pos += ratio;
        while (pos >= 0x10000) { consume(); pos -= 0x10000; }
        const int32_t frac = int32_t(pos);
        if (src_type == 0) {
            const int16_t* c = poly[frac >> 9];
            const int64_t v = (int64_t(c[0]) * last[0] + int64_t(c[1]) * last[1] +
                               int64_t(c[2]) * last[2] + int64_t(c[3]) * last[3] + 0x4000) >> 15;
            samples[i] = int16_t(std::clamp<int64_t>(v, -32768, 32767));
        } else {
            // The signed difference can span 65535, so its Q16 product needs
            // more than 32 bits even though the interpolated sample fits S16.
            samples[i] = int16_t(last[2] + ((int64_t(last[3] - last[2]) * frac) >> 16));
        }
    }
    for (int k = 0; k < 4; ++k) pb.w[PB_LAST + k] = uint16_t(last[k]);
    pb.w[PB_FRAC] = (uint16_t)pos;
    acc.finish();
    // Volume envelope
    for (int i = 0; i < SPMS; i++) {
        uint16_t v = pb.w[PB_VOL];
        samples[i] = (int16_t)std::clamp(((int32_t)samples[i] * v) >> 15, -32767, 32767);
        pb.w[PB_VOL] = (uint16_t)(v + (int16_t)pb.w[PB_VOL_DELTA]);
    }
    // 07f88145 uses individual channel enables and four ramp groups. These are
    // protocol field assignments; the pinned mixing arithmetic is reused unchanged.
    const uint16_t mc = pb.w[PB_MIXER_CONTROL];
    if (mc & 0x4000) audio_limit(UPSTREAM_AUDIO_LIMIT_DPL2);
    struct Route { int32_t *bus; int slot; uint16_t enable, ramp; int dpop; };
    const Route routes[] = {
        {g_left, 0, 0x0001, 0x0008, 41}, {g_right, 2, 0x0002, 0x0008, 44},
        {g_surround, 14, 0x0004, 0x0008, 47},
        {g_auxa_l, 4, 0x0010, 0x0040, 42}, {g_auxa_r, 6, 0x0020, 0x0040, 45},
        {g_auxa_s, 16, 0x0080, 0x0100, 48},
        {g_auxb_l, 8, 0x0200, 0x0800, 43}, {g_auxb_r, 10, 0x0400, 0x0800, 46},
        {g_auxb_s, 12, 0x1000, 0x2000, 49},
    };
    for (const Route& route : routes)
        if (mc & route.enable)
            mix_add(route.bus + offset, samples, SPMS, pb, PB_MIXER + route.slot,
                    (mc & route.ramp) != 0, route.dpop);
}

void process_pb_list(uint32_t pb_addr) {
    PB pb;
    int guard = 0;
    while (pb_addr && guard++ < 256) {
        read_pb(pb_addr, pb);
        // Core is 93 words; preserve the game's 0xEC stride and untouched tail.
        if (mem_r16(pb_addr + 186)) audio_limit(UPSTREAM_AUDIO_LIMIT_LPF);
        // Per-millisecond parameter updates: (word index, value) pairs.
        uint32_t upd = pb.get32(PB_UPDATES + 5);
        for (int ms = 0; ms < 5; ms++) {
            uint16_t n = pb.w[PB_UPDATES + ms];
            for (int k = 0; k < n; k++) {
                uint16_t idx = mem_r16(upd), val = mem_r16(upd + 2);
                upd += 4;
                if (idx < PB_WORDS) pb.w[idx] = val;
            }
            process_voice(pb, ms * SPMS);
        }
        write_pb(pb_addr, pb);
        pb_addr = pb.get32(PB_NEXT_HI);
    }
    if (pb_addr) audio_fail("AX voice list exceeds 256 entries or is cyclic");
}

void write_bufs(uint32_t addr, std::initializer_list<int32_t*> bufs) {
    for (int32_t* b : bufs)
        for (int i = 0; i < SPF; i++, addr += 4) mem_w32(addr, (uint32_t)b[i]);
}
void add_bufs(uint32_t addr, std::initializer_list<int32_t*> bufs) {
    for (int32_t* b : bufs)
        for (int i = 0; i < SPF; i++, addr += 4) b[i] += (int32_t)mem_r32(addr);
}
void set_bufs(uint32_t addr, std::initializer_list<int32_t*> bufs) {
    for (int32_t* b : bufs)
        for (int i = 0; i < SPF; i++, addr += 4) b[i] = (int32_t)mem_r32(addr);
}

// Replace the main buffers with one downloaded mono buffer: L = R = in (0x07), or
// L = -in, R = in (0x11); surround cleared either way.
void set_main_from(uint32_t addr, bool opposite) {
    for (int i = 0; i < SPF; i++, addr += 4) {
        int32_t v = (int32_t)mem_r32(addr);
        g_left[i] = opposite ? -v : v;
        g_right[i] = v;
        g_surround[i] = 0;
    }
}

// 0x01: add nine downloaded buffers (main L/R/S, AuxA L/R/S, AuxB L/R/S, all read from
// the same address in turn) scaled by a volume per group.
void download_and_mix(uint32_t addr, uint16_t vol_main, uint16_t vol_auxa, uint16_t vol_auxb) {
    int32_t* groups[3][3] = {{g_left, g_right, g_surround}, {g_auxa_l, g_auxa_r, g_auxa_s}, {g_auxb_l, g_auxb_r, g_auxb_s}};
    uint16_t vols[3] = {vol_main, vol_auxa, vol_auxb};
    for (int g = 0; g < 3; g++) {
        uint32_t p = addr;
        for (int b = 0; b < 3; b++)
            for (int i = 0; i < SPF; i++, p += 4) groups[g][b][i] += (int32_t)(((int64_t)(int32_t)mem_r32(p) * vols[g]) >> 15);
    }
}

void setup(uint32_t addr) {
    int32_t* bufs[9] = {g_left, g_right, g_surround, g_auxa_l, g_auxa_r, g_auxa_s, g_auxb_l, g_auxb_r, g_auxb_s};
    for (int b = 0; b < 9; b++) {
        int32_t init = (int32_t)(((uint32_t)mem_r16(addr) << 16) | mem_r16(addr + 2));
        int16_t delta = (int16_t)mem_r16(addr + 4);
        addr += 6;
        for (int i = 0; i < SPF; i++) { bufs[b][i] = init; init += delta; }
    }
}

int g_stat_voices, g_stat_frames, g_stat_peak;

void output(uint32_t surround_addr, uint32_t lr_addr) {
    audio_mixed_block();
    for (int i = 0; i < SPF; i++) g_stat_peak = std::max(g_stat_peak, std::abs(g_left[i]));
    if (++g_stat_frames % 400 == 0 && getenv("GCN_AXSTATS")) {
        fprintf(stderr, "[ax] frames=%d voices(running, last frame)=%d peak=%d out=%08X\n", g_stat_frames, g_stat_voices, g_stat_peak, lr_addr);
        g_stat_peak = 0;
    }
    for (int i = 0; i < SPF; i++) mem_w32(surround_addr + 4 * i, (uint32_t)g_surround[i]);
    for (int i = 0; i < SPF; i++) {
        int32_t l = std::clamp(g_left[i], -32767, 32767), r = std::clamp(g_right[i], -32767, 32767);
        mem_w16(lr_addr + 4 * i, (uint16_t)r);       // interleaved R, L
        mem_w16(lr_addr + 4 * i + 2, (uint16_t)l);
    }
}

}  // namespace

void ax_init() {
    int32_t* buses[] = {g_left, g_right, g_surround, g_auxa_l, g_auxa_r,
                       g_auxa_s, g_auxb_l, g_auxb_r, g_auxb_s};
    for (auto* b : buses) std::fill(b, b + SPF, 0);
    g_stat_voices = g_stat_frames = g_stat_peak = 0;
    g_compressor_pos = 0;
}

void ax_process_cmdlist(uint32_t addr, uint16_t size) {
    uint32_t pb_addr = 0;
    uint32_t p = addr, end = addr + uint32_t(size) * 2;
    auto rd = [&]() {
        if (p >= end || end - p < 2) audio_fail("truncated AX command list");
        uint16_t v = mem_r16(p); p += 2; return v;
    };
    auto rd32 = [&]() { uint32_t hi = rd(); return (hi << 16) | rd(); };
    for (int guard = 0; guard < 256 && p < end; guard++) {
        uint16_t cmd = rd();
        switch (cmd) {
        case 0x00: setup(rd32()); break;
        case 0x01: { uint32_t a = rd32(); uint16_t vm = rd(), va = rd(), vb = rd(); download_and_mix(a, vm, va, vb); break; }
        case 0x02: pb_addr = rd32(); break;
        case 0x03: g_stat_voices = 0; process_pb_list(pb_addr); break;
        case 0x04: { uint32_t w = rd32(), r = rd32(); write_bufs(w, {g_auxa_l, g_auxa_r, g_auxa_s});
                     add_bufs(r, {g_left, g_right, g_surround}); break; }
        case 0x05: { uint32_t w = rd32(), r = rd32(); write_bufs(w, {g_auxb_l, g_auxb_r, g_auxb_s});
                     add_bufs(r, {g_left, g_right, g_surround}); break; }
        case 0x06: write_bufs(rd32(), {g_left, g_right, g_surround}); break;
        case 0x07: set_main_from(rd32(), false); break;
        case 0x08: audio_limit(UPSTREAM_AUDIO_LIMIT_UNKNOWN_COMMAND); for (int i = 0; i < 10; ++i) rd(); break;
        case 0x09: add_bufs(rd32(), {g_left, g_right, g_surround}); break;  // mix AuxB back in, nothing uploaded
        case 0x0A: case 0x0B: case 0x0C: break;
        case 0x0D: {  // continue in another command buffer: address, size
            uint32_t a = rd32();
            uint16_t sz = rd();
            p = a;
            end = a + uint32_t(sz) * 2;
            break;
        }
        case 0x0E: { uint32_t s = rd32(), lr = rd32(); output(s, lr); break; }
        case 0x0F: return;
        case 0x10: {  // upload AuxB L/R, then download the CPU's version of them: add to main, keep as AuxB
            uint32_t up = rd32(), dl = rd32();
            write_bufs(up, {g_auxb_l, g_auxb_r});
            set_bufs(dl, {g_auxb_l, g_auxb_r});
            for (int i = 0; i < SPF; i++) { g_left[i] += g_auxb_l[i]; g_right[i] += g_auxb_r[i]; }
            break;
        }
        case 0x11: set_main_from(rd32(), true); break;
        case 0x12: {
            const uint16_t threshold = rd(), release = rd();
            const uint32_t table = rd32();
            compress_main(threshold, release, table);
            break;
        }
        case 0x13: {  // upload AuxA L/R/S and AuxB S; download and add main L, main R, AuxB L, AuxB R
            uint32_t auxa_up = rd32(), auxb_s_up = rd32(), main_l = rd32(), main_r = rd32(), auxb_l = rd32(), auxb_r = rd32();
            write_bufs(auxa_up, {g_auxa_l, g_auxa_r, g_auxa_s});
            write_bufs(auxb_s_up, {g_auxb_s});
            add_bufs(main_l, {g_left});
            add_bufs(main_r, {g_right});
            add_bufs(auxb_l, {g_auxb_l});
            add_bufs(auxb_r, {g_auxb_r});
            break;
        }
        default: {
            // Say so once, with the list itself, so a command this parser does not know
            // (or whose length it has wrong) can be identified.
            static int reported;
            if (reported++ < 2) {
                std::string words;
                for (uint32_t q = addr; q < addr + 2 * size && q < addr + 128; q += 2) {
                    char b[8]; snprintf(b, sizeof b, "%04X ", mem_r16(q)); words += b;
                }
                LOG(LOG_DSP, "AX: unknown command %04X at +%X in list %08X (%u words): %s", cmd, (unsigned)(p - 2 - addr), addr, size, words.c_str());
            }
            audio_limit(UPSTREAM_AUDIO_LIMIT_UNKNOWN_COMMAND);
            audio_fail("unknown AX command");
        }
        }
    }
    audio_fail("AX command list has no terminator or exceeds command budget");
}
