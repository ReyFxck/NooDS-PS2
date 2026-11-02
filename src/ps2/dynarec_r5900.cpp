#include "../memory.h"
#include "../cpu_backend.h"
#include "arm_alu.h"
#include "dynarec_r5900.h"
#include "r5900_emitter.h"

#include <algorithm>
#include <cstring>

namespace
{
void recordFallback(R5900DynarecProfile &profile, uint32_t pc, uint32_t opcode)
{
    for (size_t i = 0; i < 16; i++)
    {
        R5900FallbackProfile &entry = profile.topFallback[i];
        if (entry.count && entry.pc == pc && entry.opcode == opcode)
        {
            entry.count++;
            return;
        }
        if (!entry.count)
        {
            entry.pc = pc;
            entry.opcode = opcode;
            entry.count = 1;
            return;
        }
    }

    // Bounded Misra-Gries reduction: no allocation and no trace spam.
    for (size_t i = 0; i < 16; i++)
        profile.topFallback[i].count--;
}
}

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
    profile.mapInvalidations++;
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

bool DynarecR5900::analyzeArmBlock(uint32_t pc, R5900BlockAnalysis *analysis,
    size_t maxInstructions)
{
    if (!memory || !analysis || !maxInstructions)
        return false;

    synchronizeMemoryMap();
    *analysis = R5900BlockAnalysis();
    analysis->startPc = pc;
    maxInstructions = std::min(maxInstructions, R5900_ANALYSIS_MAX_INSTRUCTIONS);
    profile.compileAttempts++;

    if (findBlock(pc, false))
        profile.lookupHits++;
    else
        profile.lookupMisses++;

    for (size_t i = 0; i < maxInstructions; i++)
    {
        uint32_t address = pc + static_cast<uint32_t>(i * 4);
        uint8_t *page = memory->getDirectReadPage(0, address);
        if (!page)
        {
            if (analysis->firstUnsupported == 0xFFu)
                analysis->firstUnsupported = static_cast<uint8_t>(i);
            break;
        }

        R5900AnalyzedInstruction &instruction =
            analysis->instructions[analysis->instructionCount];
        instruction.pc = address;
        std::memcpy(&instruction.opcode, page + (address & 0xFFFu),
            sizeof(instruction.opcode));
        Ps2ArmAlu::inspect(instruction.opcode, address, &instruction.info);
        instruction.reason = Ps2ArmAlu::rejectionReason(instruction.opcode);

        analysis->instructionCount++;
        analysis->readMask |= instruction.info.readMask;
        analysis->writeMask |= instruction.info.writeMask;
        if (instruction.reason == Ps2ArmAlu::REJECT_NONE)
            analysis->supportedInstructions++;
        else if (analysis->firstUnsupported == 0xFFu)
            analysis->firstUnsupported = static_cast<uint8_t>(i);

        if (instruction.info.kind == Ps2ArmAlu::KIND_BRANCH)
        {
            analysis->branchTarget = instruction.info.branchTarget;
            analysis->backEdge = instruction.info.backEdge;
        }

        if (instruction.info.terminatesBlock)
            break;
    }

    analysis->endPc = analysis->instructionCount
        ? analysis->instructions[analysis->instructionCount - 1].pc + 4u
        : pc;
    profile.guestInstructions += analysis->instructionCount;
    profile.supportedInstructions += analysis->supportedInstructions;
    profile.largestBlock = std::max(profile.largestBlock,
        static_cast<uint32_t>(analysis->instructionCount));

    if (analysis->instructionCount && analysis->firstUnsupported == 0xFFu)
    {
        profile.compileSuccess++;
    }
    else
    {
        profile.compileRejected++;
        profile.fallbackCount++;
        Ps2ArmAlu::RejectReason reason = Ps2ArmAlu::REJECT_MEMORY;
        if (analysis->firstUnsupported < analysis->instructionCount)
        {
            const R5900AnalyzedInstruction &unsupported =
                analysis->instructions[analysis->firstUnsupported];
            reason = unsupported.reason;
            recordFallback(profile, unsupported.pc, unsupported.opcode);
        }
        profile.rejectReason[reason]++;
    }

    return analysis->instructionCount != 0;
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
    uint32_t cpsr;
    uint32_t nextPc;
};
static_assert(offsetof(ArmAluState, cpsr) == 64,
    "R5900 ARM-state CPSR ABI offset");
static_assert(offsetof(ArmAluState, nextPc) == 68,
    "R5900 ARM-state next-PC ABI offset");

bool applyArmAlu(uint32_t opcode, ArmAluState *state)
{
    return state && Ps2ArmAlu::execute(opcode, state->r, &state->cpsr);
}

size_t emitArmCondition(uint8_t condition, uint32_t *code)
{
    if (!code || condition == 0xEu)
        return 0;

    size_t words = 0;
    code[words++] = R5900::lw(R5900::R_T5, 64, R5900::R_A0);
    switch (condition & 0xFu)
    {
        case 0x0: // EQ: Z
            code[words++] = R5900::srl(R5900::R_T7, R5900::R_T5, 30);
            code[words++] = R5900::andi(R5900::R_T7, R5900::R_T7, 1);
            break;
        case 0x1: // NE: !Z
            code[words++] = R5900::srl(R5900::R_T7, R5900::R_T5, 30);
            code[words++] = R5900::andi(R5900::R_T7, R5900::R_T7, 1);
            code[words++] = R5900::xori(R5900::R_T7, R5900::R_T7, 1);
            break;
        case 0x2: // CS: C
            code[words++] = R5900::srl(R5900::R_T7, R5900::R_T5, 29);
            code[words++] = R5900::andi(R5900::R_T7, R5900::R_T7, 1);
            break;
        case 0x3: // CC: !C
            code[words++] = R5900::srl(R5900::R_T7, R5900::R_T5, 29);
            code[words++] = R5900::andi(R5900::R_T7, R5900::R_T7, 1);
            code[words++] = R5900::xori(R5900::R_T7, R5900::R_T7, 1);
            break;
        case 0x4: // MI: N
            code[words++] = R5900::srl(R5900::R_T7, R5900::R_T5, 31);
            break;
        case 0x5: // PL: !N
            code[words++] = R5900::srl(R5900::R_T7, R5900::R_T5, 31);
            code[words++] = R5900::xori(R5900::R_T7, R5900::R_T7, 1);
            break;
        case 0x6: // VS: V
            code[words++] = R5900::srl(R5900::R_T7, R5900::R_T5, 28);
            code[words++] = R5900::andi(R5900::R_T7, R5900::R_T7, 1);
            break;
        case 0x7: // VC: !V
            code[words++] = R5900::srl(R5900::R_T7, R5900::R_T5, 28);
            code[words++] = R5900::andi(R5900::R_T7, R5900::R_T7, 1);
            code[words++] = R5900::xori(R5900::R_T7, R5900::R_T7, 1);
            break;
        case 0x8: // HI: C && !Z
            code[words++] = R5900::srl(R5900::R_T7, R5900::R_T5, 29);
            code[words++] = R5900::andi(R5900::R_T7, R5900::R_T7, 1);
            code[words++] = R5900::srl(R5900::R_T6, R5900::R_T5, 30);
            code[words++] = R5900::andi(R5900::R_T6, R5900::R_T6, 1);
            code[words++] = R5900::xori(R5900::R_T6, R5900::R_T6, 1);
            code[words++] = R5900::and_(R5900::R_T7, R5900::R_T7, R5900::R_T6);
            break;
        case 0x9: // LS: !C || Z
            code[words++] = R5900::srl(R5900::R_T7, R5900::R_T5, 29);
            code[words++] = R5900::andi(R5900::R_T7, R5900::R_T7, 1);
            code[words++] = R5900::xori(R5900::R_T7, R5900::R_T7, 1);
            code[words++] = R5900::srl(R5900::R_T6, R5900::R_T5, 30);
            code[words++] = R5900::andi(R5900::R_T6, R5900::R_T6, 1);
            code[words++] = R5900::or_(R5900::R_T7, R5900::R_T7, R5900::R_T6);
            break;
        case 0xA: // GE: N == V
        case 0xB: // LT: N != V
            code[words++] = R5900::srl(R5900::R_T7, R5900::R_T5, 31);
            code[words++] = R5900::srl(R5900::R_T6, R5900::R_T5, 28);
            code[words++] = R5900::andi(R5900::R_T6, R5900::R_T6, 1);
            code[words++] = R5900::xor_(R5900::R_T7, R5900::R_T7, R5900::R_T6);
            if (condition == 0xAu)
                code[words++] = R5900::xori(R5900::R_T7, R5900::R_T7, 1);
            break;
        case 0xC: // GT: !Z && N == V
            code[words++] = R5900::srl(R5900::R_T7, R5900::R_T5, 30);
            code[words++] = R5900::andi(R5900::R_T7, R5900::R_T7, 1);
            code[words++] = R5900::xori(R5900::R_T7, R5900::R_T7, 1);
            code[words++] = R5900::srl(R5900::R_T6, R5900::R_T5, 31);
            code[words++] = R5900::srl(R5900::R_T4, R5900::R_T5, 28);
            code[words++] = R5900::andi(R5900::R_T4, R5900::R_T4, 1);
            code[words++] = R5900::xor_(R5900::R_T6, R5900::R_T6, R5900::R_T4);
            code[words++] = R5900::xori(R5900::R_T6, R5900::R_T6, 1);
            code[words++] = R5900::and_(R5900::R_T7, R5900::R_T7, R5900::R_T6);
            break;
        case 0xD: // LE: Z || N != V
            code[words++] = R5900::srl(R5900::R_T7, R5900::R_T5, 30);
            code[words++] = R5900::andi(R5900::R_T7, R5900::R_T7, 1);
            code[words++] = R5900::srl(R5900::R_T6, R5900::R_T5, 31);
            code[words++] = R5900::srl(R5900::R_T4, R5900::R_T5, 28);
            code[words++] = R5900::andi(R5900::R_T4, R5900::R_T4, 1);
            code[words++] = R5900::xor_(R5900::R_T6, R5900::R_T6, R5900::R_T4);
            code[words++] = R5900::or_(R5900::R_T7, R5900::R_T7, R5900::R_T6);
            break;
        default: // NV never executes as a normal ARM data instruction.
            code[words++] = R5900::or_(R5900::R_T7,
                R5900::R_ZERO, R5900::R_ZERO);
            break;
    }
    return words;
}

size_t emitArmBranchInstruction(uint32_t opcode, uint32_t pc, uint32_t *code)
{
    if (!code)
        return 0;

    Ps2ArmAlu::DecodedBranch decoded;
    if (!Ps2ArmAlu::decodeBranch(opcode, &decoded))
        return 0;

    uint32_t fallthrough = pc + 4u;
    uint32_t target = pc + 8u + static_cast<uint32_t>(decoded.offset);
    size_t words = 0;

    if (decoded.condition != 0xEu)
    {
        code[words++] = R5900::lui(R5900::R_T2,
            static_cast<uint16_t>(fallthrough >> 16));
        code[words++] = R5900::ori(R5900::R_T2, R5900::R_T2,
            static_cast<uint16_t>(fallthrough));
        words += emitArmCondition(decoded.condition, code + words);

        // A failed guest condition skips the two target-load instructions and
        // stores the already loaded fallthrough address. The MIPS delay slot
        // remains an explicit NOP.
        code[words++] = R5900::beq(R5900::R_T7, R5900::R_ZERO, 3);
        code[words++] = R5900::nop();
    }

    code[words++] = R5900::lui(R5900::R_T2,
        static_cast<uint16_t>(target >> 16));
    code[words++] = R5900::ori(R5900::R_T2, R5900::R_T2,
        static_cast<uint16_t>(target));
    code[words++] = R5900::sw(R5900::R_T2, 68, R5900::R_A0);
    return words;
}

size_t emitArmFlags(const Ps2ArmAlu::Decoded &decoded, uint32_t *code)
{
    if (!code || !decoded.setFlags)
        return 0;

    size_t words = 0;
    bool arithmetic = decoded.operation == 2 || decoded.operation == 3 ||
        decoded.operation == 4 || decoded.operation == 10 ||
        decoded.operation == 11;
    code[words++] = R5900::lw(R5900::R_T5, 64, R5900::R_A0);
    code[words++] = R5900::lui(R5900::R_T6,
        arithmetic ? 0x0FFFu : 0x3FFFu);
    code[words++] = R5900::ori(R5900::R_T6, R5900::R_T6, 0xFFFFu);
    code[words++] = R5900::and_(R5900::R_T5, R5900::R_T5, R5900::R_T6);

    // N and Z are common to every admitted S operation.
    code[words++] = R5900::srl(R5900::R_T6, R5900::R_T2, 31);
    code[words++] = R5900::sll(R5900::R_T6, R5900::R_T6, 31);
    code[words++] = R5900::or_(R5900::R_T5, R5900::R_T5, R5900::R_T6);
    code[words++] = R5900::sltiu(R5900::R_T6, R5900::R_T2, 1);
    code[words++] = R5900::sll(R5900::R_T6, R5900::R_T6, 30);
    code[words++] = R5900::or_(R5900::R_T5, R5900::R_T5, R5900::R_T6);

    if (arithmetic)
    {
        if (decoded.operation == 2 || decoded.operation == 10) // SUB/CMP: T0 - T1
        {
            code[words++] = R5900::nor_(R5900::R_T3, R5900::R_T0, R5900::R_ZERO);
            code[words++] = R5900::and_(R5900::R_T3, R5900::R_T3, R5900::R_T1);
            code[words++] = R5900::xor_(R5900::R_T6, R5900::R_T0, R5900::R_T1);
            code[words++] = R5900::nor_(R5900::R_T6, R5900::R_T6, R5900::R_ZERO);
            code[words++] = R5900::and_(R5900::R_T6, R5900::R_T6, R5900::R_T2);
            code[words++] = R5900::or_(R5900::R_T3, R5900::R_T3, R5900::R_T6);
            code[words++] = R5900::srl(R5900::R_T3, R5900::R_T3, 31);
            code[words++] = R5900::xori(R5900::R_T3, R5900::R_T3, 1);
            code[words++] = R5900::xor_(R5900::R_T4, R5900::R_T0, R5900::R_T1);
            code[words++] = R5900::xor_(R5900::R_T6, R5900::R_T0, R5900::R_T2);
        }
        else if (decoded.operation == 3) // RSB: T1 - T0
        {
            code[words++] = R5900::nor_(R5900::R_T3, R5900::R_T1, R5900::R_ZERO);
            code[words++] = R5900::and_(R5900::R_T3, R5900::R_T3, R5900::R_T0);
            code[words++] = R5900::xor_(R5900::R_T6, R5900::R_T1, R5900::R_T0);
            code[words++] = R5900::nor_(R5900::R_T6, R5900::R_T6, R5900::R_ZERO);
            code[words++] = R5900::and_(R5900::R_T6, R5900::R_T6, R5900::R_T2);
            code[words++] = R5900::or_(R5900::R_T3, R5900::R_T3, R5900::R_T6);
            code[words++] = R5900::srl(R5900::R_T3, R5900::R_T3, 31);
            code[words++] = R5900::xori(R5900::R_T3, R5900::R_T3, 1);
            code[words++] = R5900::xor_(R5900::R_T4, R5900::R_T1, R5900::R_T0);
            code[words++] = R5900::xor_(R5900::R_T6, R5900::R_T1, R5900::R_T2);
        }
        else // ADD: T0 + T1
        {
            code[words++] = R5900::and_(R5900::R_T3, R5900::R_T0, R5900::R_T1);
            code[words++] = R5900::or_(R5900::R_T6, R5900::R_T0, R5900::R_T1);
            code[words++] = R5900::nor_(R5900::R_T4, R5900::R_T2, R5900::R_ZERO);
            code[words++] = R5900::and_(R5900::R_T6, R5900::R_T6, R5900::R_T4);
            code[words++] = R5900::or_(R5900::R_T3, R5900::R_T3, R5900::R_T6);
            code[words++] = R5900::srl(R5900::R_T3, R5900::R_T3, 31);
            code[words++] = R5900::xor_(R5900::R_T4, R5900::R_T0, R5900::R_T1);
            code[words++] = R5900::nor_(R5900::R_T4, R5900::R_T4, R5900::R_ZERO);
            code[words++] = R5900::xor_(R5900::R_T6, R5900::R_T0, R5900::R_T2);
        }
        code[words++] = R5900::and_(R5900::R_T4, R5900::R_T4, R5900::R_T6);
        code[words++] = R5900::srl(R5900::R_T4, R5900::R_T4, 31);
        code[words++] = R5900::sll(R5900::R_T3, R5900::R_T3, 29);
        code[words++] = R5900::sll(R5900::R_T4, R5900::R_T4, 28);
        code[words++] = R5900::or_(R5900::R_T5, R5900::R_T5, R5900::R_T3);
        code[words++] = R5900::or_(R5900::R_T5, R5900::R_T5, R5900::R_T4);
    }
    code[words++] = R5900::sw(R5900::R_T5, 64, R5900::R_A0);
    return words;
}

size_t emitArmAluInstruction(uint32_t opcode, uint32_t *code)
{
    if (!code)
        return 0;

    Ps2ArmAlu::Decoded decoded;
    if (!Ps2ArmAlu::decode(opcode, &decoded))
        return 0;

    size_t words = 0;
    size_t conditionBranch = static_cast<size_t>(-1);
    if (decoded.condition != 0xEu)
    {
        words += emitArmCondition(decoded.condition, code + words);
        conditionBranch = words;
        code[words++] = R5900::beq(R5900::R_T7, R5900::R_ZERO, 0);
        code[words++] = R5900::nop();
    }
    if (decoded.operation != 13 && decoded.operation != 15)
        code[words++] = R5900::lw(R5900::R_T0,
            static_cast<int16_t>(decoded.rn * 4), R5900::R_A0);

    if (decoded.immediate)
    {
        code[words++] = R5900::lui(R5900::R_T1,
            static_cast<uint16_t>(decoded.immediateValue >> 16));
        code[words++] = R5900::ori(R5900::R_T1, R5900::R_T1,
            static_cast<uint16_t>(decoded.immediateValue));
    }
    else
    {
        code[words++] = R5900::lw(R5900::R_T1,
            static_cast<int16_t>(decoded.rm * 4), R5900::R_A0);

        if (decoded.registerShift)
        {
            code[words++] = R5900::lw(R5900::R_T3,
                static_cast<int16_t>(decoded.rs * 4), R5900::R_A0);
            code[words++] = R5900::andi(R5900::R_T3, R5900::R_T3, 0xFFu);

            switch (decoded.shiftType)
            {
                case 0: // ARM LSL Rs: amounts >= 32 produce zero.
                    code[words++] = R5900::sllv(R5900::R_T1, R5900::R_T1, R5900::R_T3);
                    code[words++] = R5900::sltiu(R5900::R_T4, R5900::R_T3, 32);
                    code[words++] = R5900::movz(R5900::R_T1, R5900::R_ZERO, R5900::R_T4);
                    break;
                case 1: // ARM LSR Rs: amounts >= 32 produce zero.
                    code[words++] = R5900::srlv(R5900::R_T1, R5900::R_T1, R5900::R_T3);
                    code[words++] = R5900::sltiu(R5900::R_T4, R5900::R_T3, 32);
                    code[words++] = R5900::movz(R5900::R_T1, R5900::R_ZERO, R5900::R_T4);
                    break;
                case 2: // ARM ASR Rs: amounts >= 32 replicate bit 31.
                    code[words++] = R5900::sra(R5900::R_T2, R5900::R_T1, 31);
                    code[words++] = R5900::srav(R5900::R_T1, R5900::R_T1, R5900::R_T3);
                    code[words++] = R5900::sltiu(R5900::R_T4, R5900::R_T3, 32);
                    code[words++] = R5900::movz(R5900::R_T1, R5900::R_T2, R5900::R_T4);
                    break;
                case 3: // ARM ROR Rs uses the low five bits after zero check.
                    code[words++] = R5900::subu(R5900::R_T4, R5900::R_ZERO, R5900::R_T3);
                    code[words++] = R5900::srlv(R5900::R_T2, R5900::R_T1, R5900::R_T3);
                    code[words++] = R5900::sllv(R5900::R_T1, R5900::R_T1, R5900::R_T4);
                    code[words++] = R5900::or_(R5900::R_T1, R5900::R_T1, R5900::R_T2);
                    break;
                default:
                    return 0;
            }
        }
        else
        {
            switch (decoded.shiftType)
            {
                case 0:
                    if (decoded.shiftAmount)
                        code[words++] = R5900::sll(R5900::R_T1, R5900::R_T1,
                            decoded.shiftAmount);
                    break;
                case 1:
                    if (decoded.shiftAmount)
                        code[words++] = R5900::srl(R5900::R_T1, R5900::R_T1,
                            decoded.shiftAmount);
                    else
                        code[words++] = R5900::or_(R5900::R_T1,
                            R5900::R_ZERO, R5900::R_ZERO);
                    break;
                case 2:
                    code[words++] = R5900::sra(R5900::R_T1, R5900::R_T1,
                        decoded.shiftAmount ? decoded.shiftAmount : 31);
                    break;
                case 3:
                    code[words++] = R5900::srl(R5900::R_T2, R5900::R_T1,
                        decoded.shiftAmount);
                    code[words++] = R5900::sll(R5900::R_T3, R5900::R_T1,
                        32u - decoded.shiftAmount);
                    code[words++] = R5900::or_(R5900::R_T1,
                        R5900::R_T2, R5900::R_T3);
                    break;
                default:
                    return 0;
            }
        }
    }

    switch (decoded.operation)
    {
        case 0:  code[words++] = R5900::and_(R5900::R_T2, R5900::R_T0, R5900::R_T1); break;
        case 1:  code[words++] = R5900::xor_(R5900::R_T2, R5900::R_T0, R5900::R_T1); break;
        case 2:  code[words++] = R5900::subu(R5900::R_T2, R5900::R_T0, R5900::R_T1); break;
        case 3:  code[words++] = R5900::subu(R5900::R_T2, R5900::R_T1, R5900::R_T0); break;
        case 4:  code[words++] = R5900::addu(R5900::R_T2, R5900::R_T0, R5900::R_T1); break;
        case 8:  code[words++] = R5900::and_(R5900::R_T2, R5900::R_T0, R5900::R_T1); break;
        case 9:  code[words++] = R5900::xor_(R5900::R_T2, R5900::R_T0, R5900::R_T1); break;
        case 10: code[words++] = R5900::subu(R5900::R_T2, R5900::R_T0, R5900::R_T1); break;
        case 11: code[words++] = R5900::addu(R5900::R_T2, R5900::R_T0, R5900::R_T1); break;
        case 12: code[words++] = R5900::or_(R5900::R_T2, R5900::R_T0, R5900::R_T1); break;
        case 13: code[words++] = R5900::or_(R5900::R_T2, R5900::R_T1, R5900::R_ZERO); break;
        case 14:
            code[words++] = R5900::nor_(R5900::R_T1, R5900::R_T1, R5900::R_ZERO);
            code[words++] = R5900::and_(R5900::R_T2, R5900::R_T0, R5900::R_T1);
            break;
        case 15: code[words++] = R5900::nor_(R5900::R_T2, R5900::R_T1, R5900::R_ZERO); break;
        default: return 0;
    }

    if (decoded.writesResult)
        code[words++] = R5900::sw(R5900::R_T2,
            static_cast<int16_t>(decoded.rd * 4), R5900::R_A0);
    words += emitArmFlags(decoded, code + words);
    if (conditionBranch != static_cast<size_t>(-1))
    {
        size_t distance = words - (conditionBranch + 1);
        if (distance > 0x7FFFu)
            return 0;
        code[conditionBranch] = R5900::beq(R5900::R_T7,
            R5900::R_ZERO, static_cast<int16_t>(distance));
    }
    return words;
}

size_t emitArmInstruction(uint32_t opcode, uint32_t pc, uint32_t *code)
{
    Ps2ArmAlu::Decoded decoded;
    if (Ps2ArmAlu::decode(opcode, &decoded))
        return emitArmAluInstruction(opcode, code);
    return emitArmBranchInstruction(opcode, pc, code);
}
}

bool DynarecR5900::emitArmAluBlock(uint32_t opcode, uint32_t **entry, uint8_t *rdOut)
{
    if (!entry || !rdOut)
        return false;

    Ps2ArmAlu::Decoded decoded;
    if (!Ps2ArmAlu::decode(opcode, &decoded))
        return false;

    uint32_t *code = cache.reserveWords(64);
    if (!code)
        return false;

    size_t words = emitArmAluInstruction(opcode, code);
    if (!words)
        return false;
    code[words++] = R5900::jr(R5900::R_RA);
    code[words++] = R5900::nop();

    cache.commitWords(words);
    cache.syncExecutable(code, code + words);

    *entry = code;
    *rdOut = decoded.rd;
    return true;
}

bool DynarecR5900::emitArmAluSequence(const uint32_t *opcodes, size_t count,
    uint32_t guestPc, uint32_t **entry, uint32_t *hostWords)
{
    if (!opcodes || !count || !entry || !hostWords)
        return false;

    // Conditions plus exact NZCV generation are deliberately explicit for the
    // correctness gate. A supported B may terminate the sequence; register
    // caching can compact this after scheduler use.
    const size_t maxWords = count * 64 + 2;
    uint32_t *code = cache.reserveWords(maxWords);
    if (!code)
        return false;

    size_t words = 0;
    for (size_t i = 0; i < count; i++)
    {
        size_t emitted = emitArmInstruction(opcodes[i],
            guestPc + static_cast<uint32_t>(i * 4), code + words);
        if (!emitted)
            return false;
        words += emitted;
    }

    code[words++] = R5900::jr(R5900::R_RA);
    code[words++] = R5900::nop();

    cache.commitWords(words);
    cache.syncExecutable(code, code + words);

    R5900CompiledBlock block = {};
    block.guestPc = guestPc;
    block.guestEndPc = guestPc + static_cast<uint32_t>(count * 4);
    block.mapGeneration = mappedGeneration;
    block.guestInstructions = static_cast<uint32_t>(count);
    block.cycles = static_cast<uint32_t>(count);
    block.thumb = false;
    block.hostCode = code;
    block.hostWords = static_cast<uint32_t>(words);

    for (size_t i = 0; i < count; i++)
    {
        Ps2ArmAlu::Decoded decoded;
        if (Ps2ArmAlu::decode(opcodes[i], &decoded))
        {
            if (decoded.operation != 13 && decoded.operation != 15)
                block.readMask |= static_cast<uint16_t>(1u << decoded.rn);
            if (!decoded.immediate)
                block.readMask |= static_cast<uint16_t>(1u << decoded.rm);
            if (decoded.registerShift)
                block.readMask |= static_cast<uint16_t>(1u << decoded.rs);
            if (decoded.writesResult)
                block.writeMask |= static_cast<uint16_t>(1u << decoded.rd);
            block.readsFlags |= decoded.condition != 0xEu;
            block.writesFlags |= decoded.setFlags;
            continue;
        }

        Ps2ArmAlu::DecodedBranch branch;
        if (!Ps2ArmAlu::decodeBranch(opcodes[i], &branch) || i + 1 != count)
            return false;
        block.readsFlags |= branch.condition != 0xEu;
    }
    blocks.push_back(block);

    *entry = code;
    *hostWords = static_cast<uint32_t>(words);
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
        { 0xE0098E18u,  8, 0x0000AB00u }, // AND r8,r9,r8,LSL r14
        { 0xE1A08E58u,  8, 0x000000ABu }, // MOV r8,r8,ASR r14
        { 0xE1A0424Bu,  4, 0xFF234567u }, // MOV r4,r11,ASR #4
        { 0xE1A0704Bu,  7, 0xFFFFFFFFu }, // MOV r7,r11,ASR #32
        { 0xE1A02001u,  2, 0x01020304u }, // MOV r2,r1
        { 0xE0803001u,  3, 0x1336597Cu }, // ADD r3,r0,r1
        { 0xE0404001u,  4, 0x11325374u }, // SUB r4,r0,r1
        { 0xE0005001u,  5, 0x00000200u }, // AND r5,r0,r1
        { 0xE0206001u,  6, 0x1336557Cu }, // EOR r6,r0,r1
        { 0xE1807001u,  7, 0x1336577Cu }, // ORR r7,r0,r1
        { 0xE3A08012u,  8, 0x00000012u }, // MOV r8,#0x12
        { 0xE2809034u,  9, 0x123456ACu }, // ADD r9,r0,#0x34
        { 0xE240A034u, 10, 0x12345644u }, // SUB r10,r0,#0x34
        { 0xE200B0FFu, 11, 0x00000078u }, // AND r11,r0,#0xFF
        { 0xE220C001u, 12, 0x12345679u }, // EOR r12,r0,#1
        { 0xE380D080u, 13, 0x123456F8u }, // ORR r13,r0,#0x80
        { 0xE060E001u, 14, 0xEECDAC8Cu }, // RSB r14,r0,r1
        { 0xE1C0E001u, 14, 0x12345478u }, // BIC r14,r0,r1
        { 0xE1E0E001u, 14, 0xFEFDFCFBu }, // MVN r14,r1
        { 0xE1A02181u,  2, 0x08101820u }, // MOV r2,r1,LSL #3
        { 0xE1A03220u,  3, 0x01234567u }, // MOV r3,r0,LSR #4
        { 0xE1A05460u,  5, 0x78123456u }, // MOV r5,r0,ROR #8
        { 0xE1A06020u,  6, 0x00000000u }  // MOV r6,r0,LSR #32
    };

    ArmAluState state = {};
    state.r[0] = 0x12345678u;
    state.r[1] = 0x01020304u;
    state.r[8] = 0x000000FFu;
    state.r[9] = 0x0000ABCDu;
    state.r[11] = 0xF2345678u;
    state.r[14] = 8u;

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

    struct FlagTest
    {
        uint32_t opcode;
        uint32_t cpsr;
        uint8_t sourceRegister;
        uint32_t sourceValue;
    };
    const FlagTest flagTests[] =
    {
        { 0x01A02001u, 0x00000013u, 1, 0xA5A55A5Au }, // MOV.EQ not taken
        { 0x01A02001u, 0x40000013u, 1, 0xA5A55A5Au }, // MOV.EQ taken
        { 0xC1A02001u, 0x00000013u, 1, 0xA5A55A5Au }, // MOV.GT taken
        { 0xE2322008u, 0x30000013u, 2, 0x00000008u }, // EORS -> Z, preserve C/V
        { 0xE2555001u, 0xF0000013u, 5, 0x00000000u }, // SUBS -> N
        { 0xE2555001u, 0x00000013u, 5, 0x80000000u }, // SUBS overflow
        { 0xE2910001u, 0xF0000013u, 1, 0xFFFFFFFFu }, // ADDS carry + zero
        { 0xE1100001u, 0x30000013u, 0, 0xFFFFFFFFu }, // TST preserves registers
        { 0xE1300001u, 0x30000013u, 0, 0x01020304u }, // TEQ preserves registers
        { 0xE15C0002u, 0xF0000013u, 12, 0x00000010u }, // hot CMP r12,r2
        { 0xE1700001u, 0xF0000013u, 0, 0xFEFDFCFCu }  // CMN preserves registers
    };

    for (size_t i = 0; i < sizeof(flagTests) / sizeof(flagTests[0]); i++)
    {
        ArmAluState source = {};
        source.r[1] = 0x01020304u;
        source.r[2] = 0x11223344u;
        source.r[flagTests[i].sourceRegister] = flagTests[i].sourceValue;
        source.cpsr = flagTests[i].cpsr;
        ArmAluState expected = source;
        if (!applyArmAlu(flagTests[i].opcode, &expected))
            return false;

        uint32_t *entry = nullptr;
        uint8_t rd = 0;
        if (!emitArmAluBlock(flagTests[i].opcode, &entry, &rd))
            return false;
        ArmAluState actual = source;
        typedef void (*ArmAluFunction)(ArmAluState *);
        ArmAluFunction function = reinterpret_cast<ArmAluFunction>(entry);
        function(&actual);
        if (std::memcmp(&actual, &expected, sizeof(actual)) != 0)
            return false;
    }

    struct BranchTest
    {
        uint32_t opcode;
        uint32_t pc;
        uint32_t cpsr;
    };
    const BranchTest branchTests[] =
    {
        { 0xBAFFFFFAu, 0x02066E90u, 0x80000013u }, // B.LT taken
        { 0xBAFFFFFAu, 0x02066E90u, 0x00000013u }, // B.LT not taken
        { 0xEA000006u, 0x0206094Cu, 0xF0000013u }  // B.AL taken
    };

    for (size_t i = 0; i < sizeof(branchTests) / sizeof(branchTests[0]); i++)
    {
        ArmAluState expected = {};
        expected.cpsr = branchTests[i].cpsr;
        expected.nextPc = branchTests[i].pc + 4u;
        if (!Ps2ArmAlu::executeBranch(branchTests[i].opcode,
            branchTests[i].pc, expected.cpsr, &expected.nextPc))
            return false;

        uint32_t *code = cache.reserveWords(32);
        if (!code)
            return false;
        size_t words = emitArmBranchInstruction(branchTests[i].opcode,
            branchTests[i].pc, code);
        if (!words)
            return false;
        code[words++] = R5900::jr(R5900::R_RA);
        code[words++] = R5900::nop();
        cache.commitWords(words);
        cache.syncExecutable(code, code + words);

        ArmAluState actual = {};
        actual.cpsr = branchTests[i].cpsr;
        actual.nextPc = branchTests[i].pc + 4u;
        typedef void (*ArmBranchFunction)(ArmAluState *);
        ArmBranchFunction function = reinterpret_cast<ArmBranchFunction>(code);
        function(&actual);
        if (std::memcmp(&actual, &expected, sizeof(actual)) != 0)
            return false;
    }

    return true;
}


bool DynarecR5900::runArm9StateSelfTest(const Interpreter &cpu, uint32_t *stateHash)
{
    if (!CpuBackendAccess::isArm9(cpu) || CpuBackendAccess::isThumb(cpu))
        return false;

    ArmAluState source = {};
    uint32_t hash = 2166136261u;
    for (int i = 0; i < 16; i++)
    {
        source.r[i] = CpuBackendAccess::readReg(cpu, i);
        hash ^= source.r[i];
        hash *= 16777619u;
    }
    source.cpsr = CpuBackendAccess::readCpsr(cpu);
    hash ^= CpuBackendAccess::readCpsr(cpu);
    hash *= 16777619u;

    if (stateHash)
        *stateHash = hash;

    struct LiveTest
    {
        uint32_t opcode;
        uint8_t rd;
        uint8_t rn;
        uint8_t rm;
        uint8_t operation;
    };

    const LiveTest tests[] =
    {
        { 0xE1A0000Cu, 0,  0, 12, 13 }, // MOV r0,r12
        { 0xE08C100Du, 1, 12, 13,  4 }, // ADD r1,r12,r13
        { 0xE04D200Cu, 2, 13, 12,  2 }, // SUB r2,r13,r12
        { 0xE00C300Du, 3, 12, 13,  0 }, // AND r3,r12,r13
        { 0xE02C400Du, 4, 12, 13,  1 }, // EOR r4,r12,r13
        { 0xE18C500Du, 5, 12, 13, 12 }  // ORR r5,r12,r13
    };

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
    {
        ArmAluState state = source;
        uint32_t expected = 0;

        switch (tests[i].operation)
        {
            case 0:  expected = source.r[tests[i].rn] & source.r[tests[i].rm]; break;
            case 1:  expected = source.r[tests[i].rn] ^ source.r[tests[i].rm]; break;
            case 2:  expected = source.r[tests[i].rn] - source.r[tests[i].rm]; break;
            case 4:  expected = source.r[tests[i].rn] + source.r[tests[i].rm]; break;
            case 12: expected = source.r[tests[i].rn] | source.r[tests[i].rm]; break;
            case 13: expected = source.r[tests[i].rm]; break;
            default: return false;
        }

        uint32_t *entry = nullptr;
        uint8_t rd = 0;
        if (!emitArmAluBlock(tests[i].opcode, &entry, &rd) || rd != tests[i].rd)
            return false;

        typedef void (*ArmAluFunction)(ArmAluState *);
        ArmAluFunction function = reinterpret_cast<ArmAluFunction>(entry);
        function(&state);

        if (state.r[rd] != expected)
            return false;
    }

    return true;
}


int DynarecR5900::runArm9PipelineSelfTest(const Interpreter &cpu,
    uint32_t *guestPc, uint32_t *opcodeOut)
{
    if (!CpuBackendAccess::isArm9(cpu) || CpuBackendAccess::isThumb(cpu))
        return 0;

    ArmAluState source = {};
    for (int i = 0; i < 16; i++)
        source.r[i] = CpuBackendAccess::readReg(cpu, i);
    source.cpsr = CpuBackendAccess::readCpsr(cpu);

    uint32_t pc = CpuBackendAccess::nextInstructionAddress(cpu);
    uint32_t opcode = CpuBackendAccess::pipelineOpcode(cpu, 0);
    Ps2ArmAlu::Decoded decoded;
    if (!Ps2ArmAlu::decode(opcode, &decoded))
        return 0;

    ArmAluState expected = source;
    if (!applyArmAlu(opcode, &expected))
        return -1;

    uint32_t *entry = nullptr;
    uint8_t emittedRd = 0;
    if (!emitArmAluBlock(opcode, &entry, &emittedRd) || emittedRd != decoded.rd)
        return -1;

    ArmAluState state = source;
    typedef void (*ArmAluFunction)(ArmAluState *);
    ArmAluFunction function = reinterpret_cast<ArmAluFunction>(entry);
    function(&state);

    if (guestPc)
        *guestPc = pc;
    if (opcodeOut)
        *opcodeOut = opcode;

    return state.r[decoded.rd] == expected.r[decoded.rd] &&
        state.cpsr == expected.cpsr ? 1 : -1;
}


int DynarecR5900::runArm9BlockShadowSelfTest(const Interpreter &cpu,
    uint32_t *guestPcOut, uint32_t *instructionCount, uint32_t *stateHash)
{
    if (!CpuBackendAccess::isArm9(cpu) || CpuBackendAccess::isThumb(cpu))
        return 0;

    synchronizeMemoryMap();

    constexpr size_t MAX_BLOCK_INSTRUCTIONS = 16;
    uint32_t opcodes[MAX_BLOCK_INSTRUCTIONS] = {};
    size_t count = 0;

    uint32_t guestPc = CpuBackendAccess::nextInstructionAddress(cpu);

    // pipeline[0] is authoritative for the next instruction. pipeline[1] is
    // authoritative for the following instruction; later words can be fetched
    // directly because this gate only accepts straight-line ALU instructions.
    for (size_t i = 0; i < MAX_BLOCK_INSTRUCTIONS; i++)
    {
        uint32_t opcode;
        if (i == 0)
            opcode = CpuBackendAccess::pipelineOpcode(cpu, 0);
        else if (i == 1)
            opcode = CpuBackendAccess::pipelineOpcode(cpu, 1);
        else
            opcode = memory->read<uint32_t>(0, guestPc + static_cast<uint32_t>(i * 4));

        Ps2ArmAlu::Decoded decoded;
        if (Ps2ArmAlu::decode(opcode, &decoded))
        {
            opcodes[count++] = opcode;
            continue;
        }

        Ps2ArmAlu::DecodedBranch branch;
        if (!Ps2ArmAlu::decodeBranch(opcode, &branch))
            break;
        opcodes[count++] = opcode;
        break;
    }

    if (!count)
        return 0;

    ArmAluState source = {};
    ArmAluState expected = {};
    for (int i = 0; i < 16; i++)
        source.r[i] = expected.r[i] = CpuBackendAccess::readReg(cpu, i);
    source.cpsr = expected.cpsr = CpuBackendAccess::readCpsr(cpu);
    source.nextPc = expected.nextPc = guestPc + static_cast<uint32_t>(count * 4);

    for (size_t i = 0; i < count; i++)
    {
        Ps2ArmAlu::Decoded decoded;
        if (Ps2ArmAlu::decode(opcodes[i], &decoded))
        {
            if (!applyArmAlu(opcodes[i], &expected))
                return -1;
        }
        else if (!Ps2ArmAlu::executeBranch(opcodes[i],
            guestPc + static_cast<uint32_t>(i * 4), expected.cpsr,
            &expected.nextPc))
        {
            return -1;
        }
    }

    uint32_t *entry = nullptr;
    uint32_t hostWords = 0;
    if (!emitArmAluSequence(opcodes, count, guestPc, &entry, &hostWords))
        return -1;

    ArmAluState actual = source;
    typedef void (*ArmAluBlockFunction)(ArmAluState *);
    ArmAluBlockFunction function = reinterpret_cast<ArmAluBlockFunction>(entry);
    function(&actual);

    uint32_t hash = 2166136261u;
    for (int i = 0; i < 16; i++)
    {
        if (actual.r[i] != expected.r[i])
            return -1;

        hash ^= actual.r[i];
        hash *= 16777619u;
    }
    if (actual.cpsr != expected.cpsr)
        return -1;
    hash ^= actual.cpsr;
    hash *= 16777619u;
    if (actual.nextPc != expected.nextPc)
        return -1;
    hash ^= actual.nextPc;
    hash *= 16777619u;

    hash ^= static_cast<uint32_t>(count);
    hash *= 16777619u;
    hash ^= hostWords;
    hash *= 16777619u;

    if (guestPcOut)
        *guestPcOut = guestPc;
    if (instructionCount)
        *instructionCount = static_cast<uint32_t>(count);
    if (stateHash)
        *stateHash = hash;

    return 1;
}


int DynarecR5900::runArm9InterpreterDifferentialSelfTest(Interpreter &cpu,
    uint32_t *guestPcOut, uint32_t *instructionCount, uint32_t *stateHash)
{
    if (!CpuBackendAccess::isArm9(cpu) || CpuBackendAccess::isThumb(cpu))
        return 0;

    uint32_t guestPc = CpuBackendAccess::nextInstructionAddress(cpu);
    const R5900CompiledBlock *block = findBlock(guestPc, false);
    if (!block || !block->hostCode || block->guestInstructions == 0)
        return 0;

    uint32_t savedRegs[16] = {};
    for (int i = 0; i < 16; i++)
        savedRegs[i] = CpuBackendAccess::readReg(cpu, i);

    uint32_t savedCpsr = CpuBackendAccess::readCpsr(cpu);
    uint32_t savedPipeline0 = CpuBackendAccess::pipelineOpcode(cpu, 0);
    uint32_t savedPipeline1 = CpuBackendAccess::pipelineOpcode(cpu, 1);

    ArmAluState dynarecState = {};
    for (int i = 0; i < 16; i++)
        dynarecState.r[i] = savedRegs[i];
    dynarecState.cpsr = savedCpsr;
    dynarecState.nextPc = block->guestEndPc;

    typedef void (*ArmAluBlockFunction)(ArmAluState *);
    ArmAluBlockFunction function =
        reinterpret_cast<ArmAluBlockFunction>(block->hostCode);
    function(&dynarecState);

    bool interpreterOk = true;
    for (uint32_t i = 0; i < block->guestInstructions; i++)
    {
        // Every admitted opcode is a straight-line ALU operation with no PC
        // or memory side effects. Conditions and NZCV are modeled explicitly.
        int cycles = cpu.runOpcode();
        if (cycles <= 0)
        {
            interpreterOk = false;
            break;
        }
    }

    uint32_t hash = 2166136261u;
    bool match = interpreterOk &&
        CpuBackendAccess::readCpsr(cpu) == dynarecState.cpsr &&
        CpuBackendAccess::nextInstructionAddress(cpu) == dynarecState.nextPc;

    // r15/pipeline naturally advance in the interpreter while the current
    // dynarec block ABI only models architectural GPR results. Compare r0-r14.
    for (int i = 0; i < 15; i++)
    {
        uint32_t interpreterValue = CpuBackendAccess::readReg(cpu, i);
        if (interpreterValue != dynarecState.r[i])
            match = false;

        hash ^= interpreterValue;
        hash *= 16777619u;
    }

    hash ^= CpuBackendAccess::readCpsr(cpu);
    hash *= 16777619u;
    hash ^= CpuBackendAccess::nextInstructionAddress(cpu);
    hash *= 16777619u;
    hash ^= block->guestInstructions;
    hash *= 16777619u;

    // Restore before returning on every path. The tested subset cannot change
    // bank/mode/memory, so this is exact and guest-invisible.
    CpuBackendAccess::restoreStraightLineState(cpu, savedRegs, savedCpsr,
        savedPipeline0, savedPipeline1);

    if (guestPcOut)
        *guestPcOut = guestPc;
    if (instructionCount)
        *instructionCount = block->guestInstructions;
    if (stateHash)
        *stateHash = hash;

    return match ? 1 : -1;
}
