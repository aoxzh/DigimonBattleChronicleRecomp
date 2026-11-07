/* Actual IRQ/VI/AI/DSP/EXI/MMIO integration. Audio output and unrelated devices
 * are mocked; device state machines, timebase, DMA and interrupt delivery are not.
 * test_device_batch.py builds/runs this on Windows and POSIX. */
#define _POSIX_C_SOURCE 200809L
#include "hal.h"
#include "irq.h"
#include "vi.h"
#include "ai.h"
#include "dsp.h"
#include "exi.h"
#include "mem.h"
#include "gc_abi.h"
#include "../platform/desktop/upstream_audio/upstream_audio.h"
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HZ 486000000u
#define FIELD 8108100u
#define EE 0x8000u
GcCtx gc;
uint32_t gc_spr_misc[1024];
uint8_t gc_ram[GC_MEM1_SIZE];
int gc_runtime_manifest_enabled(void) { return 0; }
void gc_runtime_manifest_note_block(uint32_t p) { (void)p; }
void gc_runtime_manifest_note_event(const char *s) { (void)s; }
void gc_runtime_manifest_note_mmio(uint32_t p,unsigned n,int w) { (void)p;(void)n;(void)w; }
void hal_si_frame_tick(void) {}
uint16_t hal_cp_read16(uint32_t r) { (void)r;return 0; }
uint16_t hal_pe_read16(uint32_t r) { (void)r;return 0; }
void hal_cp_write16(uint32_t r,uint16_t v) { (void)r;(void)v; }
void hal_pe_write16(uint32_t r,uint16_t v) { (void)r;(void)v; }
uint32_t hal_di_read32(uint32_t r) { (void)r;return 0; }
uint32_t hal_si_read32(uint32_t r) { (void)r;return 0; }
void hal_di_write32(uint32_t r,uint32_t v) { (void)r;(void)v; }
void hal_si_write32(uint32_t r,uint32_t v) { (void)r;(void)v; }
void hal_gxpipe_write8(uint8_t v) { (void)v; }
void hal_gxpipe_write16(uint16_t v) { (void)v; }
void hal_gxpipe_write32(uint32_t v) { (void)v; }
void hal_gxpipe_write64(uint64_t v) { (void)v; }
uint32_t hal_mem_read32(uint32_t p) {
    assert(p<=GC_MEM1_SIZE-4);
    return (uint32_t)gc_ram[p]<<24|(uint32_t)gc_ram[p+1]<<16|
           (uint32_t)gc_ram[p+2]<<8|gc_ram[p+3];
}
void hal_mem_read_burst(uint32_t p,uint8_t *d,size_t n) { assert(p+n<=GC_MEM1_SIZE);memcpy(d,gc_ram+p,n); }
void hal_mem_write_burst(uint32_t p,const uint8_t *s,size_t n) { assert(p+n<=GC_MEM1_SIZE);memcpy(gc_ram+p,s,n); }
uint8_t hal_ar_read8(uint32_t p) { (void)p;return 0; }
void hal_ar_dma(uint32_t m,uint32_t a,uint32_t n,int d) { (void)m;(void)a;(void)n;(void)d; }
int upstream_audio_bind(const UpstreamAudioConfig *c) { (void)c;return 0; }
void upstream_audio_reset(void) {}
void upstream_audio_ipl_ready(void) {}
int upstream_audio_mail(uint32_t m) { (void)m;return 0; }
uint16_t upstream_audio_mail_hi(void) { return 0; }
uint16_t upstream_audio_mail_lo(void) { return 0; }
const char *upstream_audio_last_error(void) { return "test"; }

typedef struct { uint64_t cycle,tb; uint32_t kind,a,b,c; } Event;
static Event events[4096],reference[4096];
static unsigned event_count,reference_count,frames,callback_mode;
static jmp_buf callback_exit;
static void record(uint32_t kind,uint32_t a,uint32_t b,uint32_t c) {
    assert(event_count<sizeof(events)/sizeof(events[0]));
    Event e={0}; e.cycle=hal_cpu_cycles();e.tb=hal_timebase_ticks();e.kind=kind;e.a=a;e.b=b;e.c=c;
    events[event_count++]=e;
}
int upstream_audio_dma(uint32_t a,uint32_t n,uint32_t rate) { record('D',a,n,rate);return 0; }
static void callback(void *p) {
    (void)p;++frames;
    assert(hal_cpu_cycles()==(uint64_t)frames*FIELD);
    record('V',hal_vi_read16(0x2c),hal_vi_read16(0x2e),hal_ai_samples());
    /* Direct public calls while VI's advancement stack is live. */
    hal_vi_write16(0x30,(uint16_t)(hal_vi_reg(0x30)&0x7fff));
    hal_vi_write16(0x34,(uint16_t)(hal_vi_reg(0x34)&0x7fff));
    if(callback_mode) {
        hal_ai_write16(2,0x43); /* stream48k, DMA32k, running */
        hal_dsp_write16(0x32,0x1800);
        if(callback_mode==2) { callback_mode=0;longjmp(callback_exit,1); }
    }
}
static void put32(uint32_t p,uint32_t v) {
    gc_ram[p]=(uint8_t)(v>>24);gc_ram[p+1]=(uint8_t)(v>>16);
    gc_ram[p+2]=(uint8_t)(v>>8);gc_ram[p+3]=(uint8_t)v;
}
static void reset(void) {
    memset(&gc,0,sizeof(gc));memset(gc_ram,0,sizeof(gc_ram));
    hal_irq_init();hal_ai_init();hal_dsp_init();hal_vi_init();
    hal_exi_set_memcard(NULL);hal_exi_init();
    hal_decrementer_write(0x7fffffff);
    gc.pc=0x80004000;gc.msr=EE;gc.r[13]=0x803d6120;
    put32(0x3010,0x80005000);put32(0x3020,0x80006000);
    event_count=frames=callback_mode=0;
    hal_set_frame_callback(callback,NULL);
}
static void ntsc(void) {
    hal_vi_write16(0,0x0f06);hal_vi_write16(6,429);
    hal_vi_write16(0x0c,3);hal_vi_write16(0x0e,24);
    hal_vi_write16(0x10,2);hal_vi_write16(0x12,25);
    hal_vi_write16(0x30,0x1107);hal_vi_write16(0x32,430);
    hal_vi_write16(0x34,0x1001);hal_vi_write16(0x36,1);
    hal_vi_write16(2,1);
}
static void start_audio(void) {
    hal_ai_write16(14,83);hal_ai_write16(2,0x45);
    hal_dsp_write16(0x32,0x1000);hal_dsp_write16(0x36,0x8014);
    hal_dsp_write16(0x0a,0x18);hal_pi_write32(4,PI_INT_AI|PI_INT_DSP|PI_INT_VI|PI_INT_EXI);
}
static void until(uint64_t target,uint32_t chunk,int eager) {
    while(hal_cpu_cycles()<target) {
        uint64_t left=target-hal_cpu_cycles();uint32_t n=left<chunk?(uint32_t)left:chunk;
        hal_clock_advance(n);
        if(eager) hal_clock_sync_devices();
    }
}
static void snapshot(unsigned marker) {
    uint64_t now=hal_cpu_cycles();
    record('S',marker,hal_mmio_read32(HW_AI_BASE+8),hal_mmio_read16(HW_DSP_BASE+0x3a));
    record('B',hal_vi_read16(0x2c),hal_vi_read16(0x2e),hal_pi_read32(0));
    record('T',gc_spr_misc[22],(uint32_t)hal_timebase_ticks(),hal_ai_cycles_to_event(HZ));
    uint32_t v=hal_irq_poll_cycles(0);record('I',v,gc.srr0,gc.srr1);
    if(v) {
        hal_ai_write16(2,(uint16_t)(hal_ai_read16(2)|8));
        hal_dsp_write16(0x0a,0x18);gc.pc=0x80004000;gc.msr=EE;
    }
    assert(now==hal_cpu_cycles());
}
static void script(uint32_t chunk,int eager) {
    reset();ntsc();start_audio();
    until(15187,chunk,eager);snapshot(0);
    until(15188,chunk,eager);snapshot(1); /* 32k half-cycle sample boundary */
    until(700019,chunk,eager);snapshot(2);
    hal_mmio_write16(HW_AI_BASE+2,0x07); /* pending old 32k fraction, new48k DMA */
    until(1200007,chunk,eager);snapshot(3);
    hal_mmio_write32(HW_AI_BASE+8,0xfffffffe);hal_mmio_write32(HW_AI_BASE+12,1);
    until(1230382,chunk,eager);snapshot(4);
    hal_mmio_write8(HW_AI_BASE+3,0x67); /* reset stream while DMA continues32k */
    until(2100001,chunk,eager);snapshot(5);
    hal_dsp_write16(0x32,0x1400);hal_dsp_write16(0x36,0x800a); /* next latched block */
    until(7200011,chunk,eager);snapshot(6);
    hal_dsp_write16(0x36,0);until(7300013,chunk,eager);snapshot(7);
    hal_dsp_write16(0x32,0x1800);hal_dsp_write16(0x36,0x8006);
    until(2ull*FIELD+123,chunk,eager);snapshot(8);
    hal_dsp_write16(0x36,0);
    assert(frames==2);assert(hal_cpu_cycles()==2ull*FIELD+123);
}
static void compare(unsigned marker) {
    if(event_count!=reference_count || memcmp(events,reference,event_count*sizeof(Event))) {
        fprintf(stderr,"event mismatch scenario=%u count=%u expected=%u\n",marker,event_count,reference_count);
        for(unsigned i=0;i<event_count && i<reference_count;i++) if(memcmp(&events[i],&reference[i],sizeof(Event))) {
            Event *a=&events[i],*b=&reference[i];
            fprintf(stderr,"event %u got=%c@%llu %x/%x/%x expected=%c@%llu %x/%x/%x\n",i,a->kind,(unsigned long long)a->cycle,a->a,a->b,a->c,b->kind,(unsigned long long)b->cycle,b->a,b->b,b->c);break;
        }
        abort();
    }
}
static void test_partition(void) {
    script(7,1);reference_count=event_count;memcpy(reference,events,event_count*sizeof(Event));
    const uint32_t chunks[]={1,7,256,100003,UINT32_MAX};
    for(unsigned i=0;i<sizeof(chunks)/sizeof(chunks[0]);i++) { script(chunks[i],0);compare(chunks[i]); }
    puts("PASS real device eager/batched partitions + public/MMIO transitions");
}
static void test_old_configuration(void) {
    reset();hal_clock_advance(100000); /* disabled device debt cannot enter a new DMA */
    start_audio();hal_clock_advance(2429999);assert(event_count==0);
    assert(hal_dsp_read16(0x3a)==0);hal_clock_advance(1);
    assert(event_count==1 && events[0].cycle==2530000 && events[0].a==0x1000);
    reset();hal_ai_write16(2,0x41);hal_clock_advance(15187);
    hal_ai_write16(2,0x43);assert(hal_ai_samples()==0);hal_clock_advance(1);
    assert(hal_ai_samples()==1); /* fractional 32k work survives AFR change */
    hal_ai_write16(2,0x63);assert(hal_ai_samples()==0);
    hal_clock_advance(10124);assert(hal_ai_samples()==0);
    hal_clock_advance(1);assert(hal_ai_samples()==1);
    puts("PASS old-rate flush, reset and fresh DMA cannot consume prior debt");
}
static void test_irq_boundary(void) {
    reset();hal_decrementer_write(0);gc.msr=0;
    hal_clock_advance(11);assert(gc_spr_misc[22]==0);
    hal_clock_advance(1);assert(gc_spr_misc[22]==UINT32_MAX);
    assert(!hal_irq_poll_cycles(0));gc.msr=EE;
    assert(hal_irq_poll_cycles(0)==0x80000900);assert(hal_cpu_cycles()==12);
    reset();start_audio();hal_ai_write16(14,160);hal_clock_advance(2429999);
    assert(!hal_irq_poll_cycles(0));hal_clock_advance(1);
    assert(hal_irq_poll_cycles(0)==GC_VEC_EXTERNAL);
    assert(event_count==1 && gc.srr0==0x80004000);
    puts("PASS DEC and DMA IRQ next-boundary delivery");
}
static void test_callback(void) {
    reset();ntsc();start_audio();callback_mode=1;
    until(2ull*FIELD+17,7,1);snapshot(1);
    reference_count=event_count;memcpy(reference,events,event_count*sizeof(Event));
    reset();ntsc();start_audio();callback_mode=1;
    until(2ull*FIELD+17,100003,0);snapshot(1);compare(0x123);
    reset();ntsc();start_audio();callback_mode=2;
    if(!setjmp(callback_exit)) { hal_clock_advance(FIELD+500000);abort(); }
    assert(hal_cpu_cycles()==FIELD && frames==1);
    assert(hal_vi_cycles_to_field()==FIELD);
    /* The abandoned coarse call must neither retain its unused budget nor
     * duplicate already committed device work after longjmp. */
    hal_clock_advance(17);assert(hal_cpu_cycles()==FIELD+17);
    assert(hal_ai_samples()>0);assert(frames==1);
    until(2ull*FIELD,100003,0);assert(frames==2);
    puts("PASS callback MMIO reentry and longjmp state commit");
}
static void exi_word(uint32_t w,unsigned n) { hal_exi_write32(0x10,w);hal_exi_write32(0x0c,((n-1)<<4)|5); }
static void test_exi(const char *path) {
    reset();hal_exi_set_memcard(path);hal_exi_init();
    hal_exi_write32(0,0x81);exi_word(0x81010000,2);hal_exi_write32(0,1);
    hal_pi_write32(4,PI_INT_EXI);hal_clock_advance(12345);
    hal_exi_write32(0,0x81);exi_word(0xf1005000,3);hal_exi_write32(0,1);
    hal_clock_advance(49999);assert(hal_exi_cycles_to_event()==1 && !(hal_irq_pending()&PI_INT_EXI));
    hal_clock_advance(1);assert(hal_irq_pending()&PI_INT_EXI);
    assert(hal_cpu_cycles()==62345);assert(hal_exi_cycles_to_event()==HZ);
    hal_clock_advance(HZ-1);assert(hal_exi_cycles_to_event()==1);
    hal_clock_advance(1);assert(hal_exi_cycles_to_event()==UINT32_MAX);
    hal_exi_set_memcard(NULL);hal_exi_init();
    puts("PASS EXI old-command flush, busy completion and delayed persistence");
}
static void test_large_budget(void) {
    reset();hal_ai_write16(2,0x41);
    hal_clock_advance(UINT32_MAX);hal_clock_advance(9);
    const uint64_t total=(uint64_t)UINT32_MAX+9;
    assert(hal_cpu_cycles()==total && hal_timebase_ticks()==total/12);
    assert(hal_ai_samples()==total*32000/HZ);
    assert(gc_spr_misc[22]==0x7fffffffu-(uint32_t)(total/12));
    puts("PASS UINT32_MAX accumulated budget and CPU/12 phase");
}
static void benchmark(void) {
    for(int eager=0;eager<=1;eager++) {
        reset();ntsc();start_audio();clock_t start=clock();
        for(unsigned i=0;i<1000000;i++) { hal_clock_advance(3);if(eager)hal_clock_sync_devices(); }
        hal_clock_sync_devices();
        printf("BENCH %s %.3fms for 1000000 three-cycle blocks\n",eager?"eager":"batched",1000.0*(clock()-start)/CLOCKS_PER_SEC);
    }
}
int main(int argc,char **argv) {
    assert(argc==2);
    test_partition();test_old_configuration();test_irq_boundary();test_callback();test_exi(argv[1]);test_large_budget();benchmark();
    puts("PASS device batch integration");return 0;
}
