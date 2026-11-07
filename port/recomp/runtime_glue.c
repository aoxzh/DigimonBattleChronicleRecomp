/* recomp/runtime_glue.c — 生成代码与 HAL 之间的运行时胶水。
 * 契约：hal/gc_abi.h（符号签名固定）。本文件提供：
 *   - GcCtx gc 唯一实例
 *   - gc_dispatch：lookup → 调用；NULL 打印 "MISS pc=%08x" 并 abort
 *   - gc_block_lookup / gc_entry：弱符号空表（提升器 recomp/generated/ 覆盖）
 *   - gc_unimplemented / gc_trap / gc_bswap16 / gc_bswap32
 *   - lwarx/stwcx 保留站（gc_reserve_valid / gc_reserve_addr）
 */
#include "../hal/gc_abi.h"
#include "../hal/irq.h"
extern uint8_t gc_ram[0x01800000];

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- CPU 上下文唯一实例 ---- */
GcCtx gc;

/* ---- lwarx/stwcx 保留站 ---- */
uint32_t gc_reserve_valid;
uint32_t gc_reserve_addr;

/* ---- 弱符号空块表：提升器产出 recomp/blocks.c 时被强符号覆盖 ---- */
__attribute__((weak)) GcBlockFn gc_block_lookup(uint32_t ea)
{
    (void)ea;
    return (GcBlockFn)0;
}

/* 弱入口：无生成代码时打印诊断后 abort（有生成代码则被覆盖，正常进入游戏） */
__attribute__((weak)) void gc_entry(void)
{
    fprintf(stderr,
            "gc_entry: no generated blocks (recomp/generated not linked). "
            "MISS pc=%08x\n", gc.pc ? gc.pc : 0x80003100u);
    abort();
}

uint32_t gc_prev;   /* 上一块地址（诊断） */
static uint32_t trace_ring[8192];
static unsigned trace_n;
void gc_trace_note(uint32_t a, uint32_t lr)
{
    trace_ring[trace_n % 8192] = a; trace_ring[(trace_n+1) % 8192] = lr;
    trace_n += 2;
}

/* ---- bounded dynamic runtime manifest ----
 * The manifest is opt-in. Fixed-size tables keep recording bounded and the
 * generated banks remain untouched. */
#define GC_MANIFEST_BLOCK_CAP 16384u
#define GC_MANIFEST_EVENT_CAP 32u
#define GC_MANIFEST_MMIO_CAP 512u
#define GC_MANIFEST_NAME_CAP 24u

typedef struct { uint32_t pc; uint64_t count; } GcManifestBlock;
typedef struct { char name[GC_MANIFEST_NAME_CAP]; uint64_t count; } GcManifestEvent;
typedef struct { uint32_t phys; uint8_t width, is_write; uint64_t count; } GcManifestMmio;

static int gc_manifest_on = -1;
static GcManifestBlock gc_manifest_blocks[GC_MANIFEST_BLOCK_CAP];
static GcManifestEvent gc_manifest_events[GC_MANIFEST_EVENT_CAP];
static GcManifestMmio gc_manifest_mmio[GC_MANIFEST_MMIO_CAP];
static unsigned gc_manifest_block_n, gc_manifest_event_n, gc_manifest_mmio_n;

int gc_runtime_manifest_enabled(void)
{
    if (gc_manifest_on < 0) {
        const char *e = getenv("GC_RUNTIME_MANIFEST");
        gc_manifest_on = e && *e && *e != '0';
    }
    return gc_manifest_on;
}

void gc_runtime_manifest_reset(void)
{
    gc_manifest_block_n = gc_manifest_event_n = gc_manifest_mmio_n = 0;
    memset(gc_manifest_blocks, 0, sizeof(gc_manifest_blocks));
    memset(gc_manifest_events, 0, sizeof(gc_manifest_events));
    memset(gc_manifest_mmio, 0, sizeof(gc_manifest_mmio));
}

void gc_runtime_manifest_note_block(uint32_t pc)
{
    if (!gc_runtime_manifest_enabled()) return;
    for (unsigned i = 0; i < gc_manifest_block_n; i++)
        if (gc_manifest_blocks[i].pc == pc) { gc_manifest_blocks[i].count++; return; }
    if (gc_manifest_block_n < GC_MANIFEST_BLOCK_CAP) {
        gc_manifest_blocks[gc_manifest_block_n].pc = pc;
        gc_manifest_blocks[gc_manifest_block_n++].count = 1;
    }
}

void gc_runtime_manifest_note_event(const char *name)
{
    if (!gc_runtime_manifest_enabled() || !name) return;
    for (unsigned i = 0; i < gc_manifest_event_n; i++)
        if (strncmp(gc_manifest_events[i].name, name, GC_MANIFEST_NAME_CAP) == 0) {
            gc_manifest_events[i].count++; return;
        }
    if (gc_manifest_event_n < GC_MANIFEST_EVENT_CAP) {
        GcManifestEvent *e = &gc_manifest_events[gc_manifest_event_n++];
        strncpy(e->name, name, GC_MANIFEST_NAME_CAP - 1u);
        e->name[GC_MANIFEST_NAME_CAP - 1u] = '\0';
        e->count = 1;
    }
}

void gc_runtime_manifest_note_mmio(uint32_t phys, unsigned width, int is_write)
{
    if (!gc_runtime_manifest_enabled()) return;
    for (unsigned i = 0; i < gc_manifest_mmio_n; i++) {
        GcManifestMmio *m = &gc_manifest_mmio[i];
        if (m->phys == phys && m->width == width && m->is_write == (uint8_t)(is_write != 0)) {
            m->count++; return;
        }
    }
    if (gc_manifest_mmio_n < GC_MANIFEST_MMIO_CAP) {
        GcManifestMmio *m = &gc_manifest_mmio[gc_manifest_mmio_n++];
        m->phys = phys; m->width = (uint8_t)width;
        m->is_write = (uint8_t)(is_write != 0); m->count = 1;
    }
}

static void gc_manifest_json_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') fputc('\\', f);
        fputc((unsigned char)*s, f);
    }
    fputc('"', f);
}

int gc_runtime_manifest_dump(const char *path)
{
    if (!gc_runtime_manifest_enabled() || !path || !*path) return 0;
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "{\"version\":1,\"icount\":%llu,\"polls\":%llu,\"frames\":%llu,\"blocks\":[",
            (unsigned long long)gc.icount,
            (unsigned long long)hal_poll_count(),
            (unsigned long long)hal_frame_count());
    for (unsigned i = 0; i < gc_manifest_block_n; i++) {
        if (i) fputc(',', f);
        fprintf(f, "{\"pc\":\"%08x\",\"count\":%llu}",
                gc_manifest_blocks[i].pc,
                (unsigned long long)gc_manifest_blocks[i].count);
    }
    fprintf(f, "],\"events\":[");
    for (unsigned i = 0; i < gc_manifest_event_n; i++) {
        if (i) fputc(',', f);
        fputc('{', f); gc_manifest_json_string(f, "name"); fputc(':', f);
        gc_manifest_json_string(f, gc_manifest_events[i].name);
        fprintf(f, ",\"count\":%llu}",
                (unsigned long long)gc_manifest_events[i].count);
    }
    fprintf(f, "],\"mmio\":[");
    for (unsigned i = 0; i < gc_manifest_mmio_n; i++) {
        if (i) fputc(',', f);
        fprintf(f, "{\"phys\":\"%08x\",\"width\":%u,\"write\":%s,\"count\":%llu}",
                gc_manifest_mmio[i].phys, gc_manifest_mmio[i].width,
                gc_manifest_mmio[i].is_write ? "true" : "false",
                (unsigned long long)gc_manifest_mmio[i].count);
    }
    fprintf(f, "]}\n");
    fclose(f);
    return 1;
}

int gc_runtime_manifest_dump_default(void)
{
    const char *path = getenv("GC_RUNTIME_MANIFEST_PATH");
    return gc_runtime_manifest_dump(path && *path ? path : "build/runtime_manifest.json");
}

/* ---- malloc 调查插桩 ----
 * 游戏 memdbg 包装（0x8028f120）→ dlmalloc（入口 0x8028dd70，epilogue 0x8028e298，
 * bins 基址 *(0x8039c6b4)）。OOM panic 块 0x80281d2c（r4=请求大小）。
 * 目的：判别 24MB 堆真实耗尽 / 中断重入损坏 free-list / 请求大小异常。 */
static int gc_mtrace_on = -1;
static unsigned gc_malloc_depth;
static uint32_t gc_malloc_req, gc_malloc_lr;
uint32_t *gc_snap_buf;
static uint32_t gc_probe_req;
static unsigned gc_lua_walk_n;   /* 0x8026c318 碰撞链连续步数（0x8026c328 退出复位） */
typedef struct { uint32_t size, lr, ret; } GcMallocRecord;
static GcMallocRecord gc_malloc_hist[16];
static unsigned gc_malloc_hist_n;
static uint32_t gc_rd32(uint32_t va)
{
    uint32_t p = va & 0x1FFFFFFFu;
    if (p + 4 > 0x01800000u) return 0xDEADBEEFu;
    return ((uint32_t)gc_ram[p] << 24) | ((uint32_t)gc_ram[p+1] << 16)
         | ((uint32_t)gc_ram[p+2] << 8) | gc_ram[p+3];
}
static void gc_hook_block(uint32_t ea)
{
    if (gc_mtrace_on < 0) {
        const char *e = getenv("GC_MALLOC_TRACE");
        gc_mtrace_on = e && *e && *e != '0';
    }
    if (ea == 0x8028f120u) {            /* memdbg alloc 包装入口: r3=size */
        if (gc_mtrace_on)
            fprintf(stderr, "[malloc] wrap size=%u lr=%08x depth=%u\n",
                    gc.r[3], gc.lr, gc_malloc_depth);
        return;
    }
    if (ea == 0x8028dd70u) {            /* dlmalloc 入口: r3=req */
        gc_malloc_depth++;
        gc_malloc_req = gc.r[3];
        gc_malloc_lr = gc.lr;
        if (gc_malloc_depth > 1)
            fprintf(stderr, "[malloc] REENTRANT depth=%u req=%u lr=%08x prev-lr=%08x\n",
                    gc_malloc_depth, gc.r[3], gc.lr, gc.r[0]);
        else if (gc_mtrace_on)
            fprintf(stderr, "[malloc] enter req=%u lr=%08x\n", gc.r[3], gc.lr);
        return;
    }
    if (ea == 0x8028e298u) {            /* dlmalloc epilogue: r3=结果 */
        if (gc_malloc_depth) gc_malloc_depth--;
        GcMallocRecord *h =
            &gc_malloc_hist[gc_malloc_hist_n++ % 16];
        h->size = gc_malloc_req; h->lr = gc_malloc_lr; h->ret = gc.r[3];
        if (gc.r[3] == 0) {
            /* dlmalloc 返回 NULL：小尺寸可能是探测，>=1024 详细 dump */
            static unsigned null_n;
            if (null_n++ < 10)
                fprintf(stderr, "[malloc] NULL req=%u lr=%08x (null#%u)\n",
                        gc_malloc_req, gc_malloc_lr, null_n);
            if (gc_malloc_req >= 1024u) {
                unsigned k;
                fprintf(stderr,
                        "[oom] dlmalloc NULL req=%u lr=%08x depth=%u\n"
                        "[oom] poolhead@8039c674:",
                        gc_malloc_req, gc_malloc_lr, gc_malloc_depth);
                for (k = 0; k < 24; k++)
                    fprintf(stderr, " %08x", gc_rd32(0x8039c674u + 4u * k));
                fprintf(stderr, "\n[oom] bins@8039c6b4:");
                for (k = 0; k < 16; k++)
                    fprintf(stderr, " %08x", gc_rd32(0x8039c6b4u + 4u * k));
                fprintf(stderr, "\n");
            }
        }
        if (gc_mtrace_on)
            fprintf(stderr, "[malloc] exit ret=%08x\n", gc.r[3]);
        return;
    }
    if (ea == 0x80281d2cu || ea == 0x80281d74u) {   /* OOM panic 路径 */
        unsigned k, n = gc_malloc_hist_n > 16 ? 16 : gc_malloc_hist_n;
        fprintf(stderr,
                "[oom] PANIC size=%u depth=%u lr=%08x\n",
                gc.r[4], gc_malloc_depth, gc.lr);
        fprintf(stderr, "[oom] last %u dlmalloc calls:\n", n);
        for (k = 0; k < n; k++) {
            GcMallocRecord *e =
                &gc_malloc_hist[(gc_malloc_hist_n - n + k) % 16];
            fprintf(stderr, "[oom]   size=%-8u lr=%08x ret=%08x\n",
                    e->size, e->lr, e->ret);
        }
        { FILE *tf = fopen("oom_dump.txt", "w");
          if (tf) {
              for (uint32_t va = 0x8039c600u; va < 0x8039c780u; va += 4)
                  fprintf(tf, "%08x %08x\n", va, gc_rd32(va));
              fclose(tf);
          } }
    }
}

/* Observation only: match selected weighted-RNG interrupts to the completed
 * OSLoadContext rfi. Keep raw bits (no floating-point operations in the probe),
 * never restore guest state, and bound the entire run to 32 entry/resume pairs. */
#define GC_FP_TRACE_CAP 32u
typedef struct {
    uint32_t active, pc, context, sp, msr, fpscr;
    uint64_t fpr[32], ps1[32];
    uint32_t gqr[8];
} GcFpIrqTrace;
static GcFpIrqTrace gc_fp_irq_trace[GC_FP_TRACE_CAP];
static unsigned gc_fp_irq_trace_n;
static int gc_fp_trace_on = -1;

static int gc_fp_trace_enabled(void)
{
    if (gc_fp_trace_on < 0) {
        const char *e = getenv("GC_FP_TRACE");
        gc_fp_trace_on = e && strcmp(e, "1") == 0;
    }
    return gc_fp_trace_on;
}

static void gc_fp_trace_irq_entry(uint32_t context, uint32_t vector)
{
    if (!gc_fp_trace_enabled() || gc_fp_irq_trace_n == GC_FP_TRACE_CAP
        || (gc.srr0 != 0x8001d874u && gc.srr0 != 0x8001d884u
            && gc.srr0 != 0x8001d89cu)) return;
    unsigned index = gc_fp_irq_trace_n++;
    GcFpIrqTrace *t = &gc_fp_irq_trace[index];
    t->active = 1;
    t->pc = gc.srr0;
    t->context = context;
    t->sp = gc.r[1];
    t->msr = gc.srr1;
    t->fpscr = gc.fpscr;
    memcpy(t->fpr, gc.fpr_bits, sizeof(t->fpr));
    memcpy(t->ps1, gc.fpr_ps1_bits, sizeof(t->ps1));
    memcpy(t->gqr, gc.gqr, sizeof(t->gqr));
    fprintf(stderr, "[fp-irq] phase=entry id=%u vector=%08x pc=%08x ctx_phys=%08x d4=%08x owner_d8=%08x thread_e4=%08x sp=%08x msr=%08x fpscr=%08x state=%04x index=%08x weight_ptr=%08x f0=%016llx f1=%016llx f13=%016llx gqr0=%08x gqr7=%08x%s\n",
            index + 1u, vector, t->pc, context, gc_rd32(0xD4u),
            gc_rd32(0xD8u), gc_rd32(0xE4u), t->sp, t->msr, t->fpscr,
            gc_rd32(context + 0x1A0u) & 0xFFFFu, gc.r[30], gc.r[9],
            (unsigned long long)t->fpr[0], (unsigned long long)t->fpr[1],
            (unsigned long long)t->fpr[13], t->gqr[0], t->gqr[7],
            gc_fp_irq_trace_n == GC_FP_TRACE_CAP ? " entry-cap=32" : "");
}

static void gc_fp_trace_irq_resume(uint32_t target_ea)
{
    if (!gc_fp_trace_enabled() || !gc_fp_irq_trace_n
        || (gc.pc != 0x802c010cu && gc.pc != 0x802c0110u)) return;
    uint32_t context = gc_rd32(0xD4u) & 0x1FFFFFFFu;
    for (unsigned i = gc_fp_irq_trace_n; i > 0; --i) {
        GcFpIrqTrace *t = &gc_fp_irq_trace[i - 1u];
        if (!t->active || t->pc != target_ea || t->context != context
            || t->sp != gc.r[1]) continue;
        uint32_t fpr_diff = 0, gqr_diff = 0;
        for (unsigned r = 0; r < 32; ++r)
            if (t->fpr[r] != gc.fpr_bits[r]
                || t->ps1[r] != gc.fpr_ps1_bits[r]) fpr_diff |= 1u << r;
        for (unsigned r = 0; r < 8; ++r)
            if (t->gqr[r] != gc.gqr[r]) gqr_diff |= 1u << r;
        t->active = 0;
        fprintf(stderr, "[fp-irq] phase=resume id=%u from=%08x pc=%08x ctx_phys=%08x d4=%08x owner_d8=%08x thread_e4=%08x sp=%08x msr=%08x fpscr=%08x fpscr_changed=%u state=%04x index=%08x weight_ptr=%08x fpr_diff=%08x gqr_diff=%02x f0=%016llx f1=%016llx f13=%016llx gqr0=%08x gqr7=%08x\n",
                i, gc.pc, target_ea, context, gc_rd32(0xD4u),
                gc_rd32(0xD8u), gc_rd32(0xE4u), gc.r[1], gc.msr,
                gc.fpscr, t->fpscr != gc.fpscr,
                gc_rd32(context + 0x1A0u) & 0xFFFFu, gc.r[30], gc.r[9],
                fpr_diff, gqr_diff,
                (unsigned long long)gc.fpr_bits[0],
                (unsigned long long)gc.fpr_bits[1],
                (unsigned long long)gc.fpr_bits[13], gc.gqr[0], gc.gqr[7]);
        break;
    }
}

/* The lifted CPU does not raise FP-unavailable exceptions, so SDK lazy FPU
 * ownership cannot protect interrupted FP registers. Preserve the host's raw
 * representation eagerly across an outstanding IRQ, without executing FP ops
 * or modifying guest FP ownership/handler control flow.
 *
 * Frames are keyed by physical OSContext and returned PC/SP. Different contexts
 * may remain suspended; the newest frame for one context is its nesting top.
 * Consume frames on return: retaining a last-return cache would overwrite FP
 * work performed by a thread after that return. This intentionally does not
 * implement cooperative OSSaveContext/lazy-FPU switching or context destruction.
 * Reusing an OSContext while its IRQ remains outstanding is unsupported; the
 * guest must unwind it first. Capacity exhaustion aborts instead of corrupting
 * a suspended context. */
#define GC_FP_CONTEXT_CAP 64u
typedef struct {
    uint64_t order;
    uint32_t context, pc, sp, fpscr;
    uint64_t fpr[32], ps1[32];
    uint32_t gqr[8];
} GcFpContextFrame;
static GcFpContextFrame gc_fp_context_frames[GC_FP_CONTEXT_CAP];
static uint64_t gc_fp_context_order;

static void gc_fp_context_save_irq(uint32_t context)
{
    if ((context & 3u) || context > sizeof(gc_ram) - 0x2C8u) {
        fprintf(stderr, "[fp-context] invalid IRQ context=%08x pc=%08x\n",
                context, gc.srr0);
        abort();
    }
    for (unsigned i = 0; i < GC_FP_CONTEXT_CAP; ++i) {
        GcFpContextFrame *f = &gc_fp_context_frames[i];
        if (f->order) continue;
        if (gc_fp_context_order == UINT64_MAX) break;
        f->order = ++gc_fp_context_order;
        f->context = context;
        f->pc = gc.srr0;
        f->sp = gc.r[1];
        f->fpscr = gc.fpscr;
        memcpy(f->fpr, gc.fpr_bits, sizeof(f->fpr));
        memcpy(f->ps1, gc.fpr_ps1_bits, sizeof(f->ps1));
        memcpy(f->gqr, gc.gqr, sizeof(f->gqr));
        return;
    }
    fprintf(stderr, "[fp-context] outstanding IRQ capacity=%u exhausted ctx=%08x pc=%08x\n",
            GC_FP_CONTEXT_CAP, context, gc.srr0);
    abort();
}

static void gc_fp_context_resume_irq(uint32_t target_ea)
{
    /* These two generated blocks end in the actual OSLoadContext rfi.
     * A normal branch to the same guest PC must not consume an IRQ frame. */
    if (gc.pc != 0x802c010cu && gc.pc != 0x802c0110u) return;
    uint32_t context = gc_rd32(0xD4u) & 0x1FFFFFFFu;
    GcFpContextFrame *top = NULL;
    for (unsigned i = 0; i < GC_FP_CONTEXT_CAP; ++i) {
        GcFpContextFrame *f = &gc_fp_context_frames[i];
        if (f->order && f->context == context
            && (!top || f->order > top->order)) top = f;
    }
    if (!top || top->pc != target_ea || top->sp != gc.r[1]) return;
    memcpy(gc.fpr_bits, top->fpr, sizeof(top->fpr));
    memcpy(gc.fpr_ps1_bits, top->ps1, sizeof(top->ps1));
    memcpy(gc.gqr, top->gqr, sizeof(top->gqr));
    gc.fpscr = top->fpscr;
    top->order = 0;
}

/* ---- bounded static-block dispatch cache ----
 * The generated block table and its function pointers are immutable for the
 * process lifetime. Like gc itself, this cache belongs to the single guest CPU
 * thread; reset of guest RAM/CPU state does not invalidate compiled functions.
 * Cache the full effective address, not a canonicalized alias: lookup tries the
 * exact address first. Fold higher instruction-address bits into the index so
 * nearby functions separated by a cache-sized span do not all collide.
 */
#define GC_DISPATCH_CACHE_CAP 16384u
static struct {
    uint32_t ea;
    GcBlockFn fn;
} gc_dispatch_cache[GC_DISPATCH_CACHE_CAP];

static GcBlockFn gc_dispatch_lookup(uint32_t ea)
{
    uint32_t slot = ((ea >> 2) ^ (ea >> 14)) & (GC_DISPATCH_CACHE_CAP - 1u);
    if (gc_dispatch_cache[slot].ea == ea && gc_dispatch_cache[slot].fn)
        return gc_dispatch_cache[slot].fn;
    GcBlockFn fn = gc_block_lookup(ea);
    if (fn) {
        gc_dispatch_cache[slot].ea = ea;
        gc_dispatch_cache[slot].fn = fn;
    }
    return fn;
}

void gc_dispatch(uint32_t target_ea)
{
    /* Log the uncorrected return state before applying the eager IRQ shadow. */
    gc_fp_trace_irq_resume(target_ea);
    gc_fp_context_resume_irq(target_ea);
    /* SDK 向量 stub HLE：0x500 的 stub 保存上下文、调试器检查后执行
     * SRR0 = *(0x802C3000); rfi。stub 是运行时拷贝的内存代码（无 lift 块），
     * 这里直接跳槽内的真 handler（.text 内已 lift），其尾部 rfi 回到 srr0。 */
    if (target_ea == 0x80000500u || target_ea == 0x80000900u) {
        /* 表索引 = (向量>>8)-1：0x500→4（外部中断）、0x900→8（decrementer） */
        uint32_t exc_no = ((target_ea >> 8) & 0xFFu) - 1u;
        uint32_t tbl_pa = (0x3000u + 4u * exc_no) & 0x1FFFFFFFu;
        /* SDK 向量链 HLE：stub（运行时内存代码，无 lift 块）把完整现场存进
         * OSContext（*(0xC0)），查 handler 表[4]=*(0x3010)，SRR0=handler 后 rfi。
         * 这里一步到位：写现场 → 直接进 handler；handler 尾部恢复+rfi 回 srr0。 */
        uint32_t ctx = ((uint32_t)gc_ram[0xC0] << 24) | ((uint32_t)gc_ram[0xC1] << 16)
                     | ((uint32_t)gc_ram[0xC2] << 8) | gc_ram[0xC3];
        uint32_t h = ((uint32_t)gc_ram[tbl_pa] << 24) | ((uint32_t)gc_ram[tbl_pa+1] << 16)
                   | ((uint32_t)gc_ram[tbl_pa+2] << 8) | gc_ram[tbl_pa+3];
        { static int once;
          if (!once++) fprintf(stderr, "ee-check: ctx=%08x tbl4=%08x\n", ctx, h); }
        if (ctx & 0x80000000u) ctx &= 0x1FFFFFFFu;   /* OSInit 早期指针是物理形态 */
        if (!(ctx < 0x01800000u && (ctx & 3) == 0
              && h >= 0x80003100u && h < 0x81800000u)) {
            /* 一次性诊断：哪个向量、表内容为何不合法 */
            static int bad_once;
            if (!bad_once++) {
                fprintf(stderr, "ee-hle-invalid: exc=%u ctx=%08x h=%08x tbl:",
                        exc_no, ctx, h);
                for (int k = 0; k < 16; k++)
                    fprintf(stderr, " %08x",
                            ((uint32_t)gc_ram[0x3000u + 4u*k] << 24)
                            | ((uint32_t)gc_ram[0x3001u + 4u*k] << 16)
                            | ((uint32_t)gc_ram[0x3002u + 4u*k] << 8)
                            | gc_ram[0x3003u + 4u*k]);
                fprintf(stderr, "\n");
            }
        }
        if (ctx < 0x01800000u && (ctx & 3) == 0
            && h >= 0x80003100u && h < 0x81800000u) {
            uint32_t cp = ctx;
            int k;
            gc_fp_trace_irq_entry(cp, target_ea);
            gc_fp_context_save_irq(cp);
            for (k = 0; k < 32; k++) {
                uint32_t v = gc.r[k];
                gc_ram[cp + 4*k]     = (uint8_t)(v >> 24);
                gc_ram[cp + 4*k + 1] = (uint8_t)(v >> 16);
                gc_ram[cp + 4*k + 2] = (uint8_t)(v >> 8);
                gc_ram[cp + 4*k + 3] = (uint8_t)v;
            }
            #define ST32(off, v) do { uint32_t _v = (v); \
                gc_ram[cp+(off)] = (uint8_t)(_v>>24); gc_ram[cp+(off)+1] = (uint8_t)(_v>>16); \
                gc_ram[cp+(off)+2] = (uint8_t)(_v>>8); gc_ram[cp+(off)+3] = (uint8_t)_v; } while (0)
            ST32(0x80, gc.cr);
            ST32(0x84, gc.lr);
            ST32(0x88, gc.ctr);
            ST32(0x8C, gc.xer);
            ST32(0x198, gc.srr0);   /* OSContext.SRR0/@0x198（GPR[n]=4n 布局） */
            ST32(0x19C, gc.srr1);
            /* SDK OSContext stores GQR1..7; GQR0 is retained in the host
             * IRQ shadow, not written over the guest's PSF area at +0x1C8. */
            for (k = 1; k < 8; k++)
                ST32(0x1A8 + 4*(k - 1), gc.gqr[k]);
            #undef ST32
            gc_ram[cp + 0x1A3] |= 0x02u;    /* 状态 |= 2（stub 同款） */
            /* 真实 stub（tp OS.c __OSEVStart）末段：lwz r4, 0xD4 —— handler
             * 拿虚拟形态当前 context；0xC0 里的物理指针只用于保存现场。
             * 传物理指针会让 __VIRetraceHandler 把 0xD4 恢复成物理形态，
             * 调度器 802c79e4 的 D4≠E4 判等永假 → 拒调度 → EE 永不恢复。 */
            gc.r[4] = ((uint32_t)gc_ram[0xD4] << 24) | ((uint32_t)gc_ram[0xD5] << 16)
                    | ((uint32_t)gc_ram[0xD6] << 8) | gc_ram[0xD7];
            gc.r[3] = exc_no;  /* 异常号 */
            { static int once;
              if (!once++) fprintf(stderr, "ee-hle: exc=%u ctx=%08x handler=%08x\n",
                                   exc_no, ctx, h); }
            target_ea = h;
        }
    }
    /* Resolve vector HLE above on every dispatch, even when its final block is cached. */
    GcBlockFn fn = gc_dispatch_lookup(target_ea);
    if (!fn) {
        fprintf(stderr, "MISS pc=%08x (from %08x -> %08x, lr=%08x ctr=%08x)\n",
                target_ea, gc_prev, gc.pc, gc.lr, gc.ctr);
        {
            const char *path = getenv("GC_RAM_DUMP");
            if (path && *path) {
                FILE *dump = fopen(path, "wb");
                size_t written = 0;
                if (dump) {
                    written = fwrite(gc_ram, 1, sizeof(gc_ram), dump);
                    fclose(dump);
                }
                fprintf(stderr, "MISS RAM dump %s bytes=%zu\n", path, written);
            }
            fprintf(stderr, "MISS registers:");
            for (unsigned i = 0; i < 32; ++i)
                fprintf(stderr, " r%u=%08x", i, gc.r[i]);
            fprintf(stderr, "\n");
        }
        {   /* 全 RAM 扫描：运行时填充的函数表/vtable 指向的文本地址里，
             * 凡是块表尚未覆盖的都打印出来，供 tools/extra_roots.txt 批量回填。 */
            unsigned found = 0;
            fprintf(stderr, "MISSROOTS begin\n");
            for (uint32_t p = 0; p + 4 <= 0x01800000u; p += 4) {
                uint32_t v = ((uint32_t)gc_ram[p] << 24) | ((uint32_t)gc_ram[p+1] << 16)
                           | ((uint32_t)gc_ram[p+2] << 8) | gc_ram[p+3];
                if (v >= 0x80003100u && v < 0x802e13e0u && (v & 3u) == 0
                    && !gc_block_lookup(v)) {
                    fprintf(stderr, "MISSROOT %08x\n", v);
                    if (++found >= 4000) { fprintf(stderr, "MISSROOTS capped\n"); break; }
                }
            }
            fprintf(stderr, "MISSROOTS end (%u)\n", found);
        }
        { FILE *tf = fopen("trace_last.txt", "w");
          unsigned start = trace_n >= 8192 ? trace_n - 8192 : 0;
          for (unsigned k = start; k < trace_n; k += 2)
            fprintf(tf, "%08x %08x\n", trace_ring[k % 8192], trace_ring[(k+1) % 8192]);
          if (tf) fclose(tf);
          fprintf(stderr, "trace_last.txt written (%u entries)\n", (trace_n - start) / 2); }
        abort();
    }
    gc_trace_note(gc.pc, gc.lr);
    gc_hook_block(target_ea);
    gc_prev = gc.pc;
    gc.pc = target_ea;
    fn();
}

/* ---- 生成期插桩：--hook-blocks 指定的块入口调用这里 ----
 * 生成代码块间是直接 C 调用，不经过 gc_dispatch，运行时 hook 看不到；
 * OOM 调查必须在块体里埋点。打印寄存器 + 游戏 r1 回溯链。 */
static unsigned gc_probe_quiet;
static unsigned gc_probe_9f;   /* 0x80297fc8 memset 循环专用计数 */
/* bi2 区（0x81700000）写监视：音频状态结构 0x8170000C 被谁污染 */
static int gc_probe_on = -1;
int gc_probe_enabled(void)
{
    if (gc_probe_on < 0) {
        const char *e = getenv("GC_PROBE");
        gc_probe_on = e && *e && *e != '0';
    }
    return gc_probe_on;
}
void gc_watch_note(uint32_t p, uint32_t v)
{
    static unsigned n;
    if (!gc_probe_enabled() || n++ >= 500000u)
        return;
    if (p >= 0x003C4538u && p < 0x003C46C0u) {
        if (v == 0 && (gc.pc < 0x802de400u || gc.pc >= 0x802de800u))
            return;
        fprintf(stderr, "[watch-vi-config] phys=%08x ea=%08x value=%08x pc=%08x lr=%08x\n",
                p, p + 0x80000000u, v, gc.pc, gc.lr);
    } else {
        fprintf(stderr, "[watch] w32 %08x = %08x (pc=%08x lr=%08x)\n",
                p + 0x80000000u, v, gc.pc, gc.lr);
    }
}

void gc_probe_block(uint32_t pc)
{
    unsigned k;
    if (pc == 0x8028f120u) {            /* memdbg 包装入口：记录请求大小 */
        gc_probe_req = gc.r[3];
    }
    if (pc == 0x8028dd70u) {            /* dlmalloc 入口：malloc(size) 或 free(ptr) */
        static unsigned mal_n;
        if (gc_probe_enabled() && mal_n++ < 30000u)
            fprintf(stderr, "[mal-in] %s arg=%08x lr=%08x\n",
                    gc.r[3] >= 0x80003100u ? "free " : "malloc",
                    gc.r[3], gc.lr);
    }
    if (pc == 0x8028e298u && gc.r[3] != 0) {
        /* dlmalloc 出口（成功路径）：r3=结果指针，配对 memdbg 记录的请求大小 */
        static unsigned mal_out_n;
        if (gc_probe_enabled() && mal_out_n++ < 30000u)
            fprintf(stderr, "[mal-out] ret=%08x req=%u lr=%08x\n",
                    gc.r[3], gc_probe_req, gc.lr);
    }
    if (pc == 0x8028e298u && gc.r[3] == 0) {
        /* 干预实验：dlmalloc 失败时从备用 bump 区供给（GC_BUMP_ALLOC=1 开启） */
        static int bump_on = -1;
        if (bump_on < 0) {
            const char *e = getenv("GC_BUMP_ALLOC");
            bump_on = e && *e && *e != '0';
        }
        if (bump_on) {
            static uint32_t bump;
            if (!bump) bump = 0x81600000u;
            uint32_t sz = (gc_probe_req + 0x1Fu) & ~0x1Fu;
            if (bump + sz < 0x81700000u) {
                gc.r[3] = bump;
                bump += sz;
            }
        }
        gc_probe_req = 0;
    }
    if (!gc_probe_enabled()) return;
    if (pc == 0x802dcf78u) {   /* VI retrace 回调入口：执行计数 */
        static unsigned vi_cb_n;
        vi_cb_n++;
        if (vi_cb_n <= 40 || vi_cb_n % 2000u == 0)
            fprintf(stderr, "[vi-cb] #%u pc=802dcf78 counter=%08x\n",
                    vi_cb_n, gc_rd32(0x803CFE94u));
        return;
    }
    if (pc == 0x8026bbfcu) {   /* lua rehash 入口：计数前的表状态 */
        static unsigned rh_n;
        uint32_t t = gc.r[3];
        uint32_t node = gc_rd32(t + 0x10u);
        uint32_t lsz = gc_ram[(t - 0x80000000u) + 7u];
        uint32_t n = 1u << (lsz & 31u);
        unsigned nonzero = 0;
        if (node >= 0x80000000u && node < 0x81800000u && n <= 256u) {
            for (uint32_t i = 0; i < n; i++) {
                uint32_t nd = node + i * 0x14u;
                if (gc_rd32(nd + 8u) != 0u) nonzero++;
            }
        }
        if (rh_n++ < 30u)
            fprintf(stderr, "[rehash] t=%08x node=%08x lsiz=%u sizenode=%u "
                            "nonnil=%u arr=%08x szarr=%u\n",
                    t, node, lsz, n, nonzero, gc_rd32(t + 0xCu),
                    gc_rd32(t + 0x18u));
        return;
    }
    if (pc == 0x8026bea0u) {   /* setnodevector(L,t,nasize,nhsize) 入口 */
        static unsigned sn_n;
        if (sn_n++ < 30u)
            fprintf(stderr, "[setnv] t=%08x nasize=%d nhsize=%d "
                            "cur.node=%08x cur.lsiz=%u\n",
                    gc.r[4], gc.r[5], gc.r[6], gc_rd32(gc.r[4] + 0x10u),
                    gc_ram[(gc.r[4] - 0x80000000u) + 7u]);
        return;
    }
    if (pc == 0x8026bfc4u) {   /* setnodevector 分支点：mtcrf 0x80,r24 后 bne cr0 */
        static unsigned bp_n;
        if (bp_n++ < 20u)
            fprintf(stderr, "[bfc4] nhsize(r27)=%d r24=%08x cr0restored=%x "
                            "EQ=%u cr=%08x\n",
                    gc.r[27], gc.r[24], (gc.r[24] >> 28) & 0xFu,
                    (gc.cr >> 29) & 1u, gc.cr);
        return;
    }
    if (pc == 0x8026bfccu) {   /* dummynode 路径被选中 */
        static unsigned dn_n;
        if (dn_n++ < 10u)
            fprintf(stderr, "[bfcc] DUMMYNODE path taken: t=%08x nhsize=%d\n",
                    gc.r[28], gc.r[27]);
        return;
    }
    if (pc == 0x8026bfe0u) {   /* alloc 路径被选中 */
        static unsigned al_n;
        if (al_n++ < 10u)
            fprintf(stderr, "[bfe0] alloc path taken: t=%08x nhsize=%d "
                            "nodes=%d\n", gc.r[28], gc.r[27], gc.r[31]);
        return;
    }
    if (pc == 0x8020ee24u) {   /* 构造函数入口：this 来自哪个调用者、栈相对位置 */
        static unsigned ct_n;
        if (ct_n++ < 12u)
            fprintf(stderr, "[ctor] this=%08x sp=%08x lr=%08x %s\n",
                    gc.r[3], gc.r[1], gc.lr,
                    gc.lr == 0x801ff578u ? "(caller A 801ff570)"
                    : gc.lr == 0x8020a3b4u ? "(caller B 8020a3ac)" : "");
        return;
    }
    if (pc == 0x8020eea0u) {   /* 构造函数尾声：即将 blr，LR 应为调用点 */
        static unsigned ce_n;
        if (ce_n++ < 12u)
            fprintf(stderr, "[ctor-end] sp=%08x lr=%08x\n", gc.r[1], gc.lr);
        return;
    }
    if (pc == 0x8020a31cu) {   /* 阴影对象数组构造：param_2 资源描述符头 */
        static unsigned sh_n;
        if (sh_n++ < 8u) {
            uint32_t p2 = gc.r[4];
            fprintf(stderr, "[shadow] p1=%08x p2=%08x +28=%08x +2c=%08x "
                            "+30=%08x +1c=%08x +16=%04x\n",
                    gc.r[3], p2, gc_rd32(p2 + 0x28u), gc_rd32(p2 + 0x2Cu),
                    gc_rd32(p2 + 0x30u), gc_rd32(p2 + 0x1Cu),
                    (unsigned)((gc_rd32(p2 + 0x14u) >> 16) & 0xFFFFu));
        }
        return;
    }
    if (pc == 0x802132acu) {   /* 资源查找：r13-0x68e0 包链表状态 */
        static unsigned rs_n;
        static int rs_scan_done;
        if (rs_n++ < 8u) {
            uint32_t head = gc_rd32(gc.r[13] - 0x68e0u);
            uint32_t first = head ? gc_rd32(head) : 0;
            unsigned n = 0;
            uint32_t p = first;
            while (p && p != head && n < 64u) { n++; p = gc_rd32(p); }
            fprintf(stderr, "[resq] name=%08x type=%08x list=%08x first=%08x "
                            "packs=%u\n", gc.r[3], gc.r[4], head, first, n);
            if (gc.r[3] == 0x80305e84u && !rs_scan_done) {
                /* 全 RAM 找 "fadeout"：字符串在已加载数据里=匹配器问题；
                 * 完全不存在=包含它的包没加载 */
                static const char pat[] = "fadeout";
                unsigned hits = 0;
                for (uint32_t p = 0; p + sizeof(pat) <= 0x01800000u && hits < 5u;
                     p += 4) {
                    unsigned ok = 1;
                    for (unsigned k = 0; k < sizeof(pat) - 1u; k++)
                        if (gc_ram[p + k] != (uint8_t)pat[k]) { ok = 0; break; }
                    if (ok) {
                        fprintf(stderr, "[resq] \"fadeout\" @ phys %08x (ea %08x): "
                                        "%.32s\n", p, p + 0x80000000u,
                                (const char *)&gc_ram[p]);
                        hits++;
                    }
                }
                if (!hits) fprintf(stderr, "[resq] \"fadeout\" not in RAM\n");
                {   /* 扫已加载数据里的 .tif 文件名（包目录应含纯名） */
                    unsigned tf = 0;
                    for (uint32_t p = 0; p + 8 <= 0x01800000u && tf < 40u;
                         p += 1) {
                        if (gc_ram[p] == '.' && gc_ram[p + 1] == 't'
                            && gc_ram[p + 2] == 'i' && gc_ram[p + 3] == 'f'
                            && gc_ram[p + 4] == 0) {
                            uint32_t s = p;
                            while (s > 0 && gc_ram[s - 1] >= 0x20
                                   && gc_ram[s - 1] < 0x7F && p - s < 64u)
                                s--;
                            fprintf(stderr, "[resq] tif@%08x: %.*s\n",
                                    s + 0x80000000u,
                                    (int)(p + 4 - s > 63 ? 63 : p + 4 - s),
                                    (const char *)&gc_ram[s]);
                            tf++;
                            p += 4;
                        }
                    }
                    if (!tf) fprintf(stderr, "[resq] no .tif strings in RAM\n");
                }
                rs_scan_done = 1;
            }
        }
        return;
    }
    if (pc == 0x802125d0u) {   /* "Load of %s failed" 报告器：包加载失败证据 */
        static unsigned lf_n;
        if (lf_n++ < 10u) {
            uint32_t obj = gc.r[3];
            fprintf(stderr, "[loadfail] obj=%08x vtbl=%08x arg2=%08x objdump:",
                    obj, obj ? gc_rd32(obj) : 0, gc.r[4]);
            for (unsigned k = 0; k < 0x20u; k++)
                fputc((obj >= 0x80000000u && obj < 0x81800000u)
                      ? (gc_ram[(obj - 0x80000000u) + k] >= 0x20
                         && gc_ram[(obj - 0x80000000u) + k] < 0x7F)
                        ? gc_ram[(obj - 0x80000000u) + k] : '.' : '?',
                      stderr);
            fprintf(stderr, "\n");
        }
        return;
    }
    if (pc == 0x802bc90cu) {   /* DVD 驱动 RequestError 步骤 */
        static unsigned dv_n;
        if (dv_n++ < 10u)
            fprintf(stderr, "[dvd] reset-reqerr #%u cb=%08x\n", dv_n, gc.r[5]);
        return;
    }
    if (pc == 0x802b8540u) {   /* 状态机下一步 */
        static unsigned nx_n;
        if (nx_n++ < 10u)
            fprintf(stderr, "[dvd] next-step(802b8540) #%u\n", nx_n);
        return;
    }
    if (pc == 0x802bbffcu) {   /* 超时 alarm 回调 */
        static unsigned to_n;
        if (to_n++ < 10u)
            fprintf(stderr, "[dvd] TIMEOUT callback #%u\n", to_n);
        return;
    }
    if (pc == 0x8026c328u) {   /* Lua 碰撞链正常退出：复位步数 */
        gc_lua_walk_n = 0;
        return;
    }
    if (pc == 0x8026c318u) {   /* Lua luaH_newkey 碰撞链遍历：正常走 1~3 步，
                                * 连续 >64 步说明 next 链断裂（堆损坏现场） */
        unsigned lua_walk_n = ++gc_lua_walk_n;
        if (++lua_walk_n > 64u) {
            uint32_t cur = gc.r[3], tgt = gc.r[30];
            uint32_t n = cur;
            fprintf(stderr, "[lua-walk] BROKEN chain: steps=%u cursor=%08x "
                            "target=%08x r31=%08x r28=%08x r4=%08x\n",
                    lua_walk_n, cur, tgt, gc.r[31], gc.r[28], gc.r[4]);
            for (unsigned s = 0; s < 48u; s++) {
                fprintf(stderr, "[lua-walk] node=%08x k=%08x,%08x v=%08x,%08x next=%08x\n",
                        n, gc_rd32(n), gc_rd32(n + 4u), gc_rd32(n + 8u),
                        gc_rd32(n + 0xcu), gc_rd32(n + 0x10u));
                n = gc_rd32(n + 0x10u);
                if (n == tgt) { fprintf(stderr, "[lua-walk] chain closes\n"); break; }
                if (!n) { fprintf(stderr, "[lua-walk] chain ends at NULL\n"); break; }
            }
            /* 表头候选：Lua5.0 Table = CommonHeader(8)+flags/lsizenode(2)+metatable@0xC
             * +array@0x10 +node@0x14 +sizearray@0x18 +firstfree@0x1C */
            {
                static const int cand[] = { 4, 7, 10, 28, 30, 31 };
                for (unsigned c = 0; c < sizeof(cand) / sizeof(cand[0]); c++) {
                    uint32_t t = gc.r[cand[c]];
                    if (t >= 0x80000000u && t < 0x81800000u) {
                        fprintf(stderr, "[lua-walk] cand r%d=%08x node@0x14=%08x "
                                        "sizearray=%08x lsz=%02x\n",
                                cand[c], t, gc_rd32(t + 0x14u), gc_rd32(t + 0x18u),
                                gc_ram[(t - 0x80000000u) + 9u]);
                    }
                }
            }
            abort();
        }
        return;
    }
    if (pc == 0x802c7ae4u) {   /* 调度器空闲自旋：周期性 dump 全线程等待队列 */
        static unsigned idle_n;
        if (gc_probe_enabled() && (idle_n++ % 40000u) == 1u) {
            /* linkActive(0x2FC) 链枚举所有线程：state=be16(+2C8)
             * attr=be16(+2CA) prio(+2D0) suspend(+2CC) queue(+2DC)
             * 停靠SRR0(+198)。参照 re4 OSThread.h；旧写法 RD32&0xffff
             * 读到的是 attr 半字，曾把 WAITING(4) 误报成 READY(1)。 */
            uint32_t t = 0x803C3150u;
            fprintf(stderr, "[idle] RunQBits=%08x gate=%08x\n",
                    gc_rd32(0x803CFD58u), gc_rd32(0x803CFD5Cu));
            for (unsigned k = 0; k < 12; k++) {
                if (t < 0x80000000u || t >= 0x81800000u) break;
                uint32_t pa = (t - 0x80000000u) + 0x2C8u;
                uint32_t st = ((uint32_t)gc_ram[pa] << 8) | gc_ram[pa + 1u];
                uint32_t q = gc_rd32(t + 0x2DCu);
                fprintf(stderr, "[idle] th=%08x state=%u attr=%u prio=%u susp=%d q=%08x",
                        t, st, ((uint32_t)gc_ram[pa + 2u] << 8) | gc_ram[pa + 3u],
                        gc_rd32(t + 0x2D0u),
                        (int)gc_rd32(t + 0x2CCu), q);
                if (q >= 0x80000000u && q < 0x81800000u)
                    fprintf(stderr, " (h=%08x t=%08x)",
                            gc_rd32(q), gc_rd32(q + 4u));
                fprintf(stderr, " srr0=%08x\n", gc_rd32(t + 0x198u));
                t = gc_rd32(t + 0x2FCu);
                if (t == 0x803C3150u) break;
            }
        }
        return;
    }
    if (pc == 0x802a49fcu) {   /* 音频检查点自旋现场：flag 到底是不是 >0 */
        static unsigned cp_n;
        cp_n++;
        if (cp_n <= 20u || cp_n % 20000u == 0u) {
            uint32_t flag = gc_rd32(0x803CFB14u);
            uint32_t e4 = gc_rd32(0xE4u);
            { extern uint16_t hal_dsp_csr(void);
              uint32_t taskp = gc_rd32(0x803CFBB8u);
              fprintf(stderr,
                      "[ckpt] #%u flag=%08x cnt(B90)=%08x e4=%08x lr=%08x dspcr=%04x task=%08x flags8=%08x\n",
                      cp_n, flag, gc_rd32(0x803CFB90u), e4, gc.lr,
                      hal_dsp_csr(), taskp,
                      (taskp & 0x80000000u) ? gc_rd32((taskp & 0x1FFFFFFFu) + 8u) : 0); }
            /* r86：任务队列与回调槽位全景（B14 信号链定位） */
            {
                uint32_t tp = gc_rd32(0x803CFBB8u);
                uint32_t base = tp & 0x1FFFFFFFu;
                fprintf(stderr,
                        "[ckpt-q] B18=%08x tail=%08x A4=%08x A8=%08x "
                        "cb28=%08x cb2c=%08x cb30=%08x cb34=%08x "
                        "ax7a8=%08x ax7ac=%08x ax7b0=%08x\n",
                        gc_rd32(0x803CFB18u), gc_rd32(0x803CFB94u),
                        gc_rd32(0x803CFBA4u), gc_rd32(0x803CFBA8u),
                        (tp & 0x80000000u) ? gc_rd32(base + 0x28u) : 0,
                        (tp & 0x80000000u) ? gc_rd32(base + 0x2Cu) : 0,
                        (tp & 0x80000000u) ? gc_rd32(base + 0x30u) : 0,
                        (tp & 0x80000000u) ? gc_rd32(base + 0x34u) : 0,
                        gc_rd32(0x803AA840u + 0x7A8u),
                        gc_rd32(0x803AA840u + 0x7ACu),
                        gc_rd32(0x803AA840u + 0x7B0u));
            }
        }
        return;
    }
    /* r86：DSP 任务分派树。已解码（生成代码 b_802ad648.c/b_802afd78.c）：
     * DCD10000→res_cb(+0x28)、DCD10001→done_cb(+0x2C)、DCD10002→CFBA4/A8 门、
     * DCD10003→cb3(+0x30)→flags=3→唤醒；完成尾部 0x802af6c0 置 B90=1。
     * 主线程自旋条件=0x803CFB14==0（帧完成标志，0x80251740 thunk 经
     * 0x80250ea4 同步变量机制置位）；r81"自旋条件=B90"为误读。 */
    if (pc == 0x802afa20u) {   /* handler 读到邮件：r3=邮件值，r5=task */
        static unsigned n;
        n++;
        if (n <= 24u || n % 2000u == 0u) {
            uint32_t task = gc_rd32(0x803CFBB8u);
            uint32_t base = task & 0x1FFFFFFFu;
            fprintf(stderr,
                    "[tree] mail=%08x task=%08x tflags=%08x cb28=%08x "
                    "cb2c=%08x cb30=%08x A4=%08x A8=%08x ee=%d\n",
                    gc.r[3], task,
                    (task & 0x80000000u) ? gc_rd32(base + 8u) : 0,
                    (task & 0x80000000u) ? gc_rd32(base + 0x28u) : 0,
                    (task & 0x80000000u) ? gc_rd32(base + 0x2Cu) : 0,
                    (task & 0x80000000u) ? gc_rd32(base + 0x30u) : 0,
                    gc_rd32(0x803CFBA4u), gc_rd32(0x803CFBA8u),
                    (gc.msr & 0x8000u) ? 1 : 0);
        }
        return;
    }
    if (pc == 0x802afa7cu || pc == 0x802afaa0u || pc == 0x802afb94u
        || pc == 0x802afcacu || pc == 0x802afc38u || pc == 0x802afd50u) {
        /* 回调分发点：af8b8→+0x28(res) aa0/fb94→+0x2C(done)
         * cac/c38/d50→+0x30(cb3)。r5=task。 */
        static unsigned n;
        n++;
        if (n <= 24u || n % 2000u == 0u) {
            uint32_t task = gc.r[5];
            uint32_t off = (pc == 0x802afa7cu) ? 0x28u
                         : (pc == 0x802afaa0u || pc == 0x802afb94u) ? 0x2Cu
                                                                    : 0x30u;
            fprintf(stderr, "[cb] pc=%08x task=%08x cb=%08x B90=%08x "
                            "B14=%08x B18=%08x\n",
                    pc, task,
                    (task & 0x80000000u) ? gc_rd32((task & 0x1FFFFFFFu) + off) : 0,
                    gc_rd32(0x803CFB90u), gc_rd32(0x803CFB14u),
                    gc_rd32(0x803CFB18u));
        }
        return;
    }
    if (pc == 0x802a4924u) {   /* B18=1 + 唤醒（AX 回调，task+0x7B0 注册值） */
        static unsigned n;
        n++;
        if (n <= 16u)
            fprintf(stderr, "[b18cb] #%u lr=%08x\n", n, gc.lr);
        return;
    }
    if (pc == 0x80251740u) {   /* B14 同步变量 thunk（0x80250ea4 机制） */
        static unsigned n;
        n++;
        if (n <= 16u)
            fprintf(stderr, "[b14thunk] #%u r3=%08x r4=%08x lr=%08x\n",
                    n, gc.r[3], gc.r[4], gc.lr);
        return;
    }
    if (pc == 0x802a4894u) {   /* 相位回调→优先级插入 0x802af8b0(0x803AA840) */
        static unsigned n;
        n++;
        if (n <= 16u)
            fprintf(stderr, "[prio-ins] #%u task=%08x head=%08x state=%08x\n",
                    n, gc.r[3], gc_rd32(0x803CFBB8u), gc_rd32(0x803CFB04u));
        return;
    }
    if (pc == 0x802a48ccu) {   /* done 相位回调（0x803CFB04 状态机） */
        static unsigned n;
        n++;
        if (n <= 16u)
            fprintf(stderr, "[phase-done] #%u state=%08x\n", n,
                    gc_rd32(0x803CFB04u));
        return;
    }
    if (pc == 0x802af698u) {   /* 帧工作分支：0x802bd954(r3)（B90!=1 时） */
        static unsigned n;
        n++;
        if (n <= 16u || n % 2000u == 0u)
            fprintf(stderr, "[framework] #%u r3=%08x ee=%d B90=%08x\n",
                    n, gc.r[3], (gc.msr & 0x8000u) ? 1 : 0,
                    gc_rd32(0x803CFB90u));
        return;
    }
    if (pc == 0x802af6c0u) {   /* 帧完成尾部：DSPCR 收尾+清队列+B90→1 */
        static unsigned n;
        n++;
        if (n <= 16u)
            fprintf(stderr, "[done-tail] #%u task=%08x A4=%08x A8=%08x\n",
                    n, gc_rd32(0x803CFBB8u), gc_rd32(0x803CFBA4u),
                    gc_rd32(0x803CFBA8u));
        return;
    }
    /* r66-r78：0x802af9d0（IRQ7 handler 入口）处的 sync-arm 已证自续
     * 循环（handler 消费→重投→再消费 80.6 万次），已撤除；真实等待点
     * 待 r79 定位。 */
    if (pc == 0x802af9a0u || pc == 0x802af68cu || pc == 0x802af978u) {
        static unsigned fp_n;
        fp_n++;
        if (fp_n <= 30u || fp_n % 2000u == 0u)
            fprintf(stderr, "[frame-path] #%u pc=%08x lr=%08x ee=%d B90=%08x\n",
                    fp_n, pc, gc.lr, (gc.msr & 0x8000u) ? 1 : 0,
                    gc_rd32(0x803CFB90u));
        return;
    }
    if (pc == 0x802af5c0u || pc == 0x802af5d0u || pc == 0x802af5f4u
        || pc == 0x802af60cu) {   /* DSP mail 助手：c0=等TO空 d0=等FROM有
                                     f4=读FROM 60c=发TO。lr 定位等待者 */
        static unsigned mh_n;
        mh_n++;
        if (mh_n <= 300u || mh_n % 500u == 0u)
            fprintf(stderr,
                    "[mail] #%u pc=%08x lr=%08x ee=%d tohi=%04x frhi=%04x\n",
                    mh_n, pc, gc.lr, (gc.msr & 0x8000u) ? 1 : 0,
                    gc_rd32(0x0C005000u) & 0xFFFFu,
                    gc_rd32(0x0C005004u) & 0xFFFFu);
        return;
    }
    if (pc == 0x802c8778u) {   /* OSWakeupThread 入口：r3=队列指针 */
        static unsigned wk_n;
        wk_n++;
        if (wk_n <= 3000u || wk_n % 500u == 0)
            fprintf(stderr,
                    "[wake] #%u r3=%08x h=%08x t=%08x RunQ=%08x%08x cur(E4)=%08x\n",
                    wk_n, gc.r[3],
                    gc.r[3] >= 0x80000000u ? gc_rd32((gc.r[3] & 0x1FFFFFFFu)) : 0,
                    gc.r[3] >= 0x80000000u ? gc_rd32((gc.r[3] & 0x1FFFFFFFu) + 4u) : 0,
                    gc_rd32(0x803CFD58u), gc_rd32(0x803CFD5Cu), gc_rd32(0xE4u));
        return;
    }
    if (pc == 0x802ddb44u) {   /* VIWaitForRetrace 循环检查：r30 vs 实时 count */
        static unsigned chk_n;
        if (chk_n++ < 30)
            fprintf(stderr, "[vi-chk] #%u r30=%08x count=%08x RunQBits=%08x%08x\n",
                    chk_n, gc.r[30], gc_rd32(0x803CFE94u),
                    gc_rd32(0x803CFD58u), gc_rd32(0x803CFD5Cu));
        if (chk_n <= 30 && (chk_n == 1u || chk_n % 10u == 0u)) {
            /* 调度器全局字演化：0xC0=异常 ctx、0xD4=当前 ctx、0xD8=主线程、
             * 0xDC=退出队列、0xE4=当前线程 —— 802c79e4 用 D4 vs E4 判等 */
            fprintf(stderr, "[vi-chk] sched C0=%08x D4=%08x D8=%08x DC=%08x E4=%08x\n",
                    gc_rd32(0xC0u), gc_rd32(0xD4u), gc_rd32(0xD8u),
                    gc_rd32(0xDCu), gc_rd32(0xE4u));
        }
        return;
    }
    if (pc == 0x802dd028u) {   /* retraceCount++ 执行点（lwz/stw r13-0x628C
                                * = 0x803CFE94，与 VIWaitForRetrace 轮询同址） */
        static unsigned inc_n;
        inc_n++;
        if (inc_n <= 20 || inc_n % 5000u == 0)
            fprintf(stderr, "[retrace++] #%u th(E4)=%08x count=%08x\n",
                    inc_n, gc_rd32(0xE4u), gc_rd32(0x803CFE94u));
        return;
    }
    if (pc == 0x802dd6d8u) {   /* 计数器初始化写点 */
        static unsigned init_n;
        if (init_n++ < 4)
            fprintf(stderr, "[vi-init] write counter <- %08x (r31)\n", gc.r[31]);
        return;
    }
    if (pc == 0x802ddb1cu) {   /* VIWaitForRetrace 入口 */
        static unsigned w_n;
        if (w_n++ < 8)
            fprintf(stderr, "[vi-wait] #%u entry (caller lr=%08x)\n", w_n, gc.lr);
        return;
    }
    if (pc == 0x802cab24u) {
        static unsigned spin_n;
        if (spin_n++ < 10)
            fprintf(stderr, "[ai-spin] r30=%08x r3=%08x r0=%08x AICR=%04x%04x\n",
                    gc.r[30], gc.r[3], gc.r[0],
                    hal_mmio_read16(0x0C006C08u), hal_mmio_read16(0x0C006C0Au));
        return;
    }
    if (pc == 0x802befa4u) {
        static unsigned ld_n;
        if (ld_n++ < 10)
            fprintf(stderr, "[stream-pos] *803CF328=%08x\n", gc.r[3]);
        return;
    }
    if (pc == 0x802bd490u) {
        static unsigned sp_n;
        if (sp_n++ < 10) {
            uint32_t g = gc_rd32(0x003CC594u);
            fprintf(stderr, "[audio-stream] glob@803CC594=%08x r13glob-reg=%08x\n",
                    g, gc.r[3]);
        }
        return;
    }
    if (pc == 0x802bd49cu) {
        static unsigned st_n;
        if (st_n++ < 12)
            fprintf(stderr, "[audio-st] r3=%08x *r3=%08x\n",
                    gc.r[3], gc_rd32(gc.r[3] & 0x1FFFFFFFu));
        return;
    }
    if (pc == 0x802cab24u) {
        static unsigned spin_n;
        if (spin_n++ < 10)
            fprintf(stderr, "[dsp-spin] r30=%08x *r30=%08x r3=%08x r31=%08x\n",
                    gc.r[30], gc_rd32(gc.r[30] & 0x1FFFFFFFu), gc.r[3], gc.r[31]);
        return;
    }
    if (pc == 0x80297fc8u) {
        if (gc_probe_9f++ < 12) {
            fprintf(stderr, "[probe] memset-loop r3=%08x r4=%08x r5=%08x r31=%08x lr=%08x\n",
                    gc.r[3], gc.r[4], gc.r[5], gc.r[31], gc.lr);
            if (gc.r[3] == 0x68726f6eu) {   /* 垃圾参数专用：一次性深度取证 */
                uint32_t sp2 = gc.r[1];
                fprintf(stderr, "[junk] === forensics r3=%08x r4=%08x r5=%08x ===\n",
                        gc.r[3], gc.r[4], gc.r[5]);
                for (unsigned j = 0; j < 20 && sp2 >= 0x80000000u && (sp2 & 0x1FFFFFFFu) + 8 <= 0x01800000u; j++) {
                    uint32_t pp = sp2 & 0x1FFFFFFFu;
                    uint32_t back = gc_rd32(pp), lr2 = gc_rd32(pp + 4u);
                    fprintf(stderr, "[junk] frame%u sp=%08x back=%08x lr=%08x\n",
                            j, sp2, back, lr2);
                    if (back <= sp2 || back >= 0x81800000u) break;
                    sp2 = back;
                }
                {   /* 全 RAM 扫 "chro"/"Chro"（字节对齐无关）—— 字符串源在哪 */
                    unsigned hits = 0;
                    for (uint32_t a = 0; a + 4 <= 0x01800000u && hits < 24; a++) {
                        if (gc_ram[a] == 'c' && gc_ram[a+1] == 'h' && gc_ram[a+2] == 'r'
                            && gc_ram[a+3] == 'o') {
                            fprintf(stderr, "[junk] chro@%08x:", a + 0x80000000u);
                            for (unsigned j = 0; j < 12; j++) {
                                uint32_t v = gc_rd32(a + 4u * j);
                                fprintf(stderr, " %02x%02x%02x%02x", v >> 24,
                                        (v >> 16) & 255, (v >> 8) & 255, v & 255);
                            }
                            fprintf(stderr, "\n");
                            hits++;
                        }
                    }
                    fprintf(stderr, "[junk] chro scan hits=%u\n", hits);
                }
                fprintf(stderr, "[junk] 803E5560..5680:");
                for (uint32_t a = 0x803E5560u; a < 0x803E5680u; a += 4)
                    fprintf(stderr, " %08x", gc_rd32(a));
                fprintf(stderr, "\n");
                { uint32_t G = gc_rd32(0x803CFCA8u);
                  fprintf(stderr, "[junk] G=*(803CFCA8)=%08x *(G+30)=%08x slot803CF328=%08x\n",
                          G, G ? gc_rd32(G + 0x30u) : 0, gc_rd32(0x803CF328u)); }
                fprintf(stderr, "[junk] ctx=*(C0)=%08x handler=*(3010)=%08x r13=%08x\n",
                        gc_rd32(0xC0u), gc_rd32(0x3010u), gc.r[13]);
            }
        }
        return;
    }
    if (gc_probe_quiet++ < 40)
        fprintf(stderr, "[probe] pc=%08x r3=%08x r4=%08x r31=%08x lr=%08x\n",
                pc, gc.r[3], gc.r[4], gc.r[31], gc.lr);
    if (pc == 0x8028dd70u) {
        /* 差分快照：首次进入存 0x80300000-0x80400000（MSL pool/bins/游戏堆所在
         * .bss 区），panic 时 diff —— 回答"内存被吃光 vs 从未存在"。 */
        if (!gc_snap_buf) {
            gc_snap_buf = malloc(0x100000u);
            if (gc_snap_buf) {
                for (uint32_t i = 0; i < 0x100000u; i += 4)
                    gc_snap_buf[i / 4] = gc_rd32(0x80300000u + i);
                fprintf(stderr, "[snap] T0 saved\n");
            }
        }
    }
    if (pc == 0x80296a70u || pc == 0x80281d2cu || pc == 0x80281d74u) {
        {   /* 差分：T0(首次 dlmalloc) vs 现在 */
            extern uint32_t *gc_snap_buf;
            if (gc_snap_buf) {
                FILE *tf = fopen("heap_diff.txt", "w");
                if (tf) {
                    for (uint32_t i = 0; i < 0x100000u; i += 4) {
                        uint32_t now = gc_rd32(0x80300000u + i);
                        if (now != gc_snap_buf[i / 4])
                            fprintf(tf, "%08x %08x -> %08x\n",
                                    0x80300000u + i, gc_snap_buf[i / 4], now);
                    }
                    fclose(tf);
                }
            }
        }
        /* X1/X2: 游戏堆管理器对象（0x80296a70 的 r13 全局），初始 vs panic 对比 */
        static unsigned gh_n;
        uint32_t hp = gc_rd32(0x803CEDECu);
        fprintf(stderr, "[probe] gameheap#%u ptr@803CEDEC=%08x r3req=%08x\n",
                ++gh_n, hp, gc.r[3]);
        fprintf(stderr, "[probe] obj:");
        for (k = 0; k < 32; k++)
            fprintf(stderr, " %08x", gc_rd32(hp + 4u * k));
        fprintf(stderr, "\n[probe] +0x100:");
        for (k = 0; k < 20; k++)
            fprintf(stderr, " %08x", gc_rd32(hp + 0x100u + 4u * k));
        fprintf(stderr, "\n");
        {   /* 游戏堆三空闲链头 + r13 全局前后文 */
            uint32_t links[3];
            links[0] = gc_rd32(hp + 4u); links[1] = gc_rd32(hp + 8u);
            links[2] = gc_rd32(hp + 12u);
            for (k = 0; k < 3; k++)
                fprintf(stderr, "[probe] link[%u] @%08x: %08x %08x %08x %08x\n",
                        k, links[k], gc_rd32(links[k]), gc_rd32(links[k] + 4u),
                        gc_rd32(links[k] + 8u), gc_rd32(links[k] + 12u));
            fprintf(stderr, "[probe] glob@803CEDE0:");
            for (k = 0; k < 8; k++)
                fprintf(stderr, " %08x", gc_rd32(0x803CEDE0u + 4u * k));
            fprintf(stderr, "\n");
        }
    }
    {   /* 游戏 back chain：*(r1)=上一帧 r1，*(r1+4)=返回地址 */
        uint32_t sp = gc.r[1];
        for (k = 0; k < 10 && sp >= 0x80000000u && (sp & 0x1FFFFFFFu) + 8 <= 0x01800000u; k++) {
            uint32_t p = sp & 0x1FFFFFFFu;
            uint32_t back = ((uint32_t)gc_ram[p] << 24) | ((uint32_t)gc_ram[p+1] << 16)
                          | ((uint32_t)gc_ram[p+2] << 8) | gc_ram[p+3];
            uint32_t lr  = ((uint32_t)gc_ram[p+4] << 24) | ((uint32_t)gc_ram[p+5] << 16)
                          | ((uint32_t)gc_ram[p+6] << 8) | gc_ram[p+7];
            fprintf(stderr, "[probe]   sp=%08x lr=%08x\n", sp, lr);
            if (back <= sp) break;
            sp = back;
        }
    }
    if (pc == 0x80281d2cu || pc == 0x80281d74u) {   /* OOM panic：全景堆扫描 */
        uint32_t va;
        /* X1: 游戏堆管理器对象（0x80296a70 的 r13 全局） */
        { uint32_t hp = gc_rd32(0x803CEDECu);
          fprintf(stderr, "[probe] gameheap ptr@803CEDEC=%08x\n", hp);
          for (k = 0; k < 0x150u / 4u; k++) {
              uint32_t v = gc_rd32(hp + 4u * k);
              if (v && (v < 0x80000000u || v >= 0x81800000u) == 0)
                  fprintf(stderr, "[probe]   +%03x = %08x\n", 4u * k, v);
          }
          fprintf(stderr, "[probe] raw:");
          for (k = 0; k < 24; k++)
              fprintf(stderr, " %08x", gc_rd32(hp + 4u * k));
          fprintf(stderr, "\n");
        }
        fprintf(stderr, "[probe] scanning RAM for HeapDesc/heap heads...\n");
        { FILE *tf = fopen("heap_scan.txt", "w");
          if (tf) {
              /* SDK HeapDesc {size,free}：size 64KB..8MB，free 指向紧邻 Cell */
              for (va = 0x80000000u; va < 0x81800000u; va += 4) {
                  uint32_t sz = gc_rd32(va), fr = gc_rd32(va + 4);
                  if (sz >= 0x10000u && sz <= 0x800000u
                      && fr > va && fr <= va + sz && (fr & 7u) == 0) {
                      uint32_t csz = gc_rd32(fr);
                      fprintf(tf, "HeapDesc? @%08x size=%08x free=%08x "
                                  "cell.size=%08x cell.next=%08x cell.prev=%08x\n",
                              va, sz, fr, csz, gc_rd32(fr + 4), gc_rd32(fr + 8));
                  }
              }
              /* MSL pool bins 区 + 周边原始 dump */
              for (va = 0x8039c600u; va < 0x8039c800u; va += 4)
                  fprintf(tf, "%08x %08x\n", va, gc_rd32(va));
              fclose(tf);
          } }
    }
}

void gc_unimplemented(const char *what, uint32_t ea)
{
    fprintf(stderr, "gc_unimplemented: %s @%08x\n", what, ea);
    abort();
}

void gc_trap(uint32_t ea)
{
    fprintf(stderr, "gc_trap: tw/twi hit @%08x\n", ea);
    abort();
}

uint32_t gc_bswap32(uint32_t v)
{
    return ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) |
           ((v >> 8) & 0xFF00u) | ((v >> 24) & 0xFFu);
}

uint16_t gc_bswap16(uint16_t v)
{
    return (uint16_t)((v << 8) | (v >> 8));
}

void gc_trace_dump(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    unsigned start = trace_n >= 8192 ? trace_n - 8192 : 0;
    for (unsigned k = start; k < trace_n; k += 2)
        fprintf(f, "%08x %08x\n", trace_ring[k % 8192], trace_ring[(k+1) % 8192]);
    fclose(f);
}

/* ---- msr 变化取证（GC_MSR_TRACE=1 时打印前 256 次变化） ---- */
void gc_msr_note(uint32_t pc, uint32_t oldv, uint32_t newv)
{
    static int on = -1;
    static unsigned n;
    if (on < 0) {
        const char *e = getenv("GC_MSR_TRACE");
        on = e && *e && *e != '0';
    }
    if (!on || oldv == newv) return;
    if (n++ < 256)
        fprintf(stderr, "msr-change: pc=%08x %08x -> %08x\n", pc, oldv, newv);
}
