#ifndef PS2_DYNAREC_R5900_H
#define PS2_DYNAREC_R5900_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "r5900_code_cache.h"

class Memory;

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

        // Infrastructure-only probe used by CI/runtime diagnostics. It emits
        // valid R5900 code but the scheduler does not execute dynarec blocks yet.
        bool emitProbe();

        size_t codeCacheUsed() const { return cache.usedBytes(); }
        size_t codeCacheCapacity() const { return cache.capacity(); }
        size_t blockCount() const { return blocks.size(); }

    private:
        Memory *memory;
        R5900CodeCache cache;
        std::vector<R5900CompiledBlock> blocks;
        uint32_t mappedGeneration = 0;
};

#endif
