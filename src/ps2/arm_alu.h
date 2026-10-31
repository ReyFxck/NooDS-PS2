#ifndef PS2_ARM_ALU_H
#define PS2_ARM_ALU_H

#include <cstdint>

// Portable decoder/shadow semantics for the deliberately small ARM subset
// admitted by the PS2 dynarec. Keeping this independent of emitted R5900 code
// lets host CI exercise all edge cases before the runtime differential gate.
namespace Ps2ArmAlu
{
    struct Decoded
    {
        bool immediate = false;
        uint8_t operation = 0;
        uint8_t rn = 0;
        uint8_t rd = 0;
        uint8_t rm = 0;
        uint8_t shiftType = 0;
        uint8_t shiftAmount = 0;
        uint32_t immediateValue = 0;
    };

    bool applyImmediateShift(uint32_t value, uint8_t type,
        uint8_t encodedAmount, uint32_t *result);
    bool decode(uint32_t opcode, Decoded *decoded);
    bool execute(uint32_t opcode, uint32_t registers[16]);
}

#endif
