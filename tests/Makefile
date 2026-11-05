CXX ?= g++

TARGETS := cartridge_io_test arm_alu_test scheduler_queue_test gpu_2d_math_test \
	gpu_3d_math_test thumb_alu_math_test thumb_dynarec_test gpu_color_test memory_page_cache_test

CXXFLAGS := -O2 -Wall -Wextra -std=gnu++11 -fno-rtti \
	-DNOODS_PS2 -DNOODS_PS2_PORTABLE_TEST -DNOODS_PROFILE -ffunction-sections -fdata-sections -I../src
LDFLAGS := -Wl,--gc-sections

.PHONY: all test clean

all: $(TARGETS)

cartridge_io_test: cartridge_io_test.cpp ../src/cartridge.cpp
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

arm_alu_test: arm_alu_test.cpp ../src/ps2/arm_alu.cpp
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

scheduler_queue_test: scheduler_queue_test.cpp
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

gpu_2d_math_test: gpu_2d_math_test.cpp
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

gpu_3d_math_test: gpu_3d_math_test.cpp
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

thumb_alu_math_test: thumb_alu_math_test.cpp
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

thumb_dynarec_test: thumb_dynarec_test.cpp ../src/ps2/arm_alu.cpp
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

gpu_color_test: gpu_color_test.cpp
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

memory_page_cache_test: memory_page_cache_test.cpp
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

test: $(TARGETS)
	./cartridge_io_test
	./arm_alu_test
	./scheduler_queue_test
	./gpu_2d_math_test
	./gpu_3d_math_test
	./thumb_alu_math_test
	./thumb_dynarec_test
	./memory_page_cache_test

clean:
	rm -f $(TARGETS) cartridge_io_test.nds cartridge_io_test.sav
