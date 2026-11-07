/* hal/di.h — DVD 接口状态机（读 disc.iso 直通）。
 * 寄存器偏移/位域/命令流程出处：
 *   Dolphin Source/Core/Core/HW/DVD/DVDInterface.{h,cpp}
 *   https://raw.githubusercontent.com/dolphin-emu/dolphin/master/Source/Core/Core/HW/DVD/DVDInterface.cpp
 *   DI_STATUS=0x00（BREAK0 DEINTMASK1 DEINT2 TCINTMASK3 TCINT4 BRKINTMASK5 BRKINT6）
 *   DI_COVER=0x04、DI_CMD0/1/2=0x08/0x0C/0x10、DI_MAR=0x14、DI_LEN=0x18、
 *   DI_CR=0x1C（TSTART0 DMA1 RW2）、DI_IMMBUF=0x20、DI_CFG=0x24。
 *   命令 0xA8（Read）：sub=CMDBUF0&0xFF（0x00 扇区读：偏移=CMDBUF1<<2，
 *   长度=CMDBUF2；0x40 读 DiscID）；完成：TSTART=0、TCINT=1、MAR+=LEN、LEN=0、
 *   PI INT_CAUSE_DI（UpdateInterrupts：TCINT&TCINTMASK）。
 */
#ifndef HAL_DI_H
#define HAL_DI_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

bool hal_di_open_iso(const char *path);   /* 失败返回 false（无盘仍可跑） */
void hal_di_close_iso(void);
void hal_di_init(void);

/* 观测（测试/日志） */
uint64_t hal_di_read_count(void);
uint64_t hal_di_read_bytes(void);

#endif /* HAL_DI_H */
