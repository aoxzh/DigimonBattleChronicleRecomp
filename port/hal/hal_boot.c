/* hal/hal_boot.c — 引导准备与帧驱动。 */
#include "hal_boot.h"
#include "hal.h"
#include "gc_abi.h"
#include "mem.h"
#include "irq.h"
#include "vi.h"
#include "gx.h"
#include "si.h"
#include "exi.h"
#include "dsp.h"
#include "ar.h"
#include "ai.h"
#include "di.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t entry_pc;

/* DOL 整文件读入（3.4MB，一次性） */
static uint8_t *read_all(const char *path, size_t *out_n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *out_n = (size_t)n;
    return buf;
}

static uint32_t be32_at(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* 盘头 0x420 = DOL 偏移；DOL 长度取 18 个段 (offset+size) 的最大值。 */
uint8_t *hal_boot_read_disc_dol(const char *iso_path, size_t *out_n)
{
    FILE *f = iso_path ? fopen(iso_path, "rb") : NULL;
    if (!f) return NULL;
    uint8_t disc[0x440], head[0x100];
    uint8_t *buf = NULL;
    if (fread(disc, 1, sizeof(disc), f) != sizeof(disc) || be32_at(disc + 0x1C) != 0xC2339F3Du)
        goto out;
    uint32_t off = be32_at(disc + 0x420);
    if (fseek(f, (long)off, SEEK_SET) != 0 || fread(head, 1, sizeof(head), f) != sizeof(head))
        goto out;
    size_t n = sizeof(head);
    for (unsigned i = 0; i < 18; i++) {
        size_t end = (size_t)be32_at(head + 4 * i) + be32_at(head + 0x90 + 4 * i);
        if (be32_at(head + 0x90 + 4 * i) && end > n) n = end;
    }
    if (n > 0x01800000u) goto out;
    buf = (uint8_t *)malloc(n);
    if (!buf || fseek(f, (long)off, SEEK_SET) != 0 || fread(buf, 1, n, f) != n) {
        free(buf); buf = NULL; goto out;
    }
    *out_n = n;
out:
    fclose(f);
    return buf;
}

bool hal_boot_prepare(const char *dol_path, const char *iso_path)
{
    const char *err = NULL;
    entry_pc = 0;

    /* 1) 全 RAM 清零（含 BS2 不会写到的区域） */
    memset(gc_ram, 0, sizeof(gc_ram));

    /* 2) 硬件块复位 */
    hal_irq_init();
    hal_vi_init();
    hal_gx_init();
    hal_si_init();
    hal_exi_init();
    hal_dsp_init();
    hal_ar_init();
    hal_ai_init();
    hal_di_init();

    /* 3) DOL 加载（先清 bss 再拷段——见 mem.c 注释） */
    if (!dol_path && !iso_path) {
        fprintf(stderr, "hal_boot: no dol path\n");
        return false;
    }
    size_t dol_n = 0;
    uint8_t *dol = dol_path ? read_all(dol_path, &dol_n) : hal_boot_read_disc_dol(iso_path, &dol_n);
    if (!dol) {
        fprintf(stderr, "hal_boot: cannot read %s\n", dol_path ? dol_path : "main.dol from disc");
        return false;
    }
    HalDolHeader h;
    if (!hal_dol_parse(dol, dol_n, &h, &err) || !hal_dol_load(&h, dol, dol_n, &err)) {
        fprintf(stderr, "hal_boot: dol load failed: %s\n", err ? err : "?");
        free(dol);
        return false;
    }
    free(dol);
    entry_pc = h.entry;

    /* 4) disc.iso（bi2/头部 + DI 直通源） */
    FILE *iso = NULL;
    if (iso_path) {
        iso = fopen(iso_path, "rb");
        if (!iso) fprintf(stderr, "hal_boot: warn: cannot open iso %s\n", iso_path);
    }
    if (!hal_write_boot_state(iso, &err)) {
        fprintf(stderr, "hal_boot: boot state failed: %s\n", err ? err : "?");
        if (iso) fclose(iso);
        return false;
    }
    if (iso) {
        fclose(iso);
        if (!hal_di_open_iso(iso_path))
            fprintf(stderr, "hal_boot: warn: di iso open failed\n");
    }

    /* 5) GcCtx 初始寄存器（Boot_BS2Emu.cpp EmulatedBS2_GC，NTSC） */
    memset(&gc, 0, sizeof(gc));
    gc.r[1]  = 0x81566550u;   /* 栈指针 */
    gc.r[2]  = 0x81465CC0u;   /* SDA2 base */
    gc.r[13] = 0x81465320u;   /* SDA base */
    gc.msr   = 0x0000F000u;   /* EE 开 */
    gc.pc    = entry_pc;
    return true;
}

uint32_t hal_boot_entry(void) { return entry_pc; }

uint32_t hal_run_frame(uint32_t max_blocks)
{
    /* 无 CPU 执行的硬件侧推进：把 max_blocks 个 poll 量子走完。
     * 生成代码在场时，时间由生成代码里的 hal_irq_poll() 调用驱动，
     * host 只需 hal_frame_tick()。 */
    uint64_t f0 = hal_frame_count();
    for (uint32_t i = 0; i < max_blocks; i++)
        (void)hal_irq_poll();
    return (uint32_t)(hal_frame_count() - f0);
}
