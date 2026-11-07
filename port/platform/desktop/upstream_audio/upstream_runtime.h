#pragma once
#include "upstream_audio.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
/* Narrow compatibility surface for the pinned mixer. Implemented by adapter.cpp. */
uint16_t audio_mem_r16(uint32_t);
uint32_t audio_mem_r32(uint32_t);
void audio_mem_w16(uint32_t, uint16_t);
void audio_mem_w32(uint32_t, uint32_t);
uint8_t audio_aram_byte(uint32_t);
void audio_limit(uint32_t);
void audio_mixed_block();
[[noreturn]] void audio_fail(const char *);
namespace digimon_audio_memory {
inline uint16_t mem_r16(uint32_t a) { return audio_mem_r16(a); }
inline uint32_t mem_r32(uint32_t a) { return audio_mem_r32(a); }
inline void mem_w16(uint32_t a, uint16_t v) { audio_mem_w16(a, v); }
inline void mem_w32(uint32_t a, uint32_t v) { audio_mem_w32(a, v); }
}
using digimon_audio_memory::mem_r16;
using digimon_audio_memory::mem_r32;
using digimon_audio_memory::mem_w16;
using digimon_audio_memory::mem_w32;
#define LOG_DSP 0
#define LOG(category, ...) do { (void)(category); std::fprintf(stderr, "[upstream-audio] "); std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); } while (0)
