#include <cassert>
#include <cstdint>
#include <cstring>

#include "../src/ps2/arm_alu.h"

namespace
{
uint32_t randomState = 0xC001D00Du;

uint32_t nextRandom()
{
    randomState ^= randomState << 13;
    randomState ^= randomState >> 17;
    randomState ^= randomState << 5;
    return randomState;
}

void expectResult(uint32_t opcode, uint8_t rd, uint32_t expected,
    const uint32_t source[16])
{
    uint32_t registers[16];
    std::memcpy(registers, source, sizeof(registers));
    assert(Ps2ArmAlu::execute(opcode, registers));
    assert(registers[rd] == expected);
}
}

int main()
{
    uint32_t registers[16] = {};
    registers[0] = 0x12345678u;
    registers[1] = 0x01020304u;
    registers[11] = 0xF2345678u;

    expectResult(0xE060E001u, 14, 0xEECDAC8Cu, registers); // RSB
    expectResult(0xE1C0E001u, 14, 0x12345478u, registers); // BIC
    expectResult(0xE1E0E001u, 14, 0xFEFDFCFBu, registers); // MVN
    expectResult(0xE1A02181u, 2, 0x08101820u, registers);  // LSL #3
    expectResult(0xE1A03220u, 3, 0x01234567u, registers);  // LSR #4
    expectResult(0xE1A0424Bu, 4, 0xFF234567u, registers);  // ASR #4
    expectResult(0xE1A05460u, 5, 0x78123456u, registers);  // ROR #8
    expectResult(0xE1A06020u, 6, 0x00000000u, registers);  // LSR #32
    expectResult(0xE1A0704Bu, 7, 0xFFFFFFFFu, registers);  // ASR #32

    registers[8] = 0x000000FFu;
    registers[9] = 0x0000ABCDu;
    registers[14] = 8u;
    expectResult(0xE0098E18u, 8, 0x0000AB00u, registers); // AND r8,r9,r8,LSL r14
    expectResult(0xE1A08E58u, 8, 0x00000000u, registers); // MOV r8,r8,ASR r14 (source snapshot)

    uint32_t shifted = 0;
    assert(Ps2ArmAlu::applyRegisterShift(0x80000001u, 0, 32, &shifted) && shifted == 0);
    assert(Ps2ArmAlu::applyRegisterShift(0x80000001u, 1, 40, &shifted) && shifted == 0);
    assert(Ps2ArmAlu::applyRegisterShift(0x80000001u, 2, 40, &shifted) && shifted == 0xFFFFFFFFu);
    assert(Ps2ArmAlu::applyRegisterShift(0x12345678u, 3, 32, &shifted) && shifted == 0x12345678u);

    uint32_t cpsr = 0xB0000000u; // N=1, Z=0, C=1, V=1
    const bool expectedConditions[16] =
        { false, true, true, false, true, false, true, false,
          true, false, true, false, true, false, true, false };
    for (uint8_t condition = 0; condition < 16; condition++)
        assert(Ps2ArmAlu::conditionPassed(condition, cpsr) ==
            expectedConditions[condition]);

    // Conditional instructions that fail are exact no-ops.
    registers[1] = 0xA5A55A5Au;
    registers[2] = 0x11223344u;
    cpsr = 0;
    assert(Ps2ArmAlu::execute(0x01A02001u, registers, &cpsr)); // MOV.EQ r2,r1
    assert(registers[2] == 0x11223344u);
    cpsr = 0x40000000u;
    assert(Ps2ArmAlu::execute(0x01A02001u, registers, &cpsr));
    assert(registers[2] == 0xA5A55A5Au && cpsr == 0x40000000u);

    // Logical S operations in the admitted subset update N/Z and preserve C/V.
    registers[2] = 8;
    cpsr = 0x30000013u;
    assert(Ps2ArmAlu::execute(0xE2322008u, registers, &cpsr)); // EORS r2,r2,#8
    assert(registers[2] == 0 && cpsr == 0x70000013u);

    // Arithmetic S operations calculate all four flags, including borrow and overflow.
    registers[5] = 0;
    cpsr = 0xF0000013u;
    assert(Ps2ArmAlu::execute(0xE2555001u, registers, &cpsr)); // SUBS r5,r5,#1
    assert(registers[5] == 0xFFFFFFFFu && cpsr == 0x80000013u);
    registers[5] = 0x80000000u;
    cpsr = 0x13u;
    assert(Ps2ArmAlu::execute(0xE2555001u, registers, &cpsr));
    assert(registers[5] == 0x7FFFFFFFu && cpsr == 0x30000013u);
    registers[1] = 0xFFFFFFFFu;
    cpsr = 0xF0000013u;
    assert(Ps2ArmAlu::execute(0xE2910001u, registers, &cpsr)); // ADDS r0,r1,#1
    assert(registers[0] == 0 && cpsr == 0x60000013u);

    // The steady-state hot loop compares r12/r2 without modifying any GPR.
    uint32_t cmpState[16] = {};
    cmpState[12] = 3;
    cmpState[2] = 5;
    uint32_t cmpBefore[16];
    std::memcpy(cmpBefore, cmpState, sizeof(cmpState));
    cpsr = 0xF0000013u;
    assert(Ps2ArmAlu::execute(0xE15C0002u, cmpState, &cpsr)); // CMP r12,r2
    assert(std::memcmp(cmpState, cmpBefore, sizeof(cmpState)) == 0);
    assert(cpsr == 0x80000013u);

    // A failed conditional test instruction is a complete no-op, flags included.
    cpsr = 0x00000013u;
    assert(Ps2ArmAlu::execute(0x015C0002u, cmpState, &cpsr)); // CMP.EQ r12,r2
    assert(std::memcmp(cmpState, cmpBefore, sizeof(cmpState)) == 0);
    assert(cpsr == 0x00000013u);

    // Differential flag coverage for result-writing and test/compare forms.
    for (int test = 0; test < 100000; test++)
    {
        uint32_t left = nextRandom();
        uint32_t right = nextRandom();
        uint32_t lowCpsr = nextRandom() & 0x0FFFFFFFu;
        const uint32_t opcodes[] =
            { 0xE0902001u, 0xE0502001u, 0xE0702001u, 0xE0302001u,
              0xE1100001u, 0xE1300001u, 0xE1500001u, 0xE1700001u };

        for (unsigned int operation = 0;
            operation < sizeof(opcodes) / sizeof(opcodes[0]); operation++)
        {
            uint32_t state[16] = {};
            state[0] = left;
            state[1] = right;
            uint32_t flags = lowCpsr | 0x30000000u; // Logical C/V reference.
            assert(Ps2ArmAlu::execute(opcodes[operation], state, &flags));

            uint32_t expected;
            bool carry;
            bool overflow;
            if (operation == 0 || operation == 7) // ADD/CMN
            {
                expected = left + right;
                carry = static_cast<uint64_t>(left) + right > 0xFFFFFFFFu;
                overflow = ((~(left ^ right) & (left ^ expected)) &
                    0x80000000u) != 0;
            }
            else if (operation == 1 || operation == 6) // SUB/CMP
            {
                expected = left - right;
                carry = left >= right;
                overflow = (((left ^ right) & (left ^ expected)) &
                    0x80000000u) != 0;
            }
            else if (operation == 2) // RSB
            {
                expected = right - left;
                carry = right >= left;
                overflow = (((right ^ left) & (right ^ expected)) &
                    0x80000000u) != 0;
            }
            else if (operation == 3 || operation == 5) // EOR/TEQ
            {
                expected = left ^ right;
                carry = true;
                overflow = true;
            }
            else // TST preserves C/V for an unshifted register Operand2.
            {
                expected = left & right;
                carry = true;
                overflow = true;
            }

            uint32_t expectedFlags = lowCpsr;
            if (expected & 0x80000000u) expectedFlags |= 0x80000000u;
            if (!expected) expectedFlags |= 0x40000000u;
            if (carry) expectedFlags |= 0x20000000u;
            if (overflow) expectedFlags |= 0x10000000u;
            if (operation < 4)
                assert(state[2] == expected);
            else
                assert(state[0] == left && state[1] == right && state[2] == 0);
            assert(flags == expectedFlags);
        }
    }

    Ps2ArmAlu::Decoded decoded;
    assert(Ps2ArmAlu::decode(0x11A02001u, &decoded));  // condition supported
    assert(decoded.condition == 1);
    assert(Ps2ArmAlu::decode(0xE1B02001u, &decoded));  // MOVS, carry preserved
    assert(decoded.setFlags);
    assert(!Ps2ArmAlu::decode(0xE1B02181u, &decoded)); // shifted MOVS changes carry
    assert(!Ps2ArmAlu::decode(0xE1A02061u, &decoded)); // RRX needs carry
    assert(Ps2ArmAlu::decode(0xE1A02111u, &decoded));  // register-controlled LSL
    assert(decoded.registerShift && decoded.rs == 1);
    assert(Ps2ArmAlu::decode(0xE15C0002u, &decoded)); // hot CMP r12,r2
    assert(!decoded.writesResult && decoded.setFlags && decoded.rd == 0);
    assert(!Ps2ArmAlu::decode(0xE14C0002u, &decoded)); // CMP encoding requires S
    assert(!Ps2ArmAlu::decode(0xE15C1002u, &decoded)); // CMP encoding requires Rd=0
    assert(!Ps2ArmAlu::decode(0xE1A0F001u, &decoded)); // writes PC
    assert(!Ps2ArmAlu::decode(0xE1A0200Fu, &decoded)); // reads PC

    Ps2ArmAlu::InstructionInfo info;
    Ps2ArmAlu::inspect(0xE0098E18u, 0x020672F8u, &info);
    assert(info.kind == Ps2ArmAlu::KIND_DATA);
    assert(info.registerShift && info.rs == 14 && info.rm == 8);
    assert(info.readMask == ((1u << 9) | (1u << 8) | (1u << 14)));
    assert(info.writeMask == (1u << 8));
    assert(Ps2ArmAlu::rejectionReason(0xE0098E18u) == Ps2ArmAlu::REJECT_NONE);
    assert(Ps2ArmAlu::rejectionReason(0xE11190B8u) == Ps2ArmAlu::REJECT_MEMORY);
    Ps2ArmAlu::inspect(0xE15C0002u, 0x02066E80u, &info);
    assert(info.kind == Ps2ArmAlu::KIND_DATA && info.writesFlags);
    assert(info.readMask == ((1u << 12) | (1u << 2)) && info.writeMask == 0);
    assert(Ps2ArmAlu::rejectionReason(0xE15C0002u) == Ps2ArmAlu::REJECT_NONE);
    Ps2ArmAlu::inspect(0xE0810293u, 0x02000000u, &info); // UMULL r0,r1,r3,r2
    assert(info.kind == Ps2ArmAlu::KIND_MULTIPLY);
    assert(info.readMask == ((1u << 2) | (1u << 3)));
    assert(info.writeMask == ((1u << 0) | (1u << 1)));
    assert(Ps2ArmAlu::rejectionReason(0x01A02001u) == Ps2ArmAlu::REJECT_NONE);
    assert(Ps2ArmAlu::rejectionReason(0xBAFFFFFAu) == Ps2ArmAlu::REJECT_BRANCH);

    char text[96] = {};
    Ps2ArmAlu::format(0xE0098E18u, 0x020672F8u, text, sizeof(text));
    assert(std::strstr(text, "AND.AL") && std::strstr(text, "LSL r14"));
    Ps2ArmAlu::format(0xE11190B8u, 0x020672F0u, text, sizeof(text));
    assert(std::strstr(text, "LDRH.AL") && std::strstr(text, "[r1,-r8]"));

    return 0;
}
