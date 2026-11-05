#include <cstdint>
#include <cstdio>

#include "../src/gpu_3d_math.h"

static uint32_t referenceLinear(uint32_t v1, uint32_t v2,
    uint32_t x1, uint32_t x, uint32_t x2)
{
    if (x <= x1) return v1;
    if (x >= x2) return v2;
    if (v1 <= v2)
        return v1 + (v2 - v1) * (x - x1) / (x2 - x1);
    return v2 + (v1 - v2) * (x2 - x) / (x2 - x1);
}

int main()
{
    // Renderer domains: interpolated color <= 504, shifted S/T values
    // stay around 17 bits, and Z is 24-bit. For span <= 256 these preserve
    // the original uint32_t multiplication without overflow.
    const uint32_t values[] =
    {
        0, 1, 2, 7, 31, 63, 255, 504, 511, 4095, 65535,
        0x0001FFFEu, 0x00FFFFFFu
    };

    for (uint32_t span = 1; span <= 256; span++)
    {
        for (uint32_t a : values)
        {
            for (uint32_t b : values)
            {
                Gpu3DLinearStepper stepper(a, b, span);
                for (uint32_t x = 0; x <= span; x++)
                {
                    uint32_t expected =
                        referenceLinear(a, b, 0, x, span);
                    if (stepper.current() != expected)
                    {
                        std::fprintf(stderr,
                            "linear step mismatch span=%u a=%u b=%u x=%u got=%u expected=%u\n",
                            span, a, b, x, stepper.current(), expected);
                        return 1;
                    }
                    if (x != span)
                        stepper.advance();
                }
            }
        }
    }

    std::puts("gpu_3d_math_test: OK");
    return 0;
}
