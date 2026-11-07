/* hal/mem.c — MEM1 大端 RAM、DOL 加载、引导前内存状态。
 *
 * 引导状态出处（位序/数值全部逐行核对）：
 *   Dolphin Source/Core/Core/Boot/Boot_BS2Emu.cpp : SetupGCMemory()
 *   https://raw.githubusercontent.com/dolphin-emu/dolphin/master/Source/Core/Core/Boot/Boot_BS2Emu.cpp
 *   - 0x80000020 = 0x0D15EA5E（boot magic，"booted from bootrom"）
 *   - 0x80000028 = 0x01800000（RAM 大小，GetRamSizeReal()）
 *   - 0x8000002C = 0x10000006（ConsoleType::LatestDevkit，
 *     Source/Core/Core/Core.h enum class ConsoleType { ... LatestDevkit = 0x10000006 }）
 *   - 0x800000CC = 0（NTSC 伪装 IPL 已 VI init；PAL 为 1）
 *   - 0x800000D0 = 0x01000000（ARAM 16MB）
 *   - 0x800000F8 = 0x09A7EC80（总线时钟 162MHz） / 0x800000FC = 0x1CF7C580（CPU 486MHz）
 *   - 0x80000300 / 0x80000800 / 0x80000C00 = 0x4C000064（rfi 指令兜底）
 *   - 0x800030D8 = 64 位 timebase tick（写 0；PresetTimeBaseTicks 的退化情形）
 * 另（本项目入口反编译补充，协调者 2026-10-13）：
 *   - 盘上 0x440 起 0x2000 字节 bi2 → 0x81700000（phys 0x01700000，仍在 MEM1 内），
 *     指针写 0x800000F4。实测零售盘 bi2 全 0（+0xC=0 ≤1 → 正常路径）。
 *     Dolphin 无此项；决策：写入（更真），SDK 读不到也无碍（我们的 VI/IRQ 直接提供）。
 *   - 入口写 0x80000044=0、读 0x800030E6（boot 状态字）——RAM 全零即满足。
 */
#include "mem.h"
#include "gc_abi.h"

#include <string.h>
#include <stdio.h>

uint8_t gc_ram[GC_MEM1_SIZE];
static uint32_t dol_memory_end; /* 引导保留区不得覆盖已装载的段或 BSS。 */

/* ---- 大端访问器 ---- */
uint8_t hal_mem_read8(uint32_t phys)  { return gc_ram[phys]; }
uint16_t hal_mem_read16(uint32_t phys)
{
    return (uint16_t)((gc_ram[phys] << 8) | gc_ram[phys + 1]);
}
uint32_t hal_mem_read32(uint32_t phys)
{
    return ((uint32_t)gc_ram[phys] << 24) | ((uint32_t)gc_ram[phys + 1] << 16) |
           ((uint32_t)gc_ram[phys + 2] << 8) | (uint32_t)gc_ram[phys + 3];
}
uint64_t hal_mem_read64(uint32_t phys)
{
    uint64_t hi = hal_mem_read32(phys);
    uint64_t lo = hal_mem_read32(phys + 4);
    return (hi << 32) | lo;
}
void hal_mem_write8(uint32_t phys, uint8_t v) { gc_ram[phys] = v; }
void hal_mem_write16(uint32_t phys, uint16_t v)
{
    gc_ram[phys]     = (uint8_t)(v >> 8);
    gc_ram[phys + 1] = (uint8_t)v;
}
void hal_mem_write32(uint32_t phys, uint32_t v)
{
    gc_ram[phys]     = (uint8_t)(v >> 24);
    gc_ram[phys + 1] = (uint8_t)(v >> 16);
    gc_ram[phys + 2] = (uint8_t)(v >> 8);
    gc_ram[phys + 3] = (uint8_t)v;
}
void hal_mem_write64(uint32_t phys, uint64_t v)
{
    hal_mem_write32(phys,     (uint32_t)(v >> 32));
    hal_mem_write32(phys + 4, (uint32_t)v);
}
void hal_mem_read_burst(uint32_t phys, uint8_t *dst, size_t n)
{
    memcpy(dst, gc_ram + phys, n);
}
void hal_mem_write_burst(uint32_t phys, const uint8_t *src, size_t n)
{
    memcpy(gc_ram + phys, src, n);
}

/* ---- DOL ---- */
#define DOL_N_TEXT 7
#define DOL_N_DATA 11

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

bool hal_dol_parse(const uint8_t *file, size_t filesize, HalDolHeader *out,
                   const char **err)
{
    if (filesize < 0x100) { *err = "dol too small"; return false; }
    for (int i = 0; i < DOL_N_TEXT; i++) {
        out->text_off[i]  = be32(file + 0x00 + 4 * i);
        out->text_addr[i] = be32(file + 0x48 + 4 * i);
        out->text_size[i] = be32(file + 0x90 + 4 * i);
    }
    for (int i = 0; i < DOL_N_DATA; i++) {
        out->data_off[i]  = be32(file + 0x1C + 4 * i);
        out->data_addr[i] = be32(file + 0x64 + 4 * i);
        out->data_size[i] = be32(file + 0xAC + 4 * i);
    }
    out->bss_addr = be32(file + 0xD8);
    out->bss_size = be32(file + 0xDC);
    out->entry    = be32(file + 0xE0);

    /* 边界校验：段必须在文件内且落在 MEM1 */
    for (int i = 0; i < DOL_N_TEXT; i++) {
        if (!out->text_size[i]) continue;
        if ((uint64_t)out->text_off[i] + out->text_size[i] > filesize) {
            *err = "text segment out of file"; return false;
        }
        if (hal_ea_to_phys(out->text_addr[i]) + (uint64_t)out->text_size[i] > GC_MEM1_SIZE) {
            *err = "text segment out of MEM1"; return false;
        }
    }
    for (int i = 0; i < DOL_N_DATA; i++) {
        if (!out->data_size[i]) continue;
        if ((uint64_t)out->data_off[i] + out->data_size[i] > filesize) {
            *err = "data segment out of file"; return false;
        }
        if (hal_ea_to_phys(out->data_addr[i]) + (uint64_t)out->data_size[i] > GC_MEM1_SIZE) {
            *err = "data segment out of MEM1"; return false;
        }
    }
    if (hal_ea_to_phys(out->bss_addr) + (uint64_t)out->bss_size > GC_MEM1_SIZE) {
        *err = "bss out of MEM1"; return false;
    }
    *err = NULL;
    return true;
}

bool hal_dol_load(const HalDolHeader *h, const uint8_t *file, size_t filesize,
                  const char **err)
{
    dol_memory_end = h->bss_size ? hal_ea_to_phys(h->bss_addr) + h->bss_size : 0;
    for (int i = 0; i < DOL_N_TEXT; i++) {
        uint32_t end = hal_ea_to_phys(h->text_addr[i]) + h->text_size[i];
        if (h->text_size[i] && end > dol_memory_end) dol_memory_end = end;
    }
    for (int i = 0; i < DOL_N_DATA; i++) {
        uint32_t end = hal_ea_to_phys(h->data_addr[i]) + h->data_size[i];
        if (h->data_size[i] && end > dol_memory_end) dol_memory_end = end;
    }
    /* 关键顺序：先清整个声明 bss [0x80369300, 0x803CFF68)。
     * 事实：data[3]@0x803CE120(0x1320) 与 bss 重叠；游戏入口自己只 memset
     * [0x80369300,0x803CE110) 和 [0x803CF440,0x803CFF68)——与 data[3] 不重叠，
     * 维持“清整个 bss 再拷段”即可，data[3] 后拷自然恢复。 */
    if (h->bss_size) {
        uint32_t bp = hal_ea_to_phys(h->bss_addr);
        memset(gc_ram + bp, 0, h->bss_size);
    }
    for (int i = 0; i < DOL_N_TEXT; i++) {
        if (!h->text_size[i]) continue;
        memcpy(gc_ram + hal_ea_to_phys(h->text_addr[i]),
               file + h->text_off[i], h->text_size[i]);
    }
    for (int i = 0; i < DOL_N_DATA; i++) {
        if (!h->data_size[i]) continue;
        memcpy(gc_ram + hal_ea_to_phys(h->data_addr[i]),
               file + h->data_off[i], h->data_size[i]);
    }
    (void)filesize; (void)err;
    return true;
}

/* ---- 引导前内存状态（BS2 等效） ---- */
bool hal_write_boot_state(FILE *disc_iso, const char **err)
{
    /* 磁头拷贝：只拷 DVD disk ID 前 0x20 字节 → 0x80000000（Dolphin
     * DVDReadDiscID 等效；盘 ID 只有 0x20 字节）。曾经拷 0x100 字节，
     * 把盘头标题 "Digimon Battle Chronicle" 留在 0x30-0xFF：真机/Dolphin
     * 该区是 0（0x30=console type），游戏拿 0x80000000 基址读 0x30/0x34
     * 得到 "hron"/"icle" 字符串字节 → 音频流位置/临界区掩码全被污染
     * → 越界 memset + EE 屏蔽 15.8M 轮询 → IRQ 派发 MISS。 */
    if (disc_iso) {
        uint8_t hdr[0x20];
        if (fseek(disc_iso, 0, SEEK_SET) != 0 || fread(hdr, 1, sizeof(hdr), disc_iso) != sizeof(hdr)) {
            *err = "read disc header failed"; return false;
        }
        memcpy(gc_ram + 0, hdr, sizeof(hdr));
    } else {
        memset(gc_ram + 0, 0, 0x20);
    }

    hal_mem_write32(0x00000020, 0x0D15EA5Eu); /* boot magic */
    hal_mem_write32(0x00000028, GC_MEM1_SIZE);/* 0x01800000 */
    hal_mem_write32(0x0000002C, 0x10000006u); /* LatestDevkit */
    hal_mem_write32(0x000000CC, 0x00000000u); /* NTSC */
    hal_mem_write32(0x000000D0, GC_ARAM_SIZE);/* 0x01000000 */
    hal_mem_write32(0x000000F8, 0x09A7EC80u); /* bus clock */
    hal_mem_write32(0x000000FC, 0x1CF7C580u); /* cpu clock */

    /* bi2：盘上 0x440 起 0x2000 字节 → 0x81700000，指针 → 0x800000F4。
     * 实测本盘 bi2 全零（+0xC=0 → 不走调试器路径）。 */
    if (disc_iso) {
        uint8_t *bi2 = gc_ram + hal_ea_to_phys(0x81700000u);
        if (fseek(disc_iso, 0x440, SEEK_SET) != 0 ||
            fread(bi2, 1, 0x2000, disc_iso) != 0x2000) {
            *err = "read bi2 failed"; return false;
        }
    }
    hal_mem_write32(0x000000F4, 0x81700000u);

    /* Direct DOL boot skips the apploader, which normally installs the FST.
     * SDK __DVDFSInit (802b6308) reads its pointer from 80000038; leaving it
     * zero makes all resource lookups fail before any DVD Read command.
     * Reserve the disc's maximum FST size below BI2 and exclude both from
     * the OS arena. GameCube disc offsets and FST sizes are byte counts. */
    hal_mem_write32(0x34, 0);
    hal_mem_write32(0x38, 0);
    hal_mem_write32(0x3c, 0);
    if (disc_iso) {
        uint8_t header[12];
        if (fseek(disc_iso, 0x424, SEEK_SET) != 0 ||
            fread(header, 1, sizeof(header), disc_iso) != sizeof(header)) {
            *err = "read FST header failed"; return false;
        }
        uint32_t offset = be32(header), size = be32(header + 4);
        uint32_t capacity = be32(header + 8);
        const uint32_t bi2_phys = 0x01700000u;
        if (size < 12 || capacity < size || capacity > bi2_phys - 0x3100u) {
            *err = "invalid FST size"; return false;
        }
        uint32_t reserved = (capacity + 31u) & ~31u;
        uint32_t base = bi2_phys - reserved;
        if (base < dol_memory_end) {
            *err = "FST overlaps DOL memory"; return false;
        }
        if (fseek(disc_iso, (long)offset, SEEK_SET) != 0 ||
            fread(gc_ram + base, 1, size, disc_iso) != size) {
            *err = "read FST failed"; return false;
        }
        uint32_t entries = hal_mem_read32(base + 8);
        if (gc_ram[base] != 1 || !entries || entries > size / 12u) {
            *err = "invalid FST root"; return false;
        }
        memset(gc_ram + base + size, 0, reserved - size);
        hal_mem_write32(0x34, 0x80000000u + base); /* OS ArenaHi */
        hal_mem_write32(0x38, 0x80000000u + base); /* FST location */
        hal_mem_write32(0x3c, capacity);          /* FST maximum size */
    }

    hal_mem_write32(0x00000300, 0x4C000064u); /* rfi */
    hal_mem_write32(0x00000800, 0x4C000064u); /* rfi */
    hal_mem_write32(0x00000C00, 0x4C000064u); /* rfi */
    hal_mem_write32(0x000030D8, 0);           /* timebase lo */
    hal_mem_write32(0x000030DC, 0);           /* timebase hi */
    /* 0x80000044 / 0x800030E6：入口读写点，保持 0（RAM 已清零） */

    *err = NULL;
    return true;
}

/* dcbz：按 32 字节缓存行清零（提升器调用；MMIO/空洞忽略） */
void hal_dcbz(uint32_t phys)
{
    extern GcCtx gc;   /* dcbz 现场取证 */
    uint32_t line = phys & ~0x1Fu;
    if (line < 0x10000u) {   /* 低端 64KB 的 dcbz 极可疑：异常表/boot 区 */
        static unsigned lo_n;
        if (lo_n++ < 12)
            fprintf(stderr, "[dcbz-lo] #%u phys=%08x pc=%08x lr=%08x r1=%08x\n",
                    lo_n, phys, gc.pc, gc.lr, gc.r[1]);
    }
    if (line + 32u <= GC_MEM1_SIZE) memset(gc_ram + line, 0, 32);
}
