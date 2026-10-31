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

    Ps2ArmAlu::Decoded decoded;
    assert(!Ps2ArmAlu::decode(0x11A02001u, &decoded)); // condition != AL
    assert(!Ps2ArmAlu::decode(0xE1B02001u, &decoded)); // writes flags
    assert(!Ps2ArmAlu::decode(0xE1A02061u, &decoded)); // RRX needs carry
    assert(!Ps2ArmAlu::decode(0xE1A02111u, &decoded)); // register shift
    assert(!Ps2ArmAlu::decode(0xE1A0F001u, &decoded)); // writes PC
    assert(!Ps2ArmAlu::decode(0xE1A0200Fu, &decoded)); // reads PC

    return 0;
}
