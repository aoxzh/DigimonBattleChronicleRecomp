#pragma once
// Minimal source compatibility only: no upstream CPU, scheduler, FIFO or MMIO.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define GCN_BSWAP16(v) __builtin_bswap16(v)
#define GCN_BSWAP32(v) __builtin_bswap32(v)
#else
#define GCN_BSWAP16(v) (v)
#define GCN_BSWAP32(v) (v)
#endif
constexpr uint32_t RAM_SIZE = 0x01800000;
extern uint8_t *g_mem;
uint8_t *upstream_mem_ptr(uint32_t address);
inline uint8_t *phys_ptr(uint32_t address) { return upstream_mem_ptr(address); }
inline uint8_t *mem_ptr(uint32_t address) { return upstream_mem_ptr(address); }
// Scope this compatibility spelling away from the independent audio shim.
#define mem_r32 upstream_gx_mem_r32
inline uint32_t upstream_gx_mem_r32(uint32_t address) {
    const uint8_t *p = mem_ptr(address);
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
[[noreturn]] void fatal(const char *format, ...);
#define LOG_GX 0
#define LOG(category, ...) do { (void)(category); std::fprintf(stderr, "[gx] "); std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); } while (0)
uint32_t vi_display_height();
void debug_guest_check(const char *where);
void debug_guard_dma(const char *what, uint32_t address, uint32_t length);
void heap_trace_frame(uint32_t frame);
