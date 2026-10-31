#ifndef PS2_DYNAREC_R5900_H
#define PS2_DYNAREC_R5900_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "r5900_code_cache.h"

class Memory;
class Interpreter;

struct R5900CompiledBlock
{
    uint32_t guestPc = 0;
    uint32_t mapGeneration = 0;
    uint32_t cycles = 0;
    bool thumb = false;

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

        // Compile a contiguous real ARM9 ALU block into one R5900 function and
        // compare every register against a software shadow result. The live
        // emulated CPU remains untouched.
        int runArm9BlockShadowSelfTest(const Interpreter &cpu,
            uint32_t *guestPc = nullptr, uint32_t *instructionCount = nullptr,
            uint32_t *stateHash = nullptr);

        size_t codeCacheUsed() const { return cache.usedBytes(); }
        size_t codeCacheCapacity() const { return cache.capacity(); }
        size_t blockCount() const { return blocks.size(); }

    private:
        Memory *memory;
        R5900CodeCache cache;
        std::vector<R5900CompiledBlock> blocks;
        uint32_t mappedGeneration = 0;
        uint32_t *probeCode = nullptr;

        bool emitArmAluBlock(uint32_t opcode, uint32_t **entry, uint8_t *rd);
        bool emitArmAluSequence(const uint32_t *opcodes, size_t count,
            uint32_t guestPc, uint32_t **entry, uint32_t *hostWords);
};

#endif
