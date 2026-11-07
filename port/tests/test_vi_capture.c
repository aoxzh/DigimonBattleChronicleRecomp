/* Standalone VI/XFB capture regression tests.
 * Build from port/ without changing the project Makefile:
 *   clang -std=c11 -Wall -Wextra -g -fsanitize=address,undefined \
 *     -Ihal tests/test_vi_capture.c hal/vi.c -o /tmp/test_vi_capture \
 *     && /tmp/test_vi_capture
 */
#include "../hal/vi.h"
#include "../hal/hal.h"
#include "../hal/mem.h"
#include "../hal/gc_abi.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

GcCtx gc;
uint8_t gc_ram[GC_MEM1_SIZE];

static uint32_t pi_cause;
static uint64_t frame_count;

void hal_pi_set_interrupt(uint32_t bits) { pi_cause |= bits; }
void hal_pi_clear_interrupt(uint32_t bits) { pi_cause &= ~bits; }
void hal_irq_frame_event(void) { frame_count++; }
void hal_clock_advance(uint32_t cycles) { hal_vi_advance(cycles); }
uint64_t hal_frame_count(void) { return frame_count; }

static int failures;

#define CHECK(expr, name) do { \
    if (!(expr)) { fprintf(stderr, "FAIL %s\n", (name)); failures++; } \
    else { printf("PASS %s\n", (name)); } \
} while (0)

static void put16(uint32_t reg, uint16_t value)
{
    hal_vi_write16(reg, value);
}

static void put_xfb(uint32_t hi_reg, uint32_t raw, unsigned xoff, int poff)
{
    uint32_t value = (raw & 0x00FFFFFFu) |
                     ((uint32_t)(xoff & 0xFu) << 24) |
                     (poff ? 0x10000000u : 0u);
    put16(hi_reg, (uint16_t)(value >> 16));
    put16(hi_reg + 2u, (uint16_t)value);
}

static void yuyv_row(uint32_t base, uint32_t stride, uint8_t y, uint8_t cb,
                     uint8_t cr, uint32_t width)
{
    for (uint32_t x = 0; x + 1u < width; x += 2u) {
        gc_ram[base + x * 2u + 0] = y;
        gc_ram[base + x * 2u + 1] = cb;
        gc_ram[base + x * 2u + 2] = y;
        gc_ram[base + x * 2u + 3] = cr;
    }
    if (width & 1u) {
        gc_ram[base + (width - 1u) * 2u + 0] = y;
        gc_ram[base + (width - 1u) * 2u + 1] = cb;
    }
    (void)stride;
}

static void test_address_decode(void)
{
    hal_vi_init();
    put_xfb(0x1C, 0x00123456u, 0, 0);
    CHECK(hal_vi_xfb_top() == 0x00123456u, "full 32-bit FBB without POFF");
    put_xfb(0x1C, 0x00001000u, 0, 1);
    CHECK(hal_vi_xfb_top() == 0x00020000u, "POFF expands FBB in 32-byte units");
}

static void test_capture_geometry_and_fields(void)
{
    const uint32_t top = 0x00020000u;
    const uint32_t bottom = 0x00024000u;
    const uint32_t stride = 4u * 32u;
    const uint32_t width = 63u;
    uint8_t *rgba = NULL;
    int w = 0, h = 0;

    hal_vi_init();
    put_xfb(0x1C, 0x00001000u, 1, 1);
    put_xfb(0x24, 0x00001200u, 0, 0); /* bottom uses top POFF */
    put16(0x48, (uint16_t)((4u << 8) | 4u)); /* WPL=4, STD=4 */
    put16(0x00, (uint16_t)(2u << 4));         /* ACV=2 lines per field */
    put16(0x02, 0);                           /* interlaced: 4 output lines */

    for (uint32_t field_y = 0; field_y < 2u; field_y++) {
        yuyv_row(top + 2u + field_y * stride, stride, (uint8_t)(80u + field_y),
                 128, 128, width);
        yuyv_row(bottom + 2u + field_y * stride, stride, (uint8_t)(160u + field_y),
                 128, 128, width);
    }

    CHECK(hal_get_frame(&rgba, &w, &h) == 0, "capture succeeds from 0x48 config");
    CHECK(w == (int)width && h == 4, "WPL/XOFF and ACV determine output size");
    CHECK(rgba && rgba[0] == 80 && rgba[(size_t)width * 4u] == 160,
          "top and bottom fields are interleaved");
    CHECK(rgba && rgba[(size_t)(width - 1u) * 4u + 3u] == 255,
          "odd final pixel stays within its row");

    put16(0x02, 0x0004u); /* NIN: progressive, one top-field frame */
    CHECK(hal_get_frame(&rgba, &w, &h) == 0 && h == 2,
          "NIN selects one ACV-height frame");
}

static void test_bounds(void)
{
    uint8_t *rgba = NULL;
    int w = 99, h = 99;

    hal_vi_init();
    put_xfb(0x1C, 0x000C0000u, 0, 1);
    put16(0x48, (uint16_t)((4u << 8) | 4u));
    put16(0x00, (uint16_t)(2u << 4));
    put16(0x02, 0x0004u);
    int rc = hal_get_frame(&rgba, &w, &h);
    CHECK(rc == -1 && !rgba && w == 0 && h == 0,
          "capture rejects source range beyond MEM1");
}

static void test_reset_and_interrupt_line(void)
{
    hal_vi_init();
    pi_cause = 0;
    put16(0, 0x0f06u);
    put16(6, 429);
    put16(0x0c, 3); put16(0x0e, 24);
    put16(0x10, 2); put16(0x12, 25);
    put16(0x02, 1);
    put16(0x30, 0x1107u); put16(0x32, 430); /* DI0: second field start */
    hal_frame_tick();
    CHECK((hal_vi_reg(0x30) & 0x8000u) != 0 &&
          (pi_cause & PI_INT_VI) != 0, "retrace asserts VI interrupt");

    put16(0x02, 0x0003u); /* ENB + RST */
    CHECK((hal_vi_reg(0x02) & 0x0003u) == 0x0001u,
          "RST self-clears while ENB is retained");
    CHECK(hal_vi_reg(0x30) == 0 && (pi_cause & PI_INT_VI) == 0,
          "RST clears interrupt state and PI VI line");
}

int main(void)
{
    memset(gc_ram, 0, sizeof(gc_ram));
    test_address_decode();
    test_capture_geometry_and_fields();
    test_bounds();
    test_reset_and_interrupt_line();
    if (failures) {
        fprintf(stderr, "%d VI test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    puts("ALL VI TESTS PASS");
    return EXIT_SUCCESS;
}
