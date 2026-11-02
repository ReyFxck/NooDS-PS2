#include "../defines.h"
#include "arm_alu.h"

#include <cstdio>

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

bool Ps2ArmAlu::applyRegisterShift(uint32_t value, uint8_t type,
    uint32_t registerAmount, uint32_t *result)
{
    if (!result)
        return false;

    uint32_t amount = registerAmount & 0xFFu;
    if (!amount)
    {
        *result = value;
        return true;
    }

    switch (type)
    {
        case 0: // LSL Rs
            *result = (amount < 32) ? value << amount : 0;
            return true;

        case 1: // LSR Rs
            *result = (amount < 32) ? value >> amount : 0;
            return true;

        case 2: // ASR Rs
            *result = arithmeticShiftRight(value, amount);
            return true;

        case 3: // ROR Rs
        {
            uint32_t rotate = amount & 31u;
            *result = rotate
                ? (value >> rotate) | (value << (32u - rotate))
                : value;
            return true;
        }
    }

    return false;
}

bool Ps2ArmAlu::conditionPassed(uint8_t condition, uint32_t cpsr)
{
    bool n = (cpsr & BIT(31)) != 0;
    bool z = (cpsr & BIT(30)) != 0;
    bool c = (cpsr & BIT(29)) != 0;
    bool v = (cpsr & BIT(28)) != 0;

    switch (condition & 0xFu)
    {
        case 0x0: return z;
        case 0x1: return !z;
        case 0x2: return c;
        case 0x3: return !c;
        case 0x4: return n;
        case 0x5: return !n;
        case 0x6: return v;
        case 0x7: return !v;
        case 0x8: return c && !z;
        case 0x9: return !c || z;
        case 0xA: return n == v;
        case 0xB: return n != v;
        case 0xC: return !z && n == v;
        case 0xD: return z || n != v;
        case 0xE: return true;
        default:  return false;
    }
}

bool Ps2ArmAlu::decode(uint32_t opcode, Decoded *decoded)
{
    if (!decoded ||
        (opcode & 0x0C000000u) != 0)
        return false;

    *decoded = Decoded();
    decoded->condition = static_cast<uint8_t>(opcode >> 28);
    decoded->immediate = (opcode & 0x02000000u) != 0;
    decoded->setFlags = (opcode & 0x00100000u) != 0;
    decoded->operation = static_cast<uint8_t>((opcode >> 21) & 0xF);
    decoded->rn = static_cast<uint8_t>((opcode >> 16) & 0xF);
    decoded->rd = static_cast<uint8_t>((opcode >> 12) & 0xF);
    decoded->rm = static_cast<uint8_t>(opcode & 0xF);
    decoded->writesResult = decoded->operation < 8 || decoded->operation > 11;

    // TST/TEQ/CMP/CMN have a fixed S=1, Rd=0 encoding and only update flags.
    // Rejecting the other encodings avoids admitting overlapping PSR/system
    // instructions as ordinary data processing operations.
    if (!decoded->writesResult && (!decoded->setFlags || decoded->rd != 0))
        return false;

    if (!decoded->immediate)
    {
        decoded->shiftType = static_cast<uint8_t>((opcode >> 5) & 0x3);
        if (opcode & BIT(4))
        {
            // Bit 7 distinguishes data-processing register shifts from the
            // multiply/extra-transfer encodings that share bits 4-7.
            if (opcode & BIT(7))
                return false;
            decoded->registerShift = true;
            decoded->rs = static_cast<uint8_t>((opcode >> 8) & 0xF);
        }
        else
        {
            decoded->shiftAmount = static_cast<uint8_t>((opcode >> 7) & 0x1F);
            if (decoded->shiftType == 3 && decoded->shiftAmount == 0)
                return false; // RRX needs CPSR.C.
        }
    }

    if ((decoded->writesResult && decoded->rd == 15) ||
        (!decoded->immediate && decoded->rm == 15) ||
        (decoded->registerShift && decoded->rs == 15) ||
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
        case 8:  // TST
        case 9:  // TEQ
        case 10: // CMP
        case 11: // CMN
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
        decoded->immediateRotate = static_cast<uint8_t>(rotate);
        if (rotate)
            value = (value >> rotate) | (value << (32u - rotate));
        decoded->immediateValue = value;
    }

    // The emitted flags path currently admits logical operations only when
    // Operand2 preserves CPSR.C. Arithmetic operations calculate C/V from the
    // result and may use any otherwise supported shifter form.
    if (decoded->setFlags &&
        (decoded->operation == 0 || decoded->operation == 1 ||
         decoded->operation == 8 || decoded->operation == 9 ||
         decoded->operation == 12 || decoded->operation == 13 ||
         decoded->operation == 14 || decoded->operation == 15))
    {
        bool preservesCarry = decoded->immediate
            ? decoded->immediateRotate == 0
            : (!decoded->registerShift && decoded->shiftType == 0 &&
                decoded->shiftAmount == 0);
        if (!preservesCarry)
            return false;
    }

    return true;
}

bool Ps2ArmAlu::decodeBranch(uint32_t opcode, DecodedBranch *decoded)
{
    if (!decoded || (opcode & 0x0E000000u) != 0x0A000000u ||
        (opcode & BIT(24))) // BL writes LR and needs a wider block ABI.
        return false;

    uint8_t condition = static_cast<uint8_t>(opcode >> 28);
    if (condition == 0xFu) // ARMv5 BLX immediate, not a normal B condition.
        return false;

    int32_t offset = static_cast<int32_t>(opcode & 0x00FFFFFFu);
    if (offset & 0x00800000)
        offset |= static_cast<int32_t>(0xFF000000u);
    offset *= 4;

    decoded->condition = condition;
    decoded->offset = offset;
    return true;
}

bool Ps2ArmAlu::executeBranch(uint32_t opcode, uint32_t pc, uint32_t cpsr,
    uint32_t *nextPc)
{
    if (!nextPc)
        return false;

    DecodedBranch decoded;
    if (!decodeBranch(opcode, &decoded))
        return false;

    *nextPc = conditionPassed(decoded.condition, cpsr)
        ? pc + 8u + static_cast<uint32_t>(decoded.offset)
        : pc + 4u;
    return true;
}

bool Ps2ArmAlu::execute(uint32_t opcode, uint32_t registers[16], uint32_t *cpsr)
{
    if (!registers)
        return false;

    Decoded decoded;
    if (!decode(opcode, &decoded))
        return false;
    if ((decoded.condition != 0xEu || decoded.setFlags) && !cpsr)
        return false;
    if (cpsr && !conditionPassed(decoded.condition, *cpsr))
        return true;

    uint32_t operand2 = decoded.immediateValue;
    if (!decoded.immediate)
    {
        bool shifted = decoded.registerShift
            ? applyRegisterShift(registers[decoded.rm], decoded.shiftType,
                registers[decoded.rs], &operand2)
            : applyImmediateShift(registers[decoded.rm], decoded.shiftType,
                decoded.shiftAmount, &operand2);
        if (!shifted)
            return false;
    }

    uint32_t operand1 = registers[decoded.rn];
    uint32_t result = 0;
    switch (decoded.operation)
    {
        case 0:  result = operand1 & operand2; break;
        case 1:  result = operand1 ^ operand2; break;
        case 2:  result = operand1 - operand2; break;
        case 3:  result = operand2 - operand1; break;
        case 4:  result = operand1 + operand2; break;
        case 8:  result = operand1 & operand2; break;
        case 9:  result = operand1 ^ operand2; break;
        case 10: result = operand1 - operand2; break;
        case 11: result = operand1 + operand2; break;
        case 12: result = operand1 | operand2; break;
        case 13: result = operand2; break;
        case 14: result = operand1 & ~operand2; break;
        case 15: result = ~operand2; break;
        default: return false;
    }

    if (decoded.writesResult)
        registers[decoded.rd] = result;

    if (decoded.setFlags)
    {
        uint32_t flags = result & BIT(31);
        if (!result) flags |= BIT(30);

        uint32_t left = operand1;
        if (decoded.operation == 2 || decoded.operation == 10) // SUB/CMP
        {
            if (left >= operand2) flags |= BIT(29);
            if (((left ^ operand2) & (left ^ result)) & BIT(31))
                flags |= BIT(28);
        }
        else if (decoded.operation == 3) // RSB
        {
            if (operand2 >= left) flags |= BIT(29);
            if (((operand2 ^ left) & (operand2 ^ result)) & BIT(31))
                flags |= BIT(28);
        }
        else if (decoded.operation == 4 || decoded.operation == 11) // ADD/CMN
        {
            if (static_cast<uint64_t>(left) + operand2 > 0xFFFFFFFFu)
                flags |= BIT(29);
            if ((~(left ^ operand2) & (left ^ result)) & BIT(31))
                flags |= BIT(28);
        }
        else // Logical operations preserve C/V in the admitted subset.
        {
            flags |= *cpsr & (BIT(29) | BIT(28));
        }
        *cpsr = (*cpsr & 0x0FFFFFFFu) | flags;
    }
    return true;
}

void Ps2ArmAlu::inspect(uint32_t opcode, uint32_t pc, InstructionInfo *info)
{
    if (!info)
        return;

    *info = InstructionInfo();
    info->condition = static_cast<uint8_t>(opcode >> 28);
    info->readsFlags = info->condition != 0xEu;

    if ((opcode & 0x0FFFFFF0u) == 0x012FFF10u)
    {
        info->kind = KIND_BRANCH_EXCHANGE;
        info->rm = static_cast<uint8_t>(opcode & 0xFu);
        info->readMask = static_cast<uint16_t>(1u << info->rm);
        info->terminatesBlock = true;
        return;
    }

    uint32_t group = (opcode >> 25) & 0x7u;
    if (group == 0x5u)
    {
        info->kind = KIND_BRANCH;
        int32_t offset = static_cast<int32_t>(opcode & 0x00FFFFFFu);
        if (offset & 0x00800000)
            offset |= static_cast<int32_t>(0xFF000000u);
        offset *= 4;
        info->branchTarget = pc + 8u + static_cast<uint32_t>(offset);
        info->backEdge = info->branchTarget <= pc;
        info->terminatesBlock = true;
        if (opcode & BIT(24))
            info->writeMask = static_cast<uint16_t>(1u << 14);
        return;
    }

    if (group == 0x4u)
    {
        info->kind = KIND_MULTI_MEMORY;
        info->rn = static_cast<uint8_t>((opcode >> 16) & 0xFu);
        info->readMask = static_cast<uint16_t>(1u << info->rn);
        uint16_t registers = static_cast<uint16_t>(opcode);
        if (opcode & BIT(20))
            info->writeMask = registers;
        else
            info->readMask |= registers;
        if ((opcode & BIT(20)) && (registers & BIT(15)))
            info->terminatesBlock = true;
        return;
    }

    bool longMultiply = (opcode & 0x0F8000F0u) == 0x00800090u;
    if ((opcode & 0x0FC000F0u) == 0x00000090u || longMultiply)
    {
        info->kind = KIND_MULTIPLY;
        info->rd = static_cast<uint8_t>((opcode >> (longMultiply ? 12 : 16)) & 0xFu);
        info->rn = static_cast<uint8_t>((opcode >> (longMultiply ? 16 : 12)) & 0xFu);
        info->rs = static_cast<uint8_t>((opcode >> 8) & 0xFu);
        info->rm = static_cast<uint8_t>(opcode & 0xFu);
        info->readMask = static_cast<uint16_t>((1u << info->rm) | (1u << info->rs));
        if (opcode & BIT(21))
        {
            info->readMask |= static_cast<uint16_t>(1u << info->rn);
            if (longMultiply)
                info->readMask |= static_cast<uint16_t>(1u << info->rd);
        }
        info->writeMask = static_cast<uint16_t>(1u << info->rd);
        if (longMultiply)
            info->writeMask |= static_cast<uint16_t>(1u << info->rn);
        info->writesFlags = (opcode & BIT(20)) != 0;
        return;
    }

    if ((opcode & 0x0C000000u) == 0x04000000u ||
        (opcode & 0x0E000090u) == 0x00000090u)
    {
        info->kind = KIND_MEMORY;
        info->rn = static_cast<uint8_t>((opcode >> 16) & 0xFu);
        info->rd = static_cast<uint8_t>((opcode >> 12) & 0xFu);
        info->readMask = static_cast<uint16_t>(1u << info->rn);
        if (opcode & BIT(20))
            info->writeMask = static_cast<uint16_t>(1u << info->rd);
        else
            info->readMask |= static_cast<uint16_t>(1u << info->rd);
        bool extraTransfer = (opcode & 0x0E000090u) == 0x00000090u;
        bool registerOffset = extraTransfer
            ? !(opcode & BIT(22)) : (opcode & BIT(25));
        if (registerOffset)
        {
            info->rm = static_cast<uint8_t>(opcode & 0xFu);
            info->readMask |= static_cast<uint16_t>(1u << info->rm);
        }
        if ((opcode & BIT(21)) || !(opcode & BIT(24)))
            info->writeMask |= static_cast<uint16_t>(1u << info->rn);
        if ((opcode & BIT(20)) && info->rd == 15)
            info->terminatesBlock = true;
        return;
    }

    if ((opcode & 0x0C000000u) == 0)
    {
        info->kind = KIND_DATA;
        info->immediate = (opcode & BIT(25)) != 0;
        info->operation = static_cast<uint8_t>((opcode >> 21) & 0xFu);
        info->rn = static_cast<uint8_t>((opcode >> 16) & 0xFu);
        info->rd = static_cast<uint8_t>((opcode >> 12) & 0xFu);
        info->rm = static_cast<uint8_t>(opcode & 0xFu);
        info->writesFlags = (opcode & BIT(20)) != 0;

        bool usesRn = info->operation != 13 && info->operation != 15;
        bool writesRd = info->operation < 8 || info->operation > 11;
        if (usesRn)
            info->readMask |= static_cast<uint16_t>(1u << info->rn);
        if (writesRd)
        {
            info->writeMask |= static_cast<uint16_t>(1u << info->rd);
            if (info->rd == 15)
                info->terminatesBlock = true;
        }

        if (info->immediate)
        {
            uint32_t value = opcode & 0xFFu;
            uint32_t rotate = ((opcode >> 8) & 0xFu) * 2u;
            if (rotate)
                value = (value >> rotate) | (value << (32u - rotate));
            info->immediateValue = value;
        }
        else
        {
            info->readMask |= static_cast<uint16_t>(1u << info->rm);
            info->shiftType = static_cast<uint8_t>((opcode >> 5) & 0x3u);
            info->registerShift = (opcode & BIT(4)) != 0;
            if (info->registerShift)
            {
                info->rs = static_cast<uint8_t>((opcode >> 8) & 0xFu);
                info->readMask |= static_cast<uint16_t>(1u << info->rs);
            }
            else
            {
                info->shiftAmount = static_cast<uint8_t>((opcode >> 7) & 0x1Fu);
                if (info->shiftType == 3 && !info->shiftAmount)
                    info->readsFlags = true; // RRX consumes CPSR.C.
            }
        }
        return;
    }

    info->kind = (group >= 0x6u) ? KIND_SYSTEM : KIND_UNKNOWN;
}

Ps2ArmAlu::RejectReason Ps2ArmAlu::rejectionReason(uint32_t opcode)
{
    Decoded decoded;
    if (decode(opcode, &decoded))
        return REJECT_NONE;
    DecodedBranch branch;
    if (decodeBranch(opcode, &branch))
        return REJECT_NONE;

    InstructionInfo info;
    inspect(opcode, 0, &info);
    switch (info.kind)
    {
        case KIND_MEMORY:
        case KIND_MULTI_MEMORY:
            return REJECT_MEMORY;
        case KIND_BRANCH:
        case KIND_BRANCH_EXCHANGE:
            return REJECT_BRANCH;
        case KIND_MULTIPLY:
            return REJECT_UNSUPPORTED_OPCODE;
        case KIND_SYSTEM:
            return REJECT_SYSTEM;
        case KIND_DATA:
            if (info.writesFlags || info.readsFlags)
                return REJECT_FLAGS;
            if ((info.readMask | info.writeMask) & BIT(15))
                return REJECT_PC;
            if (!info.immediate && info.registerShift && (opcode & BIT(7)))
                return REJECT_SHIFT;
            return REJECT_UNSUPPORTED_OPCODE;
        default:
            return REJECT_OTHER;
    }
}

const char *Ps2ArmAlu::rejectionReasonName(RejectReason reason)
{
    static const char *names[REJECT_REASON_COUNT] =
    {
        "supported", "unsupported-opcode", "condition", "flags", "shift",
        "memory", "branch", "pc", "system", "other"
    };
    return (reason < REJECT_REASON_COUNT) ? names[reason] : "other";
}

void Ps2ArmAlu::format(uint32_t opcode, uint32_t pc, char *buffer, uint32_t size)
{
    if (!buffer || !size)
        return;

    InstructionInfo info;
    inspect(opcode, pc, &info);
    static const char *conditions[16] =
        { "EQ", "NE", "CS", "CC", "MI", "PL", "VS", "VC",
          "HI", "LS", "GE", "LT", "GT", "LE", "AL", "NV" };
    static const char *operations[16] =
        { "AND", "EOR", "SUB", "RSB", "ADD", "ADC", "SBC", "RSC",
          "TST", "TEQ", "CMP", "CMN", "ORR", "MOV", "BIC", "MVN" };
    static const char *shifts[4] = { "LSL", "LSR", "ASR", "ROR" };
    const char *condition = conditions[info.condition & 0xFu];

    if (info.kind == KIND_BRANCH)
    {
        std::snprintf(buffer, size, "%s.%s 0x%08lX",
            (opcode & BIT(24)) ? "BL" : "B", condition,
            static_cast<unsigned long>(info.branchTarget));
        return;
    }
    if (info.kind == KIND_BRANCH_EXCHANGE)
    {
        std::snprintf(buffer, size, "BX.%s r%u", condition, info.rm);
        return;
    }
    if (info.kind == KIND_MEMORY || info.kind == KIND_MULTI_MEMORY)
    {
        bool extraTransfer = (opcode & 0x0E000090u) == 0x00000090u;
        const char *mnemonic = (opcode & BIT(20)) ? "LDR" : "STR";
        if (extraTransfer)
        {
            uint8_t type = static_cast<uint8_t>((opcode >> 5) & 0x3u);
            if (type == 1) mnemonic = (opcode & BIT(20)) ? "LDRH" : "STRH";
            else if (type == 2) mnemonic = "LDRSB";
            else if (type == 3) mnemonic = "LDRSH";
        }

        if (info.rm != 0xFFu)
            std::snprintf(buffer, size, "%s.%s r%u,[r%u,%sr%u]",
                mnemonic, condition, info.rd, info.rn,
                (opcode & BIT(23)) ? "+" : "-", info.rm);
        else
            std::snprintf(buffer, size, "%s.%s r%u,[r%u]",
                mnemonic, condition, info.rd, info.rn);
        return;
    }
    if (info.kind == KIND_MULTIPLY)
    {
        bool longMultiply = (opcode & 0x0F8000F0u) == 0x00800090u;
        if (longMultiply)
        {
            const char *mnemonic;
            if (opcode & BIT(22))
                mnemonic = (opcode & BIT(21)) ? "SMLAL" : "SMULL";
            else
                mnemonic = (opcode & BIT(21)) ? "UMLAL" : "UMULL";
            std::snprintf(buffer, size, "%s.%s r%u,r%u,r%u,r%u",
                mnemonic, condition, info.rd, info.rn, info.rm, info.rs);
        }
        else
        {
            std::snprintf(buffer, size, "%s.%s r%u,r%u,r%u",
                (opcode & BIT(21)) ? "MLA" : "MUL", condition,
                info.rd, info.rm, info.rs);
        }
        return;
    }
    if (info.kind != KIND_DATA)
    {
        std::snprintf(buffer, size, "UNKNOWN.%s 0x%08lX", condition,
            static_cast<unsigned long>(opcode));
        return;
    }

    char operand[48] = {};
    if (info.immediate)
    {
        std::snprintf(operand, sizeof(operand), "#0x%lX",
            static_cast<unsigned long>(info.immediateValue));
    }
    else if (info.registerShift)
    {
        std::snprintf(operand, sizeof(operand), "r%u,%s r%u",
            info.rm, shifts[info.shiftType], info.rs);
    }
    else if (info.shiftAmount || info.shiftType)
    {
        uint32_t amount = info.shiftAmount;
        if (!amount && (info.shiftType == 1 || info.shiftType == 2))
            amount = 32;
        if (!amount && info.shiftType == 3)
            std::snprintf(operand, sizeof(operand), "r%u,RRX", info.rm);
        else
            std::snprintf(operand, sizeof(operand), "r%u,%s #%lu",
                info.rm, shifts[info.shiftType], static_cast<unsigned long>(amount));
    }
    else
    {
        std::snprintf(operand, sizeof(operand), "r%u", info.rm);
    }

    const char *mnemonic = operations[info.operation & 0xFu];
    if (info.operation >= 8 && info.operation <= 11)
        std::snprintf(buffer, size, "%s.%s r%u,%s", mnemonic, condition,
            info.rn, operand);
    else if (info.operation == 13 || info.operation == 15)
        std::snprintf(buffer, size, "%s%s.%s r%u,%s", mnemonic,
            info.writesFlags ? "S" : "", condition, info.rd, operand);
    else
        std::snprintf(buffer, size, "%s%s.%s r%u,r%u,%s", mnemonic,
            info.writesFlags ? "S" : "", condition, info.rd, info.rn, operand);
}
