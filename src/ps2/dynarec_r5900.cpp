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
    activeRejectedPcs.clear();
    cache.shutdown();
    mappedGeneration = 0;
    probeCode = nullptr;
}

void DynarecR5900::reset()
{
    blocks.clear();
    activeRejectedPcs.clear();
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

R5900CompiledBlock *DynarecR5900::findMutableBlock(uint32_t pc, bool thumb)
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

bool DynarecR5900::isDirectArmHalfwordAccess(uint32_t opcode,
    uint32_t address) const
{
    Ps2ArmAlu::DecodedHalfwordTransfer transfer;
    if (!memory || !Ps2ArmAlu::decodeHalfwordTransfer(opcode, &transfer) ||
        (address & 0xFFFu) == 0xFFFu)
        return false;

    return transfer.load
        ? memory->getDirectReadPage(0, address) != nullptr
        : memory->getDirectWritePage(0, address) != nullptr;
}

bool DynarecR5900::isDirectArmWordAccess(uint32_t opcode,
    uint32_t address) const
{
    Ps2ArmAlu::DecodedSingleTransfer transfer;
    if (!memory || !Ps2ArmAlu::decodeSingleTransfer(opcode, &transfer) ||
        (address & 3u) || (address & 0xFFFu) > 0xFFCu)
        return false;

    return transfer.load
        ? memory->getDirectReadPage(0, address) != nullptr
        : memory->getDirectWritePage(0, address) != nullptr;
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
struct ArmMemoryWrite
{
    uint32_t address;
    uint32_t before;
    uint32_t after;
    uint8_t size;
};

struct ArmAluState
{
    uint32_t r[16];
    uint32_t cpsr;
    uint32_t nextPc;
    Memory *memory;
    uint32_t memoryFault;
    uint32_t writeCount;
    uint32_t cycles;
    ArmMemoryWrite writes[16];
};
static_assert(offsetof(ArmAluState, cpsr) == 64,
    "R5900 ARM-state CPSR ABI offset");
static_assert(offsetof(ArmAluState, nextPc) == 68,
    "R5900 ARM-state next-PC ABI offset");
static_assert(offsetof(ArmAluState, cycles) == 84,
    "R5900 ARM-state cycle ABI offset");

bool applyArmAlu(uint32_t opcode, ArmAluState *state)
{
    if (!state || !Ps2ArmAlu::execute(opcode, state->r, &state->cpsr))
        return false;
    state->cycles++;
    return true;
}

bool applyArmAluHelper(ArmAluState *state, uint32_t opcode)
{
    return applyArmAlu(opcode, state);
}

bool readDirectArm9Halfword(void *context, uint32_t address, uint16_t *value)
{
    ArmAluState *state = static_cast<ArmAluState*>(context);
    Memory *memory = state ? state->memory : nullptr;
    uint32_t offset = address & 0xFFFu;
    if (!memory || !value || offset == 0xFFFu)
        return false;

    uint8_t *page = memory->getDirectReadPage(0, address);
    if (!page)
        return false;
    *value = static_cast<uint16_t>(page[offset] |
        (static_cast<uint16_t>(page[offset + 1]) << 8));
    return true;
}

bool writeDirectArm9Halfword(void *context, uint32_t address, uint16_t value)
{
    ArmAluState *state = static_cast<ArmAluState*>(context);
    Memory *memory = state ? state->memory : nullptr;
    uint32_t offset = address & 0xFFFu;
    if (!memory || offset == 0xFFFu || state->writeCount >= 16)
        return false;

    uint8_t *page = memory->getDirectWritePage(0, address);
    if (!page)
        return false;

    ArmMemoryWrite &write = state->writes[state->writeCount++];
    write.address = address;
    write.before = static_cast<uint16_t>(page[offset] |
        (static_cast<uint16_t>(page[offset + 1]) << 8));
    write.after = value;
    write.size = 2;
    page[offset] = static_cast<uint8_t>(value);
    page[offset + 1] = static_cast<uint8_t>(value >> 8);
    return true;
}

void restoreArmMemoryWrites(ArmAluState *state)
{
    if (!state || !state->memory)
        return;

    for (uint32_t i = state->writeCount; i > 0; i--)
    {
        const ArmMemoryWrite &write = state->writes[i - 1];
        uint32_t offset = write.address & 0xFFFu;
        uint8_t *page = state->memory->getDirectWritePage(0, write.address);
        if (!page || !write.size || offset + write.size > 0x1000u)
            continue;
        page[offset] = static_cast<uint8_t>(write.before);
        if (write.size >= 2)
            page[offset + 1] = static_cast<uint8_t>(write.before >> 8);
        if (write.size == 4)
        {
            page[offset + 2] = static_cast<uint8_t>(write.before >> 16);
            page[offset + 3] = static_cast<uint8_t>(write.before >> 24);
        }
    }
}

class ArmMemoryWriteGuard
{
    public:
        explicit ArmMemoryWriteGuard(ArmAluState *state): state(state) {}
        ~ArmMemoryWriteGuard() { restoreNow(); }

        void restoreNow()
        {
            if (!active)
                return;
            restoreArmMemoryWrites(state);
            active = false;
        }

    private:
        ArmAluState *state;
        bool active = true;
};

bool applyArmHalfwordTransfer(ArmAluState *state, uint32_t opcode)
{
    if (!state || !state->memory)
        return false;
    if (Ps2ArmAlu::executeHalfwordTransfer(opcode, state->r, &state->cpsr,
        state, readDirectArm9Halfword, writeDirectArm9Halfword))
    {
        state->cycles++;
        return true;
    }

    state->memoryFault = 1;
    return false;
}

bool readDirectArm9Byte(void *context, uint32_t address, uint8_t *value)
{
    ArmAluState *state = static_cast<ArmAluState*>(context);
    Memory *memory = state ? state->memory : nullptr;
    if (!memory || !value)
        return false;
    uint8_t *page = memory->getDirectReadPage(0, address);
    if (!page)
        return false;
    *value = page[address & 0xFFFu];
    return true;
}

bool writeDirectArm9Byte(void *context, uint32_t address, uint8_t value)
{
    ArmAluState *state = static_cast<ArmAluState*>(context);
    Memory *memory = state ? state->memory : nullptr;
    if (!memory || state->writeCount >= 16)
        return false;
    uint8_t *page = memory->getDirectWritePage(0, address);
    if (!page)
        return false;

    uint32_t offset = address & 0xFFFu;
    ArmMemoryWrite &entry = state->writes[state->writeCount++];
    entry.address = address;
    entry.before = page[offset];
    entry.after = value;
    entry.size = 1;
    page[offset] = value;
    return true;
}

bool readDirectArm9Word(void *context, uint32_t address, uint32_t *value)
{
    ArmAluState *state = static_cast<ArmAluState*>(context);
    Memory *memory = state ? state->memory : nullptr;
    uint32_t offset = address & 0xFFFu;
    if (!memory || !value || (address & 3u) || offset > 0xFFCu)
        return false;

    uint8_t *page = memory->getDirectReadPage(0, address);
    if (!page)
        return false;
    *value = static_cast<uint32_t>(page[offset]) |
        (static_cast<uint32_t>(page[offset + 1]) << 8) |
        (static_cast<uint32_t>(page[offset + 2]) << 16) |
        (static_cast<uint32_t>(page[offset + 3]) << 24);
    return true;
}

bool writeDirectArm9Word(void *context, uint32_t address, uint32_t value)
{
    ArmAluState *state = static_cast<ArmAluState*>(context);
    Memory *memory = state ? state->memory : nullptr;
    uint32_t offset = address & 0xFFFu;
    if (!memory || (address & 3u) || offset > 0xFFCu ||
        state->writeCount >= 16)
        return false;

    uint8_t *page = memory->getDirectWritePage(0, address);
    if (!page)
        return false;

    ArmMemoryWrite &write = state->writes[state->writeCount++];
    write.address = address;
    write.before = static_cast<uint32_t>(page[offset]) |
        (static_cast<uint32_t>(page[offset + 1]) << 8) |
        (static_cast<uint32_t>(page[offset + 2]) << 16) |
        (static_cast<uint32_t>(page[offset + 3]) << 24);
    write.after = value;
    write.size = 4;
    page[offset] = static_cast<uint8_t>(value);
    page[offset + 1] = static_cast<uint8_t>(value >> 8);
    page[offset + 2] = static_cast<uint8_t>(value >> 16);
    page[offset + 3] = static_cast<uint8_t>(value >> 24);
    return true;
}

bool applyArmByteTransfer(ArmAluState *state, uint32_t opcode)
{
    if (!state || !state->memory)
        return false;
    if (Ps2ArmAlu::executeByteTransfer(opcode, state->r, &state->cpsr,
        state, readDirectArm9Byte, writeDirectArm9Byte))
    {
        state->cycles++;
        return true;
    }
    state->memoryFault = 1;
    return false;
}

bool applyArmLiteralLoad(ArmAluState *state, uint32_t opcode, uint32_t pc)
{
    if (!state || !state->memory)
        return false;
    if (Ps2ArmAlu::executeLiteralLoad(opcode, pc, state->r, &state->cpsr,
        state, readDirectArm9Word))
    {
        state->cycles++;
        return true;
    }
    state->memoryFault = 1;
    return false;
}

bool applyArmSingleTransfer(ArmAluState *state, uint32_t opcode)
{
    if (!state || !state->memory)
        return false;
    if (Ps2ArmAlu::executeSingleTransfer(opcode, state->r, &state->cpsr,
        state, readDirectArm9Word, writeDirectArm9Word))
    {
        state->cycles++;
        return true;
    }

    state->memoryFault = 1;
    return false;
}

bool applyArmBranchExchange(ArmAluState *state, uint32_t opcode)
{
    if (!state)
        return false;
    Ps2ArmAlu::DecodedBranchExchange decoded;
    if (!Ps2ArmAlu::decodeBranchExchange(opcode, &decoded))
        return false;
    bool taken = Ps2ArmAlu::conditionPassed(decoded.condition, state->cpsr);
    if (!Ps2ArmAlu::executeBranchExchange(opcode, state->r,
        &state->cpsr, &state->nextPc))
        return false;
    state->cycles += taken ? 3u : 1u;
    return true;
}

bool applyArmBranch(ArmAluState *state, uint32_t opcode, uint32_t pc)
{
    if (!state)
        return false;
    Ps2ArmAlu::DecodedBranch decoded;
    if (!Ps2ArmAlu::decodeBranch(opcode, &decoded))
        return false;
    bool taken = Ps2ArmAlu::conditionPassed(decoded.condition, state->cpsr);
    if (!Ps2ArmAlu::executeBranch(opcode, pc, state->cpsr, &state->nextPc))
        return false;
    state->cycles += taken ? 3u : 1u;
    return true;
}

bool applyArmSingleBlockTransfer(ArmAluState *state, uint32_t opcode)
{
    if (!state || !state->memory)
        return false;
    Ps2ArmAlu::DecodedSingleBlockTransfer decoded;
    if (!Ps2ArmAlu::decodeSingleBlockTransfer(opcode, &decoded))
        return false;
    bool taken = Ps2ArmAlu::conditionPassed(decoded.condition, state->cpsr);
    if (Ps2ArmAlu::executeSingleBlockTransfer(opcode, state->r, &state->cpsr,
        state, readDirectArm9Word, writeDirectArm9Word))
    {
        state->cycles += taken ? 2u : 1u;
        return true;
    }

    state->memoryFault = 1;
    return false;
}

size_t emitArmCycleAdd(uint32_t amount, uint32_t *code)
{
    if (!code || !amount || amount > 0x7FFFu)
        return 0;
    size_t words = 0;
    code[words++] = R5900::lw(R5900::R_T6, 84, R5900::R_A0);
    code[words++] = R5900::addiu(R5900::R_T6, R5900::R_T6,
        static_cast<int16_t>(amount));
    code[words++] = R5900::sw(R5900::R_T6, 84, R5900::R_A0);
    return words;
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

    // Every ARM instruction consumes one cycle even when its condition fails.
    words += emitArmCycleAdd(1, code + words);

    if (decoded.condition != 0xEu)
    {
        code[words++] = R5900::lui(R5900::R_T2,
            static_cast<uint16_t>(fallthrough >> 16));
        code[words++] = R5900::ori(R5900::R_T2, R5900::R_T2,
            static_cast<uint16_t>(fallthrough));
        words += emitArmCondition(decoded.condition, code + words);

        // Failed condition skips the taken-branch +2 cycle charge and target
        // load, then stores the fallthrough PC.
        code[words++] = R5900::beq(R5900::R_T7, R5900::R_ZERO, 6);
        code[words++] = R5900::nop();
        words += emitArmCycleAdd(2, code + words);
    }
    else
    {
        // Taken B costs three ARM9 cycles total.
        words += emitArmCycleAdd(2, code + words);
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

bool logicalShiftNeedsHelper(const Ps2ArmAlu::Decoded &decoded)
{
    if (!decoded.setFlags)
        return false;
    bool logical = decoded.operation == 0 || decoded.operation == 1 ||
        decoded.operation == 8 || decoded.operation == 9 ||
        decoded.operation == 12 || decoded.operation == 13 ||
        decoded.operation == 14 || decoded.operation == 15;
    if (!logical)
        return false;
    return decoded.immediate
        ? decoded.immediateRotate != 0
        : (decoded.registerShift || decoded.shiftType != 0 ||
            decoded.shiftAmount != 0);
}

size_t emitArmAluHelperInstruction(uint32_t opcode, uint32_t *code)
{
    if (!code)
        return 0;
    uintptr_t helper = reinterpret_cast<uintptr_t>(&applyArmAluHelper);
    size_t words = 0;
    code[words++] = R5900::addiu(R5900::R_SP, R5900::R_SP, -32);
    code[words++] = R5900::sw(R5900::R_RA, 28, R5900::R_SP);
    code[words++] = R5900::sw(R5900::R_A0, 24, R5900::R_SP);
    code[words++] = R5900::lui(R5900::R_A1,
        static_cast<uint16_t>(opcode >> 16));
    code[words++] = R5900::ori(R5900::R_A1, R5900::R_A1,
        static_cast<uint16_t>(opcode));
    code[words++] = R5900::lui(R5900::R_T9,
        static_cast<uint16_t>(helper >> 16));
    code[words++] = R5900::ori(R5900::R_T9, R5900::R_T9,
        static_cast<uint16_t>(helper));
    code[words++] = R5900::jalr(R5900::R_RA, R5900::R_T9);
    code[words++] = R5900::nop();
    code[words++] = R5900::lw(R5900::R_A0, 24, R5900::R_SP);
    code[words++] = R5900::lw(R5900::R_RA, 28, R5900::R_SP);
    code[words++] = R5900::addiu(R5900::R_SP, R5900::R_SP, 32);
    return words;
}

size_t emitArmAluInstruction(uint32_t opcode, uint32_t *code)
{
    if (!code)
        return 0;

    Ps2ArmAlu::Decoded decoded;
    if (!Ps2ArmAlu::decode(opcode, &decoded))
        return 0;
    if (logicalShiftNeedsHelper(decoded))
        return emitArmAluHelperInstruction(opcode, code);

    size_t words = 0;
    words += emitArmCycleAdd(1, code + words);
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

size_t emitArmHalfwordTransferInstruction(uint32_t opcode, uint32_t *code)
{
    Ps2ArmAlu::DecodedHalfwordTransfer decoded;
    if (!code || !Ps2ArmAlu::decodeHalfwordTransfer(opcode, &decoded))
        return 0;

    // Call a tightly bounded C++ helper while the dynarec is shadow-only. It
    // accepts direct ARM9 pages exclusively, so MMIO and cross-page accesses
    // never reach generated code. t9 carries the callee
    // address as required by the MIPS ABI; a0 and ra survive the nested call.
    uintptr_t helper = reinterpret_cast<uintptr_t>(&applyArmHalfwordTransfer);
    size_t words = 0;
    code[words++] = R5900::addiu(R5900::R_SP, R5900::R_SP, -32);
    code[words++] = R5900::sw(R5900::R_RA, 28, R5900::R_SP);
    code[words++] = R5900::sw(R5900::R_A0, 24, R5900::R_SP);
    code[words++] = R5900::lui(R5900::R_A1,
        static_cast<uint16_t>(opcode >> 16));
    code[words++] = R5900::ori(R5900::R_A1, R5900::R_A1,
        static_cast<uint16_t>(opcode));
    code[words++] = R5900::lui(R5900::R_T9,
        static_cast<uint16_t>(helper >> 16));
    code[words++] = R5900::ori(R5900::R_T9, R5900::R_T9,
        static_cast<uint16_t>(helper));
    code[words++] = R5900::jalr(R5900::R_RA, R5900::R_T9);
    code[words++] = R5900::nop();
    code[words++] = R5900::lw(R5900::R_A0, 24, R5900::R_SP);
    code[words++] = R5900::lw(R5900::R_RA, 28, R5900::R_SP);
    code[words++] = R5900::addiu(R5900::R_SP, R5900::R_SP, 32);
    return words;
}

size_t emitArmByteTransferInstruction(uint32_t opcode, uint32_t *code)
{
    Ps2ArmAlu::DecodedByteTransfer decoded;
    if (!code || !Ps2ArmAlu::decodeByteTransfer(opcode, &decoded))
        return 0;

    uintptr_t helper = reinterpret_cast<uintptr_t>(&applyArmByteTransfer);
    size_t words = 0;
    code[words++] = R5900::addiu(R5900::R_SP, R5900::R_SP, -32);
    code[words++] = R5900::sw(R5900::R_RA, 28, R5900::R_SP);
    code[words++] = R5900::sw(R5900::R_A0, 24, R5900::R_SP);
    code[words++] = R5900::lui(R5900::R_A1, static_cast<uint16_t>(opcode >> 16));
    code[words++] = R5900::ori(R5900::R_A1, R5900::R_A1, static_cast<uint16_t>(opcode));
    code[words++] = R5900::lui(R5900::R_T9, static_cast<uint16_t>(helper >> 16));
    code[words++] = R5900::ori(R5900::R_T9, R5900::R_T9, static_cast<uint16_t>(helper));
    code[words++] = R5900::jalr(R5900::R_RA, R5900::R_T9);
    code[words++] = R5900::nop();
    code[words++] = R5900::lw(R5900::R_A0, 24, R5900::R_SP);
    code[words++] = R5900::lw(R5900::R_RA, 28, R5900::R_SP);
    code[words++] = R5900::addiu(R5900::R_SP, R5900::R_SP, 32);
    return words;
}

size_t emitArmLiteralLoadInstruction(uint32_t opcode, uint32_t pc,
    uint32_t *code)
{
    Ps2ArmAlu::DecodedLiteralLoad decoded;
    if (!code || !Ps2ArmAlu::decodeLiteralLoad(opcode, &decoded))
        return 0;

    uintptr_t helper = reinterpret_cast<uintptr_t>(&applyArmLiteralLoad);
    size_t words = 0;
    code[words++] = R5900::addiu(R5900::R_SP, R5900::R_SP, -32);
    code[words++] = R5900::sw(R5900::R_RA, 28, R5900::R_SP);
    code[words++] = R5900::sw(R5900::R_A0, 24, R5900::R_SP);
    code[words++] = R5900::lui(R5900::R_A1, static_cast<uint16_t>(opcode >> 16));
    code[words++] = R5900::ori(R5900::R_A1, R5900::R_A1, static_cast<uint16_t>(opcode));
    code[words++] = R5900::lui(R5900::R_A2, static_cast<uint16_t>(pc >> 16));
    code[words++] = R5900::ori(R5900::R_A2, R5900::R_A2, static_cast<uint16_t>(pc));
    code[words++] = R5900::lui(R5900::R_T9, static_cast<uint16_t>(helper >> 16));
    code[words++] = R5900::ori(R5900::R_T9, R5900::R_T9, static_cast<uint16_t>(helper));
    code[words++] = R5900::jalr(R5900::R_RA, R5900::R_T9);
    code[words++] = R5900::nop();
    code[words++] = R5900::lw(R5900::R_A0, 24, R5900::R_SP);
    code[words++] = R5900::lw(R5900::R_RA, 28, R5900::R_SP);
    code[words++] = R5900::addiu(R5900::R_SP, R5900::R_SP, 32);
    return words;
}

size_t emitArmSingleTransferInstruction(uint32_t opcode, uint32_t *code)
{
    Ps2ArmAlu::DecodedSingleTransfer decoded;
    if (!code || !Ps2ArmAlu::decodeSingleTransfer(opcode, &decoded))
        return 0;

    uintptr_t helper = reinterpret_cast<uintptr_t>(&applyArmSingleTransfer);
    size_t words = 0;
    code[words++] = R5900::addiu(R5900::R_SP, R5900::R_SP, -32);
    code[words++] = R5900::sw(R5900::R_RA, 28, R5900::R_SP);
    code[words++] = R5900::sw(R5900::R_A0, 24, R5900::R_SP);
    code[words++] = R5900::lui(R5900::R_A1,
        static_cast<uint16_t>(opcode >> 16));
    code[words++] = R5900::ori(R5900::R_A1, R5900::R_A1,
        static_cast<uint16_t>(opcode));
    code[words++] = R5900::lui(R5900::R_T9,
        static_cast<uint16_t>(helper >> 16));
    code[words++] = R5900::ori(R5900::R_T9, R5900::R_T9,
        static_cast<uint16_t>(helper));
    code[words++] = R5900::jalr(R5900::R_RA, R5900::R_T9);
    code[words++] = R5900::nop();
    code[words++] = R5900::lw(R5900::R_A0, 24, R5900::R_SP);
    code[words++] = R5900::lw(R5900::R_RA, 28, R5900::R_SP);
    code[words++] = R5900::addiu(R5900::R_SP, R5900::R_SP, 32);
    return words;
}

size_t emitArmSingleBlockTransferInstruction(uint32_t opcode,
    uint32_t *code)
{
    Ps2ArmAlu::DecodedSingleBlockTransfer decoded;
    if (!code || !Ps2ArmAlu::decodeSingleBlockTransfer(opcode, &decoded))
        return 0;

    uintptr_t helper = reinterpret_cast<uintptr_t>(&applyArmSingleBlockTransfer);
    size_t words = 0;
    code[words++] = R5900::addiu(R5900::R_SP, R5900::R_SP, -32);
    code[words++] = R5900::sw(R5900::R_RA, 28, R5900::R_SP);
    code[words++] = R5900::sw(R5900::R_A0, 24, R5900::R_SP);
    code[words++] = R5900::lui(R5900::R_A1,
        static_cast<uint16_t>(opcode >> 16));
    code[words++] = R5900::ori(R5900::R_A1, R5900::R_A1,
        static_cast<uint16_t>(opcode));
    code[words++] = R5900::lui(R5900::R_T9,
        static_cast<uint16_t>(helper >> 16));
    code[words++] = R5900::ori(R5900::R_T9, R5900::R_T9,
        static_cast<uint16_t>(helper));
    code[words++] = R5900::jalr(R5900::R_RA, R5900::R_T9);
    code[words++] = R5900::nop();
    code[words++] = R5900::lw(R5900::R_A0, 24, R5900::R_SP);
    code[words++] = R5900::lw(R5900::R_RA, 28, R5900::R_SP);
    code[words++] = R5900::addiu(R5900::R_SP, R5900::R_SP, 32);
    return words;
}

size_t emitArmBranchExchangeInstruction(uint32_t opcode, uint32_t *code)
{
    Ps2ArmAlu::DecodedBranchExchange decoded;
    if (!code || !Ps2ArmAlu::decodeBranchExchange(opcode, &decoded))
        return 0;

    uintptr_t helper = reinterpret_cast<uintptr_t>(&applyArmBranchExchange);
    size_t words = 0;
    code[words++] = R5900::addiu(R5900::R_SP, R5900::R_SP, -32);
    code[words++] = R5900::sw(R5900::R_RA, 28, R5900::R_SP);
    code[words++] = R5900::sw(R5900::R_A0, 24, R5900::R_SP);
    code[words++] = R5900::lui(R5900::R_A1,
        static_cast<uint16_t>(opcode >> 16));
    code[words++] = R5900::ori(R5900::R_A1, R5900::R_A1,
        static_cast<uint16_t>(opcode));
    code[words++] = R5900::lui(R5900::R_T9,
        static_cast<uint16_t>(helper >> 16));
    code[words++] = R5900::ori(R5900::R_T9, R5900::R_T9,
        static_cast<uint16_t>(helper));
    code[words++] = R5900::jalr(R5900::R_RA, R5900::R_T9);
    code[words++] = R5900::nop();
    code[words++] = R5900::lw(R5900::R_A0, 24, R5900::R_SP);
    code[words++] = R5900::lw(R5900::R_RA, 28, R5900::R_SP);
    code[words++] = R5900::addiu(R5900::R_SP, R5900::R_SP, 32);
    return words;
}

size_t emitArmInstruction(uint32_t opcode, uint32_t pc, uint32_t *code)
{
    Ps2ArmAlu::Decoded decoded;
    if (Ps2ArmAlu::decode(opcode, &decoded))
        return emitArmAluInstruction(opcode, code);
    Ps2ArmAlu::DecodedHalfwordTransfer transfer;
    if (Ps2ArmAlu::decodeHalfwordTransfer(opcode, &transfer))
        return emitArmHalfwordTransferInstruction(opcode, code);
    Ps2ArmAlu::DecodedByteTransfer byteTransfer;
    if (Ps2ArmAlu::decodeByteTransfer(opcode, &byteTransfer))
        return emitArmByteTransferInstruction(opcode, code);
    Ps2ArmAlu::DecodedLiteralLoad literalLoad;
    if (Ps2ArmAlu::decodeLiteralLoad(opcode, &literalLoad))
        return emitArmLiteralLoadInstruction(opcode, pc, code);
    Ps2ArmAlu::DecodedSingleTransfer singleTransfer;
    if (Ps2ArmAlu::decodeSingleTransfer(opcode, &singleTransfer))
        return emitArmSingleTransferInstruction(opcode, code);
    Ps2ArmAlu::DecodedSingleBlockTransfer blockTransfer;
    if (Ps2ArmAlu::decodeSingleBlockTransfer(opcode, &blockTransfer))
        return emitArmSingleBlockTransferInstruction(opcode, code);
    Ps2ArmAlu::DecodedBranchExchange branchExchange;
    if (Ps2ArmAlu::decodeBranchExchange(opcode, &branchExchange))
        return emitArmBranchExchangeInstruction(opcode, code);
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
    block.cycles = 0; // Runtime state records exact condition-dependent cycles.
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

        Ps2ArmAlu::DecodedHalfwordTransfer transfer;
        if (Ps2ArmAlu::decodeHalfwordTransfer(opcodes[i], &transfer))
        {
            block.readMask |= static_cast<uint16_t>(1u << transfer.rn);
            if (!transfer.immediate)
                block.readMask |= static_cast<uint16_t>(1u << transfer.rm);
            if (transfer.load)
                block.writeMask |= static_cast<uint16_t>(1u << transfer.rd);
            else
            {
                block.readMask |= static_cast<uint16_t>(1u << transfer.rd);
                block.writesMemory = true;
            }
            if (transfer.writeBack)
                block.writeMask |= static_cast<uint16_t>(1u << transfer.rn);
            block.readsFlags |= transfer.condition != 0xEu;
            continue;
        }

        Ps2ArmAlu::DecodedByteTransfer byteTransfer;
        if (Ps2ArmAlu::decodeByteTransfer(opcodes[i], &byteTransfer))
        {
            block.readMask |= static_cast<uint16_t>(1u << byteTransfer.rn);
            if (byteTransfer.registerOffset)
                block.readMask |= static_cast<uint16_t>(1u << byteTransfer.rm);
            if (byteTransfer.load)
                block.writeMask |= static_cast<uint16_t>(1u << byteTransfer.rd);
            else
            {
                block.readMask |= static_cast<uint16_t>(1u << byteTransfer.rd);
                block.writesMemory = true;
            }
            if (byteTransfer.writeBack)
                block.writeMask |= static_cast<uint16_t>(1u << byteTransfer.rn);
            block.readsFlags |= byteTransfer.condition != 0xEu;
            continue;
        }

        Ps2ArmAlu::DecodedLiteralLoad literalLoad;
        if (Ps2ArmAlu::decodeLiteralLoad(opcodes[i], &literalLoad))
        {
            block.writeMask |= static_cast<uint16_t>(1u << literalLoad.rd);
            block.readsFlags |= literalLoad.condition != 0xEu;
            continue;
        }

        Ps2ArmAlu::DecodedSingleTransfer singleTransfer;
        if (Ps2ArmAlu::decodeSingleTransfer(opcodes[i], &singleTransfer))
        {
            block.readMask |= static_cast<uint16_t>(1u << singleTransfer.rn);
            if (singleTransfer.registerOffset)
                block.readMask |= static_cast<uint16_t>(1u << singleTransfer.rm);
            if (singleTransfer.load)
                block.writeMask |= static_cast<uint16_t>(1u << singleTransfer.rd);
            else
            {
                block.readMask |= static_cast<uint16_t>(1u << singleTransfer.rd);
                block.writesMemory = true;
            }
            if (singleTransfer.writeBack)
                block.writeMask |= static_cast<uint16_t>(1u << singleTransfer.rn);
            block.readsFlags |= singleTransfer.condition != 0xEu;
            continue;
        }

        Ps2ArmAlu::DecodedSingleBlockTransfer blockTransfer;
        if (Ps2ArmAlu::decodeSingleBlockTransfer(opcodes[i], &blockTransfer))
        {
            block.readMask |= static_cast<uint16_t>(1u << blockTransfer.rn);
            if (blockTransfer.load)
                block.writeMask |= static_cast<uint16_t>(1u << blockTransfer.reg);
            else
            {
                block.readMask |= static_cast<uint16_t>(1u << blockTransfer.reg);
                block.writesMemory = true;
            }
            if (blockTransfer.writeBack)
                block.writeMask |= static_cast<uint16_t>(1u << blockTransfer.rn);
            block.readsFlags |= blockTransfer.condition != 0xEu;
            continue;
        }

        Ps2ArmAlu::DecodedBranchExchange branchExchange;
        if (Ps2ArmAlu::decodeBranchExchange(opcodes[i], &branchExchange))
        {
            if (i + 1 != count)
                return false;
            block.readMask |= static_cast<uint16_t>(1u << branchExchange.rm);
            block.readsFlags |= branchExchange.condition != 0xEu;
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
        { 0xE2121102u, 0x30000013u, 2, 0x80000000u }, // hot ANDS rotated immediate
        { 0xE1B01821u, 0x30000013u, 1, 0x00018000u }, // hot MOVS LSR #16
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
        if (!applyArmBranch(&expected, branchTests[i].opcode,
            branchTests[i].pc))
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

    // Gate direct STRB post-index with transactional rollback.
    const uint32_t byteAddress = 0x02000010u;
    uint8_t *bytePage = memory->getDirectWritePage(0, byteAddress);
    if (!bytePage)
        return false;
    uint8_t byteBefore = bytePage[byteAddress & 0xFFFu];
    uint32_t *byteCode = cache.reserveWords(32);
    if (!byteCode)
        return false;
    size_t byteWords = emitArmByteTransferInstruction(0xE4C01001u,
        byteCode); // STRB r1,[r0],#1
    if (!byteWords)
        return false;
    byteCode[byteWords++] = R5900::jr(R5900::R_RA);
    byteCode[byteWords++] = R5900::nop();
    cache.commitWords(byteWords);
    cache.syncExecutable(byteCode, byteCode + byteWords);
    {
        ArmAluState byteState = {};
        byteState.r[0] = byteAddress;
        byteState.r[1] = 0xABu;
        byteState.memory = memory;
        ArmMemoryWriteGuard byteWrites(&byteState);
        typedef void (*ArmByteFunction)(ArmAluState *);
        ArmByteFunction byteFunction = reinterpret_cast<ArmByteFunction>(byteCode);
        byteFunction(&byteState);
        if (byteState.memoryFault || byteState.writeCount != 1 ||
            byteState.r[0] != byteAddress + 1u ||
            bytePage[byteAddress & 0xFFFu] != 0xABu)
            return false;
    }
    if (bytePage[byteAddress & 0xFFFu] != byteBefore)
        return false;

    // Gate LDR literal with an explicit guest PC+8 base.
    const uint32_t literalPc = 0x02000000u;
    uint8_t *literalPage = memory->getDirectReadPage(0, literalPc + 8u);
    if (!literalPage)
        return false;
    uint32_t literalOffset = (literalPc + 8u) & 0xFFFu;
    uint32_t literalExpected = static_cast<uint32_t>(literalPage[literalOffset]) |
        (static_cast<uint32_t>(literalPage[literalOffset + 1]) << 8) |
        (static_cast<uint32_t>(literalPage[literalOffset + 2]) << 16) |
        (static_cast<uint32_t>(literalPage[literalOffset + 3]) << 24);
    uint32_t *literalCode = cache.reserveWords(32);
    if (!literalCode)
        return false;
    size_t literalWords = emitArmLiteralLoadInstruction(0xE59F1000u,
        literalPc, literalCode);
    if (!literalWords)
        return false;
    literalCode[literalWords++] = R5900::jr(R5900::R_RA);
    literalCode[literalWords++] = R5900::nop();
    cache.commitWords(literalWords);
    cache.syncExecutable(literalCode, literalCode + literalWords);
    ArmAluState literalState = {};
    literalState.r[1] = 0xFFFFFFFFu;
    literalState.memory = memory;
    typedef void (*ArmLiteralFunction)(ArmAluState *);
    ArmLiteralFunction literalFunction =
        reinterpret_cast<ArmLiteralFunction>(literalCode);
    literalFunction(&literalState);
    if (literalState.memoryFault || literalState.r[1] != literalExpected)
        return false;

    // Gate the hot single-register LDMIA/STMIA form on a direct ARM9 page.
    uint32_t *blockLoadCode = cache.reserveWords(32);
    if (!blockLoadCode)
        return false;
    size_t blockLoadWords = emitArmSingleBlockTransferInstruction(
        0xE8B00002u, blockLoadCode); // LDMIA r0!,{r1}
    if (!blockLoadWords)
        return false;
    blockLoadCode[blockLoadWords++] = R5900::jr(R5900::R_RA);
    blockLoadCode[blockLoadWords++] = R5900::nop();
    cache.commitWords(blockLoadWords);
    cache.syncExecutable(blockLoadCode, blockLoadCode + blockLoadWords);

    ArmAluState blockLoadState = {};
    blockLoadState.r[0] = 0x02000000u;
    blockLoadState.r[1] = 0xFFFFFFFFu;
    blockLoadState.memory = memory;
    typedef void (*ArmBlockTransferFunction)(ArmAluState *);
    ArmBlockTransferFunction blockLoadFunction =
        reinterpret_cast<ArmBlockTransferFunction>(blockLoadCode);
    blockLoadFunction(&blockLoadState);
    uint8_t *blockLoadPage = memory->getDirectReadPage(0, 0x02000000u);
    if (!blockLoadPage)
        return false;
    uint32_t blockExpected = static_cast<uint32_t>(blockLoadPage[0]) |
        (static_cast<uint32_t>(blockLoadPage[1]) << 8) |
        (static_cast<uint32_t>(blockLoadPage[2]) << 16) |
        (static_cast<uint32_t>(blockLoadPage[3]) << 24);
    if (blockLoadState.memoryFault || blockLoadState.r[1] != blockExpected ||
        blockLoadState.r[0] != 0x02000004u)
        return false;

    // Gate BX, including ARM<->THUMB state selection, through generated code.
    uint32_t *bxCode = cache.reserveWords(32);
    if (!bxCode)
        return false;
    size_t bxWords = emitArmBranchExchangeInstruction(0xE12FFF1Eu,
        bxCode); // BX lr
    if (!bxWords)
        return false;
    bxCode[bxWords++] = R5900::jr(R5900::R_RA);
    bxCode[bxWords++] = R5900::nop();
    cache.commitWords(bxWords);
    cache.syncExecutable(bxCode, bxCode + bxWords);

    ArmAluState bxState = {};
    bxState.r[14] = 0x02000101u;
    bxState.cpsr = 0x00000013u;
    bxState.nextPc = 0x02000004u;
    typedef void (*ArmBxFunction)(ArmAluState *);
    ArmBxFunction bxFunction = reinterpret_cast<ArmBxFunction>(bxCode);
    bxFunction(&bxState);
    if (bxState.nextPc != 0x02000100u || !(bxState.cpsr & BIT(5)))
        return false;

    // Exercise the non-leaf helper ABI and a real direct ARM9 page before any
    // live block may contain LDRH. This is read-only and therefore invisible
    // to the emulated guest.
    const uint32_t loadAddress = 0x02000000u;
    uint8_t *loadPage = memory->getDirectReadPage(0, loadAddress);
    if (!loadPage)
        return false;
    uint16_t expectedHalfword = static_cast<uint16_t>(loadPage[0] |
        (static_cast<uint16_t>(loadPage[1]) << 8));

    uint32_t *loadCode = cache.reserveWords(32);
    if (!loadCode)
        return false;
    size_t loadWords = emitArmHalfwordTransferInstruction(0xE1D010B0u,
        loadCode); // LDRH r1,[r0]
    if (!loadWords)
        return false;
    loadCode[loadWords++] = R5900::jr(R5900::R_RA);
    loadCode[loadWords++] = R5900::nop();
    cache.commitWords(loadWords);
    cache.syncExecutable(loadCode, loadCode + loadWords);

    ArmAluState loadState = {};
    loadState.r[0] = loadAddress;
    loadState.r[1] = 0xFFFFFFFFu;
    loadState.memory = memory;
    typedef void (*ArmLoadFunction)(ArmAluState *);
    ArmLoadFunction loadFunction = reinterpret_cast<ArmLoadFunction>(loadCode);
    loadFunction(&loadState);
    if (loadState.memoryFault || loadState.r[1] != expectedHalfword)
        return false;

    const uint32_t storeAddress = loadAddress + 2u;
    uint16_t beforeStore = static_cast<uint16_t>(loadPage[2] |
        (static_cast<uint16_t>(loadPage[3]) << 8));
    uint32_t *storeCode = cache.reserveWords(32);
    if (!storeCode)
        return false;
    size_t storeWords = emitArmHalfwordTransferInstruction(0xE1C010B0u,
        storeCode); // STRH r1,[r0]
    if (!storeWords)
        return false;
    storeCode[storeWords++] = R5900::jr(R5900::R_RA);
    storeCode[storeWords++] = R5900::nop();
    cache.commitWords(storeWords);
    cache.syncExecutable(storeCode, storeCode + storeWords);

    {
        ArmAluState storeState = {};
        storeState.r[0] = storeAddress;
        storeState.r[1] = 0xA55Au;
        storeState.memory = memory;
        ArmMemoryWriteGuard storeWrites(&storeState);
        ArmLoadFunction storeFunction =
            reinterpret_cast<ArmLoadFunction>(storeCode);
        storeFunction(&storeState);
        if (storeState.memoryFault || storeState.writeCount != 1 ||
            loadPage[2] != 0x5Au || loadPage[3] != 0xA5u)
            return false;
    }
    if (loadPage[2] != static_cast<uint8_t>(beforeStore) ||
        loadPage[3] != static_cast<uint8_t>(beforeStore >> 8))
        return false;

    // Gate ordinary 32-bit LDR/STR through the same direct-page helper ABI.
    uint32_t expectedWord = static_cast<uint32_t>(loadPage[0]) |
        (static_cast<uint32_t>(loadPage[1]) << 8) |
        (static_cast<uint32_t>(loadPage[2]) << 16) |
        (static_cast<uint32_t>(loadPage[3]) << 24);
    uint32_t *wordLoadCode = cache.reserveWords(32);
    if (!wordLoadCode)
        return false;
    size_t wordLoadWords = emitArmSingleTransferInstruction(0xE5901000u,
        wordLoadCode); // LDR r1,[r0]
    if (!wordLoadWords)
        return false;
    wordLoadCode[wordLoadWords++] = R5900::jr(R5900::R_RA);
    wordLoadCode[wordLoadWords++] = R5900::nop();
    cache.commitWords(wordLoadWords);
    cache.syncExecutable(wordLoadCode, wordLoadCode + wordLoadWords);

    ArmAluState wordLoadState = {};
    wordLoadState.r[0] = loadAddress;
    wordLoadState.r[1] = 0xFFFFFFFFu;
    wordLoadState.memory = memory;
    ArmLoadFunction wordLoadFunction =
        reinterpret_cast<ArmLoadFunction>(wordLoadCode);
    wordLoadFunction(&wordLoadState);
    if (wordLoadState.memoryFault || wordLoadState.r[1] != expectedWord)
        return false;

    uint32_t beforeWordStore = expectedWord;
    uint32_t *wordStoreCode = cache.reserveWords(32);
    if (!wordStoreCode)
        return false;
    size_t wordStoreWords = emitArmSingleTransferInstruction(0xE5801000u,
        wordStoreCode); // STR r1,[r0]
    if (!wordStoreWords)
        return false;
    wordStoreCode[wordStoreWords++] = R5900::jr(R5900::R_RA);
    wordStoreCode[wordStoreWords++] = R5900::nop();
    cache.commitWords(wordStoreWords);
    cache.syncExecutable(wordStoreCode, wordStoreCode + wordStoreWords);

    {
        ArmAluState wordStoreState = {};
        wordStoreState.r[0] = loadAddress;
        wordStoreState.r[1] = 0x89ABCDEFu;
        wordStoreState.memory = memory;
        ArmMemoryWriteGuard wordStoreWrites(&wordStoreState);
        ArmLoadFunction wordStoreFunction =
            reinterpret_cast<ArmLoadFunction>(wordStoreCode);
        wordStoreFunction(&wordStoreState);
        if (wordStoreState.memoryFault || wordStoreState.writeCount != 1 ||
            loadPage[0] != 0xEFu || loadPage[1] != 0xCDu ||
            loadPage[2] != 0xABu || loadPage[3] != 0x89u)
            return false;
    }
    uint32_t restoredWord = static_cast<uint32_t>(loadPage[0]) |
        (static_cast<uint32_t>(loadPage[1]) << 8) |
        (static_cast<uint32_t>(loadPage[2]) << 16) |
        (static_cast<uint32_t>(loadPage[3]) << 24);
    if (restoredWord != beforeWordStore)
        return false;

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
    uint32_t *guestPcOut, uint32_t *instructionCount, uint32_t *stateHash,
    size_t maxInstructions)
{
    if (!CpuBackendAccess::isArm9(cpu) || CpuBackendAccess::isThumb(cpu))
        return 0;

    synchronizeMemoryMap();

    constexpr size_t MAX_BLOCK_INSTRUCTIONS = 16;
    if (!maxInstructions)
        return 0;
    maxInstructions = std::min(maxInstructions, MAX_BLOCK_INSTRUCTIONS);

    uint32_t opcodes[MAX_BLOCK_INSTRUCTIONS] = {};
    size_t count = 0;

    uint32_t guestPc = CpuBackendAccess::nextInstructionAddress(cpu);

    // pipeline[0] is authoritative for the next instruction. pipeline[1] is
    // authoritative for the following instruction; later words can be fetched
    // directly because this gate accepts only straight-line ALU instructions,
    // side-effect-free halfword loads and an optional terminal branch.
    for (size_t i = 0; i < maxInstructions; i++)
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

        Ps2ArmAlu::DecodedHalfwordTransfer transfer;
        if (Ps2ArmAlu::decodeHalfwordTransfer(opcode, &transfer))
        {
            opcodes[count++] = opcode;
            continue;
        }

        Ps2ArmAlu::DecodedByteTransfer byteTransfer;
        if (Ps2ArmAlu::decodeByteTransfer(opcode, &byteTransfer))
        {
            opcodes[count++] = opcode;
            continue;
        }

        Ps2ArmAlu::DecodedLiteralLoad literalLoad;
        if (Ps2ArmAlu::decodeLiteralLoad(opcode, &literalLoad))
        {
            opcodes[count++] = opcode;
            continue;
        }

        Ps2ArmAlu::DecodedSingleTransfer singleTransfer;
        if (Ps2ArmAlu::decodeSingleTransfer(opcode, &singleTransfer))
        {
            opcodes[count++] = opcode;
            continue;
        }

        Ps2ArmAlu::DecodedSingleBlockTransfer blockTransfer;
        if (Ps2ArmAlu::decodeSingleBlockTransfer(opcode, &blockTransfer))
        {
            opcodes[count++] = opcode;
            continue;
        }

        Ps2ArmAlu::DecodedBranchExchange branchExchange;
        if (Ps2ArmAlu::decodeBranchExchange(opcode, &branchExchange))
        {
            opcodes[count++] = opcode;
            break;
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
    source.memory = expected.memory = memory;
    ArmMemoryWriteGuard expectedWrites(&expected);

    for (size_t i = 0; i < count; i++)
    {
        Ps2ArmAlu::Decoded decoded;
        if (Ps2ArmAlu::decode(opcodes[i], &decoded))
        {
            if (!applyArmAlu(opcodes[i], &expected))
                return -1;
        }
        else
        {
            Ps2ArmAlu::DecodedHalfwordTransfer transfer;
            if (Ps2ArmAlu::decodeHalfwordTransfer(opcodes[i], &transfer))
            {
                if (!applyArmHalfwordTransfer(&expected, opcodes[i]))
                    return 0;
            }
            else
            {
                Ps2ArmAlu::DecodedByteTransfer byteTransfer;
                if (Ps2ArmAlu::decodeByteTransfer(opcodes[i], &byteTransfer))
                {
                    if (!applyArmByteTransfer(&expected, opcodes[i]))
                        return 0;
                }
                else
                {
                    Ps2ArmAlu::DecodedLiteralLoad literalLoad;
                    if (Ps2ArmAlu::decodeLiteralLoad(opcodes[i], &literalLoad))
                    {
                        if (!applyArmLiteralLoad(&expected, opcodes[i],
                            guestPc + static_cast<uint32_t>(i * 4)))
                            return 0;
                    }
                    else
                    {
                        Ps2ArmAlu::DecodedSingleTransfer singleTransfer;
                        if (Ps2ArmAlu::decodeSingleTransfer(opcodes[i], &singleTransfer))
                        {
                            if (!applyArmSingleTransfer(&expected, opcodes[i]))
                                return 0;
                        }
                        else
                        {
                            Ps2ArmAlu::DecodedSingleBlockTransfer blockTransfer;
                            if (Ps2ArmAlu::decodeSingleBlockTransfer(opcodes[i],
                                &blockTransfer))
                            {
                                if (!applyArmSingleBlockTransfer(&expected, opcodes[i]))
                                    return 0;
                            }
                            else
                            {
                                Ps2ArmAlu::DecodedBranchExchange branchExchange;
                                if (Ps2ArmAlu::decodeBranchExchange(opcodes[i],
                                    &branchExchange))
                                {
                                    if (!applyArmBranchExchange(&expected, opcodes[i]))
                                        return -1;
                                }
                                else if (!applyArmBranch(&expected, opcodes[i],
                                    guestPc + static_cast<uint32_t>(i * 4)))
                                {
                                    return -1;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Generated code must observe the same original bytes as the portable
    // shadow. Keep the journal for comparison, but roll its writes back first.
    expectedWrites.restoreNow();

    uint32_t *entry = nullptr;
    uint32_t hostWords = 0;
    if (!emitArmAluSequence(opcodes, count, guestPc, &entry, &hostWords))
        return -1;

    ArmAluState actual = source;
    ArmMemoryWriteGuard actualWrites(&actual);
    typedef void (*ArmAluBlockFunction)(ArmAluState *);
    ArmAluBlockFunction function = reinterpret_cast<ArmAluBlockFunction>(entry);
    function(&actual);
    if (actual.memoryFault)
        return 0;

    if (actual.writeCount != expected.writeCount)
        return -1;
    for (uint32_t i = 0; i < actual.writeCount; i++)
    {
        if (actual.writes[i].address != expected.writes[i].address ||
            actual.writes[i].before != expected.writes[i].before ||
            actual.writes[i].after != expected.writes[i].after ||
            actual.writes[i].size != expected.writes[i].size)
            return -1;
    }

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
    if (actual.cycles != expected.cycles)
        return -1;
    hash ^= actual.cycles;
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
    R5900CompiledBlock *block = findMutableBlock(guestPc, false);
    if (!block || !block->hostCode || block->guestInstructions == 0)
        return 0;
    if (block->writesMemory)
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
    dynarecState.memory = memory;

    typedef void (*ArmAluBlockFunction)(ArmAluState *);
    ArmAluBlockFunction function =
        reinterpret_cast<ArmAluBlockFunction>(block->hostCode);
    function(&dynarecState);
    if (dynarecState.memoryFault)
        return 0;

    bool interpreterOk = true;
    uint32_t interpreterCycles = 0;
    for (uint32_t i = 0; i < block->guestInstructions; i++)
    {
        int cycles = cpu.runOpcode();
        if (cycles <= 0)
        {
            interpreterOk = false;
            break;
        }
        interpreterCycles += static_cast<uint32_t>(cycles);
    }

    uint32_t hash = 2166136261u;
    bool match = interpreterOk &&
        CpuBackendAccess::readCpsr(cpu) == dynarecState.cpsr &&
        CpuBackendAccess::nextInstructionAddress(cpu) == dynarecState.nextPc &&
        interpreterCycles == dynarecState.cycles;

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
    hash ^= interpreterCycles;
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

    block->differentialVerified = match;
    return match ? 1 : -1;
}


int DynarecR5900::runArm9ActiveBlock(Interpreter &cpu, uint32_t cycleBudget,
    uint32_t *guestInstructions)
{
    if (guestInstructions)
        *guestInstructions = 0;

    profile.activeAttempts++;
    if (!cycleBudget || !CpuBackendAccess::isArm9(cpu) ||
        CpuBackendAccess::isThumb(cpu))
    {
        profile.activeStateFallbacks++;
        return 0;
    }

    synchronizeMemoryMap();

    uint32_t guestPc = CpuBackendAccess::nextInstructionAddress(cpu);
    R5900CompiledBlock *block = findMutableBlock(guestPc, false);

    if (!block)
    {
        if (std::find(activeRejectedPcs.begin(), activeRejectedPcs.end(),
            guestPc) != activeRejectedPcs.end())
        {
            profile.activeVerifyFallbacks++;
            return 0;
        }

        // Bound first-use validation work and generated-code memory. One
        // instruction costs at least one ARM9 cycle, so a block longer than
        // cycleBudget could never commit in this scheduler slot.
        if (blocks.size() >= 256)
        {
            profile.activeVerifyFallbacks++;
            return 0;
        }

        profile.activeCompileMisses++;
        size_t maxInstructions = std::min<size_t>(16,
            static_cast<size_t>(cycleBudget));
        int shadow = runArm9BlockShadowSelfTest(cpu, nullptr, nullptr, nullptr,
            maxInstructions);
        if (shadow <= 0)
        {
            activeRejectedPcs.push_back(guestPc);
            profile.activeVerifyFallbacks++;
            return 0;
        }

        block = findMutableBlock(guestPc, false);
        if (!block)
        {
            activeRejectedPcs.push_back(guestPc);
            profile.activeVerifyFallbacks++;
            return 0;
        }
    }

    // The first active milestone is intentionally transactional/read-only.
    // Direct stores already pass the shadow gate, but stay on the interpreter
    // until live write journaling and rollback are proven in scheduler use.
    if (block->writesMemory)
    {
        profile.activeWriteFallbacks++;
        return 0;
    }

    // Generated blocks that explicitly read/write architectural R15 are kept
    // out of live execution. Literal LDR has its own PC+8 helper and therefore
    // does not set this mask bit.
    if ((block->readMask | block->writeMask) & 0x8000u)
    {
        profile.activeStateFallbacks++;
        return 0;
    }

    if (!block->differentialVerified)
    {
        int differential = runArm9InterpreterDifferentialSelfTest(cpu);
        if (differential <= 0)
        {
            profile.activeVerifyFallbacks++;
            return 0;
        }

        block = findMutableBlock(guestPc, false);
        if (!block || !block->differentialVerified)
        {
            profile.activeVerifyFallbacks++;
            return 0;
        }
    }

    ArmAluState state = {};
    for (int i = 0; i < 16; i++)
        state.r[i] = CpuBackendAccess::readReg(cpu, i);
    state.cpsr = CpuBackendAccess::readCpsr(cpu);
    state.nextPc = block->guestEndPc;
    state.memory = memory;

    typedef void (*ArmAluBlockFunction)(ArmAluState *);
    ArmAluBlockFunction function =
        reinterpret_cast<ArmAluBlockFunction>(block->hostCode);
    function(&state);

    // No guest state has been committed yet, so every failure below is a
    // lossless interpreter fallback.
    if (state.memoryFault || state.writeCount ||
        ((state.cpsr ^ CpuBackendAccess::readCpsr(cpu)) & 0x1Fu))
    {
        profile.activeStateFallbacks++;
        return 0;
    }

    if (!state.cycles || state.cycles > cycleBudget)
    {
        profile.activeBudgetFallbacks++;
        return 0;
    }

    if (!CpuBackendAccess::commitBlockState(cpu, state.r, state.cpsr,
        state.nextPc))
    {
        profile.activeStateFallbacks++;
        return 0;
    }

    profile.activeExecutions++;
    profile.activeGuestInstructions += block->guestInstructions;
    profile.activeCycles += state.cycles;
    if (guestInstructions)
        *guestInstructions = block->guestInstructions;

    return static_cast<int>(state.cycles);
}
