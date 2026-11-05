#include <cstdint>
#include <cstdio>

#include "../src/thumb_alu_math.h"

static uint32_t state = 0x13579BDFu;
static uint32_t nextRandom()
{
    state = state * 1664525u + 1013904223u;
    return state;
}

static bool checkOne(uint32_t left, uint32_t right, bool carryIn)
{
    const ThumbArithmeticResult add =
        thumbAddWithCarry(left, right, carryIn);
    const uint64_t addWide = static_cast<uint64_t>(left) +
        static_cast<uint64_t>(right) +
        static_cast<uint32_t>(carryIn);
    const uint32_t addResult = static_cast<uint32_t>(addWide);
    uint32_t addFlags = 0;
    if (addResult & 0x80000000u) addFlags |= 0x80000000u;
    if (!addResult) addFlags |= 0x40000000u;
    if (addWide > 0xFFFFFFFFull) addFlags |= 0x20000000u;
    const int64_t addSigned =
        static_cast<int64_t>(static_cast<int32_t>(left)) +
        static_cast<int64_t>(static_cast<int32_t>(right)) +
        static_cast<int64_t>(carryIn);
    if (addSigned > 2147483647ll || addSigned < -2147483648ll)
        addFlags |= 0x10000000u;
    if (add.result != addResult || add.nzcv != addFlags)
        return false;

    const ThumbArithmeticResult sub =
        thumbSubWithCarry(left, right, carryIn);
    const uint64_t borrow = carryIn ? 0ull : 1ull;
    const uint64_t rhs = static_cast<uint64_t>(right) + borrow;
    const uint32_t subResult =
        left - right - static_cast<uint32_t>(borrow);
    uint32_t subFlags = 0;
    if (subResult & 0x80000000u) subFlags |= 0x80000000u;
    if (!subResult) subFlags |= 0x40000000u;
    if (static_cast<uint64_t>(left) >= rhs)
        subFlags |= 0x20000000u;
    const int64_t subSigned =
        static_cast<int64_t>(static_cast<int32_t>(left)) -
        static_cast<int64_t>(static_cast<int32_t>(right)) -
        static_cast<int64_t>(borrow);
    if (subSigned > 2147483647ll || subSigned < -2147483648ll)
        subFlags |= 0x10000000u;
    return sub.result == subResult && sub.nzcv == subFlags;
}

int main()
{
    const uint32_t edge[] =
    {
        0u, 1u, 2u, 0x7FFFFFFFu, 0x80000000u,
        0xFFFFFFFEu, 0xFFFFFFFFu, 0x12345678u, 0x89ABCDEFu
    };

    for (uint32_t a : edge)
        for (uint32_t b : edge)
            for (int carry = 0; carry < 2; carry++)
                if (!checkOne(a, b, carry != 0))
                    return 1;

    for (int i = 0; i < 200000; i++)
        if (!checkOne(nextRandom(), nextRandom(),
            (nextRandom() & 1u) != 0))
            return 1;

    // Regression: CMP 0,1 must clear C so BCC/LO is taken.
    const ThumbArithmeticResult cmpBorrow = thumbSub(0u, 1u);
    if (cmpBorrow.nzcv & 0x20000000u)
        return 1;

    // CMP 1,0 must set C (no borrow).
    const ThumbArithmeticResult cmpNoBorrow = thumbSub(1u, 0u);
    if (!(cmpNoBorrow.nzcv & 0x20000000u))
        return 1;

    std::puts("thumb_alu_math_test: OK");
    return 0;
}
