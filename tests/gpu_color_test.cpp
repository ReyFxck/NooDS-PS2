#include <cstdint>
#include <cstdio>

#include "../src/gpu_color.h"

static uint32_t referenceRgb6(uint32_t color)
{
    uint32_t r = ((color >> 0) & 0x3Fu) * 255u / 63u;
    uint32_t g = ((color >> 6) & 0x3Fu) * 255u / 63u;
    uint32_t b = ((color >> 12) & 0x3Fu) * 255u / 63u;
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

static uint32_t referenceRgb5(uint32_t color)
{
    uint32_t r6 = ((color >> 0) & 0x1Fu) << 1;
    uint32_t g6 = ((color >> 5) & 0x1Fu) << 1;
    uint32_t b6 = ((color >> 10) & 0x1Fu) << 1;
    uint32_t r = r6 * 255u / 63u;
    uint32_t g = g6 * 255u / 63u;
    uint32_t b = b6 * 255u / 63u;
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

int main()
{
    // Exhaust every possible 18-bit RGB6 pixel.
    for (uint32_t color = 0; color < (1u << 18); color++)
    {
        uint32_t expected = referenceRgb6(color);
        uint32_t actual = gpuRgb6ToRgb8(color);
        if (actual != expected)
        {
            std::fprintf(stderr,
                "RGB6 mismatch color=%05X expected=%08X actual=%08X\n",
                color, expected, actual);
            return 1;
        }

        // GS CT16 stores the high five bits of each NDS channel, with
        // opaque alpha; renderer metadata must not leak into the texture.
        uint16_t ct16 = static_cast<uint16_t>(0x8000u |
            (((color >> 1) & 0x1Fu) << 0) |
            (((color >> 7) & 0x1Fu) << 5) |
            (((color >> 13) & 0x1Fu) << 10));
        if (gpuRgb6ToGsCt16(color | 0xFFFC0000u) != ct16)
        {
            std::fprintf(stderr, "RGB6 -> GS CT16 mismatch color=%05X\n", color);
            return 1;
        }
    }

    // Exhaust every possible RGB5 pixel used by the GBA path.
    for (uint32_t color = 0; color < (1u << 15); color++)
    {
        uint32_t expected = referenceRgb5(color);
        uint32_t actual = gpuRgb5ToRgb8(color);
        if (actual != expected)
        {
            std::fprintf(stderr,
                "RGB5 mismatch color=%04X expected=%08X actual=%08X\n",
                color, expected, actual);
            return 1;
        }
        if (gpuRgb5ToGsCt16(color | 0xFFFF8000u) !=
            (color | 0x8000u))
        {
            std::fprintf(stderr, "RGB5 -> GS CT16 mismatch color=%04X\n", color);
            return 1;
        }
    }

    return 0;
}
