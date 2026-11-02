#include <cassert>
#include <cstdint>

#include "../src/memory.h"

int main()
{
    uint8_t pages[5][0x1000] = {};
    MemoryPageCache cache;
    uint8_t *page = nullptr;

    assert(!cache.lookup(0x02000004u, page));
    cache.store(0x02000004u, pages[0]);
    assert(cache.lookup(0x02000FFCu, page) && page == pages[0]);
    cache.invalidate();
    assert(!cache.lookup(0x02000004u, page)); // mapGeneration invalidation path

    cache.store(0x04000000u, nullptr);
    page = pages[1];
    assert(cache.lookup(0x04000ABCu, page) && page == nullptr);

    // Four differently indexed pages coexist, while a fifth with the same
    // direct-map index replaces only its colliding entry.
    for (uint32_t i = 0; i < 4; i++)
        cache.store(0x03000000u + i * 0x1000u, pages[i]);
    for (uint32_t i = 0; i < 4; i++)
    {
        assert(cache.lookup(0x03000080u + i * 0x1000u, page));
        assert(page == pages[i]);
    }

    cache.store(0x03004000u, pages[4]);
    assert(cache.lookup(0x03004020u, page) && page == pages[4]);
    assert(!cache.lookup(0x03000020u, page));
    assert(cache.lookup(0x03001020u, page) && page == pages[1]);
    return 0;
}
