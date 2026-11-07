/* hal/gx.c — WGPIPE 吸收 + GX 命令同步解析器 + CP/PE 影子。出处见 gx.h。 */
#include "gx.h"
#include "hal.h"
#include "mem.h"
#include "gc_abi.h"
#include "irq.h"
#include "../third_party/gcn-recomp/cp_fifo.h"
#ifdef GC_UPSTREAM_GX
#include "../platform/desktop/upstream_gx/upstream_gx.h"
void gc_trace_dump(const char *path);
#endif

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- CP 状态（顶点格式，供图元长度计算） ---- */
static uint32_t vcd_lo, vcd_hi;
static uint32_t vat[3][8];        /* VAT group A/B/C × 8 槽 */
static uint32_t array_base[16], array_stride[16];

/* ---- PE 状态 ----
 * 线模型 = pending && enable（电平）：
 *   BP 0x45000002 / BP 0x47 置 pending；PE_CTRL bits2/3 写 1 = ACK 清 pending。
 * PE_CTRL 的 ACK bits2/3 读回为零；pending 状态独立驱动 PI 中断线。 */
static uint16_t pe_ctrl;          /* bit0 token_en, bit1 finish_en */
static uint16_t pe_token;
static int token_pending, finish_pending;

static void pe_refresh(void)
{
    if (token_pending && (pe_ctrl & 1u))
        hal_pi_set_interrupt(PI_INT_PE_TOKEN);
    else
        hal_pi_clear_interrupt(PI_INT_PE_TOKEN);
    if (finish_pending && (pe_ctrl & 2u))
        hal_pi_set_interrupt(PI_INT_PE_FINISH);
    else
        hal_pi_clear_interrupt(PI_INT_PE_FINISH);
}

/* ---- 统计 / 观测 ---- */
static uint32_t efb_dst;          /* BP 0x4B */
static uint32_t efb_copies, xf_loads, cmd_count;
static uint64_t bytes_fed, bytes_consumed;

/* ---- gather pipe（WGPIPE → 32B 对齐 burst → PI FIFO 环 + 解析器） ---- */
static uint8_t  gather[32];
static unsigned gather_cnt;

/* ---- 解析器暂存（跨 burst 的部分命令） ---- */
#define PARSE_CAP (256 * 1024)
static uint8_t  pbuf[PARSE_CAP];
static size_t   plen;
static CpFifo cp_fifo;

static void parse_feed(const uint8_t *d, size_t n);

static void cp_consume(uint32_t phys, uint32_t length, void *user)
{
    (void)user;
    parse_feed(gc_ram + phys, length);
}

static void cp_interrupt(bool asserted, void *user)
{
    (void)user;
    if (asserted) hal_pi_set_interrupt(PI_INT_CP);
    else          hal_pi_clear_interrupt(PI_INT_CP);
}

void hal_gx_init(void)
{
    vcd_lo = vcd_hi = 0;
    memset(vat, 0, sizeof(vat));
    memset(array_base, 0, sizeof(array_base));
    memset(array_stride, 0, sizeof(array_stride));
    pe_ctrl = 0; pe_token = 0;
    token_pending = finish_pending = 0;
    efb_dst = 0; efb_copies = xf_loads = cmd_count = 0;
    bytes_fed = bytes_consumed = 0;
    gather_cnt = 0;
    plen = 0;
    cp_fifo_init(&cp_fifo, GC_MEM1_SIZE, cp_consume, cp_interrupt, NULL);
    pe_refresh();
}

uint32_t hal_gx_efb_copy_dst(void)   { return efb_dst; }
uint32_t hal_gx_efb_copy_count(void) { return efb_copies; }
uint32_t hal_gx_xf_load_count(void)  { return xf_loads; }
uint64_t hal_gx_bytes_consumed(void) { return bytes_consumed; }
uint64_t hal_gx_bytes_fed(void)      { return bytes_fed; }
uint32_t hal_gx_cmd_count(void)      { return cmd_count; }

void hal_gx_signal_finish(void)
{
    finish_pending = 1;
    pe_refresh();
    gc_runtime_manifest_note_event("gx_finish");
}

void hal_gx_signal_token(uint16_t token, int interrupt)
{
    pe_token = token;
    if (interrupt) token_pending = 1;
    pe_refresh();
}

/* ---- 顶点尺寸（Dolphin VertexLoader 语义；字段位域见 CPMemory.h） ---- */
static int comp_fmt_size(unsigned f)   /* UByte/Byte/UShort/Short/Float */
{
    static const int sz[8] = { 1, 1, 2, 2, 4, 4, 4, 4 };
    return sz[f & 7];
}
static int color_fmt_size(unsigned f)  /* RGB565..RGBA8888 */
{
    static const int sz[8] = { 2, 3, 4, 2, 3, 4, 4, 4 };
    return sz[f & 7];
}
/* src: 0=不存在 1=direct 2=index8 3=index16 */
static int comp_bytes(unsigned src, unsigned direct_size)
{
    switch (src & 3) {
    case 1: return (int)direct_size;
    case 2: return 1;
    case 3: return 2;
    default: return 0;
    }
}

static uint32_t vertex_size(unsigned vat_idx)
{
    uint32_t g0 = vat[0][vat_idx & 7];
    uint32_t g1 = vat[1][vat_idx & 7];
    uint32_t g2 = vat[2][vat_idx & 7];
    uint32_t sz = 0;

    /* 矩阵索引（VCD bits 0..8，各 1 字节 direct） */
    for (int i = 0; i < 9; i++)
        if ((vcd_lo >> i) & 1u) sz += 1;

    /* Position：VCD bits 9-10；elements g0 bit0（XY=2/XYZ=3），fmt g0 bits 1-3 */
    sz += (uint32_t)comp_bytes((vcd_lo >> 9) & 3,
        ((g0 & 1u) ? 3u : 2u) * (uint32_t)comp_fmt_size((g0 >> 1) & 7));

    /* Normal：VCD bits 11-12；N=3 标量、NTB=9 标量。
     * Indexed normals consume one index, or three when NormalIndex3 is set;
     * the index width is still selected by VCD (index8/index16). */
    unsigned nsrc = (vcd_lo >> 11) & 3;
    unsigned normal_ntb = (g0 >> 9) & 1u;
    unsigned normal_index3 = (g0 >> 31) & 1u;
    if (nsrc == 1)
        sz += (normal_ntb ? 9u : 3u) *
              (uint32_t)comp_fmt_size((g0 >> 10) & 7);
    else if (nsrc)
        sz += (uint32_t)comp_bytes(nsrc, 1u) * (normal_index3 ? 3u : 1u);

    /* Color0/1：VCD bits 13-16；elements g0 bit13/17，fmt g0 bits 14-16/18-20 */
    sz += (uint32_t)comp_bytes((vcd_lo >> 13) & 3,
        (uint32_t)color_fmt_size((g0 >> 14) & 7));
    sz += (uint32_t)comp_bytes((vcd_lo >> 15) & 3,
        (uint32_t)color_fmt_size((g0 >> 18) & 7));

    /* Tex0..7：VCD_HI bits 0-15（各 2 位）；
     * Tex0 fmt 在 g0 bits 22-24 / elements bit21；
     * Tex1..3 在 g1（0/1,9/10,18/19）；Tex4..7 在 g1 bit27+ 与 g2 */
    static const unsigned elem_bit[8] = { 21, 0,  9, 18, 27, 5, 14, 23 };
    static const unsigned fmt_sh[8]   = { 22, 1, 10, 19, 28, 6, 15, 24 };
    for (int t = 0; t < 8; t++) {
        unsigned src = (vcd_hi >> (2 * t)) & 3;
        if (t == 0) {
            sz += (uint32_t)comp_bytes(src,
                (((g0 >> 21) & 1u) ? 2u : 1u) * (uint32_t)comp_fmt_size((g0 >> 22) & 7));
        } else {
            /* 分组归属：Tex1..4 → g1（含 bit27..），Tex5..7 → g2 */
            uint32_t g = (t <= 4) ? g1 : g2;
            unsigned elem = (g >> elem_bit[t]) & 1u;
            unsigned fmt  = (g >> fmt_sh[t]) & 7u;
            sz += (uint32_t)comp_bytes(src, (elem ? 2u : 1u) * (uint32_t)comp_fmt_size(fmt));
        }
    }
    return sz;
}

/* ---- BP 写（LOAD_BP cmd2 + 24-bit value） ---- */
static void bp_write(uint8_t reg, uint32_t val)
{
    switch (reg) {
    case 0x45:  /* BPMEM_SETDRAWDONE：bit1=finish 事件（GXSetDrawDone 0x45000002） */
        if (val & 2u) {
            static unsigned dd_n;
            if (dd_n++ < 5)
                fprintf(stderr,
                        "[gx] drawdone#%u pc=%08x fed=%llu pend=%d en=%d\n",
                        dd_n, gc.pc, (unsigned long long)bytes_fed,
                        finish_pending, pe_ctrl & 2u);
            finish_pending = 1;
            pe_refresh();
        }
        break;
    case 0x47:  /* BPMEM_PE_TOKEN_ID：置 token 并挂 token 事件 */
        pe_token = (uint16_t)(val & 0xFFFF);
        token_pending = 1;
        pe_refresh();
        break;
    case 0x4B:  /* BPMEM_EFB_ADDR：copy 目标，FBB 为 32B 单位 (BPMemory.h) */
        efb_dst = (val & 0x1FFFFFu) << 5;
        {
            static unsigned n;
            if (getenv("GC_GX_TRACE") && n++ < 8u)
                fprintf(stderr, "[gx] efb-addr#%u pc=%08x bp=%02x val=%06x dst=%08x\n",
                        n, gc.pc, reg, val & 0xFFFFFFu, efb_dst);
        }
        break;
    case 0x52:  /* BPMEM_TRIGGER_EFB_COPY：bit14 copy_to_xfb */
        {
            static unsigned n;
            if (getenv("GC_GX_TRACE") && n++ < 8u)
                fprintf(stderr, "[gx] efb-trigger#%u pc=%08x bp=%02x val=%06x xfb=%d\n",
                        n, gc.pc, reg, val & 0xFFFFFFu,
                        (val & (1u << 14)) != 0);
        }
        if (val & (1u << 14)) efb_copies++;
        break;
    default:
        break;
    }
}

/* ---- CP 写（LOAD_CP cmd2 + 32-bit value） ---- */
static void cp_write(uint8_t sub, uint32_t val)
{
    switch (sub & 0xF0) {
    case 0x30: break;                                   /* MATINDEX_A */
    case 0x40: break;                                   /* MATINDEX_B */
    case 0x50: vcd_lo = val; break;                     /* VCD_LO */
    case 0x60: vcd_hi = val; break;                     /* VCD_HI */
    case 0x70: vat[0][sub & 7] = val; break;            /* VAT_A */
    case 0x80: vat[1][sub & 7] = val; break;            /* VAT_B */
    case 0x90: vat[2][sub & 7] = val; break;            /* VAT_C */
    case 0xA0: array_base[sub & 0xF] = val; break;      /* ARRAY_BASE */
    case 0xB0: array_stride[sub & 0xF] = val; break;    /* ARRAY_STRIDE */
    default: break;                                     /* 0x00/0x10/0x20 perf */
    }
}

/* ---- 命令解析（长度表 = OpcodeDecoding.h RunCommand） ---- */
static size_t parse_one(const uint8_t *d, size_t avail)
{
    if (avail < 1) return 0;
    uint8_t cmd = d[0];
    switch (cmd) {
    case 0x00: {   /* NOP：合并连续 */
        size_t n = 1;
        while (n < avail && d[n] == 0x00) n++;
        cmd_count++;
        return n;
    }
    case 0x08: {   /* LOAD_CP */
        if (avail < 6) return 0;
        cp_write(d[1], ((uint32_t)d[2] << 24) | ((uint32_t)d[3] << 16) |
                       ((uint32_t)d[4] << 8) | d[5]);
        cmd_count++;
        return 6;
    }
    case 0x10: {   /* LOAD_XF：5 + 4*n */
        if (avail < 5) return 0;
        uint32_t cmd2 = ((uint32_t)d[1] << 24) | ((uint32_t)d[2] << 16) |
                        ((uint32_t)d[3] << 8) | d[4];
        uint32_t n = (((cmd2 >> 16) & 0xFu) + 1u);
        size_t total = 5 + (size_t)n * 4u;
        if (avail < total) return 0;
        xf_loads++;           /* M1：只记录长度 */
        cmd_count++;
        return total;
    }
    case 0x20: case 0x28: case 0x30: case 0x38:   /* LOAD_INDX_A..D */
        if (avail < 5) return 0;
        cmd_count++;
        return 5;
    case 0x40:    /* CALL_DL：9 字节；M1 不解析 DL 内容（CP 才会执行） */
        if (avail < 9) return 0;
        cmd_count++;
        return 9;
    case 0x61: {   /* LOAD_BP：5 */
        if (avail < 5) return 0;
        bp_write(d[1], ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 8) | d[4]);
        cmd_count++;
        return 5;
    }
    default:
        if (cmd >= 0x80 && cmd <= 0xBF) {   /* 图元：3 + n*vsize */
            if (avail < 3) return 0;
            unsigned vat_idx = cmd & 7;
            uint32_t vs = vertex_size(vat_idx);
            uint16_t n = (uint16_t)((d[1] << 8) | d[2]);
            size_t total = 3 + (size_t)n * vs;
            if (avail < total) return 0;
            cmd_count++;
            return total;
        }
        /* 未知命令（0x44 metrics/0x48 INVL_VC 等）：1 字节吞掉，不乱步 */
        cmd_count++;
        return 1;
    }
}

static void parse_feed(const uint8_t *d, size_t n)
{
    static int dump_checked;
    static FILE *dump;
    static size_t dump_bytes;
    if (!dump_checked) {
        const char *path = getenv("GC_GX_DUMP");
        dump_checked = 1;
        if (path && *path) dump = fopen(path, "wb");
    }
    if (dump && dump_bytes < 8u * 1024u * 1024u) {
        size_t room = 8u * 1024u * 1024u - dump_bytes;
        size_t count = n < room ? n : room;
        dump_bytes += fwrite(d, 1, count, dump);
        fflush(dump);
    }
    bytes_fed += n;
#ifdef GC_UPSTREAM_GX
    if (upstream_gx_consume(d, n) != 0) {
        fprintf(stderr, "upstream GX: %s pc=%08x lr=%08x frame=%llu\n",
                upstream_gx_error(), gc.pc, gc.lr,
                (unsigned long long)hal_frame_count());
        const char *path = getenv("GC_RAM_DUMP");
        if (path && *path) {
            FILE *dump = fopen(path, "wb");
            if (dump) {
                size_t written = fwrite(gc_ram, 1, sizeof(gc_ram), dump);
                fclose(dump);
                fprintf(stderr, "GX failure RAM dump %s bytes=%zu\n", path, written);
            }
        }
        const char *capture = getenv("GC_GX_FAILURE_PNG");
        if (capture && *capture) (void)upstream_gx_capture_png(capture);
        gc_trace_dump("trace_last.txt");
        abort();
    }
    bytes_consumed += n;
    return;
#endif
    if (plen + n > PARSE_CAP) {
        /* 超出暂存的畸形流：按未知字节对齐丢弃并保序计数（日志后继续） */
        fprintf(stderr, "hal/gx: parse buffer overflow, dropping %zu bytes\n", plen + n);
        bytes_consumed += plen + n;
        plen = 0;
        return;
    }
    memcpy(pbuf + plen, d, n);
    plen += n;

    size_t off = 0;
    static unsigned cn;
    for (;;) {
        size_t used = parse_one(pbuf + off, plen - off);
        if (!used) break;                    /* 命令不完整，等下个 burst */
        if (cn < 96)                         /* 对齐取证：DrawDone 不被解析时用它定位 */
            fprintf(stderr, "[gx] c%u op=%02x len=%zu\n",
                    cn, pbuf[off], used), cn++;
        off += used;
    }
    bytes_consumed += off;
    if (off) { memmove(pbuf, pbuf + off, plen - off); plen -= off; }
}

/* ---- WGPIPE（0xCC008000 字节写 = GX FIFO） ----
 * Dolphin GPFifo.cpp：写满 32 字节 → 拷到 PI_FIFO_WPTR，wp += 32，
 * wp == end 则回 base（UpdateGatherPipe）。M1 同步推进该环。 */
void hal_gx_burst32(const uint8_t *b32)
{
    uint32_t wp = hal_pi_fifo_wptr & ~0x1Fu;
    uint32_t end = hal_pi_fifo_end;
    uint32_t base = hal_pi_fifo_base;
    uint32_t phys = wp & 0x03FFFFE0u;
    if (base == end || phys > GC_MEM1_SIZE - 32u) return;
    hal_mem_write_burst(phys, b32, 32);
    wp = (wp == end) ? base : wp + 32u;
    hal_pi_fifo_wptr = wp;
    cp_fifo_note_burst(&cp_fifo, wp);
}

void hal_gxpipe_write8(uint8_t v)
{
    gather[gather_cnt++] = v;
    if (gather_cnt == 32) {
        hal_gx_burst32(gather);
        gather_cnt = 0;
    }
}

/* ---- 宽度写助手：全部按 PPC 大端拆字节，与逐字节写在 FIFO 中逐位一致 ---- */
void hal_gxpipe_write16(uint16_t v)
{
    hal_gxpipe_write8((uint8_t)(v >> 8));
    hal_gxpipe_write8((uint8_t)v);
}

void hal_gxpipe_write32(uint32_t v)
{
    hal_gxpipe_write8((uint8_t)(v >> 24));
    hal_gxpipe_write8((uint8_t)(v >> 16));
    hal_gxpipe_write8((uint8_t)(v >> 8));
    hal_gxpipe_write8((uint8_t)v);
}

void hal_gxpipe_write64(uint64_t v)
{
    for (int i = 7; i >= 0; i--)
        hal_gxpipe_write8((uint8_t)(v >> (8 * i)));
}

/* ---- CP / PE MMIO ---- */
uint16_t hal_cp_read16(uint32_t reg)
{
    return cp_fifo_read16(&cp_fifo, reg);
}

void hal_cp_write16(uint32_t reg, uint16_t v)
{
    cp_fifo_write16(&cp_fifo, reg, v);
}

void hal_gx_dump_state(void)
{
    fprintf(stderr, "GX FIFO: CP ctrl=%04x status=%04x base=%08x end=%08x "
                    "read=%08x write=%08x distance=%08x breakpoint=%08x; "
                    "PI base=%08x end=%08x write=%08x; gather=%u partial=%zu\n",
            cp_fifo.cr, cp_fifo.sr, cp_fifo.base, cp_fifo.end, cp_fifo.rptr,
            cp_fifo.wptr, cp_fifo.distance, cp_fifo.bp,
            hal_pi_fifo_base, hal_pi_fifo_end, hal_pi_fifo_wptr, gather_cnt, plen);
    if (plen) fprintf(stderr, "GX partial: %02x %02x %02x\n", pbuf[0],
                      plen > 1 ? pbuf[1] : 0, plen > 2 ? pbuf[2] : 0);
}

uint16_t hal_pe_read16(uint32_t reg)
{
    switch (reg) {
    case 0x0A:  /* ACK bits are write-only; pending interrupt lines are separate. */
        return pe_ctrl;
    case 0x0E: return pe_token;
    default:   return 0;   /* BBOX 等只读 0 */
    }
}

void hal_pe_write16(uint32_t reg, uint16_t v)
{
    switch (reg) {
    case 0x0A:
        /* bits2/3 写 1 = ACK：清对应 pending（撤 PI 线）；bits0/1 = 使能 */
        if (v & 8u) {
            static unsigned ack_n;
            if (finish_pending && ack_n++ < 5)
                fprintf(stderr, "[gx] finish-ack#%u pc=%08x cause=%04x\n",
                        ack_n, gc.pc, hal_pi_read32(0x00));
            finish_pending = 0;
        }
        if (v & 4u) token_pending = 0;
        pe_ctrl = (uint16_t)(v & 3u);
        pe_refresh();
        break;
    default: break;
    }
}
