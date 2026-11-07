/* gc_abi.h — 生成代码与 HAL 的固定契约（双方不得单方面改动，改动须回写
 * docs/architecture.md 并知会集成）。
 *
 * 提升器（tools/gekko）产出的 C 代码 include 本头；HAL 实现这些符号。
 */
#ifndef GC_ABI_H
#define GC_ABI_H

#include <stdint.h>

/* ---- CPU 上下文（唯一全局） ----
 * FPR 存储：每个寄存器有两个独立的 64 位 double 原始位模式。
 *  - fpr_bits[] 为 PS0，标量与 paired-single lane0 共用同一数值。
 *  - fpr_ps1_bits[] 为 PS1；双精度指令保留它，单精度结果与 lfs 广播。
 *  - 上下文保存必须同时保留两 lane（不能把两 float 打包成一个 double）。
 * GQR：8 个（SPR 912-919），格式与硬件一致：
 *  bits63-63? —— GC GQR: [63:32] 未用? 实际取低32: [15:12]=scale(0..31? 6bit?),
 *  规范：GQR = { [15:13] 保留? } —— 简化且正确的口径：
 *  u32 GQR: scale = bits[15:11]??? —— 权威口径（YAGCD/Broadway）：
 *  GQRx: bits 12-15 = scale (4-bit, 0..15)? Broadway: [15:12] scale、
 *  [11:8] type_dst? 不—— Broadway 手册：GQR { [15:12] scale, [11:8] l/s type }?
 *  最终以 tools/gekko 的 GQR 解码函数为准：gqr_scale(v)= (v>>12)&0xF,
 *  gqr_type(v) = (v>>16)&0x7 (2=U8? ) —— ⚠️ 以提升器内单一实现为准，
 *  HAL 不重复解码 GQR。
 */
typedef struct {
    uint32_t r[32];       /* GPR r0..r31 */
    uint64_t fpr_bits[32];    /* FPR PS0: scalar/paired lane0 double bits */
    uint64_t fpr_ps1_bits[32];/* FPR PS1: independent paired lane1 double bits */
    uint32_t cr;          /* CR 全 32 位，CRn = bits[4n+3 .. 4n] */
    uint32_t xer;         /* [0]=SO [1]=OV [2]=CA [25:31]=计数前缀 */
    uint32_t lr, ctr;
    uint32_t pc;          /* 当前块地址（仅调试/异常报告） */
    uint32_t msr;         /* 仅维护 EE(bit16) DR/IR 忽略（实模式运行） */
    uint32_t gqr[8];      /* SPR 912-919 */
    uint32_t fpscr;       /* 子集：RN[0:1]、VE/OE/ZE/XE、FPSCR[6]? 简化维护 */
    uint32_t srr0, srr1;  /* 异常保存 */
    /* 保留给运行时的字段 */
    uint64_t icount;      /* 已执行指令数（诊断） */
} GcCtx;

extern GcCtx gc;

/* ---- 生成块接口（提升器实现） ---- */
typedef void (*GcBlockFn)(void);
/* 由 recomp/blocks.c 提供：地址→块；找不到返回 NULL */
GcBlockFn gc_block_lookup(uint32_t ea);
void gc_entry(void);            /* DOL entry 的块 */
/* 由运行时 glue 实现：间接跳转/未知目标统一走这里 */
void gc_dispatch(uint32_t target_ea);
void gc_unimplemented(const char *what, uint32_t ea);
void gc_trap(uint32_t ea);      /* tw/twi 命中 */

/* ---- bounded dynamic runtime manifest (opt-in) ----
 * Disabled unless GC_RUNTIME_MANIFEST is set to a non-zero value. The
 * fixed-size tables keep the hot path bounded; generated files do not call
 * this API directly. */
void gc_runtime_manifest_reset(void);
int  gc_runtime_manifest_enabled(void);
void gc_runtime_manifest_note_block(uint32_t pc);
void gc_runtime_manifest_note_event(const char *name);
void gc_runtime_manifest_note_mmio(uint32_t phys, unsigned width, int is_write);
int  gc_runtime_manifest_dump(const char *path);
int  gc_runtime_manifest_dump_default(void);

/* ---- HAL 提供给生成代码的钩子 ---- */
uint32_t hal_irq_poll(void);    /* 返回后 ctx.pc 可能被改为中断向量；返回1表示已注入 */
uint8_t  hal_mmio_read8(uint32_t phys);
uint16_t hal_mmio_read16(uint32_t phys);
uint32_t hal_mmio_read32(uint32_t phys);
void hal_mmio_write8(uint32_t phys, uint8_t v);
void hal_mmio_write16(uint32_t phys, uint16_t v);
void hal_mmio_write32(uint32_t phys, uint32_t v);

/* WGPIPE（0xCC008000，GX FIFO）写口 —— mmio.c 与 gen_prefix 直连。
 * 宽度助手按 PPC 大端拆字节，与逐字节写逐位等价（GPFifo 收任意宽度写）。 */
void hal_gxpipe_write8(uint8_t v);
void hal_gxpipe_write16(uint16_t v);
void hal_gxpipe_write32(uint32_t v);
void hal_gxpipe_write64(uint64_t v);

/* 慢路径内存访问（提升器对 phys>=0x01800000 的访问调用；含 MMIO 与空洞） */
uint8_t  hal_slow_read8(uint32_t phys);
uint16_t hal_slow_read16(uint32_t phys);
uint32_t hal_slow_read32(uint32_t phys);
uint64_t hal_slow_read64(uint32_t phys);
void hal_slow_write8(uint32_t phys, uint8_t v);
void hal_slow_write16(uint32_t phys, uint16_t v);
void hal_slow_write32(uint32_t phys, uint32_t v);
void hal_slow_write64(uint32_t phys, uint64_t v);

/* 伪指令辅助（提升器可选用，运行时实现） */
uint32_t gc_bswap32(uint32_t v);
uint16_t gc_bswap16(uint16_t v);

/* dcbz：按 32 字节缓存行清零（phys） */
void hal_dcbz(uint32_t phys);
/* lwarx/stwcx 保留站 */
extern uint32_t gc_reserve_valid;      /* 0=无保留；定义在 runtime_glue.c */
extern uint32_t gc_reserve_addr;

#endif /* GC_ABI_H */
