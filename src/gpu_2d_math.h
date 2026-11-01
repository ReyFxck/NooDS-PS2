#ifndef GPU_2D_MATH_H
#define GPU_2D_MATH_H

#include <cstdint>

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

#endif
