/* hal/irq.c — PI 汇聚 + IRQ 注入 + HAL 时间模型。位序出处见 irq.h。 */
#include "irq.h"
#include "hal.h"
#include "gc_abi.h"
#include "vi.h"
#include "ai.h"
#include "dsp.h"
#include "mem.h"
#include "si.h"
#include "exi.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* PI 寄存器影子（0x00 cause / 0x04 mask；FIFO 三件套给 GX） */
static uint32_t pi_cause;
static uint32_t pi_mask;
uint32_t hal_pi_fifo_base, hal_pi_fifo_end, hal_pi_fifo_wptr;
static uint32_t pi_reset_code;

static void reset_timing(void);
static void irq_refresh_fast(void);

enum {
    POLL_DIAG_FP = 1u << 0, POLL_DIAG_AX = 1u << 1,
    POLL_DIAG_RAM = 1u << 2, POLL_DIAG_VI = 1u << 3,
    POLL_DIAG_MANIFEST = 1u << 4, POLL_DIAG_IRQ = 1u << 5
};
static unsigned poll_diagnostics;

static int env_enabled(const char *name)
{
    const char *value = getenv(name);
    return value && *value && *value != '0';
}

static void init_poll_diagnostics(void)
{
    const char *fp = getenv("GC_FP_TRACE"), *ax = getenv("GC_AX_DEPOP_TRACE");
    poll_diagnostics = 0;
    if (fp && strcmp(fp, "1") == 0) poll_diagnostics |= POLL_DIAG_FP;
    if (ax && strcmp(ax, "1") == 0) poll_diagnostics |= POLL_DIAG_AX;
    /* RAM_WATCH has an address/length grammar, not boolean flag semantics. */
    if (getenv("GC_RAM_WATCH")) poll_diagnostics |= POLL_DIAG_RAM;
    if (env_enabled("GC_VI_TRACE")) poll_diagnostics |= POLL_DIAG_VI;
    if (gc_runtime_manifest_enabled()) poll_diagnostics |= POLL_DIAG_MANIFEST;
    if (env_enabled("GC_IRQ_DIAG")) poll_diagnostics |= POLL_DIAG_IRQ;
}

/* 复位按钮状态位（PI cause bit16：1=未按）——BS2 后为 1 */
#define PI_INT_RST_BUTTON 0x10000u

void hal_irq_init(void)
{
    pi_cause = PI_INT_RST_BUTTON;   /* 未按下 */
    pi_mask = 0;
    hal_pi_fifo_base = hal_pi_fifo_end = hal_pi_fifo_wptr = 0;
    pi_reset_code = 0;
    reset_timing();
}

void hal_pi_set_interrupt(uint32_t bits)   { pi_cause |= bits & 0xFFFFu; irq_refresh_fast(); }
void hal_pi_clear_interrupt(uint32_t bits) { pi_cause &= ~bits; irq_refresh_fast(); }
void hal_pi_fifo_reset(void)               { hal_pi_fifo_wptr = hal_pi_fifo_base; }

uint32_t hal_pi_read32(uint32_t reg)
{
    hal_clock_sync_devices();
    switch (reg) {
    case 0x00: return pi_cause;
    case 0x04: return pi_mask;
    case 0x0C: return hal_pi_fifo_base;
    case 0x10: return hal_pi_fifo_end;
    case 0x14: return hal_pi_fifo_wptr;
    case 0x1C: return 0;  /* PI_ERROR_CAUSE：NoError */
    case 0x20: return 0;  /* PI_ERROR_ADDRESS */
    case 0x24: return pi_reset_code;
    case 0x2C: return 0x09A71100; /* FLIPPER_REV（Dolphin 返回翻转版本常量） */
    default:   return 0;
    }
}

void hal_pi_write32(uint32_t reg, uint32_t v)
{
    hal_clock_sync_devices();
    switch (reg) {
    case 0x00:
        /* W1C（写 1 清除），复位按钮状态位不可写 */
        pi_cause &= ~(v & 0xFFFFu);
        pi_cause |= PI_INT_RST_BUTTON;
        break;
    case 0x04:
        pi_mask = v & 0xFFFFu;
        break;
    case 0x0C: hal_pi_fifo_base = v & ~0x1Fu; break;
    case 0x10: hal_pi_fifo_end  = v & ~0x1Fu; break;
    case 0x14: hal_pi_fifo_wptr = v & ~0x1Fu; break;
    case 0x18: hal_pi_fifo_reset(); break;   /* PI_FIFO_RESET（GXAbortFrame 用） */
    case 0x24: pi_reset_code = v; break;
    default: break;
    }
    irq_refresh_fast();
}

uint32_t hal_irq_pending(void)
{
    hal_clock_sync_devices();
    return pi_cause & pi_mask;
}

/* 诊断：中断注入健康度（SIGABRT 时 main_headless 打印） */
uint64_t hal_irq_diag_ee_off;      /* poll 时 EE=0（中断被屏蔽）次数 */
uint64_t hal_irq_diag_injected;    /* 实际注入次数 */
uint64_t hal_irq_diag_pending;     /* EE 开且 pending（理论可注入）次数 */
uint64_t hal_irq_diag_msr_hist[16]; /* msr 低 4 位直方图 */
uint64_t hal_irq_diag_msr_hi8[16];  /* msr bit15:12 直方图 */
uint64_t hal_irq_diag_src[16];      /* 注入时 pending 位分布（PI 线 0-15） */
static uint64_t diag_pending_line;  /* EE 开 + PI 线 pending 次数 */
uint64_t hal_irq_diag_line;         /* 同上（ABRT dump 用） */
uint64_t hal_irq_diag_notready;     /* 上述但 ext handler 未就绪次数 */

/* ---- 时间模型 ---- */
/* Watchdog breadcrumb for a split guest/render thread: the main thread can
 * report where the guest stopped even without a host debugger. */
volatile int hal_guest_phase;
/* One load per generated block replaces a call into the slow poll path while
 * no interrupt is deliverable. Diagnostics force the slow path. */
volatile uint32_t hal_irq_fast_pending;
uint64_t hal_irq_poll_total;

static uint64_t frames;
static uint64_t cpu_cycles;
static uint64_t timebase_ticks;
static uint32_t timer_phase;                    /* CPU cycles carried toward CPU/12 */
/* Device state may lag CPU time only before its earliest observable event.
 * pending + deadline_remaining never exceeds the cached uint32_t deadline. */
static uint32_t pending_device_cycles, deadline_remaining;
static uint32_t decrementer;
static int decrementer_pending;
static uint32_t poll_cycles = 256;               /* 可调：平均每块周期估算 */
#define CPU_CLOCK_HZ 486000000ull
#define TIMER_RATIO 12u

static void irq_refresh_fast(void)
{
    hal_irq_fast_pending = (poll_diagnostics || decrementer_pending ||
                            (pi_cause & pi_mask)) ? 1u : 0u;
}

extern uint32_t gc_spr_misc[1024];
static hal_frame_cb frame_cb;
static void *frame_cb_user;

static void reset_timing(void)
{
    frames = cpu_cycles = timebase_ticks = 0;
    timer_phase = decrementer = 0;
    pending_device_cycles = deadline_remaining = 0;
    decrementer_pending = 0;
    gc_spr_misc[22] = 0;
    poll_cycles = 256;
    frame_cb = NULL;
    frame_cb_user = NULL;
    hal_irq_poll_total = 0;
    init_poll_diagnostics();
    irq_refresh_fast();
}

uint64_t hal_cpu_cycles(void) { return cpu_cycles; }
uint64_t hal_timebase_ticks(void) { return timebase_ticks; }

/* PPC 750 DEC is free-running, including while negative. A write sign edge
 * requests an exception but does not acknowledge an already pending request.
 * Sources: MPC750UM, Decrementer Exception; Dolphin Interpreter_SystemRegisters.cpp
 * mtspr SPR_DEC and HW/SystemTimers.h TIMER_RATIO (12 CPU cycles per tick).
 * Keep an internal old value so a caller's preliminary SPR shadow store is safe. */
void hal_decrementer_write(uint32_t value)
{
    if (!(decrementer & 0x80000000u) && (value & 0x80000000u))
        decrementer_pending = 1;
    decrementer = value;
    gc_spr_misc[22] = value;
    irq_refresh_fast();
}

static void advance_clock(uint32_t cycles)
{
    cpu_cycles += cycles;
    const uint64_t phase = (uint64_t)timer_phase + cycles;
    const uint32_t ticks = (uint32_t)(phase / TIMER_RATIO);
    timer_phase = (uint32_t)(phase % TIMER_RATIO);
    timebase_ticks += ticks;
    /* Distance to the next 0 -> 0xffffffff edge, even from a negative DEC.
     * A poll's uint32_t CPU budget cannot span a complete DEC tick period. */
    if ((uint64_t)ticks >= (uint64_t)decrementer + 1u) {
        decrementer_pending = 1;
        hal_irq_fast_pending = 1;
    }
    decrementer -= ticks;
    gc_spr_misc[22] = decrementer;
}

static void vi_trace_block(uint32_t pc)
{
    static int enabled = -1;
    static unsigned count;
    if (enabled < 0) {
        const char *e = getenv("GC_VI_TRACE");
        enabled = e && *e && *e != '0';
    }
    if (!enabled || count >= 128u) return;
    switch (pc) {
    case 0x802c32f4u: case 0x802c32f8u: case 0x802c3304u:
    case 0x802c3308u: case 0x802c3314u: case 0x802c3318u:
    case 0x802dd304u: case 0x802dd344u: case 0x802dd480u:
    case 0x802dd49cu: case 0x802dd6d4u:
    case 0x802dde5cu: case 0x802ddea4u: case 0x802ddeacu:
    case 0x802ddeb4u: case 0x802dded0u: case 0x802ddee0u:
    case 0x802ddf04u: case 0x802ddf3cu: case 0x802ddf44u:
    case 0x802ddf4cu: case 0x802ddf68u: case 0x802ddf78u:
    case 0x802ddf90u: case 0x802ddfa0u: case 0x802ddfacu:
    case 0x802ddfb8u: case 0x802ddfc4u: case 0x802ddfd0u:
    case 0x802ddfd8u: case 0x802ddfccu: case 0x802de008u:
    case 0x802a4fb4u: case 0x802a4fc8u: case 0x802a4ff0u:
    case 0x802a505cu: case 0x802a5034u:
    case 0x802de430u: case 0x802de538u:
    case 0x802de540u: case 0x802de548u:
    case 0x802de550u: case 0x802de564u: case 0x802de580u:
    case 0x802de588u: case 0x802de590u: case 0x802de598u:
    case 0x802de59cu: case 0x802de5b0u: case 0x802de5bcu:
    case 0x802de5c0u:
        fprintf(stderr,
                "[vi-trace] frame=%llu pc=%08x cr=%08x r0=%08x r3=%08x r4=%08x "
                "r5=%08x r6=%08x r7=%08x r9=%08x r10=%08x r11=%08x r12=%08x "
                "r14=%08x r15=%08x r16=%08x r17=%08x r28=%08x r29=%08x r31=%08x\n",
                (unsigned long long)frames, pc, gc.cr, gc.r[0], gc.r[3], gc.r[4],
                gc.r[5], gc.r[6], gc.r[7], gc.r[9], gc.r[10], gc.r[11], gc.r[12],
                gc.r[14], gc.r[15], gc.r[16], gc.r[17], gc.r[28], gc.r[29], gc.r[31]);
        count++;
        break;
    default:
        break;
    }
}

static void ram_watch_block(void)
{
    static int checked, enabled;
    static uint32_t address, prior_pc, prior_lr, previous[4];
    static unsigned changes, bytes = 16;
    static uint32_t prior_r[32], prior_gqr7;
    if (!checked) {
        const char *value = getenv("GC_RAM_WATCH");
        checked = 1;
        if (value) (void)sscanf(value, "%x:%u", &address, &bytes);
        enabled = value && bytes && bytes <= sizeof(previous) &&
                  !(bytes & 3u) && address <= GC_MEM1_SIZE - bytes;
        if (enabled) memcpy(previous, gc_ram + address, sizeof(previous));
    }
    if (!enabled) return;
    if (memcmp(previous, gc_ram + address, bytes) && changes++ < 256u) {
        fprintf(stderr, "[ram-watch] frame=%llu phys=%08x previous_pc=%08x previous_lr=%08x next_pc=%08x words=",
                (unsigned long long)frames, address, prior_pc, prior_lr, gc.pc);
        for (unsigned i = 0; i < bytes / 4u; i++)
            fprintf(stderr, "%08x%s", hal_mem_read32(address + i * 4u), i + 1u == bytes / 4u ? "\n" : " ");
        fprintf(stderr, "[ram-watch] prior gqr7=%08x", prior_gqr7);
        for (unsigned i = 0; i < 32; i++) fprintf(stderr, " r%u=%08x", i, prior_r[i]);
        fputc('\n', stderr);
        memcpy(previous, gc_ram + address, sizeof(previous));
    }
    prior_pc = gc.pc;
    prior_lr = gc.lr;
    memcpy(prior_r, gc.r, sizeof(prior_r));
    prior_gqr7 = gc.gqr[7];
}

void hal_set_poll_cycles(uint32_t cycles) { poll_cycles = cycles ? cycles : 1; }
uint64_t hal_poll_count(void)  { return hal_irq_poll_total; }
uint64_t hal_frame_count(void) { return frames; }
void hal_set_frame_callback(hal_frame_cb cb, void *user)
{
    frame_cb = cb; frame_cb_user = user;
}

/* VI calls this once per actual field boundary, after committing beam/status.
 * Display interrupt compares are independent; this callback never asserts IRQs. */
void hal_irq_frame_event(void)
{
    frames++;
    gc_runtime_manifest_note_event("frame");
    hal_si_frame_tick();
    hal_ai_frame_tick();
    hal_dsp_frame_tick();   /* AID DMA bytes-left 随虚拟音频时钟递减 */
    if (frame_cb) frame_cb(frame_cb_user);
}

/* OSInit 装向量 stub 的中间态：*(0x3010)（EXT handler 表[4]）为 0 或模板地址
 * 0x802BD894 时注入会让 handler 链跑飞（实测）——等真 handler 装好。 */
static uint32_t irq_table_entry(unsigned exc)
{
    uint32_t pa = 0x3000u + 4u * exc;
    return ((uint32_t)gc_ram[pa] << 24) | ((uint32_t)gc_ram[pa+1] << 16)
         | ((uint32_t)gc_ram[pa+2] << 8) | gc_ram[pa+3];
}
static int ext_handler_ready(void)
{
    uint32_t h = irq_table_entry(4);
    return h != 0 && h != 0x802BD894u;
}
/* DEC handler 表[8] 无效时挂起注入：游戏 alarm 系统未就绪，真机此时同样无
 * handler 可走；我们的虚拟时间快于真实，会提前到达下溢（实测表全零）。 */
static int dec_handler_ready(void)
{
    uint32_t h = irq_table_entry(8);
    return h >= 0x80003100u && h < 0x81800000u;
}

static uint32_t take_external(void)
{
    gc.srr0 = gc.pc;       /* 重执行语义：handler rfi 后回到本块 */
    gc.srr1 = gc.msr;
    gc.msr &= ~0x8000u;    /* 取中断时 EE 清零 */
    gc.pc = GC_VEC_EXTERNAL;
    if (poll_diagnostics & POLL_DIAG_IRQ) {
        /* Optional interrupt-storm evidence; never part of IRQ delivery. */
        hal_irq_diag_injected++;
        extern uint64_t hal_irq_diag_src[16];
        uint32_t pend = pi_cause & pi_mask;
        for (int b = 0; b < 16; b++)
            if (pend & (1u << b)) hal_irq_diag_src[b]++;
    }
    return GC_VEC_EXTERNAL;
}

/* Read-only guest AX depop evidence. No MMIO helpers or guest-state writes. */
static void ax_depop_trace(void)
{
    struct Pending {
        uint32_t sp, sda, ret, pb;
        int32_t before[9], contribution[9], initial[9], delta[9], residual[9];
        unsigned stage;
    };
    static struct Pending collect[8], ramp[8];
    static unsigned nc, nr, logs;
    static int enabled = -1;
    static unsigned long long collect_ok, collect_bad, ramp_ok, ramp_bad;
    static unsigned long long return_ok, return_bad, invalid, dropped;
    const uint32_t pc = gc.pc, sp = gc.r[1], sda = gc.r[13];
    const unsigned words[9] = {41,44,47,42,45,48,43,46,49};
    struct Pending *p = NULL;
    unsigned kind = 0, slot = 0;
    if (enabled < 0) {
        const char *e = getenv("GC_AX_DEPOP_TRACE");
        enabled = e && strcmp(e, "1") == 0;
    }
    if (!enabled) return;
    /* Only matching PCs (including saved collector returns) read any RAM. */
    for (unsigned i = nc; i > 0; --i)
        if (pc == collect[i-1].ret && sp == collect[i-1].sp && sda == collect[i-1].sda) {
            p = &collect[i-1]; slot = i-1; kind = 2; break;
        }
    if (!kind && pc == 0x802a5264u) {
        for (unsigned i = 0; i < nc; ++i)
            if (sp == collect[i].sp && sda == collect[i].sda &&
                gc.r[3] == collect[i].pb && (gc.lr & ~3u) == collect[i].ret)
                return; /* IRQ retry of entry before its first instruction. */
        if (nc == 8) { ++dropped; return; }
        p = &collect[nc]; kind = 1;
    }
    if (!kind && pc == 0x802a4e3cu) {
        for (unsigned i = 0; i < nr; ++i)
            if (sp == ramp[i].sp && sda == ramp[i].sda && (gc.lr & ~3u) == ramp[i].ret)
                return;
        if (nr == 8) { ++dropped; return; }
        p = &ramp[nr]; kind = 3;
    }
    if (!kind && (pc == 0x802a5224u || pc == 0x802a4700u)) {
        for (unsigned i = nr; i > 0; --i) {
            struct Pending *q = &ramp[i-1];
            if (sda != q->sda) continue;
            if (pc == 0x802a5224u && q->sp >= 8 && sp == q->sp-8 && !q->stage) {
                p = q; slot = i-1; kind = 4; break;
            }
            if (pc == 0x802a4700u && sp == q->sp && q->ret == pc && q->stage) {
                p = q; slot = i-1; kind = 5; break;
            }
        }
    }
    if (!kind) return;
    /* Validate full spans before any byte read; physical/cached/uncached MEM1 only.
     * Signed address arithmetic prevents a bad SDA wrapping into valid low RAM. */
    const int64_t acc_addr = (int64_t)sda - 0x65f8;
    uint32_t addresses[3] = {0, 0, 0x803ae8e0u}, physical[3];
    unsigned lengths[3] = {36, 100, 54};
    int valid = acc_addr >= 0 && acc_addr <= UINT32_MAX;
    addresses[0] = (uint32_t)acc_addr;
    addresses[1] = kind == 1 ? gc.r[3] : p->pb;
    if (kind >= 3) lengths[1] = 0;
    for (unsigned j = 0; j < 3; ++j) {
        uint32_t a = addresses[j];
        if (a >= 0x80000000u && a < 0x81800000u) a -= 0x80000000u;
        else if (a >= 0xc0000000u && a < 0xc1800000u) a -= 0xc0000000u;
        physical[j] = a;
        if (lengths[j] && ((addresses[j] & 1u) || a > GC_MEM1_SIZE ||
                          lengths[j] > GC_MEM1_SIZE-a)) valid = 0;
    }
    if (kind == 1 || kind == 3) {
        memset(p, 0, sizeof(*p));
        p->sp = sp; p->sda = sda; p->ret = gc.lr & ~3u;
        p->pb = kind == 1 ? gc.r[3] : 0;
    }
    int nonzero = 0, bad = !valid;
    int32_t accum[9] = {0}, initial[9] = {0}, delta[9] = {0};
    if (valid) {
        for (unsigned b = 0; b < 9; ++b) {
            const uint8_t *a = gc_ram + physical[0] + 4*b;
            accum[b] = (int32_t)((uint32_t)a[0]<<24 | (uint32_t)a[1]<<16 | (uint32_t)a[2]<<8 | a[3]);
            if (kind >= 4) {
                a = gc_ram + physical[2] + 6*b;
                initial[b] = (int32_t)((uint32_t)a[0]<<24 | (uint32_t)a[1]<<16 | (uint32_t)a[2]<<8 | a[3]);
                delta[b] = (int16_t)((uint16_t)a[4]<<8 | a[5]);
            }
        }
        if (kind == 1 || kind == 3) {
            for (unsigned b = 0; b < 9; ++b) {
                p->before[b] = accum[b];
                if (kind == 1) {
                    const uint8_t *a = gc_ram + physical[1] + 2*words[b];
                    p->contribution[b] = (int16_t)((uint16_t)a[0]<<8 | a[1]);
                }
            }
            if (kind == 1) ++nc; else ++nr;
            return;
        }
        for (unsigned b = 0; b < 9; ++b) {
            if (kind == 2) {
                bad |= (uint32_t)accum[b] != (uint32_t)p->before[b] + (uint32_t)p->contribution[b];
                nonzero |= p->contribution[b] != 0;
            } else if (kind == 4) {
                int32_t q = p->before[b]/160;
                if (q > 20) q = 20;
                if (q < -20) q = -20;
                const int32_t expect_initial = q ? p->before[b] : 0;
                const int32_t expect_residual = q ? p->before[b]-160*q : 0;
                bad |= initial[b] != expect_initial || delta[b] != -q || accum[b] != expect_residual;
                nonzero |= p->before[b] != 0;
                p->initial[b] = initial[b]; p->delta[b] = delta[b]; p->residual[b] = accum[b];
            } else {
                bad |= initial[b] != p->initial[b] || delta[b] != p->delta[b] || accum[b] != p->residual[b];
                nonzero |= p->before[b] != 0;
            }
        }
    } else ++invalid;
    if (kind == 2) { if (bad) ++collect_bad; else ++collect_ok; }
    if (kind == 4) { if (bad) ++ramp_bad; else ++ramp_ok; }
    if (kind == 5) { if (bad) ++return_bad; else ++return_ok; }
    if ((nonzero || bad) && logs < 128) {
        ++logs;
        fprintf(stderr, "[ax-depop] #%u kind=%s frame=%llu pc=%08x sp=%08x r13=%08x pb=%08x ret=%08x result=%s collect=%llu/%llu ramp=%llu/%llu return=%llu/%llu invalid=%llu dropped=%llu",
                logs, kind == 2 ? "collect" : kind == 4 ? "ramp" : kind == 5 ? "return" : "entry",
                (unsigned long long)frames, pc, sp, sda, p->pb, p->ret,
                bad ? "mismatch" : "ok", collect_ok, collect_bad, ramp_ok, ramp_bad,
                return_ok, return_bad, invalid, dropped);
        if (valid) {
            fprintf(stderr, " before=[");
            for (unsigned b = 0; b < 9; ++b) fprintf(stderr, "%s%d", b ? "," : "", p->before[b]);
            fprintf(stderr, "] %s=[", kind == 2 ? "contribution" : "initial");
            for (unsigned b = 0; b < 9; ++b) fprintf(stderr, "%s%d", b ? "," : "", kind == 2 ? p->contribution[b] : initial[b]);
            if (kind != 2) {
                fprintf(stderr, "] delta=[");
                for (unsigned b = 0; b < 9; ++b) fprintf(stderr, "%s%d", b ? "," : "", delta[b]);
            }
            fprintf(stderr, "] after=[");
            for (unsigned b = 0; b < 9; ++b) fprintf(stderr, "%s%d", b ? "," : "", accum[b]);
            fputc(']', stderr);
        }
        fprintf(stderr, "%s\n", logs == 128 ? " log-cap=128" : "");
    }
    if (kind == 2) { memmove(&collect[slot], &collect[slot+1], (nc-slot-1)*sizeof(*p)); --nc; }
    if (kind == 4) p->stage = 1;
    if (kind == 5 || (kind == 4 && !valid)) {
        memmove(&ramp[slot], &ramp[slot+1], (nr-slot-1)*sizeof(*p)); --nr;
    }
}

static void fp_trace_block(void)
{
    static int enabled = -1;
    static unsigned selection_logs, collision_logs;
    if (enabled < 0) {
        const char *e = getenv("GC_FP_TRACE");
        enabled = e && strcmp(e, "1") == 0;
    }
    if (!enabled) return;
    const int selection = gc.pc == 0x8001d874u || gc.pc == 0x8001d89cu;
    const int collision = gc.pc == 0x8022ed58u || gc.pc == 0x8022edc0u;
    if ((!selection || selection_logs++ >= 256u) &&
        (!collision || collision_logs++ >= 256u)) return;
    fprintf(stderr, "[fp-block] frame=%llu pc=%08x lr=%08x sp=%08x msr=%08x cr=%08x",
            (unsigned long long)frames, gc.pc, gc.lr, gc.r[1], gc.msr, gc.cr);
    for (unsigned r = 0; r < 32; r++) fprintf(stderr, " r%u=%08x", r, gc.r[r]);
    for (unsigned f = 0; f < 32; f++) fprintf(stderr, " f%u=%016llx", f, (unsigned long long)gc.fpr_bits[f]);
    fputc('\n', stderr);
}

static uint32_t device_deadline(void)
{
    uint32_t next = hal_vi_cycles_to_event();
    uint32_t n = hal_ai_cycles_to_event((uint32_t)CPU_CLOCK_HZ);
    if (n < next) next = n;
    n = hal_dsp_cycles_to_event((uint32_t)CPU_CLOCK_HZ);
    if (n < next) next = n;
    n = hal_exi_cycles_to_event();
    if (n < next) next = n;
    return next;
}

void hal_clock_sync_devices(void)
{
    const uint32_t cycles = pending_device_cycles;
    if (!cycles) return;
    hal_guest_phase = 2;
    /* Commit the debt before entering the devices. VI's host frame callback
     * runs last, after all devices are current, and may use MMIO or longjmp;
     * no stale debt or in-flush flag survives that abandoned stack. The audio
     * sink retains its existing non-reentrant contract and eager device order.
     * CPU/TB/DEC were already advanced. */
    pending_device_cycles = 0;
    deadline_remaining = 0;
    hal_guest_phase = 20;
    hal_dsp_tick_cycles(cycles, (uint32_t)CPU_CLOCK_HZ);
    hal_guest_phase = 21;
    hal_ai_tick_cycles(cycles, (uint32_t)CPU_CLOCK_HZ);
    hal_guest_phase = 22;
    hal_exi_tick_cycles(cycles);
    hal_guest_phase = 23;
    hal_vi_advance(cycles);
    hal_guest_phase = 0;
}

void hal_clock_device_changed(void)
{
    /* Public mutation sites synchronize the OLD configuration first. */
    deadline_remaining = 0;
}

void hal_clock_advance(uint32_t cycles)
{
    if (cycles) hal_guest_phase = 1;
    while (cycles) {
        if (!deadline_remaining) {
            hal_guest_phase = 3;
            deadline_remaining = device_deadline();
            /* A device whose event is exactly due reports zero. Advancing zero
             * cycles cannot commit it, so make one cycle of progress and let the
             * following sync deliver the event. */
            if (!deadline_remaining) deadline_remaining = 1;
            hal_guest_phase = 1;
        }
        const uint32_t step = cycles < deadline_remaining ? cycles : deadline_remaining;
        advance_clock(step);
        pending_device_cycles += step;
        deadline_remaining -= step;
        cycles -= step;
        if (!deadline_remaining) hal_clock_sync_devices();
    }
    hal_guest_phase = 0;
}

/* Only the exact scheduler idle loop recognized by the emitter may use this.
 * Its taken branch has no stores, calls, or observable progress. Stop at the
 * nearest VI/AI/DSP/EXI/DEC event, then allow the next block to dispatch its IRQ.
 * Pending deliverable IRQs always prevent a skip. */
void hal_scheduler_idle(void)
{
    hal_guest_phase = 4;
    if (gc.pc != 0x802c7ae4u || !(gc.msr & 0x8000u) ||
        gc.r[13] != 0x803d6120u || hal_mem_read32(0x003cfd58u) != 0 ||
        (hal_irq_pending() && ext_handler_ready()) ||
        (decrementer_pending && dec_handler_ready())) { hal_guest_phase = 0; return; }
    uint64_t cycles = device_deadline();
    const uint64_t dec_edge = ((uint64_t)decrementer + 1u) * TIMER_RATIO - timer_phase;
    if (cycles > dec_edge) cycles = dec_edge;
    if (cycles == UINT32_MAX) { hal_guest_phase = 0; return; } /* no bounded event to fast-forward toward */
    if (!cycles) cycles = 1; /* an event exactly due still needs one clock step */
    if (getenv("GC_THREAD_DIAG") && cycles > 1000000u) {
        static unsigned n;
        if (n++ < 40u)
            fprintf(stderr, "[idle] cycles=%llu vi=%u ai=%u dsp=%u exi=%u dec_edge=%llu pending=%u decp=%u\n",
                    (unsigned long long)cycles, hal_vi_cycles_to_event(),
                    hal_ai_cycles_to_event((uint32_t)CPU_CLOCK_HZ),
                    hal_dsp_cycles_to_event((uint32_t)CPU_CLOCK_HZ),
                    hal_exi_cycles_to_event(), (unsigned long long)dec_edge,
                    (unsigned)(pi_cause & pi_mask), decrementer_pending);
    }
    hal_clock_advance((uint32_t)cycles);
}

uint32_t hal_irq_poll(void)
{
    return hal_irq_poll_cycles(poll_cycles);
}

static uint32_t poll_pending_interrupts(void)
{
    /* An edge stays latched while EE is clear or the guest handler is not ready.
     * Delivery consumes the request; DEC keeps its current free-running value. */
    if (decrementer_pending && (gc.msr & 0x8000u) && dec_handler_ready()) {
        decrementer_pending = 0;
        gc.srr0 = gc.pc;
        gc.srr1 = gc.msr;
        gc.msr &= ~0x8000u;
        gc.pc = 0x80000900u;
        if (poll_diagnostics & POLL_DIAG_IRQ) hal_irq_diag_injected++;
        return 0x80000900u;
    }

    if (!(gc.msr & 0x8000u)) { /* msr.EE */
        if (poll_diagnostics & POLL_DIAG_IRQ) {
            static uint64_t di_eeoff;
            if ((pi_cause & pi_mask & PI_INT_DI) && di_eeoff++ < 8u)
                fprintf(stderr, "[irq-diag] DI pending while EE off #%llu pc=%08x\n",
                        (unsigned long long)di_eeoff, gc.pc);
            hal_irq_diag_ee_off++;
        }
        return 0;
    }
    /* Clock advancement has already committed every due IRQ event. Do not
     * flush sub-event device time on this per-block delivery path. */
    if (pi_cause & pi_mask) {
        if (!ext_handler_ready()) {
            if (poll_diagnostics & POLL_DIAG_IRQ) {
                static uint64_t di_nr;
                if ((pi_cause & pi_mask & PI_INT_DI) && di_nr++ < 8u)
                    fprintf(stderr, "[irq-diag] DI pending, handler not ready "
                                    "#%llu pc=%08x tbl4=%08x\n",
                            (unsigned long long)di_nr, gc.pc,
                            irq_table_entry(4));
                hal_irq_diag_notready++;
                if ((hal_irq_diag_notready & 0xFFFFFFFu) == 1u)
                    fprintf(stderr,
                            "[irq-diag] notready#1 pc=%08x tbl4=%08x tbl8=%08x\n",
                            gc.pc, irq_table_entry(4), irq_table_entry(8));
            }
            return 0;
        }
        if (poll_diagnostics & POLL_DIAG_IRQ) {
            diag_pending_line++;
            hal_irq_diag_line = diag_pending_line;
            hal_irq_diag_pending++;
        }
        return take_external();
    }
    return 0;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((cold, noinline))
#endif
static uint32_t poll_with_diagnostics(uint32_t cycles)
{
    /* Keep the observation points unchanged: FP/AX see the incoming block;
     * RAM/VI/manifest/histograms see the state after its clock budget. */
    if (poll_diagnostics & POLL_DIAG_FP) fp_trace_block();
    if (poll_diagnostics & POLL_DIAG_AX) ax_depop_trace();
    hal_irq_poll_total++;
    if (cycles) hal_clock_advance(cycles);
    if (poll_diagnostics & POLL_DIAG_RAM) ram_watch_block();
    if (poll_diagnostics & POLL_DIAG_VI) vi_trace_block(gc.pc);
    if ((poll_diagnostics & POLL_DIAG_MANIFEST) && gc_runtime_manifest_enabled()) {
        gc_runtime_manifest_note_block(gc.pc);
        gc_runtime_manifest_note_event("poll");
    }
    if (poll_diagnostics & POLL_DIAG_IRQ) {
        hal_irq_diag_msr_hist[gc.msr & 0xFu]++;
        hal_irq_diag_msr_hi8[(gc.msr >> 12) & 0xFu]++;
    }
    const uint32_t irq = poll_pending_interrupts();
    irq_refresh_fast();
    return irq;
}

uint32_t hal_irq_poll_cycles(uint32_t cycles)
{
    if (poll_diagnostics) return poll_with_diagnostics(cycles);
    hal_irq_poll_total++;
    if (cycles) {
        hal_guest_phase = 6;
        hal_clock_advance(cycles);
    }
    /* The generated entry check has cycles=0. Most blocks have neither a
     * deliverable DEC nor a masked PI source; avoid all diagnostic work here. */
    if (!(gc.msr & 0x8000u) ||
        (!decrementer_pending && !(pi_cause & pi_mask))) { hal_guest_phase = 0; return 0; }
    hal_guest_phase = 7;
    const uint32_t irq = poll_pending_interrupts();
    hal_guest_phase = 0;
    irq_refresh_fast();
    return irq;
}
