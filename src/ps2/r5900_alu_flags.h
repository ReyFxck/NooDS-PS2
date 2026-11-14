#ifndef PS2_R5900_ALU_FLAGS_H
#define PS2_R5900_ALU_FLAGS_H

#include <cstddef>
#include <cstdint>
#include "r5900_emitter.h"

// Input T0=Rn, T1=operand2, T2=final 32-bit result. Output V0 is
// the signed overflow bit; T3 and T4 are clobbered. Carry/borrow is
// already included in T2. This matches the interpreter's NZCV semantics.
inline size_t emitArmCarryOverflow(uint32_t *code, bool subtraction,
    bool reverse)
{
    size_t words = 0;
    const int left = reverse ? R5900::R_T1 : R5900::R_T0;
    const int right = reverse ? R5900::R_T0 : R5900::R_T1;
    code[words++] = R5900::xor_(R5900::R_T3, left, right);
    if (!subtraction)
        code[words++] = R5900::nor_(R5900::R_T3, R5900::R_T3, R5900::R_ZERO);
    code[words++] = R5900::xor_(R5900::R_T4, left, R5900::R_T2);
    code[words++] = R5900::and_(R5900::R_V0, R5900::R_T3, R5900::R_T4);
    code[words++] = R5900::srl(R5900::R_V0, R5900::R_V0, 31);
    return words;
}
#endif
