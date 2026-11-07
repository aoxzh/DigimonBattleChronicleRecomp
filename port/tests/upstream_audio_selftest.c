/* Standalone C caller + actual optional HAL integration; requires user's DOL.
 * Build commands live in analysis/upstream-audio.md. No game bytes are embedded. */
#include "../platform/desktop/upstream_audio/upstream_audio.h"
#include "../hal/gc_abi.h"
#include "../hal/hal.h"
#include "../hal/ai.h"
#include "../hal/dsp.h"
#include "../hal/mem.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t gc_ram[GC_MEM1_SIZE];
GcCtx gc;
static uint8_t aram[0x10000];
static unsigned irqs, pcm_frames;
static int16_t captured[320];
static uint32_t pi;
void hal_pi_set_interrupt(uint32_t bits) { pi |= bits; }
void hal_pi_clear_interrupt(uint32_t bits) { pi &= ~bits; }
uint64_t hal_frame_count(void) { return 0; }
static uint8_t read_aram(void *u, uint32_t a) { (void)u; return aram[a & 0xffff]; }
uint8_t hal_ar_read8(uint32_t a) { return read_aram(NULL, a); }
void hal_ar_dma(uint32_t m, uint32_t a, uint32_t n, int d)
{ (void)m; (void)a; (void)n; (void)d; }
static void irq(void *u) { (void)u; ++irqs; }
static void pcm(void *u, const int16_t *s, uint32_t n, uint32_t rate) {
    (void)u; assert(rate == 32000 || rate == 48000);
    if (n == 160) memcpy(captured, s, sizeof(captured));
    pcm_frames += n;
}
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }
static void w16(uint32_t a, uint16_t x) { gc_ram[a] = x >> 8; gc_ram[a+1] = x; }
static void w32(uint32_t a, uint32_t x) { w16(a, x>>16); w16(a+2, x); }
static uint16_t r16(uint32_t a) { return (uint16_t)gc_ram[a]<<8 | gc_ram[a+1]; }
static void pb16(unsigned word, uint16_t x) { w16(0x2000 + 2*word, x); }
static void pb32(unsigned word, uint32_t x) { w32(0x2000 + 2*word, x); }
static void load_ucode(const char *path) {
    FILE *f = fopen(path, "rb"); assert(f);
    uint8_t hdr[256]; assert(fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr));
    int found = 0, found_table = 0;
    for (unsigned i=0; i<18; ++i) {
        uint32_t a=be32(hdr+0x48+4*i), n=be32(hdr+0x90+4*i);
        if (a<=0x80360a80 && (uint64_t)a+n>=0x803629a0) {
            assert(fseek(f, be32(hdr+4*i)+0x80360a80-a, SEEK_SET)==0);
            assert(fread(gc_ram+0x360a80,1,0x1f20,f)==0x1f20); found=1;
        }
        if (a<=0x8035f040 && (uint64_t)a+n>=0x80360a80) {
            assert(fseek(f,be32(hdr+4*i)+0x8035f040-a,SEEK_SET)==0);
            assert(fread(gc_ram+0x35f040,1,6720,f)==6720);found_table=1;
        }
    }
    fclose(f); assert(found && found_table);
}
static void boot(void) {
    assert(upstream_audio_boot(0x80360a80,0x1f20,0x10)>=0);
    assert(upstream_audio_stats().ucode_hash==0x07f88145);
}
static void setup_voice(uint16_t format, uint16_t mixer) {
    memset(gc_ram+0x1000,0,0x4000);
    /* setup; PB address; process; output surround & R,L; end */
    const uint16_t cmds[]={0,0,0x1800,2,0,0x2000,3,0xe,0,0x3000,0,0x4000,0xf};
    for (unsigned i=0;i<sizeof(cmds)/2;++i) w16(0x1000+2*i,cmds[i]);
    pb16(4,2); pb16(6,mixer); pb16(7,1);
    pb16(9,0x4000); pb16(11,0x2000); pb16(50,0x8000);
    pb16(56,format); pb32(59,511); pb32(61,0); pb32(83,0x10000);
    for(unsigned i=0;i<512;++i) { aram[2*i]=0x20; aram[2*i+1]=0; }
}
/* Synthetic row/phase-distinct gains exercise indexing without game bytes. */
static uint16_t test_gain(unsigned row, unsigned sample) {
    return (uint16_t)(0x4000 + row * 0x100 + sample);
}
static void compressor_frame(int32_t left, int32_t right, uint16_t threshold,
                             uint16_t release, uint32_t table, int row) {
    memset(gc_ram+0x1800,0,54);
    w32(0x1800,(uint32_t)left); w32(0x1806,(uint32_t)right);
    w32(0x180c,1234);
    const uint16_t cmds[]={0,0,0x1800,0x12,threshold,release,
        (uint16_t)(table>>16),(uint16_t)table,0xe,0,0x3000,0,0x4000,0xf};
    for(unsigned i=0;i<14;++i)w16(0x1000+2*i,cmds[i]);
    assert(upstream_audio_process(0x1000,14)==UPSTREAM_AUDIO_PARTIAL);
    for(unsigned i=0;i<160;++i) {
        int64_t l=left,r=right;
        if(row>=0) { l=(l*test_gain((unsigned)row,i))>>15; r=(r*test_gain((unsigned)row,i))>>15; }
        if(l>32767)l=32767;if(l< -32767)l= -32767;
        if(r>32767)r=32767;if(r< -32767)r= -32767;
        assert((int16_t)r16(0x4000+4*i)==r);
        assert((int16_t)r16(0x4002+4*i)==l);
        assert(be32(gc_ram+0x3000+4*i)==1234); /* main stereo only */
    }
}
static void test_compressor(const UpstreamAudioConfig *c) {
    assert(upstream_audio_bind(c)==UPSTREAM_AUDIO_PARTIAL);boot();
    for(unsigned row=0;row<21;++row)
        for(unsigned i=0;i<160;++i)w16(0x6000+row*320+2*i,test_gain(row,i));
    compressor_frame(32767,-32767,0x8000,10,0x80006000,-1); /* below */
    compressor_frame(32768,-32768,0x8000,10,0x80006000,-1); /* equality */
    compressor_frame(40000,-20000,0x8000,10,0x80006000,0); /* attack from idle */
    compressor_frame(20000,-40000,0x8000,10,0x80006000,10); /* right retrigger */
    compressor_frame(20000,-20000,0x8000,10,0x80006000,20); /* decrement before release */
    compressor_frame(-40000,20000,0x8000,10,0x80006000,9); /* attack during release */
    for(int row=20;row>=11;--row)
        compressor_frame(20000,-20000,0x8000,10,0x80006000,row);
    compressor_frame(20000,-20000,0x8000,10,0x80006000,-1); /* idle after 50ms */
    compressor_frame(40000,-20000,0x8000,10,0x80006000,0);
    boot(); /* fresh boot resets envelope */
    compressor_frame(20000,-20000,0x8000,10,0x80006000,-1);
    assert(!(upstream_audio_stats().limitations&UPSTREAM_AUDIO_LIMIT_COMPRESSOR));
    /* Truncated command, invalid address and overflowing row cannot read/wrap. */
    assert(upstream_audio_process(0x1000,7)<0);
    w16(0x100c,0xcc00);w16(0x100e,0x5000);w32(0x1800,40000);
    assert(upstream_audio_process(0x1000,14)<0);
    w16(0x100c,0xffff);w16(0x100e,0xfff0);
    assert(upstream_audio_process(0x1000,14)<0);
    compressor_frame(40000,-20000,0x8000,10,0x80006000,0); /* failures did not commit state */
    /* Unsupported release/table domains remain explicitly partial. */
    compressor_frame(20000,-20000,0x8000,11,0x80006000,-1);
    assert(upstream_audio_stats().limitations&UPSTREAM_AUDIO_LIMIT_COMPRESSOR);
    assert(upstream_audio_bind(c)>=0);boot();w16(0x6000,0xffff);
    compressor_frame(40000,-20000,0x8000,10,0x80006000,-1);
    assert(upstream_audio_stats().limitations&UPSTREAM_AUDIO_LIMIT_COMPRESSOR);
    assert(upstream_audio_bind(c)>=0);boot();
}
static int16_t src_input(unsigned i) { return (int16_t)((int)i*73-12000); }
static void test_src(const UpstreamAudioConfig *c) {
    assert(upstream_audio_bind(c)>=0);boot();
    /* This large signed transition overflowed the old int32 interpolation
     * product. UBSan catches it even where the final int16 wrap hides it. */
    setup_voice(0xa,1);pb16(4,1);pb16(9,0x8000);pb32(83,0);pb16(85,0xc000);
    pb16(88,(uint16_t)-30000);pb16(89,30000);
    assert(upstream_audio_process(0x1000,13)>=0);
    for(unsigned i=0;i<160;++i)assert((int16_t)r16(0x4002+4*i)==15000);
    /* No SRC consumes exactly one sample per output regardless of ratio or
     * saved fraction. It writes the newest four inputs back chronologically. */
    const uint32_t ratios[]={0,0x8000,0x10000,0x28000};
    for(unsigned k=0;k<sizeof(ratios)/sizeof(ratios[0]);++k) {
        setup_voice(0xa,1);pb16(9,0x8000);pb32(83,ratios[k]);pb16(85,0x5678);
        for(unsigned i=0;i<512;++i) {
            uint16_t sample=(uint16_t)src_input(i);
            aram[2*i]=sample>>8;aram[2*i+1]=(uint8_t)sample;
        }
        assert(upstream_audio_process(0x1000,13)>=0);
        for(unsigned i=0;i<160;++i)assert((int16_t)r16(0x4002+4*i)==src_input(i));
        assert(be32(gc_ram+0x2000+2*61)==160 && r16(0x2000+2*85)==0x5678);
        for(unsigned i=0;i<4;++i)assert((int16_t)r16(0x2000+2*(86+i))==src_input(156+i));
    }
    /* Linear half-rate: advance phase before output. A ramp exposes a one-
     * sample delay and verifies history continuity across all five ms slices. */
    setup_voice(0xa,1);pb16(4,1);pb16(9,0x8000);pb32(83,0x8000);
    for(unsigned i=0;i<4;++i)pb16(86+i,(uint16_t)((int)i*100-300));
    for(unsigned i=0;i<512;++i) { uint16_t v=(uint16_t)(i*100+100);aram[2*i]=v>>8;aram[2*i+1]=(uint8_t)v; }
    assert(upstream_audio_process(0x1000,13)>=0);
    for(unsigned i=0;i<160;++i)assert((int16_t)r16(0x4002+4*i)==(int)i*50-50);
    assert(be32(gc_ram+0x2000+2*61)==80 && !r16(0x2000+2*85));
    /* Phase zero of the authored symmetric FIR preserves a linear ramp at
     * its center tap. Check all coefficient banks and continuation next block. */
    for(unsigned bank=0;bank<3;++bank) {
        setup_voice(0xa,1);pb16(4,0);pb16(5,bank);pb16(9,0x8000);
        for(unsigned i=0;i<4;++i)pb16(86+i,(uint16_t)((int)i*40-120));
        for(unsigned i=0;i<512;++i) { uint16_t v=(uint16_t)(i*40+40);aram[2*i]=v>>8;aram[2*i+1]=(uint8_t)v; }
        for(unsigned block=0;block<2;++block) {
            assert(upstream_audio_process(0x1000,13)>=0);
            for(unsigned i=0;i<160;++i)assert((int16_t)r16(0x4002+4*i)==(int)(block*160+i)*40-40);
        }
    }
    /* Every phase/bank retains exact DC, including negative input. This is an
     * authored approximation, so its limitation must still be advertised. */
    for(unsigned bank=0;bank<3;++bank)for(unsigned phase=0;phase<128;++phase) {
        setup_voice(0xa,1);pb16(4,0);pb16(5,bank);pb16(9,0x8000);pb32(83,0);pb16(85,phase<<9);
        int16_t sample=phase&1 ? -8192 : 8192;
        for(unsigned i=0;i<4;++i)pb16(86+i,(uint16_t)sample);
        assert(upstream_audio_process(0x1000,13)>=0);
        for(unsigned i=0;i<160;++i)assert((int16_t)r16(0x4002+4*i)==sample);
    }
    assert(upstream_audio_stats().limitations&UPSTREAM_AUDIO_LIMIT_POLYPHASE);
    assert(upstream_audio_bind(c)>=0);boot();
}
static const unsigned depop_words[9]={41,44,47,42,45,48,43,46,49};
static const unsigned depop_slots[9]={0,2,14,4,6,16,8,10,12};
static const uint16_t depop_enables[9]={1,2,4,0x10,0x20,0x80,0x200,0x400,0x1000};
static void depop_voice(uint16_t mixer, int16_t sample) {
    setup_voice(0xa,mixer);
    for(unsigned b=0;b<9;++b)pb16(9+depop_slots[b],(uint16_t)(0x1000+0x100*depop_slots[b]));
    for(unsigned i=0;i<4;++i)pb16(86+i,(uint16_t)sample);
    for(unsigned i=0;i<512;++i) { aram[2*i]=(uint16_t)sample>>8;aram[2*i+1]=(uint8_t)sample; }
}
static int16_t depop_value(unsigned bus) { return (int16_t)r16(0x2000+2*depop_words[bus]); }
static void test_depop(const UpstreamAudioConfig *c) {
    assert(upstream_audio_bind(c)>=0);boot();
    /* Distinct signed values and one enabled route at a time detect permutations. */
    for(unsigned b=0;b<9;++b) {
        depop_voice(depop_enables[b],b&1 ? -8192 : 8192);
        for(unsigned k=0;k<9;++k)pb16(depop_words[k],(uint16_t)(-100-(int)k));
        assert(upstream_audio_process(0x1000,13)>=0);
        for(unsigned k=0;k<9;++k) {
            int expected=k==b ? (b&1 ? -1 : 1)*(int)(0x1000+0x100*depop_slots[b])/4 : -100-(int)k;
            assert(depop_value(k)==expected);
        }
    }
    /* Last contribution uses sample159's pre-increment volume, not final volume. */
    depop_voice(9,8192);pb16(10,4);
    assert(upstream_audio_process(0x1000,13)>=0);
    assert(depop_value(0)==(0x1000+159*4)/4);
    assert(r16(0x2000+18)==0x1000+160*4);
    /* At ms1 disable, ms2 right-only, ms3 stop: preserve each last mixed segment. */
    depop_voice(0xf,8192);pb16(10,4);pb16(12,4);pb16(24,4);
    pb16(35,1);pb16(36,1);pb16(37,1);pb32(39,0x5000);
    w16(0x5000,6);w16(0x5002,0);
    w16(0x5004,6);w16(0x5006,2|8);
    w16(0x5008,7);w16(0x500a,0);
    assert(upstream_audio_process(0x1000,13)>=0);
    assert(depop_value(0)==(0x1000+31*4)/4);
    assert(depop_value(1)==(0x1200+63*4)/4);
    assert(depop_value(2)==(0x1e00+31*4)/4);
    int16_t stopped[9];for(unsigned b=0;b<9;++b)stopped[b]=depop_value(b);
    for(unsigned ms=0;ms<5;++ms)pb16(34+ms,0);
    assert(upstream_audio_process(0x1000,13)>=0);
    for(unsigned b=0;b<9;++b)assert(depop_value(b)==stopped[b]);
    /* Reuse PB: enabled destination replaces prior history, disabled keeps it. */
    pb16(7,1);pb16(6,1);pb16(9,0x4000);pb16(10,0);
    assert(upstream_audio_process(0x1000,13)>=0);
    assert(depop_value(0)==4096 && depop_value(1)==stopped[1]);
    /* A stopped ms4 PB update can explicitly replace history; do not clear it. */
    pb16(7,0);pb16(38,1);pb32(39,0x5000);
    w16(0x5000,41);w16(0x5002,(uint16_t)-12345);
    assert(upstream_audio_process(0x1000,13)>=0);assert(depop_value(0)==-12345);
    /* Match the actual local bus saturation, including its known -32767 floor. */
    depop_voice(1,-32768);pb16(9,0xffff);
    assert(upstream_audio_process(0x1000,13)>=0);assert(depop_value(0)==-32767);
    depop_voice(1,32767);pb16(9,0xffff);
    assert(upstream_audio_process(0x1000,13)>=0);assert(depop_value(0)==32767);
    /* Local nonlooping end drains history to silence in first ms, then preserves it. */
    depop_voice(1,8192);pb32(59,7);
    assert(upstream_audio_process(0x1000,13)>=0);
    assert(r16(0x200e)==0 && depop_value(0)==0);
    assert(upstream_audio_process(0x1000,13)>=0);assert(depop_value(0)==0);
    depop_voice(1,8192);pb32(59,7);pb16(55,1);pb32(57,0);
    assert(upstream_audio_process(0x1000,13)>=0);
    assert(r16(0x200e)==1 && depop_value(0)==1024);
    /* Guest-derived collector/ramp roundtrip: signed nine-bus sums, stop, setup.
     * Models PPC 802a5264 + 802a4e3c; does not execute guest CPU or prove hardware. */
    int32_t sums[9]={0};
    for(unsigned voice=0;voice<2;++voice) {
        depop_voice(0x16b7,voice ? -24000 : 24000);
        for(unsigned b=0;b<9;++b) {
            unsigned vol=0x1000+0x100*depop_slots[b];
            if(voice)vol=b&1 ? vol*2 : vol/2;
            pb16(9+depop_slots[b],(uint16_t)vol);
        }
        assert(upstream_audio_process(0x1000,13)>=0);
        for(unsigned b=0;b<9;++b)sums[b]+=depop_value(b);
    }
    for(unsigned b=0;b<9;++b)assert(b&1 ? sums[b]<0 : sums[b]>0);
    /* Separate aux uploads make all nine setup buses observable. */
    const uint16_t cmds[]={0,0,0x1800,6,0,0x7000,4,0,0x8000,0,0xa000,5,0,0x9000,0,0xa000,0xf};
    for(unsigned i=0;i<17;++i)w16(0x1000+2*i,cmds[i]);
    memset(gc_ram+0xa000,0,1920);
    for(unsigned frame=0;frame<12;++frame) {
        int32_t initial[9];int delta[9];
        for(unsigned b=0;b<9;++b) {
            int q=sums[b]/160;if(q>20)q=20;if(q< -20)q= -20;
            initial[b]=q ? sums[b] : 0;delta[b]=-q;
            sums[b]=q ? sums[b]-160*q : 0;
            w32(0x1800+6*b,(uint32_t)initial[b]);w16(0x1804+6*b,(uint16_t)delta[b]);
        }
        assert(upstream_audio_process(0x1000,17)>=0);
        for(unsigned b=0;b<9;++b)for(unsigned i=0;i<160;++i) {
            uint32_t base=b<3 ? 0x7000 : b<6 ? 0x8000 : 0x9000;
            unsigned slot=b%3;
            assert((int32_t)be32(gc_ram+base+640*slot+4*i)==initial[b]+(int32_t)i*delta[b]);
        }
    }
    for(unsigned b=0;b<9;++b)assert(sums[b]==0);
    assert(upstream_audio_stats().limitations&UPSTREAM_AUDIO_LIMIT_DPOP);
    assert(upstream_audio_bind(c)>=0);boot();
}
static void send(uint32_t m) { hal_dsp_write16(0,m>>16); hal_dsp_write16(2,m); }
int main(int argc,char **argv) {
    assert(argc>=2); load_ucode(argv[1]);
    UpstreamAudioConfig c={gc_ram,sizeof(gc_ram),read_aram,irq,NULL};
    assert(upstream_audio_bind(&c)==UPSTREAM_AUDIO_PARTIAL);
    assert(upstream_audio_mail_hi()==0x8071); assert(upstream_audio_mail_lo()==0xfeed);
    boot(); assert(irqs==1); assert(upstream_audio_mail_hi()==0xdcd1);
    assert(upstream_audio_mail_hi()==0xdcd1); assert(upstream_audio_mail_lo()==0);
    assert(upstream_audio_mail_hi()==0);
    upstream_audio_set_pcm_sink(pcm,NULL);
    setup_voice(0xa,3);
    assert(upstream_audio_process(0x80001000,13)>=0);
    assert(r16(0x4000+4*10)==2048); assert(r16(0x4002+4*10)==4096);
    assert(upstream_audio_dma(0x4000,640,32000)>=0);
    assert(captured[20]==4096 && captured[21]==2048 && pcm_frames==160);
    assert(upstream_audio_stats().nonzero_dma_frames>0);
    /* Newer AX channel bits: bit1 enables right only, not AUXB. */
    setup_voice(0xa,2); assert(upstream_audio_process(0x1000,13)>=0);
    assert(r16(0x4028)==2048 && r16(0x402a)==0);
    /* ADPCM with predictor0, scale4 and all +1 residuals yields nonzero. */
    setup_voice(0,1); memset(aram,0x11,sizeof(aram));
    for(unsigned i=0;i<sizeof(aram);i+=8) aram[i]=4;
    assert(upstream_audio_process(0x1000,13)>=0); assert(r16(0x402a)>0);
    test_src(&c);
    test_depop(&c);
    test_compressor(&c);
    /* Actual guest table is loaded from the supplied DOL, never embedded. */
    memset(gc_ram+0x1800,0,54);w32(0x1800,40000);w32(0x1806,(uint32_t)-20000);
    const uint16_t real[]={0,0,0x1800,0x12,0x8000,10,0x8035,0xf040,0xe,0,0x3000,0,0x4000,0xf};
    for(unsigned i=0;i<14;++i)w16(0x1000+2*i,real[i]);
    assert(upstream_audio_process(0x1000,14)==UPSTREAM_AUDIO_PARTIAL);
    for(unsigned i=0;i<160;++i) {
        int64_t gain=r16(0x35f040+2*i),l=(40000*gain)>>15,r=(-20000*gain)>>15;
        if(l>32767)l=32767;
        assert((int16_t)r16(0x4002+4*i)==l && (int16_t)r16(0x4000+4*i)==r);
    }
    assert(!(upstream_audio_stats().limitations&UPSTREAM_AUDIO_LIMIT_COMPRESSOR));
    w16(0x1000,0xe); assert(upstream_audio_process(0x1000,1)<0);
    assert(upstream_audio_dma(0x017ffffe,4,32000)<0);
    assert(upstream_audio_dma(0xcc005000,4,32000)<0);
    assert(upstream_audio_boot(0x1000,32,0)<0);
    assert(upstream_audio_process(0x1000,1)==UPSTREAM_AUDIO_UNSUPPORTED_UCODE);
    /* Cyclic PB and missing terminator cannot hang; errors are bounded. */
    boot(); setup_voice(0xa,3); pb32(0,0x2000);
    assert(upstream_audio_process(0x1000,13)<0);
    setup_voice(0xa,3); w16(0x1000,0xfffe);
    assert(upstream_audio_process(0x1000,1)<0);
    /* Raw ten-word task upload, with same measured ucode at resume vector. */
    boot(); while(upstream_audio_mail_hi())upstream_audio_mail_lo();
    assert(upstream_audio_mail(0xcdd10001)>=0);
    const uint32_t task[]={0,0,0,0x80360a80,0x1f20,0,0x14,0,0,0};
    for(unsigned i=0;i<10;++i)assert(upstream_audio_mail(task[i])>=0);
    assert(upstream_audio_mail_lo()==1);
    /* Actual C HAL routes low-half commits and honors DSP interrupt mask. */
    hal_ai_init();
    assert(hal_ai_dma_sample_rate()==32000);
    hal_ai_write16(2,0); assert(hal_ai_dma_sample_rate()==48000);
    hal_ai_write16(2,0x42); assert(hal_ai_dma_sample_rate()==32000);
    hal_dsp_init(); upstream_audio_set_pcm_sink(pcm,NULL);
    assert(hal_dsp_read16(4)==0x8071);assert(hal_dsp_read16(6)==0xfeed);
    const uint32_t rom[]={0x80f3a001,0x80360a80,0x80f3c002,0,0x80f3a002,0x1f20,0x80f3b002,0,0x80f3d001,0x10};
    for(unsigned i=0;i<10;++i)send(rom[i]);
    assert((hal_dsp_csr()&0x80) && !(pi&PI_INT_DSP));
    hal_dsp_write16(0xa,0x900); assert(pi&PI_INT_DSP);
    assert(hal_dsp_read16(4)==0xdcd1); assert(hal_dsp_read16(6)==0);
    hal_dsp_write16(0xa,0x990); assert(!(pi&PI_INT_DSP));
    setup_voice(0xa,3); send(0xbabe000d);send(0x80001000);
    assert(hal_dsp_read16(6)==2);
    hal_dsp_write16(0x30,0);hal_dsp_write16(0x32,0x4000);hal_dsp_write16(0x36,0x8014);
    unsigned before=pcm_frames; hal_dsp_frame_tick(); assert(pcm_frames-before==480);
    assert(hal_dsp_read16(0x36)&0x8000); /* repeating AI DMA */
    hal_dsp_write16(0x36,0);before=pcm_frames;hal_dsp_frame_tick();assert(pcm_frames==before);
    /* Cycle timing: exact 5ms block, no frame double-count, IRQ boundary. */
    hal_dsp_init(); upstream_audio_set_pcm_sink(pcm,NULL);
    hal_dsp_tick_cycles(0,486000000);
    hal_dsp_write16(0x32,0x4000);hal_dsp_write16(0x36,0x8014);
    before=pcm_frames;hal_dsp_frame_tick();assert(pcm_frames==before);
    hal_dsp_tick_cycles(2429999,486000000);assert(pcm_frames==before);
    hal_dsp_tick_cycles(1,486000000);assert(pcm_frames-before==160);
    /* A 20ms jump returns after one block; caller can dispatch IRQ before catchup. */
    before=pcm_frames;hal_dsp_tick_cycles(9720000,486000000);assert(pcm_frames-before==160);
    hal_dsp_write16(0x32,0x4400); /* guest queues different next buffer */
    before=pcm_frames;hal_dsp_tick_cycles(0,486000000);assert(pcm_frames-before==160);
    hal_dsp_write16(0x36,0);before=pcm_frames;hal_dsp_tick_cycles(9720000,486000000);assert(pcm_frames==before);
    if(argc>=3) {
        FILE *f=fopen(argv[2],"rb");assert(f);
        assert(fread(gc_ram,1,sizeof(gc_ram),f)==sizeof(gc_ram));fclose(f);
        assert(upstream_audio_bind(&c)>=0);boot();
        unsigned active=0;
        uint32_t at=0x3ae920;
        for(unsigned i=0;i<64 && at && at+236<sizeof(gc_ram);++i) {
            active += r16(at+14)!=0; at=be32(gc_ram+at)&0x1fffffff;
        }
        int rc=upstream_audio_process(0x803a9ac0,0x180);
        fprintf(stderr,"snapshot replay activePB=%u status=%d error=%s (ARAM unavailable: zeros/fixture bytes only)\n",active,rc,upstream_audio_last_error());
        assert(rc>=0);
        assert(!(upstream_audio_stats().limitations&UPSTREAM_AUDIO_LIMIT_COMPRESSOR));
    }
    puts("upstream audio selftest: PASS (diagnostic partial mixer)");
    return 0;
}
