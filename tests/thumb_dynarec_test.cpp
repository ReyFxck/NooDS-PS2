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

    // Register shifts reuse ARM's exact carry semantics but mark the emitter
    // as one-cycle Thumb operations. MUL uses old Rd as ARM Rs so ARM7's
    // variable multiply timing is based on the same value as mulDpT().
    {
        Ps2Thumb::Decoded d;
        assert(Ps2Thumb::decode(0x4088u, 0x1000u, &d)); // LSL r0,r1
        assert(d.kind == Ps2Thumb::ARM_ALU);
        assert(d.armOpcode == 0xE1B00110u);
        assert(d.oneCycleRegisterShift);
        assert(d.readMask == ((1u << 0) | (1u << 1)));
        assert(d.writeMask == (1u << 0));

        assert(Ps2Thumb::decode(0x40C8u, 0x1000u, &d)); // LSR r0,r1
        assert(d.armOpcode == 0xE1B00130u && d.oneCycleRegisterShift);

        assert(Ps2Thumb::decode(0x4108u, 0x1000u, &d)); // ASR r0,r1
        assert(d.armOpcode == 0xE1B00150u && d.oneCycleRegisterShift);

        assert(Ps2Thumb::decode(0x41C8u, 0x1000u, &d)); // ROR r0,r1
        assert(d.armOpcode == 0xE1B00170u && d.oneCycleRegisterShift);

        assert(Ps2Thumb::decode(0x4348u, 0x1000u, &d)); // MUL r0,r1
        assert(d.kind == Ps2Thumb::ARM_MULTIPLY);
        assert(d.armOpcode == 0xE0100091u);
        assert(d.readMask == ((1u << 0) | (1u << 1)));
        assert(d.writeMask == (1u << 0));
        assert(d.minCycles == 2 && d.maxCycles == 5);
    }

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

        // Thumb BLX-register is ARM9-only at compile time, but decoding is
        // CPU-neutral. The ARM BLX emitter is reused with a pc-1 adjustment
        // so LR becomes Thumb pc+3 exactly.
        assert(Ps2Thumb::decode(0x4788u, 0x1000u, &bx)); // BLX r1
        assert(bx.kind == Ps2Thumb::BRANCH_LINK_EXCHANGE);
        assert(bx.armOpcode == 0xE12FFF31u);
        assert(bx.readMask == (1u << 1));
        assert(bx.writeMask == (1u << 14));
        assert(bx.terminal && bx.minCycles == 3 && bx.maxCycles == 3);
    }

    // Register-offset signed/unsigned loads also map onto the existing
    // ARM memory backend with identical ARM7 timing.
    {
        Ps2Thumb::Decoded load;
        assert(Ps2Thumb::decode(0x5688u, 0x1000u, &load)); // LDRSB r0,[r1,r2]
        assert(load.kind == Ps2Thumb::ARM_LOAD);
        assert(load.armOpcode == 0xE19100D2u);

        assert(Ps2Thumb::decode(0x5963u, 0x1000u, &load)); // LDR r3,[r4,r5]
        assert(load.armOpcode == 0xE7943005u);

        assert(Ps2Thumb::decode(0x5A3Eu, 0x1000u, &load)); // LDRH r6,[r7,r0]
        assert(load.armOpcode == 0xE19760B0u);

        assert(Ps2Thumb::decode(0x5CD1u, 0x1000u, &load)); // LDRB r1,[r2,r3]
        assert(load.armOpcode == 0xE7D21003u);

        assert(Ps2Thumb::decode(0x5FACu, 0x1000u, &load)); // LDRSH r4,[r5,r6]
        assert(load.armOpcode == 0xE19540F6u);
        assert(load.minCycles == 3 && load.maxCycles == 3);
    }

    // Read-only load subset maps onto the already differential-gated ARM
    // memory backend with ARM7-equivalent timing.
    {
        Ps2Thumb::Decoded load;
        assert(Ps2Thumb::decode(0x6848u, 0x1000u, &load)); // LDR r0,[r1,#4]
        assert(load.kind == Ps2Thumb::ARM_LOAD);
        assert(load.armOpcode == 0xE5910004u);
        assert(load.readMask == (1u << 1) &&
            load.writeMask == (1u << 0));

        assert(Ps2Thumb::decode(0x795Au, 0x1000u, &load)); // LDRB r2,[r3,#5]
        assert(load.armOpcode == 0xE5D32005u);

        assert(Ps2Thumb::decode(0x88ECu, 0x1000u, &load)); // LDRH r4,[r5,#6]
        assert(load.armOpcode == 0xE1D540B6u);

        assert(Ps2Thumb::decode(0x9E03u, 0x1000u, &load)); // LDR r6,[sp,#12]
        assert(load.armOpcode == 0xE59D600Cu);
        assert(load.readMask == (1u << 13));
        assert(load.minCycles == 3 && load.maxCycles == 3);
    }

    // Address-generation forms are direct Thumb semantics rather than
    // synthetic ARM PC reads. PC is Thumb PC+4 aligned; SP forms preserve
    // flags and use a signed immediate addend.
    {
        Ps2Thumb::Decoded addr;
        assert(Ps2Thumb::decode(0xA001u, 0x1002u, &addr)); // ADD r0,pc,#4
        assert(addr.kind == Ps2Thumb::ADDRESS_ADD);
        assert(addr.rd == 0 && addr.baseReg == 0xFFu);
        assert(addr.target == 0x1008u);
        assert(addr.writeMask == (1u << 0) && addr.readMask == 0);

        assert(Ps2Thumb::decode(0xAA03u, 0x1000u, &addr)); // ADD r2,sp,#12
        assert(addr.kind == Ps2Thumb::ADDRESS_ADD);
        assert(addr.rd == 2 && addr.baseReg == 13 && addr.addend == 12);
        assert(addr.readMask == (1u << 13) &&
            addr.writeMask == (1u << 2));

        assert(Ps2Thumb::decode(0xB004u, 0x1000u, &addr)); // ADD sp,#16
        assert(addr.rd == 13 && addr.baseReg == 13 && addr.addend == 16);
        assert(addr.readMask == (1u << 13) &&
            addr.writeMask == (1u << 13));

        assert(Ps2Thumb::decode(0xB084u, 0x1000u, &addr)); // SUB sp,#16
        assert(addr.addend == -16);
    }

    // Read-only stack/multiple loads reuse the gated ARM7 LDM backend.
    {
        Ps2Thumb::Decoded load;
        assert(Ps2Thumb::decode(0xBC85u, 0x1000u, &load)); // POP {r0,r2,r7}
        assert(load.kind == Ps2Thumb::ARM_LOAD);
        assert(load.armOpcode == 0xE8BD0085u);
        assert(load.readMask == (1u << 13));
        assert(load.writeMask ==
            static_cast<uint16_t>(0x0085u | (1u << 13)));
        assert(load.minCycles == 5 && load.maxCycles == 5);

        assert(Ps2Thumb::decode(0xCB13u, 0x1000u, &load)); // LDMIA r3!,{r0,r1,r4}
        assert(load.kind == Ps2Thumb::ARM_LOAD);
        assert(load.armOpcode == 0xE8B30013u);
        assert(load.readMask == (1u << 3));
        assert(load.writeMask ==
            static_cast<uint16_t>(0x0013u | (1u << 3)));
        assert(load.minCycles == 5 && load.maxCycles == 5);

        assert(Ps2Thumb::decode(0xBD85u, 0x1000u, &load)); // POP {r0,r2,r7,pc}
        assert(load.kind == Ps2Thumb::POP_PC);
        assert(load.armOpcode == 0xE8BD0085u);
        assert(load.readMask == (1u << 13));
        assert(load.writeMask ==
            static_cast<uint16_t>(0x0085u | (1u << 13)));
        assert(load.terminal && load.minCycles == 8 && load.maxCycles == 8);

        assert(Ps2Thumb::decode(0xBD00u, 0x1000u, &load)); // POP {pc}
        assert(load.kind == Ps2Thumb::POP_PC && load.armOpcode == 0);
        assert(load.writeMask == (1u << 13));
        assert(load.terminal && load.minCycles == 5 && load.maxCycles == 5);

        assert(!Ps2Thumb::decode(0xCB08u, 0x1000u, &load)); // base in list
        assert(!Ps2Thumb::decode(0xC313u, 0x1000u, &load)); // STMIA
    }

    // Scalar stores now translate to the transactional ARM backend. ARM9
    // admits them live only after journal differential verification; ARM7
    // compilation still blocks stores at the CPU-specific gate.
    Ps2Thumb::Decoded d;
    assert(!Ps2Thumb::decode(0x46F8u, 0x1000u, &d)); // MOV pc,r15/PC write
    assert(!Ps2Thumb::decode(0xB500u, 0x1000u, &d)); // PUSH stays later

    assert(Ps2Thumb::decode(0x5008u, 0x1000u, &d)); // STR r0,[r1,r0]
    assert(d.kind == Ps2Thumb::ARM_STORE);
    assert(d.armOpcode == 0xE7810000u);
    assert(d.readMask == ((1u << 0) | (1u << 1)));
    assert(d.writeMask == 0 && d.minCycles == 2 && d.maxCycles == 2);

    assert(Ps2Thumb::decode(0x5248u, 0x1000u, &d)); // STRH r0,[r1,r1]
    assert(d.kind == Ps2Thumb::ARM_STORE);
    assert(d.armOpcode == 0xE18100B1u);

    assert(Ps2Thumb::decode(0x5488u, 0x1000u, &d)); // STRB r0,[r1,r2]
    assert(d.kind == Ps2Thumb::ARM_STORE);
    assert(d.armOpcode == 0xE7C10002u);

    assert(Ps2Thumb::decode(0x6048u, 0x1000u, &d)); // STR r0,[r1,#4]
    assert(d.kind == Ps2Thumb::ARM_STORE);
    assert(d.armOpcode == 0xE5810004u);

    assert(Ps2Thumb::decode(0x7148u, 0x1000u, &d)); // STRB r0,[r1,#5]
    assert(d.kind == Ps2Thumb::ARM_STORE);
    assert(d.armOpcode == 0xE5C10005u);

    assert(Ps2Thumb::decode(0x8088u, 0x1000u, &d)); // STRH r0,[r1,#4]
    assert(d.kind == Ps2Thumb::ARM_STORE);
    assert(d.armOpcode == 0xE1C100B4u);

    assert(Ps2Thumb::decode(0x9003u, 0x1000u, &d)); // STR r0,[sp,#12]
    assert(d.kind == Ps2Thumb::ARM_STORE);
    assert(d.armOpcode == 0xE58D000Cu);
    assert(d.readMask == ((1u << 13) | (1u << 0)));

    // ARM7 Thumb long BL is two independently decodable halves. The
    // setup half writes the signed high target to LR; the suffix reads LR,
    // branches, and rewrites LR with the Thumb return address.
    {
        Ps2Thumb::Decoded bl;
        assert(Ps2Thumb::decode(0xF001u, 0x2000u, &bl));
        assert(bl.kind == Ps2Thumb::LONG_BRANCH_SETUP);
        assert(bl.target == 0x00003004u);
        assert(bl.writeMask == (1u << 14));
        assert(bl.minCycles == 1 && bl.maxCycles == 1);

        assert(Ps2Thumb::decode(0xF802u, 0x2002u, &bl));
        assert(bl.kind == Ps2Thumb::LONG_BRANCH_LINK);
        assert(bl.addend == 4);
        assert(bl.target == 0x2005u);
        assert(bl.readMask == (1u << 14));
        assert(bl.writeMask == (1u << 14));
        assert(bl.terminal && bl.minCycles == 3 && bl.maxCycles == 3);

        assert(Ps2Thumb::decode(0xF7FFu, 0x3000u, &bl));
        assert(bl.kind == Ps2Thumb::LONG_BRANCH_SETUP);
        assert(bl.target == 0x00002004u); // signed -1 high half
    }

    // Unconditional Thumb B uses PC+4 and signed halfword offset.
    assert(Ps2Thumb::decode(0xE7FEu, 0x2000u, &d));
    assert(d.kind == Ps2Thumb::BRANCH);
    assert(d.target == 0x2000u); // B . self-loop
    assert(d.minCycles == 3 && d.maxCycles == 3);

    std::puts("thumb_dynarec_test: OK");
    return 0;
}
