#include <cassert>
#include <cstdint>
#include <cstring>

#include "../src/ps2/arm_alu.h"

namespace
{
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

    Ps2ArmAlu::Decoded decoded;
    assert(!Ps2ArmAlu::decode(0x11A02001u, &decoded)); // condition != AL
    assert(!Ps2ArmAlu::decode(0xE1B02001u, &decoded)); // writes flags
    assert(!Ps2ArmAlu::decode(0xE1A02061u, &decoded)); // RRX needs carry
    assert(Ps2ArmAlu::decode(0xE1A02111u, &decoded));  // register-controlled LSL
    assert(decoded.registerShift && decoded.rs == 1);
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
    Ps2ArmAlu::inspect(0xE0810293u, 0x02000000u, &info); // UMULL r0,r1,r3,r2
    assert(info.kind == Ps2ArmAlu::KIND_MULTIPLY);
    assert(info.readMask == ((1u << 2) | (1u << 3)));
    assert(info.writeMask == ((1u << 0) | (1u << 1)));
    assert(Ps2ArmAlu::rejectionReason(0xBAFFFFFAu) == Ps2ArmAlu::REJECT_CONDITION);

    char text[96] = {};
    Ps2ArmAlu::format(0xE0098E18u, 0x020672F8u, text, sizeof(text));
    assert(std::strstr(text, "AND.AL") && std::strstr(text, "LSL r14"));
    Ps2ArmAlu::format(0xE11190B8u, 0x020672F0u, text, sizeof(text));
    assert(std::strstr(text, "LDRH.AL") && std::strstr(text, "[r1,-r8]"));

    return 0;
}
