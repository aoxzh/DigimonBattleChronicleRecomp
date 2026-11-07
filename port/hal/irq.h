/* hal/irq.h — PI 中断汇聚 + IRQ 注入管线。
 * PI 寄存器偏移与中断线位序出处：
 *   Dolphin Source/Core/Core/HW/ProcessorInterface.h（PI_INTERRUPT_CAUSE=0x00、
 *   PI_INTERRUPT_MASK=0x04、PI_FIFO_BASE=0x0C、PI_FIFO_END=0x10、PI_FIFO_WPTR=0x14、
 *   PI_FIFO_RESET=0x18；INT_CAUSE_DI=0x4、INT_CAUSE_SI=0x8、INT_CAUSE_EXI=0x10、
 *   INT_CAUSE_AI=0x20、INT_CAUSE_DSP=0x40、INT_CAUSE_VI=0x100、
 *   INT_CAUSE_PE_TOKEN=0x200、INT_CAUSE_PE_FINISH=0x400、INT_CAUSE_CP=0x800）
 *   https://raw.githubusercontent.com/dolphin-emu/dolphin/master/Source/Core/Core/HW/ProcessorInterface.h
 */
#ifndef HAL_IRQ_H
#define HAL_IRQ_H

#include <stdint.h>

/* IRQ 注入点（提升器在每个函数入口与向后分支目标处调用）。
 * 返回 0 = 无中断；返回 0x80000500 = 外部中断，0x80000900 = DEC 中断：
 *   srr0 = 注入点块地址（重执行语义：handler rfi 回到该块重执行当前指令）
 *   srr1 = 注入前 msr
 *   msr.EE 清零（取中断硬件语义），pc = 返回的向量
 * 生成代码约定：uint32_t v; if ((v = hal_irq_poll())) return gc_dispatch(v);
 */
uint32_t hal_irq_poll(void);
/* Explicit boundary query: zero checks/delivers pending IRQs without time.
 * Optional per-block diagnostics are selected at hal_irq_init, after setting
 * their environment variables. GC_FP_TRACE/GC_AX_DEPOP_TRACE retain exact "1"
 * semantics; GC_VI_TRACE and GC_RUNTIME_MANIFEST retain their boolean grammar;
 * GC_RAM_WATCH retains its address:length grammar. GC_IRQ_DIAG=1 opts into the
 * former always-on IRQ counters, MSR histograms and not-ready/EE-off logging.
 * These statistics are zero until enabled (and retain their process lifetime
 * once collected); an ABRT dump requires this flag for IRQ diagnostic data.
 * Public poll/frame/cycle counters and all guest IRQ behavior remain always on.
 * FP/AX observations precede clock advancement; all other poll probes follow it.
 * Configure flags before initialization, not during an active guest run. */
uint32_t hal_irq_poll_cycles(uint32_t cycles);
/* Fast-path state for generated blocks: one volatile load when nothing is
 * deliverable, instead of a call through the full poll/diagnostics path. */
extern volatile uint32_t hal_irq_fast_pending;
extern uint64_t hal_irq_poll_total;
/* Commit CPU/TB/DEC immediately. Device work accumulates only until the next
 * VI/AI/DSP/EXI event; due IRQs are visible at the next generated boundary. */
void hal_clock_advance(uint32_t cycles);
/* Make device state current without adding CPU time. All device public reads
 * use this; mutators call it BEFORE touching their old configuration, then
 * invalidate the cached deadline through device_changed before any return.
 * Raw device ticks are invoked with no pending debt, so nested queries are safe.
 * Standalone device tests without irq.c provide explicit no-op mocks. */
void hal_clock_sync_devices(void);
void hal_clock_device_changed(void);
/* Wait until the nearest VI/AI/DSP/EXI/DEC event, only for the verified
 * scheduler idle PC/empty queue/EE; never skip a deliverable pending IRQ. */
void hal_scheduler_idle(void);

/* Public PI cause & mask query, synchronizing device visibility first. */
uint32_t hal_irq_pending(void);

/* One virtual CPU clock at nominal 486 MHz. Generated blocks check IRQs with
 * hal_irq_poll_cycles(0), then commit executed original instructions through
 * hal_clock_advance before their terminator. One cycle/instruction is an explicit
 * approximation, not Gekko pipeline/cache latency. Legacy hal_irq_poll advances
 * its configured budget. TB and DEC tick every 12 CPU cycles (40.5 MHz),
 * preserving fractional phase across polls and DEC writes. VI derives scan,
 * field and compare deadlines from its registers (see vi.h). All advances
 * split at device events so callbacks observe their actual CPU/TB timestamp.
 * hal_irq_init resets clock, DEC/shadow/pending, poll size and frame callback;
 * configure poll size/callback after boot initialization. Guest icount remains
 * an instruction diagnostic, independent of this clock. TB writes are not modeled. */
void     hal_irq_init(void);
void     hal_set_poll_cycles(uint32_t cycles);   /* default 256; zero selects 1 */
uint64_t hal_cpu_cycles(void);
uint64_t hal_timebase_ticks(void);               /* lifted TBL/TBU reads */
uint64_t hal_poll_count(void);
uint64_t hal_frame_count(void);

/* All guest mtspr22 writes must use this hook. It owns gc_spr_misc[22], which
 * remains readable as the current DEC. A nonnegative -> negative write latches
 * a request; no write clears a pending request or resets the CPU/12 phase.
 * Requests survive EE=0 and handler-not-ready until delivered by hal_irq_poll. */
void hal_decrementer_write(uint32_t value);

/* 帧回调：帧事件发生时（hal_frame_tick / 时间自然到期）回调（host 取帧时机） */
typedef void (*hal_frame_cb)(void *user);
void hal_set_frame_callback(hal_frame_cb cb, void *user);

#endif /* HAL_IRQ_H */
