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

#ifndef CARTRIDGE_H
#define CARTRIDGE_H

#include <cstdint>
#include <string>

class Core;

enum RomIoCaller : uint8_t
{
    ROM_IO_HEADER = 0,
    ROM_IO_DIRECT_ARM9,
    ROM_IO_DIRECT_ARM7,
    ROM_IO_CACHE_MISS,
    ROM_IO_FIRMWARE,
    ROM_IO_BIOS,
    ROM_IO_RUNTIME,
    ROM_IO_OTHER,
    ROM_IO_CALLER_COUNT
};

#ifdef NOODS_PROFILE
struct RomSlowRead
{
    RomIoCaller caller = ROM_IO_OTHER;
    int64_t offset = -1;
    uint64_t requested = 0;
    uint64_t got = 0;
    uint64_t seekTicks = 0;
    uint64_t readTicks = 0;
    bool seekNeeded = false;
    bool cacheMiss = false;
    bool sequential = false;
};

struct RomIoProfile
{
    uint64_t seekCalls = 0;
    uint64_t seekTicks = 0;
    uint64_t maxSeekTicks = 0;
    uint64_t readCalls = 0;
    uint64_t readTicks = 0;
    uint64_t maxReadTicks = 0;
    uint64_t requestedBytes = 0;
    uint64_t readBytes = 0;
    uint64_t shortReads = 0;
    uint64_t latencyHistogram[7] = {};
    uint64_t sizeHistogram[5] = {};
    uint64_t callerReads[ROM_IO_CALLER_COUNT] = {};
    uint64_t callerTicks[ROM_IO_CALLER_COUNT] = {};
    RomSlowRead slowReads[8] = {};
    uint8_t slowReadCount = 0;
};
#endif

enum NdsCmdMode
{
    CMD_NONE = 0,
    CMD_HEADER,
    CMD_CHIP,
    CMD_SECURE,
    CMD_DATA
};

class Cartridge
{
    public:
        Cartridge(Core *core): core(core) {}
        ~Cartridge();

        virtual void loadRom(std::string path);
        void writeSave();

        void trimRom();
        void resizeSave(int newSize, bool dirty = true);

        int getRomSize()  { return romSize;  }
        int getSaveSize() { return saveSize; }
        const std::string &getRomPath() const { return romName; }

        uint32_t getRomCacheHits() const   { return romCacheHits;   }
        uint32_t getRomCacheMisses() const { return romCacheMisses; }
        uint32_t getRomCacheBytes() const  { return romCacheBytes;  }
        uint64_t getRomCacheReadBytes() const { return romCacheReadBytes; }
        uint32_t getRomCacheSeekCount() const { return romCacheSeekCount; }
        uint32_t getRomCacheSequentialMisses() const { return romCacheSequentialMisses; }
        uint32_t getRomCacheSequentialRunMax() const { return romCacheSequentialRunMax; }
        uint32_t getRomCacheReadAheadReads() const { return romCacheReadAheadReads; }
        uint32_t getRomCacheReadAheadPages() const { return romCacheReadAheadPages; }

#ifdef NOODS_PROFILE
        const RomIoProfile &getRomIoProfile() const { return romIoProfile; }
        void resetRomIoProfile() { romIoProfile = RomIoProfile(); }
#endif

    protected:
        Core *core;

        FILE *romFile = nullptr;
        uint8_t *rom = nullptr, *save = nullptr;
        int romSize = 0, saveSize = 0;
        long romFilePosition = -1;
        int romCapacity = 0;
        bool saveDirty = false;

        struct RomCacheEntry
        {
            uint32_t tag = 0;
            uint32_t stamp = 0;
            bool valid = false;
        };

        uint8_t *romCache = nullptr;
        uint8_t *romReadAhead = nullptr;
        RomCacheEntry *romCacheEntries = nullptr;
        uint32_t romCachePageSize = 0x4000;
        uint32_t romCachePageCount = 0;
        uint32_t romCacheClock = 0;
        uint32_t romCacheHits = 0;
        uint32_t romCacheMisses = 0;
        uint32_t romCacheBytes = 0;

        uint64_t romCacheReadBytes = 0;
        uint32_t romCacheSeekCount = 0;
        uint32_t romCacheSequentialMisses = 0;
        uint32_t romCacheSequentialRun = 0;
        uint32_t romCacheSequentialRunMax = 0;
        uint32_t romCacheLastAccessTag = 0xFFFFFFFFu;
        uint32_t romCacheReadAheadReads = 0;
        uint32_t romCacheReadAheadPages = 0;

#ifdef NOODS_PROFILE
        RomIoProfile romIoProfile;
#endif

        void configureRomCache(uint32_t bytes);
        void loadRomSection(uint32_t offset, uint32_t size, bool useCache = true,
            RomIoCaller caller = ROM_IO_CACHE_MISS);
        int seekRomFile(long offset, int origin);
        size_t readRomFile(void *buffer, size_t size, RomIoCaller caller,
            bool seekNeeded, uint64_t seekTicks, bool cacheMiss);
        uint32_t chooseRomCacheVictim(int excluded = -1) const;

    private:
        std::string romName, saveName;
};

class CartridgeNds: public Cartridge
{
    public:
        CartridgeNds(Core *core): Cartridge(core) {}

        void loadRom(std::string path);
        void directBoot();

        uint16_t readAuxSpiCnt(bool cpu)  { return auxSpiCnt[cpu];  }
        uint8_t  readAuxSpiData(bool cpu) { return auxSpiData[cpu]; }
        uint32_t readRomCtrl(bool cpu)    { return romCtrl[cpu];    }
        uint32_t readRomDataIn(bool cpu);

        void writeAuxSpiCnt(bool cpu, uint16_t mask, uint16_t value);
        void writeAuxSpiData(bool cpu, uint8_t value);
        void writeRomCtrl(bool cpu, uint32_t mask, uint32_t value);
        void writeRomCmdOutL(bool cpu, uint32_t mask, uint32_t value);
        void writeRomCmdOutH(bool cpu, uint32_t mask, uint32_t value);

    private:
        uint32_t romCode = 0;
        bool romEncrypted = false;
        NdsCmdMode cmdMode = CMD_NONE;

        uint32_t encTable[0x412] = {};
        uint32_t encCode[3] = {};

        uint32_t romAddrReal[2] = {}, romAddrVirt[2] = {};
        uint16_t blockSize[2] = {}, readCount[2] = {};
        bool encrypted[2] = {};

        uint8_t auxCommand[2] = {};
        uint32_t auxAddress[2] = {};
        int auxWriteCount[2] = {};

        uint16_t auxSpiCnt[2] = {};
        uint8_t auxSpiData[2] = {};
        uint32_t romCtrl[2] = {};
        uint64_t romCmdOut[2] = {};

        uint64_t encrypt64(uint64_t value);
        uint64_t decrypt64(uint64_t value);
        void initKeycode(int level);
        void applyKeycode();
};

class CartridgeGba: public Cartridge
{
    public:
        CartridgeGba(Core *core): Cartridge(core) {}

        void loadRom(std::string path);

        uint8_t *getRom() { return rom; }

        bool isEeprom(uint32_t address)
        {
            return (saveSize == -1 || saveSize == 0x200 || saveSize == 0x2000) && (romSize <= 0x1000000 || address >= 0x0DFFFF00);
        }

        uint8_t eepromRead();
        void eepromWrite(uint8_t value);

        uint8_t sramRead(uint32_t address);
        void sramWrite(uint32_t address, uint8_t value);

    private:
        int eepromCount = 0;
        uint16_t eepromCmd = 0;
        uint64_t eepromData = 0;
        bool eepromDone = false;

        uint8_t flashCmd = 0;
        bool bankSwap = false;
        bool flashErase = false;
};

#endif // CARTRIDGE_H
