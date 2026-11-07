/* hal/mem.h — MEM1 (24MB) 大端 RAM + DOL 加载。
 * 布局事实：analysis/disc_info.json（entry=0x80003100；data[3]=0x803CE120 与
 * bss [0x80369300,0x803CFF68) 重叠 → 必须先清 bss 再拷段）。
 */
#ifndef HAL_MEM_H
#define HAL_MEM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

#define GC_MEM1_SIZE   0x01800000u   /* 24MB MEM1（0x8000002C 处 RAM 大小同值） */
#define GC_MEM1_BASE   0x80000000u
#define GC_ARAM_SIZE   0x01000000u   /* 16MB ARAM（见 hal/ar.h） */

/* 有效地址 → 物理地址规范化：0x8xxxxxxx 与 0xCxxxxxxx 同源 */
static inline uint32_t hal_ea_to_phys(uint32_t ea)
{
    return ea & 0x1FFFFFFFu;
}

/* gc_ram 按字节原始存储；下列访问器实现 GC 大端语义 */
extern uint8_t gc_ram[GC_MEM1_SIZE];

uint8_t  hal_mem_read8 (uint32_t phys);
uint16_t hal_mem_read16(uint32_t phys);
uint32_t hal_mem_read32(uint32_t phys);
uint64_t hal_mem_read64(uint32_t phys);
void     hal_mem_write8 (uint32_t phys, uint8_t  v);
void     hal_mem_write16(uint32_t phys, uint16_t v);
void     hal_mem_write32(uint32_t phys, uint32_t v);
void     hal_mem_write64(uint32_t phys, uint64_t v);

void     hal_mem_read_burst (uint32_t phys, uint8_t *dst, size_t n);
void     hal_mem_write_burst(uint32_t phys, const uint8_t *src, size_t n);

/* DOL 头（64 字节 × 0x40 字段，大端）解析与加载。
 * 返回 entry 地址；失败返回 0 并置 *err（静态字符串）。 */
typedef struct {
    uint32_t text_off[7], data_off[11];
    uint32_t text_addr[7], data_addr[11];
    uint32_t text_size[7], data_size[11];
    uint32_t bss_addr, bss_size, entry;
} HalDolHeader;

bool hal_dol_parse(const uint8_t *file, size_t filesize, HalDolHeader *out,
                   const char **err);
/* 先清整个声明 bss，再拷全部段（顺序是硬性要求，见文件头注释） */
bool hal_dol_load(const HalDolHeader *h, const uint8_t *file, size_t filesize,
                  const char **err);

/* 引导前内存状态（等效 BS2 SetupGCMemory；Dolphin Boot_BS2Emu.cpp）。
 * disc_iso 需已定位到偏移 0：拷头部 0x100 字节 + bi2。 */
bool hal_write_boot_state(FILE *disc_iso, const char **err);

#endif /* HAL_MEM_H */
