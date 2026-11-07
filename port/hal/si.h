/* GameCube Serial Interface controller input.
 * MMIO: channel OUT/IN_HI/IN_LO at 0x00..0x2c (stride 12), POLL=0x30,
 * COMCSR=0x34, SISR=0x38, EXI clock=0x3c, communication RAM=0x80..0xff.
 * Guest controller packets use active-high SDK button bits plus USE_ORIGIN.
 * See docs/analysis/si-pad-protocol.md for references and timing limits. */
#ifndef HAL_SI_H
#define HAL_SI_H

#include <stdint.h>

void hal_si_init(void);
/* Apply GC_INPUT_SCRIPT events, then latch enabled channels and raise RDST.
 * Called by the existing virtual-frame event; no guest execution required. */
void hal_si_frame_tick(void);

/* Legacy host ABI: neutral=0x1ff0, A held=0x1ef0. Translates with XOR 0x1ff0.
 * New input backends should use the active-high raw setter below. */
void hal_si_set_pad(unsigned chan, uint16_t button16,
                    uint8_t stickx, uint8_t sticky,
                    uint8_t subx, uint8_t suby,
                    uint8_t trigL, uint8_t trigR);

/* Native SDK buttons, 1=pressed: LEFT/RIGHT/DOWN/UP=0x0001/2/4/8,
 * Z/R/L=0x0010/20/40, A/B/X/Y/START=0x0100/0200/0400/0800/1000.
 * Sticks 0..255 (center 128), triggers 0..255 (released 0).
 * Injection changes host state; the next enabled poll latches the packet.
 * Setters do not change connection state and run on the emulation thread. */
void hal_si_set_pad_raw(unsigned chan, uint16_t buttons,
                        uint8_t stickx, uint8_t sticky,
                        uint8_t subx, uint8_t suby,
                        uint8_t trigL, uint8_t trigR);
/* Initialization connects channel 0 only. Other channels must be attached
 * explicitly. Out-of-range channel indices are ignored by all setters. */
void hal_si_set_connected(unsigned chan, int connected);
/* Rumble motor state the guest last wrote to the channel's poll command
 * (OUT 0x40xxyy, low two bits: 1=run, 0/2=stop). */
int hal_si_rumble(unsigned chan);

#endif /* HAL_SI_H */
