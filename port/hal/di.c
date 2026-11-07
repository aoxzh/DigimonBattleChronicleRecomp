/* hal/di.c — DVD 状态机：TSTART 写触发 → disc.iso 直通 DMA → TCINT + PI 线。 */
#include "di.h"
#include "hal.h"
#include "mem.h"
#include "gc_abi.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static FILE *iso;
static uint64_t iso_size;

static uint32_t disr, dicvr;
static uint32_t cmdbuf[3];
static uint32_t dimar, dilength, dicr, dimmbuf;

static uint64_t stat_reads, stat_bytes;

bool hal_di_open_iso(const char *path)
{
    if (iso) fclose(iso);
    iso = NULL; iso_size = 0;
    if (!path) return false;
    iso = fopen(path, "rb");
    if (!iso) return false;
    fseek(iso, 0, SEEK_END);
    iso_size = (uint64_t)ftell(iso);
    fseek(iso, 0, SEEK_SET);
    return true;
}

void hal_di_close_iso(void)
{
    if (iso) fclose(iso);
    iso = NULL; iso_size = 0;
}

void hal_di_init(void)
{
    disr = dicvr = 0;
    cmdbuf[0] = cmdbuf[1] = cmdbuf[2] = 0;
    dimar = dilength = dicr = dimmbuf = 0;
    stat_reads = stat_bytes = 0;
}

uint64_t hal_di_read_count(void) { return stat_reads; }
uint64_t hal_di_read_bytes(void) { return stat_bytes; }

static void di_update_interrupts(void)
{
    /* DVDInterface::UpdateInterrupts：DEINT&DEINTMASK || TCINT&TCINTMASK ||
     * BRKINT&BRKINTMASK || CVRINT&CVRINTMASK → PI INT_CAUSE_DI */
    /* 真实位序（Dolphin UDISR / 硬件）：BREAK=0, DEINTMASK=1, DEINT=2,
     * TCINTMASK=3, TCINT=4, BRKINTMASK=5, BRKINT=6 —— 标志位在配对高位。
     * r92 修复：旧实现 (0,4)(1,5)(2,6)(3,7) 配对错，游戏写 0x2A 使能三个
     * 掩码后，TCINT 置 bit0 永不满足条件，DVD 状态机第一条命令即停摆。 */
    bool set = ((disr & (1u << 2)) && (disr & (1u << 1))) ||
               ((disr & (1u << 4)) && (disr & (1u << 3))) ||
               ((disr & (1u << 6)) && (disr & (1u << 5))) ||
               ((dicvr & (1u << 2)) && (dicvr & (1u << 1)));
    if (set) hal_pi_set_interrupt(PI_INT_DI);
    else     hal_pi_clear_interrupt(PI_INT_DI);
}

/* 立即完成语义（M1 无寻道时延）：DMA 直通 + TCINT */
static int di_log = -1;
static void di_execute(void)
{
    if (di_log < 0) { const char *e = getenv("GC_DI_LOG"); di_log = e && *e && *e != '0'; }
    if (di_log)
        fprintf(stderr, "[DI] cmd=%02x sub=%02x off=%08llx len=%08x mar=%08x\n",
                cmdbuf[0] >> 24, cmdbuf[0] & 0xFF,
                (unsigned long long)((uint64_t)cmdbuf[1] << 2),
                dilength, dimar);
    uint8_t cmd = cmdbuf[0] >> 24;
    uint8_t sub = cmdbuf[0] & 0xFF;

    switch (cmd) {
    case 0xA8: {  /* Read */
        if (sub == 0x40) {
            /* Read DiscID：DMA 到 MAR，长度取 min(0x20, LEN) */
            uint8_t hdr[0x20];
            if (iso && fseek(iso, 0, SEEK_SET) == 0 &&
                fread(hdr, 1, sizeof(hdr), iso) == sizeof(hdr)) {
                uint32_t n = dilength < 0x20u ? dilength : 0x20u;
                if (n) hal_mem_write_burst(hal_ea_to_phys(dimar), hdr, n);
                stat_reads++; stat_bytes += n;
            }
        } else {
            /* 扇区读：偏移 = CMDBUF1 << 2，源长 = CMDBUF2，DMA 长 = DILENGTH */
            uint64_t off = (uint64_t)cmdbuf[1] << 2;
            uint32_t len = dilength;
            if (iso && off < iso_size && len) {
                uint64_t max = iso_size - off;
                if ((uint64_t)len > max) len = (uint32_t)max;
                uint8_t tmp[512];
                if (fseek(iso, (long)off, SEEK_SET) == 0) {
                    uint32_t done = 0;
                    while (done < len) {
                        uint32_t chunk = len - done > sizeof(tmp) ? (uint32_t)sizeof(tmp) : len - done;
                        if (fread(tmp, 1, chunk, iso) != chunk) break;
                        hal_mem_write_burst(hal_ea_to_phys(dimar + done), tmp, chunk);
                        done += chunk;
                    }
                    stat_reads++; stat_bytes += done;
                }
            }
        }
        break;
    }
    case 0x12: {
        /* bit25 置位 = Inquiry（IMM 返回修订号）；否则 = RequestError
         * （DMA 32 字节错误码到 MAR，无错 = 全 0。r41：游戏引导期发
         * 0x12000000（bit25=0），旧实现误当 Inquiry → DMA 没跑，卡加载）。 */
        if (cmdbuf[0] & 0x02000000u) {
            dimmbuf = 0x00000002;
        } else {
            uint8_t err[0x20];
            memset(err, 0, sizeof(err));
            hal_mem_write_burst(hal_ea_to_phys(dimar), err, sizeof(err));
        }
        break;
    }
    case 0xE3: {  /* StopMotor：直接完成 */
        break;
    }
    default:
        break;
    }

    /* 完成：DVDInterface::FinishExecuting（TCINT 分支） */
    dicr &= ~1u;                       /* TSTART = 0（ready） */
    if (dilength) dimar += dilength;   /* MAR += LEN（DMA 模式） */
    dilength = 0;
    disr |= 1u << 4;                   /* TCINT (bit4，真实位序) */
    di_update_interrupts();
}

uint32_t hal_di_read32(uint32_t reg)
{
    if (di_log > 0)
        fprintf(stderr, "[DI-r] reg=%02x disr=%08x pc=%08x msr=%08x\n",
                reg, disr, gc.pc, gc.msr);
    switch (reg) {
    case 0x00: return disr;
    case 0x04: return dicvr;
    case 0x08: return cmdbuf[0];
    case 0x0C: return cmdbuf[1];
    case 0x10: return cmdbuf[2];
    case 0x14: return dimar;
    case 0x18: return dilength;
    case 0x1C: return dicr;
    case 0x20: return dimmbuf;
    case 0x24: return 0x00000001;      /* DICFG：CONFIG=1（Dolphin 口径；原 0x911 无依据） */
    default:   return 0;
    }
}

void hal_di_write32(uint32_t reg, uint32_t v)
{
    if (di_log > 0)
        fprintf(stderr, "[DI-w] reg=%02x <- %08x pc=%08x msr=%08x\n", reg, v, gc.pc, gc.msr);
    switch (reg) {
    case 0x00:  /* DISR：标志位 DEINT(0x04)/TCINT(0x10)/BRKINT(0x40) W1C；
                 * 掩码位 DEINTMASK(0x02)/TCINTMASK(0x08)/BRKINTMASK(0x20) 直写。
                 * BREAK(bit0) 写入 = 停转命令，此处仅吸收。 */
        disr &= ~(v & 0x54u);
        disr = (disr & ~0x2Au) | (v & 0x2Au);
        di_update_interrupts();
        break;
    case 0x04:  /* DICVR：bit2 CVRINT W1C；bit1 CVRINTMASK；bit0 COVER */
        if (v & (1u << 2)) dicvr &= ~(1u << 2);
        dicvr = (dicvr & ~0x2u) | (v & 0x2u);
        di_update_interrupts();
        break;
    case 0x08: cmdbuf[0] = v; break;
    case 0x0C: cmdbuf[1] = v; break;
    case 0x10: cmdbuf[2] = v; break;
    case 0x14: dimar = v & ~0x1Fu; break;
    case 0x18: dilength = v & ~0x1Fu; break;
    case 0x1C:
        dicr = v & 0x7u;
        if (dicr & 1u) di_execute();   /* TSTART */
        break;
    case 0x20: dimmbuf = v; break;
    default: break;
    }
}
