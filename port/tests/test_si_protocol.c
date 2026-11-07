/* Standalone SI protocol tests; no CPU, disc, window, or game execution.
 * cc -std=c11 -Wall -Wextra -Werror -Ihal tests/test_si_protocol.c hal/si.c \
 *    -o /tmp/test_si_protocol && /tmp/test_si_protocol
 */
#define _POSIX_C_SOURCE 200809L
#include "hal.h"
#include "si.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef _WIN32
#define setenv(name, value, overwrite) ((void)(overwrite), _putenv_s((name), (value)))
#define unsetenv(name) _putenv_s((name), "")
#endif

static uint32_t pi;
static uint64_t frame;
void hal_pi_set_interrupt(uint32_t bits) { pi |= bits; }
void hal_pi_clear_interrupt(uint32_t bits) { pi &= ~bits; }
uint64_t hal_frame_count(void) { return frame; }

#define TC 0x80000000u
#define TC_MASK 0x40000000u
#define COMERR 0x20000000u
#define RDST 0x10000000u
#define RDST_MASK 0x08000000u

static void reset(void)
{
    frame = 0;
    hal_si_init();
    assert(!(pi & PI_INT_SI));
}

/* Mirrors the guest SDK's request RAM -> CSR -> reply RAM path. */
static uint32_t transfer(unsigned channel, uint32_t request,
                         unsigned outlen, unsigned inlen, uint32_t mask)
{
    hal_si_write32(0x80, request);
    uint32_t csr = TC | mask | ((outlen & 127u) << 16) |
                   ((inlen & 127u) << 8) | (channel << 1) | 1u;
    hal_si_write32(0x34, csr);
    csr = hal_si_read32(0x34);
    assert(!(csr & 1));
    assert(csr & TC);
    assert((csr & 0x007F7F06u) ==
           (((outlen & 127u) << 16) | ((inlen & 127u) << 8) | (channel << 1)));
    return csr;
}

static void set_mode(unsigned channel, unsigned mode)
{
    hal_si_write32(0x30, 0); /* SDK disables polling before changing mode. */
    hal_si_write32(channel * 12, 0x00400000u | (mode << 8));
    hal_si_write32(0x38, 0x80000000u); /* copy output command to device */
    assert(!(hal_si_read32(0x38) & 0x90101010u));
}

static void test_buffer_and_transfers(void)
{
    reset();
    for (uint32_t r = 0x80; r <= 0xFC; r += 4)
        hal_si_write32(r, 0xA0B0C000u | r);
    for (uint32_t r = 0x80; r <= 0xFC; r += 4)
        assert(hal_si_read32(r) == (0xA0B0C000u | r));
    hal_si_write32(0x100, 0xDEADBEEFu);
    hal_si_write32(0xFD, 0xDEADBEEFu);
    assert(hal_si_read32(0xFC) == 0xA0B0C0FCu);
    assert(hal_si_read32(0x100) == 0);
    assert(hal_si_read32(0xFD) == 0);
    assert(hal_si_read32(0x40) == 0);

    /* OUT is deliberately different: TSTART must use communication RAM. */
    hal_si_write32(0, 0x00EE0000u);
    assert(!(transfer(0, 0x00112233u, 1, 3, 0) & COMERR));
    assert(hal_si_read32(0x80) == 0x09000033u);
    assert(hal_si_read32(0x84) == 0xA0B0C084u);
    assert(!(pi & PI_INT_SI)); /* completion latches while masked */
    hal_si_write32(0x34, TC_MASK);
    assert(pi & PI_INT_SI);   /* enable after completion */
    hal_si_write32(0x34, TC_MASK | TC);
    assert(!(pi & PI_INT_SI));
    assert(!(hal_si_read32(0x34) & TC));

    assert(!(transfer(0, 0xFF445566u, 1, 2, TC_MASK) & COMERR));
    assert(hal_si_read32(0x80) == 0x09005566u); /* exact receive length */
    assert(pi & PI_INT_SI);
    hal_si_write32(0x34, 0);
    assert(hal_si_read32(0x34) & TC); /* mask write must not clear TC */
    assert(!(pi & PI_INT_SI));

    /* Zero length field means 128, not zero, and stays within RAM. */
    assert(!(transfer(0, 0, 0, 0, 0) & COMERR));
    assert(hal_si_read32(0x80) == 0x09000000u);
    for (uint32_t r = 0x84; r <= 0xFC; r += 4)
        assert(hal_si_read32(r) == 0);

    /* Reject a short direct command; a later success clears COMERR. */
    assert(transfer(0, 0x40000000u, 1, 8, 0) & COMERR);
    assert(transfer(0, 0xEE000000u, 1, 3, 0) & COMERR);
    assert(!(transfer(0, 0, 1, 3, 0) & COMERR));
    assert(hal_si_read32(0x38) & 0x08000000u); /* sticky until W1C */
    hal_si_write32(0x38, 0x08000000u);
    assert(!(hal_si_read32(0x38) & 0x08000000u));
}

static void test_origin_and_direct(void)
{
    reset();
    hal_si_set_pad_raw(0, 0x1101, 0xC8, 0x40, 0xAB, 0xCD, 0xE5, 0x67);
    for (unsigned cmd = 0x41; cmd <= 0x42; cmd++) {
        hal_si_write32(0x88, 0x11223344);
        assert(!(transfer(0, cmd << 24, cmd == 0x41 ? 1 : 3, 10, 0) & COMERR));
        assert(hal_si_read32(0x80) == 0x00808080u);
        assert(hal_si_read32(0x84) == 0x80800000u);
        assert(hal_si_read32(0x88) == 0x00003344u);
    }
    assert(!(transfer(0, 0x40030000u, 3, 8, TC_MASK) & COMERR));
    assert(hal_si_read32(0x80) == 0x1181C840u);
    assert(hal_si_read32(0x84) == 0xABCDE567u);
    assert(hal_si_read32(4) == 0); /* transfer must not fill poll registers */
}

static void test_poll_latching_and_interrupts(void)
{
    reset();
    hal_si_set_pad_raw(0, 0x1101, 0xC8, 0x40, 0xAB, 0xCD, 0xE5, 0x67);
    set_mode(0, 3);
    hal_si_write32(0x34, RDST_MASK);
    hal_si_frame_tick(); /* polling disabled */
    assert(hal_si_read32(4) == 0);
    assert(!(hal_si_read32(0x34) & RDST));
    hal_si_write32(0x30, 0x80);
    hal_si_frame_tick();
    assert(hal_si_read32(0x38) & 0x20000000u);
    assert(hal_si_read32(0x34) & RDST);
    assert(pi & PI_INT_SI);
    hal_si_write32(0x34, RDST_MASK | RDST);
    assert(pi & PI_INT_SI); /* derived from unread per-channel RDST */
    hal_si_write32(0x38, 0x20000000u);
    assert(pi & PI_INT_SI); /* RDST is not W1C in SISR */
    hal_si_set_pad_raw(0, 0, 0x80, 0x80, 0x80, 0x80, 0, 0);
    assert(hal_si_read32(4) == 0x1181C840u); /* held snapshot until next tick */
    assert(!(pi & PI_INT_SI));
    assert(!(hal_si_read32(0x34) & RDST));
    assert(hal_si_read32(8) == 0xABCDE567u);
    hal_si_frame_tick();
    assert(pi & PI_INT_SI);
    assert(hal_si_read32(8) == 0x80800000u); /* low read also acknowledges */
    assert(!(pi & PI_INT_SI));
    assert(hal_si_read32(4) == 0x00808080u);

    /* Simultaneous transfer and polling IRQ causes are independent. */
    hal_si_frame_tick();
    transfer(0, 0, 1, 3, TC_MASK | RDST_MASK);
    hal_si_read32(4);
    assert(pi & PI_INT_SI); /* TC still pending */
    hal_si_write32(0x34, TC_MASK | RDST_MASK | TC);
    assert(!(pi & PI_INT_SI));
}

static void test_modes(void)
{
    static const uint32_t expected[8] = {
        0xABCDE600u, 0xACE56700u, 0xACE60000u, 0xABCDE567u,
        0xABCD0000u, 0xABCDE600u, 0xABCDE600u, 0xABCDE600u
    };
    reset();
    hal_si_set_pad_raw(0, 0xFFFF, 0x23, 0x45, 0xAB, 0xCD, 0xE5, 0x67);
    for (unsigned mode = 0; mode < 8; mode++) {
        set_mode(0, mode);
        hal_si_write32(0x30, 0x80);
        hal_si_frame_tick();
        assert(hal_si_read32(4) == 0x1FFF2345u); /* invalid button bits stripped */
        assert(hal_si_read32(8) == expected[mode]);
    }
}

static void test_channels_and_legacy(void)
{
    reset();
    for (unsigned c = 1; c < 4; c++) {
        assert(transfer(c, 0, 1, 3, TC_MASK) & COMERR);
        assert(hal_si_read32(0x38) & (0x08000000u >> (8 * c)));
        assert(pi & PI_INT_SI);
    }
    hal_si_write32(0x38, 0x0F0F0F0Fu);
    assert(hal_si_read32(0x38) == 0);
    hal_si_write32(0x34, TC | RDST_MASK);
    hal_si_write32(0x30, 0xF0);
    hal_si_frame_tick();
    assert(hal_si_read32(0x38) == 0x20080808u);
    assert(hal_si_read32(16) == 0xC0000000u);
    assert(hal_si_read32(4) == 0x00808080u);
    assert(!(pi & PI_INT_SI));
    for (unsigned c = 1; c < 4; c++) {
        hal_si_set_connected(c, 1);
        hal_si_set_pad_raw(c, (uint16_t)(1u << c), 0x80, 0x80, 0x80, 0x80, 0, 0);
        assert(!(transfer(c, 0, 1, 3, 0) & COMERR));
    }
    hal_si_write32(0x38, 0x0F0F0F0Fu);
    hal_si_frame_tick();
    assert(hal_si_read32(0x38) == 0x20202020u);
    for (unsigned c = 1; c < 4; c++)
        assert((hal_si_read32(c * 12 + 4) & 0x1F7F0000u) == (1u << (c + 16)));

    hal_si_set_pad(0, 0x1FF0, 0x80, 0x80, 0x80, 0x80, 0, 0);
    hal_si_frame_tick();
    assert((hal_si_read32(4) & 0x1F7F0000u) == 0);
    hal_si_set_pad(0, 0x1EF0, 0x80, 0x80, 0x80, 0x80, 0, 0);
    hal_si_frame_tick();
    assert((hal_si_read32(4) & 0x1F7F0000u) == 0x01000000u);
    hal_si_set_connected(4, 1);
    hal_si_set_pad_raw(4, 0xFFFF, 1, 2, 3, 4, 5, 6);
    hal_si_set_pad(4, 0, 1, 2, 3, 4, 5, 6);
    hal_si_set_connected(0, 0);
    assert(transfer(0, 0, 1, 3, 0) & COMERR);
    reset();
    assert(!(transfer(0, 0, 1, 3, 0) & COMERR));
    assert(transfer(1, 0, 1, 3, 0) & COMERR);
}

static void test_script(void)
{
    char path[] = "/tmp/si-protocol-input-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *f = fdopen(fd, "w");
    assert(f);
    fputs("[{\"frame\":1,\"button\":\"A\",\"state\":\"press\"},"
          "{\"frame\":1,\"button\":\"LEFT\",\"state\":\"press\"},"
          "{\"frame\":1,\"axis\":\"STICK_X\",\"value\":200},"
          "{\"frame\":2,\"button\":\"A\",\"state\":\"release\"},"
          "{\"frame\":1,\"channel\":1,\"connected\":1},"
          "{\"frame\":1,\"channel\":1,\"button\":\"B\",\"state\":\"press\"},"
          "{\"frame\":2,\"channel\":1,\"connected\":0}]", f);
    assert(fclose(f) == 0);
    assert(setenv("GC_INPUT_SCRIPT", path, 1) == 0);
    reset();
    assert(unlink(path) == 0);
    assert(transfer(1, 0, 1, 3, 0) & COMERR);
    hal_si_write32(0x30, 0xC0);
    hal_si_frame_tick();
    assert(hal_si_read32(4) == 0x00808080u);
    frame = 1;
    hal_si_frame_tick();
    assert(hal_si_read32(4) == 0x0181C880u);
    /* Reconnection supplies B and centered sticks; the prior error stays latched. */
    assert(hal_si_read32(16) == 0x42808080u);
    assert(!(transfer(1, 0, 1, 3, 0) & COMERR));
    frame = 2;
    hal_si_frame_tick();
    assert(hal_si_read32(4) == 0x0081C880u);
    assert(transfer(1, 0, 1, 3, 0) & COMERR);
    assert(unsetenv("GC_INPUT_SCRIPT") == 0);
    reset(); /* releases replay storage */
}

int main(void)
{
    assert(unsetenv("GC_INPUT_SCRIPT") == 0);
    test_buffer_and_transfers();
    test_origin_and_direct();
    test_poll_latching_and_interrupts();
    test_modes();
    test_channels_and_legacy();
    test_script();
    puts("SI protocol: buffer, SDK transfers, IRQs, modes, channels, legacy, replay passed");
    return 0;
}
