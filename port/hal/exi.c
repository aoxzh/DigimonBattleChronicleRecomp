/* hal/exi.c — EXI 通道 + IPL（RTC/SRAM）+ 槽 A 内存卡。协议出处见 exi.h。
 * 传输按字节流交给 CS 选中的设备，即时完成（TCINT）；卡的擦除/编程在
 * CARD_DONE_CYCLES 后完成，避免 SDK 在持有 EXI 锁时收到完成中断。 */
#include "exi.h"
#include "hal.h"
#include "irq.h"
#include "mem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define EXI_NCHAN 3          /* GC：槽 A/B + 串口 2（ch2 = SP1/AD16） */
#define GC_EPOCH  0x386D4380u  /* 2000-01-01 */
#define TB_HZ     40500000ull  /* OSGetTime 时基 */

typedef struct {
    uint32_t status, dma_addr, dma_len, dma_ctrl, imm;
} ExiChan;

static ExiChan exi[EXI_NCHAN];

/* ---------------- IPL：RTC + SRAM（ch0 dev1） ---------------- */
static uint8_t sram[0x40];
static struct { uint32_t cmd; unsigned pos; uint32_t rtc; } ipl;

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)(v >> 16)); put16(p + 2, (uint16_t)v); }
static uint64_t be64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

/* GC RTC 计本地时间（Dolphin 同口径），SRAM counterBias = 0。 */
static uint32_t rtc_now(void)
{
    time_t t = time(NULL);
    struct tm g = *gmtime(&t);
    g.tm_isdst = -1;
    double offset = difftime(t, mktime(&g));
    return (uint32_t)((int64_t)t + (int64_t)offset - GC_EPOCH);
}

/* OSSram(0x14) + OSSramEx(0x2C)。flags 0x2C = Dolphin 缺省（立体声）。
 * flashID 须与卡头 serial 对应：CARDMount 校验 flashIDCheckSum == ~Σ flashID，
 * VerifyID 用 flashID 与 formatTime 复算 serial。 */
static void sram_build(const uint8_t flash_a[12])
{
    memset(sram, 0, sizeof(sram));
    sram[0x13] = 0x2C;
    uint16_t sum = 0, inv = 0;
    for (unsigned i = 0x0C; i < 0x14; i += 2) {
        uint16_t w = be16(sram + i);
        sum = (uint16_t)(sum + w); inv = (uint16_t)(inv + (uint16_t)~w);
    }
    put16(sram + 0, sum); put16(sram + 2, inv);
    uint8_t fsum = 0;
    for (unsigned i = 0; i < 12; i++) { sram[0x14 + i] = flash_a[i]; fsum = (uint8_t)(fsum + flash_a[i]); }
    sram[0x3A] = (uint8_t)~fsum;
    sram[0x3B] = 0xFF;      /* 槽 B flashID 全 0 */
}

/* 返回 -1 = 不驱动数据线（ROM/UART 等未实现区）。 */
static int ipl_byte(uint8_t in)
{
    if (ipl.pos < 4) {
        ipl.cmd = (ipl.cmd << 8) | in;
        ipl.pos++;
        return -1;
    }
    const uint32_t a = (ipl.cmd & 0x7FFFFFFFu) >> 6;
    const int write = (ipl.cmd >> 31) != 0;
    const unsigned off = ipl.pos++ - 4;
    if (a >= 0x800000u && a < 0x800004u) {
        unsigned i = (a - 0x800000u) + off;
        return write || i >= 4 ? -1 : (int)((ipl.rtc >> (24 - 8 * i)) & 0xFF);
    }
    if (a >= 0x800004u && a < 0x800044u) {
        unsigned i = (a - 0x800004u) + off;
        if (i >= sizeof(sram)) return -1;
        if (write) { sram[i] = in; return -1; }
        return sram[i];
    }
    return -1;
}

/* ---------------- 槽 A 内存卡（ch0 dev0） ---------------- */
#define MC_BUSY      0x80u
#define MC_UNLOCKED  0x40u
#define MC_ERASEERR  0x10u
#define MC_PROGERR   0x08u
#define MC_READY     0x01u
#define CARD_DONE_CYCLES  50000u          /* ~0.1 ms 客体时间 */
#define CARD_FLUSH_CYCLES 486000000ull    /* 修改后空闲 1 s 写回 */
#define CARD_SECTOR 0x2000u
/* OSGetFontEncode()：vi.c 的 DTV 状态 bit1（NTSC-J）置位 → SJIS(1)。卡头 encode
 * 必须与之相同，否则 CARDMount 报 CARD_RESULT_ENCODING。 */
#define CARD_ENCODE 1u

static char card_path[1024];
static struct {
    uint8_t *data;
    uint32_t size;          /* 字节；EXI ID = size / 128 KiB（Mbit） */
    char path[1024];
    int dirty;
    uint64_t idle;
    unsigned pos;
    uint8_t cmd, status;
    uint32_t addr;
    int irq_enabled;
    uint32_t busy;
    uint8_t prog[128];
    unsigned prog_n;
} card;

static void card_checksum(const uint8_t *p, unsigned len, uint8_t *out_sum, uint8_t *out_inv)
{
    uint16_t sum = 0, inv = 0;
    for (unsigned i = 0; i < len; i += 2) {
        uint16_t w = be16(p + i);
        sum = (uint16_t)(sum + w); inv = (uint16_t)(inv + (uint16_t)~w);
    }
    if (sum == 0xFFFF) sum = 0;
    if (inv == 0xFFFF) inv = 0;
    put16(out_sum, sum); put16(out_inv, inv);
}

static uint64_t serial_rand(uint64_t r) { return (r * 1103515245ull + 12345ull) >> 16; }

/* SDK __CARDFormatRegionAsync 的系统区：ID 块、双目录、双 FAT。flashID 全 0；
 * encode = CARD_ENCODE。 */
static void card_format(void)
{
    memset(card.data, 0xFF, card.size);
    uint8_t *id = card.data;
    uint64_t now = (uint64_t)rtc_now() * TB_HZ, r = now;
    for (unsigned i = 0; i < 12; i++) {
        r = serial_rand(r);
        id[i] = (uint8_t)r;
        r = serial_rand(r) & 0x7FFF;
    }
    for (unsigned i = 0; i < 8; i++) id[12 + i] = (uint8_t)(now >> (56 - 8 * i));
    put32(id + 20, 0);          /* counterBias */
    put32(id + 24, 0);          /* language */
    put32(id + 28, 3);          /* VI DTV status（vi.c 0x6E） */
    put16(id + 0x20, 0);        /* deviceID */
    put16(id + 0x22, (uint16_t)(card.size >> 17));
    put16(id + 0x24, CARD_ENCODE);
    card_checksum(id, 0x1FC, id + 0x1FC, id + 0x1FE);
    for (unsigned i = 0; i < 2; i++) {
        uint8_t *dir = card.data + (1 + i) * CARD_SECTOR;
        put16(dir + 0x1FFA, (uint16_t)i);
        card_checksum(dir, 0x1FFC, dir + 0x1FFC, dir + 0x1FFE);
    }
    const uint16_t blocks = (uint16_t)(card.size / CARD_SECTOR);
    for (unsigned i = 0; i < 2; i++) {
        uint8_t *fat = card.data + (3 + i) * CARD_SECTOR;
        memset(fat, 0, CARD_SECTOR);
        put16(fat + 4, (uint16_t)i);              /* checkCode */
        put16(fat + 6, (uint16_t)(blocks - 5));   /* freeBlocks */
        put16(fat + 8, 4);                        /* lastSlot */
        card_checksum(fat + 4, CARD_SECTOR - 4, fat + 0, fat + 2);
    }
}

/* 导入的卡（如 Dolphin 格式化）：由 serial 与 formatTime 反推 flashID。 */
static void card_flash_id(uint8_t out[12])
{
    memset(out, 0, 12);
    if (!card.data) return;
    const uint8_t *id = card.data;
    uint8_t sum[2], inv[2];
    card_checksum(id, 0x1FC, sum, inv);
    if (memcmp(sum, id + 0x1FC, 2) || memcmp(inv, id + 0x1FE, 2) || be16(id + 0x22) != (card.size >> 17))
        return;     /* 未格式化：游戏会提示格式化，SRAM flashID 保持 0 */
    uint64_t r = be64(id + 12);
    for (unsigned i = 0; i < 12; i++) {
        r = serial_rand(r);
        out[i] = (uint8_t)(id[i] - (uint8_t)r);
        r = serial_rand(r) & 0x7FFF;
    }
}

/* 有效卡头但 encode 与本机不同（旧版 ANSI 卡、海外设置的 Dolphin 卡）：
 * 只改 encode 并重算卡头校验，存档内容不动。 */
static void card_fix_encode(void)
{
    uint8_t *id = card.data, sum[2], inv[2];
    card_checksum(id, 0x1FC, sum, inv);
    if (memcmp(sum, id + 0x1FC, 2) || memcmp(inv, id + 0x1FE, 2) || be16(id + 0x24) == CARD_ENCODE)
        return;
    fprintf(stderr, "[exi] memcard A: encode %u -> %u (console is NTSC-J)\n", be16(id + 0x24), CARD_ENCODE);
    put16(id + 0x24, CARD_ENCODE);
    card_checksum(id, 0x1FC, id + 0x1FC, id + 0x1FE);
    card.dirty = 1;
}

static void card_load(void)
{
    if (card.data && !strcmp(card.path, card_path)) return;
    free(card.data);
    memset(&card, 0, sizeof(card));
    if (!card_path[0]) return;
    snprintf(card.path, sizeof(card.path), "%s", card_path);
    FILE *f = fopen(card.path, "rb");
    long n = -1;
    if (f && fseek(f, 0, SEEK_END) == 0) n = ftell(f);
    if (f && n >= 0) {
        const int valid = n == 0x80000 || n == 0x100000 || n == 0x200000 ||
                          n == 0x400000 || n == 0x800000 || n == 0x1000000;
        if (!valid) {
            fprintf(stderr, "[exi] memcard A: %s has unsupported size %ld; slot left empty\n", card.path, n);
            fclose(f);
            card.path[0] = 0;
            return;
        }
        card.size = (uint32_t)n;
        card.data = (uint8_t *)malloc(card.size);
        if (!card.data || fseek(f, 0, SEEK_SET) != 0 || fread(card.data, 1, card.size, f) != card.size) {
            fprintf(stderr, "[exi] memcard A: cannot read %s; slot left empty\n", card.path);
            free(card.data); card.data = NULL; card.path[0] = 0;
            fclose(f);
            return;
        }
        fclose(f);
        fprintf(stderr, "[exi] memcard A: %s (%u blocks)\n", card.path, card.size / CARD_SECTOR - 5);
        card_fix_encode();
    } else {
        if (f) fclose(f);
        card.size = 0x200000;
        card.data = (uint8_t *)malloc(card.size);
        if (!card.data) { card.path[0] = 0; return; }
        card_format();
        card.dirty = 1;
        fprintf(stderr, "[exi] memcard A: new formatted 251-block card %s\n", card.path);
    }
}

static void card_select(int on)
{
    if (on) { card.pos = 0; card.prog_n = 0; return; }
    if (card.pos == 0) return;
    int busy = 0;
    switch (card.cmd) {
    case 0xF1:      /* sector erase */
        if (card.pos >= 3) {
            memset(card.data + ((card.addr & (card.size - 1)) & ~(CARD_SECTOR - 1)), 0xFF, CARD_SECTOR);
            busy = 1;
        }
        break;
    case 0xF4:      /* chip erase */
        if (card.pos >= 3) { memset(card.data, 0xFF, card.size); busy = 1; }
        break;
    case 0xF2:      /* page program: 最多 128 字节，页内回绕 */
        if (card.pos >= 5) {
            uint32_t a = card.addr;
            for (unsigned i = 0; i < card.prog_n; i++) {
                card.data[a & (card.size - 1)] = card.prog[i];
                a = (a & ~0x1FFu) | ((a + 1) & 0x1FFu);
            }
            busy = 1;
        }
        break;
    default: break;
    }
    if (busy) {
        card.status = (uint8_t)((card.status | MC_BUSY) & ~MC_READY);
        card.busy = CARD_DONE_CYCLES;
        card.dirty = 1;
        card.idle = 0;
    }
}

static void exi_update_interrupts(void);

static void card_done(void)
{
    card.status = (uint8_t)((card.status & ~MC_BUSY) | MC_READY);
    if (card.irq_enabled) {
        exi[0].status |= 1u << 1;   /* EXIINT */
        exi_update_interrupts();
    }
}

static void card_address_byte(uint8_t in)
{
    switch (card.pos) {
    case 1: card.addr = (uint32_t)(in & 0x7F) << 17; break;
    case 2: card.addr |= (uint32_t)in << 9; break;
    case 3: card.addr |= (uint32_t)(in & 3) << 7; break;
    case 4: card.addr |= in & 0x7Fu; break;
    default: break;
    }
}

static int card_byte(uint8_t in)
{
    int out = 0xFF;
    if (card.pos == 0) {
        card.cmd = in;
        if (in == 0x89) card.status = (uint8_t)((card.status & ~(MC_ERASEERR | MC_PROGERR)) | MC_READY);
        card.pos++;
        return out;
    }
    switch (card.cmd) {
    case 0x00: {    /* EXI ID：容量 Mbit，位于命令后第 2..5 字节 */
        const uint32_t id = card.size >> 17;
        if (card.pos >= 2) out = (int)((id >> (24 - 8 * ((card.pos - 2) & 3))) & 0xFF);
        else out = 0;
        break;
    }
    case 0x83: out = card.status; break;
    case 0x85: out = card.pos == 2 ? 0xC2 : card.pos == 3 ? 0x21 : 0xFF; break;
    case 0x81: if (card.pos == 1) card.irq_enabled = in & 1; break;
    case 0x52:      /* 5 字节命令 + 4 字节等待（latency）后开始出数据 */
        if (card.pos <= 4) card_address_byte(in);
        else if (card.pos >= 9) {
            out = card.data[card.addr & (card.size - 1)];
            card.addr = (card.addr & ~0x1FFu) | ((card.addr + 1) & 0x1FFu);
        }
        break;
    case 0xF1: if (card.pos <= 2) card_address_byte(in); break;
    case 0xF2:
        if (card.pos <= 4) card_address_byte(in);
        else {
            card.prog[(card.pos - 5) & 127] = in;
            if (card.prog_n < 128) card.prog_n++;
        }
        break;
    default: break;
    }
    card.pos++;
    return out;
}

/* ---------------- 通道 ---------------- */
static int selected_device(uint32_t status)
{
    const uint32_t cs = (status >> 7) & 7u;
    return cs & 1 ? 0 : cs & 2 ? 1 : cs & 4 ? 2 : -1;
}

static void device_select(unsigned c, int dev, int on)
{
    if (c == 0 && dev == 0 && card.data) card_select(on);
    else if (c == 0 && dev == 1 && on) { ipl.cmd = 0; ipl.pos = 0; ipl.rtc = rtc_now(); }
}

static int device_byte(unsigned c, int dev, uint8_t in)
{
    if (c == 0 && dev == 0 && card.data) return card_byte(in);
    if (c == 0 && dev == 1) return ipl_byte(in);
    return -1;
}

static void exi_update_interrupts(void)
{
    /* EXI_Channel.cpp UpdateInterrupts：EXIINT&EXIINTMASK || TCINT&TCINTMASK ||
     * EXTINT&EXTINTMASK → PI EXI 线（3 通道任一） */
    bool set = false;
    for (unsigned i = 0; i < EXI_NCHAN; i++) {
        uint32_t s = exi[i].status;
        if (((s & (1u << 1)) && (s & (1u << 0))) ||
            ((s & (1u << 3)) && (s & (1u << 2))) ||
            ((s & (1u << 11)) && (s & (1u << 10))))
            set = true;
    }
    if (set) hal_pi_set_interrupt(PI_INT_EXI);
    else     hal_pi_clear_interrupt(PI_INT_EXI);
}

static void exi_transfer(unsigned c)
{
    ExiChan *e = &exi[c];
    const int dev = selected_device(e->status);
    const unsigned rw = (e->dma_ctrl >> 2) & 3u;
    if (e->dma_ctrl & 2u) {
        /* DMA：读 = 设备→RAM（仅当设备驱动了全部字节才写内存），写 = RAM→设备 */
        uint32_t phys = hal_ea_to_phys(e->dma_addr), len = e->dma_len;
        if (dev >= 0 && phys < GC_MEM1_SIZE && len <= GC_MEM1_SIZE - phys) {
            uint8_t *buf = (uint8_t *)malloc(len ? len : 1);
            if (buf) {
                if (rw == 1) {
                    hal_mem_read_burst(phys, buf, len);
                    for (uint32_t i = 0; i < len; i++) (void)device_byte(c, dev, buf[i]);
                } else {
                    int driven = 1;
                    for (uint32_t i = 0; i < len; i++) {
                        int b = device_byte(c, dev, 0);
                        if (b < 0) driven = 0;
                        buf[i] = (uint8_t)b;
                    }
                    if (driven && len) hal_mem_write_burst(phys, buf, len);
                }
                free(buf);
            }
        }
    } else {
        /* IMM：TLEN+1 字节，MSB 先；未驱动字节读为 0xFF */
        const unsigned n = ((e->dma_ctrl >> 4) & 3u) + 1u;
        uint32_t out = 0;
        for (unsigned i = 0; i < n; i++) {
            const uint8_t in = (uint8_t)(e->imm >> (24 - 8 * i));
            int b = dev >= 0 ? device_byte(c, dev, rw == 0 ? 0 : in) : -1;
            out |= (uint32_t)(b < 0 ? 0xFF : b) << (24 - 8 * i);
        }
        if (rw != 1) e->imm = out;
    }
    e->dma_ctrl &= ~1u;
    e->status |= 1u << 3;      /* TCINT */
    exi_update_interrupts();
}

void hal_exi_set_memcard(const char *path)
{
    hal_clock_sync_devices();
    hal_clock_device_changed();
    snprintf(card_path, sizeof(card_path), "%s", path ? path : "");
}

void hal_exi_init(void)
{
    hal_clock_sync_devices();
    hal_clock_device_changed();
    memset(exi, 0, sizeof(exi));
    memset(&ipl, 0, sizeof(ipl));
    card_load();
    card.pos = 0; card.busy = 0; card.irq_enabled = 0;
    card.status = MC_UNLOCKED | MC_READY;
    uint8_t flash[12];
    card_flash_id(flash);
    sram_build(flash);
}

uint32_t hal_exi_cycles_to_event(void)
{
    hal_clock_sync_devices();
    if (!card.data) return UINT32_MAX;
    if (card.busy) return card.busy;
    if (!card.dirty || !card.path[0]) return UINT32_MAX;
    return card.idle >= CARD_FLUSH_CYCLES ? 1u :
           (uint32_t)(CARD_FLUSH_CYCLES - card.idle);
}

void hal_exi_tick_cycles(uint32_t cycles)
{
    hal_clock_sync_devices();
    hal_clock_device_changed();
    if (!card.data) return;
    if (card.busy) {
        if (card.busy > cycles) { card.busy -= cycles; return; }
        cycles -= card.busy; /* only time AFTER completion counts as idle */
        card.busy = 0;
        card_done();
    }
    if (card.dirty && !card.busy) {
        card.idle += cycles;
        if (card.idle >= CARD_FLUSH_CYCLES) (void)hal_exi_flush();
    }
}

int hal_exi_flush(void)
{
    hal_clock_sync_devices();
    hal_clock_device_changed();
    if (!card.data || !card.dirty || !card.path[0]) return 0;
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp", card.path);
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite(card.data, 1, card.size, f) == card.size;
    if (f && fclose(f) != 0) ok = 0;
#ifdef _WIN32
    if (ok) ok = MoveFileExA(tmp, card.path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    if (ok) ok = rename(tmp, card.path) == 0;
#endif
    if (!ok) {
        remove(tmp);
        fprintf(stderr, "[exi] memcard A: cannot write %s\n", card.path);
        card.idle = 0;      /* 1 秒后重试 */
        return -1;
    }
    card.dirty = 0;
    card.idle = 0;
    fprintf(stderr, "[exi] memcard A saved %s\n", card.path);
    return 0;
}

uint32_t hal_exi_read32(uint32_t reg)
{
    hal_clock_sync_devices();
    unsigned c = reg / 0x14;
    uint32_t off = reg % 0x14;
    if (c >= EXI_NCHAN) return 0;
    switch (off) {
    case 0x00: return exi[c].status | (c == 0 && card.data ? 1u << 12 : 0u);  /* EXT：卡在槽 */
    case 0x04: return exi[c].dma_addr;
    case 0x08: return exi[c].dma_len;
    case 0x0C: return exi[c].dma_ctrl;         /* 传输即时完成，TSTART 恒为 0 */
    case 0x10: return exi[c].imm;
    default:   return 0;
    }
}

void hal_exi_write32(uint32_t reg, uint32_t v)
{
    hal_clock_sync_devices();
    hal_clock_device_changed();
    unsigned c = reg / 0x14;
    uint32_t off = reg % 0x14;
    if (c >= EXI_NCHAN) return;
    ExiChan *e = &exi[c];
    switch (off) {
    case 0x00: {
        /* EXIINT/TCINT/EXTINT 写 1 清；三个 MASK、CLK、CS 直写；ROMDIS 置位后保持 */
        const int old_dev = selected_device(e->status);
        uint32_t s = e->status & ~(v & 0x80Au);
        s = (s & ~0x7F5u) | (v & 0x7F5u);
        e->status = s | (v & 0x2000u);
        const int new_dev = selected_device(e->status);
        if (old_dev != new_dev) {
            if (old_dev >= 0) device_select(c, old_dev, 0);
            if (new_dev >= 0) device_select(c, new_dev, 1);
        }
        exi_update_interrupts();
        break;
    }
    case 0x04: e->dma_addr = v & 0x03FFFFE0u; break;
    case 0x08: e->dma_len = v & 0x03FFFFE0u; break;
    case 0x0C:
        e->dma_ctrl = v & 0x3Fu;
        if (v & 1u) exi_transfer(c);           /* TSTART */
        break;
    case 0x10: e->imm = v; break;
    default: break;
    }
}
