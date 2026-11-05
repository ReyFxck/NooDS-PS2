#ifndef GPU_3D_MATH_H
#define GPU_3D_MATH_H

#include <cstdint>

class Gpu3DLinearStepper
{
public:
    Gpu3DLinearStepper(uint32_t start, uint32_t end, uint32_t span):
        value(start), span(span), increasing(start <= end)
    {
        if (!span)
            return;

        const uint32_t delta =
            increasing ? (end - start) : (start - end);
        whole = delta / span;
        remainder = delta % span;
        error = increasing ? 0 : (span - 1);
    }

    uint32_t current() const { return value; }

    void advance()
    {
        if (!span)
            return;

        if (increasing)
            value += whole;
        else
            value -= whole;

        error += remainder;
        if (error >= span)
        {
            error -= span;
            if (increasing)
                value++;
            else
                value--;
        }
    }

    void advanceBy(uint32_t count)
    {
        while (count--)
            advance();
    }

private:
    uint32_t value = 0;
    uint32_t span = 0;
    uint32_t whole = 0;
    uint32_t remainder = 0;
    uint32_t error = 0;
    bool increasing = true;
};

#endif
