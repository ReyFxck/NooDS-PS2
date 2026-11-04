#ifndef PS2_DYNAREC_R5900_H
#define PS2_DYNAREC_R5900_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "arm_alu.h"
#include "r5900_code_cache.h"

constexpr size_t R5900_ANALYSIS_MAX_INSTRUCTIONS = 32;

struct R5900AnalyzedInstruction
{
    uint32_t pc = 0;
    uint32_t opcode = 0;
    Ps2ArmAlu::InstructionInfo info;
    Ps2ArmAlu::RejectReason reason = Ps2ArmAlu::REJECT_OTHER;
};

struct R5900BlockAnalysis
{
    uint32_t startPc = 0;
    uint32_t endPc = 0;
    uint32_t branchTarget = 0xFFFFFFFFu;
    uint16_t readMask = 0;
    uint16_t writeMask = 0;
    uint8_t instructionCount = 0;
    uint8_t supportedInstructions = 0;
    uint8_t firstUnsupported = 0xFFu;
    bool backEdge = false;
    bool writesMemory = false;
    R5900AnalyzedInstruction instructions[R5900_ANALYSIS_MAX_INSTRUCTIONS] = {};
};

struct R5900FallbackProfile
{
    uint32_t pc = 0;
    uint32_t opcode = 0;
    uint32_t count = 0;
};

struct R5900DynarecProfile
{
    uint64_t compileAttempts = 0;
    uint64_t compileSuccess = 0;
    uint64_t compileRejected = 0;
    uint64_t lookupHits = 0;
    uint64_t lookupMisses = 0;
    uint64_t guestInstructions = 0;
    uint64_t supportedInstructions = 0;
    uint64_t fallbackCount = 0;
    uint64_t mapInvalidations = 0;

    // Guarded live-execution counters. These are separate from shadow compile
    // coverage so PROFILE builds can show whether the JIT actually replaced
    // interpreter work in the scheduler.
    uint64_t activeAttempts = 0;
    uint64_t activeExecutions = 0;
    uint64_t activeGuestInstructions = 0;
    uint64_t activeCycles = 0;
    uint64_t activeCompileMisses = 0;
    uint64_t activeBudgetFallbacks = 0;
    uint64_t activeWriteFallbacks = 0;
    uint64_t activeVerifyFallbacks = 0;
    uint64_t activeStateFallbacks = 0;

    uint64_t rejectReason[Ps2ArmAlu::REJECT_REASON_COUNT] = {};
    uint32_t largestBlock = 0;
    R5900FallbackProfile topFallback[16] = {};
};

class Memory;
class Interpreter;

struct R5900CompiledBlock
{
    uint32_t guestPc = 0;
    uint32_t guestEndPc = 0;
    uint32_t mapGeneration = 0;
    uint32_t guestInstructions = 0;
    uint32_t cycles = 0;
    uint32_t entryOpcode0 = 0;
    uint32_t entryOpcode1 = 0;
    uint16_t readMask = 0;
    uint16_t writeMask = 0;
    uint8_t liveRegCount = 0;
    uint8_t writeRegCount = 0;
    uint8_t liveRegs[15] = {};
    uint8_t writeRegs[15] = {};
    bool readsFlags = false;
    bool writesFlags = false;
    bool writesMemory = false;
    bool thumb = false;
    bool differentialVerified = false;
    bool activeReady = false;

    uint32_t *hostCode = nullptr;
    uint32_t hostWords = 0;
};

class DynarecR5900
{
    public:
        explicit DynarecR5900(Memory *memory);

        bool init(size_t cacheBytes = 2 * 1024 * 1024);
        void shutdown();
        void reset();

        // Called before block lookup. Remaps of TCM/WRAM/VRAM make direct
        // pointers embedded in old code unsafe, so they invalidate all blocks.
        bool synchronizeMemoryMap();

        const R5900CompiledBlock *findBlock(uint32_t pc, bool thumb) const;

        // Infrastructure probe used by CI/runtime diagnostics. It emits and
        // executes a tiny leaf function without touching emulated CPU state.
        bool emitProbe();
        bool runProbe(uint32_t *value = nullptr) const;

        // Decode a small, deterministic ARM data-processing subset, emit
        // equivalent R5900 code and execute it against isolated register state.
        // This is the correctness gate before wiring blocks into the scheduler.
        bool runArmAluSelfTest();

        // Repeat the same ALU gate using a snapshot of the live ARM9 register
        // bank. The emulated CPU itself is never modified by this test.
        bool runArm9StateSelfTest(const Interpreter &cpu, uint32_t *stateHash = nullptr);

        // Try the exact next live ARM9 opcode without mutating it. Returns 1
        // when validated, 0 when it is outside the current subset, and -1 on a
        // mismatch. Only pipeline[0] is used so register state is exact.
        int runArm9PipelineSelfTest(const Interpreter &cpu,
            uint32_t *guestPc = nullptr, uint32_t *opcode = nullptr);

        // Compile a contiguous real ARM9 ALU block, optionally ending in B,
        // into one R5900 function and compare registers/next PC against a
        // software shadow result. The live emulated CPU remains untouched.
        int runArm9BlockShadowSelfTest(const Interpreter &cpu,
            uint32_t *guestPc = nullptr, uint32_t *instructionCount = nullptr,
            uint32_t *stateHash = nullptr, size_t maxInstructions = 16);

        // Differential gate against NooDS' own interpreter. ALU/read-only
        // blocks with an optional terminating B are eligible; store blocks are
        // kept in the transactional shadow gate until interpreter writes can
        // also be journaled and restored exactly.
        int runArm9InterpreterDifferentialSelfTest(Interpreter &cpu,
            uint32_t *guestPc = nullptr, uint32_t *instructionCount = nullptr,
            uint32_t *stateHash = nullptr);

        // Execute one already-safe ARM9 block against live state. On a cache
        // miss the candidate is shadow-validated and differentially checked
        // once before it can commit. Store blocks and blocks that do not fit
        // entirely inside cycleBudget fall back to the interpreter.
        int runArm9ActiveBlock(Interpreter &cpu, uint32_t cycleBudget,
            uint32_t *guestInstructions = nullptr);

        // PROFILE/shadow analysis only. Reads opcodes exclusively through the
        // ARM9 direct map and never executes or mutates guest state.
        bool analyzeArmBlock(uint32_t pc, R5900BlockAnalysis *analysis,
            size_t maxInstructions = R5900_ANALYSIS_MAX_INSTRUCTIONS);
        bool isDirectArmHalfwordAccess(uint32_t opcode, uint32_t address) const;
        bool isDirectArmWordAccess(uint32_t opcode, uint32_t address) const;
        void resetProfile() { profile = R5900DynarecProfile(); }
        const R5900DynarecProfile &getProfile() const { return profile; }

        size_t codeCacheUsed() const { return cache.usedBytes(); }
        size_t codeCacheCapacity() const { return cache.capacity(); }
        size_t blockCount() const { return blocks.size(); }

    private:
        static constexpr size_t ACTIVE_CACHE_SLOTS = 2048;
        enum ActiveRejectKind : uint8_t
        {
            ACTIVE_REJECT_NONE = 0,
            ACTIVE_REJECT_VERIFY,
            ACTIVE_REJECT_WRITE,
            ACTIVE_REJECT_STATE
        };

        Memory *memory;
        R5900CodeCache cache;
        std::vector<R5900CompiledBlock> blocks;

        // Direct-mapped live lookup caches. Collisions are harmless: at worst
        // a PC is revalidated once. This avoids linear scans on every ARM9
        // scheduler step after the active cache starts filling.
        uint32_t activeBlockPc[ACTIVE_CACHE_SLOTS] = {};
        uint16_t activeBlockIndex[ACTIVE_CACHE_SLOTS] = {};
        uint32_t activeRejectedPc[ACTIVE_CACHE_SLOTS] = {};
        uint8_t activeRejectedKind[ACTIVE_CACHE_SLOTS] = {};

        uint32_t mappedGeneration = 0;
        uint32_t *probeCode = nullptr;
        R5900DynarecProfile profile;

        size_t activeLookupSlot(uint32_t pc) const
        {
            return ((pc >> 2) ^ (pc >> 13)) & (ACTIVE_CACHE_SLOTS - 1);
        }
        void clearActiveLookups();
        void cacheActiveReject(uint32_t pc, ActiveRejectKind kind);
        ActiveRejectKind findActiveReject(uint32_t pc) const;
        R5900CompiledBlock *findMutableBlock(uint32_t pc, bool thumb);
        bool emitArmAluBlock(uint32_t opcode, uint32_t **entry, uint8_t *rd);
        bool emitArmAluSequence(const uint32_t *opcodes, size_t count,
            uint32_t guestPc, uint32_t **entry, uint32_t *hostWords);
};

#endif
