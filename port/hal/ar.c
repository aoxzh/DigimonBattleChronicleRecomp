/* hal/ar.c — 16MB ARAM + DMA 引擎。 */
#include "ar.h"
#include "mem.h"

#include <string.h>

uint8_t gc_aram[GC_ARAM_SIZE_];

void hal_ar_init(void)
{
    memset(gc_aram, 0, sizeof(gc_aram));
}

uint8_t hal_ar_read8(uint32_t addr)
{
    return gc_aram[addr & (GC_ARAM_SIZE_ - 1)];
}

void hal_ar_write8(uint32_t addr, uint8_t v)
{
    gc_aram[addr & (GC_ARAM_SIZE_ - 1)] = v;
}

void hal_ar_dma(uint32_t mram_addr, uint32_t aram_addr, uint32_t len, int dir)
{
    uint32_t mp = mram_addr & 0x1FFFFFFFu;
    uint32_t ap = aram_addr & (GC_ARAM_SIZE_ - 1);
    uint32_t n = len & ~0x1Fu;   /* 低 5 位按 HW 掩掉（WMASK_LO_ALIGN_32BIT） */

    if (dir) {   /* ARAM → MRAM；传出每 64MB 镜像（MRAM 侧自然包裹在 MEM1） */
        for (uint32_t i = 0; i < n; i++) {
            uint32_t m = mp + i;
            if (m >= GC_MEM1_SIZE) break;
            gc_ram[m] = gc_aram[(ap + i) & (GC_ARAM_SIZE_ - 1)];
        }
    } else {     /* MRAM → ARAM */
        for (uint32_t i = 0; i < n; i++) {
            uint32_t m = mp + i;
            if (m >= GC_MEM1_SIZE) break;
            gc_aram[(ap + i) & (GC_ARAM_SIZE_ - 1)] = gc_ram[m];
        }
    }
}
