#ifndef PS2_R5900_CODE_CACHE_H
#define PS2_R5900_CODE_CACHE_H

#include <cstddef>
#include <cstdint>

class R5900CodeCache
{
    public:
        R5900CodeCache() = default;
        ~R5900CodeCache();

        R5900CodeCache(const R5900CodeCache&) = delete;
        R5900CodeCache &operator=(const R5900CodeCache&) = delete;

        bool init(size_t sizeBytes);
        void shutdown();
        void reset();

        uint32_t *reserveWords(size_t wordCount);
        void commitWords(size_t wordCount);
        void syncExecutable(uint32_t *start, uint32_t *end);

        uint8_t *base() const { return memory; }
        uint8_t *current() const { return memory ? memory + used : nullptr; }
        size_t capacity() const { return size; }
        size_t usedBytes() const { return used; }
        size_t freeBytes() const { return size - used; }
        bool ready() const { return memory != nullptr; }

    private:
        uint8_t *memory = nullptr;
        size_t size = 0;
        size_t used = 0;
};

#endif
