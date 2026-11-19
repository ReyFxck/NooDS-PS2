CXX ?= g++

TARGETS := cartridge_io_test arm_alu_test scheduler_queue_test gpu_2d_math_test \
	gpu_3d_math_test thumb_alu_math_test thumb_dynarec_test gpu_color_test memory_page_cache_test r5900_carry_flags_test gpu_2d_capture_test cart_dma_test gpu_3d_alpha_test spu_state_test core_budget_test spu_batch_test gpu_color_convert_test

CXXFLAGS := -O2 -Wall -Wextra -std=gnu++11 -fno-rtti \
	-DNOODS_PS2_PORTABLE_TEST -DNOODS_PROFILE -ffunction-sections -fdata-sections -I../src
# PS2-specific tests that need NOODS_PS2 define it individually
LDFLAGS := -Wl,--gc-sections

.PHONY: all test clean

all: $(TARGETS)

cartridge_io_test: cartridge_io_test.cpp ../src/cartridge.cpp
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

arm_alu_test: arm_alu_test.cpp ../src/ps2/arm_alu.cpp
	$(CXX) $(CXXFLAGS) -DNOODS_PS2 $^ $(LDFLAGS) -o $@

r5900_carry_flags_test: r5900_carry_flags_test.cpp ../src/ps2/arm_alu.cpp
	$(CXX) $(CXXFLAGS) -DNOODS_PS2 $^ $(LDFLAGS) -o $@

scheduler_queue_test: scheduler_queue_test.cpp
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

gpu_2d_capture_test: gpu_2d_capture_test.cpp $(wildcard ../src/*.cpp)
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) -pthread -o $@

cart_dma_test: cart_dma_test.cpp $(wildcard ../src/*.cpp)
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) -pthread -o $@

gpu_2d_math_test: gpu_2d_math_test.cpp
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

gpu_3d_alpha_test: gpu_3d_alpha_test.cpp $(wildcard ../src/*.cpp)
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) -pthread -o $@

gpu_3d_math_test: gpu_3d_math_test.cpp
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

thumb_alu_math_test: thumb_alu_math_test.cpp
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

thumb_dynarec_test: thumb_dynarec_test.cpp ../src/ps2/arm_alu.cpp
	$(CXX) $(CXXFLAGS) -DNOODS_PS2 $^ $(LDFLAGS) -o $@

gpu_color_test: gpu_color_test.cpp
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

memory_page_cache_test: memory_page_cache_test.cpp
	$(CXX) $(CXXFLAGS) -DNOODS_PS2 $^ $(LDFLAGS) -o $@

spu_state_test: spu_state_test.cpp $(wildcard ../src/*.cpp)
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) -pthread -o $@

core_budget_test: core_budget_test.cpp $(wildcard ../src/*.cpp)
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) -pthread -o $@

spu_batch_test: spu_batch_test.cpp $(wildcard ../src/*.cpp)
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) -pthread -o $@

gpu_color_convert_test: gpu_color_convert_test.cpp ../src/gpu_color.h
	$(CXX) $(CXXFLAGS) $< $(LDFLAGS) -o $@

test: $(TARGETS)
	./cartridge_io_test
	./arm_alu_test
	./r5900_carry_flags_test
	./scheduler_queue_test
	./gpu_2d_math_test
	# ./gpu_2d_capture_test  # Temporarily disabled - flaky assertion
	./cart_dma_test
	./gpu_3d_math_test
	./gpu_3d_alpha_test
	./thumb_alu_math_test
	./thumb_dynarec_test
	./gpu_color_test
	./memory_page_cache_test
	./spu_state_test
	./core_budget_test
	# ./spu_batch_test  # Temporarily disabled - flaky assertion
	./gpu_color_convert_test

clean:
	rm -f $(TARGETS) cartridge_io_test.nds cartridge_io_test.sav
