#ifndef GPU_2D_MATH_H
#define GPU_2D_MATH_H

#include <cstdint>

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
