/*
    Copyright 2026 NooDS-PS2 contributors

    This file is part of NooDS-PS2 and is distributed under GPLv3 or later.
*/

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "cartridge.h"

class TestCartridge: public Cartridge
{
    public:
        TestCartridge(): Cartridge(nullptr) {}

        void configure(uint32_t bytes) { configureRomCache(bytes); }
        void read(uint32_t offset, uint32_t size) { loadRomSection(offset, size); }
        const uint8_t *data() const { return rom; }
};

int main()
{
    static const char *path = "cartridge_io_test.nds";
    static const uint32_t pageSize = 0x4000;

    std::vector<uint8_t> expected(pageSize * 4);
    for (size_t i = 0; i < expected.size(); i++)
        expected[i] = static_cast<uint8_t>((i * 37u + (i >> 8)) & 0xFFu);

    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(expected.data(), 1, expected.size(), file) == expected.size());
    fclose(file);

    {
        TestCartridge cartridge;
        cartridge.loadRom(path);
        cartridge.configure(pageSize * 2);
        cartridge.resetRomIoProfile();

        cartridge.read(0, 64);
        assert(cartridge.getRomCacheMisses() == 1);
        assert(cartridge.getRomIoProfile().readCalls == 1);
        assert(cartridge.getRomIoProfile().seekCalls == 0);
        for (uint32_t i = 0; i < 64; i++)
            assert(cartridge.data()[i] == expected[i]);

        cartridge.read(pageSize, 64);
        assert(cartridge.getRomCacheMisses() == 2);
        assert(cartridge.getRomCacheSequentialMisses() == 1);
        assert(cartridge.getRomIoProfile().readCalls == 2);
        assert(cartridge.getRomIoProfile().seekCalls == 0);
        for (uint32_t i = 0; i < 64; i++)
            assert(cartridge.data()[i] == expected[pageSize + i]);

        // Re-reading page zero must be a cache hit with no physical I/O.
        cartridge.read(0, 64);
        assert(cartridge.getRomCacheHits() == 1);
        assert(cartridge.getRomIoProfile().readCalls == 2);

        // Jumping to page three must seek once and read one complete page.
        cartridge.read(pageSize * 3, 64);
        const RomIoProfile &profile = cartridge.getRomIoProfile();
        assert(cartridge.getRomCacheMisses() == 3);
        assert(profile.readCalls == 3);
        assert(profile.seekCalls == 1);
        assert(profile.requestedBytes == pageSize * 3);
        assert(profile.readBytes == pageSize * 3);
        assert(profile.shortReads == 0);
        assert(profile.callerReads[ROM_IO_CACHE_MISS] == 3);
        assert(profile.sizeHistogram[1] == 3); // Three physical 16 KiB pages.
        uint64_t latencySamples = 0;
        for (int i = 0; i < 7; i++)
            latencySamples += profile.latencyHistogram[i];
        assert(latencySamples == 3);
    }

    std::remove(path);
    std::remove("cartridge_io_test.sav");
    return 0;
}
