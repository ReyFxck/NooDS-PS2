#include <cstdint>
#include <initializer_list>
#include <cstdio>

#include "../src/gpu_2d_math.h"

namespace
{
uint32_t randomState = 0x42F00D5Au;

uint32_t nextRandom()
{
    randomState ^= randomState << 13;
    randomState ^= randomState >> 17;
    randomState ^= randomState << 5;
    return randomState;
}

bool testWrapPowerOfTwo()
{
    const int sizes[] = { 128, 256, 512, 1024 };
    for (int size : sizes)
    {
        for (int value = -4097; value <= 4097; value++)
        {
            int expected = value % size;
            if (expected < 0)
                expected += size;
            if (gpu2dWrapPowerOfTwo(value, size) != expected)
                return false;
        }
    }
    return true;
}

bool testBlendMath()
{
    for (uint32_t channel = 0; channel < 64; channel++)
    {
        for (uint32_t rawFactor = 0; rawFactor < 32; rawFactor++)
        {
            uint32_t factor = rawFactor > 16 ? 16 : rawFactor;
            uint32_t expectedAlpha = channel * factor / 16;
            uint32_t expectedUp =
                channel + (63 - channel) * factor / 16;
            uint32_t expectedDown =
                channel - channel * factor / 16;

            if (gpu2dAlphaTerm(static_cast<uint8_t>(channel),
                    static_cast<uint8_t>(rawFactor)) != expectedAlpha ||
                gpu2dBrightIncrease(static_cast<uint8_t>(channel),
                    static_cast<uint8_t>(rawFactor)) != expectedUp ||
                gpu2dBrightDecrease(static_cast<uint8_t>(channel),
                    static_cast<uint8_t>(rawFactor)) != expectedDown)
                return false;
        }
    }
    return true;
}

bool testNativeLoads()
{
    uint8_t data[128];
    for (size_t i = 0; i < sizeof(data); i++)
        data[i] = static_cast<uint8_t>(nextRandom());

    for (size_t offset = 0; offset + 8 <= sizeof(data); offset++)
    {
        uint16_t expected16 = 0;
        uint32_t expected32 = 0;
        uint64_t expected64 = 0;
        for (size_t byte = 0; byte < 2; byte++)
            expected16 |= static_cast<uint16_t>(data[offset + byte]) <<
                (byte * 8);
        for (size_t byte = 0; byte < 4; byte++)
            expected32 |= static_cast<uint32_t>(data[offset + byte]) <<
                (byte * 8);
        for (size_t byte = 0; byte < 8; byte++)
            expected64 |= static_cast<uint64_t>(data[offset + byte]) <<
                (byte * 8);

        if (gpu2dLoad16(data, offset) != expected16 ||
            gpu2dLoad32(data, offset) != expected32 ||
            gpu2dLoad64(data, offset) != expected64)
            return false;
    }
    return true;
}

bool testFill32()
{
    alignas(8) uint32_t storage[264];
    for (int offset = 0; offset < 4; offset++)
    {
        for (int count = 0; count <= 257; count++)
        {
            for (int i = 0; i < 264; i++)
                storage[i] = 0xDEADBEEFu;

            uint32_t value = nextRandom();
            gpu2dFill32(storage + offset, count, value);

            for (int i = 0; i < 264; i++)
            {
                bool inside = i >= offset && i < offset + count;
                uint32_t expected = inside ? value : 0xDEADBEEFu;
                if (storage[i] != expected)
                    return false;
            }
        }
    }
    return true;
}

bool testRgb5ToRgb6()
{
    for (uint32_t color = 0; color < 0x8000u; color++)
    {
        for (uint32_t metadata : { 0u, 1u << 18, 1u << 24, 1u << 25,
                                   1u << 26, 0xFFFC0000u })
        {
            uint32_t source = color | metadata;
            uint32_t r = ((source >> 0) & 0x1Fu) * 2u;
            uint32_t g = ((source >> 5) & 0x1Fu) * 2u;
            uint32_t b = ((source >> 10) & 0x1Fu) * 2u;
            uint32_t expected = (source & 0xFFFC0000u) |
                (b << 12) | (g << 6) | r;
            if (gpu2dRgb5ToRgb6(source) != expected)
                return false;
        }
    }

    for (int i = 0; i < 200000; i++)
    {
        uint32_t lo = nextRandom();
        uint32_t hi = nextRandom();
        uint64_t pair = static_cast<uint64_t>(lo) |
            (static_cast<uint64_t>(hi) << 32);
        uint64_t expected = static_cast<uint64_t>(gpu2dRgb5ToRgb6(lo)) |
            (static_cast<uint64_t>(gpu2dRgb5ToRgb6(hi)) << 32);
        if (gpu2dRgb5PairToRgb6(pair) != expected)
            return false;
    }

    return true;
}

bool testAffineStepper()
{
    // DS affine reference points are signed 28-bit values. That range plus a
    // full scanline of signed 16-bit deltas cannot overflow int32_t.
    for (int test = 0; test < 20000; test++)
    {
        int32_t startX = static_cast<int32_t>(nextRandom() & 0x0FFFFFFFu);
        int32_t startY = static_cast<int32_t>(nextRandom() & 0x0FFFFFFFu);
        if (startX & 0x08000000) startX |= 0xF0000000;
        if (startY & 0x08000000) startY |= 0xF0000000;

        int16_t stepX = static_cast<int16_t>(nextRandom());
        int16_t stepY = static_cast<int16_t>(nextRandom());
        Gpu2DAffineLineStepper stepper(startX, startY, stepX, stepY);

        for (int pixel = 0; pixel < 256; pixel++)
        {
            int referenceX = (startX + stepX * pixel) >> 8;
            int referenceY = (startY + stepY * pixel) >> 8;
            if (stepper.x() != referenceX || stepper.y() != referenceY)
                return false;
            stepper.advance();
        }
    }
    return true;
}

bool testPowerOfTwoWrap()
{
    static const int sizes[] = { 128, 256, 512, 1024 };
    static const int edgeCoordinates[] = {
        INT32_MIN, -1048577, -1025, -1024, -1023, -513, -512, -511,
        -257, -256, -255, -129, -128, -127, -1, 0, 1, 127, 128,
        129, 255, 256, 257, 511, 512, 513, 1023, 1024, 1025,
        1048577, INT32_MAX
    };

    for (unsigned int s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++)
    {
        int size = sizes[s];
        for (unsigned int i = 0;
            i < sizeof(edgeCoordinates) / sizeof(edgeCoordinates[0]); i++)
        {
            int reference = edgeCoordinates[i] % size;
            if (reference < 0) reference += size;
            if (gpu2dWrapPowerOfTwo(edgeCoordinates[i], size) != reference)
                return false;
        }

        for (int i = 0; i < 250000; i++)
        {
            int coordinate = static_cast<int32_t>(nextRandom());
            int reference = coordinate % size;
            if (reference < 0) reference += size;
            if (gpu2dWrapPowerOfTwo(coordinate, size) != reference)
                return false;
        }
    }
    return true;
}

bool testExtendedTiledOffsets()
{
    static const int sizes[] = { 128, 256, 512, 1024 };
    for (unsigned int s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++)
    {
        int size = sizes[s];
        for (int y = 0; y < size; y++)
        {
            for (int x = 0; x < size; x++)
            {
                uint32_t reference = ((y / 8) * (size / 8) + (x / 8)) * 2;
                if (gpu2dExtendedTileOffset(x, y, size) != reference)
                    return false;

                uint16_t tile = static_cast<uint16_t>(nextRandom());
                uint32_t pixelReference = (tile & 0x03FF) * 64;
                pixelReference += ((tile & 0x0800) ? (7 - y % 8) :
                    (y % 8)) * 8;
                pixelReference += (tile & 0x0400) ? (7 - x % 8) :
                    (x % 8);
                if (gpu2dExtendedPixelOffset(tile, x, y) != pixelReference)
                    return false;
            }
        }
    }
    return true;
}

bool testLastAddressCache()
{
    uint16_t memory[64];
    for (uint32_t i = 0; i < 64; i++)
        memory[i] = static_cast<uint16_t>(nextRandom());

    Gpu2DLastAddressCache<uint16_t> cache;
    uint32_t physicalReads = 0;

    // Model a 1:1 affine scanline: eight adjacent pixels share each tile-map
    // entry. The cached stream must be bit-identical and perform one lookup per
    // tile instead of one per pixel.
    for (uint32_t pixel = 0; pixel < 256; pixel++)
    {
        uint32_t address = pixel >> 3;
        uint16_t value;
        if (!cache.lookup(address, value))
        {
            value = memory[address];
            cache.store(address, value);
            physicalReads++;
        }
        if (value != memory[address])
            return false;
    }
    if (physicalReads != 32)
        return false;

    // The valid bit must keep every possible 32-bit address cacheable.
    cache.store(UINT32_MAX, 0xA55Au);
    uint16_t value = 0;
    return cache.lookup(UINT32_MAX, value) && value == 0xA55Au;
}

bool testDirectPageCache()
{
    uint8_t firstPage[0x1000] = {};
    uint8_t secondPage[0x1000] = {};
    Gpu2DDirectPageCache cache;
    uint8_t *page = nullptr;

    if (cache.lookup(0x06001234u, page))
        return false;
    cache.store(0x06001234u, firstPage);
    if (!cache.lookup(0x06001FFFu, page) || page != firstPage)
        return false;
    if (cache.lookup(0x06002000u, page))
        return false;

    // Null mappings represent overlapping/unmapped VRAM and must not replace
    // the last valid direct page or become cache hits themselves.
    cache.store(0x06002000u, nullptr);
    if (cache.lookup(0x06002001u, page))
        return false;
    if (!cache.lookup(0x06001000u, page) || page != firstPage)
        return false;

    cache.store(0x06002000u, secondPage);
    if (!cache.lookup(0x06002ABCu, page) || page != secondPage ||
        cache.lookup(0x06001000u, page))
        return false;

    // A linear 256-pixel stream should consult its page table once.
    Gpu2DDirectPageCache streamCache;
    uint32_t pageLookups = 0;
    for (uint32_t pixel = 0; pixel < 256; pixel++)
    {
        uint32_t address = 0x06003000u + pixel;
        if (!streamCache.lookup(address, page))
        {
            page = firstPage;
            streamCache.store(address, page);
            pageLookups++;
        }
        if (page != firstPage)
            return false;
    }
    return pageLookups == 1;
}
}

int main()
{
    if (!testWrapPowerOfTwo())
    {
        std::fprintf(stderr, "power-of-two wrap mismatch\n");
        return 1;
    }
    if (!testBlendMath())
    {
        std::fprintf(stderr, "GPU2D blend math mismatch\n");
        return 1;
    }
    if (!testNativeLoads())
    {
        std::fprintf(stderr, "native little-endian load mismatch\n");
        return 1;
    }
    if (!testFill32())
    {
        std::fprintf(stderr, "fill32 mismatch\n");
        return 1;
    }
    if (!testRgb5ToRgb6())
    {
        std::fprintf(stderr, "rgb5ToRgb6 mismatch\n");
        return 1;
    }
    if (!testAffineStepper())
    {
        std::fprintf(stderr, "affine line stepper mismatch\n");
        return 1;
    }
    if (!testPowerOfTwoWrap())
    {
        std::fprintf(stderr, "power-of-two affine wrap mismatch\n");
        return 1;
    }
    if (!testExtendedTiledOffsets())
    {
        std::fprintf(stderr, "extended tiled address mismatch\n");
        return 1;
    }
    if (!testLastAddressCache())
    {
        std::fprintf(stderr, "last-address tile cache mismatch\n");
        return 1;
    }
    if (!testDirectPageCache())
    {
        std::fprintf(stderr, "direct-page cache mismatch\n");
        return 1;
    }
    return 0;
}
