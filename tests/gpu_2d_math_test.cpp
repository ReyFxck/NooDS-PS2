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
}

int main()
{
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
    return 0;
}
