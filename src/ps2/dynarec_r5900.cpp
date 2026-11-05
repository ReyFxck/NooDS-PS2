#include "../memory.h"
#include "../cpu_backend.h"
#include "arm_alu.h"
#include "dynarec_r5900.h"
#include "r5900_emitter.h"

#include <algorithm>
#include <cstring>

#ifdef NOODS_PROFILE
#define DYNAREC_PROFILE(statement) do { statement; } while (0)
#else
#define DYNAREC_PROFILE(statement) do { } while (0)
#endif

namespace
{
#ifdef NOODS_PROFILE
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

void recordActiveFallback(R5900DynarecProfile &profile, uint32_t pc,
    R5900ActiveFallbackReason reason)
{
#if NOODS_PROFILE_LEVEL < 2
    (void)profile;
    (void)pc;
    (void)reason;
    return;
#else
    // Heavy-hitter attribution is diagnostic, not an exact counter. Sample
    // one fallback in 64 so pathological loops cannot make profiling itself
    // dominate the JIT dispatch path.
    if ((++profile.activeFallbackSampleCounter & 63u) != 0)
        return;

    for (size_t i = 0; i < 16; i++)
    {
        R5900ActiveFallbackProfile &entry = profile.topActiveFallback[i];
        if (entry.count && entry.pc == pc && entry.reason == reason)
        {
            entry.count++;
            return;
        }
        if (!entry.count)
        {
            entry.pc = pc;
            entry.reason = static_cast<uint8_t>(reason);
            entry.count = 1;
            return;
        }
    }

    // Bounded heavy-hitter reduction, matching topFallback[].
    for (size_t i = 0; i < 16; i++)
        profile.topActiveFallback[i].count--;
#endif
}
#endif
}

DynarecR5900::DynarecR5900(Memory *memory, uint8_t targetCpuId):
    memory(memory), targetCpuId(targetCpuId ? 1u : 0u)
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

void DynarecR5900::clearActiveLookups()
{
    std::memset(activeBlockPc, 0, sizeof(activeBlockPc));
    std::memset(activeBlockIndex, 0, sizeof(activeBlockIndex));
    std::memset(activeRejectedPc, 0, sizeof(activeRejectedPc));
    std::memset(activeRejectedKind, 0, sizeof(activeRejectedKind));
    std::memset(compiledGuestPageBloom, 0, sizeof(compiledGuestPageBloom));
    std::memset(compiledPhysicalPageBloom, 0,
        sizeof(compiledPhysicalPageBloom));
}

void DynarecR5900::cacheActiveReject(uint32_t pc, ActiveRejectKind kind)
{
    size_t slot = activeLookupSlot(pc);
    activeRejectedPc[slot] = pc;
    activeRejectedKind[slot] = static_cast<uint8_t>(kind);
}

DynarecR5900::ActiveRejectKind DynarecR5900::findActiveReject(
    uint32_t pc) const
{
    size_t slot = activeLookupSlot(pc);
    if (activeRejectedKind[slot] && activeRejectedPc[slot] == pc)
        return static_cast<ActiveRejectKind>(activeRejectedKind[slot]);
    return ACTIVE_REJECT_NONE;
}

void DynarecR5900::shutdown()
{
    blocks.clear();
    clearActiveLookups();
    cache.shutdown();
    mappedGeneration = 0;
    probeCode = nullptr;
}

void DynarecR5900::reset()
{
    blocks.clear();
    clearActiveLookups();
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
    DYNAREC_PROFILE(profile.mapInvalidations++);
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
    // The active executor currently handles ARM only. Keep the generic linear
    // path for THUMB/future callers, while ARM gets a direct-mapped fast path.
    if (!thumb)
    {
        size_t slot = activeLookupSlot(pc);
        uint16_t indexPlusOne = activeBlockIndex[slot];
        if (indexPlusOne && activeBlockPc[slot] == pc)
        {
            size_t index = static_cast<size_t>(indexPlusOne - 1);
            if (index < blocks.size())
            {
                R5900CompiledBlock &block = blocks[index];
                if (block.guestPc == pc && !block.thumb &&
                    block.mapGeneration == mappedGeneration)
                    return &block;
            }

            activeBlockIndex[slot] = 0;
        }
    }

    // Search newest-first on a direct-cache miss. A PC may have an older,
    // shorter shadow block plus a newer active compilation; collisions in the
    // direct lookup must not resurrect the stale/partial version.
    for (size_t end = blocks.size(); end > 0; end--)
    {
        size_t i = end - 1;
        if (blocks[i].guestPc == pc && blocks[i].thumb == thumb &&
            blocks[i].mapGeneration == mappedGeneration)
        {
            if (!thumb && i < 0xFFFFu)
            {
                size_t slot = activeLookupSlot(pc);
                activeBlockPc[slot] = pc;
                activeBlockIndex[slot] = static_cast<uint16_t>(i + 1);
            }
            return &blocks[i];
        }
    }

    return nullptr;
}

namespace
{
void addCompiledPageBloom(uint64_t bloom[4], uint64_t key)
{
    uint64_t mixed1 = key ^ (key >> 17) ^ (key * 0x9E3779B185EBCA87ull);
    uint64_t mixed2 = (key >> 7) ^ (key * 0xC2B2AE3D27D4EB4Full);
    uint32_t bit1 = static_cast<uint32_t>(mixed1) & 0xFFu;
    uint32_t bit2 = static_cast<uint32_t>(mixed2) & 0xFFu;
    bloom[bit1 >> 6] |= 1ull << (bit1 & 63u);
    bloom[bit2 >> 6] |= 1ull << (bit2 & 63u);
}

bool maybeCompiledPageBloom(const uint64_t bloom[4], uint64_t key)
{
    uint64_t mixed1 = key ^ (key >> 17) ^ (key * 0x9E3779B185EBCA87ull);
    uint64_t mixed2 = (key >> 7) ^ (key * 0xC2B2AE3D27D4EB4Full);
    uint32_t bit1 = static_cast<uint32_t>(mixed1) & 0xFFu;
    uint32_t bit2 = static_cast<uint32_t>(mixed2) & 0xFFu;
    return (bloom[bit1 >> 6] & (1ull << (bit1 & 63u))) &&
        (bloom[bit2 >> 6] & (1ull << (bit2 & 63u)));
}
}

void DynarecR5900::rememberCompiledCodePages(
    const R5900CompiledBlock &block)
{
    if (!memory || block.guestEndPc <= block.guestPc)
        return;

    bool cpu = targetCpuId != 0;
    uint32_t address = block.guestPc;
    while (address < block.guestEndPc)
    {
        uint32_t guestPage = address >> 12;
        addCompiledPageBloom(compiledGuestPageBloom, guestPage);

        uint8_t *page = memory->getDirectReadPage(cpu, address);
        if (page)
        {
            uint64_t physicalKey =
                static_cast<uint64_t>(reinterpret_cast<uintptr_t>(page)) >> 12;
            addCompiledPageBloom(compiledPhysicalPageBloom, physicalKey);
        }

        uint32_t next = (address & ~0xFFFu) + 0x1000u;
        if (next <= address)
            break;
        address = std::min(next, block.guestEndPc);
    }
}

bool DynarecR5900::writeOverlapsCompiledCode(
    uint32_t address, uint8_t size) const
{
    if (!memory || !size || blocks.empty())
        return false;

    bool cpu = targetCpuId != 0;
    uint8_t *writePage = memory->getDirectWritePage(cpu, address);
    uint64_t guestKey = static_cast<uint64_t>(address >> 12);
    bool guestMaybe = maybeCompiledPageBloom(compiledGuestPageBloom, guestKey);
    bool physicalMaybe = false;
    if (writePage)
    {
        uint64_t physicalKey =
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(writePage)) >> 12;
        physicalMaybe =
            maybeCompiledPageBloom(compiledPhysicalPageBloom, physicalKey);
    }

    if (!guestMaybe && !physicalMaybe)
        return false;

    uint64_t writeStart = address;
    uint64_t writeEnd = writeStart + size;
    for (const R5900CompiledBlock &block : blocks)
    {
        uint64_t codeStart = block.guestPc;
        uint64_t codeEnd = block.guestEndPc;
        if (writeStart < codeEnd && writeEnd > codeStart)
            return true;

        if (!writePage)
            continue;

        uint32_t codeAddress = block.guestPc;
        while (codeAddress < block.guestEndPc)
        {
            uint8_t *codePage =
                memory->getDirectReadPage(cpu, codeAddress);
            uint32_t pageBase = codeAddress & ~0xFFFu;
            uint32_t pageEnd = std::min<uint32_t>(
                block.guestEndPc, pageBase + 0x1000u);

            if (codePage && codePage == writePage)
            {
                uint32_t writeOffset = address & 0xFFFu;
                uint32_t writeOffsetEnd = writeOffset + size;
                uint32_t codeOffset = codeAddress & 0xFFFu;
                uint32_t codeOffsetEnd = pageEnd - pageBase;
                if (writeOffset < codeOffsetEnd &&
                    writeOffsetEnd > codeOffset)
                    return true;
            }

            if (pageEnd <= codeAddress)
                break;
            codeAddress = pageEnd;
        }
    }
    return false;
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
#ifdef NOODS_PROFILE
    profile.compileAttempts++;
    // This lookup is telemetry-only. Do not pay a linear blocks[] scan in
    // RELEASE/DEBUG merely to classify the analysis as a hit or miss.
    if (findBlock(pc, false))
        profile.lookupHits++;
    else
        profile.lookupMisses++;
#endif

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

        Ps2ArmAlu::DecodedHalfwordTransfer halfTransfer;
        Ps2ArmAlu::DecodedByteTransfer byteTransfer;
        Ps2ArmAlu::DecodedSingleTransfer singleTransfer;
        Ps2ArmAlu::DecodedSingleBlockTransfer blockTransfer;
        Ps2ArmAlu::DecodedMultiBlockTransfer multiBlockTransfer;
        if ((Ps2ArmAlu::decodeHalfwordTransfer(instruction.opcode,
                &halfTransfer) && !halfTransfer.load) ||
            (Ps2ArmAlu::decodeByteTransfer(instruction.opcode,
                &byteTransfer) && !byteTransfer.load) ||
            (Ps2ArmAlu::decodeSingleTransfer(instruction.opcode,
                &singleTransfer) && !singleTransfer.load) ||
            (Ps2ArmAlu::decodeSingleBlockTransfer(instruction.opcode,
                &blockTransfer) && !blockTransfer.load) ||
            (Ps2ArmAlu::decodeMultiBlockTransfer(instruction.opcode,
                &multiBlockTransfer) && !multiBlockTransfer.load))
        {
            analysis->writesMemory = true;
        }

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
#ifdef NOODS_PROFILE
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

            uint32_t rejectedOpcode = unsupported.opcode;
            bool longMultiply =
                (rejectedOpcode & 0x0F8000F0u) == 0x00800090u;
            bool extraTransfer =
                (rejectedOpcode & 0x0E000090u) == 0x00000090u;
            uint8_t extraType =
                static_cast<uint8_t>((rejectedOpcode >> 5) & 0x3u);
            bool signedLoad = extraTransfer &&
                (rejectedOpcode & BIT(20)) &&
                (extraType == 2u || extraType == 3u);
            if (longMultiply)
                profile.longMultiplyRejects++;
            if (signedLoad)
                profile.signedLoadRejects++;
        }
        profile.rejectReason[reason]++;
    }
#endif

    return analysis->instructionCount != 0;
}

bool DynarecR5900::isDirectArmHalfwordAccess(uint32_t opcode,
    uint32_t address) const
{
    Ps2ArmAlu::DecodedHalfwordTransfer transfer;
    if (!memory || !Ps2ArmAlu::decodeHalfwordTransfer(opcode, &transfer))
        return false;
    if (!transfer.byteLoad && (address & 0xFFFu) == 0xFFFu)
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
    uint32_t readCacheTag;
    uint8_t *readCachePage;
    uint32_t cycleLimit;
    uint32_t guestExecuted;
    uint32_t writeCacheTag;
    uint8_t *writeCachePage;
    ArmMemoryWrite writes[16];

    // Kept after the generated-code ABI payload so every existing R5900
    // offset remains unchanged. 0 = ARM9, 1 = ARM7.
    uint32_t cpuId;
};
static_assert(offsetof(ArmAluState, cpsr) == 64,
    "R5900 ARM-state CPSR ABI offset");
static_assert(offsetof(ArmAluState, nextPc) == 68,
    "R5900 ARM-state next-PC ABI offset");
static_assert(offsetof(ArmAluState, cycles) == 84,
    "R5900 ARM-state cycle ABI offset");
static_assert(offsetof(ArmAluState, readCacheTag) == 88,
    "R5900 ARM-state read-cache tag ABI offset");
static_assert(offsetof(ArmAluState, readCachePage) == 92,
    "R5900 ARM-state read-cache page ABI offset");
static_assert(offsetof(ArmAluState, cycleLimit) == 96,
    "R5900 ARM-state cycle-limit ABI offset");
static_assert(offsetof(ArmAluState, guestExecuted) == 100,
    "R5900 ARM-state guest-executed ABI offset");
static_assert(offsetof(ArmAluState, writeCacheTag) == 104,
    "R5900 ARM-state write-cache tag ABI offset");
static_assert(offsetof(ArmAluState, writeCachePage) == 108,
    "R5900 ARM-state write-cache page ABI offset");
static_assert(offsetof(ArmAluState, writes) == 112,
    "R5900 ARM-state write-journal ABI offset");
static_assert(offsetof(ArmAluState, cpuId) == 368,
    "R5900 ARM-state CPU-ID ABI offset");
static_assert(sizeof(ArmMemoryWrite) == 16,
    "R5900 ARM write-journal entry ABI size");

bool applyArmAlu(uint32_t opcode, ArmAluState *state)
{
    Ps2ArmAlu::Decoded decoded;
    if (!state || !Ps2ArmAlu::decode(opcode, &decoded))
        return false;

    bool passed = Ps2ArmAlu::conditionPassed(decoded.condition, state->cpsr);
    if (!Ps2ArmAlu::execute(opcode, state->r, &state->cpsr))
        return false;

    // The interpreter checks the ARM condition before decoding the operand.
    // A register-specified shift therefore costs its extra cycle only when the
    // instruction actually executes; a failed condition costs exactly 1.
    state->cycles += (passed && decoded.registerShift) ? 2u : 1u;
    return true;
}

uint32_t arm7MultiplyM(int32_t value)
{
    if (value >= -256 && value < 256)
        return 1u;
    if (value >= -65536 && value < 65536)
        return 2u;
    if (value >= -16777216 && value < 16777216)
        return 3u;
    return 4u;
}

bool applyArmPsrTransfer(uint32_t opcode, ArmAluState *state)
{
    Ps2ArmAlu::DecodedPsrTransfer decoded;
    if (!state || !Ps2ArmAlu::decodePsrTransfer(opcode, &decoded))
        return false;

    const bool passed =
        Ps2ArmAlu::conditionPassed(decoded.condition, state->cpsr);
    if (!passed)
    {
        state->cycles += 1u;
        return true;
    }

    if (decoded.write)
    {
        const uint32_t oldCpsr = state->cpsr;
        uint32_t nextCpsr = oldCpsr;
        if (!Ps2ArmAlu::executePsrTransfer(
            opcode, state->r, &nextCpsr))
            return false;

        // ArmAluState has no banked-register ABI. Let the interpreter own
        // actual mode switches; same-mode CPSR control/IRQ/flag updates are
        // safe to commit at this terminal block boundary.
        if ((nextCpsr ^ oldCpsr) & 0x1Fu)
        {
            state->memoryFault = 1u;
            state->cycles += 1u;
            return true;
        }
        state->cpsr = nextCpsr;
        state->cycles += 1u;
        return true;
    }

    if (!Ps2ArmAlu::executePsrTransfer(opcode, state->r, &state->cpsr))
        return false;
    state->cycles += state->cpuId ? 1u : 2u;
    return true;
}

bool applyArmMultiply(uint32_t opcode, ArmAluState *state)
{
    Ps2ArmAlu::DecodedMultiply decoded;
    if (!state || !Ps2ArmAlu::decodeMultiply(opcode, &decoded))
        return false;

    bool passed = Ps2ArmAlu::conditionPassed(decoded.condition, state->cpsr);
    int32_t multiplier = static_cast<int32_t>(state->r[decoded.rs]);
    if (!Ps2ArmAlu::executeMultiply(opcode, state->r, &state->cpsr))
        return false;

    if (!passed)
    {
        state->cycles += 1u;
    }
    else if (state->cpuId)
    {
        // Match Interpreter::mul/mla/muls/mlas on ARM7. Rs determines m from
        // multiplier early termination: upper [31:8]/[31:16]/[31:24] bits
        // must all be sign bits for m=1/2/3. S changes flags but adds no cycles.
        uint32_t m = arm7MultiplyM(multiplier);
        state->cycles += m + 1u + (decoded.accumulate ? 1u : 0u);
    }
    else
    {
        // ARM9 has the existing fixed timing model.
        state->cycles += decoded.setFlags ? 4u : 2u;
    }
    return true;
}

bool applyArmLongMultiply(uint32_t opcode, ArmAluState *state)
{
    Ps2ArmAlu::DecodedLongMultiply decoded;
    if (!state || !Ps2ArmAlu::decodeLongMultiply(opcode, &decoded))
        return false;

    bool passed = Ps2ArmAlu::conditionPassed(decoded.condition, state->cpsr);
    int32_t multiplier = static_cast<int32_t>(state->r[decoded.rs]);
    if (!Ps2ArmAlu::executeLongMultiply(opcode, state->r, &state->cpsr))
        return false;

    if (!passed)
    {
        state->cycles += 1u;
    }
    else if (state->cpuId)
    {
        uint32_t m = arm7MultiplyM(multiplier);
        state->cycles += m + 2u + (decoded.accumulate ? 1u : 0u);
    }
    else
    {
        state->cycles += decoded.setFlags ? 5u : 3u;
    }
    return true;
}

uint8_t *getArm9CachedReadPage(ArmAluState *state, uint32_t address)
{
    if (!state || !state->memory)
        return nullptr;

    uint32_t tag = (address >> 12) + 1u;
    if (state->readCacheTag == tag && state->readCachePage)
        return state->readCachePage;

    uint8_t *page = state->memory->getDirectReadPageCached(state->cpuId != 0, address);
    if (!page)
        return nullptr;

    state->readCacheTag = tag;
    state->readCachePage = page;
    return page;
}

bool readDirectArm9Halfword(void *context, uint32_t address, uint16_t *value)
{
    ArmAluState *state = static_cast<ArmAluState*>(context);
    Memory *memory = state ? state->memory : nullptr;
    uint32_t offset = address & 0xFFFu;
    if (!memory || !value || offset == 0xFFFu)
        return false;

    uint8_t *page = getArm9CachedReadPage(state, address);
    if (!page)
        return false;
    *value = static_cast<uint16_t>(page[offset] |
        (static_cast<uint16_t>(page[offset + 1]) << 8));
    return true;
}

uint32_t refillArm9ByteReadCache(ArmAluState *state,
    uint32_t address)
{
    if (!state || !state->memory)
    {
        if (state)
            state->memoryFault = 1;
        return 0;
    }

    uint8_t *page = state->memory->getDirectReadPageCached(state->cpuId != 0, address);
    if (!page)
    {
        state->memoryFault = 1;
        return 0;
    }

    state->readCacheTag = (address >> 12) + 1u;
    state->readCachePage = page;
    return page[address & 0xFFFu];
}

uint32_t refillArm9WordReadCache(ArmAluState *state,
    uint32_t address)
{
    if (!state || !state->memory || (address & 3u) ||
        (address & 0xFFFu) > 0xFFCu)
    {
        if (state)
            state->memoryFault = 1;
        return 0;
    }

    uint8_t *page = state->memory->getDirectReadPageCached(state->cpuId != 0, address);
    if (!page)
    {
        state->memoryFault = 1;
        return 0;
    }

    state->readCacheTag = (address >> 12) + 1u;
    state->readCachePage = page;
    uint32_t offset = address & 0xFFFu;
    return static_cast<uint32_t>(page[offset]) |
        (static_cast<uint32_t>(page[offset + 1]) << 8) |
        (static_cast<uint32_t>(page[offset + 2]) << 16) |
        (static_cast<uint32_t>(page[offset + 3]) << 24);
}

uint32_t refillArm9HalfwordReadCache(ArmAluState *state,
    uint32_t address)
{
    if (!state || !state->memory || (address & 1u) ||
        (address & 0xFFFu) == 0xFFFu)
    {
        if (state)
            state->memoryFault = 1;
        return 0;
    }

    uint8_t *page = state->memory->getDirectReadPageCached(state->cpuId != 0, address);
    if (!page)
    {
        state->memoryFault = 1;
        return 0;
    }

    state->readCacheTag = (address >> 12) + 1u;
    state->readCachePage = page;
    uint32_t offset = address & 0xFFFu;
    return static_cast<uint32_t>(page[offset]) |
        (static_cast<uint32_t>(page[offset + 1]) << 8);
}

uint8_t *getArm9CachedWritePage(ArmAluState *state, uint32_t address)
{
    if (!state || !state->memory)
        return nullptr;

    uint32_t tag = (address >> 12) + 1u;
    if (state->writeCacheTag == tag && state->writeCachePage)
        return state->writeCachePage;

    uint8_t *page = state->memory->getDirectWritePageCached(state->cpuId != 0, address);
    if (!page)
        return nullptr;

    state->writeCacheTag = tag;
    state->writeCachePage = page;
    return page;
}

bool writeDirectArm9Halfword(void *context, uint32_t address, uint16_t value)
{
    ArmAluState *state = static_cast<ArmAluState*>(context);
    Memory *memory = state ? state->memory : nullptr;
    uint32_t offset = address & 0xFFFu;
    if (!memory || offset == 0xFFFu || state->writeCount >= 16)
        return false;

    uint8_t *page = getArm9CachedWritePage(state, address);
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
        uint8_t *page = state->memory->getDirectWritePageCached(state->cpuId != 0, write.address);
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

        void dismiss() { active = false; }

    private:
        ArmAluState *state;
        bool active = true;
};

// Store helpers are defined below their matching read helpers. Keep the
// predecoded store fast path independent of source ordering.
bool writeDirectArm9Byte(void *context, uint32_t address, uint8_t value);
bool writeDirectArm9Word(void *context, uint32_t address, uint32_t value);

bool applyArmHalfwordRegisterStoreFast(ArmAluState *state,
    uint32_t packed)
{
    if (!state || !state->memory)
        return false;

    uint8_t rn = static_cast<uint8_t>(packed & 0xFu);
    uint8_t rd = static_cast<uint8_t>((packed >> 4) & 0xFu);
    uint8_t rm = static_cast<uint8_t>((packed >> 8) & 0xFu);
    bool addOffset = (packed & BIT(12)) != 0;
    bool preIndex = (packed & BIT(13)) != 0;
    bool writeBack = (packed & BIT(14)) != 0;

    uint32_t base = state->r[rn];
    uint32_t offset = state->r[rm];
    uint32_t adjusted = addOffset ? base + offset : base - offset;
    uint32_t address = preIndex ? adjusted : base;

    if (!writeDirectArm9Halfword(state, address,
        static_cast<uint16_t>(state->r[rd])))
    {
        state->memoryFault = 1;
        return false;
    }

    if (writeBack)
        state->r[rn] = adjusted;
    return true;
}

bool applyArmRegisterStoreFast(ArmAluState *state, uint32_t packed)
{
    if (!state || !state->memory)
        return false;

    uint8_t rn = static_cast<uint8_t>(packed & 0xFu);
    uint8_t rd = static_cast<uint8_t>((packed >> 4) & 0xFu);
    uint8_t rm = static_cast<uint8_t>((packed >> 8) & 0xFu);
    bool addOffset = (packed & BIT(12)) != 0;
    bool writeBack = (packed & BIT(13)) != 0;
    bool word = (packed & BIT(14)) != 0;
    uint8_t shiftType = static_cast<uint8_t>((packed >> 15) & 3u);
    uint8_t shiftAmount = static_cast<uint8_t>((packed >> 17) & 0x1Fu);
    bool preIndex = (packed & BIT(22)) != 0;

    uint32_t offset = 0;
    if (!Ps2ArmAlu::applyImmediateShift(state->r[rm], shiftType,
        shiftAmount, &offset))
    {
        state->memoryFault = 1;
        return false;
    }

    uint32_t base = state->r[rn];
    uint32_t adjusted = addOffset ? base + offset : base - offset;
    uint32_t address = preIndex ? adjusted : base;
    bool ok = word
        ? writeDirectArm9Word(state, address, state->r[rd])
        : writeDirectArm9Byte(state, address,
            static_cast<uint8_t>(state->r[rd]));
    if (!ok)
    {
        state->memoryFault = 1;
        return false;
    }

    if (writeBack)
        state->r[rn] = adjusted;
    return true;
}

bool applyArmImmediateStoreFast(ArmAluState *state, uint32_t packed)
{
    if (!state || !state->memory)
        return false;

    uint8_t rn = static_cast<uint8_t>(packed & 0xFu);
    uint8_t rd = static_cast<uint8_t>((packed >> 4) & 0xFu);
    uint32_t offset = (packed >> 8) & 0xFFFu;
    bool addOffset = (packed & BIT(20)) != 0;
    bool preIndex = (packed & BIT(21)) != 0;
    bool writeBack = (packed & BIT(22)) != 0;
    uint8_t sizeCode = static_cast<uint8_t>((packed >> 23) & 3u);

    uint32_t base = state->r[rn];
    uint32_t adjusted = addOffset ? base + offset : base - offset;
    uint32_t address = preIndex ? adjusted : base;
    bool ok = false;

    switch (sizeCode)
    {
        case 0:
            ok = writeDirectArm9Byte(state, address,
                static_cast<uint8_t>(state->r[rd]));
            break;
        case 1:
            ok = writeDirectArm9Halfword(state, address,
                static_cast<uint16_t>(state->r[rd]));
            break;
        case 2:
            ok = writeDirectArm9Word(state, address, state->r[rd]);
            break;
        default:
            break;
    }

    if (!ok)
    {
        state->memoryFault = 1;
        return false;
    }

    if (writeBack)
        state->r[rn] = adjusted;
    return true;
}

bool applyArmHalfwordTransfer(ArmAluState *state, uint32_t opcode)
{
    if (!state || !state->memory)
        return false;

    Ps2ArmAlu::DecodedHalfwordTransfer decoded;
    if (!Ps2ArmAlu::decodeHalfwordTransfer(opcode, &decoded))
        return false;
    bool passed = Ps2ArmAlu::conditionPassed(decoded.condition, state->cpsr);

    if (!passed)
    {
        state->cycles += 1u;
        return true;
    }

    if (decoded.signedLoad)
    {
        uint32_t base = state->r[decoded.rn];
        uint32_t offset = decoded.immediate
            ? decoded.immediateOffset : state->r[decoded.rm];
        uint32_t adjusted =
            decoded.addOffset ? base + offset : base - offset;
        uint32_t address = decoded.preIndex ? adjusted : base;

        if (decoded.byteLoad)
        {
            uint8_t *page = getArm9CachedReadPage(state, address);
            if (!page)
            {
                state->memoryFault = 1;
                return false;
            }
            state->r[decoded.rd] = static_cast<uint32_t>(
                static_cast<int32_t>(static_cast<int8_t>(
                    page[address & 0xFFFu])));
        }
        else
        {
            uint16_t value = 0;
            if (!readDirectArm9Halfword(state, address, &value))
            {
                state->memoryFault = 1;
                return false;
            }
            int32_t signedValue = static_cast<int16_t>(value);
            if (state->cpuId && (address & 1u))
                signedValue >>= 8;
            state->r[decoded.rd] = static_cast<uint32_t>(signedValue);
        }

        if (decoded.writeBack)
            state->r[decoded.rn] = adjusted;
        state->cycles += state->cpuId ? 3u : 1u;
        return true;
    }

    if (Ps2ArmAlu::executeHalfwordTransfer(opcode, state->r, &state->cpsr,
        state, readDirectArm9Halfword, writeDirectArm9Halfword))
    {
        if (state->cpuId)
            state->cycles += decoded.load ? 3u : 2u;
        else
            state->cycles += 1u;
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
    uint8_t *page = getArm9CachedReadPage(state, address);
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
    uint8_t *page = getArm9CachedWritePage(state, address);
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

    uint8_t *page = getArm9CachedReadPage(state, address);
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

    uint8_t *page = getArm9CachedWritePage(state, address);
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

    Ps2ArmAlu::DecodedByteTransfer decoded;
    if (!Ps2ArmAlu::decodeByteTransfer(opcode, &decoded))
        return false;
    bool passed = Ps2ArmAlu::conditionPassed(decoded.condition, state->cpsr);

    if (Ps2ArmAlu::executeByteTransfer(opcode, state->r, &state->cpsr,
        state, readDirectArm9Byte, writeDirectArm9Byte))
    {
        if (!passed)
            state->cycles += 1u;
        else if (state->cpuId)
            state->cycles += decoded.load ? 3u : 2u;
        else
            state->cycles += 1u;
        return true;
    }
    state->memoryFault = 1;
    return false;
}

bool applyArmLiteralLoad(ArmAluState *state, uint32_t opcode, uint32_t pc)
{
    if (!state || !state->memory)
        return false;

    Ps2ArmAlu::DecodedLiteralLoad decoded;
    if (!Ps2ArmAlu::decodeLiteralLoad(opcode, &decoded))
        return false;
    bool passed = Ps2ArmAlu::conditionPassed(decoded.condition, state->cpsr);

    if (Ps2ArmAlu::executeLiteralLoad(opcode, pc, state->r, &state->cpsr,
        state, readDirectArm9Word))
    {
        if (!passed)
            state->cycles += 1u;
        else
            state->cycles += state->cpuId ? 3u : 1u;
        return true;
    }
    state->memoryFault = 1;
    return false;
}

bool applyArmSingleTransfer(ArmAluState *state, uint32_t opcode)
{
    if (!state || !state->memory)
        return false;

    Ps2ArmAlu::DecodedSingleTransfer decoded;
    if (!Ps2ArmAlu::decodeSingleTransfer(opcode, &decoded))
        return false;
    bool passed = Ps2ArmAlu::conditionPassed(decoded.condition, state->cpsr);

    if (Ps2ArmAlu::executeSingleTransfer(opcode, state->r, &state->cpsr,
        state, readDirectArm9Word, writeDirectArm9Word))
    {
        if (!passed)
            state->cycles += 1u;
        else if (state->cpuId)
            state->cycles += decoded.load ? 3u : 2u;
        else
            state->cycles += 1u;
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
    if (!Ps2ArmAlu::executeBranch(opcode, pc, state->cpsr,
        &state->nextPc, state->r))
        return false;
    state->cycles += taken ? 3u : 1u;
    return true;
}

bool applyArmSingleBlockTransferFast(ArmAluState *state,
    uint32_t packed)
{
    if (!state || !state->memory)
        return false;

    uint8_t rn = static_cast<uint8_t>(packed & 0xFu);
    uint8_t reg = static_cast<uint8_t>((packed >> 4) & 0xFu);
    bool load = (packed & BIT(8)) != 0;
    bool writeBack = (packed & BIT(9)) != 0;
    uint32_t address = state->r[rn];

    if (load)
    {
        uint32_t value = 0;
        if (!readDirectArm9Word(state, address, &value))
        {
            state->memoryFault = 1;
            return false;
        }
        state->r[reg] = value;
    }
    else if (!writeDirectArm9Word(state, address, state->r[reg]))
    {
        state->memoryFault = 1;
        return false;
    }

    if (writeBack)
        state->r[rn] = address + 4u;
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
        if (!taken)
            state->cycles += 1u;
        else if (state->cpuId)
            state->cycles += decoded.load ? 3u : 2u;
        else
            state->cycles += 2u;
        return true;
    }

    state->memoryFault = 1;
    return false;
}

bool applyArmMultiBlockTransfer(ArmAluState *state, uint32_t opcode)
{
    if (!state || !state->memory)
        return false;

    Ps2ArmAlu::DecodedMultiBlockTransfer decoded;
    if (!Ps2ArmAlu::decodeMultiBlockTransfer(opcode, &decoded))
        return false;
    bool taken = Ps2ArmAlu::conditionPassed(decoded.condition, state->cpsr);
    if (Ps2ArmAlu::executeMultiBlockTransfer(opcode, state->r, &state->cpsr,
        state, readDirectArm9Word, writeDirectArm9Word))
    {
        if (!taken)
            state->cycles += 1u;
        else if (state->cpuId)
            state->cycles += decoded.count + (decoded.load ? 2u : 1u);
        else
            state->cycles += decoded.count;
        return true;
    }

    state->memoryFault = 1;
    return false;
}

bool applyArmMultiBlockTransferFast(ArmAluState *state, uint32_t opcode)
{
    if (!state || !state->memory)
        return false;

    if (Ps2ArmAlu::executeMultiBlockTransfer(opcode, state->r, &state->cpsr,
        state, readDirectArm9Word, writeDirectArm9Word))
        return true;

    state->memoryFault = 1;
    return false;
}

size_t emitArmCycleAdd(uint32_t amount, uint32_t *code,
    bool registerCycles = false)
{
    if (!code || !amount || amount > 0x7FFFu)
        return 0;

    // Multi-instruction blocks keep their cycle accumulator in caller-saved
    // t8. Standalone self-test snippets retain the old memory-backed form.
    if (registerCycles)
    {
        code[0] = R5900::addiu(R5900::R_T8, R5900::R_T8,
            static_cast<int16_t>(amount));
        return 1;
    }

    size_t words = 0;
    code[words++] = R5900::lw(R5900::R_T6, 84, R5900::R_A0);
    code[words++] = R5900::addiu(R5900::R_T6, R5900::R_T6,
        static_cast<int16_t>(amount));
    code[words++] = R5900::sw(R5900::R_T6, 84, R5900::R_A0);
    return words;
}

size_t emitArmCondition(uint8_t condition, uint32_t *code,
    bool registerCpsr = false)
{
    if (!code || condition == 0xEu)
        return 0;

    size_t words = 0;
    int cpsrSource = registerCpsr ? R5900::R_V1 : R5900::R_T5;
    if (!registerCpsr)
        code[words++] = R5900::lw(cpsrSource, 64, R5900::R_A0);
    switch (condition & 0xFu)
    {
        case 0x0: // EQ: Z
            code[words++] = R5900::srl(R5900::R_T7, cpsrSource, 30);
            code[words++] = R5900::andi(R5900::R_T7, R5900::R_T7, 1);
            break;
        case 0x1: // NE: !Z
            code[words++] = R5900::srl(R5900::R_T7, cpsrSource, 30);
            code[words++] = R5900::andi(R5900::R_T7, R5900::R_T7, 1);
            code[words++] = R5900::xori(R5900::R_T7, R5900::R_T7, 1);
            break;
        case 0x2: // CS: C
            code[words++] = R5900::srl(R5900::R_T7, cpsrSource, 29);
            code[words++] = R5900::andi(R5900::R_T7, R5900::R_T7, 1);
            break;
        case 0x3: // CC: !C
            code[words++] = R5900::srl(R5900::R_T7, cpsrSource, 29);
            code[words++] = R5900::andi(R5900::R_T7, R5900::R_T7, 1);
            code[words++] = R5900::xori(R5900::R_T7, R5900::R_T7, 1);
            break;
        case 0x4: // MI: N
            code[words++] = R5900::srl(R5900::R_T7, cpsrSource, 31);
            break;
        case 0x5: // PL: !N
            code[words++] = R5900::srl(R5900::R_T7, cpsrSource, 31);
            code[words++] = R5900::xori(R5900::R_T7, R5900::R_T7, 1);
            break;
        case 0x6: // VS: V
            code[words++] = R5900::srl(R5900::R_T7, cpsrSource, 28);
            code[words++] = R5900::andi(R5900::R_T7, R5900::R_T7, 1);
            break;
        case 0x7: // VC: !V
            code[words++] = R5900::srl(R5900::R_T7, cpsrSource, 28);
            code[words++] = R5900::andi(R5900::R_T7, R5900::R_T7, 1);
            code[words++] = R5900::xori(R5900::R_T7, R5900::R_T7, 1);
            break;
        case 0x8: // HI: C && !Z
            code[words++] = R5900::srl(R5900::R_T7, cpsrSource, 29);
            code[words++] = R5900::andi(R5900::R_T7, R5900::R_T7, 1);
            code[words++] = R5900::srl(R5900::R_T6, cpsrSource, 30);
            code[words++] = R5900::andi(R5900::R_T6, R5900::R_T6, 1);
            code[words++] = R5900::xori(R5900::R_T6, R5900::R_T6, 1);
            code[words++] = R5900::and_(R5900::R_T7, R5900::R_T7, R5900::R_T6);
            break;
        case 0x9: // LS: !C || Z
            code[words++] = R5900::srl(R5900::R_T7, cpsrSource, 29);
            code[words++] = R5900::andi(R5900::R_T7, R5900::R_T7, 1);
            code[words++] = R5900::xori(R5900::R_T7, R5900::R_T7, 1);
            code[words++] = R5900::srl(R5900::R_T6, cpsrSource, 30);
            code[words++] = R5900::andi(R5900::R_T6, R5900::R_T6, 1);
            code[words++] = R5900::or_(R5900::R_T7, R5900::R_T7, R5900::R_T6);
            break;
        case 0xA: // GE: N == V
        case 0xB: // LT: N != V
            code[words++] = R5900::srl(R5900::R_T7, cpsrSource, 31);
            code[words++] = R5900::srl(R5900::R_T6, cpsrSource, 28);
            code[words++] = R5900::andi(R5900::R_T6, R5900::R_T6, 1);
            code[words++] = R5900::xor_(R5900::R_T7, R5900::R_T7, R5900::R_T6);
            if (condition == 0xAu)
                code[words++] = R5900::xori(R5900::R_T7, R5900::R_T7, 1);
            break;
        case 0xC: // GT: !Z && N == V
            code[words++] = R5900::srl(R5900::R_T7, cpsrSource, 30);
            code[words++] = R5900::andi(R5900::R_T7, R5900::R_T7, 1);
            code[words++] = R5900::xori(R5900::R_T7, R5900::R_T7, 1);
            code[words++] = R5900::srl(R5900::R_T6, cpsrSource, 31);
            code[words++] = R5900::srl(R5900::R_T4, cpsrSource, 28);
            code[words++] = R5900::andi(R5900::R_T4, R5900::R_T4, 1);
            code[words++] = R5900::xor_(R5900::R_T6, R5900::R_T6, R5900::R_T4);
            code[words++] = R5900::xori(R5900::R_T6, R5900::R_T6, 1);
            code[words++] = R5900::and_(R5900::R_T7, R5900::R_T7, R5900::R_T6);
            break;
        case 0xD: // LE: Z || N != V
            code[words++] = R5900::srl(R5900::R_T7, cpsrSource, 30);
            code[words++] = R5900::andi(R5900::R_T7, R5900::R_T7, 1);
            code[words++] = R5900::srl(R5900::R_T6, cpsrSource, 31);
            code[words++] = R5900::srl(R5900::R_T4, cpsrSource, 28);
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

size_t emitArmEqNeBranchFromResult(uint32_t opcode, uint32_t pc,
    uint32_t *code, bool registerCycles)
{
    Ps2ArmAlu::DecodedBranch decoded;
    if (!code || !Ps2ArmAlu::decodeBranch(opcode, &decoded) ||
        decoded.link ||
        (decoded.condition != 0x0u && decoded.condition != 0x1u))
        return 0;

    uint32_t fallthrough = pc + 4u;
    uint32_t target = pc + 8u + static_cast<uint32_t>(decoded.offset);
    size_t words = 0;

    // Base branch cost is one cycle. The previous S instruction left the
    // exact result in t2, so EQ/NE can test Z without re-extracting CPSR.Z.
    // Write the fallthrough first because a generated self-loop may arrive
    // here with nextPc still holding the target from a previous iteration.
    words += emitArmCycleAdd(1, code + words, registerCycles);
    code[words++] = R5900::lui(R5900::R_T3,
        static_cast<uint16_t>(fallthrough >> 16));
    code[words++] = R5900::ori(R5900::R_T3, R5900::R_T3,
        static_cast<uint16_t>(fallthrough));
    code[words++] = R5900::sw(R5900::R_T3, 68, R5900::R_A0);
    size_t notTakenBranch = words;
    code[words++] = (decoded.condition == 0x0u)
        ? R5900::bne(R5900::R_T2, R5900::R_ZERO, 0)
        : R5900::beq(R5900::R_T2, R5900::R_ZERO, 0);
    code[words++] = R5900::nop();

    // Taken B costs three cycles total.
    words += emitArmCycleAdd(2, code + words, registerCycles);
    code[words++] = R5900::lui(R5900::R_T3,
        static_cast<uint16_t>(target >> 16));
    code[words++] = R5900::ori(R5900::R_T3, R5900::R_T3,
        static_cast<uint16_t>(target));
    code[words++] = R5900::sw(R5900::R_T3, 68, R5900::R_A0);

    size_t done = words;
    size_t distance = done - (notTakenBranch + 1);
    if (distance > 0x7FFFu)
        return 0;
    code[notTakenBranch] = (decoded.condition == 0x0u)
        ? R5900::bne(R5900::R_T2, R5900::R_ZERO,
            static_cast<int16_t>(distance))
        : R5900::beq(R5900::R_T2, R5900::R_ZERO,
            static_cast<int16_t>(distance));
    return words;
}

size_t emitArmBranchInstruction(uint32_t opcode, uint32_t pc, uint32_t *code,
    bool registerCycles = false, bool registerCpsr = false,
    bool reuseCondition = false)
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
    words += emitArmCycleAdd(1, code + words, registerCycles);

    size_t conditionBranch = static_cast<size_t>(-1);
    if (decoded.condition != 0xEu)
    {
        code[words++] = R5900::lui(R5900::R_T2,
            static_cast<uint16_t>(fallthrough >> 16));
        code[words++] = R5900::ori(R5900::R_T2, R5900::R_T2,
            static_cast<uint16_t>(fallthrough));
        if (!reuseCondition)
            words += emitArmCondition(decoded.condition, code + words,
                registerCpsr);

        // Patch this after cycle emission: register-backed accounting is one
        // host instruction while the standalone memory-backed form is three.
        conditionBranch = words;
        code[words++] = R5900::beq(R5900::R_T7, R5900::R_ZERO, 0);
        code[words++] = R5900::nop();
        words += emitArmCycleAdd(2, code + words, registerCycles);
    }
    else
    {
        // Taken B/BL costs three ARM9 cycles total.
        words += emitArmCycleAdd(2, code + words, registerCycles);
    }

    if (decoded.link)
    {
        uint32_t returnAddress = pc + 4u;
        code[words++] = R5900::lui(R5900::R_T3,
            static_cast<uint16_t>(returnAddress >> 16));
        code[words++] = R5900::ori(R5900::R_T3, R5900::R_T3,
            static_cast<uint16_t>(returnAddress));
        code[words++] = R5900::sw(R5900::R_T3, 14 * 4, R5900::R_A0);
    }

    code[words++] = R5900::lui(R5900::R_T2,
        static_cast<uint16_t>(target >> 16));
    code[words++] = R5900::ori(R5900::R_T2, R5900::R_T2,
        static_cast<uint16_t>(target));

    size_t nextPcStore = words;
    code[words++] = R5900::sw(R5900::R_T2, 68, R5900::R_A0);

    if (conditionBranch != static_cast<size_t>(-1))
    {
        size_t distance = nextPcStore - (conditionBranch + 1);
        if (distance > 0x7FFFu)
            return 0;
        code[conditionBranch] = R5900::beq(R5900::R_T7,
            R5900::R_ZERO, static_cast<int16_t>(distance));
    }
    return words;
}

size_t emitArmFlags(const Ps2ArmAlu::Decoded &decoded, uint32_t *code,
    bool registerCpsr = false, bool logicalCarryValid = false)
{
    if (!code || !decoded.setFlags)
        return 0;

    size_t words = 0;
    int cpsrReg = registerCpsr ? R5900::R_V1 : R5900::R_T5;
    bool arithmetic = decoded.operation == 2 || decoded.operation == 3 ||
        decoded.operation == 4 || decoded.operation == 10 ||
        decoded.operation == 11;
    if (!registerCpsr)
        code[words++] = R5900::lw(cpsrReg, 64, R5900::R_A0);
    uint16_t flagPreserveMaskHi = arithmetic
        ? 0x0FFFu
        : (logicalCarryValid ? 0x1FFFu : 0x3FFFu);
    code[words++] = R5900::lui(R5900::R_T6, flagPreserveMaskHi);
    code[words++] = R5900::ori(R5900::R_T6, R5900::R_T6, 0xFFFFu);
    code[words++] = R5900::and_(cpsrReg, cpsrReg, R5900::R_T6);

    // N and Z are common to every admitted S operation.
    code[words++] = R5900::srl(R5900::R_T6, R5900::R_T2, 31);
    code[words++] = R5900::sll(R5900::R_T6, R5900::R_T6, 31);
    code[words++] = R5900::or_(cpsrReg, cpsrReg, R5900::R_T6);
    code[words++] = R5900::sltiu(R5900::R_T6, R5900::R_T2, 1);
    code[words++] = R5900::sll(R5900::R_T6, R5900::R_T6, 30);
    code[words++] = R5900::or_(cpsrReg, cpsrReg, R5900::R_T6);

    if (!arithmetic && logicalCarryValid)
    {
        code[words++] = R5900::sll(R5900::R_T7, R5900::R_T7, 29);
        code[words++] = R5900::or_(cpsrReg, cpsrReg, R5900::R_T7);
    }

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
        code[words++] = R5900::or_(cpsrReg, cpsrReg, R5900::R_T3);
        code[words++] = R5900::or_(cpsrReg, cpsrReg, R5900::R_T4);
    }
    if (!registerCpsr)
        code[words++] = R5900::sw(cpsrReg, 64, R5900::R_A0);
    return words;
}

size_t emitArmAluInstruction(uint32_t opcode, uint32_t *code,
    bool registerCycles = false, bool registerCpsr = false,
    int forwardedGuestReg = -1, bool suppressResultStore = false)
{
    if (!code)
        return 0;

    Ps2ArmAlu::Decoded decoded;
    if (!Ps2ArmAlu::decode(opcode, &decoded))
        return 0;
    size_t words = 0;
    bool logicalSetFlags = decoded.setFlags &&
        (decoded.operation == 0 || decoded.operation == 1 ||
         decoded.operation == 8 || decoded.operation == 9 ||
         decoded.operation == 12 || decoded.operation == 13 ||
         decoded.operation == 14 || decoded.operation == 15);
    bool logicalCarryValid = false;
    // Every ARM instruction costs one cycle even when its condition fails.
    // Register-specified shifts add their second cycle only on the taken path.
    words += emitArmCycleAdd(1, code + words, registerCycles);
    size_t conditionBranch = static_cast<size_t>(-1);
    if (decoded.condition != 0xEu)
    {
        words += emitArmCondition(decoded.condition, code + words,
            registerCpsr);
        conditionBranch = words;
        code[words++] = R5900::beq(R5900::R_T7, R5900::R_ZERO, 0);
        code[words++] = R5900::nop();
    }
    if (decoded.registerShift)
        words += emitArmCycleAdd(1, code + words, registerCycles);

    if (decoded.operation != 13 && decoded.operation != 15)
    {
        if (forwardedGuestReg >= 0 &&
            decoded.rn == static_cast<uint8_t>(forwardedGuestReg))
        {
            code[words++] = R5900::or_(R5900::R_T0,
                R5900::R_T2, R5900::R_ZERO);
        }
        else
        {
            code[words++] = R5900::lw(R5900::R_T0,
                static_cast<int16_t>(decoded.rn * 4), R5900::R_A0);
        }
    }

    if (decoded.immediate)
    {
        if (logicalSetFlags && decoded.immediateRotate)
        {
            code[words++] = R5900::addiu(R5900::R_T7, R5900::R_ZERO,
                static_cast<int16_t>((decoded.immediateValue >> 31) & 1u));
            logicalCarryValid = true;
        }
        code[words++] = R5900::lui(R5900::R_T1,
            static_cast<uint16_t>(decoded.immediateValue >> 16));
        code[words++] = R5900::ori(R5900::R_T1, R5900::R_T1,
            static_cast<uint16_t>(decoded.immediateValue));
    }
    else
    {
        if (forwardedGuestReg >= 0 &&
            decoded.rm == static_cast<uint8_t>(forwardedGuestReg))
        {
            code[words++] = R5900::or_(R5900::R_T1,
                R5900::R_T2, R5900::R_ZERO);
        }
        else
        {
            code[words++] = R5900::lw(R5900::R_T1,
                static_cast<int16_t>(decoded.rm * 4), R5900::R_A0);
        }

        if (!decoded.registerShift && logicalSetFlags)
        {
            uint8_t carryShift = 0;
            bool hasCarry = false;
            switch (decoded.shiftType)
            {
                case 0: // LSL #0 preserves C; LSL #1..31 exports bit 32-n.
                    if (decoded.shiftAmount)
                    {
                        carryShift = static_cast<uint8_t>(
                            32u - decoded.shiftAmount);
                        hasCarry = true;
                    }
                    break;
                case 1: // LSR #0 encodes LSR #32.
                case 2: // ASR #0 encodes ASR #32.
                    carryShift = decoded.shiftAmount
                        ? static_cast<uint8_t>(decoded.shiftAmount - 1u)
                        : 31u;
                    hasCarry = true;
                    break;
                case 3: // RRX (#0) is rejected by decode(); ROR #n exports n-1.
                    carryShift = static_cast<uint8_t>(
                        decoded.shiftAmount - 1u);
                    hasCarry = true;
                    break;
                default:
                    break;
            }

            if (hasCarry)
            {
                code[words++] = R5900::srl(R5900::R_T7, R5900::R_T1,
                    carryShift);
                code[words++] = R5900::andi(R5900::R_T7,
                    R5900::R_T7, 1u);
                logicalCarryValid = true;
            }
        }

        if (decoded.registerShift)
        {
            if (forwardedGuestReg >= 0 &&
                decoded.rs == static_cast<uint8_t>(forwardedGuestReg))
            {
                code[words++] = R5900::or_(R5900::R_T3,
                    R5900::R_T2, R5900::R_ZERO);
            }
            else
            {
                code[words++] = R5900::lw(R5900::R_T3,
                    static_cast<int16_t>(decoded.rs * 4), R5900::R_A0);
            }
            code[words++] = R5900::andi(R5900::R_T3, R5900::R_T3, 0xFFu);

            if (logicalSetFlags)
            {
                // T6 holds the old ARM carry so amount==0 can preserve it.
                if (registerCpsr)
                    code[words++] = R5900::srl(R5900::R_T6, R5900::R_V1, 29);
                else
                {
                    code[words++] = R5900::lw(R5900::R_T6, 64, R5900::R_A0);
                    code[words++] = R5900::srl(R5900::R_T6, R5900::R_T6, 29);
                }
                code[words++] = R5900::andi(R5900::R_T6, R5900::R_T6, 1u);

                switch (decoded.shiftType)
                {
                    case 0: // LSL Rs
                        code[words++] = R5900::subu(R5900::R_T4,
                            R5900::R_ZERO, R5900::R_T3);
                        code[words++] = R5900::srlv(R5900::R_T7,
                            R5900::R_T1, R5900::R_T4);
                        code[words++] = R5900::andi(R5900::R_T7,
                            R5900::R_T7, 1u);
                        code[words++] = R5900::sltiu(R5900::R_T5,
                            R5900::R_T3, 33u);
                        code[words++] = R5900::movz(R5900::R_T7,
                            R5900::R_ZERO, R5900::R_T5);
                        break;
                    case 1: // LSR Rs
                        code[words++] = R5900::addiu(R5900::R_T4,
                            R5900::R_T3, -1);
                        code[words++] = R5900::srlv(R5900::R_T7,
                            R5900::R_T1, R5900::R_T4);
                        code[words++] = R5900::andi(R5900::R_T7,
                            R5900::R_T7, 1u);
                        code[words++] = R5900::sltiu(R5900::R_T5,
                            R5900::R_T3, 33u);
                        code[words++] = R5900::movz(R5900::R_T7,
                            R5900::R_ZERO, R5900::R_T5);
                        break;
                    case 2: // ASR Rs
                        code[words++] = R5900::addiu(R5900::R_T4,
                            R5900::R_T3, -1);
                        code[words++] = R5900::srlv(R5900::R_T7,
                            R5900::R_T1, R5900::R_T4);
                        code[words++] = R5900::andi(R5900::R_T7,
                            R5900::R_T7, 1u);
                        code[words++] = R5900::srl(R5900::R_T5,
                            R5900::R_T1, 31);
                        code[words++] = R5900::sltiu(R5900::R_T4,
                            R5900::R_T3, 32u);
                        code[words++] = R5900::movz(R5900::R_T7,
                            R5900::R_T5, R5900::R_T4);
                        break;
                    case 3: // ROR Rs
                        code[words++] = R5900::addiu(R5900::R_T4,
                            R5900::R_T3, -1);
                        code[words++] = R5900::srlv(R5900::R_T7,
                            R5900::R_T1, R5900::R_T4);
                        code[words++] = R5900::andi(R5900::R_T7,
                            R5900::R_T7, 1u);
                        break;
                    default:
                        return 0;
                }

                // Every register-shift form preserves the old C when Rs[7:0]
                // is zero. MOVN applies the saved carry only for that case.
                code[words++] = R5900::sltiu(R5900::R_T5,
                    R5900::R_T3, 1u);
                code[words++] = R5900::movn(R5900::R_T7,
                    R5900::R_T6, R5900::R_T5);
                logicalCarryValid = true;
            }

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

    if (decoded.writesResult && !suppressResultStore)
        code[words++] = R5900::sw(R5900::R_T2,
            static_cast<int16_t>(decoded.rd * 4), R5900::R_A0);
    words += emitArmFlags(decoded, code + words, registerCpsr,
        logicalCarryValid);
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

size_t emitArmHalfwordLoadCachedInstruction(uint32_t opcode,
    uint32_t *code, bool registerCycles, bool registerCpsr,
    bool suppressResultStore = false, uint8_t targetCpuId = 0)
{
    Ps2ArmAlu::DecodedHalfwordTransfer decoded;
    if (!code || !Ps2ArmAlu::decodeHalfwordTransfer(opcode, &decoded) ||
        !decoded.load)
        return 0;

    size_t words = 0;
    words += emitArmCycleAdd(1, code + words, registerCycles);

    size_t conditionBranch = static_cast<size_t>(-1);
    if (decoded.condition != 0xEu)
    {
        words += emitArmCondition(decoded.condition, code + words,
            registerCpsr);
        conditionBranch = words;
        code[words++] = R5900::beq(R5900::R_T7, R5900::R_ZERO, 0);
        code[words++] = R5900::nop();
    }

    if (targetCpuId)
        words += emitArmCycleAdd(2, code + words, registerCycles);

    // t0 = effective address.
    code[words++] = R5900::lw(R5900::R_T0,
        static_cast<int16_t>(decoded.rn * 4), R5900::R_A0);
    if (decoded.immediate)
    {
        int32_t delta = decoded.addOffset
            ? static_cast<int32_t>(decoded.immediateOffset)
            : -static_cast<int32_t>(decoded.immediateOffset);
        if (decoded.preIndex && delta)
            code[words++] = R5900::addiu(R5900::R_T0, R5900::R_T0,
                static_cast<int16_t>(delta));
    }
    else
    {
        if (!decoded.preIndex)
            code[words++] = R5900::or_(R5900::R_T6,
                R5900::R_T0, R5900::R_ZERO);
        code[words++] = R5900::lw(R5900::R_T1,
            static_cast<int16_t>(decoded.rm * 4), R5900::R_A0);
        code[words++] = decoded.addOffset
            ? R5900::addu(R5900::R_T0, R5900::R_T0, R5900::R_T1)
            : R5900::subu(R5900::R_T0, R5900::R_T0, R5900::R_T1);
        if (!decoded.preIndex)
        {
            code[words++] = R5900::or_(R5900::R_T9,
                R5900::R_T0, R5900::R_ZERO);
            code[words++] = R5900::or_(R5900::R_T0,
                R5900::R_T6, R5900::R_ZERO);
        }
    }

    // Halfword loads need aligned host LH/LHU and two bytes inside the page.
    // LDRSB is byte-sized, so neither restriction applies.
    size_t oddBranch = static_cast<size_t>(-1);
    size_t crossBranch = static_cast<size_t>(-1);
    if (!decoded.byteLoad)
    {
        code[words++] = R5900::andi(R5900::R_T4, R5900::R_T0, 1);
        oddBranch = words;
        code[words++] = R5900::bne(R5900::R_T4, R5900::R_ZERO, 0);
        code[words++] = R5900::nop();
    }

    code[words++] = R5900::andi(R5900::R_T4, R5900::R_T0, 0x0FFFu);
    if (!decoded.byteLoad)
    {
        code[words++] = R5900::xori(R5900::R_T5, R5900::R_T4, 0x0FFFu);
        crossBranch = words;
        code[words++] = R5900::beq(R5900::R_T5, R5900::R_ZERO, 0);
        code[words++] = R5900::nop();
    }

    // Compare the one-page JIT-local cache tag.
    code[words++] = R5900::srl(R5900::R_T1, R5900::R_T0, 12);
    code[words++] = R5900::addiu(R5900::R_T1, R5900::R_T1, 1);
    code[words++] = R5900::lw(R5900::R_T2, 88, R5900::R_A0);
    size_t missBranch = words;
    code[words++] = R5900::bne(R5900::R_T1, R5900::R_T2, 0);
    code[words++] = R5900::nop();

    // Cache hit: use EE sign-extending loads directly for LDRSB/LDRSH.
    code[words++] = R5900::lw(R5900::R_T3, 92, R5900::R_A0);
    code[words++] = R5900::addu(R5900::R_T3, R5900::R_T3, R5900::R_T4);
    if (decoded.signedLoad)
    {
        code[words++] = decoded.byteLoad
            ? R5900::lb(R5900::R_T2, 0, R5900::R_T3)
            : R5900::lh(R5900::R_T2, 0, R5900::R_T3);
    }
    else
    {
        code[words++] = R5900::lhu(R5900::R_T2, 0, R5900::R_T3);
    }
    if (!suppressResultStore)
        code[words++] = R5900::sw(R5900::R_T2,
            static_cast<int16_t>(decoded.rd * 4), R5900::R_A0);
    if (decoded.writeBack)
    {
        if (decoded.preIndex)
        {
            code[words++] = R5900::sw(R5900::R_T0,
                static_cast<int16_t>(decoded.rn * 4), R5900::R_A0);
        }
        else
        {
            if (!decoded.immediate)
            {
                code[words++] = R5900::sw(R5900::R_T9,
                    static_cast<int16_t>(decoded.rn * 4), R5900::R_A0);
            }
            else
            {
                int32_t delta = decoded.addOffset
                    ? static_cast<int32_t>(decoded.immediateOffset)
                    : -static_cast<int32_t>(decoded.immediateOffset);
                if (delta)
                    code[words++] = R5900::addiu(R5900::R_T6,
                        R5900::R_T0, static_cast<int16_t>(delta));
                else
                    code[words++] = R5900::or_(R5900::R_T6,
                        R5900::R_T0, R5900::R_ZERO);
                code[words++] = R5900::sw(R5900::R_T6,
                    static_cast<int16_t>(decoded.rn * 4), R5900::R_A0);
            }
        }
    }
    size_t hitDoneBranch = words;
    code[words++] = R5900::beq(R5900::R_ZERO, R5900::R_ZERO, 0);
    code[words++] = R5900::nop();

    // Miss/unsafe path: synchronize caller-saved block state, resolve through
    // Memory's cache once, and seed the JIT-local page cache for later loop
    // iterations.
    size_t slowPath = words;
    if (registerCpsr)
        code[words++] = R5900::sw(R5900::R_V1, 64, R5900::R_A0);
    if (registerCycles)
        code[words++] = R5900::sw(R5900::R_T8, 84, R5900::R_A0);

    code[words++] = R5900::addiu(R5900::R_SP, R5900::R_SP, -32);
    code[words++] = R5900::sw(R5900::R_RA, 28, R5900::R_SP);
    code[words++] = R5900::sw(R5900::R_A0, 24, R5900::R_SP);
    if (decoded.writeBack)
    {
        code[words++] = R5900::sw(R5900::R_T0, 20, R5900::R_SP);
        if (!decoded.preIndex && !decoded.immediate)
            code[words++] = R5900::sw(R5900::R_T9, 16, R5900::R_SP);
    }
    code[words++] = R5900::or_(R5900::R_A1, R5900::R_T0, R5900::R_ZERO);
    uintptr_t helper = decoded.byteLoad
        ? reinterpret_cast<uintptr_t>(&refillArm9ByteReadCache)
        : reinterpret_cast<uintptr_t>(&refillArm9HalfwordReadCache);
    code[words++] = R5900::lui(R5900::R_T9,
        static_cast<uint16_t>(helper >> 16));
    code[words++] = R5900::ori(R5900::R_T9, R5900::R_T9,
        static_cast<uint16_t>(helper));
    code[words++] = R5900::jalr(R5900::R_RA, R5900::R_T9);
    code[words++] = R5900::nop();
    code[words++] = R5900::lw(R5900::R_A0, 24, R5900::R_SP);
    if (decoded.writeBack)
    {
        code[words++] = R5900::lw(R5900::R_T0, 20, R5900::R_SP);
        if (!decoded.preIndex && !decoded.immediate)
            code[words++] = R5900::lw(R5900::R_T9, 16, R5900::R_SP);
    }
    code[words++] = R5900::lw(R5900::R_RA, 28, R5900::R_SP);
    code[words++] = R5900::addiu(R5900::R_SP, R5900::R_SP, 32);
    if (decoded.signedLoad)
    {
        uint8_t shift = decoded.byteLoad ? 24u : 16u;
        code[words++] = R5900::sll(R5900::R_V0, R5900::R_V0, shift);
        code[words++] = R5900::sra(R5900::R_V0, R5900::R_V0, shift);
    }
    if (!suppressResultStore)
        code[words++] = R5900::sw(R5900::R_V0,
            static_cast<int16_t>(decoded.rd * 4), R5900::R_A0);
    code[words++] = R5900::or_(R5900::R_T2,
        R5900::R_V0, R5900::R_ZERO);
    if (decoded.writeBack)
    {
        if (decoded.preIndex)
        {
            code[words++] = R5900::sw(R5900::R_T0,
                static_cast<int16_t>(decoded.rn * 4), R5900::R_A0);
        }
        else
        {
            if (!decoded.immediate)
            {
                code[words++] = R5900::sw(R5900::R_T9,
                    static_cast<int16_t>(decoded.rn * 4), R5900::R_A0);
            }
            else
            {
                int32_t delta = decoded.addOffset
                    ? static_cast<int32_t>(decoded.immediateOffset)
                    : -static_cast<int32_t>(decoded.immediateOffset);
                if (delta)
                    code[words++] = R5900::addiu(R5900::R_T6,
                        R5900::R_T0, static_cast<int16_t>(delta));
                else
                    code[words++] = R5900::or_(R5900::R_T6,
                        R5900::R_T0, R5900::R_ZERO);
                code[words++] = R5900::sw(R5900::R_T6,
                    static_cast<int16_t>(decoded.rn * 4), R5900::R_A0);
            }
        }
    }
    if (registerCycles)
        code[words++] = R5900::lw(R5900::R_T8, 84, R5900::R_A0);
    if (registerCpsr)
        code[words++] = R5900::lw(R5900::R_V1, 64, R5900::R_A0);

    size_t done = words;

    auto patchBeq = [&](size_t branch, int rs, int rt, size_t target)
    {
        if (branch == static_cast<size_t>(-1))
            return true;
        size_t distance = target - (branch + 1);
        if (distance > 0x7FFFu)
            return false;
        code[branch] = R5900::beq(rs, rt, static_cast<int16_t>(distance));
        return true;
    };
    auto patchBne = [&](size_t branch, int rs, int rt, size_t target)
    {
        if (branch == static_cast<size_t>(-1))
            return true;
        size_t distance = target - (branch + 1);
        if (distance > 0x7FFFu)
            return false;
        code[branch] = R5900::bne(rs, rt, static_cast<int16_t>(distance));
        return true;
    };

    if (!patchBne(oddBranch, R5900::R_T4, R5900::R_ZERO, slowPath) ||
        !patchBeq(crossBranch, R5900::R_T5, R5900::R_ZERO, slowPath) ||
        !patchBne(missBranch, R5900::R_T1, R5900::R_T2, slowPath) ||
        !patchBeq(hitDoneBranch, R5900::R_ZERO, R5900::R_ZERO, done))
        return 0;

    if (conditionBranch != static_cast<size_t>(-1))
    {
        size_t distance = done - (conditionBranch + 1);
        if (distance > 0x7FFFu)
            return 0;
        code[conditionBranch] = R5900::beq(R5900::R_T7,
            R5900::R_ZERO, static_cast<int16_t>(distance));
    }
    return words;
}

size_t emitArmRegisterOffsetAddress(uint8_t rn, uint8_t rm,
    uint8_t shiftType, uint8_t shiftAmount, bool addOffset, uint32_t *code);

size_t emitArmHalfwordRegisterStoreFastInstruction(uint32_t opcode,
    uint32_t *code, bool registerCycles, bool registerCpsr,
    uint8_t targetCpuId = 0)
{
    Ps2ArmAlu::DecodedHalfwordTransfer decoded;
    if (!code || !Ps2ArmAlu::decodeHalfwordTransfer(opcode, &decoded) ||
        decoded.load || decoded.immediate)
        return 0;

    uint32_t packed = static_cast<uint32_t>(decoded.rn) |
        (static_cast<uint32_t>(decoded.rd) << 4) |
        (static_cast<uint32_t>(decoded.rm) << 8) |
        (decoded.addOffset ? BIT(12) : 0u) |
        (decoded.preIndex ? BIT(13) : 0u) |
        (decoded.writeBack ? BIT(14) : 0u);

    size_t words = 0;
    words += emitArmCycleAdd(1, code + words, registerCycles);

    size_t conditionBranch = static_cast<size_t>(-1);
    if (decoded.condition != 0xEu)
    {
        words += emitArmCondition(decoded.condition, code + words,
            registerCpsr);
        conditionBranch = words;
        code[words++] = R5900::beq(R5900::R_T7, R5900::R_ZERO, 0);
        code[words++] = R5900::nop();
    }

    if (targetCpuId)
        words += emitArmCycleAdd(1u, code + words, registerCycles);

    // t0 = original base, t1 = register offset, t2 = adjusted base.
    code[words++] = R5900::lw(R5900::R_T0,
        static_cast<int16_t>(decoded.rn * 4), R5900::R_A0);
    code[words++] = R5900::lw(R5900::R_T1,
        static_cast<int16_t>(decoded.rm * 4), R5900::R_A0);
    code[words++] = decoded.addOffset
        ? R5900::addu(R5900::R_T2, R5900::R_T0, R5900::R_T1)
        : R5900::subu(R5900::R_T2, R5900::R_T0, R5900::R_T1);
    if (decoded.preIndex)
        code[words++] = R5900::or_(R5900::R_T0,
            R5900::R_T2, R5900::R_ZERO);

    // SH needs an aligned host address; odd guest addresses retain the exact
    // bytewise helper behavior.
    code[words++] = R5900::andi(R5900::R_T4, R5900::R_T0, 1);
    size_t oddBranch = words;
    code[words++] = R5900::bne(R5900::R_T4, R5900::R_ZERO, 0);
    code[words++] = R5900::nop();

    code[words++] = R5900::srl(R5900::R_T4, R5900::R_T0, 12);
    code[words++] = R5900::addiu(R5900::R_T4, R5900::R_T4, 1);
    code[words++] = R5900::lw(R5900::R_T5, 104, R5900::R_A0);
    size_t missBranch = words;
    code[words++] = R5900::bne(R5900::R_T4, R5900::R_T5, 0);
    code[words++] = R5900::nop();
    code[words++] = R5900::lw(R5900::R_T3, 108, R5900::R_A0);
    size_t nullBranch = words;
    code[words++] = R5900::beq(R5900::R_T3, R5900::R_ZERO, 0);
    code[words++] = R5900::nop();

    code[words++] = R5900::lw(R5900::R_T5, 80, R5900::R_A0);
    code[words++] = R5900::sltiu(R5900::R_T6, R5900::R_T5, 16);
    size_t journalFullBranch = words;
    code[words++] = R5900::beq(R5900::R_T6, R5900::R_ZERO, 0);
    code[words++] = R5900::nop();

    code[words++] = R5900::andi(R5900::R_T4, R5900::R_T0, 0x0FFFu);
    code[words++] = R5900::addu(R5900::R_T3, R5900::R_T3, R5900::R_T4);
    code[words++] = R5900::lw(R5900::R_T1,
        static_cast<int16_t>(decoded.rd * 4), R5900::R_A0);

    // Journal address/before/after/size (16-byte entry at state+112).
    code[words++] = R5900::sll(R5900::R_T6, R5900::R_T5, 4);
    code[words++] = R5900::addiu(R5900::R_T7, R5900::R_A0, 112);
    code[words++] = R5900::addu(R5900::R_T7, R5900::R_T7, R5900::R_T6);
    code[words++] = R5900::lhu(R5900::R_T6, 0, R5900::R_T3);
    code[words++] = R5900::sw(R5900::R_T0, 0, R5900::R_T7);
    code[words++] = R5900::sw(R5900::R_T6, 4, R5900::R_T7);
    code[words++] = R5900::sw(R5900::R_T1, 8, R5900::R_T7);
    code[words++] = R5900::addiu(R5900::R_T6, R5900::R_ZERO, 2);
    code[words++] = R5900::sw(R5900::R_T6, 12, R5900::R_T7);
    code[words++] = R5900::sh(R5900::R_T1, 0, R5900::R_T3);
    code[words++] = R5900::addiu(R5900::R_T5, R5900::R_T5, 1);
    code[words++] = R5900::sw(R5900::R_T5, 80, R5900::R_A0);

    if (decoded.writeBack)
        code[words++] = R5900::sw(R5900::R_T2,
            static_cast<int16_t>(decoded.rn * 4), R5900::R_A0);

    size_t fastDoneBranch = words;
    code[words++] = R5900::beq(R5900::R_ZERO, R5900::R_ZERO, 0);
    code[words++] = R5900::nop();

    size_t slowPath = words;
    if (registerCpsr)
        code[words++] = R5900::sw(R5900::R_V1, 64, R5900::R_A0);
    if (registerCycles)
        code[words++] = R5900::sw(R5900::R_T8, 84, R5900::R_A0);

    code[words++] = R5900::addiu(R5900::R_SP, R5900::R_SP, -32);
    code[words++] = R5900::sw(R5900::R_RA, 28, R5900::R_SP);
    code[words++] = R5900::sw(R5900::R_A0, 24, R5900::R_SP);
    code[words++] = R5900::lui(R5900::R_A1,
        static_cast<uint16_t>(packed >> 16));
    code[words++] = R5900::ori(R5900::R_A1, R5900::R_A1,
        static_cast<uint16_t>(packed));
    uintptr_t helper =
        reinterpret_cast<uintptr_t>(&applyArmHalfwordRegisterStoreFast);
    code[words++] = R5900::lui(R5900::R_T9,
        static_cast<uint16_t>(helper >> 16));
    code[words++] = R5900::ori(R5900::R_T9, R5900::R_T9,
        static_cast<uint16_t>(helper));
    code[words++] = R5900::jalr(R5900::R_RA, R5900::R_T9);
    code[words++] = R5900::nop();
    code[words++] = R5900::lw(R5900::R_A0, 24, R5900::R_SP);
    code[words++] = R5900::lw(R5900::R_RA, 28, R5900::R_SP);
    code[words++] = R5900::addiu(R5900::R_SP, R5900::R_SP, 32);

    if (registerCycles)
        code[words++] = R5900::lw(R5900::R_T8, 84, R5900::R_A0);
    if (registerCpsr)
        code[words++] = R5900::lw(R5900::R_V1, 64, R5900::R_A0);

    size_t done = words;
    auto patchBne = [&](size_t branch, int rs, int rt, size_t target)
    {
        size_t distance = target - (branch + 1);
        if (distance > 0x7FFFu)
            return false;
        code[branch] = R5900::bne(rs, rt, static_cast<int16_t>(distance));
        return true;
    };
    auto patchBeq = [&](size_t branch, int rs, int rt, size_t target)
    {
        size_t distance = target - (branch + 1);
        if (distance > 0x7FFFu)
            return false;
        code[branch] = R5900::beq(rs, rt, static_cast<int16_t>(distance));
        return true;
    };

    if (!patchBne(oddBranch, R5900::R_T4, R5900::R_ZERO, slowPath) ||
        !patchBne(missBranch, R5900::R_T4, R5900::R_T5, slowPath) ||
        !patchBeq(nullBranch, R5900::R_T3, R5900::R_ZERO, slowPath) ||
        !patchBeq(journalFullBranch, R5900::R_T6, R5900::R_ZERO, slowPath) ||
        !patchBeq(fastDoneBranch, R5900::R_ZERO, R5900::R_ZERO, done))
        return 0;

    if (conditionBranch != static_cast<size_t>(-1) &&
        !patchBeq(conditionBranch, R5900::R_T7, R5900::R_ZERO, done))
        return 0;

    return words;
}

size_t emitArmRegisterStoreFastInstruction(uint32_t opcode,
    uint32_t *code, bool registerCycles, bool registerCpsr,
    uint8_t targetCpuId = 0)
{
    if (!code)
        return 0;

    uint8_t rn = 0, rd = 0, rm = 0, condition = 0;
    uint8_t shiftType = 0, shiftAmount = 0;
    bool addOffset = false, preIndex = false, writeBack = false, word = false;

    Ps2ArmAlu::DecodedByteTransfer byte;
    Ps2ArmAlu::DecodedSingleTransfer single;
    if (Ps2ArmAlu::decodeByteTransfer(opcode, &byte) &&
        !byte.load && byte.registerOffset)
    {
        rn = byte.rn; rd = byte.rd; rm = byte.rm;
        condition = byte.condition;
        shiftType = byte.shiftType;
        shiftAmount = byte.shiftAmount;
        addOffset = byte.addOffset;
        preIndex = byte.preIndex;
        writeBack = byte.writeBack;
        word = false;
    }
    else if (Ps2ArmAlu::decodeSingleTransfer(opcode, &single) &&
        !single.load && single.registerOffset)
    {
        rn = single.rn; rd = single.rd; rm = single.rm;
        condition = single.condition;
        shiftType = single.shiftType;
        shiftAmount = single.shiftAmount;
        addOffset = single.addOffset;
        preIndex = single.preIndex;
        writeBack = single.writeBack;
        word = true;
    }
    else
    {
        return 0;
    }

    // RRX register-offset addressing remains outside the decoded subset.
    if (shiftType == 3 && !shiftAmount)
        return 0;

    uint32_t packed = static_cast<uint32_t>(rn) |
        (static_cast<uint32_t>(rd) << 4) |
        (static_cast<uint32_t>(rm) << 8) |
        (addOffset ? BIT(12) : 0u) |
        (writeBack ? BIT(13) : 0u) |
        (word ? BIT(14) : 0u) |
        (static_cast<uint32_t>(shiftType) << 15) |
        (static_cast<uint32_t>(shiftAmount) << 17) |
        (preIndex ? BIT(22) : 0u);

    size_t words = 0;
    words += emitArmCycleAdd(1, code + words, registerCycles);

    size_t conditionBranch = static_cast<size_t>(-1);
    if (condition != 0xEu)
    {
        words += emitArmCondition(condition, code + words, registerCpsr);
        conditionBranch = words;
        code[words++] = R5900::beq(R5900::R_T7, R5900::R_ZERO, 0);
        code[words++] = R5900::nop();
    }

    if (targetCpuId)
        words += emitArmCycleAdd(1u, code + words, registerCycles);

    // Post-index stores use the original base as the memory address but
    // still need the adjusted value for writeback after a successful store.
    if (!preIndex)
        code[words++] = R5900::lw(R5900::R_T6,
            static_cast<int16_t>(rn * 4), R5900::R_A0);

    size_t addressWords = emitArmRegisterOffsetAddress(rn, rm,
        shiftType, shiftAmount, addOffset, code + words);
    if (!addressWords)
        return 0;
    words += addressWords;

    if (!preIndex)
    {
        // emitArmRegisterOffsetAddress may use t2/t3 for ROR, so preserve the
        // original post-index base in t6, which the address emitter never uses.
        code[words++] = R5900::or_(R5900::R_T4,
            R5900::R_T0, R5900::R_ZERO);
        code[words++] = R5900::or_(R5900::R_T0,
            R5900::R_T6, R5900::R_ZERO);
        code[words++] = R5900::or_(R5900::R_T2,
            R5900::R_T4, R5900::R_ZERO);
    }

    size_t alignBranch = static_cast<size_t>(-1);
    if (word)
    {
        code[words++] = R5900::andi(R5900::R_T6, R5900::R_T0, 3);
        alignBranch = words;
        code[words++] = R5900::bne(R5900::R_T6, R5900::R_ZERO, 0);
        code[words++] = R5900::nop();
    }

    code[words++] = R5900::srl(R5900::R_T5, R5900::R_T0, 12);
    code[words++] = R5900::addiu(R5900::R_T5, R5900::R_T5, 1);
    code[words++] = R5900::lw(R5900::R_T6, 104, R5900::R_A0);
    size_t cacheMissBranch = words;
    code[words++] = R5900::bne(R5900::R_T5, R5900::R_T6, 0);
    code[words++] = R5900::nop();

    code[words++] = R5900::lw(R5900::R_T3, 108, R5900::R_A0);
    size_t nullPageBranch = words;
    code[words++] = R5900::beq(R5900::R_T3, R5900::R_ZERO, 0);
    code[words++] = R5900::nop();

    code[words++] = R5900::lw(R5900::R_T5, 80, R5900::R_A0);
    code[words++] = R5900::sltiu(R5900::R_T6, R5900::R_T5, 16);
    size_t journalFullBranch = words;
    code[words++] = R5900::beq(R5900::R_T6, R5900::R_ZERO, 0);
    code[words++] = R5900::nop();

    code[words++] = R5900::andi(R5900::R_T4, R5900::R_T0, 0x0FFFu);
    code[words++] = R5900::addu(R5900::R_T3, R5900::R_T3, R5900::R_T4);
    code[words++] = R5900::lw(R5900::R_T1,
        static_cast<int16_t>(rd * 4), R5900::R_A0);

    code[words++] = R5900::sll(R5900::R_T6, R5900::R_T5, 4);
    code[words++] = R5900::addiu(R5900::R_T7, R5900::R_A0, 112);
    code[words++] = R5900::addu(R5900::R_T7, R5900::R_T7, R5900::R_T6);
    code[words++] = word
        ? R5900::lw(R5900::R_T6, 0, R5900::R_T3)
        : R5900::lbu(R5900::R_T6, 0, R5900::R_T3);
    code[words++] = R5900::sw(R5900::R_T0, 0, R5900::R_T7);
    code[words++] = R5900::sw(R5900::R_T6, 4, R5900::R_T7);
    code[words++] = R5900::sw(R5900::R_T1, 8, R5900::R_T7);
    code[words++] = R5900::addiu(R5900::R_T6, R5900::R_ZERO,
        static_cast<int16_t>(word ? 4 : 1));
    code[words++] = R5900::sw(R5900::R_T6, 12, R5900::R_T7);
    code[words++] = word
        ? R5900::sw(R5900::R_T1, 0, R5900::R_T3)
        : R5900::sb(R5900::R_T1, 0, R5900::R_T3);
    code[words++] = R5900::addiu(R5900::R_T5, R5900::R_T5, 1);
    code[words++] = R5900::sw(R5900::R_T5, 80, R5900::R_A0);

    if (writeBack)
        code[words++] = R5900::sw(preIndex ? R5900::R_T0 : R5900::R_T2,
            static_cast<int16_t>(rn * 4), R5900::R_A0);

    size_t fastDoneBranch = words;
    code[words++] = R5900::beq(R5900::R_ZERO, R5900::R_ZERO, 0);
    code[words++] = R5900::nop();

    size_t slowPath = words;
    if (registerCpsr)
        code[words++] = R5900::sw(R5900::R_V1, 64, R5900::R_A0);
    if (registerCycles)
        code[words++] = R5900::sw(R5900::R_T8, 84, R5900::R_A0);

    code[words++] = R5900::addiu(R5900::R_SP, R5900::R_SP, -32);
    code[words++] = R5900::sw(R5900::R_RA, 28, R5900::R_SP);
    code[words++] = R5900::sw(R5900::R_A0, 24, R5900::R_SP);
    code[words++] = R5900::lui(R5900::R_A1,
        static_cast<uint16_t>(packed >> 16));
    code[words++] = R5900::ori(R5900::R_A1, R5900::R_A1,
        static_cast<uint16_t>(packed));
    uintptr_t helper = reinterpret_cast<uintptr_t>(&applyArmRegisterStoreFast);
    code[words++] = R5900::lui(R5900::R_T9,
        static_cast<uint16_t>(helper >> 16));
    code[words++] = R5900::ori(R5900::R_T9, R5900::R_T9,
        static_cast<uint16_t>(helper));
    code[words++] = R5900::jalr(R5900::R_RA, R5900::R_T9);
    code[words++] = R5900::nop();
    code[words++] = R5900::lw(R5900::R_A0, 24, R5900::R_SP);
    code[words++] = R5900::lw(R5900::R_RA, 28, R5900::R_SP);
    code[words++] = R5900::addiu(R5900::R_SP, R5900::R_SP, 32);

    if (registerCycles)
        code[words++] = R5900::lw(R5900::R_T8, 84, R5900::R_A0);
    if (registerCpsr)
        code[words++] = R5900::lw(R5900::R_V1, 64, R5900::R_A0);

    size_t done = words;
    auto patchBne = [&](size_t branch, int rs, int rt, size_t target)
    {
        if (branch == static_cast<size_t>(-1))
            return true;
        size_t distance = target - (branch + 1);
        if (distance > 0x7FFFu)
            return false;
        code[branch] = R5900::bne(rs, rt, static_cast<int16_t>(distance));
        return true;
    };
    auto patchBeq = [&](size_t branch, int rs, int rt, size_t target)
    {
        size_t distance = target - (branch + 1);
        if (distance > 0x7FFFu)
            return false;
        code[branch] = R5900::beq(rs, rt, static_cast<int16_t>(distance));
        return true;
    };

    if (!patchBne(alignBranch, R5900::R_T6, R5900::R_ZERO, slowPath) ||
        !patchBne(cacheMissBranch, R5900::R_T5, R5900::R_T6, slowPath) ||
        !patchBeq(nullPageBranch, R5900::R_T3, R5900::R_ZERO, slowPath) ||
        !patchBeq(journalFullBranch, R5900::R_T6, R5900::R_ZERO, slowPath) ||
        !patchBeq(fastDoneBranch, R5900::R_ZERO, R5900::R_ZERO, done))
        return 0;

    if (conditionBranch != static_cast<size_t>(-1) &&
        !patchBeq(conditionBranch, R5900::R_T7, R5900::R_ZERO, done))
        return 0;

    return words;
}

size_t emitArmImmediateStoreFastInstruction(uint32_t opcode,
    uint32_t *code, bool registerCycles, bool registerCpsr,
    uint8_t targetCpuId = 0)
{
    if (!code)
        return 0;

    uint8_t rn = 0, rd = 0, condition = 0;
    uint32_t offset = 0;
    bool addOffset = false, preIndex = false, writeBack = false;
    uint8_t sizeCode = 3;

    Ps2ArmAlu::DecodedHalfwordTransfer half;
    Ps2ArmAlu::DecodedByteTransfer byte;
    Ps2ArmAlu::DecodedSingleTransfer word;
    if (Ps2ArmAlu::decodeHalfwordTransfer(opcode, &half) &&
        !half.load && half.immediate)
    {
        rn = half.rn; rd = half.rd; condition = half.condition;
        offset = half.immediateOffset;
        addOffset = half.addOffset;
        preIndex = half.preIndex;
        writeBack = half.writeBack;
        sizeCode = 1;
    }
    else if (Ps2ArmAlu::decodeByteTransfer(opcode, &byte) &&
        !byte.load && !byte.registerOffset)
    {
        rn = byte.rn; rd = byte.rd; condition = byte.condition;
        offset = byte.immediateOffset;
        addOffset = byte.addOffset;
        preIndex = byte.preIndex;
        writeBack = byte.writeBack;
        sizeCode = 0;
    }
    else if (Ps2ArmAlu::decodeSingleTransfer(opcode, &word) &&
        !word.load && !word.registerOffset)
    {
        rn = word.rn; rd = word.rd; condition = word.condition;
        offset = word.immediateOffset;
        addOffset = word.addOffset;
        preIndex = word.preIndex;
        writeBack = word.writeBack;
        sizeCode = 2;
    }
    else
    {
        return 0;
    }

    uint32_t packed = static_cast<uint32_t>(rn) |
        (static_cast<uint32_t>(rd) << 4) |
        ((offset & 0xFFFu) << 8) |
        (addOffset ? BIT(20) : 0u) |
        (preIndex ? BIT(21) : 0u) |
        (writeBack ? BIT(22) : 0u) |
        (static_cast<uint32_t>(sizeCode) << 23);

    size_t words = 0;
    words += emitArmCycleAdd(1, code + words, registerCycles);

    size_t conditionBranch = static_cast<size_t>(-1);
    if (condition != 0xEu)
    {
        words += emitArmCondition(condition, code + words, registerCpsr);
        conditionBranch = words;
        code[words++] = R5900::beq(R5900::R_T7, R5900::R_ZERO, 0);
        code[words++] = R5900::nop();
    }

    // ARM7 direct word stores cost 2 cycles versus 1 on ARM9. Byte/halfword
    // forms are not part of the guarded ARM7 subset yet.
    if (targetCpuId && sizeCode <= 2)
        words += emitArmCycleAdd(1, code + words, registerCycles);

    // Compute the ARM effective address in t0. Post-index writeback is
    // recomputed only after a successful store, preserving fallback semantics.
    code[words++] = R5900::lw(R5900::R_T0,
        static_cast<int16_t>(rn * 4), R5900::R_A0);
    int32_t delta = addOffset ? static_cast<int32_t>(offset)
                              : -static_cast<int32_t>(offset);
    if (preIndex && delta)
        code[words++] = R5900::addiu(R5900::R_T0, R5900::R_T0,
            static_cast<int16_t>(delta));

    // Host SH/SW require natural alignment. NooDS' bytewise halfword helper
    // still accepts odd ARM addresses, so those stay on the exact slow path.
    size_t alignBranch = static_cast<size_t>(-1);
    if (sizeCode == 1)
    {
        code[words++] = R5900::andi(R5900::R_T6, R5900::R_T0, 1);
        alignBranch = words;
        code[words++] = R5900::bne(R5900::R_T6, R5900::R_ZERO, 0);
        code[words++] = R5900::nop();
    }
    else if (sizeCode == 2)
    {
        code[words++] = R5900::andi(R5900::R_T6, R5900::R_T0, 3);
        alignBranch = words;
        code[words++] = R5900::bne(R5900::R_T6, R5900::R_ZERO, 0);
        code[words++] = R5900::nop();
    }

    // One-page persistent write cache. A miss calls the existing exact helper,
    // which also seeds these fields for the next hot iteration/dispatch.
    code[words++] = R5900::srl(R5900::R_T5, R5900::R_T0, 12);
    code[words++] = R5900::addiu(R5900::R_T5, R5900::R_T5, 1);
    code[words++] = R5900::lw(R5900::R_T6, 104, R5900::R_A0);
    size_t cacheMissBranch = words;
    code[words++] = R5900::bne(R5900::R_T5, R5900::R_T6, 0);
    code[words++] = R5900::nop();

    code[words++] = R5900::lw(R5900::R_T3, 108, R5900::R_A0);
    size_t nullPageBranch = words;
    code[words++] = R5900::beq(R5900::R_T3, R5900::R_ZERO, 0);
    code[words++] = R5900::nop();

    // Ensure the fixed 16-entry transactional journal still has room.
    code[words++] = R5900::lw(R5900::R_T5, 80, R5900::R_A0);
    code[words++] = R5900::sltiu(R5900::R_T6, R5900::R_T5, 16);
    size_t journalFullBranch = words;
    code[words++] = R5900::beq(R5900::R_T6, R5900::R_ZERO, 0);
    code[words++] = R5900::nop();

    code[words++] = R5900::andi(R5900::R_T4, R5900::R_T0, 0x0FFFu);
    code[words++] = R5900::addu(R5900::R_T3, R5900::R_T3, R5900::R_T4);
    code[words++] = R5900::lw(R5900::R_T1,
        static_cast<int16_t>(rd * 4), R5900::R_A0);

    // t7 = &state->writes[writeCount], ArmMemoryWrite is 16 bytes.
    code[words++] = R5900::sll(R5900::R_T6, R5900::R_T5, 4);
    code[words++] = R5900::addiu(R5900::R_T7, R5900::R_A0, 112);
    code[words++] = R5900::addu(R5900::R_T7, R5900::R_T7, R5900::R_T6);

    // Capture the exact pre-store value for transactional rollback/differential.
    if (sizeCode == 0)
        code[words++] = R5900::lbu(R5900::R_T6, 0, R5900::R_T3);
    else if (sizeCode == 1)
        code[words++] = R5900::lhu(R5900::R_T6, 0, R5900::R_T3);
    else
        code[words++] = R5900::lw(R5900::R_T6, 0, R5900::R_T3);

    code[words++] = R5900::sw(R5900::R_T0, 0, R5900::R_T7);
    code[words++] = R5900::sw(R5900::R_T6, 4, R5900::R_T7);
    code[words++] = R5900::sw(R5900::R_T1, 8, R5900::R_T7);
    code[words++] = R5900::addiu(R5900::R_T6, R5900::R_ZERO,
        static_cast<int16_t>(sizeCode == 0 ? 1 : (sizeCode == 1 ? 2 : 4)));
    code[words++] = R5900::sw(R5900::R_T6, 12, R5900::R_T7);

    if (sizeCode == 0)
        code[words++] = R5900::sb(R5900::R_T1, 0, R5900::R_T3);
    else if (sizeCode == 1)
        code[words++] = R5900::sh(R5900::R_T1, 0, R5900::R_T3);
    else
        code[words++] = R5900::sw(R5900::R_T1, 0, R5900::R_T3);

    code[words++] = R5900::addiu(R5900::R_T5, R5900::R_T5, 1);
    code[words++] = R5900::sw(R5900::R_T5, 80, R5900::R_A0);

    if (writeBack)
    {
        if (preIndex)
        {
            code[words++] = R5900::sw(R5900::R_T0,
                static_cast<int16_t>(rn * 4), R5900::R_A0);
        }
        else
        {
            code[words++] = R5900::or_(R5900::R_T2, R5900::R_T0,
                R5900::R_ZERO);
            if (delta)
                code[words++] = R5900::addiu(R5900::R_T2, R5900::R_T2,
                    static_cast<int16_t>(delta));
            code[words++] = R5900::sw(R5900::R_T2,
                static_cast<int16_t>(rn * 4), R5900::R_A0);
        }
    }

    size_t fastDoneBranch = words;
    code[words++] = R5900::beq(R5900::R_ZERO, R5900::R_ZERO, 0);
    code[words++] = R5900::nop();

    // Cold/unsafe path: reuse the already differential-gated C++ helper.
    size_t slowPath = words;
    if (registerCpsr)
        code[words++] = R5900::sw(R5900::R_V1, 64, R5900::R_A0);
    if (registerCycles)
        code[words++] = R5900::sw(R5900::R_T8, 84, R5900::R_A0);

    code[words++] = R5900::addiu(R5900::R_SP, R5900::R_SP, -32);
    code[words++] = R5900::sw(R5900::R_RA, 28, R5900::R_SP);
    code[words++] = R5900::sw(R5900::R_A0, 24, R5900::R_SP);
    code[words++] = R5900::lui(R5900::R_A1,
        static_cast<uint16_t>(packed >> 16));
    code[words++] = R5900::ori(R5900::R_A1, R5900::R_A1,
        static_cast<uint16_t>(packed));
    uintptr_t helper = reinterpret_cast<uintptr_t>(&applyArmImmediateStoreFast);
    code[words++] = R5900::lui(R5900::R_T9,
        static_cast<uint16_t>(helper >> 16));
    code[words++] = R5900::ori(R5900::R_T9, R5900::R_T9,
        static_cast<uint16_t>(helper));
    code[words++] = R5900::jalr(R5900::R_RA, R5900::R_T9);
    code[words++] = R5900::nop();
    code[words++] = R5900::lw(R5900::R_A0, 24, R5900::R_SP);
    code[words++] = R5900::lw(R5900::R_RA, 28, R5900::R_SP);
    code[words++] = R5900::addiu(R5900::R_SP, R5900::R_SP, 32);

    if (registerCycles)
        code[words++] = R5900::lw(R5900::R_T8, 84, R5900::R_A0);
    if (registerCpsr)
        code[words++] = R5900::lw(R5900::R_V1, 64, R5900::R_A0);

    size_t done = words;
    auto patchBne = [&](size_t branch, int rs, int rt, size_t target)
    {
        if (branch == static_cast<size_t>(-1))
            return true;
        size_t distance = target - (branch + 1);
        if (distance > 0x7FFFu)
            return false;
        code[branch] = R5900::bne(rs, rt, static_cast<int16_t>(distance));
        return true;
    };
    auto patchBeq = [&](size_t branch, int rs, int rt, size_t target)
    {
        if (branch == static_cast<size_t>(-1))
            return true;
        size_t distance = target - (branch + 1);
        if (distance > 0x7FFFu)
            return false;
        code[branch] = R5900::beq(rs, rt, static_cast<int16_t>(distance));
        return true;
    };

    if (!patchBne(alignBranch, R5900::R_T6, R5900::R_ZERO, slowPath) ||
        !patchBne(cacheMissBranch, R5900::R_T5, R5900::R_T6, slowPath) ||
        !patchBeq(nullPageBranch, R5900::R_T3, R5900::R_ZERO, slowPath) ||
        !patchBeq(journalFullBranch, R5900::R_T6, R5900::R_ZERO, slowPath) ||
        !patchBeq(fastDoneBranch, R5900::R_ZERO, R5900::R_ZERO, done))
        return 0;

    if (conditionBranch != static_cast<size_t>(-1) &&
        !patchBeq(conditionBranch, R5900::R_T7, R5900::R_ZERO, done))
        return 0;

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

size_t emitArmRegisterOffsetAddress(uint8_t rn, uint8_t rm,
    uint8_t shiftType, uint8_t shiftAmount, bool addOffset, uint32_t *code)
{
    if (!code)
        return 0;

    size_t words = 0;
    code[words++] = R5900::lw(R5900::R_T0,
        static_cast<int16_t>(rn * 4), R5900::R_A0);
    code[words++] = R5900::lw(R5900::R_T1,
        static_cast<int16_t>(rm * 4), R5900::R_A0);

    switch (shiftType)
    {
        case 0: // LSL
            if (shiftAmount)
                code[words++] = R5900::sll(R5900::R_T1,
                    R5900::R_T1, shiftAmount);
            break;
        case 1: // LSR #0 means LSR #32
            if (shiftAmount)
                code[words++] = R5900::srl(R5900::R_T1,
                    R5900::R_T1, shiftAmount);
            else
                code[words++] = R5900::or_(R5900::R_T1,
                    R5900::R_ZERO, R5900::R_ZERO);
            break;
        case 2: // ASR #0 means ASR #32
            code[words++] = R5900::sra(R5900::R_T1, R5900::R_T1,
                shiftAmount ? shiftAmount : 31);
            break;
        case 3: // RRX was rejected by the decoder; amount is 1..31 here.
            if (!shiftAmount)
                return 0;
            code[words++] = R5900::srl(R5900::R_T2,
                R5900::R_T1, shiftAmount);
            code[words++] = R5900::sll(R5900::R_T3,
                R5900::R_T1, 32u - shiftAmount);
            code[words++] = R5900::or_(R5900::R_T1,
                R5900::R_T2, R5900::R_T3);
            break;
        default:
            return 0;
    }

    code[words++] = addOffset
        ? R5900::addu(R5900::R_T0, R5900::R_T0, R5900::R_T1)
        : R5900::subu(R5900::R_T0, R5900::R_T0, R5900::R_T1);
    return words;
}

size_t emitArmByteLoadCachedInstruction(uint32_t opcode,
    uint32_t *code, bool registerCycles, bool registerCpsr,
    bool suppressResultStore = false, uint8_t targetCpuId = 0)
{
    Ps2ArmAlu::DecodedByteTransfer decoded;
    if (!code || !Ps2ArmAlu::decodeByteTransfer(opcode, &decoded) ||
        !decoded.load)
        return 0;

    size_t words = 0;
    words += emitArmCycleAdd(1, code + words, registerCycles);

    size_t conditionBranch = static_cast<size_t>(-1);
    if (decoded.condition != 0xEu)
    {
        words += emitArmCondition(decoded.condition, code + words,
            registerCpsr);
        conditionBranch = words;
        code[words++] = R5900::beq(R5900::R_T7, R5900::R_ZERO, 0);
        code[words++] = R5900::nop();
    }

    if (targetCpuId)
        words += emitArmCycleAdd(2, code + words, registerCycles);

    if (decoded.registerOffset)
    {
        if (!decoded.preIndex)
            code[words++] = R5900::lw(R5900::R_T6,
                static_cast<int16_t>(decoded.rn * 4), R5900::R_A0);
        size_t addressWords = emitArmRegisterOffsetAddress(decoded.rn,
            decoded.rm, decoded.shiftType, decoded.shiftAmount,
            decoded.addOffset, code + words);
        if (!addressWords)
            return 0;
        words += addressWords;
        if (!decoded.preIndex)
        {
            code[words++] = R5900::or_(R5900::R_T9,
                R5900::R_T0, R5900::R_ZERO);
            code[words++] = R5900::or_(R5900::R_T0,
                R5900::R_T6, R5900::R_ZERO);
        }
    }
    else
    {
        code[words++] = R5900::lw(R5900::R_T0,
            static_cast<int16_t>(decoded.rn * 4), R5900::R_A0);
        int32_t delta = decoded.addOffset
            ? static_cast<int32_t>(decoded.immediateOffset)
            : -static_cast<int32_t>(decoded.immediateOffset);
        if (decoded.preIndex && delta)
            code[words++] = R5900::addiu(R5900::R_T0, R5900::R_T0,
                static_cast<int16_t>(delta));
    }

    code[words++] = R5900::andi(R5900::R_T4, R5900::R_T0, 0x0FFFu);
    code[words++] = R5900::srl(R5900::R_T1, R5900::R_T0, 12);
    code[words++] = R5900::addiu(R5900::R_T1, R5900::R_T1, 1);
    code[words++] = R5900::lw(R5900::R_T2, 88, R5900::R_A0);
    size_t missBranch = words;
    code[words++] = R5900::bne(R5900::R_T1, R5900::R_T2, 0);
    code[words++] = R5900::nop();

    code[words++] = R5900::lw(R5900::R_T3, 92, R5900::R_A0);
    code[words++] = R5900::addu(R5900::R_T3, R5900::R_T3, R5900::R_T4);
    code[words++] = R5900::lbu(R5900::R_T2, 0, R5900::R_T3);
    if (!suppressResultStore)
        code[words++] = R5900::sw(R5900::R_T2,
            static_cast<int16_t>(decoded.rd * 4), R5900::R_A0);
    if (decoded.writeBack)
    {
        if (decoded.preIndex)
        {
            code[words++] = R5900::sw(R5900::R_T0,
                static_cast<int16_t>(decoded.rn * 4), R5900::R_A0);
        }
        else
        {
            if (decoded.registerOffset)
            {
                code[words++] = R5900::sw(R5900::R_T9,
                    static_cast<int16_t>(decoded.rn * 4), R5900::R_A0);
            }
            else
            {
                int32_t delta = decoded.addOffset
                    ? static_cast<int32_t>(decoded.immediateOffset)
                    : -static_cast<int32_t>(decoded.immediateOffset);
                if (delta)
                    code[words++] = R5900::addiu(R5900::R_T6,
                        R5900::R_T0, static_cast<int16_t>(delta));
                else
                    code[words++] = R5900::or_(R5900::R_T6,
                        R5900::R_T0, R5900::R_ZERO);
                code[words++] = R5900::sw(R5900::R_T6,
                    static_cast<int16_t>(decoded.rn * 4), R5900::R_A0);
            }
        }
    }
    size_t hitDoneBranch = words;
    code[words++] = R5900::beq(R5900::R_ZERO, R5900::R_ZERO, 0);
    code[words++] = R5900::nop();

    size_t slowPath = words;
    if (registerCpsr)
        code[words++] = R5900::sw(R5900::R_V1, 64, R5900::R_A0);
    if (registerCycles)
        code[words++] = R5900::sw(R5900::R_T8, 84, R5900::R_A0);

    code[words++] = R5900::addiu(R5900::R_SP, R5900::R_SP, -32);
    code[words++] = R5900::sw(R5900::R_RA, 28, R5900::R_SP);
    code[words++] = R5900::sw(R5900::R_A0, 24, R5900::R_SP);
    if (decoded.writeBack)
    {
        code[words++] = R5900::sw(R5900::R_T0, 20, R5900::R_SP);
        if (!decoded.preIndex && decoded.registerOffset)
            code[words++] = R5900::sw(R5900::R_T9, 16, R5900::R_SP);
    }
    code[words++] = R5900::or_(R5900::R_A1, R5900::R_T0, R5900::R_ZERO);
    uintptr_t helper = reinterpret_cast<uintptr_t>(&refillArm9ByteReadCache);
    code[words++] = R5900::lui(R5900::R_T9,
        static_cast<uint16_t>(helper >> 16));
    code[words++] = R5900::ori(R5900::R_T9, R5900::R_T9,
        static_cast<uint16_t>(helper));
    code[words++] = R5900::jalr(R5900::R_RA, R5900::R_T9);
    code[words++] = R5900::nop();
    code[words++] = R5900::lw(R5900::R_A0, 24, R5900::R_SP);
    if (decoded.writeBack)
    {
        code[words++] = R5900::lw(R5900::R_T0, 20, R5900::R_SP);
        if (!decoded.preIndex && decoded.registerOffset)
            code[words++] = R5900::lw(R5900::R_T9, 16, R5900::R_SP);
    }
    code[words++] = R5900::lw(R5900::R_RA, 28, R5900::R_SP);
    code[words++] = R5900::addiu(R5900::R_SP, R5900::R_SP, 32);
    if (!suppressResultStore)
        code[words++] = R5900::sw(R5900::R_V0,
            static_cast<int16_t>(decoded.rd * 4), R5900::R_A0);
    code[words++] = R5900::or_(R5900::R_T2,
        R5900::R_V0, R5900::R_ZERO);
    if (decoded.writeBack)
    {
        if (decoded.preIndex)
        {
            code[words++] = R5900::sw(R5900::R_T0,
                static_cast<int16_t>(decoded.rn * 4), R5900::R_A0);
        }
        else
        {
            if (decoded.registerOffset)
            {
                code[words++] = R5900::sw(R5900::R_T9,
                    static_cast<int16_t>(decoded.rn * 4), R5900::R_A0);
            }
            else
            {
                int32_t delta = decoded.addOffset
                    ? static_cast<int32_t>(decoded.immediateOffset)
                    : -static_cast<int32_t>(decoded.immediateOffset);
                if (delta)
                    code[words++] = R5900::addiu(R5900::R_T6,
                        R5900::R_T0, static_cast<int16_t>(delta));
                else
                    code[words++] = R5900::or_(R5900::R_T6,
                        R5900::R_T0, R5900::R_ZERO);
                code[words++] = R5900::sw(R5900::R_T6,
                    static_cast<int16_t>(decoded.rn * 4), R5900::R_A0);
            }
        }
    }
    if (registerCycles)
        code[words++] = R5900::lw(R5900::R_T8, 84, R5900::R_A0);
    if (registerCpsr)
        code[words++] = R5900::lw(R5900::R_V1, 64, R5900::R_A0);

    size_t done = words;
    size_t missDistance = slowPath - (missBranch + 1);
    size_t doneDistance = done - (hitDoneBranch + 1);
    if (missDistance > 0x7FFFu || doneDistance > 0x7FFFu)
        return 0;
    code[missBranch] = R5900::bne(R5900::R_T1, R5900::R_T2,
        static_cast<int16_t>(missDistance));
    code[hitDoneBranch] = R5900::beq(R5900::R_ZERO, R5900::R_ZERO,
        static_cast<int16_t>(doneDistance));

    if (conditionBranch != static_cast<size_t>(-1))
    {
        size_t distance = done - (conditionBranch + 1);
        if (distance > 0x7FFFu)
            return 0;
        code[conditionBranch] = R5900::beq(R5900::R_T7,
            R5900::R_ZERO, static_cast<int16_t>(distance));
    }

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

size_t emitArmLiteralLoadDirectInstruction(uint32_t opcode, uint32_t pc,
    uint32_t *code, Memory *memory, bool registerCycles, bool registerCpsr,
    bool suppressResultStore = false, uint8_t targetCpuId = 0)
{
    Ps2ArmAlu::DecodedLiteralLoad decoded;
    if (!code || !memory || !Ps2ArmAlu::decodeLiteralLoad(opcode, &decoded))
        return 0;

    uint32_t base = pc + 8u;
    uint32_t address = decoded.addOffset
        ? base + decoded.immediateOffset
        : base - decoded.immediateOffset;
    if ((address & 3u) || (address & 0xFFFu) > 0xFFCu)
        return 0;

    uint8_t *page =
        memory->getDirectReadPage(targetCpuId != 0, address);
    if (!page)
        return 0;

    uintptr_t hostAddress = reinterpret_cast<uintptr_t>(
        page + (address & 0xFFFu));
    if (hostAddress > 0xFFFFFFFFu)
        return 0;

    size_t words = 0;
    words += emitArmCycleAdd(1, code + words, registerCycles);

    size_t conditionBranch = static_cast<size_t>(-1);
    if (decoded.condition != 0xEu)
    {
        words += emitArmCondition(decoded.condition, code + words,
            registerCpsr);
        conditionBranch = words;
        code[words++] = R5900::beq(R5900::R_T7, R5900::R_ZERO, 0);
        code[words++] = R5900::nop();
    }

    if (targetCpuId)
        words += emitArmCycleAdd(2, code + words, registerCycles);

    code[words++] = R5900::lui(R5900::R_T0,
        static_cast<uint16_t>(hostAddress >> 16));
    code[words++] = R5900::ori(R5900::R_T0, R5900::R_T0,
        static_cast<uint16_t>(hostAddress));
    code[words++] = R5900::lw(R5900::R_T1, 0, R5900::R_T0);
    if (!suppressResultStore)
        code[words++] = R5900::sw(R5900::R_T1,
            static_cast<int16_t>(decoded.rd * 4), R5900::R_A0);
    code[words++] = R5900::or_(R5900::R_T2,
        R5900::R_T1, R5900::R_ZERO);

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

size_t emitArmWordLoadCachedInstruction(uint32_t opcode,
    uint32_t *code, bool registerCycles, bool registerCpsr,
    bool suppressResultStore = false, uint8_t targetCpuId = 0)
{
    Ps2ArmAlu::DecodedSingleTransfer decoded;
    if (!code || !Ps2ArmAlu::decodeSingleTransfer(opcode, &decoded) ||
        !decoded.load)
        return 0;

    size_t words = 0;
    words += emitArmCycleAdd(1, code + words, registerCycles);

    size_t conditionBranch = static_cast<size_t>(-1);
    if (decoded.condition != 0xEu)
    {
        words += emitArmCondition(decoded.condition, code + words,
            registerCpsr);
        conditionBranch = words;
        code[words++] = R5900::beq(R5900::R_T7, R5900::R_ZERO, 0);
        code[words++] = R5900::nop();
    }

    // NooDS models a normal ARM7 word load as 3 cycles versus 1 on ARM9.
    // These two extra cycles are on the taken path only.
    if (targetCpuId)
        words += emitArmCycleAdd(2, code + words, registerCycles);

    if (decoded.registerOffset)
    {
        if (!decoded.preIndex)
            code[words++] = R5900::lw(R5900::R_T6,
                static_cast<int16_t>(decoded.rn * 4), R5900::R_A0);
        size_t addressWords = emitArmRegisterOffsetAddress(decoded.rn,
            decoded.rm, decoded.shiftType, decoded.shiftAmount,
            decoded.addOffset, code + words);
        if (!addressWords)
            return 0;
        words += addressWords;
        if (!decoded.preIndex)
        {
            code[words++] = R5900::or_(R5900::R_T9,
                R5900::R_T0, R5900::R_ZERO);
            code[words++] = R5900::or_(R5900::R_T0,
                R5900::R_T6, R5900::R_ZERO);
        }
    }
    else
    {
        code[words++] = R5900::lw(R5900::R_T0,
            static_cast<int16_t>(decoded.rn * 4), R5900::R_A0);
        int32_t delta = decoded.addOffset
            ? static_cast<int32_t>(decoded.immediateOffset)
            : -static_cast<int32_t>(decoded.immediateOffset);
        if (decoded.preIndex && delta)
            code[words++] = R5900::addiu(R5900::R_T0, R5900::R_T0,
                static_cast<int16_t>(delta));
    }

    // Generic ARM9 word helper currently admits aligned accesses only.
    code[words++] = R5900::andi(R5900::R_T4, R5900::R_T0, 3);
    size_t unalignedBranch = words;
    code[words++] = R5900::bne(R5900::R_T4, R5900::R_ZERO, 0);
    code[words++] = R5900::nop();

    code[words++] = R5900::andi(R5900::R_T4, R5900::R_T0, 0x0FFFu);
    code[words++] = R5900::srl(R5900::R_T1, R5900::R_T0, 12);
    code[words++] = R5900::addiu(R5900::R_T1, R5900::R_T1, 1);
    code[words++] = R5900::lw(R5900::R_T2, 88, R5900::R_A0);
    size_t missBranch = words;
    code[words++] = R5900::bne(R5900::R_T1, R5900::R_T2, 0);
    code[words++] = R5900::nop();

    code[words++] = R5900::lw(R5900::R_T3, 92, R5900::R_A0);
    code[words++] = R5900::addu(R5900::R_T3, R5900::R_T3, R5900::R_T4);
    code[words++] = R5900::lw(R5900::R_T2, 0, R5900::R_T3);
    if (!suppressResultStore)
        code[words++] = R5900::sw(R5900::R_T2,
            static_cast<int16_t>(decoded.rd * 4), R5900::R_A0);
    if (decoded.writeBack)
    {
        if (decoded.preIndex)
        {
            code[words++] = R5900::sw(R5900::R_T0,
                static_cast<int16_t>(decoded.rn * 4), R5900::R_A0);
        }
        else
        {
            if (decoded.registerOffset)
            {
                code[words++] = R5900::sw(R5900::R_T9,
                    static_cast<int16_t>(decoded.rn * 4), R5900::R_A0);
            }
            else
            {
                int32_t delta = decoded.addOffset
                    ? static_cast<int32_t>(decoded.immediateOffset)
                    : -static_cast<int32_t>(decoded.immediateOffset);
                if (delta)
                    code[words++] = R5900::addiu(R5900::R_T6,
                        R5900::R_T0, static_cast<int16_t>(delta));
                else
                    code[words++] = R5900::or_(R5900::R_T6,
                        R5900::R_T0, R5900::R_ZERO);
                code[words++] = R5900::sw(R5900::R_T6,
                    static_cast<int16_t>(decoded.rn * 4), R5900::R_A0);
            }
        }
    }
    size_t hitDoneBranch = words;
    code[words++] = R5900::beq(R5900::R_ZERO, R5900::R_ZERO, 0);
    code[words++] = R5900::nop();

    size_t slowPath = words;
    if (registerCpsr)
        code[words++] = R5900::sw(R5900::R_V1, 64, R5900::R_A0);
    if (registerCycles)
        code[words++] = R5900::sw(R5900::R_T8, 84, R5900::R_A0);

    code[words++] = R5900::addiu(R5900::R_SP, R5900::R_SP, -32);
    code[words++] = R5900::sw(R5900::R_RA, 28, R5900::R_SP);
    code[words++] = R5900::sw(R5900::R_A0, 24, R5900::R_SP);
    if (decoded.writeBack)
    {
        code[words++] = R5900::sw(R5900::R_T0, 20, R5900::R_SP);
        if (!decoded.preIndex && decoded.registerOffset)
            code[words++] = R5900::sw(R5900::R_T9, 16, R5900::R_SP);
    }
    code[words++] = R5900::or_(R5900::R_A1, R5900::R_T0, R5900::R_ZERO);
    uintptr_t helper = reinterpret_cast<uintptr_t>(&refillArm9WordReadCache);
    code[words++] = R5900::lui(R5900::R_T9,
        static_cast<uint16_t>(helper >> 16));
    code[words++] = R5900::ori(R5900::R_T9, R5900::R_T9,
        static_cast<uint16_t>(helper));
    code[words++] = R5900::jalr(R5900::R_RA, R5900::R_T9);
    code[words++] = R5900::nop();
    code[words++] = R5900::lw(R5900::R_A0, 24, R5900::R_SP);
    if (decoded.writeBack)
    {
        code[words++] = R5900::lw(R5900::R_T0, 20, R5900::R_SP);
        if (!decoded.preIndex && decoded.registerOffset)
            code[words++] = R5900::lw(R5900::R_T9, 16, R5900::R_SP);
    }
    code[words++] = R5900::lw(R5900::R_RA, 28, R5900::R_SP);
    code[words++] = R5900::addiu(R5900::R_SP, R5900::R_SP, 32);
    if (!suppressResultStore)
        code[words++] = R5900::sw(R5900::R_V0,
            static_cast<int16_t>(decoded.rd * 4), R5900::R_A0);
    code[words++] = R5900::or_(R5900::R_T2,
        R5900::R_V0, R5900::R_ZERO);
    if (decoded.writeBack)
    {
        if (decoded.preIndex)
        {
            code[words++] = R5900::sw(R5900::R_T0,
                static_cast<int16_t>(decoded.rn * 4), R5900::R_A0);
        }
        else
        {
            if (decoded.registerOffset)
            {
                code[words++] = R5900::sw(R5900::R_T9,
                    static_cast<int16_t>(decoded.rn * 4), R5900::R_A0);
            }
            else
            {
                int32_t delta = decoded.addOffset
                    ? static_cast<int32_t>(decoded.immediateOffset)
                    : -static_cast<int32_t>(decoded.immediateOffset);
                if (delta)
                    code[words++] = R5900::addiu(R5900::R_T6,
                        R5900::R_T0, static_cast<int16_t>(delta));
                else
                    code[words++] = R5900::or_(R5900::R_T6,
                        R5900::R_T0, R5900::R_ZERO);
                code[words++] = R5900::sw(R5900::R_T6,
                    static_cast<int16_t>(decoded.rn * 4), R5900::R_A0);
            }
        }
    }
    if (registerCycles)
        code[words++] = R5900::lw(R5900::R_T8, 84, R5900::R_A0);
    if (registerCpsr)
        code[words++] = R5900::lw(R5900::R_V1, 64, R5900::R_A0);

    size_t done = words;
    auto patchBne = [&](size_t branch, int rs, int rt, size_t target)
    {
        size_t distance = target - (branch + 1);
        if (distance > 0x7FFFu)
            return false;
        code[branch] = R5900::bne(rs, rt, static_cast<int16_t>(distance));
        return true;
    };
    auto patchBeq = [&](size_t branch, int rs, int rt, size_t target)
    {
        size_t distance = target - (branch + 1);
        if (distance > 0x7FFFu)
            return false;
        code[branch] = R5900::beq(rs, rt, static_cast<int16_t>(distance));
        return true;
    };

    if (!patchBne(unalignedBranch, R5900::R_T4, R5900::R_ZERO, slowPath) ||
        !patchBne(missBranch, R5900::R_T1, R5900::R_T2, slowPath) ||
        !patchBeq(hitDoneBranch, R5900::R_ZERO, R5900::R_ZERO, done))
        return 0;

    if (conditionBranch != static_cast<size_t>(-1) &&
        !patchBeq(conditionBranch, R5900::R_T7, R5900::R_ZERO, done))
        return 0;

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

size_t emitArmSingleBlockTransferFastInstruction(uint32_t opcode,
    uint32_t *code, bool registerCycles, bool registerCpsr,
    int forwardedGuestReg = -1, uint8_t forwardedCondition = 0xEu,
    bool reuseCondition = false, uint8_t targetCpuId = 0)
{
    Ps2ArmAlu::DecodedSingleBlockTransfer decoded;
    if (!code || !Ps2ArmAlu::decodeSingleBlockTransfer(opcode, &decoded))
        return 0;

    uint32_t packed = static_cast<uint32_t>(decoded.rn) |
        (static_cast<uint32_t>(decoded.reg) << 4) |
        (decoded.load ? BIT(8) : 0u) |
        (decoded.writeBack ? BIT(9) : 0u);

    size_t words = 0;

    // Failed condition costs one ARM9 cycle; taken one-register LDM/STM costs 2.
    words += emitArmCycleAdd(1, code + words, registerCycles);
    size_t conditionBranch = static_cast<size_t>(-1);
    if (decoded.condition != 0xEu)
    {
        if (!reuseCondition)
            words += emitArmCondition(decoded.condition, code + words,
                registerCpsr);
        conditionBranch = words;
        code[words++] = R5900::beq(R5900::R_T7, R5900::R_ZERO, 0);
        code[words++] = R5900::nop();
    }
    words += emitArmCycleAdd(
        (targetCpuId && decoded.load) ? 2u : 1u,
        code + words, registerCycles);

    // If the immediately preceding generated instruction left the exact store
    // value in t2 under the same ARM condition, preserve it before the cache
    // machinery reuses t2. Architectural state is still written normally by
    // the producer, so the helper fallback remains fully correct.
    bool useForwardedStore = !decoded.load &&
        forwardedGuestReg == decoded.reg &&
        forwardedCondition == decoded.condition;
    if (useForwardedStore)
        code[words++] = R5900::or_(R5900::R_T9,
            R5900::R_T2, R5900::R_ZERO);

    // Increment-after: effective address is the original base register.
    code[words++] = R5900::lw(R5900::R_T0,
        static_cast<int16_t>(decoded.rn * 4), R5900::R_A0);

    // Direct word access is intentionally aligned in this dynarec subset.
    code[words++] = R5900::andi(R5900::R_T4, R5900::R_T0, 3);
    size_t unalignedBranch = words;
    code[words++] = R5900::bne(R5900::R_T4, R5900::R_ZERO, 0);
    code[words++] = R5900::nop();

    code[words++] = R5900::andi(R5900::R_T4, R5900::R_T0, 0x0FFFu);
    code[words++] = R5900::srl(R5900::R_T1, R5900::R_T0, 12);
    code[words++] = R5900::addiu(R5900::R_T1, R5900::R_T1, 1);

    size_t cacheMissBranch = words;
    size_t nullPageBranch = static_cast<size_t>(-1);
    size_t journalFullBranch = static_cast<size_t>(-1);

    if (decoded.load)
    {
        code[words++] = R5900::lw(R5900::R_T2, 88, R5900::R_A0);
        cacheMissBranch = words;
        code[words++] = R5900::bne(R5900::R_T1, R5900::R_T2, 0);
        code[words++] = R5900::nop();
        code[words++] = R5900::lw(R5900::R_T3, 92, R5900::R_A0);
        nullPageBranch = words;
        code[words++] = R5900::beq(R5900::R_T3, R5900::R_ZERO, 0);
        code[words++] = R5900::nop();

        code[words++] = R5900::addu(R5900::R_T3, R5900::R_T3, R5900::R_T4);
        code[words++] = R5900::lw(R5900::R_T2, 0, R5900::R_T3);
        code[words++] = R5900::sw(R5900::R_T2,
            static_cast<int16_t>(decoded.reg * 4), R5900::R_A0);
    }
    else
    {
        code[words++] = R5900::lw(R5900::R_T2, 104, R5900::R_A0);
        cacheMissBranch = words;
        code[words++] = R5900::bne(R5900::R_T1, R5900::R_T2, 0);
        code[words++] = R5900::nop();
        code[words++] = R5900::lw(R5900::R_T3, 108, R5900::R_A0);
        nullPageBranch = words;
        code[words++] = R5900::beq(R5900::R_T3, R5900::R_ZERO, 0);
        code[words++] = R5900::nop();

        code[words++] = R5900::lw(R5900::R_T5, 80, R5900::R_A0);
        code[words++] = R5900::sltiu(R5900::R_T6, R5900::R_T5, 16);
        journalFullBranch = words;
        code[words++] = R5900::beq(R5900::R_T6, R5900::R_ZERO, 0);
        code[words++] = R5900::nop();

        code[words++] = R5900::addu(R5900::R_T3, R5900::R_T3, R5900::R_T4);
        if (useForwardedStore)
            code[words++] = R5900::or_(R5900::R_T1,
                R5900::R_T9, R5900::R_ZERO);
        else
            code[words++] = R5900::lw(R5900::R_T1,
                static_cast<int16_t>(decoded.reg * 4), R5900::R_A0);

        // ArmMemoryWrite is 16 bytes and starts at state+112.
        code[words++] = R5900::sll(R5900::R_T6, R5900::R_T5, 4);
        code[words++] = R5900::addiu(R5900::R_V0, R5900::R_A0, 112);
        code[words++] = R5900::addu(R5900::R_V0, R5900::R_V0, R5900::R_T6);
        code[words++] = R5900::lw(R5900::R_T6, 0, R5900::R_T3);
        code[words++] = R5900::sw(R5900::R_T0, 0, R5900::R_V0);
        code[words++] = R5900::sw(R5900::R_T6, 4, R5900::R_V0);
        code[words++] = R5900::sw(R5900::R_T1, 8, R5900::R_V0);
        code[words++] = R5900::addiu(R5900::R_T6, R5900::R_ZERO, 4);
        code[words++] = R5900::sw(R5900::R_T6, 12, R5900::R_V0);
        code[words++] = R5900::sw(R5900::R_T1, 0, R5900::R_T3);
        code[words++] = R5900::addiu(R5900::R_T5, R5900::R_T5, 1);
        code[words++] = R5900::sw(R5900::R_T5, 80, R5900::R_A0);
    }

    if (decoded.writeBack)
    {
        // Preserve t2 across LDM writeback so the loaded guest value remains
        // available to an immediately following generated consumer.
        int writebackReg = decoded.load ? R5900::R_T6 : R5900::R_T2;
        code[words++] = R5900::addiu(writebackReg, R5900::R_T0, 4);
        code[words++] = R5900::sw(writebackReg,
            static_cast<int16_t>(decoded.rn * 4), R5900::R_A0);
    }

    size_t fastDoneBranch = words;
    code[words++] = R5900::beq(R5900::R_ZERO, R5900::R_ZERO, 0);
    code[words++] = R5900::nop();

    // Cold/unsafe path seeds the same persistent read/write page cache.
    size_t slowPath = words;
    if (registerCpsr)
        code[words++] = R5900::sw(R5900::R_V1, 64, R5900::R_A0);
    if (registerCycles)
        code[words++] = R5900::sw(R5900::R_T8, 84, R5900::R_A0);

    code[words++] = R5900::addiu(R5900::R_SP, R5900::R_SP, -32);
    code[words++] = R5900::sw(R5900::R_RA, 28, R5900::R_SP);
    code[words++] = R5900::sw(R5900::R_A0, 24, R5900::R_SP);
    code[words++] = R5900::lui(R5900::R_A1,
        static_cast<uint16_t>(packed >> 16));
    code[words++] = R5900::ori(R5900::R_A1, R5900::R_A1,
        static_cast<uint16_t>(packed));
    uintptr_t helper =
        reinterpret_cast<uintptr_t>(&applyArmSingleBlockTransferFast);
    code[words++] = R5900::lui(R5900::R_T9,
        static_cast<uint16_t>(helper >> 16));
    code[words++] = R5900::ori(R5900::R_T9, R5900::R_T9,
        static_cast<uint16_t>(helper));
    code[words++] = R5900::jalr(R5900::R_RA, R5900::R_T9);
    code[words++] = R5900::nop();
    code[words++] = R5900::lw(R5900::R_A0, 24, R5900::R_SP);
    code[words++] = R5900::lw(R5900::R_RA, 28, R5900::R_SP);
    code[words++] = R5900::addiu(R5900::R_SP, R5900::R_SP, 32);

    // The helper fallback writes the loaded guest register to ArmAluState but
    // may clobber caller-saved t2/t7. Reconstruct the forwarding value and
    // cached condition before rejoining generated code.
    if (decoded.load)
        code[words++] = R5900::lw(R5900::R_T2,
            static_cast<int16_t>(decoded.reg * 4), R5900::R_A0);
    if (registerCycles)
        code[words++] = R5900::lw(R5900::R_T8, 84, R5900::R_A0);
    if (registerCpsr)
        code[words++] = R5900::lw(R5900::R_V1, 64, R5900::R_A0);
    if (decoded.condition != 0xEu)
        words += emitArmCondition(decoded.condition, code + words,
            registerCpsr);

    size_t done = words;

    auto patchBne = [&](size_t branch, int rs, int rt, size_t target)
    {
        size_t distance = target - (branch + 1);
        if (distance > 0x7FFFu)
            return false;
        code[branch] = R5900::bne(rs, rt, static_cast<int16_t>(distance));
        return true;
    };
    auto patchBeq = [&](size_t branch, int rs, int rt, size_t target)
    {
        if (branch == static_cast<size_t>(-1))
            return true;
        size_t distance = target - (branch + 1);
        if (distance > 0x7FFFu)
            return false;
        code[branch] = R5900::beq(rs, rt, static_cast<int16_t>(distance));
        return true;
    };

    if (!patchBne(unalignedBranch, R5900::R_T4, R5900::R_ZERO, slowPath) ||
        !patchBne(cacheMissBranch, R5900::R_T1, R5900::R_T2, slowPath) ||
        !patchBeq(nullPageBranch, R5900::R_T3, R5900::R_ZERO, slowPath) ||
        !patchBeq(journalFullBranch, R5900::R_T6, R5900::R_ZERO, slowPath) ||
        !patchBeq(fastDoneBranch, R5900::R_ZERO, R5900::R_ZERO, done))
        return 0;

    if (conditionBranch != static_cast<size_t>(-1) &&
        !patchBeq(conditionBranch, R5900::R_T7, R5900::R_ZERO, done))
        return 0;

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

size_t emitArmBranchExchangeDirectInstruction(uint32_t opcode,
    uint32_t *code, bool registerCycles, bool registerCpsr)
{
    Ps2ArmAlu::DecodedBranchExchange decoded;
    if (!code || !Ps2ArmAlu::decodeBranchExchange(opcode, &decoded))
        return 0;

    size_t words = 0;
    words += emitArmCycleAdd(1, code + words, registerCycles);

    int cpsrReg = registerCpsr ? R5900::R_V1 : R5900::R_T5;
    if (!registerCpsr)
        code[words++] = R5900::lw(cpsrReg, 64, R5900::R_A0);

    size_t conditionBranch = static_cast<size_t>(-1);
    if (decoded.condition != 0xEu)
    {
        words += emitArmCondition(decoded.condition, code + words,
            registerCpsr);
        conditionBranch = words;
        code[words++] = R5900::beq(R5900::R_T7, R5900::R_ZERO, 0);
        code[words++] = R5900::nop();
    }

    // Taken BX costs three cycles total.
    words += emitArmCycleAdd(2, code + words, registerCycles);

    code[words++] = R5900::lw(R5900::R_T0,
        static_cast<int16_t>(decoded.rm * 4), R5900::R_A0);
    code[words++] = R5900::andi(R5900::R_T1, R5900::R_T0, 1);
    code[words++] = R5900::srl(R5900::R_T2, R5900::R_T0, 1);
    code[words++] = R5900::sll(R5900::R_T2, R5900::R_T2, 1);
    code[words++] = R5900::sw(R5900::R_T2, 68, R5900::R_A0);

    // CPSR.T follows target bit 0; privileged mode and NZCV are untouched.
    code[words++] = R5900::lui(R5900::R_T3, 0xFFFFu);
    code[words++] = R5900::ori(R5900::R_T3, R5900::R_T3, 0xFFDFu);
    code[words++] = R5900::and_(cpsrReg, cpsrReg, R5900::R_T3);
    code[words++] = R5900::sll(R5900::R_T1, R5900::R_T1, 5);
    code[words++] = R5900::or_(cpsrReg, cpsrReg, R5900::R_T1);

    if (!registerCpsr)
        code[words++] = R5900::sw(cpsrReg, 64, R5900::R_A0);

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

size_t emitArmMultiBlockTransferInstruction(uint32_t opcode,
    uint32_t *code, bool registerCycles, bool registerCpsr,
    uint8_t targetCpuId = 0)
{
    Ps2ArmAlu::DecodedMultiBlockTransfer decoded;
    if (!code || !Ps2ArmAlu::decodeMultiBlockTransfer(opcode, &decoded))
        return 0;

    size_t words = 0;

    // Every ARM instruction costs one cycle even when its condition fails.
    words += emitArmCycleAdd(1u, code + words, registerCycles);
    size_t conditionBranch = static_cast<size_t>(-1);
    if (decoded.condition != 0xEu)
    {
        words += emitArmCondition(decoded.condition, code + words,
            registerCpsr);
        conditionBranch = words;
        code[words++] = R5900::beq(R5900::R_T7, R5900::R_ZERO, 0);
        code[words++] = R5900::nop();
    }

    uint32_t takenCycles = targetCpuId
        ? decoded.count + (decoded.load ? 2u : 1u)
        : decoded.count;
    words += emitArmCycleAdd(takenCycles - 1u,
        code + words, registerCycles);

    // t0 = first transfer address. IA starts at Rn; DB starts at
    // Rn-count*4. The decoder excludes Rn from Rlist, so writeback is simple.
    code[words++] = R5900::lw(R5900::R_T0,
        static_cast<int16_t>(decoded.rn * 4), R5900::R_A0);
    if (decoded.decrementBefore)
        code[words++] = R5900::addiu(R5900::R_T0, R5900::R_T0,
            static_cast<int16_t>(-static_cast<int>(decoded.count * 4u)));

    // The inline path requires aligned contiguous words wholly inside one
    // direct page. Everything else falls through the portable direct helper.
    code[words++] = R5900::andi(R5900::R_T4, R5900::R_T0, 3u);
    size_t unalignedBranch = words;
    code[words++] = R5900::bne(R5900::R_T4, R5900::R_ZERO, 0);
    code[words++] = R5900::nop();

    code[words++] = R5900::andi(R5900::R_T4, R5900::R_T0, 0x0FFFu);
    uint16_t endLimit = static_cast<uint16_t>(
        0x1000u - static_cast<uint32_t>(decoded.count) * 4u + 1u);
    code[words++] = R5900::sltiu(R5900::R_T6, R5900::R_T4, endLimit);
    size_t crossPageBranch = words;
    code[words++] = R5900::beq(R5900::R_T6, R5900::R_ZERO, 0);
    code[words++] = R5900::nop();

    code[words++] = R5900::srl(R5900::R_T1, R5900::R_T0, 12);
    code[words++] = R5900::addiu(R5900::R_T1, R5900::R_T1, 1);

    size_t cacheMissBranch = words;
    size_t nullPageBranch = static_cast<size_t>(-1);
    size_t journalFullBranch = static_cast<size_t>(-1);

    if (decoded.load)
    {
        code[words++] = R5900::lw(R5900::R_T2, 88, R5900::R_A0);
        cacheMissBranch = words;
        code[words++] = R5900::bne(R5900::R_T1, R5900::R_T2, 0);
        code[words++] = R5900::nop();
        code[words++] = R5900::lw(R5900::R_T3, 92, R5900::R_A0);
        nullPageBranch = words;
        code[words++] = R5900::beq(R5900::R_T3, R5900::R_ZERO, 0);
        code[words++] = R5900::nop();
        code[words++] = R5900::addu(R5900::R_T3,
            R5900::R_T3, R5900::R_T4);

        uint16_t mask = decoded.registerMask;
        uint32_t slot = 0;
        while (mask)
        {
            uint8_t reg = static_cast<uint8_t>(__builtin_ctz(mask));
            code[words++] = R5900::lw(R5900::R_T2,
                static_cast<int16_t>(slot * 4u), R5900::R_T3);
            code[words++] = R5900::sw(R5900::R_T2,
                static_cast<int16_t>(reg * 4u), R5900::R_A0);
            slot++;
            mask = static_cast<uint16_t>(mask & (mask - 1u));
        }
    }
    else
    {
        code[words++] = R5900::lw(R5900::R_T2, 104, R5900::R_A0);
        cacheMissBranch = words;
        code[words++] = R5900::bne(R5900::R_T1, R5900::R_T2, 0);
        code[words++] = R5900::nop();
        code[words++] = R5900::lw(R5900::R_T3, 108, R5900::R_A0);
        nullPageBranch = words;
        code[words++] = R5900::beq(R5900::R_T3, R5900::R_ZERO, 0);
        code[words++] = R5900::nop();

        code[words++] = R5900::lw(R5900::R_T5, 80, R5900::R_A0);
        code[words++] = R5900::sltiu(R5900::R_T6, R5900::R_T5,
            static_cast<uint16_t>(17u - decoded.count));
        journalFullBranch = words;
        code[words++] = R5900::beq(R5900::R_T6, R5900::R_ZERO, 0);
        code[words++] = R5900::nop();

        code[words++] = R5900::addu(R5900::R_T3,
            R5900::R_T3, R5900::R_T4);

        uint16_t mask = decoded.registerMask;
        uint32_t slot = 0;
        while (mask)
        {
            uint8_t reg = static_cast<uint8_t>(__builtin_ctz(mask));
            code[words++] = R5900::lw(R5900::R_T1,
                static_cast<int16_t>(reg * 4u), R5900::R_A0);
            code[words++] = R5900::sll(R5900::R_T6, R5900::R_T5, 4);
            code[words++] = R5900::addiu(R5900::R_V0,
                R5900::R_A0, 112);
            code[words++] = R5900::addu(R5900::R_V0,
                R5900::R_V0, R5900::R_T6);
            code[words++] = R5900::lw(R5900::R_T2,
                static_cast<int16_t>(slot * 4u), R5900::R_T3);

            if (slot)
            {
                code[words++] = R5900::addiu(R5900::R_T6,
                    R5900::R_T0, static_cast<int16_t>(slot * 4u));
                code[words++] = R5900::sw(R5900::R_T6, 0, R5900::R_V0);
            }
            else
            {
                code[words++] = R5900::sw(R5900::R_T0, 0, R5900::R_V0);
            }

            code[words++] = R5900::sw(R5900::R_T2, 4, R5900::R_V0);
            code[words++] = R5900::sw(R5900::R_T1, 8, R5900::R_V0);
            code[words++] = R5900::addiu(R5900::R_T6,
                R5900::R_ZERO, 4);
            code[words++] = R5900::sw(R5900::R_T6, 12, R5900::R_V0);
            code[words++] = R5900::sw(R5900::R_T1,
                static_cast<int16_t>(slot * 4u), R5900::R_T3);
            code[words++] = R5900::addiu(R5900::R_T5, R5900::R_T5, 1);

            slot++;
            mask = static_cast<uint16_t>(mask & (mask - 1u));
        }

        code[words++] = R5900::sw(R5900::R_T5, 80, R5900::R_A0);
    }

    if (decoded.writeBack)
    {
        if (decoded.decrementBefore)
        {
            code[words++] = R5900::sw(R5900::R_T0,
                static_cast<int16_t>(decoded.rn * 4u), R5900::R_A0);
        }
        else
        {
            code[words++] = R5900::addiu(R5900::R_T2,
                R5900::R_T0,
                static_cast<int16_t>(decoded.count * 4u));
            code[words++] = R5900::sw(R5900::R_T2,
                static_cast<int16_t>(decoded.rn * 4u), R5900::R_A0);
        }
    }

    size_t fastDoneBranch = words;
    code[words++] = R5900::beq(R5900::R_ZERO, R5900::R_ZERO, 0);
    code[words++] = R5900::nop();

    size_t slowPath = words;
    if (registerCpsr)
        code[words++] = R5900::sw(R5900::R_V1, 64, R5900::R_A0);
    if (registerCycles)
        code[words++] = R5900::sw(R5900::R_T8, 84, R5900::R_A0);

    code[words++] = R5900::addiu(R5900::R_SP, R5900::R_SP, -32);
    code[words++] = R5900::sw(R5900::R_RA, 28, R5900::R_SP);
    code[words++] = R5900::sw(R5900::R_A0, 24, R5900::R_SP);
    code[words++] = R5900::lui(R5900::R_A1,
        static_cast<uint16_t>(opcode >> 16));
    code[words++] = R5900::ori(R5900::R_A1, R5900::R_A1,
        static_cast<uint16_t>(opcode));
    uintptr_t helper =
        reinterpret_cast<uintptr_t>(&applyArmMultiBlockTransferFast);
    code[words++] = R5900::lui(R5900::R_T9,
        static_cast<uint16_t>(helper >> 16));
    code[words++] = R5900::ori(R5900::R_T9, R5900::R_T9,
        static_cast<uint16_t>(helper));
    code[words++] = R5900::jalr(R5900::R_RA, R5900::R_T9);
    code[words++] = R5900::nop();
    code[words++] = R5900::lw(R5900::R_A0, 24, R5900::R_SP);
    code[words++] = R5900::lw(R5900::R_RA, 28, R5900::R_SP);
    code[words++] = R5900::addiu(R5900::R_SP, R5900::R_SP, 32);

    if (registerCycles)
        code[words++] = R5900::lw(R5900::R_T8, 84, R5900::R_A0);
    if (registerCpsr)
        code[words++] = R5900::lw(R5900::R_V1, 64, R5900::R_A0);

    size_t done = words;

    auto patchBeq = [&](size_t branch, int rs, int rt, size_t target)
    {
        if (branch == static_cast<size_t>(-1))
            return true;
        size_t distance = target - (branch + 1);
        if (distance > 0x7FFFu)
            return false;
        code[branch] = R5900::beq(rs, rt, static_cast<int16_t>(distance));
        return true;
    };
    auto patchBne = [&](size_t branch, int rs, int rt, size_t target)
    {
        size_t distance = target - (branch + 1);
        if (distance > 0x7FFFu)
            return false;
        code[branch] = R5900::bne(rs, rt, static_cast<int16_t>(distance));
        return true;
    };

    if (!patchBne(unalignedBranch, R5900::R_T4, R5900::R_ZERO, slowPath) ||
        !patchBeq(crossPageBranch, R5900::R_T6, R5900::R_ZERO, slowPath) ||
        !patchBne(cacheMissBranch, R5900::R_T1, R5900::R_T2, slowPath) ||
        !patchBeq(nullPageBranch, R5900::R_T3, R5900::R_ZERO, slowPath) ||
        !patchBeq(journalFullBranch, R5900::R_T6, R5900::R_ZERO, slowPath) ||
        !patchBeq(fastDoneBranch, R5900::R_ZERO, R5900::R_ZERO, done) ||
        !patchBeq(conditionBranch, R5900::R_T7, R5900::R_ZERO, done))
        return 0;

    return words;
}

size_t emitArm7VariableMultiplyTiming(uint32_t *code,
    bool registerCycles, uint32_t constantExtra)
{
    if (!code)
        return 0;

    size_t words = 0;

    // ARM7TDMI multiplier early termination is based on the upper bits of Rs,
    // not on a conventional signed 8/16/24-bit range. For m=1, bits [31:8]
    // must all equal the sign bit; m=2 checks [31:16], and m=3 checks [31:24].
    // Build a full sign mask once, then compare each logical upper slice with
    // the corresponding slice of that mask. This correctly includes negative
    // boundaries such as -256, -65536 and -2^24 in m=1/2/3 respectively.
    code[words++] = R5900::sra(R5900::R_T5, R5900::R_T1, 31);

    code[words++] = R5900::srl(R5900::R_T6, R5900::R_T1, 8);
    code[words++] = R5900::srl(R5900::R_T4, R5900::R_T5, 8);
    size_t notM1Branch = words;
    code[words++] = R5900::bne(R5900::R_T6, R5900::R_T4, 0);
    code[words++] = R5900::nop();
    words += emitArmCycleAdd(1u + constantExtra,
        code + words, registerCycles);
    size_t m1DoneBranch = words;
    code[words++] = R5900::beq(R5900::R_ZERO, R5900::R_ZERO, 0);
    code[words++] = R5900::nop();

    size_t checkM2 = words;
    code[words++] = R5900::srl(R5900::R_T6, R5900::R_T1, 16);
    code[words++] = R5900::srl(R5900::R_T4, R5900::R_T5, 16);
    size_t notM2Branch = words;
    code[words++] = R5900::bne(R5900::R_T6, R5900::R_T4, 0);
    code[words++] = R5900::nop();
    words += emitArmCycleAdd(2u + constantExtra,
        code + words, registerCycles);
    size_t m2DoneBranch = words;
    code[words++] = R5900::beq(R5900::R_ZERO, R5900::R_ZERO, 0);
    code[words++] = R5900::nop();

    size_t checkM3 = words;
    code[words++] = R5900::srl(R5900::R_T6, R5900::R_T1, 24);
    code[words++] = R5900::srl(R5900::R_T4, R5900::R_T5, 24);
    size_t notM3Branch = words;
    code[words++] = R5900::bne(R5900::R_T6, R5900::R_T4, 0);
    code[words++] = R5900::nop();
    words += emitArmCycleAdd(3u + constantExtra,
        code + words, registerCycles);
    size_t m3DoneBranch = words;
    code[words++] = R5900::beq(R5900::R_ZERO, R5900::R_ZERO, 0);
    code[words++] = R5900::nop();

    size_t useM4 = words;
    words += emitArmCycleAdd(4u + constantExtra,
        code + words, registerCycles);
    size_t timingDone = words;

    auto patchTimingBranch = [&](size_t branch, size_t target, bool equal)
    {
        size_t distance = target - (branch + 1);
        if (distance > 0x7FFFu)
            return false;
        code[branch] = equal
            ? R5900::beq(R5900::R_ZERO, R5900::R_ZERO,
                static_cast<int16_t>(distance))
            : R5900::bne(R5900::R_T6, R5900::R_T4,
                static_cast<int16_t>(distance));
        return true;
    };

    if (!patchTimingBranch(notM1Branch, checkM2, false) ||
        !patchTimingBranch(m1DoneBranch, timingDone, true) ||
        !patchTimingBranch(notM2Branch, checkM3, false) ||
        !patchTimingBranch(m2DoneBranch, timingDone, true) ||
        !patchTimingBranch(notM3Branch, useM4, false) ||
        !patchTimingBranch(m3DoneBranch, timingDone, true))
        return 0;

    return words;
}

size_t emitArmMultiplyInstruction(uint32_t opcode, uint32_t *code,
    bool registerCycles, bool registerCpsr, bool suppressResultStore = false,
    uint8_t targetCpuId = 0)
{
    Ps2ArmAlu::DecodedMultiply decoded;
    if (!code || !Ps2ArmAlu::decodeMultiply(opcode, &decoded))
        return 0;

    size_t words = 0;

    // Every ARM instruction consumes one cycle even when its condition fails.
    words += emitArmCycleAdd(1, code + words, registerCycles);

    size_t conditionBranch = static_cast<size_t>(-1);
    if (decoded.condition != 0xEu)
    {
        words += emitArmCondition(decoded.condition, code + words,
            registerCpsr);
        conditionBranch = words;
        code[words++] = R5900::beq(R5900::R_T7, R5900::R_ZERO, 0);
        code[words++] = R5900::nop();
    }

    code[words++] = R5900::lw(R5900::R_T1,
        static_cast<int16_t>(decoded.rs * 4), R5900::R_A0);

    if (targetCpuId)
    {
        // One base cycle is already charged; ARM7 adds m (+1 for MLA).
        size_t timingWords = emitArm7VariableMultiplyTiming(code + words,
            registerCycles, decoded.accumulate ? 1u : 0u);
        if (!timingWords)
            return 0;
        words += timingWords;
    }
    else
    {
        // Successful ARM9 MUL/MLA = 2 cycles; S variants = 4 cycles.
        words += emitArmCycleAdd(decoded.setFlags ? 3u : 1u,
            code + words, registerCycles);
    }

    code[words++] = R5900::lw(R5900::R_T0,
        static_cast<int16_t>(decoded.rm * 4), R5900::R_A0);
    code[words++] = R5900::multu(R5900::R_T2,
        R5900::R_T0, R5900::R_T1);

    if (decoded.accumulate)
    {
        code[words++] = R5900::lw(R5900::R_T3,
            static_cast<int16_t>(decoded.rn * 4), R5900::R_A0);
        code[words++] = R5900::addu(R5900::R_T2,
            R5900::R_T2, R5900::R_T3);
    }

    if (!suppressResultStore)
        code[words++] = R5900::sw(R5900::R_T2,
            static_cast<int16_t>(decoded.rd * 4), R5900::R_A0);

    if (decoded.setFlags)
    {
        int cpsrReg = registerCpsr ? R5900::R_V1 : R5900::R_T5;
        if (!registerCpsr)
            code[words++] = R5900::lw(cpsrReg, 64, R5900::R_A0);

        // MULS/MLAS update N/Z and preserve C/V + low CPSR bits.
        code[words++] = R5900::lui(R5900::R_T6, 0x3FFFu);
        code[words++] = R5900::ori(R5900::R_T6, R5900::R_T6, 0xFFFFu);
        code[words++] = R5900::and_(cpsrReg, cpsrReg, R5900::R_T6);
        code[words++] = R5900::srl(R5900::R_T6, R5900::R_T2, 31);
        code[words++] = R5900::sll(R5900::R_T6, R5900::R_T6, 31);
        code[words++] = R5900::or_(cpsrReg, cpsrReg, R5900::R_T6);
        code[words++] = R5900::sltiu(R5900::R_T6, R5900::R_T2, 1);
        code[words++] = R5900::sll(R5900::R_T6, R5900::R_T6, 30);
        code[words++] = R5900::or_(cpsrReg, cpsrReg, R5900::R_T6);

        if (!registerCpsr)
            code[words++] = R5900::sw(cpsrReg, 64, R5900::R_A0);
    }

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

size_t emitArmLongMultiplyInstruction(uint32_t opcode,
    uint32_t *code, bool registerCycles, bool registerCpsr,
    uint8_t targetCpuId = 0)
{
    Ps2ArmAlu::DecodedLongMultiply decoded;
    if (!code || !Ps2ArmAlu::decodeLongMultiply(opcode, &decoded))
        return 0;

    size_t words = 0;
    words += emitArmCycleAdd(1, code + words, registerCycles);

    size_t conditionBranch = static_cast<size_t>(-1);
    if (decoded.condition != 0xEu)
    {
        words += emitArmCondition(decoded.condition, code + words,
            registerCpsr);
        conditionBranch = words;
        code[words++] = R5900::beq(R5900::R_T7, R5900::R_ZERO, 0);
        code[words++] = R5900::nop();
    }

    code[words++] = R5900::lw(R5900::R_T1,
        static_cast<int16_t>(decoded.rs * 4), R5900::R_A0);

    if (targetCpuId)
    {
        // Long multiply totals m+2, or m+3 for accumulate. One base cycle is
        // already charged, so the variable tail is m+1(+1 accumulate).
        size_t timingWords = emitArm7VariableMultiplyTiming(code + words,
            registerCycles, 1u + (decoded.accumulate ? 1u : 0u));
        if (!timingWords)
            return 0;
        words += timingWords;
    }
    else
    {
        words += emitArmCycleAdd(decoded.setFlags ? 4u : 2u,
            code + words, registerCycles);
    }

    code[words++] = R5900::lw(R5900::R_T0,
        static_cast<int16_t>(decoded.rm * 4), R5900::R_A0);
    code[words++] = decoded.signedMultiply
        ? R5900::multHiLo(R5900::R_T0, R5900::R_T1)
        : R5900::multuHiLo(R5900::R_T0, R5900::R_T1);
    code[words++] = R5900::mflo(R5900::R_T2);
    code[words++] = R5900::mfhi(R5900::R_T3);

    if (decoded.accumulate)
    {
        code[words++] = R5900::lw(R5900::R_T4,
            static_cast<int16_t>(decoded.rdLo * 4), R5900::R_A0);
        code[words++] = R5900::lw(R5900::R_T5,
            static_cast<int16_t>(decoded.rdHi * 4), R5900::R_A0);
        code[words++] = R5900::addu(R5900::R_T6,
            R5900::R_T2, R5900::R_T4);
        code[words++] = R5900::sltu(R5900::R_T7,
            R5900::R_T6, R5900::R_T2);
        code[words++] = R5900::addu(R5900::R_T3,
            R5900::R_T3, R5900::R_T5);
        code[words++] = R5900::addu(R5900::R_T3,
            R5900::R_T3, R5900::R_T7);
        code[words++] = R5900::or_(R5900::R_T2,
            R5900::R_T6, R5900::R_ZERO);
    }

    code[words++] = R5900::sw(R5900::R_T2,
        static_cast<int16_t>(decoded.rdLo * 4), R5900::R_A0);
    code[words++] = R5900::sw(R5900::R_T3,
        static_cast<int16_t>(decoded.rdHi * 4), R5900::R_A0);

    if (decoded.setFlags)
    {
        int cpsrReg = registerCpsr ? R5900::R_V1 : R5900::R_T5;
        if (!registerCpsr)
            code[words++] = R5900::lw(cpsrReg, 64, R5900::R_A0);

        // Match NooDS: long multiply S derives N/Z from RdHi only.
        code[words++] = R5900::lui(R5900::R_T6, 0x3FFFu);
        code[words++] = R5900::ori(R5900::R_T6, R5900::R_T6, 0xFFFFu);
        code[words++] = R5900::and_(cpsrReg, cpsrReg, R5900::R_T6);
        code[words++] = R5900::srl(R5900::R_T6, R5900::R_T3, 31);
        code[words++] = R5900::sll(R5900::R_T6, R5900::R_T6, 31);
        code[words++] = R5900::or_(cpsrReg, cpsrReg, R5900::R_T6);
        code[words++] = R5900::sltiu(R5900::R_T6, R5900::R_T3, 1);
        code[words++] = R5900::sll(R5900::R_T6, R5900::R_T6, 30);
        code[words++] = R5900::or_(cpsrReg, cpsrReg, R5900::R_T6);
        if (!registerCpsr)
            code[words++] = R5900::sw(cpsrReg, 64, R5900::R_A0);
    }

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

size_t emitArmPsrTransferInstruction(uint32_t opcode, uint32_t *code,
    bool registerCycles, bool registerCpsr)
{
    Ps2ArmAlu::DecodedPsrTransfer decoded;
    if (!code || !Ps2ArmAlu::decodePsrTransfer(opcode, &decoded))
        return 0;

    size_t words = 0;
    if (registerCpsr)
        code[words++] = R5900::sw(R5900::R_V1, 64, R5900::R_A0);
    if (registerCycles)
        code[words++] = R5900::sw(R5900::R_T8, 84, R5900::R_A0);

    code[words++] = R5900::addiu(R5900::R_SP, R5900::R_SP, -32);
    code[words++] = R5900::sw(R5900::R_RA, 28, R5900::R_SP);
    code[words++] = R5900::sw(R5900::R_A0, 24, R5900::R_SP);
    code[words++] = R5900::lui(R5900::R_A1,
        static_cast<uint16_t>(opcode >> 16));
    code[words++] = R5900::ori(R5900::R_A1, R5900::R_A1,
        static_cast<uint16_t>(opcode));
    uintptr_t helper =
        reinterpret_cast<uintptr_t>(&applyArmPsrTransfer);
    code[words++] = R5900::lui(R5900::R_T9,
        static_cast<uint16_t>(helper >> 16));
    code[words++] = R5900::ori(R5900::R_T9, R5900::R_T9,
        static_cast<uint16_t>(helper));
    code[words++] = R5900::jalr(R5900::R_RA, R5900::R_T9);
    code[words++] = R5900::nop();
    code[words++] = R5900::lw(R5900::R_A0, 24, R5900::R_SP);
    code[words++] = R5900::lw(R5900::R_RA, 28, R5900::R_SP);
    code[words++] = R5900::addiu(R5900::R_SP, R5900::R_SP, 32);

    if (registerCycles)
        code[words++] = R5900::lw(R5900::R_T8, 84, R5900::R_A0);
    if (registerCpsr)
        code[words++] = R5900::lw(R5900::R_V1, 64, R5900::R_A0);
    return words;
}

size_t emitArmInstruction(uint32_t opcode, uint32_t pc, uint32_t *code,
    bool registerCycles = false, bool registerCpsr = false,
    Memory *directMemory = nullptr, int forwardedGuestReg = -1,
    bool suppressResultStore = false, uint8_t forwardedCondition = 0xEu,
    bool reuseCondition = false, uint8_t targetCpuId = 0)
{
    Ps2ArmAlu::Decoded decoded;
    if (Ps2ArmAlu::decode(opcode, &decoded))
    {
        int usableForward = (decoded.condition == 0xEu &&
            forwardedCondition == 0xEu) ? forwardedGuestReg : -1;
        return emitArmAluInstruction(opcode, code, registerCycles,
            registerCpsr, usableForward, suppressResultStore);
    }

    Ps2ArmAlu::DecodedMultiply multiply;
    if (Ps2ArmAlu::decodeMultiply(opcode, &multiply))
        return emitArmMultiplyInstruction(opcode, code,
            registerCycles, registerCpsr, suppressResultStore, targetCpuId);

    Ps2ArmAlu::DecodedLongMultiply longMultiply;
    if (Ps2ArmAlu::decodeLongMultiply(opcode, &longMultiply))
        return emitArmLongMultiplyInstruction(opcode, code,
            registerCycles, registerCpsr, targetCpuId);

    Ps2ArmAlu::DecodedMultiBlockTransfer multiBlockTransfer;
    if (Ps2ArmAlu::decodeMultiBlockTransfer(opcode, &multiBlockTransfer))
        return emitArmMultiBlockTransferInstruction(opcode, code,
            registerCycles, registerCpsr, targetCpuId);

    Ps2ArmAlu::DecodedHalfwordTransfer halfwordTransfer;
    if (Ps2ArmAlu::decodeHalfwordTransfer(opcode, &halfwordTransfer))
    {
        if (halfwordTransfer.load)
            return emitArmHalfwordLoadCachedInstruction(opcode, code,
                registerCycles, registerCpsr, suppressResultStore,
                targetCpuId);
        return halfwordTransfer.immediate
            ? emitArmImmediateStoreFastInstruction(opcode, code,
                registerCycles, registerCpsr, targetCpuId)
            : emitArmHalfwordRegisterStoreFastInstruction(opcode, code,
                registerCycles, registerCpsr, targetCpuId);
    }

    Ps2ArmAlu::DecodedByteTransfer byteTransfer;
    if (Ps2ArmAlu::decodeByteTransfer(opcode, &byteTransfer))
    {
        if (byteTransfer.load)
            return emitArmByteLoadCachedInstruction(opcode, code,
                registerCycles, registerCpsr, suppressResultStore,
                targetCpuId);
        return byteTransfer.registerOffset
            ? emitArmRegisterStoreFastInstruction(opcode, code,
                registerCycles, registerCpsr, targetCpuId)
            : emitArmImmediateStoreFastInstruction(opcode, code,
                registerCycles, registerCpsr, targetCpuId);
    }

    Ps2ArmAlu::DecodedLiteralLoad literalLoad;
    if (Ps2ArmAlu::decodeLiteralLoad(opcode, &literalLoad))
    {
        if (directMemory)
        {
            size_t direct = emitArmLiteralLoadDirectInstruction(opcode,
                pc, code, directMemory, registerCycles, registerCpsr,
                suppressResultStore, targetCpuId);
            if (direct)
                return direct;
        }

        // A literal that does not resolve to a direct page is deliberately
        // left to the interpreter instead of embedding a C++ helper in a hot
        // compiled block. Normal ROM/RAM literals take the direct path above.
        return 0;
    }

    Ps2ArmAlu::DecodedSingleTransfer singleTransfer;
    if (Ps2ArmAlu::decodeSingleTransfer(opcode, &singleTransfer))
    {
        if (singleTransfer.load)
            return emitArmWordLoadCachedInstruction(opcode, code,
                registerCycles, registerCpsr, suppressResultStore,
                targetCpuId);
        return singleTransfer.registerOffset
            ? emitArmRegisterStoreFastInstruction(opcode, code,
                registerCycles, registerCpsr, targetCpuId)
            : emitArmImmediateStoreFastInstruction(opcode, code,
                registerCycles, registerCpsr, targetCpuId);
    }

    Ps2ArmAlu::DecodedSingleBlockTransfer blockTransfer;
    if (Ps2ArmAlu::decodeSingleBlockTransfer(opcode, &blockTransfer))
    {
        return emitArmSingleBlockTransferFastInstruction(opcode,
            code, registerCycles, registerCpsr,
            forwardedGuestReg, forwardedCondition,
            reuseCondition, targetCpuId);
    }

    Ps2ArmAlu::DecodedPsrTransfer psrTransfer;
    if (Ps2ArmAlu::decodePsrTransfer(opcode, &psrTransfer))
        return emitArmPsrTransferInstruction(opcode, code,
            registerCycles, registerCpsr);

    Ps2ArmAlu::DecodedBranchExchange branchExchange;
    if (Ps2ArmAlu::decodeBranchExchange(opcode, &branchExchange))
        return emitArmBranchExchangeDirectInstruction(opcode,
            code, registerCycles, registerCpsr);

    return emitArmBranchInstruction(opcode, pc, code,
        registerCycles, registerCpsr, reuseCondition);
}
}

bool DynarecR5900::emitArmAluBlock(uint32_t opcode, uint32_t **entry, uint8_t *rdOut)
{
    if (!entry || !rdOut)
        return false;

    Ps2ArmAlu::Decoded decoded;
    if (!Ps2ArmAlu::decode(opcode, &decoded))
        return false;

    // Direct register-shift flag paths are larger than the original ALU
    // subset. Reserve enough headroom for the largest admitted single op.
    uint32_t *code = cache.reserveWords(96);
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

uint32_t maxArmInstructionCycles(uint32_t opcode, uint8_t targetCpuId = 0)
{
    Ps2ArmAlu::Decoded decoded;
    if (Ps2ArmAlu::decode(opcode, &decoded))
        return decoded.registerShift ? 2u : 1u;

    Ps2ArmAlu::DecodedMultiply multiply;
    if (Ps2ArmAlu::decodeMultiply(opcode, &multiply))
    {
        if (targetCpuId)
            return multiply.accumulate ? 6u : 5u;
        return multiply.setFlags ? 4u : 2u;
    }

    Ps2ArmAlu::DecodedLongMultiply longMultiply;
    if (Ps2ArmAlu::decodeLongMultiply(opcode, &longMultiply))
    {
        if (targetCpuId)
            return longMultiply.accumulate ? 7u : 6u;
        return longMultiply.setFlags ? 5u : 3u;
    }

    Ps2ArmAlu::DecodedLiteralLoad literalLoad;
    if (Ps2ArmAlu::decodeLiteralLoad(opcode, &literalLoad) && targetCpuId)
        return 3u;

    Ps2ArmAlu::DecodedSingleTransfer singleTransfer;
    if (Ps2ArmAlu::decodeSingleTransfer(opcode, &singleTransfer) &&
        targetCpuId)
        return singleTransfer.load ? 3u : 2u;

    Ps2ArmAlu::DecodedByteTransfer byteTransfer;
    if (Ps2ArmAlu::decodeByteTransfer(opcode, &byteTransfer) &&
        targetCpuId)
        return byteTransfer.load ? 3u : 2u;

    Ps2ArmAlu::DecodedHalfwordTransfer halfwordTransfer;
    if (Ps2ArmAlu::decodeHalfwordTransfer(opcode, &halfwordTransfer) &&
        targetCpuId)
        return halfwordTransfer.load ? 3u : 2u;

    Ps2ArmAlu::DecodedMultiBlockTransfer multiBlockTransfer;
    if (Ps2ArmAlu::decodeMultiBlockTransfer(opcode, &multiBlockTransfer))
    {
        if (targetCpuId)
            return multiBlockTransfer.count +
                (multiBlockTransfer.load ? 2u : 1u);
        return multiBlockTransfer.count;
    }

    Ps2ArmAlu::DecodedSingleBlockTransfer blockTransfer;
    if (Ps2ArmAlu::decodeSingleBlockTransfer(opcode, &blockTransfer))
        return targetCpuId && blockTransfer.load ? 3u : 2u;

    Ps2ArmAlu::DecodedBranch branch;
    Ps2ArmAlu::DecodedBranchExchange branchExchange;
    if (Ps2ArmAlu::decodeBranch(opcode, &branch) ||
        Ps2ArmAlu::decodeBranchExchange(opcode, &branchExchange))
        return 3u;

    return 1u;
}

uint32_t minArmInstructionCycles(uint32_t opcode, uint8_t targetCpuId = 0)
{
    Ps2ArmAlu::Decoded decoded;
    if (Ps2ArmAlu::decode(opcode, &decoded))
        return (decoded.registerShift && decoded.condition == 0xEu) ? 2u : 1u;

    Ps2ArmAlu::DecodedMultiply multiply;
    if (Ps2ArmAlu::decodeMultiply(opcode, &multiply))
    {
        if (multiply.condition != 0xEu)
            return 1u;
        if (targetCpuId)
            return multiply.accumulate ? 3u : 2u;
        return multiply.setFlags ? 4u : 2u;
    }

    Ps2ArmAlu::DecodedLongMultiply longMultiply;
    if (Ps2ArmAlu::decodeLongMultiply(opcode, &longMultiply))
    {
        if (longMultiply.condition != 0xEu)
            return 1u;
        if (targetCpuId)
            return longMultiply.accumulate ? 4u : 3u;
        return longMultiply.setFlags ? 5u : 3u;
    }

    Ps2ArmAlu::DecodedLiteralLoad literalLoad;
    if (Ps2ArmAlu::decodeLiteralLoad(opcode, &literalLoad) && targetCpuId)
        return literalLoad.condition == 0xEu ? 3u : 1u;

    Ps2ArmAlu::DecodedSingleTransfer singleTransfer;
    if (Ps2ArmAlu::decodeSingleTransfer(opcode, &singleTransfer) &&
        targetCpuId)
    {
        if (singleTransfer.condition != 0xEu)
            return 1u;
        return singleTransfer.load ? 3u : 2u;
    }

    Ps2ArmAlu::DecodedByteTransfer byteTransfer;
    if (Ps2ArmAlu::decodeByteTransfer(opcode, &byteTransfer) &&
        targetCpuId)
    {
        if (byteTransfer.condition != 0xEu)
            return 1u;
        return byteTransfer.load ? 3u : 2u;
    }

    Ps2ArmAlu::DecodedHalfwordTransfer halfwordTransfer;
    if (Ps2ArmAlu::decodeHalfwordTransfer(opcode, &halfwordTransfer) &&
        targetCpuId)
    {
        if (halfwordTransfer.condition != 0xEu)
            return 1u;
        return halfwordTransfer.load ? 3u : 2u;
    }

    Ps2ArmAlu::DecodedMultiBlockTransfer multiBlockTransfer;
    if (Ps2ArmAlu::decodeMultiBlockTransfer(opcode, &multiBlockTransfer))
    {
        if (multiBlockTransfer.condition != 0xEu)
            return 1u;
        if (targetCpuId)
            return multiBlockTransfer.count +
                (multiBlockTransfer.load ? 2u : 1u);
        return multiBlockTransfer.count;
    }

    Ps2ArmAlu::DecodedSingleBlockTransfer blockTransfer;
    if (Ps2ArmAlu::decodeSingleBlockTransfer(opcode, &blockTransfer))
    {
        if (blockTransfer.condition != 0xEu)
            return 1u;
        return targetCpuId && blockTransfer.load ? 3u : 2u;
    }

    Ps2ArmAlu::DecodedBranch branch;
    if (Ps2ArmAlu::decodeBranch(opcode, &branch))
        return branch.condition == 0xEu ? 3u : 1u;

    Ps2ArmAlu::DecodedBranchExchange branchExchange;
    if (Ps2ArmAlu::decodeBranchExchange(opcode, &branchExchange))
        return branchExchange.condition == 0xEu ? 3u : 1u;

    return 1u;
}

bool armSequenceIsReadOnlySelfLoop(const uint32_t *opcodes, size_t count,
    uint32_t guestPc)
{
    if (!opcodes || !count)
        return false;

    for (size_t i = 0; i < count; i++)
    {
        Ps2ArmAlu::DecodedHalfwordTransfer half;
        Ps2ArmAlu::DecodedByteTransfer byte;
        Ps2ArmAlu::DecodedSingleTransfer single;
        Ps2ArmAlu::DecodedSingleBlockTransfer block;
        Ps2ArmAlu::DecodedMultiBlockTransfer multi;
        if ((Ps2ArmAlu::decodeHalfwordTransfer(opcodes[i], &half) && !half.load) ||
            (Ps2ArmAlu::decodeByteTransfer(opcodes[i], &byte) && !byte.load) ||
            (Ps2ArmAlu::decodeSingleTransfer(opcodes[i], &single) && !single.load) ||
            (Ps2ArmAlu::decodeSingleBlockTransfer(opcodes[i], &block) && !block.load) ||
            (Ps2ArmAlu::decodeMultiBlockTransfer(opcodes[i], &multi) && !multi.load))
            return false;
    }

    Ps2ArmAlu::DecodedBranch branch;
    if (!Ps2ArmAlu::decodeBranch(opcodes[count - 1], &branch) || branch.link)
        return false;

    uint32_t branchPc = guestPc + static_cast<uint32_t>((count - 1) * 4);
    uint32_t target = branchPc + 8u + static_cast<uint32_t>(branch.offset);
    return target == guestPc;
}

bool DynarecR5900::emitArmAluSequence(const uint32_t *opcodes, size_t count,
    uint32_t guestPc, uint32_t **entry, uint32_t *hostWords)
{
    if (!opcodes || !count || !entry || !hostWords)
        return false;

    uint32_t sequenceMinCycles = 0;
    uint32_t sequenceMaxCycles = 0;
    for (size_t i = 0; i < count; i++)
    {
        sequenceMinCycles += minArmInstructionCycles(opcodes[i], targetCpuId);
        sequenceMaxCycles += maxArmInstructionCycles(opcodes[i], targetCpuId);
    }
    bool internalSelfLoop = sequenceMaxCycles &&
        armSequenceIsReadOnlySelfLoop(opcodes, count, guestPc);

    // Conditions plus exact NZCV generation are deliberately explicit for the
    // correctness gate. Keep cycles in t8 and, when direct generated
    // instructions consume/produce flags, keep CPSR in v1 too.
    bool registerCpsr = false;
    bool writesRegisterCpsr = false;
    for (size_t i = 0; i < count; i++)
    {
        Ps2ArmAlu::Decoded decoded;
        if (Ps2ArmAlu::decode(opcodes[i], &decoded))
        {
            if (decoded.condition != 0xEu || decoded.setFlags)
                registerCpsr = true;
            if (decoded.setFlags)
                writesRegisterCpsr = true;
            continue;
        }

        Ps2ArmAlu::DecodedMultiply multiply;
        if (Ps2ArmAlu::decodeMultiply(opcodes[i], &multiply))
        {
            if (multiply.condition != 0xEu || multiply.setFlags)
                registerCpsr = true;
            if (multiply.setFlags)
                writesRegisterCpsr = true;
            continue;
        }

        Ps2ArmAlu::DecodedLongMultiply longMultiply;
        if (Ps2ArmAlu::decodeLongMultiply(opcodes[i], &longMultiply))
        {
            if (longMultiply.condition != 0xEu || longMultiply.setFlags)
                registerCpsr = true;
            if (longMultiply.setFlags)
                writesRegisterCpsr = true;
            continue;
        }

        Ps2ArmAlu::DecodedBranch branch;
        if (Ps2ArmAlu::decodeBranch(opcodes[i], &branch) &&
            branch.condition != 0xEu)
        {
            registerCpsr = true;
            continue;
        }

        Ps2ArmAlu::DecodedBranchExchange branchExchange;
        if (Ps2ArmAlu::decodeBranchExchange(opcodes[i], &branchExchange))
        {
            registerCpsr = true;
            writesRegisterCpsr = true;
        }
    }

    // Store fast paths include cache checks, transaction journaling and a
    // cold helper fallback, so the old 70-word/instruction reservation is no
    // longer a safe upper bound. This is reservation headroom only; commitWords
    // still advances by the exact emitted size.
    // A single 14-register STM expands into journal bookkeeping for every
    // word. Keep enough headroom for that worst admitted instruction while
    // commitWords still advances by the exact generated size.
    const size_t maxWords = count * 256 + (internalSelfLoop ? 48 : 8);
    uint32_t *code = cache.reserveWords(maxWords);
    if (!code)
        return false;

    size_t words = 0;
    if (registerCpsr)
        code[words++] = R5900::lw(R5900::R_V1, 64, R5900::R_A0);

    // Every generated block reports a cycle delta, not a chain-global total.
    // The active executor supplies the remaining budget through cycleLimit and
    // accumulates deltas between blocks. Starting t8 at zero avoids reloading
    // state.cycles on every internal-chain entry.
    code[words++] = R5900::or_(R5900::R_T8, R5900::R_ZERO, R5900::R_ZERO);

    // Self-loops may execute dozens of iterations inside one scheduler slot.
    // Keep guestExecuted in callee-saved s0 instead of loading/storing state on
    // every iteration. Save the caller's s0 once so the generated function
    // still obeys the MIPS ABI; nested C++ helpers preserve s0 themselves.
    if (internalSelfLoop)
    {
        code[words++] = R5900::addiu(R5900::R_SP, R5900::R_SP, -16);
        code[words++] = R5900::sw(R5900::R_S0, 12, R5900::R_SP);
        code[words++] = R5900::lw(R5900::R_S0, 100, R5900::R_A0);
    }

    size_t bodyStart = words;
    int forwardedGuestReg = -1;
    uint8_t forwardedCondition = 0xFFu;
    int reusableCondition = -1;
    for (size_t i = 0; i < count; i++)
    {
        Ps2ArmAlu::Decoded currentAlu;
        bool currentDirectAlu = Ps2ArmAlu::decode(opcodes[i], &currentAlu) &&
            currentAlu.condition == 0xEu;

        int producedGuestReg = -1;
        uint8_t producedCondition = 0xFFu;
        if (currentDirectAlu && currentAlu.writesResult)
        {
            producedGuestReg = currentAlu.rd;
            producedCondition = currentAlu.condition;
        }
        else
        {
            Ps2ArmAlu::DecodedMultiply multiply;
            if (Ps2ArmAlu::decodeMultiply(opcodes[i], &multiply) &&
                multiply.condition == 0xEu)
            {
                // The direct R5900 multiply emitter leaves its low-32 result
                // in t2, matching the existing ALU/load forwarding ABI.
                producedGuestReg = multiply.rd;
                producedCondition = multiply.condition;
            }
            else
            {
            Ps2ArmAlu::DecodedHalfwordTransfer half;
            Ps2ArmAlu::DecodedByteTransfer byte;
            Ps2ArmAlu::DecodedSingleTransfer word;
            if (Ps2ArmAlu::decodeHalfwordTransfer(opcodes[i], &half) &&
                half.condition == 0xEu && half.load)
            {
                producedGuestReg = half.rd;
                producedCondition = half.condition;
            }
            else if (Ps2ArmAlu::decodeByteTransfer(opcodes[i], &byte) &&
                byte.condition == 0xEu && byte.load)
            {
                producedGuestReg = byte.rd;
                producedCondition = byte.condition;
            }
            else if (Ps2ArmAlu::decodeSingleTransfer(opcodes[i], &word) &&
                word.condition == 0xEu && word.load)
            {
                producedGuestReg = word.rd;
                producedCondition = word.condition;
            }
            else
            {
                Ps2ArmAlu::DecodedLiteralLoad literal;
                if (Ps2ArmAlu::decodeLiteralLoad(opcodes[i], &literal) &&
                    literal.condition == 0xEu)
                {
                    uint32_t literalPc =
                        guestPc + static_cast<uint32_t>(i * 4);
                    uint32_t base = literalPc + 8u;
                    uint32_t address = literal.addOffset
                        ? base + literal.immediateOffset
                        : base - literal.immediateOffset;
                    if (!(address & 3u) && memory &&
                        memory->getDirectReadPage(targetCpuId != 0, address))
                    {
                        producedGuestReg = literal.rd;
                        producedCondition = literal.condition;
                    }
                }
            }
            }

            Ps2ArmAlu::DecodedSingleBlockTransfer blockTransferProducer;
            if (producedGuestReg < 0 &&
                Ps2ArmAlu::decodeSingleBlockTransfer(opcodes[i],
                    &blockTransferProducer) &&
                blockTransferProducer.load)
            {
                producedGuestReg = blockTransferProducer.reg;
                producedCondition = blockTransferProducer.condition;
            }
        }

        bool suppressCurrentStore = false;
        if (producedGuestReg >= 0 && i + 1 < count)
        {
            Ps2ArmAlu::Decoded nextAlu;
            if (Ps2ArmAlu::decode(opcodes[i + 1], &nextAlu) &&
                nextAlu.condition == 0xEu &&
                nextAlu.writesResult &&
                nextAlu.rd == static_cast<uint8_t>(producedGuestReg))
            {
                suppressCurrentStore = true;
            }
        }

        Ps2ArmAlu::DecodedSingleBlockTransfer conditionTransfer;
        Ps2ArmAlu::DecodedBranch conditionBranchDecoded;
        bool isConditionalTransfer =
            Ps2ArmAlu::decodeSingleBlockTransfer(opcodes[i],
                &conditionTransfer) &&
            conditionTransfer.condition != 0xEu;
        bool isConditionalBranch =
            Ps2ArmAlu::decodeBranch(opcodes[i], &conditionBranchDecoded) &&
            conditionBranchDecoded.condition != 0xEu;
        uint8_t currentCondition = isConditionalTransfer
            ? conditionTransfer.condition
            : (isConditionalBranch ? conditionBranchDecoded.condition : 0xFFu);
        bool reuseCurrentCondition = reusableCondition >= 0 &&
            currentCondition == static_cast<uint8_t>(reusableCondition);

        size_t emitted = emitArmInstruction(opcodes[i],
            guestPc + static_cast<uint32_t>(i * 4), code + words, true,
            registerCpsr, memory, forwardedGuestReg, suppressCurrentStore,
            forwardedCondition, reuseCurrentCondition, targetCpuId);
        if (!emitted)
            return false;
        words += emitted;

        if (isConditionalTransfer)
            reusableCondition = currentCondition;
        else
            reusableCondition = -1;

        forwardedGuestReg = producedGuestReg;
        forwardedCondition = producedCondition;

        // Generic ALU-S -> B.EQ/B.NE fusion. The ALU emitter has already
        // committed architectural NZCV to v1/state, but its result is still
        // live in t2. Test that value directly for Z and skip the redundant
        // CPSR condition extraction in the following branch.
        if (i + 1 < count)
        {
            Ps2ArmAlu::Decoded alu;
            Ps2ArmAlu::DecodedBranch branch;
            if (Ps2ArmAlu::decode(opcodes[i], &alu) &&
                alu.condition == 0xEu && alu.setFlags &&
                Ps2ArmAlu::decodeBranch(opcodes[i + 1], &branch) &&
                !branch.link &&
                (branch.condition == 0x0u || branch.condition == 0x1u))
            {
                size_t fused = emitArmEqNeBranchFromResult(opcodes[i + 1],
                    guestPc + static_cast<uint32_t>((i + 1) * 4),
                    code + words, true);
                if (!fused)
                    return false;
                words += fused;
                i++;
                forwardedGuestReg = -1;
                forwardedCondition = 0xFFu;
            }
        }

        // Instructions that did not leave a known architectural value in t2
        // terminate host-register forwarding. Fast direct loads are producers
        // just like ALU writes.
        if (producedGuestReg < 0)
        {
            forwardedGuestReg = -1;
            forwardedCondition = 0xFFu;
        }
    }

    size_t loopExitBranches[4] = {};
    size_t loopExitCount = 0;
    size_t loopBackBranch = static_cast<size_t>(-1);

    if (internalSelfLoop)
    {
        // Count one completed guest block iteration in resident s0.
        code[words++] = R5900::addiu(R5900::R_S0, R5900::R_S0,
            static_cast<int16_t>(count));

        // Shadow/differential states leave cycleLimit=0, so they execute
        // exactly one iteration. Live states may repeat at most 64 times.
        code[words++] = R5900::lw(R5900::R_T1, 96, R5900::R_A0);
        loopExitBranches[loopExitCount++] = words;
        code[words++] = R5900::beq(R5900::R_T1, R5900::R_ZERO, 0);
        code[words++] = R5900::nop();

        code[words++] = R5900::sltiu(R5900::R_T2, R5900::R_S0,
            static_cast<uint16_t>(count * 64u));
        loopExitBranches[loopExitCount++] = words;
        code[words++] = R5900::beq(R5900::R_T2, R5900::R_ZERO, 0);
        code[words++] = R5900::nop();

        // Only a taken terminal branch back to this exact block may repeat.
        code[words++] = R5900::lw(R5900::R_T2, 68, R5900::R_A0);
        code[words++] = R5900::lui(R5900::R_T3,
            static_cast<uint16_t>(guestPc >> 16));
        code[words++] = R5900::ori(R5900::R_T3, R5900::R_T3,
            static_cast<uint16_t>(guestPc));
        loopExitBranches[loopExitCount++] = words;
        code[words++] = R5900::bne(R5900::R_T2, R5900::R_T3, 0);
        code[words++] = R5900::nop();

        // Require room for the entire worst-case next iteration.
        code[words++] = R5900::addiu(R5900::R_T2, R5900::R_T8,
            static_cast<int16_t>(sequenceMaxCycles));
        code[words++] = R5900::sltu(R5900::R_T2, R5900::R_T1, R5900::R_T2);
        loopExitBranches[loopExitCount++] = words;
        code[words++] = R5900::bne(R5900::R_T2, R5900::R_ZERO, 0);
        code[words++] = R5900::nop();

        loopBackBranch = words;
        code[words++] = R5900::beq(R5900::R_ZERO, R5900::R_ZERO, 0);
        code[words++] = R5900::nop();
    }

    size_t epilogue = words;
    code[words++] = R5900::sw(R5900::R_T8, 84, R5900::R_A0);
    // Conditional/flag-reading blocks may keep CPSR in v1 without changing
    // it. Only write it back when this sequence can actually modify NZCV/T.
    if (writesRegisterCpsr)
        code[words++] = R5900::sw(R5900::R_V1, 64, R5900::R_A0);
    if (internalSelfLoop)
    {
        code[words++] = R5900::sw(R5900::R_S0, 100, R5900::R_A0);
        code[words++] = R5900::lw(R5900::R_S0, 12, R5900::R_SP);
        code[words++] = R5900::addiu(R5900::R_SP, R5900::R_SP, 16);
    }
    code[words++] = R5900::jr(R5900::R_RA);
    code[words++] = R5900::nop();

    for (size_t i = 0; i < loopExitCount; i++)
    {
        size_t branch = loopExitBranches[i];
        size_t distance = epilogue - (branch + 1);
        if (distance > 0x7FFFu)
            return false;

        uint32_t original = code[branch];
        uint32_t opcode = original >> 26;
        int rs = (original >> 21) & 31;
        int rt = (original >> 16) & 31;
        code[branch] = (opcode == 0x04)
            ? R5900::beq(rs, rt, static_cast<int16_t>(distance))
            : R5900::bne(rs, rt, static_cast<int16_t>(distance));
    }

    if (loopBackBranch != static_cast<size_t>(-1))
    {
        int32_t distance = static_cast<int32_t>(bodyStart) -
            static_cast<int32_t>(loopBackBranch + 1);
        if (distance < -32768 || distance > 32767)
            return false;
        code[loopBackBranch] = R5900::beq(R5900::R_ZERO, R5900::R_ZERO,
            static_cast<int16_t>(distance));
    }

    cache.commitWords(words);
    cache.syncExecutable(code, code + words);

    R5900CompiledBlock block = {};
    block.guestPc = guestPc;
    block.guestEndPc = guestPc + static_cast<uint32_t>(count * 4);
    block.mapGeneration = mappedGeneration;
    block.guestInstructions = static_cast<uint32_t>(count);
    block.minCycles = sequenceMinCycles;
    // Conservative per-iteration upper bound, shared with the generated
    // scheduler-budget loop guard.
    block.cycles = sequenceMaxCycles;
    block.entryOpcode0 = opcodes[0];
    block.entryOpcode1 = (count > 1) ? opcodes[1] : 0;
    block.thumb = false;
    block.internalSelfLoop = internalSelfLoop;
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

        Ps2ArmAlu::DecodedMultiply multiply;
        if (Ps2ArmAlu::decodeMultiply(opcodes[i], &multiply))
        {
            block.readMask |= static_cast<uint16_t>(
                (1u << multiply.rm) | (1u << multiply.rs));
            if (multiply.accumulate)
                block.readMask |= static_cast<uint16_t>(1u << multiply.rn);
            block.writeMask |= static_cast<uint16_t>(1u << multiply.rd);
            block.readsFlags |= multiply.condition != 0xEu;
            block.writesFlags |= multiply.setFlags;
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
                if (block.maxMemoryWrites != 0xFFu)
                    block.maxMemoryWrites++;
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
                if (block.maxMemoryWrites != 0xFFu)
                    block.maxMemoryWrites++;
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
                if (block.maxMemoryWrites != 0xFFu)
                    block.maxMemoryWrites++;
            }
            if (singleTransfer.writeBack)
                block.writeMask |= static_cast<uint16_t>(1u << singleTransfer.rn);
            block.readsFlags |= singleTransfer.condition != 0xEu;
            continue;
        }

        Ps2ArmAlu::DecodedMultiBlockTransfer multiBlockTransfer;
        if (Ps2ArmAlu::decodeMultiBlockTransfer(opcodes[i], &multiBlockTransfer))
        {
            block.readMask |= static_cast<uint16_t>(
                1u << multiBlockTransfer.rn);
            if (multiBlockTransfer.load)
                block.writeMask |= multiBlockTransfer.registerMask;
            else
            {
                block.readMask |= multiBlockTransfer.registerMask;
                block.writesMemory = true;
                uint32_t maxWrites = static_cast<uint32_t>(
                    block.maxMemoryWrites) + multiBlockTransfer.count;
                block.maxMemoryWrites = static_cast<uint8_t>(
                    std::min<uint32_t>(maxWrites, 0xFFu));
            }
            if (multiBlockTransfer.writeBack)
                block.writeMask |= static_cast<uint16_t>(
                    1u << multiBlockTransfer.rn);
            block.readsFlags |= multiBlockTransfer.condition != 0xEu;
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
                if (block.maxMemoryWrites != 0xFFu)
                    block.maxMemoryWrites++;
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
        if (branch.link)
            block.writeMask |= static_cast<uint16_t>(1u << 14);
        block.readsFlags |= branch.condition != 0xEu;
    }

    // Precompute compact register lists for the live executor. Derive the
    // true block-entry inputs with a backwards liveness pass: an unconditional
    // write kills the need for the older value, while a conditional write does
    // not. Starting from writeMask also models the commit boundary, where every
    // architectural destination must contain its final value.
    uint16_t writeMask = static_cast<uint16_t>(block.writeMask & 0x7FFFu);
    uint16_t inputMask = writeMask;
    for (size_t i = count; i-- > 0;)
    {
        Ps2ArmAlu::InstructionInfo info;
        Ps2ArmAlu::inspect(opcodes[i],
            guestPc + static_cast<uint32_t>(i * 4), &info);
        uint16_t reads = static_cast<uint16_t>(info.readMask & 0x7FFFu);
        uint16_t writes = static_cast<uint16_t>(info.writeMask & 0x7FFFu);

        if (info.condition == 0xEu)
            inputMask = static_cast<uint16_t>(
                reads | (inputMask & ~writes));
        else
            inputMask = static_cast<uint16_t>(reads | inputMask);
    }
    block.inputMask = inputMask;

    for (uint8_t reg = 0; reg < 15; reg++)
    {
        uint16_t bit = static_cast<uint16_t>(1u << reg);
        if (inputMask & bit)
            block.inputRegs[block.inputRegCount++] = reg;
        if (writeMask & bit)
            block.writeRegs[block.writeRegCount++] = reg;
    }

    blocks.push_back(block);
    rememberCompiledCodePages(blocks.back());

    // Prefer the newest compilation for active dispatch. A PC may have been
    // compiled earlier as a shorter shadow/gate sequence; publishing the new
    // vector index here prevents that stale partial block from winning the
    // direct lookup forever.
    if (blocks.size() <= 0xFFFFu)
    {
        size_t slot = activeLookupSlot(guestPc);
        activeBlockPc[slot] = guestPc;
        activeBlockIndex[slot] = static_cast<uint16_t>(blocks.size());
        if (activeRejectedPc[slot] == guestPc)
        {
            activeRejectedPc[slot] = 0;
            activeRejectedKind[slot] = ACTIVE_REJECT_NONE;
        }
    }

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

    // Deterministically gate the ARM register-shift timing too. This exact
    // form appears in real hot blocks (MOV Rd,Rm,LSL Rs) and must match the
    // interpreter's +1 cycle before such blocks can pass the differential gate.
    {
        const uint32_t opcode = 0xE1A02013u; // MOV r2,r3,LSL r0
        ArmAluState expected = {};
        expected.r[0] = 2u;
        expected.r[3] = 0x11u;
        if (!applyArmAlu(opcode, &expected) || expected.cycles != 2u)
            return false;

        uint32_t *entry = nullptr;
        uint8_t rd = 0;
        if (!emitArmAluBlock(opcode, &entry, &rd) || rd != 2)
            return false;
        ArmAluState actual = {};
        actual.r[0] = 2u;
        actual.r[3] = 0x11u;
        typedef void (*ArmShiftCycleFunction)(ArmAluState *);
        ArmShiftCycleFunction function =
            reinterpret_cast<ArmShiftCycleFunction>(entry);
        function(&actual);
        if (actual.r[2] != expected.r[2] || actual.cycles != expected.cycles)
            return false;
    }

    // A failed ARM condition is resolved before operand2/register-shift
    // decoding in Interpreter::runOpcode(), so the extra shift cycle must not
    // be charged on the not-taken path.
    {
        const uint32_t opcode = 0x01A02013u; // MOV.EQ r2,r3,LSL r0
        uint32_t *entry = nullptr;
        uint8_t rd = 0;
        if (!emitArmAluBlock(opcode, &entry, &rd) || rd != 2)
            return false;

        typedef void (*ArmConditionalShiftFunction)(ArmAluState *);
        ArmConditionalShiftFunction function =
            reinterpret_cast<ArmConditionalShiftFunction>(entry);

        ArmAluState notTaken = {};
        notTaken.r[0] = 2u;
        notTaken.r[2] = 0xCAFEBABEu;
        notTaken.r[3] = 0x11u;
        notTaken.cpsr = 0x00000013u; // Z clear => EQ fails
        function(&notTaken);
        if (notTaken.r[2] != 0xCAFEBABEu || notTaken.cycles != 1u)
            return false;

        ArmAluState taken = {};
        taken.r[0] = 2u;
        taken.r[2] = 0xCAFEBABEu;
        taken.r[3] = 0x11u;
        taken.cpsr = 0x40000013u; // Z set => EQ passes
        function(&taken);
        if (taken.r[2] != 0x44u || taken.cycles != 2u)
            return false;

        ArmAluState portableNotTaken = {};
        portableNotTaken.r[0] = 2u;
        portableNotTaken.r[2] = 0xCAFEBABEu;
        portableNotTaken.r[3] = 0x11u;
        portableNotTaken.cpsr = 0x00000013u;
        if (!applyArmAlu(opcode, &portableNotTaken) ||
            portableNotTaken.r[2] != 0xCAFEBABEu ||
            portableNotTaken.cycles != 1u)
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
        { 0xE1B01081u, 0x10000013u, 1, 0x80000001u }, // MOVS r1,r1,LSL #1 carry
        { 0xE1B01821u, 0x30000013u, 1, 0x00018000u }, // hot MOVS LSR #16
        { 0xE1B01041u, 0x10000013u, 1, 0x80000001u }, // MOVS r1,r1,ASR #32
        { 0xE1B01461u, 0x10000013u, 1, 0x80000001u }, // MOVS r1,r1,ROR #8
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

    // Gate register-shift carry boundaries directly in generated R5900 code.
    struct RegShiftFlagTest
    {
        uint32_t opcode;
        uint32_t amount;
        uint32_t value;
        uint32_t cpsr;
    };
    const RegShiftFlagTest regShiftFlagTests[] =
    {
        { 0xE1B02013u,  0u, 0x80000001u, 0x20000013u }, // LSL Rs: preserve C
        { 0xE1B02013u, 32u, 0x80000001u, 0x00000013u }, // LSL #32: C=bit0
        { 0xE1B02013u, 33u, 0x80000001u, 0x20000013u }, // LSL >32: C=0
        { 0xE1B02033u, 32u, 0x80000001u, 0x00000013u }, // LSR #32: C=bit31
        { 0xE1B02033u, 33u, 0x80000001u, 0x20000013u }, // LSR >32: C=0
        { 0xE1B02053u, 33u, 0x80000001u, 0x00000013u }, // ASR >=32: C=sign
        { 0xE1B02073u, 32u, 0x80000001u, 0x00000013u }, // ROR #32: C=bit31
        { 0xE1B02073u, 33u, 0x80000001u, 0x00000013u }  // ROR #33: C=bit0
    };
    for (size_t i = 0;
        i < sizeof(regShiftFlagTests) / sizeof(regShiftFlagTests[0]); i++)
    {
        ArmAluState source = {};
        source.r[0] = regShiftFlagTests[i].amount;
        source.r[3] = regShiftFlagTests[i].value;
        source.cpsr = regShiftFlagTests[i].cpsr;
        ArmAluState expected = source;
        if (!applyArmAlu(regShiftFlagTests[i].opcode, &expected))
            return false;

        uint32_t *entry = nullptr;
        uint8_t rd = 0;
        if (!emitArmAluBlock(regShiftFlagTests[i].opcode, &entry, &rd) ||
            rd != 2)
            return false;

        ArmAluState actual = source;
        typedef void (*ArmRegShiftFlagFunction)(ArmAluState *);
        ArmRegShiftFlagFunction function =
            reinterpret_cast<ArmRegShiftFlagFunction>(entry);
        function(&actual);
        if (std::memcmp(&actual, &expected, sizeof(actual)) != 0)
            return false;
    }

    // Gate short ARM MUL/MLA directly in generated R5900 code. Expected
    // state and timing come from the portable implementation above.
    {
        const uint32_t multiplyOpcodes[] =
        {
            0xE0040291u, // MUL r4,r1,r2
            0xE0253291u, // MLA r5,r1,r2,r3
            0xE0140291u, // MULS r4,r1,r2
            0x00040291u  // MUL.EQ r4,r1,r2 (failed condition below)
        };
        for (size_t i = 0;
            i < sizeof(multiplyOpcodes) / sizeof(multiplyOpcodes[0]); i++)
        {
            ArmAluState source = {};
            source.r[1] = (i == 2) ? 0u : 0xFFFFFFFEu;
            source.r[2] = 3u;
            source.r[3] = 10u;
            source.r[4] = 0x11223344u;
            source.cpsr = (i == 3) ? 0x00000013u : 0x30000013u;

            ArmAluState expected = source;
            if (!applyArmMultiply(multiplyOpcodes[i], &expected))
                return false;

            uint32_t *multiplyCode = cache.reserveWords(96);
            if (!multiplyCode)
                return false;
            size_t multiplyWords = emitArmMultiplyInstruction(
                multiplyOpcodes[i], multiplyCode, false, false, false);
            if (!multiplyWords)
                return false;
            multiplyCode[multiplyWords++] = R5900::jr(R5900::R_RA);
            multiplyCode[multiplyWords++] = R5900::nop();
            cache.commitWords(multiplyWords);
            cache.syncExecutable(multiplyCode, multiplyCode + multiplyWords);

            ArmAluState actual = source;
            typedef void (*ArmMultiplyFunction)(ArmAluState *);
            ArmMultiplyFunction multiplyFunction =
                reinterpret_cast<ArmMultiplyFunction>(multiplyCode);
            multiplyFunction(&actual);
            if (std::memcmp(&actual, &expected, sizeof(actual)) != 0)
                return false;
        }
    }

    // Gate ARM7 data-dependent MUL/MLA timing against known cycle counts,
    // not only against the portable helper. Include the exact negative
    // early-termination boundaries (-256, -65536 and -2^24).
    {
        struct Arm7MultiplyTimingCase
        {
            uint32_t opcode;
            uint32_t rsValue;
            uint32_t cpsr;
            uint32_t expectedCycles;
        };
        const Arm7MultiplyTimingCase cases[] =
        {
            { 0xE0040291u, 0x0000007Fu, 0x00000013u, 2u }, // MUL m=1
            { 0xE0040291u, 0xFFFFFF00u, 0x00000013u, 2u }, // MUL m=1 (-256)
            { 0xE0040291u, 0x00000100u, 0x00000013u, 3u }, // MUL m=2
            { 0xE0040291u, 0xFFFF0000u, 0x00000013u, 3u }, // MUL m=2 (-65536)
            { 0xE0040291u, 0x00010000u, 0x00000013u, 4u }, // MUL m=3
            { 0xE0040291u, 0xFF000000u, 0x00000013u, 4u }, // MUL m=3 (-2^24)
            { 0xE0040291u, 0x01000000u, 0x00000013u, 5u }, // MUL m=4
            { 0xE0253291u, 0x01000000u, 0x00000013u, 6u }, // MLA m=4
            { 0xE0140291u, 0x00000100u, 0x00000013u, 3u }, // MULS m=2
            { 0x00040291u, 0x01000000u, 0x00000013u, 1u }  // MUL.EQ fails
        };

        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        {
            ArmAluState source = {};
            source.r[1] = 7u;
            source.r[2] = cases[i].rsValue;
            source.r[3] = 11u;
            source.r[4] = 0x11223344u;
            source.r[5] = 0x55667788u;
            source.cpsr = cases[i].cpsr;
            source.cpuId = 1;

            ArmAluState expected = source;
            if (!applyArmMultiply(cases[i].opcode, &expected) ||
                expected.cycles != cases[i].expectedCycles)
                return false;

            uint32_t *multiplyCode = cache.reserveWords(160);
            if (!multiplyCode)
                return false;
            size_t multiplyWords = emitArmMultiplyInstruction(
                cases[i].opcode, multiplyCode, false, false, false, 1);
            if (!multiplyWords)
                return false;
            multiplyCode[multiplyWords++] = R5900::jr(R5900::R_RA);
            multiplyCode[multiplyWords++] = R5900::nop();
            cache.commitWords(multiplyWords);
            cache.syncExecutable(multiplyCode,
                multiplyCode + multiplyWords);

            ArmAluState actual = source;
            typedef void (*Arm7MultiplyTimingFunction)(ArmAluState *);
            Arm7MultiplyTimingFunction function =
                reinterpret_cast<Arm7MultiplyTimingFunction>(multiplyCode);
            function(&actual);

            if (actual.cycles != cases[i].expectedCycles ||
                actual.r[4] != expected.r[4] ||
                actual.r[5] != expected.r[5] ||
                actual.cpsr != expected.cpsr)
                return false;
        }
    }

    // Gate ARM long multiplies directly in generated R5900 code. Cover
    // unsigned/signed, accumulate, S flags, failed condition and ARM7's
    // data-dependent m timing independently of live game code.
    {
        struct LongMultiplyCase
        {
            uint32_t opcode;
            uint32_t rmValue;
            uint32_t rsValue;
            uint32_t loValue;
            uint32_t hiValue;
            uint32_t cpsr;
            uint8_t cpuId;
            uint32_t expectedCycles;
        };

        const LongMultiplyCase cases[] =
        {
            { 0xE0810392u, 0xFFFFFFFFu, 2u, 0u, 0u,
              0x30000013u, 0u, 3u }, // ARM9 UMULL
            { 0xE0C10392u, 0xFFFFFFFFu, 2u, 0u, 0u,
              0x30000013u, 0u, 3u }, // ARM9 SMULL
            { 0xE0A10392u, 0xFFFFFFFFu, 2u, 5u, 1u,
              0x30000013u, 0u, 3u }, // ARM9 UMLAL
            { 0xE0910392u, 0u, 7u, 0xFFFFFFFFu, 0xFFFFFFFFu,
              0x30000013u, 0u, 5u }, // ARM9 UMULLS
            { 0x00810392u, 3u, 4u, 0xA5A55A5Au, 0x11223344u,
              0x00000013u, 0u, 1u }, // ARM9 UMULL.EQ fails
            { 0xE0810392u, 7u, 0x0000007Fu, 0u, 0u,
              0x00000013u, 1u, 3u }, // ARM7 UMULL m=1
            { 0xE0810392u, 7u, 0x01000000u, 0u, 0u,
              0x00000013u, 1u, 6u }, // ARM7 UMULL m=4
            { 0xE0A10392u, 7u, 0x01000000u, 9u, 3u,
              0x00000013u, 1u, 7u }, // ARM7 UMLAL m=4
            { 0xE0D10392u, 0xFFFFFFFFu, 0x00000100u, 0u, 0u,
              0x30000013u, 1u, 4u }  // ARM7 SMULLS m=2
        };

        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        {
            ArmAluState source = {};
            source.r[2] = cases[i].rmValue;
            source.r[3] = cases[i].rsValue;
            source.r[0] = cases[i].loValue;
            source.r[1] = cases[i].hiValue;
            source.cpsr = cases[i].cpsr;
            source.cpuId = cases[i].cpuId;

            ArmAluState expected = source;
            if (!applyArmLongMultiply(cases[i].opcode, &expected) ||
                expected.cycles != cases[i].expectedCycles)
                return false;

            uint32_t *longCode = cache.reserveWords(224);
            if (!longCode)
                return false;
            size_t longWords = emitArmLongMultiplyInstruction(
                cases[i].opcode, longCode, false, false, cases[i].cpuId);
            if (!longWords)
                return false;
            longCode[longWords++] = R5900::jr(R5900::R_RA);
            longCode[longWords++] = R5900::nop();
            cache.commitWords(longWords);
            cache.syncExecutable(longCode, longCode + longWords);

            ArmAluState actual = source;
            typedef void (*ArmLongMultiplyFunction)(ArmAluState *);
            ArmLongMultiplyFunction function =
                reinterpret_cast<ArmLongMultiplyFunction>(longCode);
            function(&actual);

            if (actual.r[0] != expected.r[0] ||
                actual.r[1] != expected.r[1] ||
                actual.cpsr != expected.cpsr ||
                actual.cycles != cases[i].expectedCycles)
                return false;
        }
    }

    // Gate writeback-load -> ALU forwarding with dead intermediate-store
    // elimination. The LDR result is deliberately not committed to state.r1;
    // ADD must consume the still-live t2 value while LDR also updates r0.
    {
        const uint32_t forwardLoadAddress = 0x02000000u;
        uint8_t *forwardLoadPage = memory
            ? memory->getDirectReadPage(targetCpuId != 0, forwardLoadAddress)
            : nullptr;
        if (!forwardLoadPage)
            return false;
        uint32_t forwardOffset = forwardLoadAddress & 0xFFFu;
        uint32_t loaded =
            static_cast<uint32_t>(forwardLoadPage[forwardOffset]) |
            (static_cast<uint32_t>(forwardLoadPage[forwardOffset + 1]) << 8) |
            (static_cast<uint32_t>(forwardLoadPage[forwardOffset + 2]) << 16) |
            (static_cast<uint32_t>(forwardLoadPage[forwardOffset + 3]) << 24);

        const uint32_t opcodes[] =
        {
            0xE4901004u, // LDR r1,[r0],#4
            0xE2811001u  // ADD r1,r1,#1
        };
        uint32_t *entry = nullptr;
        uint32_t hostWords = 0;
        if (!emitArmAluSequence(opcodes, 2, 0x02001000u,
            &entry, &hostWords))
            return false;

        ArmAluState state = {};
        state.r[0] = forwardLoadAddress;
        state.r[1] = 0xDEADBEEFu;
        state.memory = memory;
        state.cpuId = targetCpuId;
        state.readCacheTag = (forwardLoadAddress >> 12) + 1u;
        state.readCachePage = forwardLoadPage;

        typedef void (*ArmLoadForwardFunction)(ArmAluState *);
        ArmLoadForwardFunction function =
            reinterpret_cast<ArmLoadForwardFunction>(entry);
        function(&state);

        uint32_t expectedCycles = targetCpuId ? 4u : 2u;
        if (state.memoryFault ||
            state.r[0] != forwardLoadAddress + 4u ||
            state.r[1] != loaded + 1u ||
            state.cycles != expectedCycles)
            return false;
    }

    // Gate MUL -> ALU host-register forwarding and dead intermediate store
    // elimination. ADD consumes the multiply result directly from t2.
    {
        const uint32_t forwardPc = 0x02002000u;
        const uint32_t forwardOpcodes[] =
        {
            0xE0040291u, // MUL r4,r1,r2
            0xE2844001u  // ADD r4,r4,#1
        };
        ArmAluState expected = {};
        expected.r[1] = 2u;
        expected.r[2] = 3u;
        expected.nextPc = forwardPc + 8u;
        if (!applyArmMultiply(forwardOpcodes[0], &expected) ||
            !applyArmAlu(forwardOpcodes[1], &expected))
            return false;

        size_t blockCountBefore = blocks.size();
        uint32_t *forwardEntry = nullptr;
        uint32_t forwardHostWords = 0;
        if (!emitArmAluSequence(forwardOpcodes, 2, forwardPc,
            &forwardEntry, &forwardHostWords) ||
            !forwardEntry || !forwardHostWords)
            return false;

        ArmAluState actual = {};
        actual.r[1] = 2u;
        actual.r[2] = 3u;
        actual.nextPc = forwardPc + 8u;
        typedef void (*ArmMultiplyForwardFunction)(ArmAluState *);
        ArmMultiplyForwardFunction forwardFunction =
            reinterpret_cast<ArmMultiplyForwardFunction>(forwardEntry);
        forwardFunction(&actual);

        bool forwardOk = actual.r[4] == expected.r[4] &&
            actual.cycles == expected.cycles &&
            actual.cpsr == expected.cpsr;

        if (blocks.size() > blockCountBefore)
            blocks.resize(blockCountBefore);
        clearActiveLookups();

        if (!forwardOk)
            return false;
    }

    // Gate the resident self-loop guest counter and scheduler budget.
    // MOV + B back to the block start costs 4 cycles per iteration, so a
    // 12-cycle budget must execute exactly three iterations (6 guest ops).
    {
        const uint32_t loopPc = 0x02001000u;
        const uint32_t loopOpcodes[] =
        {
            0xE1A00000u, // MOV r0,r0
            0xEAFFFFFDu  // B loopPc
        };
        size_t loopBlockCountBefore = blocks.size();
        uint32_t *loopEntry = nullptr;
        uint32_t loopHostWords = 0;
        if (!emitArmAluSequence(loopOpcodes, 2, loopPc,
            &loopEntry, &loopHostWords) || !loopEntry || !loopHostWords)
            return false;

        ArmAluState loopState = {};
        loopState.r[0] = 0x12345678u;
        loopState.nextPc = loopPc + 8u;
        loopState.memory = memory;
        loopState.cycleLimit = 12u;
        typedef void (*ArmSelfLoopFunction)(ArmAluState *);
        ArmSelfLoopFunction loopFunction =
            reinterpret_cast<ArmSelfLoopFunction>(loopEntry);
        loopFunction(&loopState);

        bool loopOk = loopState.r[0] == 0x12345678u &&
            loopState.nextPc == loopPc &&
            loopState.cycles == 12u &&
            loopState.guestExecuted == 6u;

        // emitArmAluSequence publishes normal runtime metadata. This sequence
        // is synthetic, so remove that metadata before the emulator can ever
        // dispatch a real guest PC through it. Generated test bytes may remain
        // in the code cache just like the other startup gates.
        if (blocks.size() > loopBlockCountBefore)
            blocks.resize(loopBlockCountBefore);
        clearActiveLookups();

        if (!loopOk)
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
        { 0xEA000006u, 0x0206094Cu, 0xF0000013u }, // B.AL taken
        { 0xEBFFFFFAu, 0x02066E90u, 0x00000013u }, // BL.AL taken + LR
        { 0x0BFFFFFAu, 0x02066E90u, 0x00000013u }  // BL.EQ not taken
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

    // Gate cache-hit register-offset STRB directly in generated R5900 code.
    const uint32_t regByteTarget = byteAddress + 4u;
    uint8_t regByteBefore = bytePage[regByteTarget & 0xFFFu];
    uint32_t *regByteCode = cache.reserveWords(160);
    if (!regByteCode)
        return false;
    size_t regByteWords = emitArmRegisterStoreFastInstruction(
        0xE7C01002u, regByteCode, false, false); // STRB r1,[r0,r2]
    if (!regByteWords)
        return false;
    regByteCode[regByteWords++] = R5900::jr(R5900::R_RA);
    regByteCode[regByteWords++] = R5900::nop();
    cache.commitWords(regByteWords);
    cache.syncExecutable(regByteCode, regByteCode + regByteWords);
    {
        ArmAluState regByteState = {};
        regByteState.r[0] = byteAddress;
        regByteState.r[1] = 0x5Au;
        regByteState.r[2] = 4u;
        regByteState.memory = memory;
        regByteState.writeCacheTag = (regByteTarget >> 12) + 1u;
        regByteState.writeCachePage = bytePage;
        ArmMemoryWriteGuard regByteWrites(&regByteState);
        typedef void (*ArmRegByteStoreFunction)(ArmAluState *);
        ArmRegByteStoreFunction regByteFunction =
            reinterpret_cast<ArmRegByteStoreFunction>(regByteCode);
        regByteFunction(&regByteState);
        if (regByteState.memoryFault || regByteState.writeCount != 1 ||
            regByteState.cycles != 1u ||
            bytePage[regByteTarget & 0xFFFu] != 0x5Au)
            return false;
    }
    if (bytePage[regByteTarget & 0xFFFu] != regByteBefore)
        return false;

    // Gate shifted register-offset STR and its transactional rollback.
    const uint32_t regWordBase = 0x02000020u;
    const uint32_t regWordTarget = regWordBase + 4u;
    uint8_t *regWordPage = memory->getDirectWritePage(0, regWordTarget);
    if (!regWordPage)
        return false;
    uint32_t regWordOffset = regWordTarget & 0xFFFu;
    uint32_t regWordBefore =
        static_cast<uint32_t>(regWordPage[regWordOffset]) |
        (static_cast<uint32_t>(regWordPage[regWordOffset + 1]) << 8) |
        (static_cast<uint32_t>(regWordPage[regWordOffset + 2]) << 16) |
        (static_cast<uint32_t>(regWordPage[regWordOffset + 3]) << 24);
    uint32_t *regWordCode = cache.reserveWords(160);
    if (!regWordCode)
        return false;
    size_t regWordWords = emitArmRegisterStoreFastInstruction(
        0xE7801102u, regWordCode, false, false); // STR r1,[r0,r2,LSL #2]
    if (!regWordWords)
        return false;
    regWordCode[regWordWords++] = R5900::jr(R5900::R_RA);
    regWordCode[regWordWords++] = R5900::nop();
    cache.commitWords(regWordWords);
    cache.syncExecutable(regWordCode, regWordCode + regWordWords);
    {
        ArmAluState regWordState = {};
        regWordState.r[0] = regWordBase;
        regWordState.r[1] = 0x89ABCDEFu;
        regWordState.r[2] = 1u;
        regWordState.memory = memory;
        regWordState.writeCacheTag = (regWordTarget >> 12) + 1u;
        regWordState.writeCachePage = regWordPage;
        ArmMemoryWriteGuard regWordWrites(&regWordState);
        typedef void (*ArmRegWordStoreFunction)(ArmAluState *);
        ArmRegWordStoreFunction regWordFunction =
            reinterpret_cast<ArmRegWordStoreFunction>(regWordCode);
        regWordFunction(&regWordState);
        uint32_t stored =
            static_cast<uint32_t>(regWordPage[regWordOffset]) |
            (static_cast<uint32_t>(regWordPage[regWordOffset + 1]) << 8) |
            (static_cast<uint32_t>(regWordPage[regWordOffset + 2]) << 16) |
            (static_cast<uint32_t>(regWordPage[regWordOffset + 3]) << 24);
        if (regWordState.memoryFault || regWordState.writeCount != 1 ||
            regWordState.cycles != 1u || stored != 0x89ABCDEFu)
            return false;
    }
    uint32_t regWordRestored =
        static_cast<uint32_t>(regWordPage[regWordOffset]) |
        (static_cast<uint32_t>(regWordPage[regWordOffset + 1]) << 8) |
        (static_cast<uint32_t>(regWordPage[regWordOffset + 2]) << 16) |
        (static_cast<uint32_t>(regWordPage[regWordOffset + 3]) << 24);
    if (regWordRestored != regWordBefore)
        return false;

    // Gate post-index register STRB: store uses the old base, then Rn updates.
    const uint32_t postByteBase = byteAddress + 8u;
    uint8_t postByteBefore = bytePage[postByteBase & 0xFFFu];
    uint32_t *postByteCode = cache.reserveWords(160);
    if (!postByteCode)
        return false;
    size_t postByteWords = emitArmRegisterStoreFastInstruction(
        0xE6C01002u, postByteCode, false, false); // STRB r1,[r0],r2
    if (!postByteWords)
        return false;
    postByteCode[postByteWords++] = R5900::jr(R5900::R_RA);
    postByteCode[postByteWords++] = R5900::nop();
    cache.commitWords(postByteWords);
    cache.syncExecutable(postByteCode, postByteCode + postByteWords);
    {
        ArmAluState postByteState = {};
        postByteState.r[0] = postByteBase;
        postByteState.r[1] = 0xC3u;
        postByteState.r[2] = 4u;
        postByteState.memory = memory;
        postByteState.writeCacheTag = (postByteBase >> 12) + 1u;
        postByteState.writeCachePage = bytePage;
        ArmMemoryWriteGuard postByteWrites(&postByteState);
        typedef void (*ArmPostByteStoreFunction)(ArmAluState *);
        ArmPostByteStoreFunction postByteFunction =
            reinterpret_cast<ArmPostByteStoreFunction>(postByteCode);
        postByteFunction(&postByteState);
        if (postByteState.memoryFault || postByteState.writeCount != 1 ||
            postByteState.cycles != 1u ||
            postByteState.r[0] != postByteBase + 4u ||
            bytePage[postByteBase & 0xFFFu] != 0xC3u)
            return false;
    }
    if (bytePage[postByteBase & 0xFFFu] != postByteBefore)
        return false;

    // Gate post-index ROR addressing specifically: the address emitter uses
    // t2/t3 internally, so the original base must survive in a separate host
    // register until the store address/writeback split is complete.
    const uint32_t postRorBase = byteAddress + 12u;
    uint8_t postRorBefore = bytePage[postRorBase & 0xFFFu];
    uint32_t *postRorCode = cache.reserveWords(160);
    if (!postRorCode)
        return false;
    size_t postRorWords = emitArmRegisterStoreFastInstruction(
        0xE6C01462u, postRorCode, false, false); // STRB r1,[r0],r2,ROR #8
    if (!postRorWords)
        return false;
    postRorCode[postRorWords++] = R5900::jr(R5900::R_RA);
    postRorCode[postRorWords++] = R5900::nop();
    cache.commitWords(postRorWords);
    cache.syncExecutable(postRorCode, postRorCode + postRorWords);
    {
        ArmAluState postRorState = {};
        postRorState.r[0] = postRorBase;
        postRorState.r[1] = 0xD7u;
        postRorState.r[2] = 0x00000400u; // ROR #8 -> 4
        postRorState.memory = memory;
        postRorState.writeCacheTag = (postRorBase >> 12) + 1u;
        postRorState.writeCachePage = bytePage;
        ArmMemoryWriteGuard postRorWrites(&postRorState);
        typedef void (*ArmPostRorStoreFunction)(ArmAluState *);
        ArmPostRorStoreFunction postRorFunction =
            reinterpret_cast<ArmPostRorStoreFunction>(postRorCode);
        postRorFunction(&postRorState);
        if (postRorState.memoryFault || postRorState.writeCount != 1 ||
            postRorState.cycles != 1u ||
            postRorState.r[0] != postRorBase + 4u ||
            bytePage[postRorBase & 0xFFFu] != 0xD7u)
            return false;
    }
    if (bytePage[postRorBase & 0xFFFu] != postRorBefore)
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

    // Gate the direct ARM7 literal path with native load timing.
    uint8_t *arm7LiteralPage =
        memory->getDirectReadPage(1, literalPc + 8u);
    if (!arm7LiteralPage)
        return false;
    uint32_t arm7LiteralExpected =
        static_cast<uint32_t>(arm7LiteralPage[literalOffset]) |
        (static_cast<uint32_t>(arm7LiteralPage[literalOffset + 1]) << 8) |
        (static_cast<uint32_t>(arm7LiteralPage[literalOffset + 2]) << 16) |
        (static_cast<uint32_t>(arm7LiteralPage[literalOffset + 3]) << 24);
    uint32_t *arm7LiteralCode = cache.reserveWords(64);
    if (!arm7LiteralCode)
        return false;
    size_t arm7LiteralWords = emitArmLiteralLoadDirectInstruction(
        0x059F1000u, literalPc, arm7LiteralCode, memory,
        false, false, false, 1); // LDR.EQ r1,[pc]
    if (!arm7LiteralWords)
        return false;
    arm7LiteralCode[arm7LiteralWords++] = R5900::jr(R5900::R_RA);
    arm7LiteralCode[arm7LiteralWords++] = R5900::nop();
    cache.commitWords(arm7LiteralWords);
    cache.syncExecutable(arm7LiteralCode,
        arm7LiteralCode + arm7LiteralWords);
    ArmLiteralFunction arm7LiteralFunction =
        reinterpret_cast<ArmLiteralFunction>(arm7LiteralCode);
    {
        ArmAluState notTaken = {};
        notTaken.r[1] = 0xCAFEBABEu;
        notTaken.cpsr = 0x00000013u;
        notTaken.memory = memory;
        notTaken.cpuId = 1;
        arm7LiteralFunction(&notTaken);
        if (notTaken.memoryFault ||
            notTaken.r[1] != 0xCAFEBABEu ||
            notTaken.cycles != 1u)
            return false;
    }
    {
        ArmAluState taken = {};
        taken.r[1] = 0xCAFEBABEu;
        taken.cpsr = 0x40000013u;
        taken.memory = memory;
        taken.cpuId = 1;
        arm7LiteralFunction(&taken);
        if (taken.memoryFault ||
            taken.r[1] != arm7LiteralExpected ||
            taken.cycles != 3u)
            return false;
    }

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

    // Gate the same safe one-register IA subset with ARM7-native timing:
    // LDMIA=3 cycles, STMIA=2 cycles, including +4 writeback.
    const uint32_t arm7BlockLoadAddress = 0x02000000u;
    const uint32_t arm7BlockStoreAddress = 0x02000020u;
    uint8_t *arm7BlockLoadPage =
        memory->getDirectReadPage(1, arm7BlockLoadAddress);
    uint8_t *arm7BlockStorePage =
        memory->getDirectWritePage(1, arm7BlockStoreAddress);
    if (!arm7BlockLoadPage || !arm7BlockStorePage)
        return false;

    uint32_t arm7BlockExpected =
        static_cast<uint32_t>(arm7BlockLoadPage[0]) |
        (static_cast<uint32_t>(arm7BlockLoadPage[1]) << 8) |
        (static_cast<uint32_t>(arm7BlockLoadPage[2]) << 16) |
        (static_cast<uint32_t>(arm7BlockLoadPage[3]) << 24);

    uint32_t *arm7BlockLoadCode = cache.reserveWords(160);
    if (!arm7BlockLoadCode)
        return false;
    size_t arm7BlockLoadWords = emitArmSingleBlockTransferFastInstruction(
        0xE8B00002u, arm7BlockLoadCode, false, false,
        -1, 0xEu, false, 1); // LDMIA r0!,{r1}
    if (!arm7BlockLoadWords)
        return false;
    arm7BlockLoadCode[arm7BlockLoadWords++] = R5900::jr(R5900::R_RA);
    arm7BlockLoadCode[arm7BlockLoadWords++] = R5900::nop();
    cache.commitWords(arm7BlockLoadWords);
    cache.syncExecutable(arm7BlockLoadCode,
        arm7BlockLoadCode + arm7BlockLoadWords);

    ArmBlockTransferFunction arm7BlockLoadFunction =
        reinterpret_cast<ArmBlockTransferFunction>(arm7BlockLoadCode);
    {
        ArmAluState state = {};
        state.r[0] = arm7BlockLoadAddress;
        state.r[1] = 0xFFFFFFFFu;
        state.memory = memory;
        state.cpuId = 1;
        state.readCacheTag = (arm7BlockLoadAddress >> 12) + 1u;
        state.readCachePage = arm7BlockLoadPage;
        arm7BlockLoadFunction(&state);
        if (state.memoryFault || state.cycles != 3u ||
            state.r[0] != arm7BlockLoadAddress + 4u ||
            state.r[1] != arm7BlockExpected)
            return false;
    }

    uint32_t arm7BlockStoreOffset = arm7BlockStoreAddress & 0xFFFu;
    uint32_t arm7BlockStoreBefore =
        static_cast<uint32_t>(arm7BlockStorePage[arm7BlockStoreOffset]) |
        (static_cast<uint32_t>(
            arm7BlockStorePage[arm7BlockStoreOffset + 1]) << 8) |
        (static_cast<uint32_t>(
            arm7BlockStorePage[arm7BlockStoreOffset + 2]) << 16) |
        (static_cast<uint32_t>(
            arm7BlockStorePage[arm7BlockStoreOffset + 3]) << 24);

    uint32_t *arm7BlockStoreCode = cache.reserveWords(192);
    if (!arm7BlockStoreCode)
        return false;
    size_t arm7BlockStoreWords = emitArmSingleBlockTransferFastInstruction(
        0xE8A00002u, arm7BlockStoreCode, false, false,
        -1, 0xEu, false, 1); // STMIA r0!,{r1}
    if (!arm7BlockStoreWords)
        return false;
    arm7BlockStoreCode[arm7BlockStoreWords++] = R5900::jr(R5900::R_RA);
    arm7BlockStoreCode[arm7BlockStoreWords++] = R5900::nop();
    cache.commitWords(arm7BlockStoreWords);
    cache.syncExecutable(arm7BlockStoreCode,
        arm7BlockStoreCode + arm7BlockStoreWords);

    ArmBlockTransferFunction arm7BlockStoreFunction =
        reinterpret_cast<ArmBlockTransferFunction>(arm7BlockStoreCode);
    {
        ArmAluState state = {};
        state.r[0] = arm7BlockStoreAddress;
        state.r[1] = 0x2468ACE0u;
        state.memory = memory;
        state.cpuId = 1;
        state.writeCacheTag = (arm7BlockStoreAddress >> 12) + 1u;
        state.writeCachePage = arm7BlockStorePage;
        ArmMemoryWriteGuard writes(&state);
        arm7BlockStoreFunction(&state);

        uint32_t stored =
            static_cast<uint32_t>(
                arm7BlockStorePage[arm7BlockStoreOffset]) |
            (static_cast<uint32_t>(
                arm7BlockStorePage[arm7BlockStoreOffset + 1]) << 8) |
            (static_cast<uint32_t>(
                arm7BlockStorePage[arm7BlockStoreOffset + 2]) << 16) |
            (static_cast<uint32_t>(
                arm7BlockStorePage[arm7BlockStoreOffset + 3]) << 24);
        if (state.memoryFault || state.writeCount != 1 ||
            state.cycles != 2u ||
            state.r[0] != arm7BlockStoreAddress + 4u ||
            stored != 0x2468ACE0u)
            return false;
    }

    uint32_t arm7BlockStoreRestored =
        static_cast<uint32_t>(arm7BlockStorePage[arm7BlockStoreOffset]) |
        (static_cast<uint32_t>(
            arm7BlockStorePage[arm7BlockStoreOffset + 1]) << 8) |
        (static_cast<uint32_t>(
            arm7BlockStorePage[arm7BlockStoreOffset + 2]) << 16) |
        (static_cast<uint32_t>(
            arm7BlockStorePage[arm7BlockStoreOffset + 3]) << 24);
    if (arm7BlockStoreRestored != arm7BlockStoreBefore)
        return false;

    // Gate the new 2-4 register inline block-transfer path independently of
    // the portable helper. LDMIA {r1,r2} checks ascending register order and
    // IA writeback; STMDB {r1-r4} checks DB address order, native ARM9/ARM7
    // timing and transactional rollback.
    const uint32_t multiLoadAddress = 0x02000040u;
    uint8_t *multiLoadPage = memory->getDirectReadPage(0, multiLoadAddress);
    uint8_t *arm7MultiLoadPage =
        memory->getDirectReadPage(1, multiLoadAddress);
    if (!multiLoadPage || !arm7MultiLoadPage)
        return false;

    auto readWordAt = [](uint8_t *page, uint32_t address)
    {
        uint32_t offset = address & 0xFFFu;
        return static_cast<uint32_t>(page[offset]) |
            (static_cast<uint32_t>(page[offset + 1]) << 8) |
            (static_cast<uint32_t>(page[offset + 2]) << 16) |
            (static_cast<uint32_t>(page[offset + 3]) << 24);
    };

    uint32_t expectedLoad0 = readWordAt(multiLoadPage, multiLoadAddress);
    uint32_t expectedLoad1 = readWordAt(multiLoadPage, multiLoadAddress + 4u);
    uint32_t arm7ExpectedLoad0 =
        readWordAt(arm7MultiLoadPage, multiLoadAddress);
    uint32_t arm7ExpectedLoad1 =
        readWordAt(arm7MultiLoadPage, multiLoadAddress + 4u);

    uint32_t *multiLoadCode = cache.reserveWords(192);
    if (!multiLoadCode)
        return false;
    size_t multiLoadWords = emitArmMultiBlockTransferInstruction(
        0xE8B00006u, multiLoadCode, false, false, 0);
        // LDMIA r0!,{r1,r2}
    if (!multiLoadWords)
        return false;
    multiLoadCode[multiLoadWords++] = R5900::jr(R5900::R_RA);
    multiLoadCode[multiLoadWords++] = R5900::nop();
    cache.commitWords(multiLoadWords);
    cache.syncExecutable(multiLoadCode, multiLoadCode + multiLoadWords);
    ArmBlockTransferFunction multiLoadFunction =
        reinterpret_cast<ArmBlockTransferFunction>(multiLoadCode);

    {
        ArmAluState state = {};
        state.r[0] = multiLoadAddress;
        state.memory = memory;
        state.readCacheTag = (multiLoadAddress >> 12) + 1u;
        state.readCachePage = multiLoadPage;
        multiLoadFunction(&state);
        if (state.memoryFault || state.cycles != 2u ||
            state.r[0] != multiLoadAddress + 8u ||
            state.r[1] != expectedLoad0 || state.r[2] != expectedLoad1)
            return false;
    }

    uint32_t *arm7MultiLoadCode = cache.reserveWords(192);
    if (!arm7MultiLoadCode)
        return false;
    size_t arm7MultiLoadWords = emitArmMultiBlockTransferInstruction(
        0xE8B00006u, arm7MultiLoadCode, false, false, 1);
    if (!arm7MultiLoadWords)
        return false;
    arm7MultiLoadCode[arm7MultiLoadWords++] = R5900::jr(R5900::R_RA);
    arm7MultiLoadCode[arm7MultiLoadWords++] = R5900::nop();
    cache.commitWords(arm7MultiLoadWords);
    cache.syncExecutable(arm7MultiLoadCode,
        arm7MultiLoadCode + arm7MultiLoadWords);
    ArmBlockTransferFunction arm7MultiLoadFunction =
        reinterpret_cast<ArmBlockTransferFunction>(arm7MultiLoadCode);

    {
        ArmAluState state = {};
        state.r[0] = multiLoadAddress;
        state.memory = memory;
        state.cpuId = 1;
        state.readCacheTag = (multiLoadAddress >> 12) + 1u;
        state.readCachePage = arm7MultiLoadPage;
        arm7MultiLoadFunction(&state);
        if (state.memoryFault || state.cycles != 4u ||
            state.r[0] != multiLoadAddress + 8u ||
            state.r[1] != arm7ExpectedLoad0 ||
            state.r[2] != arm7ExpectedLoad1)
            return false;
    }

    const uint32_t multiStoreStart = 0x02000080u;
    const uint32_t multiStoreBase = multiStoreStart + 16u;
    uint8_t *multiStorePage =
        memory->getDirectWritePage(0, multiStoreStart);
    uint8_t *arm7MultiStorePage =
        memory->getDirectWritePage(1, multiStoreStart);
    if (!multiStorePage || !arm7MultiStorePage)
        return false;

    uint32_t multiStoreBefore[4] = {};
    uint32_t arm7MultiStoreBefore[4] = {};
    for (uint32_t i = 0; i < 4; i++)
    {
        multiStoreBefore[i] =
            readWordAt(multiStorePage, multiStoreStart + i * 4u);
        arm7MultiStoreBefore[i] =
            readWordAt(arm7MultiStorePage, multiStoreStart + i * 4u);
    }

    uint32_t *multiStoreCode = cache.reserveWords(320);
    if (!multiStoreCode)
        return false;
    size_t multiStoreWords = emitArmMultiBlockTransferInstruction(
        0xE920001Eu, multiStoreCode, false, false, 0);
        // STMDB r0!,{r1-r4}
    if (!multiStoreWords)
        return false;
    multiStoreCode[multiStoreWords++] = R5900::jr(R5900::R_RA);
    multiStoreCode[multiStoreWords++] = R5900::nop();
    cache.commitWords(multiStoreWords);
    cache.syncExecutable(multiStoreCode, multiStoreCode + multiStoreWords);
    ArmBlockTransferFunction multiStoreFunction =
        reinterpret_cast<ArmBlockTransferFunction>(multiStoreCode);

    {
        ArmAluState state = {};
        state.r[0] = multiStoreBase;
        state.r[1] = 0x11111111u;
        state.r[2] = 0x22222222u;
        state.r[3] = 0x33333333u;
        state.r[4] = 0x44444444u;
        state.memory = memory;
        state.writeCacheTag = (multiStoreStart >> 12) + 1u;
        state.writeCachePage = multiStorePage;
        ArmMemoryWriteGuard writes(&state);
        multiStoreFunction(&state);
        if (state.memoryFault || state.writeCount != 4 ||
            state.cycles != 4u || state.r[0] != multiStoreStart)
            return false;
        for (uint32_t i = 0; i < 4; i++)
        {
            if (readWordAt(multiStorePage, multiStoreStart + i * 4u) !=
                0x11111111u * (i + 1u))
                return false;
        }
    }
    for (uint32_t i = 0; i < 4; i++)
    {
        if (readWordAt(multiStorePage, multiStoreStart + i * 4u) !=
            multiStoreBefore[i])
            return false;
    }

    uint32_t *arm7MultiStoreCode = cache.reserveWords(320);
    if (!arm7MultiStoreCode)
        return false;
    size_t arm7MultiStoreWords = emitArmMultiBlockTransferInstruction(
        0xE920001Eu, arm7MultiStoreCode, false, false, 1);
    if (!arm7MultiStoreWords)
        return false;
    arm7MultiStoreCode[arm7MultiStoreWords++] = R5900::jr(R5900::R_RA);
    arm7MultiStoreCode[arm7MultiStoreWords++] = R5900::nop();
    cache.commitWords(arm7MultiStoreWords);
    cache.syncExecutable(arm7MultiStoreCode,
        arm7MultiStoreCode + arm7MultiStoreWords);
    ArmBlockTransferFunction arm7MultiStoreFunction =
        reinterpret_cast<ArmBlockTransferFunction>(arm7MultiStoreCode);

    {
        ArmAluState state = {};
        state.r[0] = multiStoreBase;
        state.r[1] = 0x11111111u;
        state.r[2] = 0x22222222u;
        state.r[3] = 0x33333333u;
        state.r[4] = 0x44444444u;
        state.memory = memory;
        state.cpuId = 1;
        state.writeCacheTag = (multiStoreStart >> 12) + 1u;
        state.writeCachePage = arm7MultiStorePage;
        ArmMemoryWriteGuard writes(&state);
        arm7MultiStoreFunction(&state);
        if (state.memoryFault || state.writeCount != 4 ||
            state.cycles != 5u || state.r[0] != multiStoreStart)
            return false;
        for (uint32_t i = 0; i < 4; i++)
        {
            if (readWordAt(arm7MultiStorePage,
                multiStoreStart + i * 4u) != 0x11111111u * (i + 1u))
                return false;
        }
    }
    for (uint32_t i = 0; i < 4; i++)
    {
        if (readWordAt(arm7MultiStorePage, multiStoreStart + i * 4u) !=
            arm7MultiStoreBefore[i])
            return false;
    }

    // Gate the real hot prologue observed in PROFILE:
    // STMDB sp!,{r4-r10,lr} (E92D47F0). This proves the widened 2-14 register
    // subset, journal capacity, DB ordering and native ARM9/ARM7 timing.
    const uint32_t largeStoreBase = 0x02000100u;
    const uint32_t largeStoreStart = largeStoreBase - 32u;
    uint8_t *largeStorePage =
        memory->getDirectWritePage(0, largeStoreStart);
    uint8_t *arm7LargeStorePage =
        memory->getDirectWritePage(1, largeStoreStart);
    if (!largeStorePage || !arm7LargeStorePage)
        return false;

    uint32_t largeBefore[8] = {};
    uint32_t arm7LargeBefore[8] = {};
    for (uint32_t i = 0; i < 8; i++)
    {
        largeBefore[i] =
            readWordAt(largeStorePage, largeStoreStart + i * 4u);
        arm7LargeBefore[i] =
            readWordAt(arm7LargeStorePage, largeStoreStart + i * 4u);
    }

    auto seedLargeStoreState = [&](ArmAluState &state, bool arm7)
    {
        state.r[13] = largeStoreBase;
        for (uint32_t reg = 4; reg <= 10; reg++)
            state.r[reg] = 0xA0000000u | reg;
        state.r[14] = 0xEEEEEEEEu;
        state.memory = memory;
        state.cpuId = arm7 ? 1u : 0u;
        state.writeCacheTag = (largeStoreStart >> 12) + 1u;
        state.writeCachePage = arm7 ? arm7LargeStorePage : largeStorePage;
    };

    uint32_t *largeStoreCode = cache.reserveWords(512);
    if (!largeStoreCode)
        return false;
    size_t largeStoreWords = emitArmMultiBlockTransferInstruction(
        0xE92D47F0u, largeStoreCode, false, false, 0);
    if (!largeStoreWords)
        return false;
    largeStoreCode[largeStoreWords++] = R5900::jr(R5900::R_RA);
    largeStoreCode[largeStoreWords++] = R5900::nop();
    cache.commitWords(largeStoreWords);
    cache.syncExecutable(largeStoreCode,
        largeStoreCode + largeStoreWords);
    ArmBlockTransferFunction largeStoreFunction =
        reinterpret_cast<ArmBlockTransferFunction>(largeStoreCode);

    {
        ArmAluState state = {};
        seedLargeStoreState(state, false);
        ArmMemoryWriteGuard writes(&state);
        largeStoreFunction(&state);
        if (state.memoryFault || state.writeCount != 8 ||
            state.cycles != 8u || state.r[13] != largeStoreStart)
            return false;
        for (uint32_t i = 0; i < 7; i++)
        {
            if (readWordAt(largeStorePage,
                largeStoreStart + i * 4u) != (0xA0000000u | (i + 4u)))
                return false;
        }
        if (readWordAt(largeStorePage, largeStoreStart + 28u) !=
            0xEEEEEEEEu)
            return false;
    }
    for (uint32_t i = 0; i < 8; i++)
    {
        if (readWordAt(largeStorePage, largeStoreStart + i * 4u) !=
            largeBefore[i])
            return false;
    }

    uint32_t *arm7LargeStoreCode = cache.reserveWords(512);
    if (!arm7LargeStoreCode)
        return false;
    size_t arm7LargeStoreWords = emitArmMultiBlockTransferInstruction(
        0xE92D47F0u, arm7LargeStoreCode, false, false, 1);
    if (!arm7LargeStoreWords)
        return false;
    arm7LargeStoreCode[arm7LargeStoreWords++] = R5900::jr(R5900::R_RA);
    arm7LargeStoreCode[arm7LargeStoreWords++] = R5900::nop();
    cache.commitWords(arm7LargeStoreWords);
    cache.syncExecutable(arm7LargeStoreCode,
        arm7LargeStoreCode + arm7LargeStoreWords);
    ArmBlockTransferFunction arm7LargeStoreFunction =
        reinterpret_cast<ArmBlockTransferFunction>(arm7LargeStoreCode);

    {
        ArmAluState state = {};
        seedLargeStoreState(state, true);
        ArmMemoryWriteGuard writes(&state);
        arm7LargeStoreFunction(&state);
        if (state.memoryFault || state.writeCount != 8 ||
            state.cycles != 9u || state.r[13] != largeStoreStart)
            return false;
        for (uint32_t i = 0; i < 7; i++)
        {
            if (readWordAt(arm7LargeStorePage,
                largeStoreStart + i * 4u) != (0xA0000000u | (i + 4u)))
                return false;
        }
        if (readWordAt(arm7LargeStorePage, largeStoreStart + 28u) !=
            0xEEEEEEEEu)
            return false;
    }
    for (uint32_t i = 0; i < 8; i++)
    {
        if (readWordAt(arm7LargeStorePage, largeStoreStart + i * 4u) !=
            arm7LargeBefore[i])
            return false;
    }

    // Gate the real hot copy-loop pair seen in PROFILE logs:
    //   LDMIA.LT r0!,{r2}
    //   STMIA.LT r1!,{r2}
    // The second instruction reuses both the first instruction's LT result in
    // t7 and its loaded value in t2. Test taken and not-taken paths, including
    // transactional rollback of the destination word.
    const uint32_t conditionalLoadAddress = 0x02000000u;
    const uint32_t conditionalStoreAddress = 0x02000020u;
    uint8_t *conditionalPage =
        memory->getDirectWritePage(0, conditionalStoreAddress);
    if (!conditionalPage ||
        memory->getDirectReadPage(0, conditionalLoadAddress) != conditionalPage)
        return false;

    uint32_t conditionalLoadOffset = conditionalLoadAddress & 0xFFFu;
    uint32_t conditionalStoreOffset = conditionalStoreAddress & 0xFFFu;
    uint32_t conditionalExpected =
        static_cast<uint32_t>(conditionalPage[conditionalLoadOffset]) |
        (static_cast<uint32_t>(conditionalPage[conditionalLoadOffset + 1]) << 8) |
        (static_cast<uint32_t>(conditionalPage[conditionalLoadOffset + 2]) << 16) |
        (static_cast<uint32_t>(conditionalPage[conditionalLoadOffset + 3]) << 24);
    uint32_t conditionalBefore =
        static_cast<uint32_t>(conditionalPage[conditionalStoreOffset]) |
        (static_cast<uint32_t>(conditionalPage[conditionalStoreOffset + 1]) << 8) |
        (static_cast<uint32_t>(conditionalPage[conditionalStoreOffset + 2]) << 16) |
        (static_cast<uint32_t>(conditionalPage[conditionalStoreOffset + 3]) << 24);

    uint32_t *conditionalBlockCode = cache.reserveWords(320);
    if (!conditionalBlockCode)
        return false;
    size_t conditionalBlockWords = emitArmSingleBlockTransferFastInstruction(
        0xB8B00004u, conditionalBlockCode, false, false);
    if (!conditionalBlockWords)
        return false;
    size_t conditionalStoreWords = emitArmSingleBlockTransferFastInstruction(
        0xB8A10004u, conditionalBlockCode + conditionalBlockWords,
        false, false, 2, 0xBu, true);
    if (!conditionalStoreWords)
        return false;
    conditionalBlockWords += conditionalStoreWords;
    conditionalBlockCode[conditionalBlockWords++] = R5900::jr(R5900::R_RA);
    conditionalBlockCode[conditionalBlockWords++] = R5900::nop();
    cache.commitWords(conditionalBlockWords);
    cache.syncExecutable(conditionalBlockCode,
        conditionalBlockCode + conditionalBlockWords);

    typedef void (*ArmConditionalBlockFunction)(ArmAluState *);
    ArmConditionalBlockFunction conditionalBlockFunction =
        reinterpret_cast<ArmConditionalBlockFunction>(conditionalBlockCode);

    {
        ArmAluState conditionalState = {};
        conditionalState.r[0] = conditionalLoadAddress;
        conditionalState.r[1] = conditionalStoreAddress;
        conditionalState.r[2] = 0xDEADBEEFu;
        conditionalState.cpsr = 0x80000013u; // LT: N != V
        conditionalState.memory = memory;
        conditionalState.readCacheTag = (conditionalLoadAddress >> 12) + 1u;
        conditionalState.readCachePage = conditionalPage;
        conditionalState.writeCacheTag = (conditionalStoreAddress >> 12) + 1u;
        conditionalState.writeCachePage = conditionalPage;

        ArmMemoryWriteGuard conditionalWrites(&conditionalState);
        conditionalBlockFunction(&conditionalState);

        uint32_t stored =
            static_cast<uint32_t>(conditionalPage[conditionalStoreOffset]) |
            (static_cast<uint32_t>(conditionalPage[conditionalStoreOffset + 1]) << 8) |
            (static_cast<uint32_t>(conditionalPage[conditionalStoreOffset + 2]) << 16) |
            (static_cast<uint32_t>(conditionalPage[conditionalStoreOffset + 3]) << 24);
        if (conditionalState.memoryFault || conditionalState.writeCount != 1 ||
            conditionalState.cycles != 4u ||
            conditionalState.r[0] != conditionalLoadAddress + 4u ||
            conditionalState.r[1] != conditionalStoreAddress + 4u ||
            conditionalState.r[2] != conditionalExpected ||
            stored != conditionalExpected)
            return false;
    }

    uint32_t conditionalRestored =
        static_cast<uint32_t>(conditionalPage[conditionalStoreOffset]) |
        (static_cast<uint32_t>(conditionalPage[conditionalStoreOffset + 1]) << 8) |
        (static_cast<uint32_t>(conditionalPage[conditionalStoreOffset + 2]) << 16) |
        (static_cast<uint32_t>(conditionalPage[conditionalStoreOffset + 3]) << 24);
    if (conditionalRestored != conditionalBefore)
        return false;

    {
        ArmAluState conditionalState = {};
        conditionalState.r[0] = conditionalLoadAddress;
        conditionalState.r[1] = conditionalStoreAddress;
        conditionalState.r[2] = 0xCAFEBABEu;
        conditionalState.cpsr = 0x00000013u; // GE: LT condition fails
        conditionalState.memory = memory;
        conditionalState.readCacheTag = (conditionalLoadAddress >> 12) + 1u;
        conditionalState.readCachePage = conditionalPage;
        conditionalState.writeCacheTag = (conditionalStoreAddress >> 12) + 1u;
        conditionalState.writeCachePage = conditionalPage;

        conditionalBlockFunction(&conditionalState);
        if (conditionalState.memoryFault || conditionalState.writeCount != 0 ||
            conditionalState.cycles != 2u ||
            conditionalState.r[0] != conditionalLoadAddress ||
            conditionalState.r[1] != conditionalStoreAddress ||
            conditionalState.r[2] != 0xCAFEBABEu)
            return false;
    }

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

    // Gate ARM7-native timing on the exact cached LDR emitter used by the
    // guarded backend. Conditional failure costs one cycle; a taken aligned
    // direct word load costs three.
    uint8_t *arm7LoadPage = memory->getDirectReadPage(1, loadAddress);
    if (!arm7LoadPage)
        return false;
    uint32_t arm7ExpectedWord = static_cast<uint32_t>(arm7LoadPage[0]) |
        (static_cast<uint32_t>(arm7LoadPage[1]) << 8) |
        (static_cast<uint32_t>(arm7LoadPage[2]) << 16) |
        (static_cast<uint32_t>(arm7LoadPage[3]) << 24);

    uint32_t *arm7WordLoadCode = cache.reserveWords(96);
    if (!arm7WordLoadCode)
        return false;
    size_t arm7WordLoadWords = emitArmWordLoadCachedInstruction(
        0x05901000u, arm7WordLoadCode, false, false, false, 1);
        // LDR.EQ r1,[r0]
    if (!arm7WordLoadWords)
        return false;
    arm7WordLoadCode[arm7WordLoadWords++] = R5900::jr(R5900::R_RA);
    arm7WordLoadCode[arm7WordLoadWords++] = R5900::nop();
    cache.commitWords(arm7WordLoadWords);
    cache.syncExecutable(arm7WordLoadCode,
        arm7WordLoadCode + arm7WordLoadWords);

    ArmLoadFunction arm7WordLoadFunction =
        reinterpret_cast<ArmLoadFunction>(arm7WordLoadCode);
    {
        ArmAluState arm7NotTaken = {};
        arm7NotTaken.r[0] = loadAddress;
        arm7NotTaken.r[1] = 0xCAFEBABEu;
        arm7NotTaken.cpsr = 0x00000013u; // Z clear
        arm7NotTaken.memory = memory;
        arm7NotTaken.cpuId = 1;
        arm7NotTaken.readCacheTag = (loadAddress >> 12) + 1u;
        arm7NotTaken.readCachePage = arm7LoadPage;
        arm7WordLoadFunction(&arm7NotTaken);
        if (arm7NotTaken.memoryFault ||
            arm7NotTaken.r[1] != 0xCAFEBABEu ||
            arm7NotTaken.cycles != 1u)
            return false;
    }
    {
        ArmAluState arm7Taken = {};
        arm7Taken.r[0] = loadAddress;
        arm7Taken.r[1] = 0xCAFEBABEu;
        arm7Taken.cpsr = 0x40000013u; // Z set
        arm7Taken.memory = memory;
        arm7Taken.cpuId = 1;
        arm7Taken.readCacheTag = (loadAddress >> 12) + 1u;
        arm7Taken.readCachePage = arm7LoadPage;
        arm7WordLoadFunction(&arm7Taken);
        if (arm7Taken.memoryFault ||
            arm7Taken.r[1] != arm7ExpectedWord ||
            arm7Taken.cycles != 3u)
            return false;
    }

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

    uint8_t *arm7StorePage = memory->getDirectWritePage(1, loadAddress);
    if (!arm7StorePage)
        return false;
    uint32_t arm7StoreBefore = static_cast<uint32_t>(arm7StorePage[0]) |
        (static_cast<uint32_t>(arm7StorePage[1]) << 8) |
        (static_cast<uint32_t>(arm7StorePage[2]) << 16) |
        (static_cast<uint32_t>(arm7StorePage[3]) << 24);

    uint32_t *arm7StoreCode = cache.reserveWords(128);
    if (!arm7StoreCode)
        return false;
    size_t arm7StoreWords = emitArmImmediateStoreFastInstruction(
        0x05801000u, arm7StoreCode, false, false, 1);
        // STR.EQ r1,[r0]
    if (!arm7StoreWords)
        return false;
    arm7StoreCode[arm7StoreWords++] = R5900::jr(R5900::R_RA);
    arm7StoreCode[arm7StoreWords++] = R5900::nop();
    cache.commitWords(arm7StoreWords);
    cache.syncExecutable(arm7StoreCode, arm7StoreCode + arm7StoreWords);

    ArmLoadFunction arm7StoreFunction =
        reinterpret_cast<ArmLoadFunction>(arm7StoreCode);
    {
        ArmAluState notTaken = {};
        notTaken.r[0] = loadAddress;
        notTaken.r[1] = 0x13579BDFu;
        notTaken.cpsr = 0x00000013u; // Z clear
        notTaken.memory = memory;
        notTaken.cpuId = 1;
        notTaken.writeCacheTag = (loadAddress >> 12) + 1u;
        notTaken.writeCachePage = arm7StorePage;
        arm7StoreFunction(&notTaken);
        if (notTaken.memoryFault || notTaken.writeCount != 0 ||
            notTaken.cycles != 1u)
            return false;
    }
    {
        ArmAluState taken = {};
        taken.r[0] = loadAddress;
        taken.r[1] = 0x13579BDFu;
        taken.cpsr = 0x40000013u; // Z set
        taken.memory = memory;
        taken.cpuId = 1;
        taken.writeCacheTag = (loadAddress >> 12) + 1u;
        taken.writeCachePage = arm7StorePage;
        ArmMemoryWriteGuard writes(&taken);
        arm7StoreFunction(&taken);
        if (taken.memoryFault || taken.writeCount != 1 ||
            taken.cycles != 2u ||
            arm7StorePage[0] != 0xDFu || arm7StorePage[1] != 0x9Bu ||
            arm7StorePage[2] != 0x57u || arm7StorePage[3] != 0x13u)
            return false;
    }
    uint32_t arm7StoreRestored =
        static_cast<uint32_t>(arm7StorePage[0]) |
        (static_cast<uint32_t>(arm7StorePage[1]) << 8) |
        (static_cast<uint32_t>(arm7StorePage[2]) << 16) |
        (static_cast<uint32_t>(arm7StorePage[3]) << 24);
    if (arm7StoreRestored != arm7StoreBefore)
        return false;

    // Gate the ARM7 byte-transfer fast paths too. Byte accesses have no
    // alignment ambiguity: taken LDRB=3 cycles, taken STRB=2, failed
    // conditions=1, and stores must roll back exactly.
    const uint32_t arm7ByteAddress = loadAddress + 7u;
    uint8_t *arm7ByteReadPage =
        memory->getDirectReadPage(1, arm7ByteAddress);
    uint8_t *arm7ByteWritePage =
        memory->getDirectWritePage(1, arm7ByteAddress);
    if (!arm7ByteReadPage || !arm7ByteWritePage)
        return false;
    uint32_t arm7ByteOffset = arm7ByteAddress & 0xFFFu;
    uint8_t arm7ExpectedByte = arm7ByteReadPage[arm7ByteOffset];
    uint8_t arm7BeforeByte = arm7ByteWritePage[arm7ByteOffset];

    uint32_t *arm7ByteLoadCode = cache.reserveWords(96);
    if (!arm7ByteLoadCode)
        return false;
    size_t arm7ByteLoadWords = emitArmByteLoadCachedInstruction(
        0x05D01000u, arm7ByteLoadCode, false, false, false, 1);
        // LDRB.EQ r1,[r0]
    if (!arm7ByteLoadWords)
        return false;
    arm7ByteLoadCode[arm7ByteLoadWords++] = R5900::jr(R5900::R_RA);
    arm7ByteLoadCode[arm7ByteLoadWords++] = R5900::nop();
    cache.commitWords(arm7ByteLoadWords);
    cache.syncExecutable(arm7ByteLoadCode,
        arm7ByteLoadCode + arm7ByteLoadWords);

    ArmLoadFunction arm7ByteLoadFunction =
        reinterpret_cast<ArmLoadFunction>(arm7ByteLoadCode);
    {
        ArmAluState notTaken = {};
        notTaken.r[0] = arm7ByteAddress;
        notTaken.r[1] = 0xCAFEBABEu;
        notTaken.cpsr = 0x00000013u;
        notTaken.memory = memory;
        notTaken.cpuId = 1;
        notTaken.readCacheTag = (arm7ByteAddress >> 12) + 1u;
        notTaken.readCachePage = arm7ByteReadPage;
        arm7ByteLoadFunction(&notTaken);
        if (notTaken.memoryFault ||
            notTaken.r[1] != 0xCAFEBABEu ||
            notTaken.cycles != 1u)
            return false;
    }
    {
        ArmAluState taken = {};
        taken.r[0] = arm7ByteAddress;
        taken.r[1] = 0xCAFEBABEu;
        taken.cpsr = 0x40000013u;
        taken.memory = memory;
        taken.cpuId = 1;
        taken.readCacheTag = (arm7ByteAddress >> 12) + 1u;
        taken.readCachePage = arm7ByteReadPage;
        arm7ByteLoadFunction(&taken);
        if (taken.memoryFault ||
            taken.r[1] != arm7ExpectedByte ||
            taken.cycles != 3u)
            return false;
    }

    uint32_t *arm7ByteStoreCode = cache.reserveWords(128);
    if (!arm7ByteStoreCode)
        return false;
    size_t arm7ByteStoreWords = emitArmImmediateStoreFastInstruction(
        0x05C01000u, arm7ByteStoreCode, false, false, 1);
        // STRB.EQ r1,[r0]
    if (!arm7ByteStoreWords)
        return false;
    arm7ByteStoreCode[arm7ByteStoreWords++] = R5900::jr(R5900::R_RA);
    arm7ByteStoreCode[arm7ByteStoreWords++] = R5900::nop();
    cache.commitWords(arm7ByteStoreWords);
    cache.syncExecutable(arm7ByteStoreCode,
        arm7ByteStoreCode + arm7ByteStoreWords);

    ArmLoadFunction arm7ByteStoreFunction =
        reinterpret_cast<ArmLoadFunction>(arm7ByteStoreCode);
    {
        ArmAluState notTaken = {};
        notTaken.r[0] = arm7ByteAddress;
        notTaken.r[1] = 0xA5u;
        notTaken.cpsr = 0x00000013u;
        notTaken.memory = memory;
        notTaken.cpuId = 1;
        notTaken.writeCacheTag = (arm7ByteAddress >> 12) + 1u;
        notTaken.writeCachePage = arm7ByteWritePage;
        arm7ByteStoreFunction(&notTaken);
        if (notTaken.memoryFault || notTaken.writeCount != 0 ||
            notTaken.cycles != 1u)
            return false;
    }
    {
        ArmAluState taken = {};
        taken.r[0] = arm7ByteAddress;
        taken.r[1] = 0xA5u;
        taken.cpsr = 0x40000013u;
        taken.memory = memory;
        taken.cpuId = 1;
        taken.writeCacheTag = (arm7ByteAddress >> 12) + 1u;
        taken.writeCachePage = arm7ByteWritePage;
        ArmMemoryWriteGuard writes(&taken);
        arm7ByteStoreFunction(&taken);
        if (taken.memoryFault || taken.writeCount != 1 ||
            taken.cycles != 2u ||
            arm7ByteWritePage[arm7ByteOffset] != 0xA5u)
            return false;
    }
    if (arm7ByteWritePage[arm7ByteOffset] != arm7BeforeByte)
        return false;

    const uint32_t arm7HalfAddress = (loadAddress + 10u) & ~1u;
    uint8_t *arm7HalfReadPage =
        memory->getDirectReadPage(1, arm7HalfAddress);
    uint8_t *arm7HalfWritePage =
        memory->getDirectWritePage(1, arm7HalfAddress);
    if (!arm7HalfReadPage || !arm7HalfWritePage)
        return false;
    uint32_t arm7HalfOffset = arm7HalfAddress & 0xFFFu;
    if (arm7HalfOffset > 0xFFEu)
        return false;
    uint16_t arm7ExpectedHalf =
        static_cast<uint16_t>(arm7HalfReadPage[arm7HalfOffset]) |
        (static_cast<uint16_t>(arm7HalfReadPage[arm7HalfOffset + 1]) << 8);
    uint16_t arm7BeforeHalf =
        static_cast<uint16_t>(arm7HalfWritePage[arm7HalfOffset]) |
        (static_cast<uint16_t>(arm7HalfWritePage[arm7HalfOffset + 1]) << 8);

    uint32_t *arm7HalfLoadCode = cache.reserveWords(128);
    if (!arm7HalfLoadCode)
        return false;
    size_t arm7HalfLoadWords = emitArmHalfwordLoadCachedInstruction(
        0x01D010B0u, arm7HalfLoadCode, false, false, false, 1);
        // LDRH.EQ r1,[r0]
    if (!arm7HalfLoadWords)
        return false;
    arm7HalfLoadCode[arm7HalfLoadWords++] = R5900::jr(R5900::R_RA);
    arm7HalfLoadCode[arm7HalfLoadWords++] = R5900::nop();
    cache.commitWords(arm7HalfLoadWords);
    cache.syncExecutable(arm7HalfLoadCode,
        arm7HalfLoadCode + arm7HalfLoadWords);

    ArmLoadFunction arm7HalfLoadFunction =
        reinterpret_cast<ArmLoadFunction>(arm7HalfLoadCode);
    {
        ArmAluState notTaken = {};
        notTaken.r[0] = arm7HalfAddress;
        notTaken.r[1] = 0xCAFEBABEu;
        notTaken.cpsr = 0x00000013u;
        notTaken.memory = memory;
        notTaken.cpuId = 1;
        notTaken.readCacheTag = (arm7HalfAddress >> 12) + 1u;
        notTaken.readCachePage = arm7HalfReadPage;
        arm7HalfLoadFunction(&notTaken);
        if (notTaken.memoryFault ||
            notTaken.r[1] != 0xCAFEBABEu ||
            notTaken.cycles != 1u)
            return false;
    }
    {
        ArmAluState taken = {};
        taken.r[0] = arm7HalfAddress;
        taken.r[1] = 0xCAFEBABEu;
        taken.cpsr = 0x40000013u;
        taken.memory = memory;
        taken.cpuId = 1;
        taken.readCacheTag = (arm7HalfAddress >> 12) + 1u;
        taken.readCachePage = arm7HalfReadPage;
        arm7HalfLoadFunction(&taken);
        if (taken.memoryFault ||
            taken.r[1] != arm7ExpectedHalf ||
            taken.cycles != 3u)
            return false;
    }

    uint32_t *arm7HalfStoreCode = cache.reserveWords(160);
    if (!arm7HalfStoreCode)
        return false;
    size_t arm7HalfStoreWords = emitArmImmediateStoreFastInstruction(
        0x01C010B0u, arm7HalfStoreCode, false, false, 1);
        // STRH.EQ r1,[r0]
    if (!arm7HalfStoreWords)
        return false;
    arm7HalfStoreCode[arm7HalfStoreWords++] = R5900::jr(R5900::R_RA);
    arm7HalfStoreCode[arm7HalfStoreWords++] = R5900::nop();
    cache.commitWords(arm7HalfStoreWords);
    cache.syncExecutable(arm7HalfStoreCode,
        arm7HalfStoreCode + arm7HalfStoreWords);

    ArmLoadFunction arm7HalfStoreFunction =
        reinterpret_cast<ArmLoadFunction>(arm7HalfStoreCode);
    {
        ArmAluState notTaken = {};
        notTaken.r[0] = arm7HalfAddress;
        notTaken.r[1] = 0xA55Au;
        notTaken.cpsr = 0x00000013u;
        notTaken.memory = memory;
        notTaken.cpuId = 1;
        notTaken.writeCacheTag = (arm7HalfAddress >> 12) + 1u;
        notTaken.writeCachePage = arm7HalfWritePage;
        arm7HalfStoreFunction(&notTaken);
        if (notTaken.memoryFault || notTaken.writeCount != 0 ||
            notTaken.cycles != 1u)
            return false;
    }
    {
        ArmAluState taken = {};
        taken.r[0] = arm7HalfAddress;
        taken.r[1] = 0xA55Au;
        taken.cpsr = 0x40000013u;
        taken.memory = memory;
        taken.cpuId = 1;
        taken.writeCacheTag = (arm7HalfAddress >> 12) + 1u;
        taken.writeCachePage = arm7HalfWritePage;
        ArmMemoryWriteGuard writes(&taken);
        arm7HalfStoreFunction(&taken);
        uint16_t stored =
            static_cast<uint16_t>(arm7HalfWritePage[arm7HalfOffset]) |
            (static_cast<uint16_t>(
                arm7HalfWritePage[arm7HalfOffset + 1]) << 8);
        if (taken.memoryFault || taken.writeCount != 1 ||
            taken.cycles != 2u || stored != 0xA55Au)
            return false;
    }
    uint16_t arm7HalfRestored =
        static_cast<uint16_t>(arm7HalfWritePage[arm7HalfOffset]) |
        (static_cast<uint16_t>(
            arm7HalfWritePage[arm7HalfOffset + 1]) << 8);
    if (arm7HalfRestored != arm7BeforeHalf)
        return false;

    // Gate signed extra loads with known negative values. The hot aligned path
    // must use EE LB/LH directly; ARM7 odd LDRSH deliberately faults out of the
    // JIT so the Interpreter keeps its special misaligned semantics.
    {
        const uint32_t signedAddress = (loadAddress + 0x150u) & ~1u;
        ArmAluState seed = {};
        seed.memory = memory;
        seed.cpuId = 1;
        ArmMemoryWriteGuard restoreSignedBytes(&seed);
        if (!writeDirectArm9Byte(&seed, signedAddress + 0u, 0x80u) ||
            !writeDirectArm9Byte(&seed, signedAddress + 1u, 0xFFu) ||
            !writeDirectArm9Byte(&seed, signedAddress + 2u, 0x80u))
            return false;

        uint8_t *page = memory->getDirectReadPage(1, signedAddress);
        if (!page)
            return false;

        uint32_t *signedByteCode = cache.reserveWords(160);
        if (!signedByteCode)
            return false;
        size_t signedByteWords = emitArmHalfwordLoadCachedInstruction(
            0x01D010D0u, signedByteCode, false, false, false, 1);
            // LDRSB.EQ r1,[r0]
        if (!signedByteWords)
            return false;
        signedByteCode[signedByteWords++] = R5900::jr(R5900::R_RA);
        signedByteCode[signedByteWords++] = R5900::nop();
        cache.commitWords(signedByteWords);
        cache.syncExecutable(signedByteCode,
            signedByteCode + signedByteWords);
        ArmLoadFunction signedByteFunction =
            reinterpret_cast<ArmLoadFunction>(signedByteCode);

        {
            ArmAluState notTaken = {};
            notTaken.r[0] = signedAddress;
            notTaken.r[1] = 0xCAFEBABEu;
            notTaken.cpsr = 0x00000013u; // Z clear
            notTaken.memory = memory;
            notTaken.cpuId = 1;
            notTaken.readCacheTag = (signedAddress >> 12) + 1u;
            notTaken.readCachePage = page;
            signedByteFunction(&notTaken);
            if (notTaken.memoryFault ||
                notTaken.r[1] != 0xCAFEBABEu ||
                notTaken.cycles != 1u)
                return false;
        }
        {
            ArmAluState taken = {};
            taken.r[0] = signedAddress;
            taken.r[1] = 0;
            taken.cpsr = 0x40000013u; // Z set
            taken.memory = memory;
            taken.cpuId = 1;
            taken.readCacheTag = (signedAddress >> 12) + 1u;
            taken.readCachePage = page;
            signedByteFunction(&taken);
            if (taken.memoryFault ||
                taken.r[1] != 0xFFFFFF80u ||
                taken.cycles != 3u)
                return false;
        }

        uint32_t *signedHalfCode = cache.reserveWords(176);
        if (!signedHalfCode)
            return false;
        size_t signedHalfWords = emitArmHalfwordLoadCachedInstruction(
            0xE1D010F0u, signedHalfCode, false, false, false, 1);
            // LDRSH r1,[r0]
        if (!signedHalfWords)
            return false;
        signedHalfCode[signedHalfWords++] = R5900::jr(R5900::R_RA);
        signedHalfCode[signedHalfWords++] = R5900::nop();
        cache.commitWords(signedHalfWords);
        cache.syncExecutable(signedHalfCode,
            signedHalfCode + signedHalfWords);
        ArmLoadFunction signedHalfFunction =
            reinterpret_cast<ArmLoadFunction>(signedHalfCode);

        {
            ArmAluState taken = {};
            taken.r[0] = signedAddress;
            taken.memory = memory;
            taken.cpuId = 1;
            taken.readCacheTag = (signedAddress >> 12) + 1u;
            taken.readCachePage = page;
            signedHalfFunction(&taken);
            if (taken.memoryFault ||
                taken.r[1] != 0xFFFFFF80u ||
                taken.cycles != 3u)
                return false;
        }
        {
            ArmAluState odd = {};
            odd.r[0] = signedAddress + 1u;
            odd.memory = memory;
            odd.cpuId = 1;
            odd.readCacheTag = ((signedAddress + 1u) >> 12) + 1u;
            odd.readCachePage = page;
            signedHalfFunction(&odd);
            if (!odd.memoryFault)
                return false;

            ArmAluState portable = {};
            portable.r[0] = signedAddress + 1u;
            portable.memory = memory;
            portable.cpuId = 1;
            if (!applyArmHalfwordTransfer(&portable, 0xE1D010F0u) ||
                portable.r[1] != 0xFFFFFF80u ||
                portable.cycles != 3u)
                return false;
        }
    }

    // Gate ARM7 register-offset loads. The decoder already excludes PC/RRX;
    // these cases verify shifted word addressing plus byte/halfword Rm forms
    // and their native 3-cycle load timing.
    {
        const uint32_t base = loadAddress;
        const uint32_t target = base + 4u;
        uint8_t *page = memory->getDirectReadPage(1, target);
        if (!page)
            return false;
        uint32_t offset = target & 0xFFFu;
        uint32_t expected =
            static_cast<uint32_t>(page[offset]) |
            (static_cast<uint32_t>(page[offset + 1]) << 8) |
            (static_cast<uint32_t>(page[offset + 2]) << 16) |
            (static_cast<uint32_t>(page[offset + 3]) << 24);

        uint32_t *code = cache.reserveWords(128);
        if (!code)
            return false;
        size_t words = emitArmWordLoadCachedInstruction(
            0xE7901102u, code, false, false, false, 1);
            // LDR r1,[r0,r2,LSL #2]
        if (!words)
            return false;
        code[words++] = R5900::jr(R5900::R_RA);
        code[words++] = R5900::nop();
        cache.commitWords(words);
        cache.syncExecutable(code, code + words);

        ArmAluState state = {};
        state.r[0] = base;
        state.r[1] = 0xCAFEBABEu;
        state.r[2] = 1u;
        state.memory = memory;
        state.cpuId = 1;
        state.readCacheTag = (target >> 12) + 1u;
        state.readCachePage = page;
        ArmLoadFunction function =
            reinterpret_cast<ArmLoadFunction>(code);
        function(&state);
        if (state.memoryFault || state.cycles != 3u ||
            state.r[1] != expected)
            return false;
    }

    {
        const uint32_t base = loadAddress;
        const uint32_t target = base + 7u;
        uint8_t *page = memory->getDirectReadPage(1, target);
        if (!page)
            return false;
        uint8_t expected = page[target & 0xFFFu];

        uint32_t *code = cache.reserveWords(128);
        if (!code)
            return false;
        size_t words = emitArmByteLoadCachedInstruction(
            0xE7D01002u, code, false, false, false, 1);
            // LDRB r1,[r0,r2]
        if (!words)
            return false;
        code[words++] = R5900::jr(R5900::R_RA);
        code[words++] = R5900::nop();
        cache.commitWords(words);
        cache.syncExecutable(code, code + words);

        ArmAluState state = {};
        state.r[0] = base;
        state.r[1] = 0xCAFEBABEu;
        state.r[2] = 7u;
        state.memory = memory;
        state.cpuId = 1;
        state.readCacheTag = (target >> 12) + 1u;
        state.readCachePage = page;
        ArmLoadFunction function =
            reinterpret_cast<ArmLoadFunction>(code);
        function(&state);
        if (state.memoryFault || state.cycles != 3u ||
            state.r[1] != expected)
            return false;
    }

    {
        const uint32_t base = loadAddress;
        const uint32_t target = base + 2u;
        uint8_t *page = memory->getDirectReadPage(1, target);
        if (!page)
            return false;
        uint32_t offset = target & 0xFFFu;
        uint16_t expected =
            static_cast<uint16_t>(page[offset]) |
            (static_cast<uint16_t>(page[offset + 1]) << 8);

        uint32_t *code = cache.reserveWords(128);
        if (!code)
            return false;
        size_t words = emitArmHalfwordLoadCachedInstruction(
            0xE19010B2u, code, false, false, false, 1);
            // LDRH r1,[r0,r2]
        if (!words)
            return false;
        code[words++] = R5900::jr(R5900::R_RA);
        code[words++] = R5900::nop();
        cache.commitWords(words);
        cache.syncExecutable(code, code + words);

        ArmAluState state = {};
        state.r[0] = base;
        state.r[1] = 0xCAFEBABEu;
        state.r[2] = 2u;
        state.memory = memory;
        state.cpuId = 1;
        state.readCacheTag = (target >> 12) + 1u;
        state.readCachePage = page;
        ArmLoadFunction function =
            reinterpret_cast<ArmLoadFunction>(code);
        function(&state);
        if (state.memoryFault || state.cycles != 3u ||
            state.r[1] != expected)
            return false;
    }

    // Gate ARM7 register-offset stores with native timing and exact
    // transactional rollback. The word case also proves failed-condition
    // timing stays at one cycle.
    {
        const uint32_t base = loadAddress + 0x20u;
        const uint32_t target = base + 4u;
        uint8_t *page = memory->getDirectWritePage(1, target);
        if (!page)
            return false;
        uint32_t offset = target & 0xFFFu;
        uint32_t before =
            static_cast<uint32_t>(page[offset]) |
            (static_cast<uint32_t>(page[offset + 1]) << 8) |
            (static_cast<uint32_t>(page[offset + 2]) << 16) |
            (static_cast<uint32_t>(page[offset + 3]) << 24);

        uint32_t *code = cache.reserveWords(192);
        if (!code)
            return false;
        size_t words = emitArmRegisterStoreFastInstruction(
            0x07801102u, code, false, false, 1);
            // STR.EQ r1,[r0,r2,LSL #2]
        if (!words)
            return false;
        code[words++] = R5900::jr(R5900::R_RA);
        code[words++] = R5900::nop();
        cache.commitWords(words);
        cache.syncExecutable(code, code + words);
        typedef void (*Arm7RegStoreFunction)(ArmAluState *);
        Arm7RegStoreFunction function =
            reinterpret_cast<Arm7RegStoreFunction>(code);

        {
            ArmAluState state = {};
            state.r[0] = base;
            state.r[1] = 0x89ABCDEFu;
            state.r[2] = 1u;
            state.cpsr = 0x00000013u; // Z clear => EQ fails
            state.memory = memory;
            state.cpuId = 1;
            state.writeCacheTag = (target >> 12) + 1u;
            state.writeCachePage = page;
            function(&state);
            if (state.memoryFault || state.writeCount != 0 ||
                state.cycles != 1u)
                return false;
        }
        uint32_t untouched =
            static_cast<uint32_t>(page[offset]) |
            (static_cast<uint32_t>(page[offset + 1]) << 8) |
            (static_cast<uint32_t>(page[offset + 2]) << 16) |
            (static_cast<uint32_t>(page[offset + 3]) << 24);
        if (untouched != before)
            return false;

        {
            ArmAluState state = {};
            state.r[0] = base;
            state.r[1] = 0x89ABCDEFu;
            state.r[2] = 1u;
            state.cpsr = 0x40000013u; // Z set => EQ executes
            state.memory = memory;
            state.cpuId = 1;
            state.writeCacheTag = (target >> 12) + 1u;
            state.writeCachePage = page;
            ArmMemoryWriteGuard writes(&state);
            function(&state);
            uint32_t stored =
                static_cast<uint32_t>(page[offset]) |
                (static_cast<uint32_t>(page[offset + 1]) << 8) |
                (static_cast<uint32_t>(page[offset + 2]) << 16) |
                (static_cast<uint32_t>(page[offset + 3]) << 24);
            if (state.memoryFault || state.writeCount != 1 ||
                state.cycles != 2u || stored != 0x89ABCDEFu)
                return false;
        }
        uint32_t restored =
            static_cast<uint32_t>(page[offset]) |
            (static_cast<uint32_t>(page[offset + 1]) << 8) |
            (static_cast<uint32_t>(page[offset + 2]) << 16) |
            (static_cast<uint32_t>(page[offset + 3]) << 24);
        if (restored != before)
            return false;
    }

    {
        const uint32_t base = loadAddress + 0x30u;
        const uint32_t target = base + 4u;
        uint8_t *page = memory->getDirectWritePage(1, target);
        if (!page)
            return false;
        uint32_t offset = target & 0xFFFu;
        uint8_t before = page[offset];

        uint32_t *code = cache.reserveWords(192);
        if (!code)
            return false;
        size_t words = emitArmRegisterStoreFastInstruction(
            0xE7C01002u, code, false, false, 1);
            // STRB r1,[r0,r2]
        if (!words)
            return false;
        code[words++] = R5900::jr(R5900::R_RA);
        code[words++] = R5900::nop();
        cache.commitWords(words);
        cache.syncExecutable(code, code + words);
        typedef void (*Arm7RegByteStoreFunction)(ArmAluState *);
        Arm7RegByteStoreFunction function =
            reinterpret_cast<Arm7RegByteStoreFunction>(code);

        {
            ArmAluState state = {};
            state.r[0] = base;
            state.r[1] = 0x5Au;
            state.r[2] = 4u;
            state.memory = memory;
            state.cpuId = 1;
            state.writeCacheTag = (target >> 12) + 1u;
            state.writeCachePage = page;
            ArmMemoryWriteGuard writes(&state);
            function(&state);
            if (state.memoryFault || state.writeCount != 1 ||
                state.cycles != 2u || page[offset] != 0x5Au)
                return false;
        }
        if (page[offset] != before)
            return false;
    }

    {
        const uint32_t base = (loadAddress + 0x40u) & ~1u;
        const uint32_t target = base + 2u;
        uint8_t *page = memory->getDirectWritePage(1, target);
        if (!page)
            return false;
        uint32_t offset = target & 0xFFFu;
        if (offset > 0xFFEu)
            return false;
        uint16_t before =
            static_cast<uint16_t>(page[offset]) |
            (static_cast<uint16_t>(page[offset + 1]) << 8);

        uint32_t *code = cache.reserveWords(192);
        if (!code)
            return false;
        size_t words = emitArmHalfwordRegisterStoreFastInstruction(
            0xE18010B2u, code, false, false, 1);
            // STRH r1,[r0,r2]
        if (!words)
            return false;
        code[words++] = R5900::jr(R5900::R_RA);
        code[words++] = R5900::nop();
        cache.commitWords(words);
        cache.syncExecutable(code, code + words);
        typedef void (*Arm7RegHalfStoreFunction)(ArmAluState *);
        Arm7RegHalfStoreFunction function =
            reinterpret_cast<Arm7RegHalfStoreFunction>(code);

        {
            ArmAluState state = {};
            state.r[0] = base;
            state.r[1] = 0xA55Au;
            state.r[2] = 2u;
            state.memory = memory;
            state.cpuId = 1;
            state.writeCacheTag = (target >> 12) + 1u;
            state.writeCachePage = page;
            ArmMemoryWriteGuard writes(&state);
            function(&state);
            uint16_t stored =
                static_cast<uint16_t>(page[offset]) |
                (static_cast<uint16_t>(page[offset + 1]) << 8);
            if (state.memoryFault || state.writeCount != 1 ||
                state.cycles != 2u || stored != 0xA55Au)
                return false;
        }

        uint16_t restored =
            static_cast<uint16_t>(page[offset]) |
            (static_cast<uint16_t>(page[offset + 1]) << 8);
        if (restored != before)
            return false;
    }

    // Gate ARM7 post-index store/writeback forms. These paths share the
    // same transactional journal as pre-index stores but must use the original
    // base as the memory address and commit the adjusted base afterwards.
    {
        const uint32_t base = loadAddress + 0x60u;
        uint8_t *page = memory->getDirectWritePage(1, base);
        if (!page)
            return false;
        uint32_t offset = base & 0xFFFu;
        uint32_t before =
            static_cast<uint32_t>(page[offset]) |
            (static_cast<uint32_t>(page[offset + 1]) << 8) |
            (static_cast<uint32_t>(page[offset + 2]) << 16) |
            (static_cast<uint32_t>(page[offset + 3]) << 24);

        uint32_t *code = cache.reserveWords(192);
        if (!code)
            return false;
        size_t words = emitArmImmediateStoreFastInstruction(
            0xE4801004u, code, false, false, 1);
            // STR r1,[r0],#4
        if (!words)
            return false;
        code[words++] = R5900::jr(R5900::R_RA);
        code[words++] = R5900::nop();
        cache.commitWords(words);
        cache.syncExecutable(code, code + words);
        typedef void (*Arm7PostWordStoreFunction)(ArmAluState *);
        Arm7PostWordStoreFunction function =
            reinterpret_cast<Arm7PostWordStoreFunction>(code);

        {
            ArmAluState state = {};
            state.r[0] = base;
            state.r[1] = 0x10203040u;
            state.memory = memory;
            state.cpuId = 1;
            state.writeCacheTag = (base >> 12) + 1u;
            state.writeCachePage = page;
            ArmMemoryWriteGuard writes(&state);
            function(&state);
            uint32_t stored =
                static_cast<uint32_t>(page[offset]) |
                (static_cast<uint32_t>(page[offset + 1]) << 8) |
                (static_cast<uint32_t>(page[offset + 2]) << 16) |
                (static_cast<uint32_t>(page[offset + 3]) << 24);
            if (state.memoryFault || state.writeCount != 1 ||
                state.cycles != 2u || state.r[0] != base + 4u ||
                stored != 0x10203040u)
                return false;
        }

        uint32_t restored =
            static_cast<uint32_t>(page[offset]) |
            (static_cast<uint32_t>(page[offset + 1]) << 8) |
            (static_cast<uint32_t>(page[offset + 2]) << 16) |
            (static_cast<uint32_t>(page[offset + 3]) << 24);
        if (restored != before)
            return false;
    }

    {
        const uint32_t base = loadAddress + 0x70u;
        uint8_t *page = memory->getDirectWritePage(1, base);
        if (!page)
            return false;
        uint32_t offset = base & 0xFFFu;
        uint8_t before = page[offset];

        uint32_t *code = cache.reserveWords(192);
        if (!code)
            return false;
        size_t words = emitArmRegisterStoreFastInstruction(
            0xE6C01002u, code, false, false, 1);
            // STRB r1,[r0],r2
        if (!words)
            return false;
        code[words++] = R5900::jr(R5900::R_RA);
        code[words++] = R5900::nop();
        cache.commitWords(words);
        cache.syncExecutable(code, code + words);
        typedef void (*Arm7PostByteStoreFunction)(ArmAluState *);
        Arm7PostByteStoreFunction function =
            reinterpret_cast<Arm7PostByteStoreFunction>(code);

        {
            ArmAluState state = {};
            state.r[0] = base;
            state.r[1] = 0xD7u;
            state.r[2] = 4u;
            state.memory = memory;
            state.cpuId = 1;
            state.writeCacheTag = (base >> 12) + 1u;
            state.writeCachePage = page;
            ArmMemoryWriteGuard writes(&state);
            function(&state);
            if (state.memoryFault || state.writeCount != 1 ||
                state.cycles != 2u || state.r[0] != base + 4u ||
                page[offset] != 0xD7u)
                return false;
        }
        if (page[offset] != before)
            return false;
    }

    // Gate pre-index load writeback. Word exercises cache hit, cache miss
    // (including t0 preservation across the refill helper) and a failed ARM
    // condition. Byte/halfword verify the same CPU1 timing and Rn update.
    {
        const uint32_t base = (loadAddress + 0x90u) & ~3u;
        const uint32_t target = base + 4u;
        uint8_t *page = memory->getDirectReadPage(1, target);
        if (!page)
            return false;
        uint32_t offset = target & 0xFFFu;
        uint32_t expected =
            static_cast<uint32_t>(page[offset]) |
            (static_cast<uint32_t>(page[offset + 1]) << 8) |
            (static_cast<uint32_t>(page[offset + 2]) << 16) |
            (static_cast<uint32_t>(page[offset + 3]) << 24);

        uint32_t *code = cache.reserveWords(160);
        if (!code)
            return false;
        size_t words = emitArmWordLoadCachedInstruction(
            0xE5B01004u, code, false, false, false, 1);
            // LDR r1,[r0,#4]!
        if (!words)
            return false;
        code[words++] = R5900::jr(R5900::R_RA);
        code[words++] = R5900::nop();
        cache.commitWords(words);
        cache.syncExecutable(code, code + words);
        ArmLoadFunction function =
            reinterpret_cast<ArmLoadFunction>(code);

        // Cache hit.
        {
            ArmAluState state = {};
            state.r[0] = base;
            state.r[1] = 0xCAFEBABEu;
            state.memory = memory;
            state.cpuId = 1;
            state.readCacheTag = (target >> 12) + 1u;
            state.readCachePage = page;
            function(&state);
            if (state.memoryFault || state.cycles != 3u ||
                state.r[0] != target || state.r[1] != expected)
                return false;
        }

        // Cache miss/refill must preserve the computed writeback address.
        {
            ArmAluState state = {};
            state.r[0] = base;
            state.r[1] = 0xCAFEBABEu;
            state.memory = memory;
            state.cpuId = 1;
            function(&state);
            if (state.memoryFault || state.cycles != 3u ||
                state.r[0] != target || state.r[1] != expected ||
                !state.readCacheTag || !state.readCachePage)
                return false;
        }

        // Failed condition: no load and no writeback.
        uint32_t *condCode = cache.reserveWords(160);
        if (!condCode)
            return false;
        size_t condWords = emitArmWordLoadCachedInstruction(
            0x05B01004u, condCode, false, false, false, 1);
            // LDR.EQ r1,[r0,#4]!
        if (!condWords)
            return false;
        condCode[condWords++] = R5900::jr(R5900::R_RA);
        condCode[condWords++] = R5900::nop();
        cache.commitWords(condWords);
        cache.syncExecutable(condCode, condCode + condWords);
        ArmLoadFunction condFunction =
            reinterpret_cast<ArmLoadFunction>(condCode);

        ArmAluState notTaken = {};
        notTaken.r[0] = base;
        notTaken.r[1] = 0xCAFEBABEu;
        notTaken.cpsr = 0x00000013u;
        notTaken.memory = memory;
        notTaken.cpuId = 1;
        notTaken.readCacheTag = (target >> 12) + 1u;
        notTaken.readCachePage = page;
        condFunction(&notTaken);
        if (notTaken.memoryFault || notTaken.cycles != 1u ||
            notTaken.r[0] != base || notTaken.r[1] != 0xCAFEBABEu)
            return false;
    }

    {
        const uint32_t base = loadAddress + 0xA0u;
        const uint32_t target = base + 1u;
        uint8_t *page = memory->getDirectReadPage(1, target);
        if (!page)
            return false;
        uint8_t expected = page[target & 0xFFFu];

        uint32_t *code = cache.reserveWords(144);
        if (!code)
            return false;
        size_t words = emitArmByteLoadCachedInstruction(
            0xE5F01001u, code, false, false, false, 1);
            // LDRB r1,[r0,#1]!
        if (!words)
            return false;
        code[words++] = R5900::jr(R5900::R_RA);
        code[words++] = R5900::nop();
        cache.commitWords(words);
        cache.syncExecutable(code, code + words);

        ArmAluState state = {};
        state.r[0] = base;
        state.r[1] = 0xCAFEBABEu;
        state.memory = memory;
        state.cpuId = 1;
        state.readCacheTag = (target >> 12) + 1u;
        state.readCachePage = page;
        ArmLoadFunction function =
            reinterpret_cast<ArmLoadFunction>(code);
        function(&state);
        if (state.memoryFault || state.cycles != 3u ||
            state.r[0] != target || state.r[1] != expected)
            return false;
    }

    {
        const uint32_t base = (loadAddress + 0xB0u) & ~1u;
        const uint32_t target = base + 2u;
        uint8_t *page = memory->getDirectReadPage(1, target);
        if (!page)
            return false;
        uint32_t offset = target & 0xFFFu;
        uint16_t expected =
            static_cast<uint16_t>(page[offset]) |
            (static_cast<uint16_t>(page[offset + 1]) << 8);

        uint32_t *code = cache.reserveWords(160);
        if (!code)
            return false;
        size_t words = emitArmHalfwordLoadCachedInstruction(
            0xE1F010B2u, code, false, false, false, 1);
            // LDRH r1,[r0,#2]!
        if (!words)
            return false;
        code[words++] = R5900::jr(R5900::R_RA);
        code[words++] = R5900::nop();
        cache.commitWords(words);
        cache.syncExecutable(code, code + words);

        ArmAluState state = {};
        state.r[0] = base;
        state.r[1] = 0xCAFEBABEu;
        state.memory = memory;
        state.cpuId = 1;
        state.readCacheTag = (target >> 12) + 1u;
        state.readCachePage = page;
        ArmLoadFunction function =
            reinterpret_cast<ArmLoadFunction>(code);
        function(&state);
        if (state.memoryFault || state.cycles != 3u ||
            state.r[0] != target || state.r[1] != expected)
            return false;
    }

    // Gate immediate post-index loads. The memory access uses the original
    // base; writeback happens only after the load path completes. Word uses a
    // forced cache miss to prove the base survives the refill helper.
    {
        const uint32_t base = (loadAddress + 0xC0u) & ~3u;
        uint8_t *page = memory->getDirectReadPage(1, base);
        if (!page)
            return false;
        uint32_t offset = base & 0xFFFu;
        uint32_t expected =
            static_cast<uint32_t>(page[offset]) |
            (static_cast<uint32_t>(page[offset + 1]) << 8) |
            (static_cast<uint32_t>(page[offset + 2]) << 16) |
            (static_cast<uint32_t>(page[offset + 3]) << 24);

        uint32_t *code = cache.reserveWords(176);
        if (!code)
            return false;
        size_t words = emitArmWordLoadCachedInstruction(
            0xE4901004u, code, false, false, false, 1);
            // LDR r1,[r0],#4
        if (!words)
            return false;
        code[words++] = R5900::jr(R5900::R_RA);
        code[words++] = R5900::nop();
        cache.commitWords(words);
        cache.syncExecutable(code, code + words);

        ArmAluState state = {};
        state.r[0] = base;
        state.r[1] = 0xCAFEBABEu;
        state.memory = memory;
        state.cpuId = 1; // leave cache empty -> refill path
        ArmLoadFunction function =
            reinterpret_cast<ArmLoadFunction>(code);
        function(&state);
        if (state.memoryFault || state.cycles != 3u ||
            state.r[0] != base + 4u || state.r[1] != expected ||
            !state.readCacheTag || !state.readCachePage)
            return false;
    }

    {
        const uint32_t base = loadAddress + 0xD0u;
        uint8_t *page = memory->getDirectReadPage(1, base);
        if (!page)
            return false;
        uint8_t expected = page[base & 0xFFFu];

        uint32_t *code = cache.reserveWords(160);
        if (!code)
            return false;
        size_t words = emitArmByteLoadCachedInstruction(
            0xE4D01001u, code, false, false, false, 1);
            // LDRB r1,[r0],#1
        if (!words)
            return false;
        code[words++] = R5900::jr(R5900::R_RA);
        code[words++] = R5900::nop();
        cache.commitWords(words);
        cache.syncExecutable(code, code + words);

        ArmAluState state = {};
        state.r[0] = base;
        state.r[1] = 0xCAFEBABEu;
        state.memory = memory;
        state.cpuId = 1;
        state.readCacheTag = (base >> 12) + 1u;
        state.readCachePage = page;
        ArmLoadFunction function =
            reinterpret_cast<ArmLoadFunction>(code);
        function(&state);
        if (state.memoryFault || state.cycles != 3u ||
            state.r[0] != base + 1u || state.r[1] != expected)
            return false;
    }

    {
        const uint32_t base = (loadAddress + 0xE0u) & ~1u;
        uint8_t *page = memory->getDirectReadPage(1, base);
        if (!page)
            return false;
        uint32_t offset = base & 0xFFFu;
        uint16_t expected =
            static_cast<uint16_t>(page[offset]) |
            (static_cast<uint16_t>(page[offset + 1]) << 8);

        uint32_t *code = cache.reserveWords(176);
        if (!code)
            return false;
        size_t words = emitArmHalfwordLoadCachedInstruction(
            0xE0D010B2u, code, false, false, false, 1);
            // LDRH r1,[r0],#2
        if (!words)
            return false;
        code[words++] = R5900::jr(R5900::R_RA);
        code[words++] = R5900::nop();
        cache.commitWords(words);
        cache.syncExecutable(code, code + words);

        ArmAluState state = {};
        state.r[0] = base;
        state.r[1] = 0xCAFEBABEu;
        state.memory = memory;
        state.cpuId = 1;
        state.readCacheTag = (base >> 12) + 1u;
        state.readCachePage = page;
        ArmLoadFunction function =
            reinterpret_cast<ArmLoadFunction>(code);
        function(&state);
        if (state.memoryFault || state.cycles != 3u ||
            state.r[0] != base + 2u || state.r[1] != expected)
            return false;
    }

    // Gate register-offset post-index loads. Word forces the refill path
    // so both the original memory address and adjusted writeback value must
    // survive the nested C++ helper.
    {
        const uint32_t base = (loadAddress + 0x100u) & ~3u;
        uint8_t *page = memory->getDirectReadPage(1, base);
        if (!page)
            return false;
        uint32_t offset = base & 0xFFFu;
        uint32_t expected =
            static_cast<uint32_t>(page[offset]) |
            (static_cast<uint32_t>(page[offset + 1]) << 8) |
            (static_cast<uint32_t>(page[offset + 2]) << 16) |
            (static_cast<uint32_t>(page[offset + 3]) << 24);

        uint32_t *code = cache.reserveWords(192);
        if (!code)
            return false;
        size_t words = emitArmWordLoadCachedInstruction(
            0xE6901102u, code, false, false, false, 1);
            // LDR r1,[r0],r2,LSL #2
        if (!words)
            return false;
        code[words++] = R5900::jr(R5900::R_RA);
        code[words++] = R5900::nop();
        cache.commitWords(words);
        cache.syncExecutable(code, code + words);

        ArmAluState state = {};
        state.r[0] = base;
        state.r[1] = 0xCAFEBABEu;
        state.r[2] = 1u;
        state.memory = memory;
        state.cpuId = 1; // cache miss
        ArmLoadFunction function =
            reinterpret_cast<ArmLoadFunction>(code);
        function(&state);
        if (state.memoryFault || state.cycles != 3u ||
            state.r[0] != base + 4u || state.r[1] != expected ||
            !state.readCacheTag || !state.readCachePage)
            return false;
    }

    {
        const uint32_t base = loadAddress + 0x110u;
        uint8_t *page = memory->getDirectReadPage(1, base);
        if (!page)
            return false;
        uint8_t expected = page[base & 0xFFFu];

        uint32_t *code = cache.reserveWords(176);
        if (!code)
            return false;
        size_t words = emitArmByteLoadCachedInstruction(
            0xE6D01002u, code, false, false, false, 1);
            // LDRB r1,[r0],r2
        if (!words)
            return false;
        code[words++] = R5900::jr(R5900::R_RA);
        code[words++] = R5900::nop();
        cache.commitWords(words);
        cache.syncExecutable(code, code + words);

        ArmAluState state = {};
        state.r[0] = base;
        state.r[1] = 0xCAFEBABEu;
        state.r[2] = 4u;
        state.memory = memory;
        state.cpuId = 1;
        state.readCacheTag = (base >> 12) + 1u;
        state.readCachePage = page;
        ArmLoadFunction function =
            reinterpret_cast<ArmLoadFunction>(code);
        function(&state);
        if (state.memoryFault || state.cycles != 3u ||
            state.r[0] != base + 4u || state.r[1] != expected)
            return false;
    }

    {
        const uint32_t base = (loadAddress + 0x120u) & ~1u;
        uint8_t *page = memory->getDirectReadPage(1, base);
        if (!page)
            return false;
        uint32_t offset = base & 0xFFFu;
        uint16_t expected =
            static_cast<uint16_t>(page[offset]) |
            (static_cast<uint16_t>(page[offset + 1]) << 8);

        uint32_t *code = cache.reserveWords(192);
        if (!code)
            return false;
        size_t words = emitArmHalfwordLoadCachedInstruction(
            0xE09010B2u, code, false, false, false, 1);
            // LDRH r1,[r0],r2
        if (!words)
            return false;
        code[words++] = R5900::jr(R5900::R_RA);
        code[words++] = R5900::nop();
        cache.commitWords(words);
        cache.syncExecutable(code, code + words);

        ArmAluState state = {};
        state.r[0] = base;
        state.r[1] = 0xCAFEBABEu;
        state.r[2] = 2u;
        state.memory = memory;
        state.cpuId = 1;
        state.readCacheTag = (base >> 12) + 1u;
        state.readCachePage = page;
        ArmLoadFunction function =
            reinterpret_cast<ArmLoadFunction>(code);
        function(&state);
        if (state.memoryFault || state.cycles != 3u ||
            state.r[0] != base + 2u || state.r[1] != expected)
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
    uint32_t *guestPcOut, uint32_t *instructionCount, uint32_t *stateHash,
    size_t maxInstructions)
{
    if (!CpuBackendAccess::isArm9(cpu) || CpuBackendAccess::isThumb(cpu))
        return 0;

    synchronizeMemoryMap();

    constexpr size_t MAX_BLOCK_INSTRUCTIONS = 24;
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

        Ps2ArmAlu::DecodedMultiply multiply;
        if (Ps2ArmAlu::decodeMultiply(opcode, &multiply))
        {
            opcodes[count++] = opcode;
            continue;
        }

        Ps2ArmAlu::DecodedLongMultiply longMultiply;
        if (Ps2ArmAlu::decodeLongMultiply(opcode, &longMultiply))
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

        Ps2ArmAlu::DecodedMultiBlockTransfer multiBlockTransfer;
        if (Ps2ArmAlu::decodeMultiBlockTransfer(opcode, &multiBlockTransfer))
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
            continue;
        }

        Ps2ArmAlu::DecodedMultiply multiply;
        if (Ps2ArmAlu::decodeMultiply(opcodes[i], &multiply))
        {
            if (!applyArmMultiply(opcodes[i], &expected))
                return -1;
            continue;
        }

        Ps2ArmAlu::DecodedLongMultiply longMultiply;
        if (Ps2ArmAlu::decodeLongMultiply(opcodes[i], &longMultiply))
        {
            if (!applyArmLongMultiply(opcodes[i], &expected))
                return -1;
            continue;
        }

        Ps2ArmAlu::DecodedHalfwordTransfer transfer;
        if (Ps2ArmAlu::decodeHalfwordTransfer(opcodes[i], &transfer))
        {
            if (!applyArmHalfwordTransfer(&expected, opcodes[i]))
                return 0;
            continue;
        }

        Ps2ArmAlu::DecodedByteTransfer byteTransfer;
        if (Ps2ArmAlu::decodeByteTransfer(opcodes[i], &byteTransfer))
        {
            if (!applyArmByteTransfer(&expected, opcodes[i]))
                return 0;
            continue;
        }

        Ps2ArmAlu::DecodedLiteralLoad literalLoad;
        if (Ps2ArmAlu::decodeLiteralLoad(opcodes[i], &literalLoad))
        {
            if (!applyArmLiteralLoad(&expected, opcodes[i],
                guestPc + static_cast<uint32_t>(i * 4)))
                return 0;
            continue;
        }

        Ps2ArmAlu::DecodedSingleTransfer singleTransfer;
        if (Ps2ArmAlu::decodeSingleTransfer(opcodes[i], &singleTransfer))
        {
            if (!applyArmSingleTransfer(&expected, opcodes[i]))
                return 0;
            continue;
        }

        Ps2ArmAlu::DecodedMultiBlockTransfer multiBlockTransfer;
        if (Ps2ArmAlu::decodeMultiBlockTransfer(opcodes[i], &multiBlockTransfer))
        {
            if (!applyArmMultiBlockTransfer(&expected, opcodes[i]))
                return 0;
            continue;
        }

        Ps2ArmAlu::DecodedSingleBlockTransfer blockTransfer;
        if (Ps2ArmAlu::decodeSingleBlockTransfer(opcodes[i], &blockTransfer))
        {
            if (!applyArmSingleBlockTransfer(&expected, opcodes[i]))
                return 0;
            continue;
        }

        Ps2ArmAlu::DecodedBranchExchange branchExchange;
        if (Ps2ArmAlu::decodeBranchExchange(opcodes[i], &branchExchange))
        {
            if (!applyArmBranchExchange(&expected, opcodes[i]))
                return -1;
            continue;
        }

        if (!applyArmBranch(&expected, opcodes[i],
            guestPc + static_cast<uint32_t>(i * 4)))
            return -1;
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



int DynarecR5900::runArm7AluBranchShadowSelfTest(const Interpreter &cpu,
    uint32_t *guestPcOut, uint32_t *instructionCount, uint32_t *stateHash,
    size_t maxInstructions)
{
    if (CpuBackendAccess::isArm9(cpu) || CpuBackendAccess::isThumb(cpu) ||
        !memory || !maxInstructions)
        return 0;

    synchronizeMemoryMap();

    constexpr size_t MAX_BLOCK_INSTRUCTIONS = 24;
    maxInstructions = std::min(maxInstructions, MAX_BLOCK_INSTRUCTIONS);

    uint32_t opcodes[MAX_BLOCK_INSTRUCTIONS] = {};
    size_t count = 0;
    uint32_t guestPc = CpuBackendAccess::nextInstructionAddress(cpu);

    for (size_t i = 0; i < maxInstructions; i++)
    {
        uint32_t opcode = 0;
        if (i == 0)
        {
            opcode = CpuBackendAccess::pipelineOpcode(cpu, 0);
        }
        else if (i == 1)
        {
            opcode = CpuBackendAccess::pipelineOpcode(cpu, 1);
        }
        else
        {
            uint32_t address =
                guestPc + static_cast<uint32_t>(i * 4);
            uint32_t offset = address & 0xFFFu;
            uint8_t *page = memory->getDirectReadPage(1, address);
            if (!page || offset > 0xFFCu)
                break;
            opcode = static_cast<uint32_t>(page[offset]) |
                (static_cast<uint32_t>(page[offset + 1]) << 8) |
                (static_cast<uint32_t>(page[offset + 2]) << 16) |
                (static_cast<uint32_t>(page[offset + 3]) << 24);
        }

        Ps2ArmAlu::Decoded decoded;
        if (Ps2ArmAlu::decode(opcode, &decoded))
        {
            Ps2ArmAlu::InstructionInfo info;
            Ps2ArmAlu::inspect(opcode,
                guestPc + static_cast<uint32_t>(i * 4), &info);

            // Keep the first ARM7 gate independent from the interpreter's
            // pipeline-visible R15 value. Literal/memory forms are not part of
            // this subset either.
            if ((info.readMask | info.writeMask) & 0x8000u)
                break;

            opcodes[count++] = opcode;
            continue;
        }

        Ps2ArmAlu::DecodedMultiply multiply;
        if (Ps2ArmAlu::decodeMultiply(opcode, &multiply))
        {
            if (multiply.rd == 15 || multiply.rm == 15 ||
                multiply.rs == 15 ||
                (multiply.accumulate && multiply.rn == 15))
                break;
            opcodes[count++] = opcode;
            continue;
        }

        Ps2ArmAlu::DecodedLongMultiply longMultiply;
        if (Ps2ArmAlu::decodeLongMultiply(opcode, &longMultiply))
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
        if (Ps2ArmAlu::decodeSingleTransfer(opcode, &singleTransfer) &&
            true)
        {
            opcodes[count++] = opcode;
            continue;
        }

        Ps2ArmAlu::DecodedByteTransfer byteTransfer;
        if (Ps2ArmAlu::decodeByteTransfer(opcode, &byteTransfer) &&
            true)
        {
            opcodes[count++] = opcode;
            continue;
        }

        Ps2ArmAlu::DecodedHalfwordTransfer halfwordTransfer;
        if (Ps2ArmAlu::decodeHalfwordTransfer(opcode, &halfwordTransfer) &&
            true)
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

        Ps2ArmAlu::DecodedMultiBlockTransfer multiBlockTransfer;
        if (Ps2ArmAlu::decodeMultiBlockTransfer(opcode, &multiBlockTransfer))
        {
            opcodes[count++] = opcode;
            continue;
        }

        Ps2ArmAlu::DecodedBranchExchange branchExchange;
        if (Ps2ArmAlu::decodeBranchExchange(opcode, &branchExchange))
        {
            if (branchExchange.rm == 15)
                break;
            opcodes[count++] = opcode;
            break;
        }

        Ps2ArmAlu::DecodedBranch branch;
        if (Ps2ArmAlu::decodeBranch(opcode, &branch))
        {
            opcodes[count++] = opcode;
            break;
        }

        // Remaining complex/CPU-specific forms stay interpreter-only until
        // their timing/semantics receive a dedicated gate.
        break;
    }

    if (!count)
        return 0;

    ArmAluState source = {};
    for (int i = 0; i < 16; i++)
        source.r[i] = CpuBackendAccess::readReg(cpu, i);
    source.cpsr = CpuBackendAccess::readCpsr(cpu);
    source.nextPc = guestPc + static_cast<uint32_t>(count * 4);
    source.memory = memory;
    source.cpuId = 1;

    ArmAluState expected = source;
    ArmMemoryWriteGuard expectedWrites(&expected);
    for (size_t i = 0; i < count; i++)
    {
        Ps2ArmAlu::Decoded decoded;
        if (Ps2ArmAlu::decode(opcodes[i], &decoded))
        {
            if (!applyArmAlu(opcodes[i], &expected))
                return -1;
            continue;
        }

        Ps2ArmAlu::DecodedMultiply multiply;
        if (Ps2ArmAlu::decodeMultiply(opcodes[i], &multiply))
        {
            if (!applyArmMultiply(opcodes[i], &expected))
                return -1;
            continue;
        }

        Ps2ArmAlu::DecodedLongMultiply longMultiply;
        if (Ps2ArmAlu::decodeLongMultiply(opcodes[i], &longMultiply))
        {
            if (!applyArmLongMultiply(opcodes[i], &expected))
                return -1;
            continue;
        }

        Ps2ArmAlu::DecodedLiteralLoad literalLoad;
        if (Ps2ArmAlu::decodeLiteralLoad(opcodes[i], &literalLoad))
        {
            if (!applyArmLiteralLoad(&expected, opcodes[i],
                guestPc + static_cast<uint32_t>(i * 4)))
                return 0;
            continue;
        }

        Ps2ArmAlu::DecodedSingleTransfer singleTransfer;
        if (Ps2ArmAlu::decodeSingleTransfer(opcodes[i], &singleTransfer))
        {

            if (!applyArmSingleTransfer(&expected, opcodes[i]))
                return 0;
            continue;
        }

        Ps2ArmAlu::DecodedByteTransfer byteTransfer;
        if (Ps2ArmAlu::decodeByteTransfer(opcodes[i], &byteTransfer))
        {

            if (!applyArmByteTransfer(&expected, opcodes[i]))
                return 0;
            continue;
        }

        Ps2ArmAlu::DecodedHalfwordTransfer halfwordTransfer;
        if (Ps2ArmAlu::decodeHalfwordTransfer(opcodes[i], &halfwordTransfer))
        {

            if (!applyArmHalfwordTransfer(&expected, opcodes[i]))
                return 0;
            continue;
        }

        Ps2ArmAlu::DecodedSingleBlockTransfer blockTransfer;
        if (Ps2ArmAlu::decodeSingleBlockTransfer(opcodes[i], &blockTransfer))
        {
            if (!applyArmSingleBlockTransfer(&expected, opcodes[i]))
                return 0;
            continue;
        }

        Ps2ArmAlu::DecodedMultiBlockTransfer multiBlockTransfer;
        if (Ps2ArmAlu::decodeMultiBlockTransfer(opcodes[i], &multiBlockTransfer))
        {
            if (!applyArmMultiBlockTransfer(&expected, opcodes[i]))
                return 0;
            continue;
        }

        Ps2ArmAlu::DecodedBranchExchange branchExchange;
        if (Ps2ArmAlu::decodeBranchExchange(opcodes[i], &branchExchange))
        {
            if (!applyArmBranchExchange(&expected, opcodes[i]))
                return -1;
            continue;
        }

        if (!applyArmBranch(&expected, opcodes[i],
            guestPc + static_cast<uint32_t>(i * 4)))
            return -1;
    }

    // Generated code must observe the pre-store bytes too.
    expectedWrites.restoreNow();

    uint32_t *entry = nullptr;
    uint32_t hostWords = 0;
    if (!emitArmAluSequence(opcodes, count, guestPc, &entry, &hostWords))
        return -1;

    R5900CompiledBlock *compiled = findMutableBlock(guestPc, false);
    if (!compiled)
        return -1;

    ArmAluState actual = source;
    ArmMemoryWriteGuard actualWrites(&actual);
    typedef void (*Arm7ShadowFunction)(ArmAluState *);
    Arm7ShadowFunction function =
        reinterpret_cast<Arm7ShadowFunction>(entry);
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
    if (actual.cpsr != expected.cpsr ||
        actual.nextPc != expected.nextPc ||
        actual.cycles != expected.cycles)
        return -1;

    hash ^= actual.cpsr;
    hash *= 16777619u;
    hash ^= actual.nextPc;
    hash *= 16777619u;
    hash ^= actual.cycles;
    hash *= 16777619u;
    hash ^= static_cast<uint32_t>(count);
    hash *= 16777619u;
    hash ^= hostWords;
    hash *= 16777619u;

    compiled->arm7TimingSafe = true;

    if (guestPcOut)
        *guestPcOut = guestPc;
    if (instructionCount)
        *instructionCount = static_cast<uint32_t>(count);
    if (stateHash)
        *stateHash = hash;
    return 1;
}


int DynarecR5900::runArm7AluBranchDifferentialSelfTest(Interpreter &cpu,
    uint32_t *guestPcOut, uint32_t *instructionCount, uint32_t *stateHash)
{
    if (CpuBackendAccess::isArm9(cpu) || CpuBackendAccess::isThumb(cpu))
        return 0;

    uint32_t guestPc = CpuBackendAccess::nextInstructionAddress(cpu);
    R5900CompiledBlock *block = findMutableBlock(guestPc, false);
    if (!block || !block->hostCode || !block->guestInstructions)
        return 0;

    uint32_t savedRegs[16] = {};
    for (int i = 0; i < 16; i++)
        savedRegs[i] = CpuBackendAccess::readReg(cpu, i);
    uint32_t savedCpsr = CpuBackendAccess::readCpsr(cpu);
    uint32_t savedPipeline0 = CpuBackendAccess::pipelineOpcode(cpu, 0);
    uint32_t savedPipeline1 = CpuBackendAccess::pipelineOpcode(cpu, 1);

    ArmAluState generated = {};
    for (int i = 0; i < 16; i++)
        generated.r[i] = savedRegs[i];
    generated.cpsr = savedCpsr;
    generated.nextPc = block->guestEndPc;
    generated.memory = memory;
    generated.cpuId = 1;

    typedef void (*Arm7DifferentialFunction)(ArmAluState *);
    Arm7DifferentialFunction function =
        reinterpret_cast<Arm7DifferentialFunction>(block->hostCode);

    ArmMemoryWrite generatedWrites[16] = {};
    uint32_t generatedWriteCount = 0;
    {
        ArmMemoryWriteGuard generatedGuard(&generated);
        function(&generated);
        if (generated.memoryFault)
            return 0;

        generatedWriteCount = generated.writeCount;
        if (generatedWriteCount > 16)
            return -1;
        for (uint32_t i = 0; i < generatedWriteCount; i++)
            generatedWrites[i] = generated.writes[i];
        generatedGuard.restoreNow();
    }

    MemoryWriteJournal interpreterWrites;
    memory->beginWriteJournal(&interpreterWrites, false);

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
    memory->endWriteJournal(&interpreterWrites);

    bool writesMatch = !interpreterWrites.overflow &&
        interpreterWrites.count == generatedWriteCount;
    if (writesMatch)
    {
        for (uint32_t i = 0; i < generatedWriteCount; i++)
        {
            const MemoryWriteJournalEntry &actual = interpreterWrites.entries[i];
            const ArmMemoryWrite &expected = generatedWrites[i];
            if (actual.address != expected.address ||
                actual.before != expected.before ||
                actual.after != expected.after ||
                actual.size != expected.size)
            {
                writesMatch = false;
                break;
            }
        }
    }

    bool match = interpreterOk && writesMatch &&
        CpuBackendAccess::readCpsr(cpu) == generated.cpsr &&
        CpuBackendAccess::nextInstructionAddress(cpu) == generated.nextPc &&
        interpreterCycles == generated.cycles;

    uint32_t hash = 2166136261u;
    for (int i = 0; i < 15; i++)
    {
        uint32_t value = CpuBackendAccess::readReg(cpu, i);
        if (value != generated.r[i])
            match = false;
        hash ^= value;
        hash *= 16777619u;
    }
    hash ^= CpuBackendAccess::readCpsr(cpu);
    hash *= 16777619u;
    hash ^= CpuBackendAccess::nextInstructionAddress(cpu);
    hash *= 16777619u;
    hash ^= interpreterCycles;
    hash *= 16777619u;
    hash ^= block->guestInstructions;
    hash *= 16777619u;
    for (uint32_t i = 0; i < generatedWriteCount; i++)
    {
        hash ^= generatedWrites[i].address;
        hash *= 16777619u;
        hash ^= generatedWrites[i].after;
        hash *= 16777619u;
    }

    memory->rollbackWriteJournal(interpreterWrites);
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



int DynarecR5900::runArm7ActiveAluBranchBlock(Interpreter &cpu,
    uint32_t cycleBudget, uint32_t *guestInstructions)
{
    if (guestInstructions)
        *guestInstructions = 0;

    DYNAREC_PROFILE(profile.activeAttempts++);
    if (!cycleBudget || CpuBackendAccess::isArm9(cpu) ||
        CpuBackendAccess::isThumb(cpu))
    {
        DYNAREC_PROFILE(profile.activeStateFallbacks++);
        return 0;
    }

    synchronizeMemoryMap();
    uint32_t guestPc = CpuBackendAccess::nextInstructionAddress(cpu);
    constexpr uint32_t MIN_ARM7_ACTIVE_INSTRUCTIONS = 2;

    // Recovery mode after the live-ARM7 black-screen regression: only admit
    // one read-only, non-resident block at a time. Shadow+differential support
    // remains wider so stores/self-loops keep getting validated without being
    // allowed to mutate live state. Re-expand each feature independently once
    // this conservative path survives long runtime tests.
    auto conservativeArm7LiveSafe = [](const R5900CompiledBlock *candidate)
    {
        return candidate &&
            candidate->arm7TimingSafe &&
            candidate->guestInstructions >= MIN_ARM7_ACTIVE_INSTRUCTIONS &&
            !candidate->writesMemory &&
            !candidate->internalSelfLoop;
    };

    R5900CompiledBlock *block = findMutableBlock(guestPc, false);
    if (!conservativeArm7LiveSafe(block))
    {
        ActiveRejectKind rejected = findActiveReject(guestPc);
        if (rejected != ACTIVE_REJECT_NONE)
        {
            DYNAREC_PROFILE(profile.activeVerifyFallbacks++; recordActiveFallback(profile, guestPc, R5900_ACTIVE_FALLBACK_VERIFY));
            return ARM9_BLOCK_RUNNER_PERMANENT_REJECT;
        }

        if (blocks.size() >= 2048)
        {
            cacheActiveReject(guestPc, ACTIVE_REJECT_VERIFY);
            DYNAREC_PROFILE(profile.activeVerifyFallbacks++; recordActiveFallback(profile, guestPc, R5900_ACTIVE_FALLBACK_VERIFY));
            return ARM9_BLOCK_RUNNER_PERMANENT_REJECT;
        }

        DYNAREC_PROFILE(profile.activeCompileMisses++; recordActiveFallback(profile, guestPc, R5900_ACTIVE_FALLBACK_COMPILE));
        int shadow = runArm7AluBranchShadowSelfTest(cpu, nullptr, nullptr,
            nullptr, 16);
        if (shadow <= 0)
        {
            cacheActiveReject(guestPc, ACTIVE_REJECT_VERIFY);
            DYNAREC_PROFILE(profile.activeVerifyFallbacks++; recordActiveFallback(profile, guestPc, R5900_ACTIVE_FALLBACK_VERIFY));
            return ARM9_BLOCK_RUNNER_PERMANENT_REJECT;
        }

        block = findMutableBlock(guestPc, false);
        if (!conservativeArm7LiveSafe(block))
        {
            cacheActiveReject(guestPc, ACTIVE_REJECT_VERIFY);
            DYNAREC_PROFILE(profile.activeVerifyFallbacks++; recordActiveFallback(profile, guestPc, R5900_ACTIVE_FALLBACK_VERIFY));
            return ARM9_BLOCK_RUNNER_PERMANENT_REJECT;
        }
    }

    uint32_t minimumCycles = block->minCycles
        ? block->minCycles
        : block->guestInstructions;
    if (minimumCycles > cycleBudget)
    {
        DYNAREC_PROFILE(profile.activeBudgetFallbacks++; recordActiveFallback(profile, guestPc, R5900_ACTIVE_FALLBACK_BUDGET));
        return -static_cast<int>(minimumCycles);
    }

    if (!block->activeReady)
    {
        if (!conservativeArm7LiveSafe(block) ||
            block->maxMemoryWrites ||
            ((block->readMask | block->writeMask) & 0x8000u))
        {
            cacheActiveReject(guestPc, ACTIVE_REJECT_STATE);
            DYNAREC_PROFILE(profile.activeStateFallbacks++; recordActiveFallback(profile, guestPc, R5900_ACTIVE_FALLBACK_STATE));
            return ARM9_BLOCK_RUNNER_PERMANENT_REJECT;
        }

        if (!block->differentialVerified)
        {
            int differential =
                runArm7AluBranchDifferentialSelfTest(cpu);
            if (differential <= 0)
            {
                cacheActiveReject(guestPc, ACTIVE_REJECT_VERIFY);
                DYNAREC_PROFILE(profile.activeVerifyFallbacks++; recordActiveFallback(profile, guestPc, R5900_ACTIVE_FALLBACK_VERIFY));
                return ARM9_BLOCK_RUNNER_PERMANENT_REJECT;
            }

            block = findMutableBlock(guestPc, false);
            if (!block || !block->differentialVerified ||
                !block->arm7TimingSafe)
            {
                cacheActiveReject(guestPc, ACTIVE_REJECT_VERIFY);
                DYNAREC_PROFILE(profile.activeVerifyFallbacks++; recordActiveFallback(profile, guestPc, R5900_ACTIVE_FALLBACK_VERIFY));
                return ARM9_BLOCK_RUNNER_PERMANENT_REJECT;
            }
        }

        block->activeReady = true;
    }

    // Keep guest state resident across a short chain of already-verified
    // ARM7 blocks. Unlike the ARM9 chain, this first ARM7 version stops after
    // the first actual memory write, so no later compiled code can observe or
    // be invalidated by a store before the final architectural commit.
    ArmAluState state;
    uint16_t initializedMask = block->inputMask;
    uint16_t unionWriteMask = static_cast<uint16_t>(block->writeMask & 0x7FFFu);
    for (uint8_t i = 0; i < block->inputRegCount; i++)
    {
        uint8_t reg = block->inputRegs[i];
        state.r[reg] = CpuBackendAccess::readReg(cpu, reg);
    }

    uint32_t liveCpsr = CpuBackendAccess::readCpsr(cpu);
    state.cpsr = liveCpsr;
    state.memory = memory;
    state.memoryFault = 0;
    state.writeCount = 0;
    state.cycles = 0;
    state.readCacheTag = block->cachedReadTag;
    state.readCachePage = block->cachedReadPage;
    state.writeCacheTag = block->cachedWriteTag;
    state.writeCachePage = block->cachedWritePage;
    state.cycleLimit = cycleBudget;
    state.guestExecuted = 0;
    state.cpuId = 1;

    ArmMemoryWriteGuard liveWrites(&state);
    constexpr uint32_t WRITE_JOURNAL_CAPACITY =
        sizeof(state.writes) / sizeof(state.writes[0]);
    // Keep the recovery path to one generated block per architectural commit.
    // The previous 8-block resident chain was heavily exercised in the broken
    // run, while isolated differential gates still passed.
    constexpr uint32_t MAX_ARM7_RESIDENT_BLOCKS = 1;

    uint32_t totalCycles = 0;
    uint32_t executedGuest = 0;
    uint32_t executedBlocks = 0;
    R5900CompiledBlock *current = block;
    uint8_t unionCommitRegs[15] = {};
    uint8_t unionCommitRegCount = 0;
    bool buildingUnionCommit = false;

    while (current && executedBlocks < MAX_ARM7_RESIDENT_BLOCKS)
    {
        uint32_t remainingBudget = cycleBudget - totalCycles;
        uint32_t writesBefore = state.writeCount;
        state.nextPc = current->guestEndPc;
        state.guestExecuted = 0;
        state.cycles = 0;
        state.cycleLimit = remainingBudget;

        typedef void (*Arm7ActiveFunction)(ArmAluState *);
        Arm7ActiveFunction function =
            reinterpret_cast<Arm7ActiveFunction>(current->hostCode);
        function(&state);
        uint32_t blockCycles = state.cycles;

        if (state.readCacheTag && state.readCachePage)
        {
            current->cachedReadTag = state.readCacheTag;
            current->cachedReadPage = state.readCachePage;
        }
        if (state.writeCacheTag && state.writeCachePage)
        {
            current->cachedWriteTag = state.writeCacheTag;
            current->cachedWritePage = state.writeCachePage;
        }

        uint32_t blockGuest = state.guestExecuted
            ? state.guestExecuted
            : current->guestInstructions;
        executedGuest += blockGuest;
        executedBlocks++;

        if (state.memoryFault || state.writeCount < writesBefore ||
            state.writeCount > WRITE_JOURNAL_CAPACITY ||
            ((state.cpsr ^ liveCpsr) & 0x1Fu) ||
            !blockCycles || blockCycles > remainingBudget)
        {
            if (blockCycles > remainingBudget)
            {
                DYNAREC_PROFILE(profile.activeBudgetFallbacks++; recordActiveFallback(profile, guestPc, R5900_ACTIVE_FALLBACK_BUDGET));
                return -static_cast<int>(minimumCycles);
            }

            DYNAREC_PROFILE(profile.activeStateFallbacks++; recordActiveFallback(profile, guestPc, R5900_ACTIVE_FALLBACK_STATE));
            return 0;
        }

        totalCycles += blockCycles;

        uint16_t currentWriteMask = static_cast<uint16_t>(
            current->writeMask & 0x7FFFu);
        if (buildingUnionCommit)
        {
            uint16_t newWrites = static_cast<uint16_t>(
                currentWriteMask & ~unionWriteMask);
            for (uint8_t i = 0; i < current->writeRegCount; i++)
            {
                uint8_t reg = current->writeRegs[i];
                if (newWrites & static_cast<uint16_t>(1u << reg))
                    unionCommitRegs[unionCommitRegCount++] = reg;
            }
        }
        initializedMask |= currentWriteMask;
        unionWriteMask |= currentWriteMask;

        bool wroteThisBlock = state.writeCount > writesBefore;
        if (wroteThisBlock)
        {
            if (executedBlocks > 1)
                DYNAREC_PROFILE(profile.activeStoreChainBlocks++);
            break;
        }

        if ((state.cpsr & BIT(5)) ||
            executedBlocks >= MAX_ARM7_RESIDENT_BLOCKS ||
            totalCycles >= cycleBudget)
            break;

        uint32_t remaining = cycleBudget - totalCycles;
        R5900CompiledBlock *next = findMutableBlock(state.nextPc, false);
        if (!next || !next->activeReady || !next->arm7TimingSafe ||
            next->guestInstructions < MIN_ARM7_ACTIVE_INSTRUCTIONS ||
            !next->cycles || next->cycles > remaining)
            break;

        if (next->writesMemory &&
            (!next->maxMemoryWrites ||
             state.writeCount + next->maxMemoryWrites >
                WRITE_JOURNAL_CAPACITY))
            break;

        uint16_t nextInputMask = next->inputMask;
        for (uint8_t i = 0; i < next->inputRegCount; i++)
        {
            uint8_t reg = next->inputRegs[i];
            uint16_t bit = static_cast<uint16_t>(1u << reg);
            if (!(initializedMask & bit))
                state.r[reg] = CpuBackendAccess::readReg(cpu, reg);
        }
        initializedMask |= nextInputMask;

        if (!buildingUnionCommit)
        {
            for (uint8_t i = 0; i < block->writeRegCount; i++)
                unionCommitRegs[i] = block->writeRegs[i];
            unionCommitRegCount = block->writeRegCount;
            buildingUnionCommit = true;
        }

        current = next;
        DYNAREC_PROFILE(profile.activeInternalChainBlocks++);
    }

    if (!totalCycles)
    {
        DYNAREC_PROFILE(profile.activeStateFallbacks++; recordActiveFallback(profile, guestPc, R5900_ACTIVE_FALLBACK_STATE));
        return 0;
    }

    bool invalidatesCompiledCode = false;
    for (uint32_t i = 0; i < state.writeCount; i++)
    {
        if (writeOverlapsCompiledCode(state.writes[i].address,
            state.writes[i].size))
        {
            invalidatesCompiledCode = true;
            break;
        }
    }

    const uint8_t *commitRegs = block->writeRegs;
    uint8_t commitRegCount = block->writeRegCount;
    if (executedBlocks > 1)
    {
        commitRegs = unionCommitRegs;
        commitRegCount = unionCommitRegCount;
    }

    // Recovery mode deliberately uses the ordinary refill path. Reusing the
    // next compiled block's cached opcodes is a separate optimization and must
    // not be coupled to the first ARM7 live-correctness retest.
    bool committed = CpuBackendAccess::commitBlockStateSparse(cpu, state.r,
        commitRegs, commitRegCount, state.cpsr, state.nextPc);

    if (!committed)
    {
        DYNAREC_PROFILE(profile.activeStateFallbacks++; recordActiveFallback(profile, guestPc, R5900_ACTIVE_FALLBACK_STATE));
        return 0;
    }

    liveWrites.dismiss();

    DYNAREC_PROFILE(profile.activeExecutions += executedBlocks);
    DYNAREC_PROFILE(profile.activeGuestInstructions += executedGuest);
    DYNAREC_PROFILE(profile.activeCycles += totalCycles);
    if (guestInstructions)
        *guestInstructions = executedGuest;

    int committedCycles = static_cast<int>(totalCycles);
    if (invalidatesCompiledCode)
    {
        DYNAREC_PROFILE(profile.activeCodeWriteInvalidations++);
        reset();
    }
    return committedCycles;
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

    uint32_t savedRegs[16] = {};
    for (int i = 0; i < 16; i++)
        savedRegs[i] = CpuBackendAccess::readReg(cpu, i);

    uint32_t savedCpsr = CpuBackendAccess::readCpsr(cpu);
    uint32_t savedPipeline0 = CpuBackendAccess::pipelineOpcode(cpu, 0);
    uint32_t savedPipeline1 = CpuBackendAccess::pipelineOpcode(cpu, 1);

    ArmAluState dynarecState = {};
    for (int i = 0; i < 16; i++)
        dynarecState.r[i] = savedRegs[i];

    // Prove the live input-mask optimization instead of merely compiling it.
    // Registers in writeMask but not inputMask are classified as unconditional
    // write-only destinations. Poison their entry values here; any accidental
    // read/preserve dependency will diverge from the Interpreter below and the
    // block will never become activeReady.
    uint16_t deadEntryMask = static_cast<uint16_t>(
        (block->writeMask & 0x7FFFu) & ~block->inputMask);
    for (uint8_t reg = 0; reg < 15; reg++)
    {
        if (deadEntryMask & static_cast<uint16_t>(1u << reg))
            dynarecState.r[reg] = 0xA5A50000u ^
                (static_cast<uint32_t>(reg) * 0x11111111u);
    }

    dynarecState.cpsr = savedCpsr;
    dynarecState.nextPc = block->guestEndPc;
    dynarecState.memory = memory;

    typedef void (*ArmAluBlockFunction)(ArmAluState *);
    ArmAluBlockFunction function =
        reinterpret_cast<ArmAluBlockFunction>(block->hostCode);

    ArmMemoryWrite dynWrites[16] = {};
    uint32_t dynWriteCount = 0;
    {
        ArmMemoryWriteGuard dynarecWrites(&dynarecState);
        function(&dynarecState);
        if (dynarecState.memoryFault)
            return 0;

        dynWriteCount = dynarecState.writeCount;
        if (dynWriteCount > 16)
            return 0;
        for (uint32_t i = 0; i < dynWriteCount; i++)
            dynWrites[i] = dynarecState.writes[i];
        dynarecWrites.restoreNow();
    }

    MemoryWriteJournal interpreterWrites;
    memory->beginWriteJournal(&interpreterWrites, false);

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
    memory->endWriteJournal(&interpreterWrites);

    bool writesMatch = !interpreterWrites.overflow &&
        interpreterWrites.count == dynWriteCount;
    if (writesMatch)
    {
        for (uint32_t i = 0; i < dynWriteCount; i++)
        {
            const MemoryWriteJournalEntry &actual = interpreterWrites.entries[i];
            const ArmMemoryWrite &expected = dynWrites[i];
            if (actual.address != expected.address ||
                actual.before != expected.before ||
                actual.after != expected.after ||
                actual.size != expected.size)
            {
                writesMatch = false;
                break;
            }
        }
    }

    uint32_t hash = 2166136261u;
    bool match = interpreterOk && writesMatch &&
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
    for (uint32_t i = 0; i < dynWriteCount; i++)
    {
        hash ^= dynWrites[i].address;
        hash *= 16777619u;
        hash ^= dynWrites[i].after;
        hash *= 16777619u;
    }

    // Restore both temporary memory writes and the architectural CPU snapshot
    // before returning on every differential path.
    memory->rollbackWriteJournal(interpreterWrites);
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

    DYNAREC_PROFILE(profile.activeAttempts++);
    if (!cycleBudget || !CpuBackendAccess::isArm9(cpu) ||
        CpuBackendAccess::isThumb(cpu))
    {
        DYNAREC_PROFILE(profile.activeStateFallbacks++);
        return 0;
    }

    synchronizeMemoryMap();

    uint32_t guestPc = CpuBackendAccess::nextInstructionAddress(cpu);
    constexpr uint32_t MIN_ACTIVE_GUEST_INSTRUCTIONS = 3;
    auto activeBlockUseful = [](const R5900CompiledBlock *candidate)
    {
        return candidate &&
            (candidate->guestInstructions >= MIN_ACTIVE_GUEST_INSTRUCTIONS ||
             (candidate->internalSelfLoop &&
              candidate->guestInstructions >= 2));
    };

    // Do not even touch the block cache when this ARM7/task slot is too short
    // for the smallest useful active block. Two-instruction read-only
    // self-loops are special: generated code repeats them internally, so their
    // dispatch cost is amortized just like a longer block.
    if (cycleBudget < 2u)
    {
        DYNAREC_PROFILE(profile.activeBudgetFallbacks++; recordActiveFallback(profile, guestPc, R5900_ACTIVE_FALLBACK_BUDGET));
        return -2;
    }

    R5900CompiledBlock *block = findMutableBlock(guestPc, false);

    // Once a block has passed every live-execution gate, successful hot-path
    // dispatches no longer need the reject-table lookup or repeated
    // writes/R15/differential checks. Map remaps clear the block cache, so the
    // ready bit cannot outlive the code/memory mapping it validated against.
    if (!block || !block->activeReady)
    {
        ActiveRejectKind rejected = findActiveReject(guestPc);
        if (rejected != ACTIVE_REJECT_NONE)
        {
            if (rejected == ACTIVE_REJECT_WRITE)
                DYNAREC_PROFILE(profile.activeWriteFallbacks++; recordActiveFallback(profile, guestPc, R5900_ACTIVE_FALLBACK_WRITE));
            else if (rejected == ACTIVE_REJECT_STATE)
                DYNAREC_PROFILE(profile.activeStateFallbacks++; recordActiveFallback(profile, guestPc, R5900_ACTIVE_FALLBACK_STATE));
            else
                DYNAREC_PROFILE(profile.activeVerifyFallbacks++; recordActiveFallback(profile, guestPc, R5900_ACTIVE_FALLBACK_VERIFY));
            return ARM9_BLOCK_RUNNER_PERMANENT_REJECT;
        }
    }

    if (!activeBlockUseful(block))
    {
        // Keep enough room for realistic game code while still placing a hard
        // ceiling on metadata growth. The 2 MiB code cache remains the actual
        // generated-code capacity guard.
        if (blocks.size() >= 2048)
        {
            cacheActiveReject(guestPc, ACTIVE_REJECT_VERIFY);
            DYNAREC_PROFILE(profile.activeVerifyFallbacks++; recordActiveFallback(profile, guestPc, R5900_ACTIVE_FALLBACK_VERIFY));
            return ARM9_BLOCK_RUNNER_PERMANENT_REJECT;
        }

        DYNAREC_PROFILE(profile.activeCompileMisses++; recordActiveFallback(profile, guestPc, R5900_ACTIVE_FALLBACK_COMPILE));

        // Compilation length is intentionally independent from cycleBudget.
        // Active hot blocks may extend to 24 instructions while the code cache
        // has room; after 75% occupancy, fall back to the original 16-op size
        // so late hot-PC discovery cannot consume the remaining cache too fast.
        size_t activeCompileLimit =
            (cache.usedBytes() * 4u < cache.capacity() * 3u) ? 24u : 16u;
        int shadow = runArm9BlockShadowSelfTest(cpu, nullptr, nullptr, nullptr,
            activeCompileLimit);
        if (shadow <= 0)
        {
            cacheActiveReject(guestPc, ACTIVE_REJECT_VERIFY);
            DYNAREC_PROFILE(profile.activeVerifyFallbacks++; recordActiveFallback(profile, guestPc, R5900_ACTIVE_FALLBACK_VERIFY));
            return ARM9_BLOCK_RUNNER_PERMANENT_REJECT;
        }

        block = findMutableBlock(guestPc, false);
        if (!activeBlockUseful(block))
        {
            cacheActiveReject(guestPc, ACTIVE_REJECT_VERIFY);
            DYNAREC_PROFILE(profile.activeVerifyFallbacks++; recordActiveFallback(profile, guestPc, R5900_ACTIVE_FALLBACK_VERIFY));
            return ARM9_BLOCK_RUNNER_PERMANENT_REJECT;
        }
    }

    // Skip generated execution when even the block's exact minimum timing
    // cannot fit before the next ARM7/task deadline. The Core learns this
    // value from the negative return and avoids subsequent backend calls in
    // equally short slots.
    uint32_t minimumCycles = block->minCycles
        ? block->minCycles
        : block->guestInstructions;
    if (minimumCycles > cycleBudget)
    {
        DYNAREC_PROFILE(profile.activeBudgetFallbacks++; recordActiveFallback(profile, guestPc, R5900_ACTIVE_FALLBACK_BUDGET));
        return -static_cast<int>(minimumCycles);
    }

    if (!block->activeReady)
    {
        // Direct-memory stores are admitted only after the transactional
        // interpreter differential below has validated their exact journal.

        // Generated blocks that explicitly read/write architectural R15 are
        // kept out of live execution. Literal LDR has its own PC+8 helper.
        if ((block->readMask | block->writeMask) & 0x8000u)
        {
            cacheActiveReject(guestPc, ACTIVE_REJECT_STATE);
            DYNAREC_PROFILE(profile.activeStateFallbacks++; recordActiveFallback(profile, guestPc, R5900_ACTIVE_FALLBACK_STATE));
            return ARM9_BLOCK_RUNNER_PERMANENT_REJECT;
        }

        if (!block->differentialVerified)
        {
            int differential = runArm9InterpreterDifferentialSelfTest(cpu);
            if (differential <= 0)
            {
                cacheActiveReject(guestPc, ACTIVE_REJECT_VERIFY);
                DYNAREC_PROFILE(profile.activeVerifyFallbacks++; recordActiveFallback(profile, guestPc, R5900_ACTIVE_FALLBACK_VERIFY));
                return ARM9_BLOCK_RUNNER_PERMANENT_REJECT;
            }

            block = findMutableBlock(guestPc, false);
            if (!block || !block->differentialVerified)
            {
                cacheActiveReject(guestPc, ACTIVE_REJECT_VERIFY);
                DYNAREC_PROFILE(profile.activeVerifyFallbacks++; recordActiveFallback(profile, guestPc, R5900_ACTIVE_FALLBACK_VERIFY));
                return ARM9_BLOCK_RUNNER_PERMANENT_REJECT;
            }
        }

        block->activeReady = true;
    }

    // Initialize only the first block's live registers. When an already-safe
    // read-only successor is chained, lazily import any additional registers
    // it needs from the still-unmodified Interpreter state. Registers written
    // by earlier blocks are already resident in state.r and never reloaded.
    ArmAluState state;
    uint16_t initializedMask = block->inputMask;
    uint16_t unionWriteMask = static_cast<uint16_t>(block->writeMask & 0x7FFFu);
    for (uint8_t i = 0; i < block->inputRegCount; i++)
    {
        uint8_t reg = block->inputRegs[i];
        state.r[reg] = CpuBackendAccess::readReg(cpu, reg);
    }

    uint32_t liveCpsr = CpuBackendAccess::readCpsr(cpu);
    state.cpsr = liveCpsr;
    state.memory = memory;
    state.memoryFault = 0;
    state.writeCount = 0;
    state.cycles = 0;
    state.readCacheTag = block->cachedReadTag;
    state.readCachePage = block->cachedReadPage;
    state.writeCacheTag = block->cachedWriteTag;
    state.writeCachePage = block->cachedWritePage;
    state.cpuId = 0;
    state.cycleLimit = cycleBudget;
    state.guestExecuted = 0;

    ArmMemoryWriteGuard liveWrites(&state);
    uint32_t executedGuestInstructions = 0;
    uint32_t executedBlocks = 0;
    uint32_t totalCycles = 0;
    uint8_t unionCommitRegs[15] = {};
    uint8_t unionCommitRegCount = 0;
    bool buildingUnionCommit = false;
    R5900CompiledBlock *pipelineHint = nullptr;
    constexpr uint32_t WRITE_JOURNAL_CAPACITY =
        sizeof(state.writes) / sizeof(state.writes[0]);

    // Already-verified blocks may stay internal while the scheduler budget and
    // transactional write journal both have room. Direct RAM stores can chain
    // too: writes are visible to later ARM9 blocks exactly as in the
    // interpreter, but remain rollback-capable until the final CPU-state
    // commit. THUMB transitions still terminate immediately.
    // A one-store copy loop can consume the full 16-entry transactional
    // journal before returning to Core. Multi-store blocks are still bounded by
    // maxMemoryWrites below, and cycleBudget remains the hard scheduler fence.
    constexpr uint32_t MAX_INTERNAL_CHAIN_BLOCKS = 16;
    R5900CompiledBlock *current = block;

    auto writesOverlapBlockCode = [&](const R5900CompiledBlock *candidate)
    {
        if (!candidate || !state.writeCount || !memory)
            return false;

        uint64_t codeStart = candidate->guestPc;
        uint64_t codeEnd = candidate->guestEndPc;
        bool cpuId = state.cpuId != 0;

        for (uint32_t i = 0; i < state.writeCount; i++)
        {
            const ArmMemoryWrite &write = state.writes[i];
            uint64_t writeStart = write.address;
            uint64_t writeEnd = writeStart + write.size;

            // Fast guest-address overlap catches the normal non-aliased case.
            if (writeStart < codeEnd && writeEnd > codeStart)
                return true;

            // DS RAM regions can be mirrored. Compare the actual direct-page
            // bases and offsets too so a write through an alias cannot modify
            // the bytes of a compiled successor behind our back.
            uint8_t *writePage =
                memory->getDirectWritePage(cpuId, write.address);
            if (!writePage)
                continue;

            uint32_t codeAddress = candidate->guestPc;
            while (codeAddress < candidate->guestEndPc)
            {
                uint8_t *codePage =
                    memory->getDirectReadPage(cpuId, codeAddress);
                uint32_t pageBase = codeAddress & ~0xFFFu;
                uint32_t pageEnd = std::min<uint32_t>(
                    candidate->guestEndPc, pageBase + 0x1000u);

                if (codePage && codePage == writePage)
                {
                    uint32_t writeOffset = write.address & 0xFFFu;
                    uint32_t writeOffsetEnd = writeOffset + write.size;
                    uint32_t codeOffset = codeAddress & 0xFFFu;
                    uint32_t codeOffsetEnd = pageEnd - pageBase;
                    if (writeOffset < codeOffsetEnd &&
                        writeOffsetEnd > codeOffset)
                        return true;
                }

                if (pageEnd <= codeAddress)
                    break;
                codeAddress = pageEnd;
            }
        }
        return false;
    };

    while (current && executedBlocks < MAX_INTERNAL_CHAIN_BLOCKS)
    {
        uint32_t remainingBudget = cycleBudget - totalCycles;
        uint32_t writesBefore = state.writeCount;
        state.nextPc = current->guestEndPc;
        state.guestExecuted = 0;
        state.cycles = 0;
        state.cycleLimit = remainingBudget;

        typedef void (*ArmAluBlockFunction)(ArmAluState *);
        ArmAluBlockFunction function =
            reinterpret_cast<ArmAluBlockFunction>(current->hostCode);
        function(&state);
        uint32_t blockCycles = state.cycles;

        if (state.readCacheTag && state.readCachePage)
        {
            current->cachedReadTag = state.readCacheTag;
            current->cachedReadPage = state.readCachePage;
        }
        if (state.writeCacheTag && state.writeCachePage)
        {
            current->cachedWriteTag = state.writeCacheTag;
            current->cachedWritePage = state.writeCachePage;
        }

        uint32_t blockGuest = state.guestExecuted
            ? state.guestExecuted
            : current->guestInstructions;
        executedGuestInstructions += blockGuest;
        executedBlocks++;

        // No architectural CPU state has been committed yet. Any generated
        // failure discards the whole internal chain and the write guard rolls
        // memory back to the exact entry state. blockCycles is local to this
        // generated block; totalCycles tracks the complete internal chain.
        if (state.memoryFault || state.writeCount < writesBefore ||
            state.writeCount > WRITE_JOURNAL_CAPACITY ||
            ((state.cpsr ^ liveCpsr) & 0x1Fu) ||
            !blockCycles || blockCycles > remainingBudget)
        {
            if (blockCycles > remainingBudget)
                DYNAREC_PROFILE(profile.activeBudgetFallbacks++; recordActiveFallback(profile, guestPc, R5900_ACTIVE_FALLBACK_BUDGET));
            else
                DYNAREC_PROFILE(profile.activeStateFallbacks++; recordActiveFallback(profile, guestPc, R5900_ACTIVE_FALLBACK_STATE));
            // A conditional path can cost more than the block's static
            // minimum (for example a taken LDM/STM or branch). Do not teach
            // Core that this one observed path is the permanent minimum for
            // the hot PC; doing so starves later cheap condition-failed paths.
            // The negative return contract carries the compile-time minimum.
            return blockCycles > remainingBudget
                ? -static_cast<int>(minimumCycles)
                : 0;
        }

        totalCycles += blockCycles;

        uint16_t currentWriteMask = static_cast<uint16_t>(
            current->writeMask & 0x7FFFu);
        if (buildingUnionCommit)
        {
            uint16_t newWrites = static_cast<uint16_t>(
                currentWriteMask & ~unionWriteMask);
            for (uint8_t i = 0; i < current->writeRegCount; i++)
            {
                uint8_t reg = current->writeRegs[i];
                if (newWrites & static_cast<uint16_t>(1u << reg))
                    unionCommitRegs[unionCommitRegCount++] = reg;
            }
        }
        initializedMask |= currentWriteMask;
        unionWriteMask |= currentWriteMask;

        if (state.cpsr & BIT(5))
            break;

        // Stop before looking up/importing a successor when this dispatch has
        // already reached the internal chain ceiling.
        if (executedBlocks >= MAX_INTERNAL_CHAIN_BLOCKS)
            break;

        uint32_t remaining = cycleBudget - totalCycles;
        R5900CompiledBlock *next = findMutableBlock(state.nextPc, false);
        if (next && next->guestInstructions >= 2)
            pipelineHint = next;
        if (!next || !next->activeReady || !activeBlockUseful(next) ||
            !next->cycles || next->cycles > remaining)
            break;

        // A block that may store can only be chained when its worst-case
        // journal demand fits. Unknown write demand stays at the old safe
        // boundary. Also never execute compiled bytes after an earlier store
        // in this transaction has overwritten that guest-code range.
        if ((next->writesMemory &&
             (!next->maxMemoryWrites ||
              state.writeCount + next->maxMemoryWrites >
                  WRITE_JOURNAL_CAPACITY)) ||
            writesOverlapBlockCode(next))
            break;

        pipelineHint = nullptr;

        uint16_t nextInputMask = next->inputMask;
        for (uint8_t i = 0; i < next->inputRegCount; i++)
        {
            uint8_t reg = next->inputRegs[i];
            uint16_t bit = static_cast<uint16_t>(1u << reg);
            if (!(initializedMask & bit))
                state.r[reg] = CpuBackendAccess::readReg(cpu, reg);
        }
        initializedMask |= nextInputMask;
        if (!buildingUnionCommit)
        {
            for (uint8_t i = 0; i < block->writeRegCount; i++)
                unionCommitRegs[i] = block->writeRegs[i];
            unionCommitRegCount = block->writeRegCount;
            buildingUnionCommit = true;
        }
        if (state.writeCount > writesBefore)
            DYNAREC_PROFILE(profile.activeStoreChainBlocks++);
        current = next;
        DYNAREC_PROFILE(profile.activeInternalChainBlocks++);
    }

    if (!totalCycles)
    {
        DYNAREC_PROFILE(profile.activeStateFallbacks++; recordActiveFallback(profile, guestPc, R5900_ACTIVE_FALLBACK_STATE));
        return 0;
    }

    bool wroteMemory = state.writeCount != 0;
    bool invalidatesCompiledCode = false;
    if (wroteMemory)
    {
        for (uint32_t i = 0; i < state.writeCount; i++)
        {
            if (writeOverlapsCompiledCode(state.writes[i].address,
                state.writes[i].size))
            {
                invalidatesCompiledCode = true;
                break;
            }
        }
    }

    // The common single-block case reuses its compile-time sparse write list.
    // Multi-block chains build the union incrementally from each compact
    // writeRegs[] list, avoiding a final r0-r14 mask scan.
    const uint8_t *commitRegs = block->writeRegs;
    uint8_t commitRegCount = block->writeRegCount;
    if (executedBlocks > 1)
    {
        commitRegs = unionCommitRegs;
        commitRegCount = unionCommitRegCount;
    }

    bool committed = false;
    R5900CompiledBlock *pipelineBlock = nullptr;
    if (!(state.cpsr & BIT(5)))
    {
        pipelineBlock = pipelineHint;
        if (!pipelineBlock)
        {
            R5900CompiledBlock *next = findMutableBlock(state.nextPc, false);
            if (next && next->guestInstructions >= 2)
                pipelineBlock = next;
        }

        // Stores no longer force a blind pipeline refill. The transactional
        // journal records every direct write and writesOverlapBlockCode()
        // compares both guest ranges and aliased physical pages. Reuse compiled
        // entry opcodes only when those writes provably did not touch them.
        if (pipelineBlock && wroteMemory &&
            writesOverlapBlockCode(pipelineBlock))
            pipelineBlock = nullptr;
    }

    if (pipelineBlock)
    {
        committed = CpuBackendAccess::commitBlockStateSparseKnownArmPipeline(
            cpu, state.r, commitRegs, commitRegCount, state.cpsr,
            state.nextPc, pipelineBlock->entryOpcode0,
            pipelineBlock->entryOpcode1);
        if (committed)
            DYNAREC_PROFILE(profile.activePipelineReuses++);
    }
    else
    {
        committed = CpuBackendAccess::commitBlockStateSparse(cpu, state.r,
            commitRegs, commitRegCount, state.cpsr, state.nextPc);
    }

    if (!committed)
    {
        DYNAREC_PROFILE(profile.activeStateFallbacks++; recordActiveFallback(profile, guestPc, R5900_ACTIVE_FALLBACK_STATE));
        return 0;
    }

    liveWrites.dismiss();

    DYNAREC_PROFILE(profile.activeExecutions += executedBlocks);
    DYNAREC_PROFILE(profile.activeGuestInstructions += executedGuestInstructions);
    DYNAREC_PROFILE(profile.activeCycles += totalCycles);
    if (guestInstructions)
        *guestInstructions = executedGuestInstructions;

    int committedCycles = static_cast<int>(totalCycles);
    if (invalidatesCompiledCode)
    {
        DYNAREC_PROFILE(profile.activeCodeWriteInvalidations++);
        reset();
    }
    return committedCycles;
}
