#include "../defines.h"
#include "arm_alu.h"

namespace
{
uint32_t arithmeticShiftRight(uint32_t value, uint32_t amount)
{
    if (amount >= 32)
        return (value & 0x80000000u) ? 0xFFFFFFFFu : 0;

    uint32_t result = value >> amount;
    if (value & 0x80000000u)
        result |= ~(0xFFFFFFFFu >> amount);
    return result;
}
}

bool Ps2ArmAlu::applyImmediateShift(uint32_t value, uint8_t type,
    uint8_t encodedAmount, uint32_t *result)
{
    if (!result)
        return false;

    switch (type)
    {
        case 0: // LSL #0 means no shift.
            *result = encodedAmount ? value << encodedAmount : value;
            return true;

        case 1: // LSR #0 encodes LSR #32.
            *result = encodedAmount ? value >> encodedAmount : 0;
            return true;

        case 2: // ASR #0 encodes ASR #32.
            *result = arithmeticShiftRight(value,
                encodedAmount ? encodedAmount : 32u);
            return true;

        case 3:
            // ROR #0 is RRX and depends on CPSR.C. The current no-flags block
            // ABI intentionally has no CPSR input, so leave it to interpreter.
            if (!encodedAmount)
                return false;
            *result = (value >> encodedAmount) |
                (value << (32u - encodedAmount));
            return true;
    }

    return false;
}

bool Ps2ArmAlu::decode(uint32_t opcode, Decoded *decoded)
{
    if (!decoded ||
        (opcode >> 28) != 0xEu ||
        (opcode & 0x0C000000u) != 0 ||
        (opcode & 0x00100000u) != 0)
        return false;

    *decoded = Decoded();
    decoded->immediate = (opcode & 0x02000000u) != 0;
    decoded->operation = static_cast<uint8_t>((opcode >> 21) & 0xF);
    decoded->rn = static_cast<uint8_t>((opcode >> 16) & 0xF);
    decoded->rd = static_cast<uint8_t>((opcode >> 12) & 0xF);
    decoded->rm = static_cast<uint8_t>(opcode & 0xF);

    if (!decoded->immediate)
    {
        // Register-controlled shifts and the miscellaneous/multiply encodings
        // share bit 4. Only immediate barrel shifts are admitted for now.
        if (opcode & BIT(4))
            return false;
        decoded->shiftType = static_cast<uint8_t>((opcode >> 5) & 0x3);
        decoded->shiftAmount = static_cast<uint8_t>((opcode >> 7) & 0x1F);
        if (decoded->shiftType == 3 && decoded->shiftAmount == 0)
            return false; // RRX needs CPSR.C.
    }

    if (decoded->rd == 15 ||
        (!decoded->immediate && decoded->rm == 15) ||
        (decoded->operation != 13 && decoded->operation != 15 &&
            decoded->rn == 15))
        return false;

    switch (decoded->operation)
    {
        case 0:  // AND
        case 1:  // EOR
        case 2:  // SUB
        case 3:  // RSB
        case 4:  // ADD
        case 12: // ORR
        case 13: // MOV
        case 14: // BIC
        case 15: // MVN
            break;
        default:
            return false;
    }

    if (decoded->immediate)
    {
        uint32_t value = opcode & 0xFFu;
        uint32_t rotate = ((opcode >> 8) & 0xFu) * 2u;
        if (rotate)
            value = (value >> rotate) | (value << (32u - rotate));
        decoded->immediateValue = value;
    }

    return true;
}

bool Ps2ArmAlu::execute(uint32_t opcode, uint32_t registers[16])
{
    if (!registers)
        return false;

    Decoded decoded;
    if (!decode(opcode, &decoded))
        return false;

    uint32_t operand2 = decoded.immediateValue;
    if (!decoded.immediate &&
        !applyImmediateShift(registers[decoded.rm], decoded.shiftType,
            decoded.shiftAmount, &operand2))
        return false;

    uint32_t result = 0;
    switch (decoded.operation)
    {
        case 0:  result = registers[decoded.rn] & operand2; break;
        case 1:  result = registers[decoded.rn] ^ operand2; break;
        case 2:  result = registers[decoded.rn] - operand2; break;
        case 3:  result = operand2 - registers[decoded.rn]; break;
        case 4:  result = registers[decoded.rn] + operand2; break;
        case 12: result = registers[decoded.rn] | operand2; break;
        case 13: result = operand2; break;
        case 14: result = registers[decoded.rn] & ~operand2; break;
        case 15: result = ~operand2; break;
        default: return false;
    }

    registers[decoded.rd] = result;
    return true;
}
