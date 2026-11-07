/* hal/vi.h — Video Interface。
 * 寄存器偏移与位域出处：
 *   Dolphin Source/Core/Core/HW/VideoInterface.h
 *   https://raw.githubusercontent.com/dolphin-emu/dolphin/master/Source/Core/Core/HW/VideoInterface.h
 *   VI_VERTICAL_TIMING=0x00（EQU:4/ACV:10）、VI_CONTROL=0x02（ENB/RST/NIN/DLR/LE/FMT）、
 *   VI_FB_LEFT_TOP=0x1C/0x1E、VI_FB_LEFT_BOTTOM=0x24/0x26、
 *   VI_VERTICAL_BEAM=0x2C、VI_PRERETRACE=0x30、VI_POSTRETRACE=0x34、
 *   VI_PICTURE_CONFIG=0x48（STD:8、WPL:7）、VI_CLOCK=0x6C、VI_FBWIDTH=0x70。
 *   UVIFBInfoRegister：FBB=bits[0:24)、XOFF=bits[24:28)、POFF=bit28、
 *   CLRPOFF=bits[29:32)。POFF=1 时 FBB 按 32 字节单位编码；bottom 使用 top 的 POFF。
 *   UVIInterruptRegister：IR_MASK=bit28、IR_INT=bit31。
 * 中断汇聚：IR_INT&IR_MASK → PI INT_CAUSE_VI（VideoInterface.cpp UpdateInterrupts）。
 */
#ifndef HAL_VI_H
#define HAL_VI_H

#include <stdint.h>

void hal_vi_init(void);
/* CPU-cycle raster progression. Deadlines are strictly positive; UINT32_MAX
 * means timing disabled/invalid. Fields derive from EQU/ACV/PRB/PSB/HLW/CLOCK;
 * NIN/FMT alone do not select a fixed refresh rate. DISP_INT compares latch
 * independently at their 1-based VCT/HCT positions, even when IR_MASK is clear. */
void hal_vi_advance(uint32_t cycles);
uint32_t hal_vi_cycles_to_event(void);
uint32_t hal_vi_cycles_to_field(void);

/* Host stepping: advance the shared CPU/device clock to the next field boundary.
 * No event is fabricated when VI timing is disabled. */
void hal_frame_tick(void);

/* XFB 取帧：按真实 VI 配置把 YUY2 转为 RGBA8。
 * 宽度来自 0x48 的 WPL*16，再扣除 top XOFF；行步长为 STD*32 字节。
 * NIN=1 读取 top 的 ACV 行；隔行模式按 top/bottom 指针交织为 2*ACV 行。
 * 所有源行和完整输出行先做 MEM1 边界检查；无效配置返回 -1 且清零输出尺寸。
 */
int hal_get_frame(uint8_t **rgba, int *w, int *h);

/* 观测口（测试/平台层） */
uint32_t hal_vi_xfb_top(void);    /* VITOP FBB 解码后的 MEM1 物理地址 */
uint16_t hal_vi_reg(uint32_t off);

#endif /* HAL_VI_H */
