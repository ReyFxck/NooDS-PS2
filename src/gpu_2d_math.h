#ifndef GPU_2D_MATH_H
#define GPU_2D_MATH_H

#include <cstdint>
#include <cstring>

// Backdrop clear is executed twice per visible scanline. Keep the stores
// strictly typed as uint32_t (no aliasing tricks), but unroll eight pixels per
// iteration to reduce loop/index overhead on the R5900.
inline void gpu2dFill32(uint32_t *destination, int count, uint32_t value)
{
    // EE/R5900 has native 64-bit stores. Production GPU buffers are explicitly
    // 8-byte aligned, so fill two identical 32-bit pixels per store. Keep a
    // 32-bit fallback for arbitrary callers/tests and use memcpy for the wide
    // store to avoid strict-aliasing violations.
    if ((reinterpret_cast<uintptr_t>(destination) & 7u) == 0)
    {
        const uint64_t pair = static_cast<uint64_t>(value) |
            (static_cast<uint64_t>(value) << 32);
        uint8_t *bytes = static_cast<uint8_t *>(
            __builtin_assume_aligned(destination, 8));

        while (count >= 16)
        {
            std::memcpy(bytes +  0, &pair, sizeof(pair));
            std::memcpy(bytes +  8, &pair, sizeof(pair));
            std::memcpy(bytes + 16, &pair, sizeof(pair));
            std::memcpy(bytes + 24, &pair, sizeof(pair));
            std::memcpy(bytes + 32, &pair, sizeof(pair));
            std::memcpy(bytes + 40, &pair, sizeof(pair));
            std::memcpy(bytes + 48, &pair, sizeof(pair));
            std::memcpy(bytes + 56, &pair, sizeof(pair));
            bytes += 64;
            count -= 16;
        }

        while (count >= 2)
        {
            std::memcpy(bytes, &pair, sizeof(pair));
            bytes += 8;
            count -= 2;
        }

        destination = reinterpret_cast<uint32_t *>(bytes);
    }
    else
    {
        while (count >= 8)
        {
            destination[0] = value;
            destination[1] = value;
            destination[2] = value;
            destination[3] = value;
            destination[4] = value;
            destination[5] = value;
            destination[6] = value;
            destination[7] = value;
            destination += 8;
            count -= 8;
        }
    }

    while (count-- > 0)
        *destination++ = value;
}

inline uint32_t gpu2dRgb5ToRgb6(uint32_t color)
{
    // Preserve renderer metadata in bits 18..31 while expanding each RGB5
    // channel into the even RGB6 values used by the original 2D engine.
    return (color & 0xFFFC0000u) |
        ((color & 0x001Fu) << 1) |
        ((color & 0x03E0u) << 2) |
        ((color & 0x7C00u) << 3);
}

inline uint64_t gpu2dRgb5PairToRgb6(uint64_t colors)
{
    // Same transform for two independent 32-bit lanes. Masks prevent shifts
    // from carrying bits across the lane boundary.
    return (colors & 0xFFFC0000FFFC0000ull) |
        ((colors & 0x0000001F0000001Full) << 1) |
        ((colors & 0x000003E0000003E0ull) << 2) |
        ((colors & 0x00007C0000007C00ull) << 3);
}

// Extended tiled backgrounds are always 128, 256, 512, or 1024 pixels wide.
// Unsigned masking is therefore the exact signed modulo used by wraparound,
// including for negative affine coordinates, without a division per axis.
inline int gpu2dWrapPowerOfTwo(int coordinate, int size)
{
    return static_cast<int>(static_cast<uint32_t>(coordinate) &
        static_cast<uint32_t>(size - 1));
}

inline uint32_t gpu2dExtendedTileOffset(int x, int y, int size)
{
    return (((static_cast<uint32_t>(y) >> 3) *
        (static_cast<uint32_t>(size) >> 3)) +
        (static_cast<uint32_t>(x) >> 3)) << 1;
}

inline uint32_t gpu2dExtendedPixelOffset(uint16_t tile, int x, int y)
{
    uint32_t localX = static_cast<uint32_t>(x) & 7u;
    uint32_t localY = static_cast<uint32_t>(y) & 7u;
    if (tile & 0x0400) localX ^= 7u;
    if (tile & 0x0800) localY ^= 7u;
    return (static_cast<uint32_t>(tile & 0x03FF) << 6) |
        (localY << 3) | localX;
}

// Affine BG coordinates advance linearly across a scanline. Keeping the
// fixed-point accumulators here avoids two multiplications for every pixel
// while preserving the original 24.8 fixed-point result exactly.
class Gpu2DAffineLineStepper
{
    public:
        Gpu2DAffineLineStepper(int32_t x, int32_t y, int16_t dx, int16_t dy):
            xAccumulator(x), yAccumulator(y), xStep(dx), yStep(dy) {}

        int x() const { return xAccumulator >> 8; }
        int y() const { return yAccumulator >> 8; }

        void advance()
        {
            xAccumulator += xStep;
            yAccumulator += yStep;
        }

    private:
        int32_t xAccumulator;
        int32_t yAccumulator;
        int16_t xStep;
        int16_t yStep;
};

// GPU scanline rendering is atomic with respect to CPU execution, so repeated
// reads from the same VRAM address within one draw call observe the same value.
// Extended affine backgrounds commonly sample one tile-map entry for several
// adjacent pixels; retaining the last entry avoids repeating the full memory
// map lookup without changing VRAM overlap semantics on cache misses.
template <typename T> class Gpu2DLastAddressCache
{
    public:
        bool lookup(uint32_t address, T &value) const
        {
            if (!valid || address != cachedAddress)
                return false;
            value = cachedValue;
            return true;
        }

        void store(uint32_t address, T value)
        {
            cachedAddress = address;
            cachedValue = value;
            valid = true;
        }

    private:
        uint32_t cachedAddress = 0;
        T cachedValue = T();
        bool valid = false;
};

// Direct VRAM mappings are stable for the duration of a GPU scanline task.
// Affine backgrounds frequently read hundreds of bytes from the same 4 KiB
// page, so retaining that page avoids repeating the sparse-table lookup for
// every pixel. Null/overlapped mappings are never cached and keep using the
// normal Memory fallback path.
class Gpu2DDirectPageCache
{
    public:
        bool lookup(uint32_t address, uint8_t *&page) const
        {
            if (!valid || (address & ~0xFFFu) != cachedAddress)
                return false;
            page = cachedPage;
            return true;
        }

        void store(uint32_t address, uint8_t *page)
        {
            if (!page)
                return;
            cachedAddress = address & ~0xFFFu;
            cachedPage = page;
            valid = true;
        }

    private:
        uint32_t cachedAddress = 0;
        uint8_t *cachedPage = nullptr;
        bool valid = false;
};

#endif
