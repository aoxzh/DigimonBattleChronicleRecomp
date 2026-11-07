/* hal/gx.h — GX FIFO：WGPIPE 字节流吸收 + 命令计数同步解析器（CP/PE）。
 * 出处：
 *   WGPIPE=0xCC008000、32 字节 gather pipe、PI FIFO 环形推进：
 *     Dolphin Source/Core/Core/HW/GPFifo.{h,cpp}
 *     https://raw.githubusercontent.com/dolphin-emu/dolphin/master/Source/Core/Core/HW/GPFifo.cpp
 *     （UpdateGatherPipe：每 32 字节拷到 PI FIFO 写指针，wp==end 则回 base）
 *   GX 命令长度表（同步解析不乱步）：
 *     Dolphin Source/Core/VideoCommon/OpcodeDecoding.h RunCommand()
 *     https://raw.githubusercontent.com/dolphin-emu/dolphin/master/Source/Core/VideoCommon/OpcodeDecoding.h
 *     NOP=0x00(1)、LOAD_CP=0x08(6)、LOAD_XF=0x10(5+4*n, n=((cmd2>>16)&0xF)+1)、
 *     LOAD_INDX_A..D=0x20/0x28/0x30/0x38(5)、CALL_DL=0x40(9，不进内容)、
 *     LOAD_BP=0x61(5)、图元 0x80-0xBF(3+n*vertex_size)
 *   CP 子命令与顶点尺寸位域：
 *     Source/Core/VideoCommon/CPMemory.{h,cpp}（MATINDEX_A=0x30 MATINDEX_B=0x40
 *     VCD_LO=0x50 VCD_HI=0x60 VAT_A/B/C=0x70/0x80/0x90 ARRAY_BASE=0xA0
 *     ARRAY_STRIDE=0xB0）
 *   PE：Source/Core/VideoCommon/PixelEngine.{h,cpp}
 *     （PE_CTRL=0x0A bit0 token_en bit1 finish_en；PE_TOKEN=0x0E；
 *      BP BPMEM_SETDRAWDONE=0x45 触发 finish、BPMEM_PE_TOKEN_ID=0x47 触发 token；
 *      EFB copy 目标 = BP BPMEM_EFB_ADDR=0x4B）
 */
#ifndef HAL_GX_H
#define HAL_GX_H

#include <stdint.h>
#include <stdbool.h>

void hal_gx_init(void);

/* M1 观测：EFB copy 目标（BPMEM_EFB_ADDR=0x4B 的 24 位地址）与已见 XF load */
uint32_t hal_gx_efb_copy_dst(void);     /* 0 = 未见 */
uint32_t hal_gx_efb_copy_count(void);
uint32_t hal_gx_xf_load_count(void);    /* LOAD_XF 次数 */
uint64_t hal_gx_bytes_consumed(void);   /* 解析器已消费字节数（自检用） */
uint64_t hal_gx_bytes_fed(void);        /* 已喂入字节数 */
uint32_t hal_gx_cmd_count(void);        /* 命令条数 */
void hal_gx_dump_state(void);
void hal_gx_signal_finish(void);
void hal_gx_signal_token(uint16_t token, int interrupt);

#endif /* HAL_GX_H */
