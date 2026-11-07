/* hal/vi.c — VI 行为：寄存器影子、retrace 中断、XFB YUY2→RGBA。 */
#include "vi.h"
#include "hal.h"
#include "mem.h"
#include "gc_abi.h"
#include "irq.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 16-bit 寄存器表：offset 0x00..0x74（偶数）。偏移见 VideoInterface.h。 */
#define VI_NREGS 0x78
static uint16_t vi_r[VI_NREGS / 2];

/* Raster time is in Gekko cycles, independent of CPU polling granularity.
 * VI samples take two 27/54 MHz source clocks: 36/18 cycles at 486 MHz.
 * Sources: Dolphin VideoInterface.cpp GetTicksPerSample/GetHalfLinesPer*Field;
 * this DOL's VIConfigure at 802de84c programs DI0 from timing->nhlines/hlw,
 * while VIInit programs DI1 at (1,1). These are alternate field boundaries. */
static uint32_t raster_cycles, odd_cycles, total_cycles, sample_cycles;
static uint32_t next_event = UINT32_MAX;
static uint32_t compare_cycles[4];
static int timing_enabled;

static void vi_update_int_line(void);
static void vi_schedule(void);

static uint32_t vi_distance(uint32_t target)
{
    return target > raster_cycles ? target - raster_cycles :
           total_cycles - raster_cycles + target;
}

uint32_t hal_vi_cycles_to_field(void)
{
    hal_clock_sync_devices();
    if (!timing_enabled) return UINT32_MAX;
    return raster_cycles < odd_cycles ? odd_cycles - raster_cycles :
           total_cycles - raster_cycles;
}

uint32_t hal_vi_cycles_to_event(void)
{
    hal_clock_sync_devices();
    return next_event;
}

static void vi_schedule(void)
{
    next_event = hal_vi_cycles_to_field();
    if (!timing_enabled) return;
    for (unsigned i = 0; i < 4; ++i) {
        if (compare_cycles[i] == UINT32_MAX ||
            (vi_r[(0x30 + 4 * i) / 2] & 0x8000u)) continue;
        uint32_t distance = vi_distance(compare_cycles[i]);
        if (distance < next_event) next_event = distance;
    }
}

static void vi_update_timing(void)
{
    const uint32_t hlw = vi_r[0x06 / 2] & 0x3ffu;
    const uint32_t sync_active = 3u * (vi_r[0] & 15u) +
                                2u * ((vi_r[0] >> 4) & 0x3ffu);
    const uint32_t odd = sync_active + (vi_r[0x0c / 2] & 0x3ffu) +
                                      (vi_r[0x0e / 2] & 0x3ffu);
    const uint32_t even = sync_active + (vi_r[0x10 / 2] & 0x3ffu) +
                                       (vi_r[0x12 / 2] & 0x3ffu);
    sample_cycles = (vi_r[0x6c / 2] & 1u) ? 18u : 36u;
    odd_cycles = odd * hlw * sample_cycles;
    total_cycles = (odd + even) * hlw * sample_cycles;
    timing_enabled = (vi_r[0x02 / 2] & 1u) && hlw && odd && even;
    if (total_cycles) raster_cycles %= total_cycles;
    for (unsigned i = 0; i < 4; ++i) {
        const uint32_t vct = vi_r[(0x30 + 4 * i) / 2] & 0x7ffu;
        const uint32_t hct = vi_r[(0x32 + 4 * i) / 2] & 0x7ffu;
        const uint32_t target = vct ?
            ((vct - 1u) * 2u * hlw + hct - 1u) * sample_cycles : UINT32_MAX;
        compare_cycles[i] = vct && hct && hct <= 2u * hlw && target < total_cycles ?
                            target : UINT32_MAX;
    }
    vi_schedule();
}

static uint16_t vi_beam(uint32_t reg)
{
    const uint32_t line = (vi_r[0x06 / 2] & 0x3ffu) * 2u;
    const uint32_t sample = sample_cycles ? raster_cycles / sample_cycles : 0;
    if (!line) return 1;
    return (uint16_t)(1u + (reg == 0x2c ? sample / line : sample % line));
}

void hal_vi_advance(uint32_t cycles)
{
    hal_clock_sync_devices();
    hal_clock_device_changed();
    while (cycles && timing_enabled) {
        /* A register write can leave a stale schedule across the call; a zero
         * or over-total deadline cannot advance the beam and used to spin here. */
        if (!next_event || next_event > total_cycles) {
            vi_schedule();
            if (!next_event || next_event > total_cycles) next_event = 1;
        }
        uint32_t step = cycles < next_event ? cycles : next_event;
        raster_cycles += step;
        /* Coarse device ticks can pass a whole field; keep the beam in range. */
        if (raster_cycles >= total_cycles) raster_cycles %= total_cycles;
        cycles -= step;
        next_event -= step;
        if (next_event) return;
        const int field_end = raster_cycles == 0 || raster_cycles == odd_cycles;
        for (unsigned i = 0; i < 4; ++i)
            if (raster_cycles == compare_cycles[i])
                vi_r[(0x30 + 4 * i) / 2] |= 0x8000u;
        vi_update_int_line();
        vi_schedule();
        /* The callback can program VI, or leave the dispatcher via longjmp.
         * Commit beam, status and the next deadline before handing it control. */
        if (field_end) hal_irq_frame_event();
    }
}

void hal_vi_init(void)
{
    hal_clock_sync_devices();
    hal_clock_device_changed();
    memset(vi_r, 0, sizeof(vi_r));
    raster_cycles = 0;
    vi_update_timing();
    vi_update_int_line();
}

uint16_t hal_vi_reg(uint32_t off)
{
    hal_clock_sync_devices();
    if (off == 0x2c || off == 0x2e) return vi_beam(off);
    return (off < VI_NREGS) ? vi_r[off / 2] : 0;
}

uint16_t hal_vi_read16(uint32_t reg)
{
    hal_clock_sync_devices();
    if (reg >= VI_NREGS) return 0;
    switch (reg) {
    case 0x2C: case 0x2E: return vi_beam(reg);
    case 0x6E: return 0x0003u;       /* VIGetDTVStatus 读 __VIRegs[0x37]=0x6E
                                      * 低 2 位并 &1：bit0=1 表示检出信号。
                                      * 0x6C 曾给过 1 但游戏读 0x6E 得 0 →
                                      * 判定无信号后永久放弃视频初始化（实测）。
                                      * bit1 = NTSC-J 机（Dolphin UVIDTVStatus.ntsc_j）：
                                      * OSGetFontEncode → SJIS，与日版机/ Dolphin 格式化的卡一致。 */
    default:   return vi_r[reg / 2];
    }
}

/* PI 线电平语义：VI 线 = 任一 DISP_INT 的 IR_INT&IR_MASK。
 * 设备 handler 写回 reg&~0x8000 清 IR_INT 后 PI cause 位必须联动掉线，
 * 否则线一直挂着、EE 每次恢复都重注入（实测每帧 2.7 万次中断风暴，
 * SDK __OSDispatchInterrupt 本身不清 PI cause——真机由设备侧清线）。 */
static void vi_update_int_line(void)
{
    static const uint32_t ir_reg_hi[4] = { 0x30, 0x34, 0x38, 0x3C };
    bool fire = false;
    for (int i = 0; i < 4; i++) {
        uint16_t hi = vi_r[ir_reg_hi[i] / 2];
        if (((hi >> 12) & 1) && (hi & 0x8000u))
            fire = true;
    }
    if (fire) hal_pi_set_interrupt(PI_INT_VI);
    else      hal_pi_clear_interrupt(PI_INT_VI);
}

static uint32_t vi_xfb_reg(uint32_t hi_reg)
{
    return ((uint32_t)vi_r[hi_reg / 2] << 16) | vi_r[(hi_reg + 2) / 2];
}

static uint32_t vi_xfb_address(uint32_t hi_reg, bool poff)
{
    uint32_t raw = vi_xfb_reg(hi_reg) & 0x00FFFFFFu;
    return poff ? raw << 5 : raw;
}

void hal_vi_write16(uint32_t reg, uint16_t v)
{
    hal_clock_sync_devices();
    hal_clock_device_changed();
    if (reg >= VI_NREGS || reg == 0x2c || reg == 0x2e) return;
    {   /* VI frame/XFB writer observation (GC_PROBE only). */
        static int vi_writer_n = -1;
        if (vi_writer_n < 0) {
            const char *e = getenv("GC_PROBE");
            vi_writer_n = (e && *e && *e != '0') ? 0 : 64;
        }
        if (vi_writer_n < 64 &&
            (reg == 0x1C || reg == 0x1E || reg == 0x70 ||
             reg == 0x00 || reg == 0x02)) {
            fprintf(stderr, "[vi-writer] frame=%llu pc=%08x reg=%02x value=%04x\n",
                    (unsigned long long)hal_frame_count(), gc.pc,
                    (unsigned)reg, (unsigned)v);
            vi_writer_n++;
        }
    }

    if (reg == 0x02) {
        /* Dolphin updates only documented control bits. RST is a self-clearing
         * command which clears the four 32-bit interrupt registers. */
        vi_r[reg / 2] = (uint16_t)(v & 0x03FDu);
        if (v & 0x0002u) {
            memset(&vi_r[0x30 / 2], 0, 4u * sizeof(uint32_t));
            raster_cycles = 0;
            vi_update_int_line();
        }
        vi_update_timing();
        return;
    }

    vi_r[reg / 2] = v;
    if (reg == 0x30 || reg == 0x34 || reg == 0x38 || reg == 0x3C)
        vi_update_int_line();
    if (reg == 0 || reg == 6 || (reg >= 0x0c && reg <= 0x12) ||
        reg == 0x6c || (reg >= 0x30 && reg <= 0x3e))
        vi_update_timing();
}

uint32_t hal_vi_xfb_top(void)
{
    hal_clock_sync_devices();
    uint32_t top = vi_xfb_reg(0x1C);
    /* POFF is bit 28 of the combined register; it selects 32-byte units. */
    return vi_xfb_address(0x1C, (top & 0x10000000u) != 0);
}

void hal_frame_tick(void)
{
    /* Legacy host stepping must advance the same CPU/TB/device timeline. */
    uint32_t cycles = hal_vi_cycles_to_field();
    if (cycles != UINT32_MAX) hal_clock_advance(cycles);
}

/* ---- XFB 捕获 ---- */
static uint8_t *rgba_buf;
static size_t   rgba_cap;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static int vi_capture_geometry(uint32_t *top, uint32_t *bottom,
                               uint32_t *width, uint32_t *height,
                               uint32_t *stride, uint32_t *xoff)
{
    uint32_t top_reg = vi_xfb_reg(0x1C);
    uint32_t pic = vi_r[0x48 / 2];
    uint32_t acv = (vi_r[0x00 / 2] >> 4) & 0x3FFu;
    uint32_t wpl = (pic >> 8) & 0x7Fu;
    uint32_t std = pic & 0xFFu;
    bool nin = (vi_r[0x02 / 2] & 0x0004u) != 0;
    uint32_t rows;
    uint64_t row_bytes;
    uint64_t source_stride;

    if (!wpl || !std || !acv) return -1;
    *xoff = (top_reg >> 24) & 0xFu;
    *width = wpl * 16u;
    if (*xoff >= *width) return -1;
    /* WPL is the fetched picture width; XOFF skips the left pixels in the
     * first fetched word, so the host-visible width is the remainder. */
    *width -= *xoff;
    rows = nin ? acv : acv * 2u;
    if (!rows || rows > INT_MAX || *width > INT_MAX) return -1;

    source_stride = (uint64_t)std * 32u;
    row_bytes = ((uint64_t)*xoff + *width) * 2u;
    if (source_stride < row_bytes || source_stride > UINT32_MAX) return -1;

    *top = hal_ea_to_phys(vi_xfb_address(0x1C, (top_reg & 0x10000000u) != 0));
    *bottom = hal_ea_to_phys(vi_xfb_address(0x24, (top_reg & 0x10000000u) != 0));
    *height = rows;
    *stride = (uint32_t)source_stride;
    return 0;
}

static int vi_range_valid(uint32_t base, uint32_t row_offset,
                          uint32_t rows, uint32_t stride, uint64_t row_bytes)
{
    uint64_t start = (uint64_t)base + row_offset;
    uint64_t end;
    if (row_bytes == 0 || rows == 0) return 0;
    end = start + (uint64_t)(rows - 1u) * stride + row_bytes;
    return start <= GC_MEM1_SIZE && end <= GC_MEM1_SIZE;
}

static void vi_yuyv_pixel(uint8_t *dst, int y, int cb, int cr)
{
    int r = y + ((91881 * cr) >> 16);
    int g = y - ((22554 * cb + 46802 * cr) >> 16);
    int b = y + ((116130 * cb) >> 16);
    dst[0] = (uint8_t)clampi(r, 0, 255);
    dst[1] = (uint8_t)clampi(g, 0, 255);
    dst[2] = (uint8_t)clampi(b, 0, 255);
    dst[3] = 255;
}

static void vi_convert_row(const uint8_t *src, uint8_t *dst, uint32_t width)
{
    uint32_t x = 0;
    for (; x + 1u < width; x += 2u) {
        int y0 = src[0], cb = src[1] - 128;
        int y1 = src[2], cr = src[3] - 128;
        vi_yuyv_pixel(dst, y0, cb, cr);
        vi_yuyv_pixel(dst + 4, y1, cb, cr);
        src += 4;
        dst += 8;
    }
    if (x < width) {
        /* An odd visible width has no complete second pixel. Consume only the
         * two bytes belonging to that pixel and use neutral Cr; never borrow
         * bytes from the next row or field. */
        vi_yuyv_pixel(dst, src[0], src[1] - 128, 0);
    }
}

int hal_get_frame(uint8_t **rgba, int *w, int *h)
{
    hal_clock_sync_devices();
    uint32_t top, bottom, width, height, stride, xoff;
    uint64_t row_bytes;
    size_t need;
    bool nin;

    if (vi_capture_geometry(&top, &bottom, &width, &height, &stride, &xoff) != 0)
        goto invalid;
    row_bytes = ((uint64_t)xoff + width) * 2u;
    nin = (vi_r[0x02 / 2] & 0x0004u) != 0;
    if (!vi_range_valid(top, xoff * 2u, nin ? height : height / 2u,
                        stride, row_bytes))
        goto invalid;
    if (!nin && !vi_range_valid(bottom, xoff * 2u, height / 2u,
                                stride, row_bytes))
        goto invalid;

    if ((uint64_t)width * height > SIZE_MAX / 4u)
        goto invalid;
    need = (size_t)width * height * 4u;
    if (!rgba_buf || rgba_cap < need) {
        uint8_t *new_buf = (uint8_t *)realloc(rgba_buf, need);
        if (!new_buf) goto invalid;
        rgba_buf = new_buf;
        rgba_cap = need;
    }

    for (uint32_t y = 0; y < height; y++) {
        uint32_t field_y = nin ? y : y / 2u;
        uint32_t base = (nin || !(y & 1u)) ? top : bottom;
        const uint8_t *src = gc_ram + base + xoff * 2u + (size_t)field_y * stride;
        vi_convert_row(src, rgba_buf + (size_t)y * width * 4u, width);
    }
    if (rgba) *rgba = rgba_buf;
    if (w) *w = (int)width;
    if (h) *h = (int)height;
    return 0;

invalid:
    if (rgba) *rgba = NULL;
    if (w) *w = 0;
    if (h) *h = 0;
    return -1;
}
