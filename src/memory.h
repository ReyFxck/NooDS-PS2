/*
    Copyright 2019-2021 Hydr8gon

    This file is part of NooDS.

    NooDS is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    NooDS is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with NooDS. If not, see <https://www.gnu.org/licenses/>.
*/

#ifndef MEMORY_H
#define MEMORY_H

#include <cstdint>
#include <cstddef>
#include <cstring>

#include "defines.h"

class Core;

#ifdef NOODS_PROFILE
struct MemoryHotMmio
{
    uint32_t address = 0;
    uint32_t samples = 0;
    uint32_t reads = 0;
    uint32_t writes = 0;
};

struct MemoryCpuProfile
{
    uint64_t sampledReads = 0;
    uint64_t sampledWrites = 0;
    uint64_t directReads = 0;
    uint64_t directWrites = 0;
    uint64_t pageCacheHits = 0;
    uint64_t fallbackReads = 0;
    uint64_t fallbackWrites = 0;
    uint64_t mmioReads = 0;
    uint64_t mmioWrites = 0;
    uint64_t vramReads = 0;
    uint64_t vramWrites = 0;
    uint64_t paletteReads = 0;
    uint64_t paletteWrites = 0;
    uint64_t tcmReads = 0;
    uint64_t tcmWrites = 0;
    uint64_t unalignedReads = 0;
    uint64_t unalignedWrites = 0;
    uint64_t crossPageReads = 0;
    uint64_t crossPageWrites = 0;
    MemoryHotMmio hotMmio[8] = {};
};

struct MemoryProfile
{
    MemoryCpuProfile cpu[2];
};
#endif

class SparsePageTable
{
    public:
        SparsePageTable() = default;
        ~SparsePageTable()
        {
            for (size_t i = 0; i < 0x1000; i++)
                delete level1[i];
        }

        SparsePageTable(const SparsePageTable&) = delete;
        SparsePageTable &operator=(const SparsePageTable&) = delete;

        FORCE_INLINE uint8_t *get(uint32_t address) const
        {
            Level2 *table = level1[address >> 20];
            return table ? table->pages[(address >> 12) & 0xFF] : nullptr;
        }

        bool set(uint32_t address, uint8_t *data)
        {
            uint32_t l1 = address >> 20;
            uint32_t l2 = (address >> 12) & 0xFF;

            Level2 *table = level1[l1];
            if (!table)
            {
                if (!data)
                    return false;

                table = new Level2();
                level1[l1] = table;
                allocatedTables++;
            }

            if (table->pages[l2] == data)
                return false;

            table->pages[l2] = data;
            return true;
        }

        size_t allocatedBytes() const
        {
            return sizeof(level1) + allocatedTables * sizeof(Level2);
        }

    private:
        struct Level2
        {
            uint8_t *pages[0x100] = {};
        };

        Level2 *level1[0x1000] = {};
        size_t allocatedTables = 0;
};

#ifdef NOODS_PS2
// The PS2 frontend runs the core single-threaded. A tiny direct-mapped cache
// keeps hot code/data pages from repeating both sparse-table pointer loads on
// every interpreter fetch or transfer. Mapping updates invalidate all entries
// alongside mapGeneration changes. Null pages are cached too, preserving the
// normal MMIO/overlap fallback semantics.
class MemoryPageCache
{
    public:
        FORCE_INLINE bool lookup(uint32_t address, uint8_t *&page) const
        {
            const Entry &entry = entries[(address >> 12) & 3u];
            if (entry.tag != (address >> 12) + 1u)
                return false;
            page = entry.page;
            return true;
        }

        FORCE_INLINE void store(uint32_t address, uint8_t *page)
        {
            Entry &entry = entries[(address >> 12) & 3u];
            entry.tag = (address >> 12) + 1u;
            entry.page = page;
        }

        void invalidate()
        {
            for (size_t i = 0; i < 4; i++)
                entries[i].tag = 0;
        }

    private:
        struct Entry
        {
            uint32_t tag = 0;
            uint8_t *page = nullptr;
        };
        Entry entries[4];
};

struct MemoryWriteJournalEntry
{
    uint32_t address = 0;
    uint32_t before = 0;
    uint32_t after = 0;
    uint8_t size = 0;
};

struct MemoryWriteJournal
{
    static constexpr size_t CAPACITY = 32;
    MemoryWriteJournalEntry entries[CAPACITY] = {};
    size_t count = 0;
    bool overflow = false;
    bool cpu = false;
};
#endif

class VramMapping
{
    public:
        void add(uint8_t *mapping);

        template <typename T> T read(uint32_t address);
        template <typename T> void write(uint32_t address, T value);

        uint8_t *getBaseMapping() { return mappings[0]; }
        int      getCount()       { return count;       }

    private:
        uint8_t *mappings[7];
        int count = 0;
};

class Memory
{
    public:
        Memory(Core *core): core(core) {};

        void loadBios();
        void loadGbaBios();

        void updateMap7(uint32_t start, uint32_t end);
        void updateMap9(uint32_t start, uint32_t end);

        template <typename T> T read(bool cpu, uint32_t address);
        template <typename T> void write(bool cpu, uint32_t address, T value);

        uint8_t  *getPalette()    { return palette;    }
        uint8_t  *getOam()        { return oam;        }
        uint8_t **getEngAExtPal() { return engAExtPal; }
        uint8_t **getEngBExtPal() { return engBExtPal; }
        uint8_t **getTex3D()      { return tex3D;      }
        uint8_t **getPal3D()      { return pal3D;      }

        // Direct-page helpers for CPU backends/dynarecs. A null result means
        // the access must use the normal fallback path.
        FORCE_INLINE uint8_t *getDirectReadPage(bool cpu, uint32_t address) const
        {
            return (cpu == 0 ? readMap9 : readMap7).get(address);
        }

        FORCE_INLINE uint8_t *getDirectWritePage(bool cpu, uint32_t address) const
        {
            return (cpu == 0 ? writeMap9 : writeMap7).get(address);
        }

        // PS2 backend helpers hit the same RAM/TCM pages repeatedly. Reuse the
        // tiny interpreter page cache instead of walking both sparse-table
        // levels for every generated load/fetch. Mapping changes already
        // invalidate these caches together with the normal read/write path.
        FORCE_INLINE uint8_t *getDirectReadPageCached(bool cpu, uint32_t address)
        {
#ifdef NOODS_PS2
            uint8_t *page = nullptr;
            MemoryPageCache &cache = readPageCache[cpu ? 1 : 0];
            if (!cache.lookup(address, page))
            {
                page = (cpu == 0 ? readMap9 : readMap7).get(address);
                cache.store(address, page);
            }
            return page;
#else
            return getDirectReadPage(cpu, address);
#endif
        }

        FORCE_INLINE uint8_t *getDirectWritePageCached(bool cpu, uint32_t address)
        {
#ifdef NOODS_PS2
            uint8_t *page = nullptr;
            MemoryPageCache &cache = writePageCache[cpu ? 1 : 0];
            if (!cache.lookup(address, page))
            {
                page = (cpu == 0 ? writeMap9 : writeMap7).get(address);
                cache.store(address, page);
            }
            return page;
#else
            return getDirectWritePage(cpu, address);
#endif
        }

        uint32_t getMapGeneration() const { return mapGeneration; }

#ifdef NOODS_PS2
        void beginWriteJournal(MemoryWriteJournal *journal, bool cpu);
        void endWriteJournal(MemoryWriteJournal *journal);
        void rollbackWriteJournal(MemoryWriteJournal &journal);
#endif

#ifdef NOODS_PROFILE
        class ProfileCpuScope
        {
            public:
                ProfileCpuScope(Memory &memory, bool cpu):
                    memory(memory), previous(memory.profileCpuContext)
                {
                    memory.profileCpuContext = cpu ? 1 : 0;
                }
                ~ProfileCpuScope() { memory.profileCpuContext = previous; }

                ProfileCpuScope(const ProfileCpuScope&) = delete;
                ProfileCpuScope &operator=(const ProfileCpuScope&) = delete;

            private:
                Memory &memory;
                int8_t previous;
        };

        void setProfileEnabled(bool enabled) { profileEnabled = enabled; }
        void resetProfile()
        {
            profile = MemoryProfile();
            profileSampleCounter[0] = profileSampleCounter[1] = 0;
        }
        const MemoryProfile &getProfile() const { return profile; }
#endif

        size_t getPageTableBytes() const
        {
            return readMap9.allocatedBytes() + readMap7.allocatedBytes() +
                   writeMap9.allocatedBytes() + writeMap7.allocatedBytes();
        }

    private:
        Core *core;

        // 32-bit address space, still split into 4KB blocks, but represented
        // by sparse two-level tables. This keeps the exact fast-map semantics
        // without reserving four million page pointers inside Core.
        SparsePageTable readMap9;
        SparsePageTable readMap7;
        SparsePageTable writeMap9;
        SparsePageTable writeMap7;

#ifdef NOODS_PS2
        MemoryPageCache readPageCache[2];
        MemoryPageCache writePageCache[2];
        MemoryWriteJournal *activeWriteJournal = nullptr;

        void invalidatePageCaches()
        {
            for (int cpu = 0; cpu < 2; cpu++)
            {
                readPageCache[cpu].invalidate();
                writePageCache[cpu].invalidate();
            }
        }
#endif

        uint32_t mapGeneration = 1;

#ifdef NOODS_PROFILE
        bool profileEnabled = false;
        int8_t profileCpuContext = -1;
        uint32_t profileSampleCounter[2] = {};
        MemoryProfile profile;

        FORCE_INLINE void profileAccess(bool cpu, uint32_t rawAddress,
            size_t size, bool write, bool direct, bool pageCacheHit)
        {
            (void)cpu;
            if (!profileEnabled || profileCpuContext < 0)
                return;

            uint8_t cpuIndex = static_cast<uint8_t>(profileCpuContext);
            if ((++profileSampleCounter[cpuIndex] & 63u) != 0)
                return;

            MemoryCpuProfile &entry = profile.cpu[cpuIndex];
            if (pageCacheHit)
                entry.pageCacheHits++;
            uint32_t address = rawAddress & ~static_cast<uint32_t>(size - 1u);
            bool unaligned = (rawAddress & (size - 1u)) != 0;
            bool crossPage = (rawAddress & 0xFFFu) + size > 0x1000u;

            if (write)
            {
                entry.sampledWrites++;
                direct ? entry.directWrites++ : entry.fallbackWrites++;
                if (unaligned) entry.unalignedWrites++;
                if (crossPage) entry.crossPageWrites++;
            }
            else
            {
                entry.sampledReads++;
                direct ? entry.directReads++ : entry.fallbackReads++;
                if (unaligned) entry.unalignedReads++;
                if (crossPage) entry.crossPageReads++;
            }

            switch (address >> 24)
            {
                case 0x00:
                    // Low memory is ARM9 ITCM/DTCM space; on ARM7 the same
                    // region contains BIOS/WRAM mappings and is not TCM.
                    if (cpuIndex == 0)
                        write ? entry.tcmWrites++ : entry.tcmReads++;
                    break;
                case 0x04:
                {
                    write ? entry.mmioWrites++ : entry.mmioReads++;
                    MemoryHotMmio &hot = entry.hotMmio[(address >> 2) & 7u];
                    uint32_t wordAddress = address & ~3u;
                    if (hot.samples && hot.address != wordAddress)
                    {
                        hot.samples--;
                        break;
                    }
                    if (!hot.samples)
                    {
                        hot.address = wordAddress;
                        hot.reads = hot.writes = 0;
                    }
                    hot.samples++;
                    write ? hot.writes++ : hot.reads++;
                    break;
                }
                case 0x05:
                    write ? entry.paletteWrites++ : entry.paletteReads++;
                    break;
                case 0x06:
                    write ? entry.vramWrites++ : entry.vramReads++;
                    break;
            }
        }
#endif

        uint8_t bios9[0x8000]   = {}; // 32KB ARM9 BIOS
        uint8_t bios7[0x4000]   = {}; // 16KB ARM7 BIOS
        uint8_t gbaBios[0x4000] = {}; // 16KB GBA BIOS

        uint8_t ram[0x400000]    = {}; //  4MB main RAM
        uint8_t wram[0x8000]     = {}; // 32KB shared WRAM
        uint8_t instrTcm[0x8000] = {}; // 32KB instruction TCM
        uint8_t dataTcm[0x4000]  = {}; // 16KB data TCM
        uint8_t wram7[0x10000]   = {}; // 64KB ARM7 WRAM
        uint8_t wifiRam[0x2000]  = {}; //  8KB WiFi RAM

        uint8_t palette[0x800] = {}; //   2KB palette
        uint8_t vramA[0x20000] = {}; // 128KB VRAM block A
        uint8_t vramB[0x20000] = {}; // 128KB VRAM block B
        uint8_t vramC[0x20000] = {}; // 128KB VRAM block C
        uint8_t vramD[0x20000] = {}; // 128KB VRAM block D
        uint8_t vramE[0x10000] = {}; //  64KB VRAM block E
        uint8_t vramF[0x4000]  = {}; //  16KB VRAM block F
        uint8_t vramG[0x4000]  = {}; //  16KB VRAM block G
        uint8_t vramH[0x8000]  = {}; //  32KB VRAM block H
        uint8_t vramI[0x4000]  = {}; //  16KB VRAM block I
        uint8_t oam[0x800]     = {}; //   2KB OAM

        VramMapping engABg[32];
        VramMapping engBBg[8];
        VramMapping engAObj[16];
        VramMapping engBObj[8];
        VramMapping lcdc[64];
        VramMapping vram7[2];

        uint8_t *engAExtPal[5] = {};
        uint8_t *engBExtPal[5] = {};
        uint8_t *tex3D[4]      = {};
        uint8_t *pal3D[6]      = {};

        uint32_t dmaFill[4] = {};
        uint8_t vramCnt[9] = {};
        uint8_t vramStat = 0;
        uint8_t wramCnt = 0;
        uint8_t haltCnt = 0;
        uint16_t rcnt = 0;

        template <typename T> T readFallback(bool cpu, uint32_t address);
        template <typename T> void writeFallback(bool cpu, uint32_t address, T value);

        template <typename T> T ioRead9(uint32_t address);
        template <typename T> T ioRead7(uint32_t address);
        template <typename T> T ioReadGba(uint32_t address);
        template <typename T> void ioWrite9(uint32_t address, T value);
        template <typename T> void ioWrite7(uint32_t address, T value);
        template <typename T> void ioWriteGba(uint32_t address, T value);

        uint32_t readDmaFill(int channel) { return dmaFill[channel]; }
        uint8_t  readVramCnt(int block)   { return vramCnt[block];   }
        uint8_t  readVramStat()           { return vramStat;         }
        uint8_t  readWramCnt()            { return wramCnt;          }
        uint8_t  readHaltCnt()            { return haltCnt;          }
        uint16_t readRcnt()                { return rcnt;             }

        void writeRcnt(uint16_t mask, uint16_t value)
        {
            // RCNT writable bits: 0-8 and 14-15. Bits 9-13 read as zero.
            mask &= 0xC1FF;
            rcnt = (rcnt & ~mask) | (value & mask);
        }

        void writeDmaFill(int channel, uint32_t mask, uint32_t value);
        void writeVramCnt(int index, uint8_t value);
        void writeWramCnt(uint8_t value);
        void writeHaltCnt(uint8_t value);
        void writeGbaHaltCnt(uint8_t value);
};

template uint8_t  Memory::read(bool cpu, uint32_t address);
template uint16_t Memory::read(bool cpu, uint32_t address);
template uint32_t Memory::read(bool cpu, uint32_t address);
template <typename T> FORCE_INLINE T Memory::read(bool cpu, uint32_t address)
{
#ifdef NOODS_PROFILE
    uint32_t rawAddress = address;
#endif
    // Align the address
    address &= ~(sizeof(T) - 1);

#ifdef NOODS_PS2
    // Palette and OAM are only 2 KiB each, so they intentionally stay out of
    // the 4 KiB sparse page tables. ARM9 software (including common copy
    // loops) hits them frequently, though, and routing every access through
    // readFallback() adds an avoidable function/switch/byte-assembly cost.
    //
    // Preserve the fallback path's exact mirroring and aligned little-endian
    // semantics, but handle these two side-effect-free regions inline.
    if (cpu == 0)
    {
        uint8_t *special = nullptr;
        switch (address & 0xFF000000u)
        {
            case 0x05000000u: special = &palette[address & 0x7FFu]; break;
            case 0x07000000u: special = &oam[address & 0x7FFu];     break;
        }

        if (special)
        {
#ifdef NOODS_PROFILE
            profileAccess(cpu, rawAddress, sizeof(T), false, true, false);
#endif
            T value = 0;
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
            std::memcpy(&value, special, sizeof(T));
#else
            for (size_t i = 0; i < sizeof(T); i++)
                value |= static_cast<T>(special[i]) << (i * 8);
#endif
            return value;
        }
    }
#endif

    const SparsePageTable &readMap = (cpu == 0) ? readMap9 : readMap7;
    uint8_t *page = nullptr;
    bool pageCacheHit = false;
#ifdef NOODS_PS2
    MemoryPageCache &pageCache = readPageCache[cpu ? 1 : 0];
    pageCacheHit = pageCache.lookup(address, page);
    if (!pageCacheHit)
    {
        page = readMap.get(address);
        pageCache.store(address, page);
    }
#else
    page = readMap.get(address);
#endif
#ifdef NOODS_PROFILE
    profileAccess(cpu, rawAddress, sizeof(T), false, page != nullptr,
        pageCacheHit);
#else
    (void)pageCacheHit;
#endif
    if (page)
    {
        // Get a pointer to readable memory mapped to the given address
        uint8_t *data = &page[address & 0xFFF];

        // Direct-mapped pages are naturally aligned by the caller. On the
        // overwhelmingly common little-endian hosts (including PS2 EE), memcpy
        // becomes one native load instead of four byte loads/shifts for u32.
        T value = 0;
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
        std::memcpy(&value, data, sizeof(T));
#else
        for (size_t i = 0; i < sizeof(T); i++)
            value |= static_cast<T>(data[i]) << (i * 8);
#endif
        return value;
    }

    return readFallback<T>(cpu, address);
}

template void Memory::write(bool cpu, uint32_t address, uint8_t  value);
template void Memory::write(bool cpu, uint32_t address, uint16_t value);
template void Memory::write(bool cpu, uint32_t address, uint32_t value);
template <typename T> FORCE_INLINE void Memory::write(bool cpu, uint32_t address, T value)
{
#ifdef NOODS_PROFILE
    uint32_t rawAddress = address;
#endif
    // Align the address
    address &= ~(sizeof(T) - 1);

#ifdef NOODS_PS2
    // Match the read fast path above. Palette/OAM writes have no MMIO side
    // effects in this core; writeFallback() simply writes the mirrored bytes.
    if (cpu == 0)
    {
        uint8_t *special = nullptr;
        switch (address & 0xFF000000u)
        {
            case 0x05000000u: special = &palette[address & 0x7FFu]; break;
            case 0x07000000u: special = &oam[address & 0x7FFu];     break;
        }

        if (special)
        {
#ifdef NOODS_PROFILE
            profileAccess(cpu, rawAddress, sizeof(T), true, true, false);
#endif
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
            std::memcpy(special, &value, sizeof(T));
#else
            for (size_t i = 0; i < sizeof(T); i++)
                special[i] = value >> (i * 8);
#endif
            return;
        }
    }
#endif

    const SparsePageTable &writeMap = (cpu == 0) ? writeMap9 : writeMap7;
    uint8_t *page = nullptr;
    bool pageCacheHit = false;
#ifdef NOODS_PS2
    MemoryPageCache &pageCache = writePageCache[cpu ? 1 : 0];
    pageCacheHit = pageCache.lookup(address, page);
    if (!pageCacheHit)
    {
        page = writeMap.get(address);
        pageCache.store(address, page);
    }
#else
    page = writeMap.get(address);
#endif
#ifdef NOODS_PROFILE
    profileAccess(cpu, rawAddress, sizeof(T), true, page != nullptr,
        pageCacheHit);
#else
    (void)pageCacheHit;
#endif
    if (page)
    {
        // Get a pointer to writable memory mapped to the given address
        uint8_t *data = &page[address & 0xFFF];

#ifdef NOODS_PS2
        if (activeWriteJournal && activeWriteJournal->cpu == cpu)
        {
            if (activeWriteJournal->count >= MemoryWriteJournal::CAPACITY)
            {
                activeWriteJournal->overflow = true;
                return;
            }

            MemoryWriteJournalEntry &entry =
                activeWriteJournal->entries[activeWriteJournal->count++];
            entry.address = address;
            entry.before = 0;
            for (size_t i = 0; i < sizeof(T); i++)
                entry.before |= static_cast<uint32_t>(data[i]) << (i * 8);
            entry.after = static_cast<uint32_t>(value);
            entry.size = static_cast<uint8_t>(sizeof(T));
        }
#endif

        // Same fast path as reads: little-endian hosts can store the aligned
        // value directly while big-endian hosts keep the explicit LSB order.
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
        std::memcpy(data, &value, sizeof(T));
#else
        for (size_t i = 0; i < sizeof(T); i++)
            data[i] = value >> (i * 8);
#endif
        return;
    }

    return writeFallback<T>(cpu, address, value);
}

#endif // MEMORY_H
