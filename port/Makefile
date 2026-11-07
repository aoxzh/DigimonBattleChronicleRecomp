# HAL 桌面构建（macOS arm64 / Linux；clang，C11，-Wall -Wextra 零警告）
CC      ?= clang
PYTHON  ?= python3
REGEN_BUILD ?= 1
CFLAGS  := -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -g
LDFLAGS :=

HAL_SRC := \
	hal/mem.c \
	hal/mmio.c \
	hal/irq.c \
	hal/di.c \
	hal/vi.c \
	hal/gx.c \
	hal/si.c \
	hal/exi.c \
	hal/dsp.c \
	hal/ar.c \
	hal/ai.c \
	hal/hal_boot.c \
	third_party/gcn-recomp/cp_fifo.c

GLUE_SRC := recomp/runtime_glue.c
TEST_STATE_SRC := tests/hal_test_state.c
TEST_STATE_OBJ := $(TEST_STATE_SRC:.c=.o)

# 生成代码（提升器产出）
GEN_SRC  := $(wildcard recomp/generated/b_*.c) recomp/blocks.c recomp/recomp_entry.c
GEN_OBJ  := $(GEN_SRC:.c=.o)

BUILD   := build
HAL_OBJS := $(HAL_SRC:.c=.o) $(GLUE_SRC:.c=.o)

.PHONY: all test clean selftest headless regen test-host-audio test-host-audio-sanitize test-host-audio test-host-audio-sanitize

# 默认目标必须是第一条规则（GNU make 取首规则的第一个目标作默认目标；
# 曾因把头依赖行放在 all 之前导致 make 默认只查 .o、main_headless 永不重链）
all: $(BUILD)/hal_selftest $(BUILD)/main_headless

# 生成头依赖：gen_prefix.h/gc_abi.h 变化必须触发生成代码与 HAL 重编，
# 否则旧 .o 里编的是旧内联内存/写路由（曾致宽度写修复对生成代码不生效）。
GEN_HEADERS := recomp/gen_prefix.h hal/gc_abi.h
$(GEN_OBJ) $(HAL_OBJS) $(TEST_STATE_OBJ): $(GEN_HEADERS)

$(BUILD):
	mkdir -p $(BUILD)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/hal_selftest: $(HAL_OBJS) $(TEST_STATE_OBJ) tests/hal_selftest.c | $(BUILD)
	$(CC) $(CFLAGS) tests/hal_selftest.c $(HAL_OBJS) $(TEST_STATE_OBJ) -o $@ $(LDFLAGS)

$(BUILD)/main_headless: $(HAL_OBJS) $(GEN_OBJ) platform/desktop/main_headless.c | $(BUILD)
	$(CC) $(CFLAGS) platform/desktop/main_headless.c $(HAL_OBJS) $(GEN_OBJ) -o $@ $(LDFLAGS)

hal/gx.o: third_party/gcn-recomp/cp_fifo.h
third_party/gcn-recomp/cp_fifo.o: third_party/gcn-recomp/cp_fifo.h

$(BUILD)/cp_fifo_selftest: tests/cp_fifo_selftest.c third_party/gcn-recomp/cp_fifo.c third_party/gcn-recomp/cp_fifo.h | $(BUILD)
	$(CC) $(CFLAGS) tests/cp_fifo_selftest.c third_party/gcn-recomp/cp_fifo.c -o $@

$(BUILD)/gx_vertex_size_selftest: tests/gx_vertex_size_selftest.c $(HAL_OBJS) $(TEST_STATE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) tests/gx_vertex_size_selftest.c $(HAL_OBJS) $(TEST_STATE_OBJ) -o $@ $(LDFLAGS)

$(BUILD)/vi_capture_selftest: tests/test_vi_capture.c tests/test_device_clock_stubs.c hal/vi.c hal/vi.h | $(BUILD)
	$(CC) $(CFLAGS) tests/test_vi_capture.c tests/test_device_clock_stubs.c hal/vi.c -o $@ $(LDFLAGS)

$(BUILD)/vi_timing_selftest: tests/test_vi_timing.c hal/vi.c hal/vi.h hal/irq.c hal/irq.h | $(BUILD)
	$(CC) $(CFLAGS) -Ihal tests/test_vi_timing.c hal/vi.c hal/irq.c -o $@ $(LDFLAGS)

$(BUILD)/si_protocol_selftest: tests/test_si_protocol.c hal/si.c hal/si.h | $(BUILD)
	$(CC) $(CFLAGS) -Ihal tests/test_si_protocol.c hal/si.c -o $@ $(LDFLAGS)

hal/si.o: hal/si.h
hal/mmio.o: hal/mmio.h

$(BUILD)/lc_dma_selftest: tests/lc_dma_selftest.c tests/test_device_clock_stubs.c hal/mmio.c hal/mmio.h | $(BUILD)
	$(CC) $(CFLAGS) -Ihal hal/mmio.c tests/lc_dma_selftest.c tests/test_device_clock_stubs.c -o $@ $(LDFLAGS)

$(BUILD)/irq_clock_selftest: tests/test_irq_clock.c hal/irq.c hal/irq.h | $(BUILD)
	$(CC) $(CFLAGS) -Ihal tests/test_irq_clock.c hal/irq.c -o $@ $(LDFLAGS)

$(BUILD)/irq_devices_selftest: tests/test_irq_devices.c hal/irq.c hal/irq.h hal/ai.c hal/ai.h hal/dsp.c hal/dsp.h hal/exi.c hal/exi.h | $(BUILD)
	$(CC) $(CFLAGS) -DGC_UPSTREAM_AUDIO -Ihal tests/test_irq_devices.c hal/irq.c hal/ai.c hal/dsp.c hal/exi.c -o $@ $(LDFLAGS)

$(BUILD)/ai_clock_selftest: tests/test_ai_clock.c tests/test_device_clock_stubs.c hal/ai.c hal/ai.h | $(BUILD)
	$(CC) $(CFLAGS) -Ihal tests/test_ai_clock.c hal/ai.c tests/test_device_clock_stubs.c -o $@ $(LDFLAGS)

$(BUILD)/runtime_fp_context_selftest: tests/test_runtime_fp_context.c recomp/runtime_glue.c hal/gc_abi.h hal/irq.h | $(BUILD)
	$(CC) $(CFLAGS) tests/test_runtime_fp_context.c recomp/runtime_glue.c -o $@ $(LDFLAGS)

selftest: $(BUILD)/hal_selftest $(BUILD)/cp_fifo_selftest $(BUILD)/gx_vertex_size_selftest $(BUILD)/vi_capture_selftest $(BUILD)/si_protocol_selftest $(BUILD)/lc_dma_selftest $(BUILD)/runtime_fp_context_selftest $(BUILD)/ai_clock_selftest $(BUILD)/irq_clock_selftest $(BUILD)/vi_timing_selftest $(BUILD)/irq_devices_selftest
	./$(BUILD)/hal_selftest
	./$(BUILD)/cp_fifo_selftest
	./$(BUILD)/gx_vertex_size_selftest
	./$(BUILD)/vi_capture_selftest
	./$(BUILD)/si_protocol_selftest
	./$(BUILD)/lc_dma_selftest
	./$(BUILD)/runtime_fp_context_selftest
	./$(BUILD)/ai_clock_selftest
	./$(BUILD)/irq_clock_selftest
	./$(BUILD)/vi_timing_selftest
	./$(BUILD)/irq_devices_selftest

test: selftest
	PYTHONPATH=. $(PYTHON) tests/test_gekko.py
	$(PYTHON) tests/test_lift_arithmetic.py
	$(PYTHON) tests/test_lift_cr.py
	$(PYTHON) tests/test_lift_psq.py
	$(PYTHON) tests/test_lift_fp.py
	$(PYTHON) tests/test_lift_memory.py
	$(PYTHON) tests/test_lift_lc_dma.py
	$(PYTHON) tests/test_lift_clock.py
	$(PYTHON) tests/test_host_audio.py --cc "$(CC)"
	$(PYTHON) tests/test_runtime_dispatch.py --cc "$(CC)"
	$(PYTHON) tests/test_irq_poll.py --cc "$(CC)"
	$(PYTHON) tests/test_device_batch.py --cc "$(CC)"
	$(PYTHON) tests/test_upstream_audio.py --cc "$(CC)" --cxx "$(CXX)"

# Host sink regression: temporary SDL/clock doubles, no SDL or guest build.
test-host-audio:
	$(PYTHON) tests/test_host_audio.py --cc "$(CC)"

test-host-audio-sanitize:
	$(PYTHON) tests/test_host_audio.py --cc "$(CC)" --sanitize

headless: $(BUILD)/main_headless
	./$(BUILD)/main_headless extracted/main.dol disc.iso 3

# 单管线原子再生成（zeldaret/tp + decomp-toolkit 的核心纪律：所有生成产物
# 只由版本化输入一次生成，禁止手工 patch、禁止半代产物残留）。
# 版本化输入 = extracted/main.dol + tools/extra_roots.txt（r23 起累积的
# MISS 回填根，3442 条）+ tools/probe_blocks.txt（OOM 埋点）。
# 漏传 --extra-roots-file 会让覆盖缩水（165874→109407 块）且静默通过 unknown=0，
# 恢复旧 .c 回目录会造成新旧混代 → gc_entry MISSROOT。改 decode/lift 后只走本目标。
regen:
	rm -f recomp/generated/*.c recomp/generated/*.o recomp/*.o
	PYTHONPATH=. $(PYTHON) tools/lift_dol.py extracted/main.dol -o recomp \
	  --extra-roots-file tools/extra_roots.txt \
	  --hook-blocks tools/probe_blocks.txt
# Set REGEN_BUILD=0 when a platform CMake/Gradle build follows regeneration.
ifneq ($(REGEN_BUILD),0)
	$(MAKE) -j6 all
endif

clean:
	rm -rf $(BUILD) $(HAL_SRC:.c=.o) $(GLUE_SRC:.c=.o) $(TEST_STATE_OBJ) $(GEN_OBJ)
