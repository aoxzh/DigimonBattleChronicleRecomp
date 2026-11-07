/* Real IRQ + AI/DSP/EXI clocks; only the audio sink and unrelated VI are mocked.
 * cc -std=c11 -O2 -Wall -Wextra -Werror -DGC_UPSTREAM_AUDIO -Ihal \
 *   tests/test_irq_devices.c hal/irq.c hal/ai.c hal/dsp.c hal/exi.c \
 *   -o /tmp/test_irq_devices && /tmp/test_irq_devices
 */
#define _POSIX_C_SOURCE 200809L
#include "hal.h"
#include "irq.h"
#include "ai.h"
#include "dsp.h"
#include "exi.h"
#include "ar.h"
#include "mem.h"
#include "gc_abi.h"
#include "../platform/desktop/upstream_audio/upstream_audio.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef _WIN32
#define setenv(name, value, overwrite) ((void)(overwrite), _putenv_s((name), (value)))
#define unsetenv(name) _putenv_s((name), "")
#endif

#define CPU_HZ 486000000u
#define EE 0x8000u
GcCtx gc;
uint32_t gc_spr_misc[1024];
uint8_t gc_ram[GC_MEM1_SIZE];
static uint64_t dma_frames, dma_blocks, last_dma_cycle;
static uint32_t dma_rate;
int gc_runtime_manifest_enabled(void) { return 0; }
void gc_runtime_manifest_note_block(uint32_t pc) { (void)pc; }
void gc_runtime_manifest_note_event(const char *s) { (void)s; }
uint32_t hal_vi_cycles_to_event(void) { return UINT32_MAX; }
void hal_vi_advance(uint32_t cycles) { (void)cycles; }
void hal_si_frame_tick(void) {}
uint32_t hal_mem_read32(uint32_t p)
{
    assert(p <= GC_MEM1_SIZE - 4);
    return (uint32_t)gc_ram[p] << 24 | (uint32_t)gc_ram[p+1] << 16 |
           (uint32_t)gc_ram[p+2] << 8 | gc_ram[p+3];
}
void hal_mem_read_burst(uint32_t p, uint8_t *d, size_t n)
{ assert(p + n <= GC_MEM1_SIZE); memcpy(d, gc_ram + p, n); }
void hal_mem_write_burst(uint32_t p, const uint8_t *s, size_t n)
{ assert(p + n <= GC_MEM1_SIZE); memcpy(gc_ram + p, s, n); }
uint8_t hal_ar_read8(uint32_t p) { (void)p; return 0; }
void hal_ar_dma(uint32_t m, uint32_t a, uint32_t n, int d)
{ (void)m; (void)a; (void)n; (void)d; }
int upstream_audio_bind(const UpstreamAudioConfig *cfg) { (void)cfg; return 0; }
void upstream_audio_reset(void) {}
void upstream_audio_ipl_ready(void) {}
int upstream_audio_mail(uint32_t m) { (void)m; return 0; }
uint16_t upstream_audio_mail_hi(void) { return 0; }
uint16_t upstream_audio_mail_lo(void) { return 0; }
const char *upstream_audio_last_error(void) { return "test"; }
int upstream_audio_dma(uint32_t a, uint32_t n, uint32_t rate)
{
    assert(a == 0x1000 && n == 640 && rate == dma_rate);
    ++dma_blocks; dma_frames += n / 4;
    last_dma_cycle = hal_cpu_cycles();
    return 0;
}
static void put32(uint32_t p, uint32_t n)
{
    gc_ram[p] = (uint8_t)(n >> 24); gc_ram[p+1] = (uint8_t)(n >> 16);
    gc_ram[p+2] = (uint8_t)(n >> 8); gc_ram[p+3] = (uint8_t)n;
}
static void reset(void)
{
    memset(&gc, 0, sizeof(gc));
    hal_irq_init(); hal_ai_init(); hal_dsp_init();
    hal_exi_set_memcard(NULL); hal_exi_init();
    hal_decrementer_write(0x7fffffff);
    gc.pc = 0x802c7ae4; gc.r[13] = 0x803d6120; gc.msr = EE;
    put32(0x3cfd58, 0); put32(0x3010, 0x80005000);
    dma_frames = dma_blocks = last_dma_cycle = 0;
}
static void assert_external(uint32_t source)
{
    assert(hal_irq_pending() == source);
    uint64_t before = hal_cpu_cycles();
    hal_scheduler_idle(); /* pending event cannot be skipped */
    assert(hal_cpu_cycles() == before);
    assert(hal_irq_poll_cycles(0) == GC_VEC_EXTERNAL);
}
static void start_dma(unsigned rate)
{
    dma_rate = rate;
    hal_ai_write16(2, rate == 32000 ? 0x40 : 0);
    hal_dsp_write16(0x30, 0);
    hal_dsp_write16(0x32, 0x1000);
    hal_dsp_write16(0x36, 0x8014); /* 20 * 32 bytes = 160 stereo samples */
    hal_dsp_write16(0x0a, 0x18); /* acknowledge start and enable AID IRQ */
    hal_pi_write32(4, PI_INT_DSP);
}
static void test_ai_deadline(void)
{
    for (unsigned rate = 32000; rate <= 48000; rate += 16000) {
        reset();
        assert(hal_ai_cycles_to_event(CPU_HZ) == UINT32_MAX);
        hal_ai_write16(0x0e, 3);
        hal_ai_write16(2, 5 | (rate == 48000 ? 2 : 0));
        hal_pi_write32(4, PI_INT_AI);
        const uint32_t edge = (3ull * CPU_HZ + rate - 1) / rate;
        hal_clock_advance(37);
        assert(hal_ai_cycles_to_event(CPU_HZ) == edge - 37);
        assert(hal_ai_samples() == 0);
        hal_scheduler_idle();
        assert(hal_cpu_cycles() == edge && hal_ai_samples() == 3);
        assert(hal_ai_cycles_to_event(CPU_HZ) == UINT32_MAX);
        assert_external(PI_INT_AI);
    }
    reset();
    hal_ai_write16(8, 0xffff); hal_ai_write16(10, 0xfffe);
    hal_ai_write16(14, 1); hal_ai_write16(2, 3);
    assert(hal_ai_cycles_to_event(CPU_HZ) == 30375);
    hal_clock_advance(30374);
    assert(!(hal_ai_read16(2) & 8));
    assert(hal_ai_cycles_to_event(CPU_HZ) == 1);
    hal_clock_advance(1);
    assert(hal_ai_samples() == 1 && (hal_ai_read16(2) & 8));
    assert(!hal_irq_pending()); /* masked comparator still latches */
    hal_ai_write16(2, 11); /* acknowledge at equal count: wait a full wrap */
    assert(hal_ai_cycles_to_event(CPU_HZ) == UINT32_MAX);
    hal_ai_write16(8, 0); hal_ai_write16(10, 0);
    hal_ai_tick_cycles(1, 96000); /* half a sample */
    assert(hal_ai_cycles_to_event(192000) == 2);
    assert(hal_ai_cycles_to_event(0) == UINT32_MAX);
}
static void test_subsample_boundaries(void)
{
    /* Real translated blocks are commonly 3-15 cycles, far shorter than a
     * sample. Check every sample edge, including the half-cycle phase at 32k,
     * and coincident AI/DSP interrupts at the 160-sample DMA boundary. */
    for (unsigned rate = 32000; rate <= 48000; rate += 16000) {
        reset(); start_dma(rate);
        hal_ai_write16(14, 160);
        hal_ai_write16(2, 5 | (rate == 48000 ? 2 : 0x40));
        hal_pi_write32(4, PI_INT_AI | PI_INT_DSP);
        uint32_t elapsed = 0, step_index = 0;
        for (unsigned sample = 1; sample <= 160; ++sample) {
            const uint32_t edge = ((uint64_t)sample * CPU_HZ + rate - 1) / rate;
            while (elapsed < edge - 1) {
                uint32_t step = 3 + step_index++ % 13;
                if (step > edge - 1 - elapsed) step = edge - 1 - elapsed;
                hal_clock_advance(step);
                elapsed += step;
            }
            assert(hal_ai_samples() == sample - 1);
            assert(hal_dsp_read16(0x3a) == (640 - 4 * (sample - 1)) / 32);
            assert(dma_blocks == 0 && !hal_irq_pending());
            hal_clock_advance(0);
            assert(hal_ai_samples() == sample - 1 && !hal_irq_pending());
            hal_clock_advance(1); ++elapsed;
            assert(hal_ai_samples() == sample);
            if (sample < 160) {
                assert(hal_dsp_read16(0x3a) == (640 - 4 * sample) / 32);
                assert(dma_blocks == 0 && !hal_irq_pending());
            }
        }
        assert(dma_frames == 160 && dma_blocks == 1 && last_dma_cycle == elapsed);
        assert(hal_dsp_read16(0x3a) == 20); /* next DMA block already latched */
        assert_external(PI_INT_AI | PI_INT_DSP);
    }
}
static void test_dma_idle_and_partition(void)
{
    const uint32_t chunks[] = {37, 997, UINT32_MAX};
    for (unsigned rate = 32000; rate <= 48000; rate += 16000) {
        reset(); start_dma(rate);
        const uint32_t period = (uint64_t)160 * CPU_HZ / rate;
        assert(hal_dsp_cycles_to_event(CPU_HZ) == period);
        hal_clock_advance(137);
        assert(hal_dsp_cycles_to_event(CPU_HZ) == period - 137);
        hal_scheduler_idle();
        assert(hal_cpu_cycles() == period && last_dma_cycle == period);
        assert(dma_frames == 160 && dma_blocks == 1);
        assert_external(PI_INT_DSP);
        hal_dsp_write16(0x36, 0);
        assert(hal_dsp_cycles_to_event(CPU_HZ) == UINT32_MAX);
        for (unsigned i = 0; i < sizeof(chunks)/sizeof(chunks[0]); ++i) {
            reset(); start_dma(rate);
            uint32_t remaining = CPU_HZ / 5; /* > former 100 ms catch-up cap */
            while (remaining) {
                uint32_t n = remaining < chunks[i] ? remaining : chunks[i];
                hal_clock_advance(n); remaining -= n;
            }
            assert(dma_frames == rate / 5);
            assert(dma_blocks == rate / 5 / 160);
            assert(hal_cpu_cycles() == CPU_HZ / 5);
            assert(hal_dsp_cycles_to_event(CPU_HZ) == period);
        }
    }
}
static void exi_select(int on)
{ hal_exi_write32(0, 1u | (on ? 0x80u : 0)); }
static void exi_write(uint32_t word, unsigned n)
{
    hal_exi_write32(0x10, word);
    hal_exi_write32(0x0c, ((n - 1) << 4) | 5u);
}
static void erase_sector(void)
{
    exi_select(1); exi_write(0xf1005000, 3); exi_select(0);
}
static void test_exi_deadline(void)
{
    reset();
    assert(hal_exi_cycles_to_event() == UINT32_MAX);
    char directory[] = "/tmp/digimon-irq-devices-XXXXXX";
    assert(mkdtemp(directory));
    char path[128]; snprintf(path, sizeof(path), "%s/card.raw", directory);
    hal_exi_set_memcard(path); hal_exi_init();
    exi_select(1); exi_write(0x81010000, 2); exi_select(0);
    hal_pi_write32(4, PI_INT_EXI);
    erase_sector();
    assert(hal_exi_cycles_to_event() == 50000);
    hal_clock_advance(123);
    assert(hal_exi_cycles_to_event() == 49877);
    hal_scheduler_idle();
    assert(hal_cpu_cycles() == 50000);
    assert(hal_exi_cycles_to_event() == CPU_HZ);
    assert_external(PI_INT_EXI);
    hal_exi_write32(0, 3); /* W1C completion */
    erase_sector();
    hal_exi_tick_cycles(70000);
    assert(hal_exi_cycles_to_event() == CPU_HZ - 20000); /* busy is not idle */
    hal_exi_tick_cycles(CPU_HZ - 20001);
    assert(hal_exi_cycles_to_event() == 1);
    hal_exi_tick_cycles(1);
    assert(hal_exi_cycles_to_event() == UINT32_MAX);
    FILE *f = fopen(path, "rb"); assert(f);
    assert(fseek(f, 0, SEEK_END) == 0 && ftell(f) == 0x200000);
    assert(fclose(f) == 0);
    hal_exi_set_memcard(NULL); hal_exi_init();
    assert(remove(path) == 0 && rmdir(directory) == 0);
}
int main(void)
{
    unsetenv("GC_AUDIO_MODE");
    test_ai_deadline();
    test_subsample_boundaries();
    test_dma_idle_and_partition();
    test_exi_deadline();
    puts("IRQ device deadlines passed (AI wrap/phase, DSP DMA, EXI busy/flush)");
    return 0;
}
