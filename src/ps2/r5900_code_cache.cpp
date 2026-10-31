#include <cstdlib>
#include <malloc.h>
#include <kernel.h>

#include "r5900_code_cache.h"

R5900CodeCache::~R5900CodeCache()
{
    shutdown();
}

bool R5900CodeCache::init(size_t sizeBytes)
{
    shutdown();

    // EE cache lines are 64 bytes. Keep the code cache aligned so generated
    // blocks can be synchronized without touching unrelated allocations.
    sizeBytes = (sizeBytes + 63) & ~size_t(63);
    memory = static_cast<uint8_t*>(memalign(64, sizeBytes));
    if (!memory)
        return false;

    size = sizeBytes;
    used = 0;
    return true;
}

void R5900CodeCache::shutdown()
{
    if (memory)
        free(memory);

    memory = nullptr;
    size = 0;
    used = 0;
}

void R5900CodeCache::reset()
{
    used = 0;
}

uint32_t *R5900CodeCache::reserveWords(size_t wordCount)
{
    size_t bytes = wordCount * sizeof(uint32_t);
    bytes = (bytes + 3) & ~size_t(3);

    if (!memory || used + bytes > size)
        return nullptr;

    return reinterpret_cast<uint32_t*>(memory + used);
}

void R5900CodeCache::commitWords(size_t wordCount)
{
    used += wordCount * sizeof(uint32_t);
    used = (used + 3) & ~size_t(3);
}

void R5900CodeCache::syncExecutable(uint32_t *start, uint32_t *end)
{
    if (!start || !end || end <= start)
        return;

    // Generated instructions are first written through the EE data cache.
    // Write them back, then invalidate the instruction cache before execution.
    SyncDCache(start, end);
    FlushCache(INVALIDATE_ICACHE);
}
