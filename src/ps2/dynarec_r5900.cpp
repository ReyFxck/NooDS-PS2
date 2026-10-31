#include "../memory.h"
#include "dynarec_r5900.h"
#include "r5900_emitter.h"

DynarecR5900::DynarecR5900(Memory *memory): memory(memory)
{
}

bool DynarecR5900::init(size_t cacheBytes)
{
    if (!memory || !cache.init(cacheBytes))
        return false;

    mappedGeneration = memory->getMapGeneration();
    blocks.clear();
    return emitProbe();
}

void DynarecR5900::shutdown()
{
    blocks.clear();
    cache.shutdown();
    mappedGeneration = 0;
    probeCode = nullptr;
}

void DynarecR5900::reset()
{
    blocks.clear();
    cache.reset();
    mappedGeneration = memory ? memory->getMapGeneration() : 0;
    probeCode = nullptr;
}

bool DynarecR5900::synchronizeMemoryMap()
{
    if (!memory)
        return false;

    uint32_t generation = memory->getMapGeneration();
    if (generation == mappedGeneration)
        return false;

    reset();
    mappedGeneration = generation;
    return true;
}

const R5900CompiledBlock *DynarecR5900::findBlock(uint32_t pc, bool thumb) const
{
    for (size_t i = 0; i < blocks.size(); i++)
    {
        if (blocks[i].guestPc == pc && blocks[i].thumb == thumb &&
            blocks[i].mapGeneration == mappedGeneration)
            return &blocks[i];
    }

    return nullptr;
}

bool DynarecR5900::emitProbe()
{
    // Emit a tiny valid leaf function:
    //   v0 = 0x4E44 ("ND")
    //   return
    // It is executed only as a bootstrap self-test. Its purpose is to validate
    // allocator, encoding and EE cache synchronization without touching emulated state.
    uint32_t *code = cache.reserveWords(4);
    if (!code)
        return false;

    probeCode = code;
    code[0] = R5900::lui(R5900::R_V0, 0);
    code[1] = R5900::ori(R5900::R_V0, R5900::R_V0, 0x4E44);
    code[2] = R5900::jr(R5900::R_RA);
    code[3] = R5900::nop();

    cache.commitWords(4);
    cache.syncExecutable(code, code + 4);
    return true;
}


bool DynarecR5900::runProbe(uint32_t *value) const
{
    if (!probeCode)
        return false;

    typedef uint32_t (*ProbeFunction)();
    ProbeFunction function = reinterpret_cast<ProbeFunction>(probeCode);
    uint32_t result = function();

    if (value)
        *value = result;

    return result == 0x4E44u;
}


namespace
{
struct ArmAluState
{
    uint32_t r[16];
};
}

bool DynarecR5900::emitArmAluBlock(uint32_t opcode, uint32_t **entry, uint8_t *rdOut)
{
    if (!entry || !rdOut)
        return false;

    // First correctness subset:
    // - ARM state, AL condition
    // - data-processing register operand
    // - no flag writes
    // - no shift (LSL #0)
    // - no PC source/destination
    if ((opcode >> 28) != 0xEu ||
        (opcode & 0x0C000000u) != 0 ||
        (opcode & 0x02000000u) != 0 ||
        (opcode & 0x00100000u) != 0 ||
        (opcode & 0x00000FF0u) != 0)
        return false;

    uint8_t operation = static_cast<uint8_t>((opcode >> 21) & 0xF);
    uint8_t rn = static_cast<uint8_t>((opcode >> 16) & 0xF);
    uint8_t rd = static_cast<uint8_t>((opcode >> 12) & 0xF);
    uint8_t rm = static_cast<uint8_t>(opcode & 0xF);

    if (rd == 15 || rm == 15 || (operation != 13 && rn == 15))
        return false;

    switch (operation)
    {
        case 0:  // AND
        case 1:  // EOR
        case 2:  // SUB
        case 4:  // ADD
        case 12: // ORR
        case 13: // MOV
            break;
        default:
            return false;
    }

    uint32_t *code = cache.reserveWords(8);
    if (!code)
        return false;

    size_t words = 0;
    if (operation != 13)
        code[words++] = R5900::lw(R5900::R_T0, static_cast<int16_t>(rn * 4), R5900::R_A0);
    code[words++] = R5900::lw(R5900::R_T1, static_cast<int16_t>(rm * 4), R5900::R_A0);

    switch (operation)
    {
        case 0:  code[words++] = R5900::and_(R5900::R_T2, R5900::R_T0, R5900::R_T1); break;
        case 1:  code[words++] = R5900::xor_(R5900::R_T2, R5900::R_T0, R5900::R_T1); break;
        case 2:  code[words++] = R5900::subu(R5900::R_T2, R5900::R_T0, R5900::R_T1); break;
        case 4:  code[words++] = R5900::addu(R5900::R_T2, R5900::R_T0, R5900::R_T1); break;
        case 12: code[words++] = R5900::or_(R5900::R_T2, R5900::R_T0, R5900::R_T1); break;
        case 13: code[words++] = R5900::or_(R5900::R_T2, R5900::R_T1, R5900::R_ZERO); break;
    }

    code[words++] = R5900::sw(R5900::R_T2, static_cast<int16_t>(rd * 4), R5900::R_A0);
    code[words++] = R5900::jr(R5900::R_RA);
    code[words++] = R5900::nop();

    cache.commitWords(words);
    cache.syncExecutable(code, code + words);

    *entry = code;
    *rdOut = rd;
    return true;
}

bool DynarecR5900::runArmAluSelfTest()
{
    struct Test
    {
        uint32_t opcode;
        uint8_t rd;
        uint32_t expected;
    };

    const Test tests[] =
    {
        { 0xE1A02001u, 2, 0x01020304u }, // MOV r2,r1
        { 0xE0803001u, 3, 0x1336597Cu }, // ADD r3,r0,r1
        { 0xE0404001u, 4, 0x11325374u }, // SUB r4,r0,r1
        { 0xE0005001u, 5, 0x00000200u }, // AND r5,r0,r1
        { 0xE0206001u, 6, 0x1336557Cu }, // EOR r6,r0,r1
        { 0xE1807001u, 7, 0x1336577Cu }  // ORR r7,r0,r1
    };

    ArmAluState state = {};
    state.r[0] = 0x12345678u;
    state.r[1] = 0x01020304u;

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
    {
        uint32_t *entry = nullptr;
        uint8_t rd = 0;
        if (!emitArmAluBlock(tests[i].opcode, &entry, &rd) || rd != tests[i].rd)
            return false;

        typedef void (*ArmAluFunction)(ArmAluState *);
        ArmAluFunction function = reinterpret_cast<ArmAluFunction>(entry);
        function(&state);

        if (state.r[rd] != tests[i].expected)
            return false;
    }

    return true;
}
