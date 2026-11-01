#include <cstdint>
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
}

int main()
{
    if (!testAffineStepper())
    {
        std::fprintf(stderr, "affine line stepper mismatch\n");
        return 1;
    }
    return 0;
}
