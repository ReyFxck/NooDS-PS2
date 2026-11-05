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

struct HalfwordMemory
{
    uint8_t bytes[512];
};

bool readHalfword(void *context, uint32_t address, uint16_t *value)
{
    HalfwordMemory *memory = static_cast<HalfwordMemory*>(context);
    if (!memory || !value || address + 1 >= sizeof(memory->bytes))
        return false;
    *value = static_cast<uint16_t>(memory->bytes[address] |
        (static_cast<uint16_t>(memory->bytes[address + 1]) << 8));
    return true;
}

bool writeHalfword(void *context, uint32_t address, uint16_t value)
{
    HalfwordMemory *memory = static_cast<HalfwordMemory*>(context);
    if (!memory || address + 1 >= sizeof(memory->bytes))
        return false;
    memory->bytes[address] = static_cast<uint8_t>(value);
    memory->bytes[address + 1] = static_cast<uint8_t>(value >> 8);
    return true;
}

bool readWord(void *context, uint32_t address, uint32_t *value)
{
    HalfwordMemory *memory = static_cast<HalfwordMemory*>(context);
    if (!memory || !value || address + 3 >= sizeof(memory->bytes))
        return false;
    *value = static_cast<uint32_t>(memory->bytes[address]) |
        (static_cast<uint32_t>(memory->bytes[address + 1]) << 8) |
        (static_cast<uint32_t>(memory->bytes[address + 2]) << 16) |
        (static_cast<uint32_t>(memory->bytes[address + 3]) << 24);
    return true;
}

bool writeWord(void *context, uint32_t address, uint32_t value)
{
    HalfwordMemory *memory = static_cast<HalfwordMemory*>(context);
    if (!memory || address + 3 >= sizeof(memory->bytes))
        return false;
    memory->bytes[address] = static_cast<uint8_t>(value);
    memory->bytes[address + 1] = static_cast<uint8_t>(value >> 8);
    memory->bytes[address + 2] = static_cast<uint8_t>(value >> 16);
    memory->bytes[address + 3] = static_cast<uint8_t>(value >> 24);
    return true;
}

bool readByte(void *context, uint32_t address, uint8_t *value)
{
    HalfwordMemory *memory = static_cast<HalfwordMemory*>(context);
    if (!memory || !value || address >= sizeof(memory->bytes))
        return false;
    *value = memory->bytes[address];
    return true;
}

bool writeByte(void *context, uint32_t address, uint8_t value)
{
    HalfwordMemory *memory = static_cast<HalfwordMemory*>(context);
    if (!memory || address >= sizeof(memory->bytes))
        return false;
    memory->bytes[address] = value;
    return true;
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

    // Logical S operations take carry from the ARM barrel shifter.
    registers[1] = 0x00018000u;
    cpsr = 0x10000013u;
    assert(Ps2ArmAlu::execute(0xE1B01821u, registers, &cpsr)); // MOVS r1,r1,LSR #16
    assert(registers[1] == 1u && cpsr == 0x30000013u);
    registers[2] = 0x80000000u;
    cpsr = 0x10000013u;
    assert(Ps2ArmAlu::execute(0xE2121102u, registers, &cpsr)); // ANDS r1,r2,#0x80000000
    assert(registers[1] == 0x80000000u && cpsr == 0xB0000013u);

    // Short ARM MUL/MLA subset, including N/Z-only S flags and condition.
    {
        Ps2ArmAlu::DecodedMultiply multiply;
        assert(Ps2ArmAlu::decodeMultiply(0xE0040291u, &multiply));
        assert(!multiply.accumulate && !multiply.setFlags &&
            multiply.rd == 4 && multiply.rm == 1 && multiply.rs == 2);

        uint32_t mulState[16] = {};
        mulState[1] = 0xFFFFFFFEu;
        mulState[2] = 3u;
        mulState[3] = 10u;
        uint32_t mulCpsr = 0x30000013u;
        assert(Ps2ArmAlu::executeMultiply(0xE0040291u, mulState, &mulCpsr));
        assert(mulState[4] == 0xFFFFFFFAu && mulCpsr == 0x30000013u);

        assert(Ps2ArmAlu::executeMultiply(0xE0253291u, mulState, &mulCpsr));
        assert(mulState[5] == 4u);

        mulState[1] = 0u;
        mulState[4] = 0xFFFFFFFFu;
        assert(Ps2ArmAlu::executeMultiply(0xE0140291u, mulState, &mulCpsr));
        assert(mulState[4] == 0u && mulCpsr == 0x70000013u);

        mulState[1] = 7u;
        mulState[4] = 0xA5A55A5Au;
        mulCpsr = 0x00000013u; // EQ fails.
        assert(Ps2ArmAlu::executeMultiply(0x00040291u, mulState, &mulCpsr));
        assert(mulState[4] == 0xA5A55A5Au && mulCpsr == 0x00000013u);

        // Long multiply subset: UMULL/UMLAL/SMULL/SMLAL, including the
        // current NooDS N/Z flag semantics and conditional no-op behavior.
        Ps2ArmAlu::DecodedLongMultiply longMultiply;
        assert(Ps2ArmAlu::decodeLongMultiply(0xE0810392u, &longMultiply));
        assert(!longMultiply.signedMultiply && !longMultiply.accumulate &&
            !longMultiply.setFlags && longMultiply.rdLo == 0 &&
            longMultiply.rdHi == 1 && longMultiply.rm == 2 &&
            longMultiply.rs == 3);

        uint32_t longState[16] = {};
        longState[2] = 0xFFFFFFFFu;
        longState[3] = 2u;
        uint32_t longCpsr = 0x30000013u;
        assert(Ps2ArmAlu::executeLongMultiply(0xE0810392u,
            longState, &longCpsr)); // UMULL r0,r1,r2,r3
        assert(longState[0] == 0xFFFFFFFEu && longState[1] == 1u &&
            longCpsr == 0x30000013u);

        longState[0] = 5u;
        longState[1] = 1u;
        assert(Ps2ArmAlu::executeLongMultiply(0xE0A10392u,
            longState, &longCpsr)); // UMLAL
        assert(longState[0] == 3u && longState[1] == 3u);

        longState[2] = 0xFFFFFFFFu;
        longState[3] = 2u;
        assert(Ps2ArmAlu::executeLongMultiply(0xE0C10392u,
            longState, &longCpsr)); // SMULL
        assert(longState[0] == 0xFFFFFFFEu &&
            longState[1] == 0xFFFFFFFFu);

        longState[0] = 2u;
        longState[1] = 0u;
        longState[2] = 0xFFFFFFFFu;
        longState[3] = 2u;
        assert(Ps2ArmAlu::executeLongMultiply(0xE0E10392u,
            longState, &longCpsr)); // SMLAL
        assert(longState[0] == 0u && longState[1] == 0u);

        longState[0] = 0xA5A55A5Au;
        longState[1] = 0x11223344u;
        longState[2] = 0u;
        longState[3] = 7u;
        longCpsr = 0x30000013u;
        assert(Ps2ArmAlu::executeLongMultiply(0xE0910392u,
            longState, &longCpsr)); // UMULLS -> high==0 => Z
        assert(longState[0] == 0u && longState[1] == 0u &&
            longCpsr == 0x70000013u);

        longState[0] = 0xA5A55A5Au;
        longState[1] = 0x11223344u;
        longState[2] = 3u;
        longState[3] = 4u;
        longCpsr = 0x00000013u; // EQ fails.
        assert(Ps2ArmAlu::executeLongMultiply(0x00810392u,
            longState, &longCpsr));
        assert(longState[0] == 0xA5A55A5Au &&
            longState[1] == 0x11223344u &&
            longCpsr == 0x00000013u);

        assert(!Ps2ArmAlu::decodeLongMultiply(0xE08F0392u,
            &longMultiply));
    }

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
    assert(Ps2ArmAlu::decode(0xE1B02181u, &decoded)); // shifted MOVS carries shifter C
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
    assert(Ps2ArmAlu::rejectionReason(0xE11190B8u) == Ps2ArmAlu::REJECT_NONE);
    assert(Ps2ArmAlu::rejectionReason(0xB18130BCu) == Ps2ArmAlu::REJECT_NONE);
    Ps2ArmAlu::inspect(0xE15C0002u, 0x02066E80u, &info);
    assert(info.kind == Ps2ArmAlu::KIND_DATA && info.writesFlags);
    assert(info.readMask == ((1u << 12) | (1u << 2)) && info.writeMask == 0);
    assert(Ps2ArmAlu::rejectionReason(0xE15C0002u) == Ps2ArmAlu::REJECT_NONE);
    Ps2ArmAlu::inspect(0xE0810293u, 0x02000000u, &info); // UMULL r0,r1,r3,r2
    assert(info.kind == Ps2ArmAlu::KIND_MULTIPLY);
    assert(info.readMask == ((1u << 2) | (1u << 3)));
    assert(info.writeMask == ((1u << 0) | (1u << 1)));
    assert(Ps2ArmAlu::rejectionReason(0x01A02001u) == Ps2ArmAlu::REJECT_NONE);

    Ps2ArmAlu::DecodedMultiBlockTransfer multiBlock;
    assert(Ps2ArmAlu::decodeMultiBlockTransfer(
        0xE8B0000Eu, &multiBlock)); // LDMIA r0!,{r1-r3}
    assert(multiBlock.load && multiBlock.writeBack &&
        !multiBlock.decrementBefore && multiBlock.rn == 0 &&
        multiBlock.count == 3 && multiBlock.registerMask == 0x000Eu);
    assert(Ps2ArmAlu::decodeMultiBlockTransfer(
        0xE92D4030u, &multiBlock)); // STMDB r13!,{r4,r5,r14}
    assert(!multiBlock.load && multiBlock.writeBack &&
        multiBlock.decrementBefore && multiBlock.rn == 13 &&
        multiBlock.count == 3 && multiBlock.registerMask == 0x4030u);
    assert(Ps2ArmAlu::decodeMultiBlockTransfer(
        0xE92D47F0u, &multiBlock)); // STMDB sp!,{r4-r10,lr}
    assert(!multiBlock.load && multiBlock.writeBack &&
        multiBlock.decrementBefore && multiBlock.rn == 13 &&
        multiBlock.count == 8 && multiBlock.registerMask == 0x47F0u);
    assert(!Ps2ArmAlu::decodeMultiBlockTransfer(
        0xE8BD8030u, &multiBlock)); // PC list excluded.
    assert(!Ps2ArmAlu::decodeMultiBlockTransfer(
        0xE8B00003u, &multiBlock)); // base-in-list excluded.

    {
        HalfwordMemory multiMemory = {};
        uint32_t multiRegs[16] = {};
        uint32_t multiCpsr = 0x00000013u;
        multiRegs[0] = 0x20u;
        writeWord(&multiMemory, 0x20u, 0x11111111u);
        writeWord(&multiMemory, 0x24u, 0x22222222u);
        writeWord(&multiMemory, 0x28u, 0x33333333u);
        assert(Ps2ArmAlu::executeMultiBlockTransfer(
            0xE8B0000Eu, multiRegs, &multiCpsr,
            &multiMemory, readWord, writeWord));
        assert(multiRegs[0] == 0x2Cu &&
            multiRegs[1] == 0x11111111u &&
            multiRegs[2] == 0x22222222u &&
            multiRegs[3] == 0x33333333u);

        std::memset(&multiMemory, 0, sizeof(multiMemory));
        std::memset(multiRegs, 0, sizeof(multiRegs));
        multiRegs[13] = 0x40u;
        multiRegs[4] = 0x44444444u;
        multiRegs[5] = 0x55555555u;
        multiRegs[14] = 0xEEEEEEEEu;
        assert(Ps2ArmAlu::executeMultiBlockTransfer(
            0xE92D4030u, multiRegs, &multiCpsr,
            &multiMemory, readWord, writeWord));
        uint32_t value = 0;
        assert(multiRegs[13] == 0x34u);
        assert(readWord(&multiMemory, 0x34u, &value) && value == 0x44444444u);
        assert(readWord(&multiMemory, 0x38u, &value) && value == 0x55555555u);
        assert(readWord(&multiMemory, 0x3Cu, &value) && value == 0xEEEEEEEEu);

        std::memset(&multiMemory, 0, sizeof(multiMemory));
        std::memset(multiRegs, 0, sizeof(multiRegs));
        multiRegs[13] = 0x100u;
        for (uint32_t reg = 4; reg <= 10; reg++)
            multiRegs[reg] = 0x10000000u | reg;
        multiRegs[14] = 0xEEEEEEEEu;
        assert(Ps2ArmAlu::executeMultiBlockTransfer(
            0xE92D47F0u, multiRegs, &multiCpsr,
            &multiMemory, readWord, writeWord));
        assert(multiRegs[13] == 0xE0u);
        for (uint32_t slot = 0; slot < 7; slot++)
        {
            assert(readWord(&multiMemory, 0xE0u + slot * 4u, &value) &&
                value == (0x10000000u | (slot + 4u)));
        }
        assert(readWord(&multiMemory, 0xFCu, &value) &&
            value == 0xEEEEEEEEu);
    }

    // CPSR register-transfer subset used by hot ARM7 IRQ code.
    {
        Ps2ArmAlu::DecodedPsrTransfer psr;
        assert(Ps2ArmAlu::decodePsrTransfer(0xE10F1000u, &psr));
        assert(!psr.write && psr.rd == 1 && psr.condition == 0xEu);

        uint32_t psrRegs[16] = {};
        uint32_t psrCpsr = 0x60000092u;
        assert(Ps2ArmAlu::executePsrTransfer(
            0xE10F1000u, psrRegs, &psrCpsr));
        assert(psrRegs[1] == 0x60000092u &&
            psrCpsr == 0x60000092u);

        psrRegs[2] = 0xA5A50092u;
        assert(Ps2ArmAlu::decodePsrTransfer(0xE121F002u, &psr));
        assert(psr.write && psr.fieldMask == 1 && psr.rm == 2);
        assert(Ps2ArmAlu::executePsrTransfer(
            0xE121F002u, psrRegs, &psrCpsr));
        assert(psrCpsr == 0x60000092u); // CPSR_c preserves upper bytes.

        // Privileged control writes may change mode in the portable model;
        // the live R5900 backend separately guards banked-mode transitions.
        psrRegs[2] = 0x00000013u;
        assert(Ps2ArmAlu::executePsrTransfer(
            0xE121F002u, psrRegs, &psrCpsr));
        assert((psrCpsr & 0xFFu) == 0x13u);

        // User mode may only write I/F/T bits in the low CPSR byte.
        psrCpsr = 0xA0000010u;
        psrRegs[2] = 0x000000FFu;
        assert(Ps2ArmAlu::executePsrTransfer(
            0xE121F002u, psrRegs, &psrCpsr));
        assert((psrCpsr & 0xFFu) == 0xF0u);

        assert(Ps2ArmAlu::rejectionReason(0xE10F1000u) ==
            Ps2ArmAlu::REJECT_NONE);
        assert(Ps2ArmAlu::rejectionReason(0xE121F002u) ==
            Ps2ArmAlu::REJECT_NONE);
        assert(!Ps2ArmAlu::decodePsrTransfer(0xE14F1000u, &psr)); // SPSR
        assert(!Ps2ArmAlu::decodePsrTransfer(0xE321F013u, &psr)); // immediate
    }

    Ps2ArmAlu::DecodedBranch branch;
    assert(Ps2ArmAlu::decodeBranch(0xBAFFFFFAu, &branch));
    assert(branch.condition == 0xB && !branch.link && branch.offset == -24);
    uint32_t nextPc = 0;
    assert(Ps2ArmAlu::executeBranch(0xBAFFFFFAu, 0x02066E90u,
        0x80000013u, &nextPc));
    assert(nextPc == 0x02066E80u); // LT taken
    assert(Ps2ArmAlu::executeBranch(0xBAFFFFFAu, 0x02066E90u,
        0x00000013u, &nextPc));
    assert(nextPc == 0x02066E94u); // LT not taken
    assert(Ps2ArmAlu::executeBranch(0xEA000006u, 0x0206094Cu,
        0x00000013u, &nextPc));
    assert(nextPc == 0x0206096Cu); // unconditional forward branch

    uint32_t branchRegs[16] = {};
    branchRegs[14] = 0xDEADBEEFu;
    assert(Ps2ArmAlu::decodeBranch(0xEBFFFFFAu, &branch));
    assert(branch.link && branch.offset == -24);
    assert(Ps2ArmAlu::executeBranch(0xEBFFFFFAu, 0x02066E90u,
        0x00000013u, &nextPc, branchRegs));
    assert(nextPc == 0x02066E80u && branchRegs[14] == 0x02066E94u);

    branchRegs[14] = 0x12345678u;
    assert(Ps2ArmAlu::executeBranch(0x0BFFFFFAu, 0x02066E90u,
        0x00000013u, &nextPc, branchRegs)); // BL.EQ fails
    assert(nextPc == 0x02066E94u && branchRegs[14] == 0x12345678u);

    assert(!Ps2ArmAlu::decodeBranch(0xFAFFFFFAu, &branch)); // BLX immediate
    assert(Ps2ArmAlu::rejectionReason(0xBAFFFFFAu) == Ps2ArmAlu::REJECT_NONE);
    assert(Ps2ArmAlu::rejectionReason(0xEBFFFFFAu) == Ps2ArmAlu::REJECT_NONE);

    Ps2ArmAlu::DecodedSingleBlockTransfer blockTransfer;
    assert(Ps2ArmAlu::decodeSingleBlockTransfer(0xB8B00004u, &blockTransfer));
    assert(blockTransfer.condition == 0xBu && blockTransfer.load &&
        blockTransfer.writeBack && blockTransfer.rn == 0 &&
        blockTransfer.reg == 2);
    assert(Ps2ArmAlu::decodeSingleBlockTransfer(0xB8A10004u, &blockTransfer));
    assert(!blockTransfer.load && blockTransfer.writeBack &&
        blockTransfer.rn == 1 && blockTransfer.reg == 2);
    assert(!Ps2ArmAlu::decodeSingleBlockTransfer(0xE8B0000Cu, &blockTransfer));
    assert(!Ps2ArmAlu::decodeSingleBlockTransfer(0xE8B08000u, &blockTransfer));
    assert(!Ps2ArmAlu::decodeSingleBlockTransfer(0xE8F00004u, &blockTransfer));
    assert(Ps2ArmAlu::rejectionReason(0xB8B00004u) ==
        Ps2ArmAlu::REJECT_NONE);
    assert(Ps2ArmAlu::rejectionReason(0xB8A10004u) ==
        Ps2ArmAlu::REJECT_NONE);

    HalfwordMemory blockMemory = {};
    assert(writeWord(&blockMemory, 0x40u, 0x12345678u));
    uint32_t blockRegs[16] = {};
    blockRegs[0] = 0x40u;
    uint32_t blockCpsr = 0x80000000u; // LT true: N != V.
    assert(Ps2ArmAlu::executeSingleBlockTransfer(0xB8B00004u, blockRegs,
        &blockCpsr, &blockMemory, readWord, writeWord));
    assert(blockRegs[2] == 0x12345678u && blockRegs[0] == 0x44u);

    blockRegs[1] = 0x48u;
    blockRegs[2] = 0x89ABCDEFu;
    assert(Ps2ArmAlu::executeSingleBlockTransfer(0xB8A10004u, blockRegs,
        &blockCpsr, &blockMemory, readWord, writeWord));
    uint32_t blockStored = 0;
    assert(readWord(&blockMemory, 0x48u, &blockStored));
    assert(blockStored == 0x89ABCDEFu && blockRegs[1] == 0x4Cu);

    // Failed LT condition is a complete no-op.
    blockRegs[0] = 0x60u;
    blockRegs[2] = 0xAAAAAAAAu;
    blockCpsr = 0;
    assert(Ps2ArmAlu::executeSingleBlockTransfer(0xB8B00004u, blockRegs,
        &blockCpsr, &blockMemory, readWord, writeWord));
    assert(blockRegs[0] == 0x60u && blockRegs[2] == 0xAAAAAAAAu);

    Ps2ArmAlu::DecodedBranchExchange bx;
    assert(Ps2ArmAlu::decodeBranchExchange(0xE12FFF1Eu, &bx));
    assert(bx.condition == 0xEu && bx.rm == 14);
    assert(!Ps2ArmAlu::decodeBranchExchange(0xE12FFF1Fu, &bx));
    assert(!Ps2ArmAlu::decodeBranchExchange(0xF12FFF1Eu, &bx));
    assert(Ps2ArmAlu::rejectionReason(0xE12FFF1Eu) ==
        Ps2ArmAlu::REJECT_NONE);

    uint32_t bxRegs[16] = {};
    uint32_t bxCpsr = 0x00000013u;
    uint32_t bxNextPc = 0x02000004u;
    bxRegs[14] = 0x02000101u;
    assert(Ps2ArmAlu::executeBranchExchange(0xE12FFF1Eu, bxRegs,
        &bxCpsr, &bxNextPc));
    assert(bxNextPc == 0x02000100u && (bxCpsr & (1u << 5)));

    bxRegs[14] = 0x02000200u;
    assert(Ps2ArmAlu::executeBranchExchange(0xE12FFF1Eu, bxRegs,
        &bxCpsr, &bxNextPc));
    assert(bxNextPc == 0x02000200u && !(bxCpsr & (1u << 5)));

    // Failed condition leaves both target and T bit untouched.
    bxRegs[14] = 0x02000301u;
    bxCpsr = 0x00000013u; // Z clear, EQ fails.
    bxNextPc = 0x02000004u;
    assert(Ps2ArmAlu::executeBranchExchange(0x012FFF1Eu, bxRegs,
        &bxCpsr, &bxNextPc));
    assert(bxNextPc == 0x02000004u && bxCpsr == 0x00000013u);

    Ps2ArmAlu::DecodedHalfwordTransfer transfer;
    assert(Ps2ArmAlu::decodeHalfwordTransfer(0xE11190B8u, &transfer));
    assert(transfer.load && !transfer.immediate && transfer.preIndex &&
        !transfer.addOffset && !transfer.writeBack && transfer.rn == 1 &&
        transfer.rd == 9 && transfer.rm == 8);
    assert(Ps2ArmAlu::decodeHalfwordTransfer(0xB19030BCu, &transfer));
    assert(transfer.load && transfer.condition == 0xBu && transfer.rm == 12);
    assert(Ps2ArmAlu::decodeHalfwordTransfer(0xE1F120B4u, &transfer));
    assert(transfer.load && transfer.immediate && transfer.writeBack &&
        transfer.immediateOffset == 4);
    assert(!Ps2ArmAlu::decodeHalfwordTransfer(0xE1F110B4u, &transfer));
    assert(!Ps2ArmAlu::decodeHalfwordTransfer(0xE1D1F0B4u, &transfer));

    // Signed extra loads share the halfword encoding family. LDRSB/LDRSH are
    // load-only; the corresponding S=1 store encodings remain outside the
    // admitted subset (doubleword/reserved forms).
    assert(Ps2ArmAlu::decodeHalfwordTransfer(0xE1D120D0u, &transfer));
    assert(transfer.load && transfer.signedLoad && transfer.byteLoad &&
        transfer.immediate && transfer.rn == 1 && transfer.rd == 2);
    assert(Ps2ArmAlu::decodeHalfwordTransfer(0xE1D120F0u, &transfer));
    assert(transfer.load && transfer.signedLoad && !transfer.byteLoad &&
        transfer.immediate && transfer.rn == 1 && transfer.rd == 2);
    assert(Ps2ArmAlu::decodeHalfwordTransfer(0xE19120D3u, &transfer));
    assert(transfer.load && transfer.signedLoad && transfer.byteLoad &&
        !transfer.immediate && transfer.rm == 3);
    assert(!Ps2ArmAlu::decodeHalfwordTransfer(0xE1C120D0u, &transfer));
    assert(!Ps2ArmAlu::decodeHalfwordTransfer(0xE1C120F0u, &transfer));
    assert(Ps2ArmAlu::rejectionReason(0xE1D120D0u) ==
        Ps2ArmAlu::REJECT_NONE);
    assert(Ps2ArmAlu::rejectionReason(0xE1D120F0u) ==
        Ps2ArmAlu::REJECT_NONE);

    HalfwordMemory halfwordMemory = {};
    halfwordMemory.bytes[0x44] = 0xCD;
    halfwordMemory.bytes[0x45] = 0xAB;
    uint32_t transferRegs[16] = {};
    transferRegs[1] = 0x48;
    transferRegs[8] = 4;
    cpsr = 0;
    assert(Ps2ArmAlu::executeHalfwordTransfer(0xE11190B8u, transferRegs,
        &cpsr, &halfwordMemory, readHalfword, writeHalfword));
    assert(transferRegs[9] == 0xABCDu && transferRegs[1] == 0x48u);

    // Failed conditions are memory-free no-ops; taken post-index forms use
    // the old address and apply writeback only after a successful transfer.
    transferRegs[3] = 0x5678u;
    transferRegs[1] = 0x80u;
    transferRegs[2] = 6u;
    cpsr = 0;
    assert(Ps2ArmAlu::executeHalfwordTransfer(0x008130B2u, transferRegs,
        &cpsr, &halfwordMemory, readHalfword, writeHalfword)); // STRH.EQ skipped
    assert(transferRegs[1] == 0x80u && halfwordMemory.bytes[0x80] == 0);
    cpsr = 0x40000000u;
    assert(Ps2ArmAlu::executeHalfwordTransfer(0x008130B2u, transferRegs,
        &cpsr, &halfwordMemory, readHalfword, writeHalfword));
    assert(transferRegs[1] == 0x86u && halfwordMemory.bytes[0x80] == 0x78 &&
        halfwordMemory.bytes[0x81] == 0x56);

    // Differential address/writeback sweep across immediate and register
    // offsets, add/subtract and pre/post-index forms.
    for (int test = 0; test < 10000; test++)
    {
        bool immediate = (nextRandom() & 1u) != 0;
        bool preIndex = (nextRandom() & 1u) != 0;
        bool addOffset = (nextRandom() & 1u) != 0;
        bool writeBack = preIndex && ((nextRandom() & 1u) != 0);
        uint32_t offset = nextRandom() & 0x3Fu;
        uint32_t base = addOffset ? 0x100u : 0x180u;
        uint32_t opcode = 0xE00000B0u | (1u << 20) | (1u << 16) |
            (2u << 12);
        if (immediate)
            opcode |= (1u << 22) | ((offset & 0xF0u) << 4) | (offset & 0xFu);
        else
            opcode |= 3u;
        if (preIndex) opcode |= 1u << 24;
        if (addOffset) opcode |= 1u << 23;
        if (writeBack) opcode |= 1u << 21;

        uint32_t state[16] = {};
        state[1] = base;
        state[3] = offset;
        uint32_t address = preIndex
            ? (addOffset ? base + offset : base - offset) : base;
        halfwordMemory.bytes[address] = static_cast<uint8_t>(test);
        halfwordMemory.bytes[address + 1] = static_cast<uint8_t>(test >> 8);
        assert(Ps2ArmAlu::executeHalfwordTransfer(opcode, state, &cpsr,
            &halfwordMemory, readHalfword, writeHalfword));
        assert(state[2] == static_cast<uint16_t>(test));
        assert(state[1] == ((writeBack || !preIndex)
            ? (addOffset ? base + offset : base - offset) : base));
    }

    Ps2ArmAlu::DecodedByteTransfer byteTransfer;
    assert(Ps2ArmAlu::decodeByteTransfer(0xE4C14001u, &byteTransfer));
    assert(!byteTransfer.load && !byteTransfer.preIndex &&
        byteTransfer.writeBack && byteTransfer.rn == 1 &&
        byteTransfer.rd == 4 && byteTransfer.immediateOffset == 1);
    uint32_t byteRegs[16] = {};
    byteRegs[1] = 0x40u;
    byteRegs[4] = 0x1234ABCDu;
    cpsr = 0;
    assert(Ps2ArmAlu::executeByteTransfer(0xE4C14001u, byteRegs, &cpsr,
        &halfwordMemory, readByte, writeByte));
    assert(halfwordMemory.bytes[0x40] == 0xCDu && byteRegs[1] == 0x41u);

    Ps2ArmAlu::DecodedLiteralLoad literalLoad;
    assert(Ps2ArmAlu::decodeLiteralLoad(0xE59F201Cu, &literalLoad));
    assert(literalLoad.addOffset && literalLoad.rd == 2 &&
        literalLoad.immediateOffset == 0x1Cu);
    HalfwordMemory literalMemory = {};
    assert(writeWord(&literalMemory, 0x124u, 0x89ABCDEFu));
    uint32_t literalRegs[16] = {};
    assert(Ps2ArmAlu::executeLiteralLoad(0xE59F201Cu, 0x100u,
        literalRegs, &cpsr, &literalMemory, readWord));
    assert(literalRegs[2] == 0x89ABCDEFu);
    assert(Ps2ArmAlu::rejectionReason(0xE59F201Cu) ==
        Ps2ArmAlu::REJECT_NONE);
    assert(Ps2ArmAlu::rejectionReason(0xE4C14001u) ==
        Ps2ArmAlu::REJECT_NONE);

    Ps2ArmAlu::DecodedSingleTransfer single;
    assert(Ps2ArmAlu::decodeSingleTransfer(0xE5912004u, &single)); // LDR r2,[r1,#4]
    assert(single.load && !single.registerOffset && single.preIndex &&
        single.addOffset && !single.writeBack && single.rn == 1 &&
        single.rd == 2 && single.immediateOffset == 4);
    assert(Ps2ArmAlu::decodeSingleTransfer(0xE7A12083u, &single)); // STR r2,[r1,r3,LSL#1]!
    assert(!single.load && single.registerOffset && single.writeBack &&
        single.rm == 3 && single.shiftType == 0 && single.shiftAmount == 1);
    assert(!Ps2ArmAlu::decodeSingleTransfer(0xE5D12000u, &single)); // byte transfer
    assert(!Ps2ArmAlu::decodeSingleTransfer(0xE591F000u, &single)); // PC destination
    assert(!Ps2ArmAlu::decodeSingleTransfer(0xE4911004u, &single)); // post-index Rn==Rd
    assert(Ps2ArmAlu::rejectionReason(0xE5912004u) == Ps2ArmAlu::REJECT_NONE);

    // Word transfer semantics: immediate/register offsets, writeback and
    // conditional no-op behavior.
    HalfwordMemory wordMemory = {};
    wordMemory.bytes[0x104] = 0x78;
    wordMemory.bytes[0x105] = 0x56;
    wordMemory.bytes[0x106] = 0x34;
    wordMemory.bytes[0x107] = 0x12;
    uint32_t wordRegs[16] = {};
    wordRegs[1] = 0x100;
    cpsr = 0;
    assert(Ps2ArmAlu::executeSingleTransfer(0xE5912004u, wordRegs, &cpsr,
        &wordMemory, readWord, writeWord));
    assert(wordRegs[2] == 0x12345678u && wordRegs[1] == 0x100u);

    wordRegs[2] = 0x89ABCDEFu;
    wordRegs[3] = 2u;
    assert(Ps2ArmAlu::executeSingleTransfer(0xE7A12083u, wordRegs, &cpsr,
        &wordMemory, readWord, writeWord));
    assert(wordRegs[1] == 0x104u);
    uint32_t storedWord = 0;
    assert(readWord(&wordMemory, 0x104u, &storedWord));
    assert(storedWord == 0x89ABCDEFu);

    wordRegs[1] = 0x120u;
    wordRegs[2] = 0xCAFEBABEu;
    cpsr = 0;
    assert(Ps2ArmAlu::executeSingleTransfer(0x05812004u, wordRegs, &cpsr,
        &wordMemory, readWord, writeWord)); // STR.EQ skipped
    assert(wordRegs[1] == 0x120u);
    uint32_t untouched = 0;
    assert(readWord(&wordMemory, 0x124u, &untouched));
    assert(untouched == 0);

    // Randomized addressing/writeback sweep for LDR word.
    for (int test = 0; test < 10000; test++)
    {
        bool registerOffset = (nextRandom() & 1u) != 0;
        bool preIndex = (nextRandom() & 1u) != 0;
        bool addOffset = (nextRandom() & 1u) != 0;
        bool writeBack = preIndex && ((nextRandom() & 1u) != 0);
        uint32_t offset = (nextRandom() & 0x1Cu);
        uint32_t base = addOffset ? 0x100u : 0x180u;
        uint32_t opcode = 0xE4000000u | (1u << 20) | (1u << 16) |
            (2u << 12);
        if (registerOffset)
        {
            opcode |= 1u << 25;
            opcode |= 3u; // r3, LSL #0
        }
        else
        {
            opcode |= offset;
        }
        if (preIndex) opcode |= 1u << 24;
        if (addOffset) opcode |= 1u << 23;
        if (writeBack) opcode |= 1u << 21;

        uint32_t state[16] = {};
        state[1] = base;
        state[3] = offset;
        uint32_t address = preIndex
            ? (addOffset ? base + offset : base - offset) : base;
        uint32_t value = nextRandom();
        assert(writeWord(&wordMemory, address, value));
        assert(Ps2ArmAlu::executeSingleTransfer(opcode, state, &cpsr,
            &wordMemory, readWord, writeWord));
        assert(state[2] == value);
        assert(state[1] == ((writeBack || !preIndex)
            ? (addOffset ? base + offset : base - offset) : base));
    }

    char text[96] = {};
    Ps2ArmAlu::format(0xE0098E18u, 0x020672F8u, text, sizeof(text));
    assert(std::strstr(text, "AND.AL") && std::strstr(text, "LSL r14"));
    Ps2ArmAlu::format(0xE11190B8u, 0x020672F0u, text, sizeof(text));
    assert(std::strstr(text, "LDRH.AL") && std::strstr(text, "[r1,-r8]"));

    // Hot ARM9 copy loop: LDMIA r0!,{r2} / STMIA r1!,{r2}. The profiler
    // must report block-transfer semantics and include base writeback in the
    // dependency masks instead of formatting these as bogus r255 LDR/STR.
    Ps2ArmAlu::inspect(0xB8B00004u, 0x0210F490u, &info);
    assert(info.kind == Ps2ArmAlu::KIND_MULTI_MEMORY);
    assert(info.rn == 0 && info.readMask == (1u << 0));
    assert(info.writeMask == ((1u << 0) | (1u << 2)));
    Ps2ArmAlu::format(0xB8B00004u, 0x0210F490u, text, sizeof(text));
    assert(std::strstr(text, "LDMIA.LT") && std::strstr(text, "r0!") &&
        std::strstr(text, "{r2}"));

    Ps2ArmAlu::inspect(0xB8A10004u, 0x0210F494u, &info);
    assert(info.kind == Ps2ArmAlu::KIND_MULTI_MEMORY);
    assert(info.readMask == ((1u << 1) | (1u << 2)));
    assert(info.writeMask == (1u << 1));
    Ps2ArmAlu::format(0xB8A10004u, 0x0210F494u, text, sizeof(text));
    assert(std::strstr(text, "STMIA.LT") && std::strstr(text, "r1!") &&
        std::strstr(text, "{r2}"));

    return 0;
}
