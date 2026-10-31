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

        uint32_t getMapGeneration() const { return mapGeneration; }

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

        uint32_t mapGeneration = 1;

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
    // Align the address
    address &= ~(sizeof(T) - 1);

    const SparsePageTable &readMap = (cpu == 0) ? readMap9 : readMap7;
    if (uint8_t *page = readMap.get(address))
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
    // Align the address
    address &= ~(sizeof(T) - 1);

    const SparsePageTable &writeMap = (cpu == 0) ? writeMap9 : writeMap7;
    if (uint8_t *page = writeMap.get(address))
    {
        // Get a pointer to writable memory mapped to the given address
        uint8_t *data = &page[address & 0xFFF];

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
