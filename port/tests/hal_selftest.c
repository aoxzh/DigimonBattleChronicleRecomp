/* tests/hal_selftest.c — HAL 自检（必须全绿）。
 * 覆盖：BE 内存读写、DOL 装载（0x80003100 与文件一致 + bss/段顺序）、
 * IRQ 注入流程（EE/掩码/srr0/向量）、DI 读盘 0x420 得 0x1EC00、
 * WGPIPE 吸收不乱步（32 字节对齐 + 命令同步计数 + 回卷回 base）、
 * WGPIPE 宽度写等价（byte/16/32/混合）+ PE finish/token 电平与 ACK、
 * DSP reset spin 退出、ARAM DMA、SI 轮询回填、EXI 无卡 ID、VI retrace。
 */
#include "../hal/hal_boot.h"
#include "../hal/hal.h"
#include "../hal/mem.h"
#include "../hal/mmio.h"
#include "../hal/irq.h"
#include "../hal/di.h"
#include "../hal/gx.h"
#include "../hal/si.h"
#include "../hal/exi.h"
#include "../hal/dsp.h"
#include "../hal/ar.h"
#include "../hal/vi.h"
#include "../hal/gc_abi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

#define CHECK(cond, name, fmt, ...)                                          \
    do {                                                                     \
        if (cond) { printf("PASS %s\n", (name)); }                           \
        else { printf("FAIL %s  " fmt "\n", (name), __VA_ARGS__); fails++; } \
    } while (0)

#define CHECKI(cond, name, v) CHECK(cond, name, "(value=%08x)", (unsigned)(v))

static const char *DOL = "extracted/main.dol";
static const char *ISO = "disc.iso";

/* The DOL/disc checks need the user's own game files (port/extracted/main.dol and
 * port/disc.iso), which a fresh clone and CI do not have: they are skipped, loudly. */
static int have_game_files(void)
{
    FILE *d = fopen(DOL, "rb"), *i = fopen(ISO, "rb");
    int ok = d && i;
    if (d) fclose(d);
    if (i) fclose(i);
    return ok;
}

static void t_runtime_manifest(void)
{
    const char *path = "build/hal_selftest_manifest.json";
    FILE *f;
    char buf[2048];
    size_t n;

    remove(path);
    gc_runtime_manifest_reset();
    if (!gc_runtime_manifest_enabled()) {
        gc_runtime_manifest_note_block(0x80003100u);
        CHECKI(gc_runtime_manifest_dump(path) == 0, "manifest default off", 0);
        return;
    }

    gc_runtime_manifest_note_block(0x80003100u);
    gc_runtime_manifest_note_block(0x80003100u);
    gc_runtime_manifest_note_event("frame");
    gc_runtime_manifest_note_mmio(0x0C002030u, 16u, 1);
    CHECKI(gc_runtime_manifest_dump(path) == 1, "manifest dump", 0);
    f = fopen(path, "rb");
    CHECKI(!!f, "manifest open", f ? 1 : 0);
    if (!f) return;
    n = fread(buf, 1, sizeof(buf) - 1u, f);
    fclose(f);
    buf[n] = '\0';
    CHECKI(strstr(buf, "\"blocks\"") && strstr(buf, "\"events\"")
           && strstr(buf, "\"mmio\"") && strstr(buf, "\"count\":2"),
           "manifest json fields", (unsigned)n);
    remove(path);
}

static void t_be_mem(void)
{
    memset(gc_ram, 0, 64);
    hal_mem_write32(0x1000, 0x12345678u);
    CHECKI(hal_mem_read32(0x1000) == 0x12345678u, "be32 rw", hal_mem_read32(0x1000));
    CHECKI(gc_ram[0x1000] == 0x12 && gc_ram[0x1003] == 0x78, "be32 layout",
           (gc_ram[0x1000] << 24) | gc_ram[0x1003]);
    hal_mem_write16(0x1004, 0xABCDu);
    CHECKI(hal_mem_read16(0x1004) == 0xABCDu, "be16 rw", hal_mem_read16(0x1004));
    hal_mem_write64(0x1010, 0x0102030405060708ull);
    CHECKI(hal_mem_read64(0x1010) == 0x0102030405060708ull, "be64 rw",
           (uint32_t)hal_mem_read64(0x1010));
    CHECKI(hal_mem_read8(0x1010) == 0x01, "be8", hal_mem_read8(0x1010));

    /* 慢路径空洞：读 0 不炸 */
    CHECKI(hal_slow_read32(0x0A000000) == 0, "hole read", hal_slow_read32(0x0A000000));
    hal_slow_write32(0x0A000000, 0xDEAD);
    CHECKI(hal_slow_read32(0x0A000000) == 0, "hole write ignored",
           hal_slow_read32(0x0A000000));
}

static void t_dol_load(void)
{
    if (!hal_boot_prepare(DOL, ISO)) {
        CHECKI(0, "boot prepare", 0);
        return;
    }
    CHECKI(hal_boot_entry() == 0x80003100u, "dol entry", hal_boot_entry());

    /* 0x80003100 与文件逐字节一致 */
    FILE *f = fopen(DOL, "rb");
    CHECKI(!!f, "dol open", f ? 1 : 0);
    if (!f) return;
    fseek(f, 0x100, SEEK_SET);   /* text[0].file = 256，addr = 0x80003100 */
    uint8_t ref[0x3C0];
    if (fread(ref, 1, sizeof(ref), f) != sizeof(ref)) { fclose(f); CHECKI(0, "dol read", 0); return; }
    fclose(f);
    CHECKI(memcmp(gc_ram + 0x3100, ref, sizeof(ref)) == 0, "text[0] bytes match",
             *(uint32_t *)(gc_ram + 0x3100));

    /* 引导内存状态 */
    CHECKI(hal_mem_read32(0x20) == 0x0D15EA5Eu, "boot magic", hal_mem_read32(0x20));
    CHECKI(hal_mem_read32(0x2C) == 0x10000006u, "devkit type", hal_mem_read32(0x2C));
    CHECKI(hal_mem_read32(0x28) == 0x01800000u, "ram size", hal_mem_read32(0x28));
    CHECKI(hal_mem_read32(0xF8) == 0x09A7EC80u, "bus clock", hal_mem_read32(0xF8));
    CHECKI(hal_mem_read32(0x300) == 0x4C000064u, "rfi vector", hal_mem_read32(0x300));
    CHECKI(hal_mem_read32(0xF4) == 0x81700000u, "bi2 ptr", hal_mem_read32(0xF4));
    CHECKI(hal_mem_read32(0x0170000Cu) == 0u, "bi2 retail path(+0xC=0)",
           hal_mem_read32(0x0170000Cu));
    CHECKI(gc.r[1] == 0x81566550u, "initial r1", gc.r[1]);
    CHECKI(gc.msr == 0x0000F000u, "initial msr", gc.msr);
    CHECKI(gc.pc == 0x80003100u, "initial pc", gc.pc);
    CHECKI(memcmp(gc_ram, "G2DJB2", 6) == 0, "disc header copied", gc_ram[0]);

    FILE *disc = fopen(ISO, "rb");
    CHECKI(!!disc, "fst disc open", disc ? 1 : 0);
    if (!disc) return;
    uint8_t header[12];
    if (fseek(disc, 0x424, SEEK_SET) != 0 ||
        fread(header, 1, sizeof(header), disc) != sizeof(header)) {
        fclose(disc);
        CHECKI(0, "fst disc header", 0);
        return;
    }
    uint32_t fields[3];
    for (unsigned i = 0; i < 3; i++) {
        const uint8_t *p = header + i * 4;
        fields[i] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
                    (uint32_t)p[2] << 8 | p[3];
    }
    uint32_t fst = hal_mem_read32(0x38);
    uint32_t phys = hal_ea_to_phys(fst);
    int valid = fst >= 0x80003100u && fst < 0x81800000u &&
                fields[1] >= 12 && fields[1] <= fields[2] &&
                (uint64_t)phys + fields[2] <= GC_MEM1_SIZE;
    CHECKI(valid, "fst valid memory range", fst);
    CHECKI((fst & 31u) == 0, "fst aligned", fst);
    CHECKI(hal_mem_read32(0x3C) == fields[2], "fst max size",
           hal_mem_read32(0x3C));
    CHECKI(hal_mem_read32(0x34) <= fst && hal_mem_read32(0x34) >= 0x80003100u,
           "arena excludes fst", hal_mem_read32(0x34));
    uint32_t bi2 = hal_mem_read32(0xF4);
    CHECKI((uint64_t)fst + fields[2] <= bi2 || (uint64_t)bi2 + 0x2000 <= fst,
           "fst excludes bi2", bi2);
    if (valid) {
        uint8_t *reference = malloc(fields[1]);
        CHECKI(!!reference, "fst reference allocated", fields[1]);
        if (reference) {
            int read_ok = fseek(disc, fields[0], SEEK_SET) == 0 &&
                          fread(reference, 1, fields[1], disc) == fields[1];
            CHECKI(read_ok, "fst disc bytes read", fields[1]);
            if (read_ok)
                CHECKI(memcmp(gc_ram + phys, reference, fields[1]) == 0,
                       "fst complete bytes match disc", fields[1]);
            free(reference);
        }
    }
    fclose(disc);
}

static void fixture_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static void t_fst_boot_validation(void)
{
    static const struct {
        const char *name;
        uint32_t size, capacity, count, root, available;
        int accepted;
    } cases[] = {
        { "fst minimal directory", 12, 32, 1, 0x01000000u, 12, 1 },
        { "fst rejects short header", 8, 32, 1, 0x01000000u, 12, 0 },
        { "fst rejects undersized capacity", 32, 12, 1, 0x01000000u, 32, 0 },
        { "fst rejects capacity overflow", 12, 0xFFFFFFFFu, 1, 0x01000000u, 12, 0 },
        { "fst rejects DOL overlap", 12, 0x01600000u, 1, 0x01000000u, 12, 0 },
        { "fst rejects root file", 12, 32, 1, 0, 12, 0 },
        { "fst rejects missing root", 12, 32, 0, 0x01000000u, 12, 0 },
        { "fst rejects entries beyond data", 12, 32, 2, 0x01000000u, 12, 0 },
        { "fst rejects entry count overflow", 12, 32, 0x40000001u, 0x01000000u, 12, 0 },
        { "fst rejects truncated data", 32, 32, 1, 0x01000000u, 12, 0 },
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        /* Overlap is judged against the DOL loaded by t_dol_load. */
        if (cases[i].capacity == 0x01600000u && !have_game_files()) {
            printf("SKIP %s (no game files)\n", cases[i].name);
            continue;
        }
        uint8_t image[0x2440 + 32] = {0};
        fixture_be32(image + 0x424, 0x2440);
        fixture_be32(image + 0x428, cases[i].size);
        fixture_be32(image + 0x42C, cases[i].capacity);
        fixture_be32(image + 0x2440, cases[i].root);
        fixture_be32(image + 0x2448, cases[i].count);
        FILE *disc = tmpfile();
        CHECKI(!!disc, "fst fixture open", i);
        if (!disc) continue;
        size_t size = 0x2440 + cases[i].available;
        if (fwrite(image, 1, size, disc) != size || fflush(disc) != 0) {
            CHECKI(0, "fst fixture write", i);
            fclose(disc);
            continue;
        }
        const char *error = NULL;
        int accepted = hal_write_boot_state(disc, &error);
        CHECKI(accepted == cases[i].accepted, cases[i].name, accepted);
        if (!accepted) CHECKI(error != NULL, "fst failure has reason", i);
        fclose(disc);
    }
    hal_mem_write32(0x34, 0x816E9660u);
    hal_mem_write32(0x38, 0x816E9660u);
    hal_mem_write32(0x3C, 0x16991u);
    const char *error = NULL;
    CHECKI(hal_write_boot_state(NULL, &error), "boot without disc", 0);
    CHECKI(hal_mem_read32(0x34) == 0 && hal_mem_read32(0x38) == 0 &&
           hal_mem_read32(0x3C) == 0, "no disc clears fst metadata", hal_mem_read32(0x38));
}

static void t_irq_inject(void)
{
    CHECKI(hal_boot_prepare(DOL, ISO), "irq boot prepare", 0);
    /* IRQ poll waits until OSInit has installed a real external handler. */
    hal_mem_write32(0x3010, 0x80003100u);
    /* PI 掩码开 DI 线，模拟 DI 完成 */
    hal_pi_write32(0x04, PI_INT_DI);
    hal_pi_set_interrupt(PI_INT_DI);
    CHECKI(hal_irq_pending() != 0, "irq pending", hal_irq_pending());
    gc.pc = 0x80005678u;
    uint32_t v = hal_irq_poll();
    CHECKI(v == 0x80000500u, "irq vector", v);
    CHECKI(gc.pc == 0x80000500u, "irq pc", gc.pc);
    CHECKI(gc.srr0 == 0x80005678u, "irq srr0", gc.srr0);
    CHECKI(!(gc.msr & 0x8000u), "msr EE cleared", gc.msr);
    /* EE 关闭时不再注入 */
    CHECKI(hal_irq_poll() == 0, "irq blocked when EE=0", 0);
    /* 恢复 EE + 清线 → 无注入 */
    gc.msr |= 0x8000u;
    hal_pi_clear_interrupt(PI_INT_DI);
    CHECKI(hal_irq_poll() == 0, "no irq after clear", 0);
}

static void t_di_read(void)
{
    hal_boot_prepare(DOL, ISO);
    /* 0x420 处盘上 BE32 = 0x1EC00：CMDBUF1 = 0x420>>2，LEN=4 */
    hal_di_write32(0x0C, 0x420u >> 2);      /* CMDBUF1 = 偏移>>2 */
    hal_di_write32(0x10, 4u);               /* CMDBUF2 = 源长 */
    hal_di_write32(0x14, 0x80010000u);      /* MAR */
    /* 注：DILENGTH 按 HW 掩 ~0x1F（Dolphin DirectWrite ~0x1F），4 会被掩成 0，
     * 用 0x20（首 4 字节即 0x1EC00） */
    hal_di_write32(0x18, 0x20u);            /* LEN（32 对齐） */
    hal_di_write32(0x08, (0xA8u << 24) | 0x00u);  /* CMD0 = 0xA8000000 */
    hal_di_write32(0x00, 1u << 3);          /* DISR.TCINTMASK=bit3（真实位序） */
    hal_di_write32(0x1C, 1u);               /* CR.TSTART */
    uint32_t got = hal_mem_read32(0x00010000);   /* phys of 0x80010000 */
    CHECKI(got == 0x0001EC00u, "di read 0x420 -> 0x1EC00", got);
    uint32_t disr = hal_di_read32(0x00);
    CHECKI((disr & (1u << 4)) != 0, "di TCINT set", disr);
    CHECKI((hal_di_read32(0x1C) & 1u) == 0, "di TSTART cleared", hal_di_read32(0x1C));
    /* 掩码开 → PI 线 */
    hal_pi_write32(0x04, PI_INT_DI);
    CHECKI((hal_pi_read32(0x00) & PI_INT_DI) != 0, "pi DI line", hal_pi_read32(0x00));
}

static void gx_link_fifo(uint32_t base, uint32_t end)
{
    hal_cp_write16(0x02, 0);
    hal_cp_write16(0x20, (uint16_t)base);
    hal_cp_write16(0x22, (uint16_t)(base >> 16));
    hal_cp_write16(0x24, (uint16_t)end);
    hal_cp_write16(0x26, (uint16_t)(end >> 16));
    hal_cp_write16(0x34, (uint16_t)base);
    hal_cp_write16(0x36, (uint16_t)(base >> 16));
    hal_cp_write16(0x38, (uint16_t)base);
    hal_cp_write16(0x3A, (uint16_t)(base >> 16));
    hal_cp_write16(0x02, 0x11u);
}

static void t_wgpipe(void)
{
    hal_boot_prepare(DOL, ISO);
    /* 设 PI FIFO 环：base 0x8000F000 / end 0x8000F100（8×32B） */
    hal_pi_write32(0x0C, 0x8000F000u);
    hal_pi_write32(0x10, 0x8000F100u);
    hal_pi_write32(0x14, 0x8000F000u);
    gx_link_fifo(0x0000F000u, 0x0000F100u);

    /* 喂一串命令：NOP×3、LOAD_CP(VCD_LO)、LOAD_XF(2 regs)、LOAD_BP(EFB_ADDR)、
     * SETDRAWDONE(2)、再加 16 字节图元（2 顶点×格式 8B）——
     * 先配 VCD/VAT 使顶点尺寸确定。 */
    uint8_t stream[] = {
        0x00, 0x00, 0x00,                                   /* NOP ×3 */
        0x08, 0x50, 0x00, 0x00, 0x00, 0x00,                 /* LOAD_CP VCD_LO = 0 */
        0x08, 0x70, 0x00, 0x00, 0x00, 0x00,                 /* LOAD_CP VAT_A[0] = 0 */
        0x08, 0x80, 0x00, 0x00, 0x00, 0x00,                 /* LOAD_CP VAT_B[0] = 0 */
        0x08, 0x90, 0x00, 0x00, 0x00, 0x00,                 /* LOAD_CP VAT_C[0] = 0 */
        0x10, 0x00, 0x00, 0x01, 0x00,                       /* LOAD_XF 2 regs（8B 载荷） */
              0, 0, 0, 0, 0, 0, 0, 0,
        0x61, 0x4B, 0x02, 0x60, 0x3F,                     /* Actual game copy target 0x004c07e0 */
        0x61, 0x52, 0x00, 0x4C, 0x00,                     /* LOAD_BP TRIGGER_EFB_COPY bit14 */
        0x90, 0x00, 0x04,                                   /* 图元 QUAD VAT0 4 顶点 × vsize=0 */
        0x00, 0x00, 0x00, 0x00,                             /* 凑 32 字节 */
    };
    size_t n = sizeof(stream);
    /* 追加 NOP 到 96 字节（3 个完整 burst）保证 gather pipe flush */
    uint8_t buf[96];
    memset(buf, 0x00, sizeof(buf));        /* NOP 填充无害 */
    memcpy(buf, stream, n);

    for (size_t i = 0; i < sizeof(buf); i++)
        hal_gxpipe_write8(buf[i]);

    CHECKI(hal_gx_bytes_fed() == 96, "gx bytes fed", (uint32_t)hal_gx_bytes_fed());
    CHECKI(hal_gx_bytes_consumed() == 96, "gx sync (consumed==fed)",
           (uint32_t)(hal_gx_bytes_fed() - hal_gx_bytes_consumed()));
    CHECKI(hal_gx_xf_load_count() == 1, "xf load counted", hal_gx_xf_load_count());
    CHECKI(hal_gx_efb_copy_dst() == 0x004C07E0u, "efb copy dst", hal_gx_efb_copy_dst());
    CHECKI(hal_gx_efb_copy_count() == 1, "efb copy done", hal_gx_efb_copy_count());
    /* PI FIFO 环推进：3 burst 后 wptr = base+96 */
    CHECKI(hal_pi_fifo_wptr == 0x8000F060u, "pi fifo wptr", hal_pi_fifo_wptr);
    CHECKI(hal_mem_read32(0x0000F000) ==
           (((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
            ((uint32_t)buf[2] << 8) | buf[3]), "fifo ring bytes", hal_mem_read32(0x0000F000));

    /* 环回卷：再喂 5 个 burst（160B）→ wptr 越过 end 回 base */
    uint8_t more[160];
    memset(more, 0, sizeof(more));
    for (size_t i = 0; i < sizeof(more); i++) hal_gxpipe_write8(more[i]);
    /* 恰好 8 burst 填满 [base,end)：按 Dolphin 后写后判语义 wptr=end */
    CHECKI(hal_pi_fifo_wptr == 0x8000F100u, "fifo wrap", hal_pi_fifo_wptr);
    CHECKI(hal_gx_bytes_consumed() == 96 + 160, "gx sync after wrap",
           (uint32_t)(hal_gx_bytes_fed() - hal_gx_bytes_consumed()));
    /* 第 9 个 burst：UpdateGatherPipe 先写当前 wp（=end）再判 wp==end → 回 base。
     * 数据落在 0xF100，wptr 回 0xF000。 */
    for (size_t i = 0; i < 32; i++) hal_gxpipe_write8(0x77);
    CHECKI(hal_pi_fifo_wptr == 0x8000F000u, "fifo wrap 9th burst -> base",
           hal_pi_fifo_wptr);
    CHECKI(hal_mem_read32(0x0000F100) == 0x77777777u, "9th burst bytes at end",
           hal_mem_read32(0x0000F100));
}

/* ---- WGPIPE 宽度写 + PE finish/token 中断协议 ----
 * 命令流：任意宽度写法必须与逐字节写在 FIFO 中逐位一致；
 * PE：pending&&enable 电平、PE_CTRL bits2/3 写1=ACK 撤线、ACK 读回为零。 */

/* 同一条字节流的写入形式。 */
static void wr_bytes (const uint8_t *d, size_t n)
{ for (size_t i = 0; i < n; i++) hal_gxpipe_write8(d[i]); }

static void wr_words16(const uint8_t *d, size_t n)
{
    size_t i = 0;
    for (; i + 1 < n; i += 2)
        hal_gxpipe_write16((uint16_t)(((uint16_t)d[i] << 8) | d[i + 1]));
    if (n & 1u) hal_gxpipe_write8(d[n - 1]);
}

static void wr_words32(const uint8_t *d, size_t n)
{
    size_t i = 0;
    for (; i + 4 <= n; i += 4)
        hal_gxpipe_write32(((uint32_t)d[i] << 24) | ((uint32_t)d[i+1] << 16)
                         | ((uint32_t)d[i+2] << 8) | d[i+3]);
    for (; i < n; i++) hal_gxpipe_write8(d[i]);
}

/* 游戏真实形态：stb + stw + …（首字节单写，其后 4 字节组 32 位写） */
static void wr_mixed(const uint8_t *d, size_t n)
{
    size_t i = 0;
    hal_gxpipe_write8(d[i++]);
    for (; i + 4 <= n; i += 4)
        hal_gxpipe_write32(((uint32_t)d[i] << 24) | ((uint32_t)d[i+1] << 16)
                         | ((uint32_t)d[i+2] << 8) | d[i+3]);
    for (; i < n; i++) hal_gxpipe_write8(d[i]);
}

/* 重置 GX/PI FIFO 环/PE 后按指定写法喂一条流，NOP 补满一个 burst flush */
static void gx_feed(const uint8_t *d, size_t n,
                    void (*wr)(const uint8_t *, size_t), uint16_t ctrl_init)
{
    static const uint8_t pad[32] = { 0 };
    hal_gx_init();
    hal_pi_write32(0x0C, 0x8000F000u);
    hal_pi_write32(0x10, 0x8000F100u);
    hal_pi_write32(0x14, 0x8000F000u);
    gx_link_fifo(0x0000F000u, 0x0000F100u);
    hal_pe_write16(0x0A, (uint16_t)(ctrl_init | 0x0Cu));   /* 清残留 + 设使能 */
    wr(d, n);
    wr(pad, sizeof(pad));
}

static void wr_mmio_pairs(const uint8_t *d, size_t n)
{
    size_t i = 0;
    hal_mmio_write8(HW_WGPIPE, d[i++]);
    for (; i + 8 <= n; i += 8) {
        uint32_t hi = ((uint32_t)d[i] << 24) | ((uint32_t)d[i+1] << 16)
                    | ((uint32_t)d[i+2] << 8) | d[i+3];
        uint32_t lo = ((uint32_t)d[i+4] << 24) | ((uint32_t)d[i+5] << 16)
                    | ((uint32_t)d[i+6] << 8) | d[i+7];
        hal_mmio_write32(HW_WGPIPE, hi);
        hal_mmio_write32(HW_WGPIPE + 4u, lo);
    }
    for (; i < n; i++) hal_mmio_write8(HW_WGPIPE + (uint32_t)(i & 31u), d[i]);
}

static void t_gx_width(void)
{
    /* 游戏真实指令 stb 0x61 + stw 0x45000002 = LOAD_BP 0x45 val 0x000002（5B）
     * 与目标文档的 9B 版本（前 5B + 0x45 00 00 02 字节序数据） */
    static const uint8_t V1[5] = { 0x61, 0x45, 0x00, 0x00, 0x02 };
    static const uint8_t V2[9] = { 0x61, 0x45, 0x00, 0x00, 0x02,
                                   0x45, 0x00, 0x00, 0x02 };
    void (*styles[5])(const uint8_t *, size_t) =
        { wr_bytes, wr_words16, wr_words32, wr_mixed, wr_mmio_pairs };
    static const char *sn[5] = { "byte", "16bit", "32bit", "mixed", "MMIO pairs" };
    static const unsigned V1_expected_cmd = 2;  /* LOAD_BP + 尾部 NOP 合并 */
    static const unsigned V2_expected_cmd = 5;  /* +0x45未知 +NOP(2) +0x02未知 */

    for (unsigned v = 0; v < 2; v++) {
        const uint8_t *d = v ? V2 : V1;
        size_t n = v ? sizeof(V2) : sizeof(V1);
        unsigned exp_cmd = v ? V2_expected_cmd : V1_expected_cmd;
        uint32_t base_cmd = 0, base_line = 0;
        for (unsigned s = 0; s < 5; s++) {
            char name[64];
            gx_feed(d, n, styles[s], 0x0002u);   /* finish 使能 + 双 ACK */
            snprintf(name, sizeof(name), "w%d %s fed", v + 1, sn[s]);
            CHECKI(hal_gx_bytes_fed() == 32, name, (uint32_t)hal_gx_bytes_fed());
            snprintf(name, sizeof(name), "w%d %s consumed", v + 1, sn[s]);
            CHECKI(hal_gx_bytes_consumed() == 32, name,
                   (uint32_t)hal_gx_bytes_consumed());
            snprintf(name, sizeof(name), "w%d %s cmds", v + 1, sn[s]);
            CHECKI(hal_gx_cmd_count() == exp_cmd, name, hal_gx_cmd_count());
            snprintf(name, sizeof(name), "w%d %s finish line", v + 1, sn[s]);
            uint32_t line = hal_pi_read32(0x00) & PI_INT_PE_FINISH;
            CHECKI(line != 0, name, line);
            snprintf(name, sizeof(name), "w%d %s ctrl status", v + 1, sn[s]);
            uint16_t ctrl = hal_pe_read16(0x0A);
            CHECKI((ctrl & 0x0Fu) == 0x02u, name, ctrl);   /* enable, ACK reads zero */
            if (s == 0) { base_cmd = hal_gx_cmd_count(); base_line = line; }
            else {
                snprintf(name, sizeof(name), "w%d %s == byte-style", v + 1, sn[s]);
                CHECKI(hal_gx_cmd_count() == base_cmd && line == base_line, name,
                       (hal_gx_cmd_count() << 16) | (line != base_line));
            }
            /* handler ACK：写回读值|ACK → 撤线、清状态、使能保留 */
            hal_pe_write16(0x0A, (uint16_t)(hal_pe_read16(0x0A) | 8u));
            snprintf(name, sizeof(name), "w%d %s ack drops line", v + 1, sn[s]);
            CHECKI((hal_pi_read32(0x00) & PI_INT_PE_FINISH) == 0, name,
                   hal_pi_read32(0x00));
            CHECKI((hal_pe_read16(0x0A) & 0x0Au) == 0x02u, "w ack status clear",
                   hal_pe_read16(0x0A));
        }
    }

    /* enable=0：DrawDone 只挂 pending 不拉线；后开使能 → 电平立即上升 */
    gx_feed(V1, sizeof(V1), wr_mixed, 0x0000u);
    CHECKI((hal_pi_read32(0x00) & PI_INT_PE_FINISH) == 0,
           "w no line when disabled", hal_pi_read32(0x00));
    hal_pe_write16(0x0A, 0x0002u);
    CHECKI((hal_pi_read32(0x00) & PI_INT_PE_FINISH) != 0,
           "w late enable raises line", hal_pi_read32(0x00));
    hal_pe_write16(0x0A, (uint16_t)(hal_pe_read16(0x0A) | 8u));
    CHECKI((hal_pi_read32(0x00) & PI_INT_PE_FINISH) == 0,
           "w ack after late enable", hal_pi_read32(0x00));

    /* token 线独立：BP 0x47 置 token（16 位值占 24 位载荷低位）→ token 线 */
    static const uint8_t TOK[5] = { 0x61, 0x47, 0x00, 0xAB, 0xCD };
    gx_feed(TOK, sizeof(TOK), wr_mixed, 0x0003u);   /* token+finish 使能 */
    CHECKI((hal_pi_read32(0x00) & PI_INT_PE_TOKEN) != 0, "w token line",
           hal_pi_read32(0x00));
    CHECKI((hal_pi_read32(0x00) & PI_INT_PE_FINISH) == 0, "w finish untouched",
           hal_pi_read32(0x00));
    CHECKI(hal_pe_read16(0x0E) == 0xABCDu, "w token value", hal_pe_read16(0x0E));
    hal_pe_write16(0x0A, (uint16_t)(hal_pe_read16(0x0A) | 4u));
    CHECKI((hal_pi_read32(0x00) & PI_INT_PE_TOKEN) == 0, "w token ack",
           hal_pi_read32(0x00));
    CHECKI((hal_pe_read16(0x0A) & 3u) == 3u, "w enables preserved",
           hal_pe_read16(0x0A));
}

static void t_dsp_aram(void)
{
    hal_boot_prepare(DOL, ISO);
    /* DSPReset 写 1 → 读回 0（SDK init spin 退出点） */
    hal_dsp_write16(0x0A, 0x0001);
    CHECKI((hal_dsp_csr() & 1u) == 0, "dsp reset self-clear", hal_dsp_csr());
    /* ARAM DMA：MRAM→ARAM 再读回 */
    hal_mem_write_burst(0x00100000, (const uint8_t *)"ARAM-TEST-BLOCK-32-BYTES!!!", 24);
    hal_dsp_write16(0x20, 0x0010u >> 0);   /* 占位（hi=0） */
    hal_dsp_write16(0x22, 0x0000u);        /* MMADDR lo → phys 0x0 */
    /* 直接用引擎 API（寄存器路径在 dsp 上已通） */
    hal_ar_dma(0x00100000, 0x100, 32, 0);  /* MRAM→ARAM */
    hal_ar_dma(0x00200000, 0x100, 32, 1);  /* ARAM→MRAM */
    CHECKI(hal_mem_read32(0x00200000) == hal_mem_read32(0x00100000),
           "aram roundtrip", hal_mem_read32(0x00200000));
    CHECKI(hal_ar_read8(0x100) == (hal_mem_read32(0x00100000) >> 24),
           "aram byte", hal_ar_read8(0x100));
}

/* EXI 访问按 SDK EXIBios：选中 = STATUS 写 CS 位，传输 = DMA_CTRL.TSTART。 */
static void exi_select(unsigned c, int dev)
{
    uint32_t keep = hal_exi_read32(c * 0x14) & 0x405u;
    hal_exi_write32(c * 0x14, keep | (dev >= 0 ? ((1u << dev) << 7) | (3u << 4) : 0u));
}

static uint32_t exi_imm(unsigned c, uint32_t data, unsigned n, unsigned write)
{
    hal_exi_write32(c * 0x14 + 0x10, data);
    hal_exi_write32(c * 0x14 + 0x0C, ((n - 1) << 4) | (write << 2) | 1u);
    return hal_exi_read32(c * 0x14 + 0x10);
}

static void exi_dma(unsigned c, uint32_t addr, uint32_t len, unsigned write)
{
    hal_exi_write32(c * 0x14 + 0x04, addr);
    hal_exi_write32(c * 0x14 + 0x08, len);
    hal_exi_write32(c * 0x14 + 0x0C, (write << 2) | 2u | 1u);
}

static void t_si_exi(void)
{
    hal_boot_prepare(DOL, ISO);
    /* SITransfer uses communication RAM; channel OUT belongs to polling. */
    hal_si_write32(0x80, 0);
    hal_si_write32(0x34, (1u << 16) | (3u << 8) | 1u);
    CHECKI((hal_si_read32(0x80) & 0xFFFFFF00u) == 0x09000000u, "si type id",
           hal_si_read32(0x80));
    CHECKI((hal_si_read32(0x34) & (1u << 31)) != 0, "si TCINT", hal_si_read32(0x34));
    hal_si_write32(0x80, 0x41000000u);
    hal_si_write32(0x34, (1u << 31) | (1u << 16) | (10u << 8) | 1u);
    CHECKI(hal_si_read32(0x80) == 0x00808080u, "si origin center",
           hal_si_read32(0x80));

    hal_si_write32(0x00, 0x00400300u);
    hal_si_write32(0x30, 0x80u);
    hal_si_frame_tick();
    uint32_t hi = hal_si_read32(0x04);
    CHECKI((hi & 0x1F7F0000u) == 0, "si neutral buttons", hi);
    CHECKI(((hi >> 8) & 0xFFu) == 0x80u, "si stick centered", hi);

    hal_si_set_pad(0, 0x1EF0u, 0x80, 0x80, 0x80, 0x80, 0, 0);
    hal_si_frame_tick();
    hi = hal_si_read32(0x04);
    CHECKI((hi & 0x1F7F0000u) == 0x01000000u, "si injected button", hi);
    hal_si_set_pad(0, 0x1FF0u, 0xC8u, 0x80, 0x80, 0x80, 0, 0);
    hal_si_frame_tick();
    hi = hal_si_read32(0x04);
    CHECKI(((hi >> 8) & 0xFFu) == 0xC8u, "si injected stick", hi);

    /* EXI 无卡：槽 A 无 EXT，EXIProbe 失败 */
    CHECKI((hal_exi_read32(0x00) & (1u << 12)) == 0, "exi slot A empty",
           hal_exi_read32(0x00));

    /* EXI RTC：ch0 dev1，IMM 写 0x20000000 再 IMM 读 4 字节（SDK ReadRTC） */
    exi_select(0, 1);
    exi_imm(0, 0x20000000u, 4, 1);
    uint32_t rtc = exi_imm(0, 0, 4, 0);
    exi_select(0, -1);
    CHECKI(rtc != 0xFFFFFFFFu && rtc > 700000000u, "exi rtc", rtc);
    CHECKI((hal_exi_read32(0x00) & (1u << 3)) != 0, "exi TCINT",
           hal_exi_read32(0x00));

    /* EXI SRAM：IMM 写 0x20000100 + DMA 读 64 字节（SDK ReadSram） */
    exi_select(0, 1);
    exi_imm(0, 0x20000100u, 4, 1);
    exi_dma(0, 0x00300000u, 64, 0);
    exi_select(0, -1);
    uint32_t w = hal_mem_read32(0x00300000u + 0x0C), w2 = hal_mem_read32(0x00300000u + 0x10);
    uint16_t sum = (uint16_t)((w >> 16) + (w & 0xFFFF) + (w2 >> 16) + (w2 & 0xFFFF));
    CHECKI((hal_mem_read32(0x00300000u) >> 16) == sum, "exi sram checksum",
           hal_mem_read32(0x00300000u));
    CHECKI((hal_mem_read32(0x00300000u + 0x38) & 0xFFFFu) == 0xFFFFu,
           "exi sram flashID checksums", hal_mem_read32(0x00300000u + 0x38));
}

static void t_memcard(void)
{
    const char *path = "build/selftest_memcard.raw";
    remove(path);
    hal_exi_set_memcard(path);
    hal_boot_prepare(DOL, ISO);
    CHECKI((hal_exi_read32(0x00) & (1u << 12)) != 0, "memcard EXT", hal_exi_read32(0x00));
    /* EXIGetID：写 2 字节 0x0000，读 4 字节 → 16 Mbit */
    exi_select(0, 0);
    exi_imm(0, 0, 2, 1);
    uint32_t id = exi_imm(0, 0, 4, 0);
    exi_select(0, -1);
    CHECKI(id == 0x00000010u, "memcard id", id);
    /* 0x83 读状态：UNLOCKED|READY，跳过 __CARDUnlock */
    exi_select(0, 0);
    exi_imm(0, 0x83000000u, 2, 1);
    uint32_t st = exi_imm(0, 0, 1, 0) >> 24;
    exi_select(0, -1);
    CHECKI(st == 0x41u, "memcard status", st);
    /* 新卡已格式化：ID 块 size 字段与 FAT freeBlocks */
    exi_select(0, 0);
    exi_imm(0, 0x52000000u, 4, 1); exi_imm(0, 0, 1, 1);   /* addr 0 */
    exi_imm(0, 0, 4, 1);                                  /* latency */
    exi_dma(0, 0x00310000u, 512, 0);
    exi_select(0, -1);
    CHECKI((hal_mem_read32(0x00310000u + 0x20) & 0xFFFFu) == 16u, "memcard header size",
           hal_mem_read32(0x00310000u + 0x20));
    CHECKI((hal_mem_read32(0x00310000u + 0x24) >> 16) == 1u, "memcard header SJIS",
           hal_mem_read32(0x00310000u + 0x24));
    CHECKI((hal_vi_read16(0x6E) & 2u) != 0, "vi DTV NTSC-J", hal_vi_read16(0x6E));
    /* 0x81 开中断；擦除扇区 5，编程 128 字节，等完成中断 */
    exi_select(0, 0); exi_imm(0, 0x81010000u, 2, 1); exi_select(0, -1);
    hal_exi_write32(0x00, 1u);                            /* EXIINTMASK */
    const uint32_t a = 5u * 0x2000u;
    exi_select(0, 0);
    exi_imm(0, 0xF1000000u | ((a >> 17) & 0x7F) << 16 | ((a >> 9) & 0xFF) << 8, 3, 1);
    exi_select(0, -1);
    CHECKI((hal_exi_read32(0x00) & 2u) == 0, "memcard erase pending", hal_exi_read32(0x00));
    hal_exi_tick_cycles(100000);
    CHECKI((hal_exi_read32(0x00) & 2u) != 0, "memcard erase EXIINT", hal_exi_read32(0x00));
    hal_exi_write32(0x00, 3u);                            /* W1C EXIINT */
    for (uint32_t i = 0; i < 128; i += 4) hal_mem_write32(0x00320000u + i, 0xC0DE0000u | i);
    exi_select(0, 0);
    exi_imm(0, 0xF2000000u | ((a >> 17) & 0x3F) << 16 | ((a >> 9) & 0xFF) << 8 | ((a >> 7) & 3), 4, 1);
    exi_imm(0, (a & 0x7F) << 24, 1, 1);
    exi_dma(0, 0x00320000u, 128, 1);
    exi_select(0, -1);
    hal_exi_tick_cycles(100000);
    exi_select(0, 0);
    exi_imm(0, 0x52000000u | ((a >> 17) & 0x7F) << 16 | ((a >> 9) & 0xFF) << 8 | ((a >> 7) & 3), 4, 1);
    exi_imm(0, (a & 0x7F) << 24, 1, 1);
    exi_imm(0, 0, 4, 1);
    exi_dma(0, 0x00330000u, 512, 0);
    exi_select(0, -1);
    CHECKI(hal_mem_read32(0x00330000u + 0x40) == 0xC0DE0040u, "memcard program/read",
           hal_mem_read32(0x00330000u + 0x40));
    CHECKI(hal_mem_read32(0x00330000u + 0x80) == 0xFFFFFFFFu, "memcard erased tail",
           hal_mem_read32(0x00330000u + 0x80));
    CHECKI(hal_exi_flush() == 0, "memcard flush", 0);
    FILE *f = fopen(path, "rb");
    long n = -1;
    if (f) { fseek(f, 0, SEEK_END); n = ftell(f); fclose(f); }
    CHECKI(n == 0x200000, "memcard file size", (uint32_t)n);
    hal_exi_set_memcard(NULL);
    remove(path);
}

static void t_vi_frame(void)
{
    hal_boot_prepare(DOL, ISO);
    /* 64x240 interlaced XFB: each field advances two 128-byte rows. */
    hal_vi_write16(0x1C, 0x1000u);   /* POFF: FBB is in 32-byte units */
    hal_vi_write16(0x1E, 0x0980u);   /* top field @ 0x13000 */
    hal_vi_write16(0x24, 0x0000u);
    hal_vi_write16(0x26, 0x0984u);   /* bottom field @ 0x13080 */
    hal_vi_write16(0x48, (4u << 8) | 8u); /* WPL=4, STD=8 */
    hal_vi_write16(0x00, (120u << 4));
    hal_vi_write16(0x02, 0);
    /* Neutral chroma and Y=128 produce gray in both fields. */
    memset(gc_ram + 0x13000, 0x80, 64u * 240u * 2u);
    uint8_t *rgba; int w, h;
    int rc = hal_get_frame(&rgba, &w, &h);
    CHECKI(rc == 0 && w == 64 && h == 240, "vi frame size", (unsigned)((w << 16) | h));
    CHECKI(rgba && rgba[0] == 0x80 && rgba[1] == 0x80 && rgba[2] == 0x80,
           "vi yuy2->rgba gray", rgba ? rgba[0] : 0xFF);

    /* Enable the DOL's measured NTSC scan and DI0 second-field compare. */
    hal_vi_write16(0x00, 0x0f06);
    hal_vi_write16(0x06, 429);
    hal_vi_write16(0x0c, 3); hal_vi_write16(0x0e, 24);
    hal_vi_write16(0x10, 2); hal_vi_write16(0x12, 25);
    hal_vi_write16(0x6c, 0);
    hal_vi_write16(0x02, 1);
    hal_mmio_write32(0x0C002030, 0x110701ae);
    hal_frame_tick();
    CHECKI((hal_vi_reg(0x30) & 0x8000u) != 0, "vi IR_INT", hal_vi_reg(0x30));
    CHECKI((hal_pi_read32(0x00) & PI_INT_VI) != 0, "pi VI line", hal_pi_read32(0x00));
}

static void t_aid_dma(void)
{
    hal_boot_prepare(DOL, ISO);
    hal_dsp_init();
    /* r89 Dolphin 权威位序：bit11=DSPInit 标志、bit4=AID_mask（非 bit11！） */
    hal_dsp_write16(0x0A, 0x0810u);              /* DSPInit(bit11) + AID_mask(bit4) */
    hal_pi_write32(0x04, PI_INT_DSP);            /* PI mask */
    /* AIStartDMA(0x00100000, 640)：32kHz 立体声 16bit ≈ 5ms ≈ 0.3 虚拟帧 */
    hal_dsp_write16(0x30, 0x0010u);              /* addr hi */
    hal_dsp_write16(0x32, 0x0000u);              /* addr lo */
    hal_dsp_write16(0x36, 0x8000u | (640u >> 5));/* len + enable */
    CHECKI((hal_dsp_read16(0x36) & 0x8000u) != 0, "aid enable readback",
           hal_dsp_read16(0x36));
    CHECKI(hal_dsp_read16(0x3A) == (640u >> 5), "aid bytes-left init",
           hal_dsp_read16(0x3A));
    CHECKI((hal_pi_read32(0x00) & PI_INT_DSP) == 0, "aid no line before done",
           hal_pi_read32(0x00));

    /* 一个虚拟帧（≈2133B 配额）即完成 640B */
    hal_dsp_frame_tick();
    CHECKI(hal_dsp_read16(0x3A) == 0, "aid bytes-left drained",
           hal_dsp_read16(0x3A));
    CHECKI((hal_pi_read32(0x00) & PI_INT_DSP) != 0, "aid PI line (mask4&cause3)",
           hal_pi_read32(0x00));
    CHECKI((hal_dsp_csr() & 0x0008u) != 0, "aid cause bit3", hal_dsp_csr());

    /* __AIDHandler ACK：DSPCR = (tmp & ~0xA0) | 8 → cause bit3 W1C 清除 → 撤线 */
    uint16_t tmp = hal_dsp_csr();
    hal_dsp_write16(0x0A, (uint16_t)((tmp & ~0xA0u) | 8u));
    CHECKI((hal_pi_read32(0x00) & PI_INT_DSP) == 0, "aid ack drops line",
           hal_pi_read32(0x00));
    CHECKI((hal_dsp_csr() & 0x0008u) == 0, "aid cause W1C cleared", hal_dsp_csr());

    /* AR handler 的 |0x20 写（W1C 清 bit5）不得重新拉线（无新完成） */
    hal_dsp_write16(0x0A, (uint16_t)(hal_dsp_csr() | 0x20u));
    CHECKI((hal_pi_read32(0x00) & PI_INT_DSP) == 0, "aid no re-fire",
           hal_pi_read32(0x00));

    /* 无掩码（bit4=0）时完成不拉线——电平公式；bit11(DSPInit) 不是掩码 */
    hal_dsp_write16(0x0A, 0x0800u);
    hal_dsp_write16(0x36, 0x8000u | (640u >> 5));
    hal_dsp_frame_tick();
    CHECKI((hal_dsp_csr() & 0x0008u) != 0, "cause set without mask",
           hal_dsp_csr());
    CHECKI((hal_pi_read32(0x00) & PI_INT_DSP) == 0, "no mask -> no line",
           hal_pi_read32(0x00));
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("== hal_selftest ==\n");
    t_runtime_manifest();
    t_be_mem();
    const int game = have_game_files();
    if (game) t_dol_load();
    else printf("SKIP DOL load, IRQ injection and DI reads: %s / %s not found\n", DOL, ISO);
    t_fst_boot_validation();
    if (game) { t_irq_inject(); t_di_read(); }
    t_wgpipe();
    t_gx_width();
    t_dsp_aram();
    t_si_exi();
    t_memcard();
    t_aid_dma();
    t_vi_frame();
    printf(fails ? "== %d FAIL ==\n" : "== ALL GREEN ==\n", fails);
    return fails ? 1 : 0;
}
