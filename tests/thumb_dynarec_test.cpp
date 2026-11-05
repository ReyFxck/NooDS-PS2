#include <cassert>
#include <cstdint>
#include <cstdio>

#include "../src/ps2/arm_alu.h"
#include "../src/ps2/thumb_alu.h"

static void requireArm(uint16_t thumb, uint32_t pc,
    uint32_t expectedArm)
{
    Ps2Thumb::Decoded decoded;
    assert(Ps2Thumb::decode(thumb, pc, &decoded));
    assert(decoded.kind == Ps2Thumb::ARM_ALU);
    assert(decoded.armOpcode == expectedArm);

    Ps2ArmAlu::InstructionInfo info;
    Ps2ArmAlu::inspect(decoded.armOpcode, pc, &info);
    assert(info.kind == Ps2ArmAlu::KIND_DATA);
}

int main()
{
    // Exact ARM7 BIOS hot loop observed in the PS2 profile.
    {
        Ps2Thumb::Decoded d;
        assert(Ps2Thumb::decode(0x4909u, 0x00002C5Cu, &d));
        assert(d.kind == Ps2Thumb::LITERAL_LOAD);
        assert(d.rd == 1);
        assert(d.target == 0x00002C84u);
        assert(d.writeMask == (1u << 1));
        assert(d.minCycles == 3 && d.maxCycles == 3);

        requireArm(0x4288u, 0x00002C5Eu, 0xE1500001u); // CMP r0,r1

        assert(Ps2Thumb::decode(0xD302u, 0x00002C60u, &d));
        assert(d.kind == Ps2Thumb::CONDITIONAL_BRANCH);
        assert(d.condition == 3u); // CC/LO
        assert(d.target == 0x00002C68u);
        assert(d.readsFlags && d.terminal);
        assert(d.minCycles == 1 && d.maxCycles == 3);

        requireArm(0x2000u, 0x00002C6Eu, 0xE3B00000u); // MOVS r0,#0
    }

    // Immediate shifts retain Thumb's special LSR/ASR #0 == #32 semantics
    // because ARM uses the same immediate-shift encoding.
    requireArm(0x0008u, 0x1000u, 0xE1B00001u); // LSLS r0,r1,#0
    requireArm(0x0808u, 0x1000u, 0xE1B00021u); // LSRS r0,r1,#32
    requireArm(0x1008u, 0x1000u, 0xE1B00041u); // ASRS r0,r1,#32

    // Register and immediate arithmetic.
    requireArm(0x1888u, 0x1000u, 0xE0910002u); // ADDS r0,r1,r2
    requireArm(0x1A88u, 0x1000u, 0xE0510002u); // SUBS r0,r1,r2
    requireArm(0x1C88u, 0x1000u, 0xE2910002u); // ADDS r0,r1,#2
    requireArm(0x1E88u, 0x1000u, 0xE2510002u); // SUBS r0,r1,#2

    requireArm(0x3001u, 0x1000u, 0xE2900001u); // ADDS r0,#1
    requireArm(0x3801u, 0x1000u, 0xE2500001u); // SUBS r0,#1

    // Carry-input operations now map onto the newly completed ARM backend.
    requireArm(0x4148u, 0x1000u, 0xE0B00001u); // ADCS r0,r1
    requireArm(0x4188u, 0x1000u, 0xE0D00001u); // SBCS r0,r1

    // High-register ALU and BX stay read-only/terminal in the first live
    // backend. PC-writing ADD/MOV and ARM7-invalid BLX remain rejected.
    requireArm(0x4488u, 0x1000u, 0xE0888001u); // ADD r8,r1
    requireArm(0x4580u, 0x1000u, 0xE1580000u); // CMP r8,r0
    requireArm(0x4680u, 0x1000u, 0xE1A08000u); // MOV r8,r0
    requireArm(0x4640u, 0x1000u, 0xE1A00008u); // MOV r0,r8
    {
        Ps2Thumb::Decoded bx;
        assert(Ps2Thumb::decode(0x4740u, 0x1000u, &bx)); // BX r8
        assert(bx.kind == Ps2Thumb::BRANCH_EXCHANGE);
        assert(bx.armOpcode == 0xE12FFF18u);
        assert(bx.readMask == (1u << 8));
        assert(bx.terminal && bx.minCycles == 3 && bx.maxCycles == 3);
    }

    // Initial subset intentionally rejects operations whose Thumb timing
    // differs from the ARM emitter or that mutate memory/stack state.
    Ps2Thumb::Decoded d;
    assert(!Ps2Thumb::decode(0x4088u, 0x1000u, &d)); // LSL r0,r1 (reg)
    assert(!Ps2Thumb::decode(0x4348u, 0x1000u, &d)); // MUL r0,r1
    assert(!Ps2Thumb::decode(0x4788u, 0x1000u, &d)); // BLX r1 (ARM9-only)
    assert(!Ps2Thumb::decode(0x46F8u, 0x1000u, &d)); // MOV pc,r15/PC write
    assert(!Ps2Thumb::decode(0xB500u, 0x1000u, &d)); // PUSH
    assert(!Ps2Thumb::decode(0x6008u, 0x1000u, &d)); // STR

    // Unconditional Thumb B uses PC+4 and signed halfword offset.
    assert(Ps2Thumb::decode(0xE7FEu, 0x2000u, &d));
    assert(d.kind == Ps2Thumb::BRANCH);
    assert(d.target == 0x2000u); // B . self-loop
    assert(d.minCycles == 3 && d.maxCycles == 3);

    std::puts("thumb_dynarec_test: OK");
    return 0;
}
