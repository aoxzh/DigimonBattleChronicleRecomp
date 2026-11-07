/* tests/gx_vertex_size_selftest.c — focused GX FIFO vertex stride checks.
 *
 * The parser does not draw. Each case feeds one configured primitive followed by
 * a BP EFB address command. Exact command count, byte consumption, and marker
 * visibility prove that vertex_size stopped at the GX-defined stream boundary.
 *
 * Reference semantics: Dolphin VertexLoaderBase::GetVertexSize,
 * VertexLoader_Position/Normal/TextCoord::GetSize, and CPMemory.h UVAT_group0/1/2.
 */
#include "../hal/hal.h"
#include "../hal/gc_abi.h"
#include "../hal/gx.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int fails;

#define CHECK(cond, name, value) \
    do { \
        if (cond) printf("PASS %s\n", (name)); \
        else { printf("FAIL %s (value=%u)\n", (name), (unsigned)(value)); fails++; } \
    } while (0)

static size_t put_cp(uint8_t *out, size_t at, uint8_t sub, uint32_t value)
{
    out[at++] = 0x08;
    out[at++] = sub;
    out[at++] = (uint8_t)(value >> 24);
    out[at++] = (uint8_t)(value >> 16);
    out[at++] = (uint8_t)(value >> 8);
    out[at++] = (uint8_t)value;
    return at;
}

static int run_case(const char *name, uint32_t vcd_lo, uint32_t vcd_hi,
                    uint32_t vat_a, uint32_t vat_b, uint32_t vat_c,
                    size_t vertex_bytes)
{
    uint8_t stream[128];
    size_t at = 0;
    const uint32_t marker = 0x1234u;
    const size_t expected_commands = 8; /* 5 CP loads + primitive + BP + NOP */

    at = put_cp(stream, at, 0x50, vcd_lo);
    at = put_cp(stream, at, 0x60, vcd_hi);
    at = put_cp(stream, at, 0x70, vat_a);
    at = put_cp(stream, at, 0x80, vat_b);
    at = put_cp(stream, at, 0x90, vat_c);
    stream[at++] = 0x90; /* QUAD, VAT 0 */
    stream[at++] = 0x00;
    stream[at++] = 0x01; /* one vertex */
    memset(stream + at, 0xA5, vertex_bytes);
    at += vertex_bytes;
    stream[at++] = 0x61; /* LOAD_BP */
    stream[at++] = 0x4B; /* BPMEM_EFB_ADDR */
    stream[at++] = (uint8_t)(marker >> 16);
    stream[at++] = (uint8_t)(marker >> 8);
    stream[at++] = (uint8_t)marker;
    while (at & 31u) stream[at++] = 0x00;

    hal_irq_init();
    hal_gx_init();
    hal_pi_write32(0x0C, 0x8000F000u);
    hal_pi_write32(0x10, 0x80010000u);
    hal_pi_write32(0x14, 0x8000F000u);
    hal_cp_write16(0x20, 0xF000u);
    hal_cp_write16(0x24, 0x0000u);
    hal_cp_write16(0x26, 0x0001u);
    hal_cp_write16(0x34, 0xF000u);
    hal_cp_write16(0x38, 0xF000u);
    hal_cp_write16(0x02, 0x11u);
    for (size_t i = 0; i < at; i++) hal_gxpipe_write8(stream[i]);

    char label[128];
    snprintf(label, sizeof(label), "%s consumed", name);
    CHECK(hal_gx_bytes_consumed() == at, label,
          (unsigned)(hal_gx_bytes_consumed() - at));
    snprintf(label, sizeof(label), "%s commands", name);
    CHECK(hal_gx_cmd_count() == expected_commands, label, hal_gx_cmd_count());
    snprintf(label, sizeof(label), "%s marker", name);
    CHECK(hal_gx_efb_copy_dst() == (marker << 5), label, hal_gx_efb_copy_dst());
    return 0;
}

int main(void)
{
    /* Position: direct XYZ float = 3 * 4 bytes. */
    run_case("direct position XYZ float", 1u << 9, 0,
              1u | (4u << 1), 0, 0, 12);

    /* Position index16 is one 16-bit index, regardless of XY/XYZ VAT shape. */
    run_case("indexed position16 XYZ", 3u << 9, 0,
              1u | (4u << 1), 0, 0, 2);

    /* Normal: direct N float = 3 * 4; NTB = 9 * 4. */
    run_case("direct normal N float", 1u << 11, 0, 4u << 10, 0, 0, 12);
    run_case("direct normal NTB float", 1u << 11, 0,
              (1u << 9) | (4u << 10), 0, 0, 36);

    /* Indexed normals consume one index, or three with NormalIndex3.
     * Index16 is two bytes per index, independent of component format. */
    run_case("indexed normal16 N", 3u << 11, 0, 0, 0, 0, 2);
    run_case("indexed normal16 NTB", 3u << 11, 0,
              1u << 31, 0, 0, 6);

    /* Tex4 coordinate count/format are VAT_B bits 27/28..30. VAT_C bits 0..4
     * are Tex4Frac and must not change the FIFO vertex length. */
    run_case("tex4 direct ST float with VAT_C frac", 0, 1u << 8,
              0, (1u << 27) | (4u << 28), 0x1Fu, 8);
    run_case("tex4 indexed16 ST with VAT_C frac", 0, 3u << 8,
              0, (1u << 27) | (4u << 28), 0x1Fu, 2);

    printf(fails ? "== %d FAIL ==\n" : "== ALL GREEN ==\n", fails);
    return fails ? 1 : 0;
}
