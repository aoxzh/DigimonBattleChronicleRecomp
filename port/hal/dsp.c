/* hal/dsp.c — DSP 邮箱/控制桩 + ARAM DMA 寄存器（AR_* 在 DSP 块内，0xCC0050xx）。
 * 注意：任务简报把 AR 标在 0xCC004000——实测该基址是 MemInterface（Dolphin
 * Memmap.cpp 注册表）；ARAM DMA 寄存器随 DSP 块（0xCC005020..0x2A）。已按
 * 硬件实情实现，决策记录于报告。 */
#include "dsp.h"
#include "ar.h"
#include "hal.h"
#include "irq.h"
#include "mem.h"
#include "gc_abi.h"
#ifdef GC_UPSTREAM_AUDIO
#include "ai.h"
#include "../platform/desktop/upstream_audio/upstream_audio.h"
static void dsp_update_interrupts(void);
static void upstream_dsp_irq(void *unused);
static uint32_t aid_play_addr, aid_play_length;
static uint64_t aid_budget;
static uint64_t aid_cycle_budget;
static int aid_cycle_timing;
static uint8_t upstream_aram_read(void *unused, uint32_t address)
{ (void)unused; return hal_ar_read8(address); }
#endif

#include <string.h>

static uint16_t mail_to_hi, mail_to_lo;
static uint16_t mail_from_hi, mail_from_lo;
static uint16_t dspcr;
/* DSP boot 邮件时序：__DSP_init 置 DSPReset 后立刻清空 from 邮箱（此刻真实 DSP
 * 的 boot mail 尚未到达），ARAM DMA 期间邮件才到，最后在 0x802bf15c 等 bit15。
 * HLE：reset 自清 → stage=1，跳过第一次 HI 读（清空循环读的那次），再读即投递。 */
static int dsp_boot_stage;      /* 0=未boot 1=reset后待投递 2=已投递 */
static int boot_mail_skip;
static unsigned reply_idx;      /* Zelda/AX 应答序列游标 */
static uint16_t ar_info, ar_mode, ar_refresh;
static uint32_t ar_mmaddr, ar_araddr, ar_cnt;

/* ---- AI DMA（AID；寄存器=re4 ai.c __DSPRegs[24/25/27/29]）----
 * [24]=0x5030 起址 hi（bits9:0）、[25]=0x5032 lo（&0xFFE0）、
 * [27]=0x5036 长度（(v&0x7FFF)<<5，bit15=DMA enable）、[29]=0x503A bytes-left。
 * 完成：DSPCR bit3（AID cause）置位；PI 线由电平公式（见 dsp_update_interrupts）
 * 决定；__AIDHandler 的 `DSPCR=(tmp&~0xA0)|8` 写按 W1C 清 bit3 → 电平撤线。 */
static uint32_t aid_addr, aid_length, aid_left;
static int aid_enable;
static unsigned to_mail_cnt, reply_cnt;   /* Zelda uCode 应答配平 */
static unsigned aid_start_cnt;
static unsigned param_block_cnt, frame_reply_cnt, sync_mail_cnt;
static int sync_stage;                    /* r87：0=idle 1=任务周期 2=参数块 3=已完成 */
static uint16_t sync_q[8];                /* r87：待投递 DCD1 sync 邮件队列 */
static unsigned sync_q_len, sync_q_rd;
static int sync_armed;                    /* r65：逐帧 sync 投递武装 */

void hal_dsp_init(void)
{
    hal_clock_sync_devices();
    hal_clock_device_changed();
    mail_to_hi = mail_to_lo = 0;
    mail_from_hi = mail_from_lo = 0;
    dspcr = 0;
    dsp_boot_stage = 0;
    boot_mail_skip = 0;
    reply_idx = 0;
    ar_info = 0;
    ar_mode = 1;      /* ARAM Controller init'd（Dolphin DSP.cpp Init 同值） */
    ar_refresh = 0x1A5;    /* 常用刷新率影子 */
    ar_mmaddr = ar_araddr = ar_cnt = 0;
    aid_addr = aid_length = aid_left = 0;
    aid_enable = 0;
    to_mail_cnt = reply_cnt = 0;
    aid_start_cnt = 0;
    param_block_cnt = frame_reply_cnt = sync_mail_cnt = 0;
    sync_stage = 0;
    sync_q_len = sync_q_rd = 0;
    sync_armed = 0;
#ifdef GC_UPSTREAM_AUDIO
    aid_play_addr = aid_play_length = 0;
    aid_budget = aid_cycle_budget = 0;
    aid_cycle_timing = 0;
    {
        UpstreamAudioConfig cfg = {gc_ram, GC_MEM1_SIZE, upstream_aram_read,
                                   upstream_dsp_irq, NULL};
        (void)upstream_audio_bind(&cfg);
    }
#endif
}

uint16_t hal_dsp_csr(void)
{
    hal_clock_sync_devices();
    return dspcr;
}

/* r86 静态解码（b_802ad648.c/b_802afd78.c 生成代码）：IRQ7 handler 分派树
 *   DCD10000→res_cb(task+0x28，0x802A48B8：相位状态机→优先级插入 CFBA8/A4)
 *   DCD10001→done_cb(task+0x2C，0x802A48CC：相位状态机另一半)
 *   DCD10002→CFBA4/A8 门（A4=1&A8==task→回 CDD10003+done_cb+清门）
 *   DCD10003→cb3(task+0x30，0x802A4924：B18=1+唤醒，回 CDD10002，flags=3)
 *   DCD10004→cb4(task+0x34)。主线程音频 init 自旋在 0x803CFB14==0。
 * r86 实证：r77 状态机跳过了 DCD10000→res_cb（该回调从未运行、CFBA4/A8
 * 未武装），整个任务舞步从第一封就偏离协议。r87 改为队列化序列：
 * init 块(80F3D001) → [DCD10000, DCD10002]；游戏回 CDD10003 → [DCD10003]；
 * 游戏发参数块 CDD10002+0x10 → 吸收（后续动作观察 r87 日志再定）。 */

static void dsp_update_interrupts(void);   /* 前向声明（sync_deliver_next 先用） */

/* FROM 邮箱空且已 boot 时投递队首 sync + 断言 DSPINT（IRQ7 逐封处理）。 */
static void sync_deliver_next(void)
{
    if (sync_q_len == 0 || dsp_boot_stage != 2
        || (mail_from_hi & 0x8000u) != 0)
        return;
    {
        uint16_t lo = sync_q[sync_q_rd & 7u];
        sync_q_rd++;
        sync_q_len--;
        mail_from_hi = 0xDCD1u;
        mail_from_lo = lo;
        dspcr |= 0x0080u;
        dsp_update_interrupts();
        if (sync_mail_cnt++ < 32u || sync_mail_cnt % 10000u == 0u)
            fprintf(stderr, "[dsp] sync-mail #%u DCD1%04x (q=%u)\n",
                    sync_mail_cnt, lo, sync_q_len);
    }
}

static void sync_push(uint16_t lo)
{
    if (sync_q_len < 8u)
        sync_q[(sync_q_rd + sync_q_len) & 7u] = lo;
    if (sync_q_len < 8u)
        sync_q_len++;
}

/* r65：游戏进入逐帧 sync 睡眠（0x802af9d0）时武装——下一 tick 投一封
 * DCD10003 + 断言 DSPINT（DSPCR bit7 + PI 线）唤醒 OSSaveContext 睡眠者。 */
static int sync_armed;
void hal_dsp_sync_arm(void)
{
    hal_clock_sync_devices();
    hal_clock_device_changed();
#ifdef GC_UPSTREAM_AUDIO
    return; /* HLE emits replies only for actual DSP work. */
#endif
    static unsigned sa_n;
    if (sa_n++ < 8 || sa_n % 5000u == 0u)
        fprintf(stderr, "[dsp] sync-arm #%u\n", sa_n);
    sync_armed = 1;
}

/* PI bit6 电平（Dolphin Core/HW/DSP.cpp UpdateInterrupts 权威公式）：
 *   level = (dspcr>>1) & dspcr & (DSP 0x80 | ARAM 0x20 | AID 0x08)
 * 即 cause bit7&掩码 bit8、bit5&bit6、bit3&bit4 任一对齐。SetInterrupt(false)
 * 清 PI 位——线是纯电平，无一次性 pending 状态（r89 修正：旧模型的
 * aid_pending && bit11 把 DSPInit 标志当掩码，AID 完成后线被永久重拉，
 * 派发器子解码又撞上 boot 残留的 ARAM cause bit5 → 19.3 亿次 __ARHandler）。
 * DSPINT 例外：sync 邮件 raise（bit7）无条件拉线——本游戏从不写 DSP_mask
 * bit8（r88 实证 cr 序列无 bit4/6/8），r87 握手依赖 bit7→派发器子解码
 * __DSPHandler 通路；__DSPHandler 的 W1C 写清 bit7 后线撤，无风暴。 */
static void dsp_update_interrupts(void)
{
    uint16_t pairs = (uint16_t)((dspcr >> 1) & dspcr & (0x80u | 0x20u | 0x08u));
#ifdef GC_UPSTREAM_AUDIO
    if (pairs)
#else
    if (pairs || (dspcr & 0x0080u))
#endif
        hal_pi_set_interrupt(PI_INT_DSP);
    else
        hal_pi_clear_interrupt(PI_INT_DSP);
}

#ifdef GC_UPSTREAM_AUDIO
static void upstream_dsp_irq(void *unused)
{ (void)unused; dspcr |= 0x0080u; dsp_update_interrupts(); }
/* AI DMA clock in samples, carried between VI ticks; repeating hardware DMA
 * queues a new interrupt at each block start, while consuming the latched block. */
static void upstream_aid_start(void)
{
    aid_play_addr = aid_addr;
    aid_play_length = aid_length;
    aid_left = aid_play_length;
    dspcr |= 0x0008u;
    dsp_update_interrupts();
}
#endif

uint32_t hal_dsp_cycles_to_event(uint32_t cpu_hz)
{
    hal_clock_sync_devices();
#ifdef GC_UPSTREAM_AUDIO
    if (!cpu_hz || !aid_enable || !aid_play_length) return UINT32_MAX;
    const uint64_t needed = (uint64_t)(aid_left / 4u) * cpu_hz;
    if (aid_cycle_budget >= needed) return 1;
    const uint64_t remaining = needed - aid_cycle_budget;
    const uint32_t rate = hal_ai_dma_sample_rate();
    const uint64_t cycles = remaining / rate + (remaining % rate != 0);
    return cycles > UINT32_MAX ? UINT32_MAX : (uint32_t)cycles;
#else
    (void)cpu_hz;
    return UINT32_MAX; /* legacy mailbox/DMA work runs at VI field events */
#endif
}

void hal_dsp_tick_cycles(uint32_t cycles, uint32_t cpu_hz)
{
    hal_clock_sync_devices();
    hal_clock_device_changed();
#ifdef GC_UPSTREAM_AUDIO
    if (!cpu_hz) return;
    aid_cycle_timing = 1;
    if (!aid_enable || !aid_play_length) { aid_cycle_budget = 0; return; }
    /* Units are sample-frame * CPU Hz. The scheduler stops at block completion;
     * keep every fractional sample, including a coarse caller's residual debt. */
    aid_cycle_budget += (uint64_t)cycles * hal_ai_dma_sample_rate();
    uint64_t needed = (uint64_t)(aid_left / 4u) * cpu_hz;
    if (aid_cycle_budget < needed) {
        /* A short block usually contributes only a fraction of one sample.
         * Keep that numerator and bytes-left unchanged until a frame is due. */
        if (aid_cycle_budget < cpu_hz) return;
        uint32_t frames = (uint32_t)(aid_cycle_budget / cpu_hz);
        aid_left -= frames * 4u;
        aid_cycle_budget %= cpu_hz;
        return;
    }
    aid_cycle_budget -= needed;
    aid_left = 0;
    int rc = upstream_audio_dma(aid_play_addr, aid_play_length, hal_ai_dma_sample_rate());
    if (rc < 0) fprintf(stderr, "[audio] DMA error: %s\n", upstream_audio_last_error());
    if (aid_length) upstream_aid_start();
    else aid_enable = 0;
    /* Never loop here: CPU dispatches the newly raised IRQ between blocks. */
#else
    (void)cycles; (void)cpu_hz;
#endif
}

void hal_dsp_frame_tick(void)
{
    hal_clock_sync_devices();
    hal_clock_device_changed();
#ifdef GC_UPSTREAM_AUDIO
    if (aid_cycle_timing || !aid_enable || !aid_play_length) return;
    aid_budget += (uint64_t)hal_ai_dma_sample_rate() * 4u;
    uint32_t due = (uint32_t)(aid_budget / 60u);
    aid_budget %= 60u;
    while (due && aid_enable) {
        uint32_t n = due < aid_left ? due : aid_left;
        due -= n;
        aid_left -= n;
        if (aid_left == 0) {
            int rc = upstream_audio_dma(aid_play_addr, aid_play_length,
                                        hal_ai_dma_sample_rate());
            if (rc < 0) fprintf(stderr, "[audio] DMA error: %s\n", upstream_audio_last_error());
            if (!aid_length) { aid_enable = 0; break; }
            upstream_aid_start();
        }
    }
    return;
#endif
    /* r57：Zelda 每帧 sync 的持续供给——B90>0（帧待处理）期间保持 FROM
     * 邮箱有一封未读 DCD10003，消除"投递时机 vs 游戏等待窗口"的交错敏感
     * （r55/r56 运行间方差根因）。消费方是轮询路径，静默即可。 */
    {
        uint32_t b90 = ((uint32_t)gc_ram[0x003CFB90u] << 24)
                     | ((uint32_t)gc_ram[0x003CFB91u] << 16)
                     | ((uint32_t)gc_ram[0x003CFB92u] << 8)
                     | gc_ram[0x003CFB93u];
        /* 每帧一封：B90 出现 0→1（游戏标记新帧）时投一封 DCD10003 +
         * 断言 DSPINT；消费后不补——handler 的分发循环以邮箱空为退出
         * 条件，持续补货会让它无限消费（r61 实证 93 万次）。 */
        static int sync_delivered;
        (void)sync_delivered;
        if (b90 == 0)
            sync_delivered = 0;
        /* r65：投递改由 0x802af9d0（sync 睡眠入口）经 hal_dsp_sync_arm()
         * 触发——睡眠者等的是 DSPINT 不是邮箱轮询。 */
    }
    if (sync_armed && dsp_boot_stage == 2 && (mail_from_hi & 0x8000u) == 0) {
        sync_armed = 0;
        mail_from_hi = 0xDCD1u;
        mail_from_lo = 0x0003u;
        dspcr |= 0x0080u;
        dsp_update_interrupts();
        if (reply_cnt <= 32u || reply_cnt % 10000u == 0u)
            fprintf(stderr, "[dsp] frame-sync #%u delivered\n", reply_cnt);
    }
    sync_deliver_next();                      /* r87：队列 sync 泵 */
    /* r89：参数块（CDD10002+0x0002，帧 N 混音请求）已收 → 下一 tick 回
     * DCD10003（r86 分派树：cb3→B18=1+唤醒音频线程）。r87 状态机在此静默
     * 吸收、从未应答——音频线程等不到帧同步 → AID DMA 只装弹一次不重装。 */
    if (sync_stage == 3 && dsp_boot_stage == 2
        && (mail_from_hi & 0x8000u) == 0 && sync_q_len == 0) {
        sync_stage = 4;
        sync_push(0x0003u);
        if (frame_reply_cnt++ < 32u || frame_reply_cnt % 10000u == 0u)
            fprintf(stderr, "[dsp] frame-reply #%u DCD10003 queued\n",
                    frame_reply_cnt);
        sync_deliver_next();
    }
    if (!aid_enable || aid_left == 0)
        return;
    /* 32kHz 立体声 16bit = 128 kB/s；虚拟帧 1/60 s ≈ 2133 B（640B ≈ 5ms） */
    uint32_t step = aid_left < 2133u ? aid_left : 2133u;
    aid_left -= step;
    if (aid_left == 0) {
        aid_enable = 0;                     /* DMA 完成，enable 自清 */
        dspcr |= 0x0008u;                   /* DSPCR bit3 = AID cause（W1C ack）*/
        {
            static unsigned ac_n;
            if (ac_n++ < 8 || ac_n % 4096u == 0)
                fprintf(stderr, "[dsp] aid complete#%u dspcr=%04x\n",
                        ac_n, dspcr);
        }
        dsp_update_interrupts();
    }
}

static void aram_dma_start(void)
{
    int dir = (int)((ar_cnt >> 31) & 1u);
    hal_ar_dma(ar_mmaddr, ar_araddr, ar_cnt & 0x7FFFFFFFu, dir);
    /* 完成：ARAM 中断（bit5 状态置位，按 mask6 汇聚） */
    dspcr |= 0x20u;
    dsp_update_interrupts();
}

uint16_t hal_dsp_read16(uint32_t reg)
{
    hal_clock_sync_devices();
    switch (reg) {
    case 0x00:
        /* 读 TO 邮箱 = 发送方/DSP 在查 occupied。真机 DSP 会取走邮件：
         * 轮询到 pending 即消费（清 bit15），否则 0xBEEF 垃圾哨兵（游戏
         * init 写 HI 不写 LO）会让 handler 发送等待永久活锁（r45 实证
         * 11.65 亿次轮询）。 */
        if (mail_to_hi & 0x8000u)
            mail_to_hi &= (uint16_t)~0x8000u;
        return mail_to_hi;
    case 0x02: return mail_to_lo;
    case 0x04:
#ifdef GC_UPSTREAM_AUDIO
        return upstream_audio_mail_hi();
#endif
        /* 读 HI 不弹出（occupied 位保留）；boot mail 首次呈现前跳过一次读 */
        if (dsp_boot_stage == 1) {
            if (boot_mail_skip > 0)
                boot_mail_skip--;
            else {
                dsp_boot_stage = 2;
                mail_from_hi = 0xD640u;   /* 真实 DSP 复位后的 boot 标识邮件 */
                mail_from_lo = 0x0001u;   /* __DSP_init 读走即弃，不检查内容 */
            }
        } else if (dsp_boot_stage == 2 && (gc.msr & 0x8000u)
                   && (mail_from_hi & 0x8000u) == 0) {
            /* 每帧 DSP sync（r53 实证）：0x802af9d0 的逐帧同步循环等一封
             * DCD10003（0x802afa10 轮询 FROM）。消费方是游戏轮询路径，
             * 不拉 DSP 中断（IRQ7 会进 NULL task）。静默投递。 */
            uint32_t m = 0xDCD10003u;
            reply_cnt++;
            mail_from_hi = (uint16_t)(m >> 16);
            mail_from_lo = (uint16_t)m;
            if (reply_cnt <= 16)
                fprintf(stderr, "[dsp] frame-sync #%u DCD10003\n", reply_cnt);
        }
        return mail_from_hi;
    case 0x06: {
#ifdef GC_UPSTREAM_AUDIO
        return upstream_audio_mail_lo();
#endif
        uint16_t v = mail_from_lo;
        mail_from_hi &= (uint16_t)~0x8000u;   /* 读 LO 弹出：occupied 清零 */
        sync_deliver_next();                  /* r87：队首下一封立即接续 */
        return v;
    }
    case 0x0A: {
        static unsigned rn;
        if (rn++ < 24)
            fprintf(stderr, "[dspcr-rd] #%u v=%04x pc=%08x\n", rn, dspcr, gc.pc);
        return dspcr;
    }
    case 0x12: return ar_info;
    case 0x16: return ar_mode;
    case 0x1A: return ar_refresh;
    case 0x20: return (uint16_t)(ar_mmaddr >> 16);
    case 0x22: return (uint16_t)ar_mmaddr;
    case 0x24: return (uint16_t)(ar_araddr >> 16);
    case 0x26: return (uint16_t)ar_araddr;
    case 0x28: return (uint16_t)(ar_cnt >> 16);
    case 0x2A: return (uint16_t)ar_cnt;
    case 0x30: return (uint16_t)(aid_addr >> 16);
    case 0x32: return (uint16_t)aid_addr;
    case 0x36: {
        uint16_t v = (uint16_t)((aid_length >> 5) | (aid_enable ? 0x8000u : 0u));
        static unsigned pn;
        if (pn++ < 24)
            fprintf(stderr, "[aid-poll] #%u en=%04x pc=%08x\n", pn, v, gc.pc);
        return v;
    }
    case 0x3A: {
        uint16_t v = (uint16_t)((aid_left >> 5) & 0x7FFFu);
        static unsigned qn;
        if (qn++ < 24)
            fprintf(stderr, "[aid-poll] #%u left=%04x pc=%08x\n", qn, v, gc.pc);
        return v;
    }
    default:   return 0;
    }
}

void hal_dsp_write16(uint32_t reg, uint16_t v)
{
    hal_clock_sync_devices();
    hal_clock_device_changed();
    switch (reg) {
    case 0x00: case 0x02:
        /* 邮件到 DSP：HI 置 occupied(bit15)，写 LO = 提交 → DSP 取走自清。
         * r29：游戏发送侧在 0x802af5c0 轮询该位，不清则永久自旋。 */
        if (reg == 0x00) {
            mail_to_hi = v;
        } else {
            mail_to_lo = v;
            static unsigned tm_n;
            if (tm_n++ < 32)
                fprintf(stderr, "[dsp] to-mail #%u %04x%04x\n",
                        tm_n, mail_to_hi, mail_to_lo);
            to_mail_cnt++;
#ifdef GC_UPSTREAM_AUDIO
            {
                uint32_t mail = ((uint32_t)mail_to_hi << 16) | mail_to_lo;
                mail_to_hi &= 0x7fffu;
                int rc = upstream_audio_mail(mail);
                if (rc < 0) fprintf(stderr, "[audio] mail %08x error: %s\n", mail, upstream_audio_last_error());
            }
#else
            /* r87：队列化 sync 状态机（对照 r86 解码的分派树）。
             * 阶段0：init 命令块末（0x80F3D001）→ 排队 [DCD10000(res),
             *       DCD10002(task)]——先 res_cb 武装 CFBA8/A4，再走门完成路径；
             * 阶段1：游戏分派器回 CDD10003 → 排队 [DCD10003(done)]→cb3/B18；
             * 阶段2：游戏发参数块头 CDD10002 → 吸收（0x10=字数，后续字静默）。
             * 注意：比较须在清 occupied 位之前（0xCDD1 bit15=1，掩码后恒假
             * 会被 -O2 连字符串删块，r43 教训）。 */
            if (mail_to_hi == 0x80F3u && v == 0xD001u && sync_stage == 0) {
                sync_stage = 1;
                sync_q_len = sync_q_rd = 0;
                sync_push(0x0000u);
                sync_push(0x0002u);
                fprintf(stderr, "[dsp] cycle#1 queue DCD10000(res)+DCD10002(task)\n");
                sync_deliver_next();
            } else if (mail_to_hi == 0xCDD1u && v == 0x0003u && sync_stage == 1) {
                sync_stage = 2;
                sync_push(0x0003u);
                fprintf(stderr, "[dsp] cycle#2 queue DCD10003(done)\n");
                sync_deliver_next();
            } else if (mail_to_hi == 0xCDD1u && v == 0x0002u
                       && (sync_stage == 2 || sync_stage == 4)) {
                /* CDD10002+0x0002 = 音频参数块（帧混音请求）：init 期首见、
                 * 稳态期每帧再见；帧 tick 时回 DCD10003（见 hal_dsp_frame_tick）。 */
                sync_stage = 3;
                if (param_block_cnt++ < 32u || param_block_cnt % 10000u == 0u)
                    fprintf(stderr, "[dsp] param-block #%u hdr (frame req)\n",
                            param_block_cnt);
            }
#endif
        }
        break;
    case 0x04: case 0x06:
        /* 0xBEEF = 游戏 init 的邮箱垃圾哨兵（__DSPReset 流程），不应使
         * FROM occupied 置位（会让后续等待读到假邮件）。 */
        if (reg == 0x04) {
            if (v != 0xBEEFu) mail_from_hi = v;
        } else {
            mail_from_lo = v;
        }
        break;
    case 0x0A: {
        /* DSPCR 写模型（Dolphin UDSPControl 权威位序）：
         * bit3/5/7 = cause（AID/ARAM/DSPINT）→ **W1C**（写 1 清除——三个
         *   handler 各写 1 确认自己的子中断：AID 写 (~0xA0)|8、ARAM 写
         *   (~0x88)|0x20、DSP 写 (~0x28)|0x80；r63 误把 W1C 实验失败归因于
         *   "派发器看不到标记"，实因当时还叠加了错误的线驱动）；
         * bit4/6/8 = 对应掩码、bit0/1/2/10/11 = 普通 RW（bit11=DSPInit
         *   标志不是掩码——旧模型误用，r89 风暴根因）；
         * bit9 DMAState 只由 HAL 更新（__ARWaitForDMA 轮询位）。
         * bit0 DSPReset 写 1 → 自清（等待循环退出）。 */
        uint16_t rw = (uint16_t)(0x0C07u | 0x0150u);
        uint16_t keep = (uint16_t)((dspcr & ~rw) | (v & rw));
        if (v & 0x0001u) {
            keep &= (uint16_t)~0x0001u;                /* DSPReset 自清 */
            if (dsp_boot_stage == 0) {
                dsp_boot_stage = 1;                    /* DSP 复位：boot mail 排队 */
                boot_mail_skip = 1;
            }
        }
        keep = (uint16_t)(keep & ~(v & 0x00A8u));      /* cause 位 W1C */
        dspcr = keep;
#ifdef GC_UPSTREAM_AUDIO
        if (v & 1u) upstream_audio_reset();
        if (!(v & 0x0800u)) { dspcr &= (uint16_t)~0x0400u; upstream_audio_ipl_ready(); }
        dspcr &= (uint16_t)~0x0002u; /* CPU interrupt consumed synchronously. */
#endif
        dsp_update_interrupts();
        break;
    }
    case 0x30: aid_addr = (aid_addr & 0x0000FFFFu) | ((uint32_t)(v & 0x03FFu) << 16); break;
    case 0x32: aid_addr = (aid_addr & 0x03FF0000u) | (v & 0xFFE0u); break;
    case 0x36:
#ifdef GC_UPSTREAM_AUDIO
        {
            int was_enabled = aid_enable;
            aid_length = (uint32_t)(v & 0x7fffu) << 5;
            aid_enable = (v & 0x8000u) != 0 && aid_length != 0;
            if (aid_enable && !was_enabled) { aid_budget = aid_cycle_budget = 0; upstream_aid_start(); }
            if (!aid_enable) aid_left = 0;
        }
        break;
#endif
        aid_length = (uint32_t)(v & 0x7FFFu) << 5;
        aid_enable = (v & 0x8000u) != 0;
        if (aid_enable) {
            aid_left = aid_length;   /* AIStartDMA：bytes_left = length */
            if (aid_start_cnt++ < 32u || aid_start_cnt % 10000u == 0u)
                fprintf(stderr, "[dsp] aid-dma #%u start addr=%08x len=%u\n",
                        aid_start_cnt, aid_addr, aid_length);
        }
        break;
    case 0x3A: /* bytes-left 只读语义（AIGetDMABytesLeft）；写忽略 */ break;
    case 0x12: ar_info = (uint16_t)(v & 0x007Fu); break;
    case 0x16: ar_mode = v; break;
    case 0x1A: ar_refresh = (uint16_t)(v & 0x07FFu); break;
    case 0x20: ar_mmaddr = (ar_mmaddr & 0x0000FFFFu) | ((uint32_t)(v & 0x3FFu) << 16); break;
    case 0x22: ar_mmaddr = (ar_mmaddr & 0x03FF0000u) | (v & 0xFFE0u); break;
    case 0x24: ar_araddr = (ar_araddr & 0x0000FFFFu) | ((uint32_t)(v & 0x3FFu) << 16); break;
    case 0x26: ar_araddr = (ar_araddr & 0x03FF0000u) | (v & 0xFFE0u); break;
    case 0x28: ar_cnt = (ar_cnt & 0x0000FFFFu) | ((uint32_t)(v & 0x3FFu) << 16); break;
    case 0x2A:
        ar_cnt = (ar_cnt & 0x03FF0000u) | (v & 0xFFE0u);
        aram_dma_start();   /* 写 CNT_L 触发 DMA（Dolphin DSP.cpp 同语义） */
        break;
    default: break;
    }
}
