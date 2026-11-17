// The frame conversion that feeds the GS (RGB6 -> CT16) now handles two pixels
// per 64-bit load. It must match the scalar conversion bit for bit, for every
// 18-bit colour, with arbitrary garbage in the unused upper bits, and for any
// buffer alignment.
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../src/gpu_color.h"

namespace
{
    uint64_t rngState = 0xA24BAED4963EE407ull;
    uint32_t rnd()
    {
        rngState ^= rngState << 13;
        rngState ^= rngState >> 7;
        rngState ^= rngState << 17;
        return static_cast<uint32_t>(rngState >> 16);
    }
}

int main()
{
    // Every colour in either lane, with random neighbours and upper bits
    for (uint32_t color = 0; color < (1u << 18); color++)
    {
        const uint32_t junkHigh = (color & 1) ? rnd() : (rnd() & 0xFFFC0000u);
        const uint32_t withJunk = color | (rnd() & 0xFFFC0000u);

        const uint64_t pairLow = static_cast<uint64_t>(withJunk) | (static_cast<uint64_t>(junkHigh) << 32);
        const uint32_t r1 = gpuRgb6x2ToGsCt16(pairLow);
        assert((r1 & 0xFFFFu) == gpuRgb6ToGsCt16(withJunk));
        assert((r1 >> 16) == gpuRgb6ToGsCt16(junkHigh));

        const uint64_t pairHigh = static_cast<uint64_t>(junkHigh) | (static_cast<uint64_t>(withJunk) << 32);
        const uint32_t r2 = gpuRgb6x2ToGsCt16(pairHigh);
        assert((r2 & 0xFFFFu) == gpuRgb6ToGsCt16(junkHigh));
        assert((r2 >> 16) == gpuRgb6ToGsCt16(withJunk));
    }

    // Whole frames, aligned and misaligned buffers
    const uint32_t kPixels = 256 * 192 * 2;
    std::vector<uint64_t> srcStore(kPixels / 2 + 2);   // 8-byte aligned backing
    std::vector<uint64_t> dstStore(kPixels / 4 + 2);
    std::vector<uint16_t> expected(kPixels);

    for (int srcOffset = 0; srcOffset < 2; srcOffset++)       // 0: 8-aligned, 1: only 4-aligned
        for (int dstOffset = 0; dstOffset < 4; dstOffset++)   // 0: 8-aligned, others: not
        {
            uint32_t *src = reinterpret_cast<uint32_t*>(srcStore.data()) + srcOffset;
            uint16_t *dst = reinterpret_cast<uint16_t*>(dstStore.data()) + dstOffset;
            for (uint32_t i = 0; i < kPixels; i++)
                src[i] = rnd();
            for (uint32_t i = 0; i < kPixels; i++)
                expected[i] = gpuRgb6ToGsCt16(src[i]);

            std::memset(dstStore.data(), 0xAB, dstStore.size() * sizeof(uint64_t));
            gpuConvertRgb6ToGsCt16(src, dst, kPixels);
            assert(!std::memcmp(dst, expected.data(), kPixels * sizeof(uint16_t)));

            // Nothing outside the destination range was touched
            const uint8_t *raw = reinterpret_cast<const uint8_t*>(dstStore.data());
            for (size_t i = 0; i < static_cast<size_t>(dstOffset) * 2; i++)
                assert(raw[i] == 0xAB);
            for (size_t i = static_cast<size_t>(dstOffset + kPixels) * 2; i < dstStore.size() * sizeof(uint64_t); i++)
                assert(raw[i] == 0xAB);
        }

    // Informational timing (x86, aligned): scalar loop vs the 64-bit path
    uint32_t *src = reinterpret_cast<uint32_t*>(srcStore.data());
    uint16_t *dst = reinterpret_cast<uint16_t*>(dstStore.data());
    volatile uint16_t sink = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int rep = 0; rep < 300; rep++)
    {
        for (uint32_t i = 0; i < kPixels; i += 4)
        {
            dst[i + 0] = gpuRgb6ToGsCt16(src[i + 0]);
            dst[i + 1] = gpuRgb6ToGsCt16(src[i + 1]);
            dst[i + 2] = gpuRgb6ToGsCt16(src[i + 2]);
            dst[i + 3] = gpuRgb6ToGsCt16(src[i + 3]);
        }
        sink = dst[rep];
    }
    auto t1 = std::chrono::steady_clock::now();
    for (int rep = 0; rep < 300; rep++)
    {
        gpuConvertRgb6ToGsCt16(src, dst, kPixels);
        sink = dst[rep];
    }
    auto t2 = std::chrono::steady_clock::now();
    (void)sink;
    printf("gpu_color_convert_test: OK (all 2^18 colours in both lanes, 8 alignments); host per frame: scalar %.0f us, 64-bit %.0f us (informational)\n",
        std::chrono::duration<double, std::micro>(t1 - t0).count() / 300,
        std::chrono::duration<double, std::micro>(t2 - t1).count() / 300);
    return 0;
}
