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

    std::vector<uint8_t> expected(pageSize * 20);
    for (size_t i = 0; i < expected.size(); i++)
        expected[i] = static_cast<uint8_t>((i * 37u + (i >> 8)) & 0xFFu);

    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(expected.data(), 1, expected.size(), file) == expected.size());
    fclose(file);

    {
        TestCartridge cartridge;
        cartridge.loadRom(path);
        cartridge.configure(pageSize * 3);
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
        assert(cartridge.getRomCacheReadAheadReads() == 1);
        assert(cartridge.getRomCacheReadAheadPages() == 1);
        for (uint32_t i = 0; i < 64; i++)
            assert(cartridge.data()[i] == expected[pageSize + i]);

        // The confirmed sequential access coalesces pages one and two into a
        // single physical read. Page two must now hit with identical bytes.
        cartridge.read(pageSize * 2, pageSize);
        assert(cartridge.getRomCacheHits() == 1);
        assert(cartridge.getRomIoProfile().readCalls == 2);
        for (uint32_t i = 0; i < pageSize; i++)
            assert(cartridge.data()[i] == expected[pageSize * 2 + i]);

        // Re-reading page zero must also be a cache hit with no physical I/O.
        cartridge.read(0, 64);
        assert(cartridge.getRomCacheHits() == 2);
        assert(cartridge.getRomIoProfile().readCalls == 2);

        // Page three is no longer adjacent to the last demand access, so it is
        // read alone. The coalesced read already left the file at its offset.
        cartridge.read(pageSize * 3, 64);
        const RomIoProfile &profile = cartridge.getRomIoProfile();
        assert(cartridge.getRomCacheMisses() == 3);
        assert(profile.readCalls == 3);
        assert(profile.seekCalls == 0);
        assert(profile.requestedBytes == pageSize * 4);
        assert(profile.readBytes == pageSize * 4);
        assert(profile.shortReads == 0);
        assert(profile.callerReads[ROM_IO_CACHE_MISS] == 3);
        assert(profile.sizeHistogram[1] == 2); // Two physical 16 KiB reads.
        assert(profile.sizeHistogram[2] == 1); // One coalesced 32 KiB read.
        uint64_t latencySamples = 0;
        for (int i = 0; i < 7; i++)
            latencySamples += profile.latencyHistogram[i];
        assert(latencySamples == 3);
    }

    {
        TestCartridge cartridge;
        cartridge.loadRom(path);
        cartridge.configure(pageSize * 20);
        cartridge.resetRomIoProfile();

        // Model the measured PS2 burst (the real log reached 17 adjacent
        // misses). Confirmed two-page reads reduce 18 demand accesses to ten
        // physical calls, with at most one speculative page at the tail.
        for (uint32_t page = 0; page < 18; page++)
        {
            cartridge.read(page * pageSize, 64);
            for (uint32_t i = 0; i < 64; i++)
                assert(cartridge.data()[i] == expected[page * pageSize + i]);
        }
        assert(cartridge.getRomCacheMisses() == 10);
        assert(cartridge.getRomCacheHits() == 8);
        assert(cartridge.getRomCacheReadAheadReads() == 9);
        assert(cartridge.getRomCacheReadAheadPages() == 9);
        assert(cartridge.getRomIoProfile().readCalls == 10);
        assert(cartridge.getRomIoProfile().requestedBytes == pageSize * 19);
        assert(cartridge.getRomIoProfile().readBytes == pageSize * 19);
    }

    {
        TestCartridge cartridge;
        cartridge.loadRom(path);
        cartridge.configure(pageSize * 3);

        // A caller spanning two pages must still receive an uninterrupted,
        // byte-exact result while the second physical read also stages page 2.
        cartridge.read(pageSize - 32, 64);
        for (uint32_t i = 0; i < 64; i++)
            assert(cartridge.data()[i] == expected[pageSize - 32 + i]);
        assert(cartridge.getRomCacheReadAheadReads() == 1);
    }

    std::remove(path);
    std::remove("cartridge_io_test.sav");
    return 0;
}
