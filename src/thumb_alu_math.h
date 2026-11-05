#ifndef THUMB_ALU_MATH_H
#define THUMB_ALU_MATH_H

#include <cstdint>

struct ThumbArithmeticResult
{
    uint32_t result = 0;
    uint32_t nzcv = 0;
};

inline ThumbArithmeticResult thumbAddWithCarry(uint32_t left,
    uint32_t right, bool carryIn)
{
    ThumbArithmeticResult out;
    const uint64_t wide = static_cast<uint64_t>(left) +
        static_cast<uint64_t>(right) +
        static_cast<uint32_t>(carryIn);
    out.result = static_cast<uint32_t>(wide);

    if (out.result & 0x80000000u) out.nzcv |= 0x80000000u;
    if (!out.result)              out.nzcv |= 0x40000000u;
    if (wide > 0xFFFFFFFFull)     out.nzcv |= 0x20000000u;

    const int64_t signedWide =
        static_cast<int64_t>(static_cast<int32_t>(left)) +
        static_cast<int64_t>(static_cast<int32_t>(right)) +
        static_cast<int64_t>(carryIn);
    if (signedWide > 2147483647ll || signedWide < -2147483648ll)
        out.nzcv |= 0x10000000u;
    return out;
}

inline ThumbArithmeticResult thumbSubWithCarry(uint32_t left,
    uint32_t right, bool carryIn)
{
    // ARM subtraction interprets C as NOT-borrow. SBC computes:
    // left - right - (1-C).
    ThumbArithmeticResult out;
    const uint64_t borrow = carryIn ? 0ull : 1ull;
    const uint64_t rhs = static_cast<uint64_t>(right) + borrow;
    out.result = left - right - static_cast<uint32_t>(borrow);

    if (out.result & 0x80000000u) out.nzcv |= 0x80000000u;
    if (!out.result)              out.nzcv |= 0x40000000u;
    if (static_cast<uint64_t>(left) >= rhs)
        out.nzcv |= 0x20000000u;

    const int64_t signedWide =
        static_cast<int64_t>(static_cast<int32_t>(left)) -
        static_cast<int64_t>(static_cast<int32_t>(right)) -
        static_cast<int64_t>(borrow);
    if (signedWide > 2147483647ll || signedWide < -2147483648ll)
        out.nzcv |= 0x10000000u;
    return out;
}

inline ThumbArithmeticResult thumbAdd(uint32_t left, uint32_t right)
{
    return thumbAddWithCarry(left, right, false);
}

inline ThumbArithmeticResult thumbSub(uint32_t left, uint32_t right)
{
    return thumbSubWithCarry(left, right, true);
}

#endif
