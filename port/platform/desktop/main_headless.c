/* platform/desktop/main_headless.c — headless 帧转储驱动。
 * 用法：main_headless <main.dol> <disc.iso> [frames]
 * 流程：hal_boot_prepare → gc_entry()（生成代码在场时永不返回；无生成代码时
 * 弱入口打印 MISS 并 abort——这是定义好的诊断行为）。
 * 帧节流：host 侧 hal_frame_tick() + 帧回调（VI retrace 时取帧写 PPM），
 * 生成代码在场时由 hal_irq_poll 的时间自然到期驱动同样路径。
 * PPM：前 10 帧 + 每 60 帧。
 */
#include "../../hal/hal_boot.h"
#include "../../hal/irq.h"
#include "../../hal/vi.h"
#include "../../hal/gx.h"
#include "../../hal/di.h"
#include "../../hal/mem.h"
#include "../../hal/gc_abi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

static uint32_t ppm_frame;

static void write_ppm(const char *path, const uint8_t *rgba, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++)
        fwrite(rgba + 4 * i, 1, 3, f);
    fclose(f);
}

static void on_frame(void *user)
{
    (void)user;
    uint8_t *rgba;
    int w, h;
    ppm_frame++;
    static long stop_frame = -1;
    if (stop_frame < 0) {
        const char *e = getenv("GC_STOP_FRAME");
        stop_frame = e ? strtol(e, NULL, 0) : 0;
    }
    if (stop_frame > 0 && ppm_frame >= (uint32_t)stop_frame) raise(SIGABRT);
    bool dump = (ppm_frame <= 10) || (ppm_frame % 60 == 0);
    if (!dump) return;
    if (hal_get_frame(&rgba, &w, &h) == 0 && w && h) {
        char path[128];
        snprintf(path, sizeof(path), "build/frame_%05d.ppm", ppm_frame);
        write_ppm(path, rgba, w, h);
        printf("[frame %u] %dx%d -> %s (xfb@%08x)\n",
               ppm_frame, w, h, path, hal_vi_xfb_top());
    } else {
        printf("[frame %u] no XFB configured yet\n", ppm_frame);
    }
}

/* ---- pc 采样器：SIGABRT 时倾倒执行轨迹 ---- */
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
static volatile uint32_t pc_samples[8192];
static volatile uint32_t srr0_samples[4096];
static volatile uint64_t pc_n, srr0_n;
static volatile int sampling;
static volatile uint32_t main_pc[8192];  /* E4==主线程时的 pc 采样（r41） */
static volatile uint64_t main_pc_n;
static void *sampler(void *arg)
{
    (void)arg;
    while (sampling) {
        uint32_t p = gc.pc;
        pc_samples[pc_n % 8192] = p;
        {   /* r41：当前线程=E4；主线程样本单列，回答“帧工作去了哪” */
            uint32_t e4 = ((uint32_t)gc_ram[0xE4] << 24) | ((uint32_t)gc_ram[0xE5] << 16)
                        | ((uint32_t)gc_ram[0xE6] << 8) | gc_ram[0xE7];
            if (e4 == 0x803c3150u) {
                main_pc[main_pc_n % 8192] = p;
                main_pc_n++;
            }
        }
        pc_n++;
        {   /* OS 线程取证：当前上下文的 SRR0 = 该线程停靠点 */
            uint32_t ctx = ((uint32_t)gc_ram[0xC0] << 24) | ((uint32_t)gc_ram[0xC1] << 16)
                         | ((uint32_t)gc_ram[0xC2] << 8) | gc_ram[0xC3];
            if (ctx & 0x80000000u) ctx &= 0x1FFFFFFFu;
            if (ctx >= 0x1000u && ctx + 0x1A0u <= 0x01800000u) {
                uint32_t srr0 = ((uint32_t)gc_ram[ctx + 0x198] << 24)
                              | ((uint32_t)gc_ram[ctx + 0x199] << 16)
                              | ((uint32_t)gc_ram[ctx + 0x19A] << 8)
                              | gc_ram[ctx + 0x19B];
                srr0_samples[srr0_n % 4096] = srr0;
                srr0_n++;
            }
        }
        usleep(100);
    }
    return NULL;
}
static void dump_samples(int sig)
{
    sampling = 0;
    {   /* 死锁现场：热点循环关键寄存器与其指向内存 */
        static const int regs[] = { 29, 31, 30, 28, 27, 12, 11, 9, 10, 6, 3, 4 };
        for (unsigned k = 0; k < sizeof(regs)/sizeof(regs[0]); k++) {
            uint32_t v = gc.r[regs[k]];
            fprintf(stderr, "r%d=%08x:", regs[k], v);
            if (v >= 0x80000000u && v < 0x81800000u) {
                uint32_t pa = v & 0x1FFFFFFFu;
                fprintf(stderr, " ascii=");
                for (int b = 0; b < 128; b++) {
                    uint8_t c = gc_ram[(pa + b) & 0x1FFFFFFu];
                    fputc(c >= 0x20 && c < 0x7F ? c : '.', stderr);
                }
                fprintf(stderr, "\n");
            }
            fprintf(stderr, "\n");
        }
    }
    {   /* PPC 栈回溯：back chain [sp], lr [sp+4] */
        uint32_t sp = gc.r[1];
        fprintf(stderr, "stack-trace: sp=%08x r13=%08x:", sp, gc.r[13]);
        for (int d = 0; d < 14 && sp >= 0x80000000u && sp < 0x81800000u; d++) {
            uint32_t pa = sp & 0x1FFFFFFFu;
            uint32_t lr = ((uint32_t)gc_ram[pa+4]<<24) | ((uint32_t)gc_ram[pa+5]<<16)
                        | ((uint32_t)gc_ram[pa+6]<<8) | gc_ram[pa+7];
            uint32_t nxt = ((uint32_t)gc_ram[pa]<<24) | ((uint32_t)gc_ram[pa+1]<<16)
                         | ((uint32_t)gc_ram[pa+2]<<8) | gc_ram[pa+3];
            fprintf(stderr, " lr%02d=%08x", d, lr);
            if (nxt <= sp) break;
            sp = nxt;
        }
        fprintf(stderr, "\n");
    }
    {   /* OSReport 现场：r3-r10 + 栈头 96 字节 */
        fprintf(stderr, "regs:");
        for (int k = 0; k <= 10; k++)
            fprintf(stderr, " r%d=%08x", k, gc.r[k]);
        fprintf(stderr, "\n");
        uint32_t sp = gc.r[1] & 0x1FFFFFFFu;
        fprintf(stderr, "stack:");
        for (int w = 0; w < 24; w++) {
            uint32_t v = (uint32_t)gc_ram[sp+4*w]<<24 | (uint32_t)gc_ram[sp+4*w+1]<<16
                       | (uint32_t)gc_ram[sp+4*w+2]<<8 | gc_ram[sp+4*w+3];
            fprintf(stderr, " %08x", v);
        }
        fprintf(stderr, "\n");
    }
    {   /* 位图现场：r13-0x626c..-0x6254 与 r31 表头部 */
        uint32_t sda = gc.r[13];
        uint32_t addrs2[] = { sda - 0x626c, sda - 0x6268, sda - 0x6264,
                              sda - 0x6260, sda - 0x6258, sda - 0x6254 };
        for (unsigned k = 0; k < 6; k++) {
            uint32_t pa = addrs2[k] & 0x1FFFFFFFu;
            uint32_t v = (uint32_t)gc_ram[pa]<<24 | (uint32_t)gc_ram[pa+1]<<16
                       | (uint32_t)gc_ram[pa+2]<<8 | gc_ram[pa+3];
            fprintf(stderr, "flag[%08x]=%08x\n", addrs2[k], v);
        }
        uint32_t tb = gc.r[31] & 0x1FFFFFFFu;
        fprintf(stderr, "r31 table:");
        for (int b = 0; b < 32; b++)
            fprintf(stderr, " %02x", gc_ram[(tb + b) & 0x1FFFFFFu]);
        fprintf(stderr, "\n");
    }
    {   /* 中断注入健康度：EE 关闭/注入/pending 比例定位"关中断空转" */
        extern uint64_t hal_irq_diag_ee_off, hal_irq_diag_injected,
                         hal_irq_diag_pending;
        {   /* 注入分支计数：line=EE开+PI线pending；notready=handler表无效拦下 */
            extern uint64_t hal_irq_diag_line, hal_irq_diag_notready;
            fprintf(stderr,
                    "irq-diag: line_pending=%llu ext_not_ready=%llu\n",
                    (unsigned long long)hal_irq_diag_line,
                    (unsigned long long)hal_irq_diag_notready);
        }
        {   /* msr 值分布：低 4 位与 bit15:12 各 16 桶 */
            extern uint64_t hal_irq_diag_msr_hist[16];
            extern uint64_t hal_irq_diag_msr_hi8[16];
            fprintf(stderr, "msr hi4: 0=");
            for (int k = 0; k < 16; k++)
                if (hal_irq_diag_msr_hist[k])
                    fprintf(stderr, " %x=%llu", k,
                            (unsigned long long)hal_irq_diag_msr_hist[k]);
            fprintf(stderr, "\nmsr b15-12: 0=");
            for (int k = 0; k < 16; k++)
                if (hal_irq_diag_msr_hi8[k])
                    fprintf(stderr, " %x=%llu", k,
                            (unsigned long long)hal_irq_diag_msr_hi8[k]);
            fprintf(stderr, "\n");
        }
        {   /* 注入时的中断源位分布：定位中断风暴来自哪条 PI 线 */
            extern uint64_t hal_irq_diag_src[16];
            fprintf(stderr, "irq src:");
            for (int k = 0; k < 16; k++)
                if (hal_irq_diag_src[k])
                    fprintf(stderr, " bit%d=%llu", k,
                            (unsigned long long)hal_irq_diag_src[k]);
            fprintf(stderr, "\n");
        }
    }
    extern void gc_trace_dump(const char *path);
    gc_trace_dump("trace_last.txt");
    { const char *path = getenv("GC_RAM_DUMP");
      if (path && *path) {
          FILE *ram = fopen(path, "wb");
          if (ram) { fwrite(gc_ram, 1, GC_MEM1_SIZE, ram); fclose(ram); }
      } }
    gc_runtime_manifest_dump_default();
    hal_gx_dump_state();
    {   /* OS 调度取证：current thread / 状态 / 运行队列 / 上下文 */
#define RD32(va) ({ uint32_t pa_ = (va) & 0x1FFFFFFFu; \
            (pa_ + 4 > 0x01800000u) ? 0xDEADBEEFu : \
            (((uint32_t)gc_ram[pa_] << 24) | ((uint32_t)gc_ram[pa_+1] << 16) \
           | ((uint32_t)gc_ram[pa_+2] << 8) | gc_ram[pa_+3]); })
#define RD16(va) ({ uint32_t pa_ = (va) & 0x1FFFFFFFu; \
            (pa_ + 2 > 0x01800000u) ? 0xDEADu : \
            (((uint32_t)gc_ram[pa_] << 8) | gc_ram[pa_+1]); })
        uint32_t ctx = RD32(0x800000D4u);
        uint32_t th = RD32(0x800000E4u);
        fprintf(stderr, "sched: ctx(0xD4)=%08x thread(0xE4)=%08x\n", ctx, th);
        fprintf(stderr, "  cpu: pc=%08x lr=%08x msr=%08x srr0=%08x srr1=%08x\n",
                gc.pc, gc.lr, gc.msr, gc.srr0, gc.srr1);
        if (th >= 0x80000000u && th < 0x81800000u) {
            fprintf(stderr, "  thread: state(+2C8)=%04x attr(+2CA)=%04x "
                            "prio(+2D0)=%u suspend(+2CC)=%d queue(+2DC)=%08x\n",
                    (unsigned)RD16(th + 0x2C8u), (unsigned)RD16(th + 0x2CAu),
                    RD32(th + 0x2D0u), (int32_t)RD32(th + 0x2CCu),
                    RD32(th + 0x2DCu));
            fprintf(stderr, "  thread: SRR0(+198)=%08x SRR1(+19C)=%08x\n",
                    RD32(th + 0x198u), RD32(th + 0x19Cu));
        }
        /* 真实 retrace 计数/队列（r13-0x628C/-0x6284）：0x803CFE94 / 0x803CFE9C */
        fprintf(stderr, "  retraceCount(803CFE94)=%08x  Q(803CFE9C): head=%08x tail=%08x\n",
                RD32(0x803CFE94u), RD32(0x803CFE9Cu), RD32(0x803CFEA0u));
        fprintf(stderr, "  VI门控(803CFE90/98/9C..): %08x %08x\n",
                RD32(0x803CFE90u), RD32(0x803CFE98u));
        fprintf(stderr, "  803C2D38(RunQueue?): %08x %08x %08x %08x\n",
                RD32(0x803C2D38u), RD32(0x803C2D3Cu), RD32(0x803C2D40u), RD32(0x803C2D44u));
        fprintf(stderr, "  r13glob(803CFD60)=%08x  C394struct: %08x %08x\n",
                RD32(0x803CFD60u), RD32(0x803CC394u), RD32(0x803CC594u));
        {   /* RunQueue[31]（prio31 线程）与其上下文 */
            uint32_t q31 = 0x803C2D38u + 31u * 8u;
            uint32_t th = RD32(q31);
            fprintf(stderr, "  RunQueue[31]@%08x head=%08x\n", q31, th);
            if (th >= 0x80000000u && th < 0x81800000u) {
                fprintf(stderr, "  idle-th: SRR0(+198)=%08x SRR1(+19C)=%08x "
                                "state(+2C8)=%04x queue(+2DC)=%08x\n",
                        RD32(th + 0x198u), RD32(th + 0x19Cu),
                        (unsigned)RD16(th + 0x2C8u), RD32(th + 0x2DCu));
                fprintf(stderr, "  idle-th GPR1(sp,+4)=%08x\n", RD32(th + 4u));
            }
        }
        {   /* HAL 侧：VI 中断使能影子 + PI cause/mask */
            extern uint32_t hal_pi_mask_dbg(void);
            fprintf(stderr, "  VI irEn(0x30)=%08x\n", hal_mmio_read16(0x0C002030u) << 16
                    | hal_mmio_read16(0x0C002032u));
            fprintf(stderr, "  VI 0x34=%08x 0x38=%08x\n",
                    (unsigned)(hal_mmio_read16(0x0C002034u) << 16 | hal_mmio_read16(0x0C002036u)),
                    (unsigned)(hal_mmio_read16(0x0C002038u) << 16 | hal_mmio_read16(0x0C00203Au)));
            fprintf(stderr, "  PI cause=%08x mask=%08x\n",
                    hal_mmio_read32(0x0C003000u), hal_mmio_read32(0x0C003004u));
            fprintf(stderr, "  switchfn(803CF348)=%08x  803CF344=%08x 803CF34C=%08x\n",
                    RD32(0x803CF348u), RD32(0x803CF344u), RD32(0x803CF34Cu));
            fprintf(stderr, "  ctx->SRR0(*(0xD4)+198)=%08x  thread+198=%08x\n",
                    RD32((RD32(0x800000D4u) & 0x1FFFFFFFu) + 0x198u),
                    RD32(RD32(0x800000E4u) + 0x198u));
        }
        fprintf(stderr, "  RunQ bitmap=%08x reschedule=%08x GPU-wait=%08x submitted=%08x\n",
                RD32(0x803CFD58u), RD32(0x803CFD5Cu),
                RD32(0x803CECB0u), RD32(0x803CF9BCu));
        fprintf(stderr, "  GX fed=%llu consumed=%llu commands=%u copies=%u dst=%08x; "
                        "DI reads=%llu bytes=%llu\n",
                (unsigned long long)hal_gx_bytes_fed(),
                (unsigned long long)hal_gx_bytes_consumed(), hal_gx_cmd_count(),
                hal_gx_efb_copy_count(), hal_gx_efb_copy_dst(),
                (unsigned long long)hal_di_read_count(),
                (unsigned long long)hal_di_read_bytes());
#undef RD16
#undef RD32
    }
    {   /* OS 线程停靠点直方图：__OSCurrentContext.SRR0 采样 */
        uint32_t uniq[64], cnt[64];
        unsigned n = 0, q, j;
        for (uint64_t k = 0; k < srr0_n && k < 4096u; k++) {
            uint32_t v = srr0_samples[k % 4096];
            for (j = 0; j < n; j++)
                if (uniq[j] == v) { cnt[j]++; break; }
            if (j == n && n < 64) { uniq[n] = v; cnt[n] = 1; n++; }
        }
        for (q = 0; q < n; q++)
            for (j = q + 1; j < n; j++)
                if (cnt[j] > cnt[q]) {
                    uint32_t t = uniq[q]; uniq[q] = uniq[j]; uniq[j] = t;
                    uint32_t c = cnt[q]; cnt[q] = cnt[j]; cnt[j] = c;
                }
        fprintf(stderr, "srr0 samples=%llu uniq=%u\n", (unsigned long long)srr0_n, n);
        for (q = 0; q < n && q < 12; q++)
            fprintf(stderr, "  srr0=%08x x%u\n", uniq[q], cnt[q]);
    }
    {   /* 主线程专属直方图（r41） */
        uint32_t mu[64], mc[64];
        unsigned n = 0, q, j2;
        for (uint64_t k = 0; k < main_pc_n && k < 8192u; k++) {
            uint32_t v = main_pc[k % 8192];
            for (j2 = 0; j2 < n; j2++)
                if (mu[j2] == v) { mc[j2]++; break; }
            if (j2 == n && n < 64) { mu[n] = v; mc[n] = 1; n++; }
        }
        for (q = 0; q < n; q++)
            for (j2 = q + 1; j2 < n; j2++)
                if (mc[j2] > mc[q]) {
                    uint32_t t = mu[q]; mu[q] = mu[j2]; mu[j2] = t;
                    uint32_t c = mc[q]; mc[q] = mc[j2]; mc[j2] = c;
                }
        fprintf(stderr, "main-thread pc samples=%llu uniq=%u\n",
                (unsigned long long)main_pc_n, n);
        for (q = 0; q < n && q < 10; q++)
            fprintf(stderr, "  main pc=%08x x%u\n", mu[q], mc[q]);
    }
    FILE *f = fopen("pc_trace.txt", "w");
    if (f) {
        uint64_t start = pc_n > 8192 ? pc_n - 8192 : 0;
        for (uint64_t k = start; k < pc_n; k++)
            fprintf(f, "%08x\n", pc_samples[k % 8192]);
        fclose(f);
    }
    _exit(sig);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <main.dol> <disc.iso> [frames]\n", argv[0]);
        return 2;
    }
    if (!hal_boot_prepare(argv[1], argv[2])) {
        fprintf(stderr, "boot prepare failed\n");
        return 1;
    }
    gc_runtime_manifest_reset();
    printf("boot ok: entry=%08x r1=%08x msr=%08x\n",
           hal_boot_entry(), gc.r[1], gc.msr);

    { const char *e = getenv("GC_MMIO_TRACE"); extern int hal_mmio_trace;
      hal_mmio_trace = e && *e && *e != '0'; }
    hal_set_frame_callback(on_frame, NULL);
    signal(SIGABRT, dump_samples);
    pthread_t th;
    sampling = 1;
    pthread_create(&th, NULL, sampler, NULL);

    /* 无生成代码（M1 阶段）：硬件侧驱动 N 帧（DI/VI/GX 行为可观测） */
    long frames = (argc > 3) ? strtol(argv[3], NULL, 0) : 3;
    for (long i = 0; i < frames; i++)
        hal_frame_tick();

    /* 生成代码在场时：进入 CPU（永不返回）。缺位时弱入口打印 MISS 后 abort。 */
    gc_entry();
    gc_runtime_manifest_dump_default();
    return 0;
}
