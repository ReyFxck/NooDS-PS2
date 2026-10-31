CXX ?= g++

TARGET := cartridge_io_test

CXXFLAGS := -O2 -Wall -Wextra -std=gnu++11 -fno-rtti \
	-DNOODS_PS2 -DNOODS_PROFILE -ffunction-sections -fdata-sections -I../src
LDFLAGS := -Wl,--gc-sections

.PHONY: all test clean

all: $(TARGET)

$(TARGET): cartridge_io_test.cpp ../src/cartridge.cpp
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

test: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) cartridge_io_test.nds cartridge_io_test.sav
