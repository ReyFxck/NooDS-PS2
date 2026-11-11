#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>

#include "../src/core.h"
#include "../src/settings.h"

static std::string fixture(int size)
{
    char path[] = "/tmp/noods-cart-dma-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0 && ftruncate(fd, size) == 0 && close(fd) == 0);
    return path;
}

static std::unique_ptr<Core> makeCore()
{
    std::string b9 = fixture(0x1000), b7 = fixture(0x4000), fw = fixture(0x40000);
    Settings::setBios9Path(b9); Settings::setBios7Path(b7); Settings::setFirmwarePath(fw);
    Settings::setThreaded2D(0); Settings::setThreaded3D(0);
    std::unique_ptr<Core> core(new Core());
    unlink(b9.c_str()); unlink(b7.c_str()); unlink(fw.c_str());
    // Bit 7 is independent of IRQ wakeup's bit 0. No BIOS/guest opcode runs.
    core->interpreter[0].halt(7); core->interpreter[1].halt(7);
    core->setProfileEnabled(true);
    return core;
}

static unsigned scheduled(Core &core, bool cpu, int mode, unsigned channels)
{
    const uint64_t before = core.getRunProfile().scheduleCalls;
    core.dma[cpu].trigger(mode, channels);
    return core.getRunProfile().scheduleCalls - before;
}

static void startHeader(Core &core, bool cpu)
{
    core.cartridgeNds.writeRomCmdOutL(cpu, ~0u, 0);
    core.cartridgeNds.writeRomCmdOutH(cpu, ~0u, 0);
    core.cartridgeNds.writeRomCtrl(cpu, ~0u, 0x81000000u); // 512 bytes
}

int main()
{
    auto core = makeCore();
    std::string rom = fixture(0x10000);
    std::vector<uint8_t> bytes(0x10000);
    for (unsigned i = 0; i < bytes.size(); i++) bytes[i] = (i * 37 + (i >> 8)) & 255;
    FILE *file = fopen(rom.c_str(), "wb"); assert(file);
    assert(fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size()); fclose(file);
    core->cartridgeNds.loadRom(rom);
    for (int cpu = 0; cpu < 2; cpu++)
    {
        core->cartridgeNds.writeAuxSpiCnt(cpu, 0xffff, 1u << 14);
        core->interpreter[cpu].writeIrf(~0u, ~0u);
        startHeader(*core, cpu);
        const uint32_t ctrl = core->cartridgeNds.readRomCtrl(cpu);
        assert((ctrl & 0x80800000u) == 0x80800000u);
        for (int offset = 0; offset < 4; offset++)
        {
            assert(core->memory.read<uint8_t>(cpu, 0x40001A4 + offset) == uint8_t(ctrl >> (8 * offset)));
            assert(core->memory.read<uint32_t>(cpu, 0x40001A4 + offset) == ctrl);
        }
        for (unsigned i = 0; i < 128; i++)
        {
            uint32_t expected = uint32_t(bytes[i * 4]) | (uint32_t(bytes[i * 4 + 1]) << 8) |
                (uint32_t(bytes[i * 4 + 2]) << 16) | (uint32_t(bytes[i * 4 + 3]) << 24);
            assert(core->memory.read<uint32_t>(cpu, 0x4100010 + (i & 3)) == expected);
            assert(bool(core->cartridgeNds.readRomCtrl(cpu) & (1u << 23)) == (i != 127));
            assert(bool(core->interpreter[cpu].readIrf() & (1u << 19)) == (i == 127));
        }
        assert(!(core->cartridgeNds.readRomCtrl(cpu) & (1u << 31)));
        assert(core->memory.read<uint32_t>(cpu, 0x4100010) == 0);
        startHeader(*core, cpu);
        assert(core->memory.read<uint8_t>(cpu, 0x4100011) == bytes[1]);
        assert(core->memory.read<uint16_t>(cpu, 0x4100012) == (bytes[6] | (bytes[7] << 8)));
        assert(core->memory.read<uint32_t>(cpu, 0x4100010) ==
            (uint32_t(bytes[8]) | (uint32_t(bytes[9]) << 8) | (uint32_t(bytes[10]) << 16) | (uint32_t(bytes[11]) << 24)));
        // Drain before changing CPUs/commands: partial reads consumed one word each.
        for (int i = 3; i < 128; i++) core->memory.read<uint32_t>(cpu, 0x4100010);
    }
    unlink(rom.c_str());
    std::remove((rom + ".sav").c_str());

    if (std::getenv("NOODS_CART_BENCH"))
    {
        core->setProfileEnabled(false);
        volatile uint32_t sum = 0;
        auto begin = std::chrono::steady_clock::now();
        for (int block = 0; block < 20000; block++)
        {
            // CHIP-ID needs no file I/O. Keep the exact ready/end/DMA paths.
            core->cartridgeNds.writeRomCmdOutL(0, ~0u, 0x90);
            core->cartridgeNds.writeRomCtrl(0, ~0u, 0x81000000u);
            for (int i = 0; i < 128; i++)
            {
                sum += core->memory.read<uint32_t>(0, 0x40001A4);
                sum += core->memory.read<uint32_t>(0, 0x4100010);
            }
        }
        printf("cart ms=%.3f sum=%x\n", std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count(), unsigned(sum));
        begin = std::chrono::steady_clock::now();
        for (int i = 0; i < 2560000; i++) core->dma[0].trigger(5);
        printf("dma-disabled ms=%.3f\n", std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count());
        begin = std::chrono::steady_clock::now();
        for (int i = 0; i < 2560000; i++) sum += core->memory.read<uint32_t>(0, 0x4000130);
        printf("input-mmio ms=%.3f sum=%x\n", std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count(), unsigned(sum));
        return 0;
    }

    // Oracle uses readable register state, independent of the cached masks.
    uint32_t rng = 0x12345678;
    for (int cpu = 0; cpu < 2; cpu++)
    for (int step = 0; step < 80; step++)
    {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        int channel = rng & 3;
        uint32_t value = (rng & 0xf8000000u); // zero word count, no repeat
        const uint32_t mask = step & 1 ? 0xf8000000u : 0x38000000u;
        core->dma[cpu].writeDmaCnt(channel, mask, value);
        for (int mode = 0; mode < 8; mode++)
        {
            const unsigned channels = (rng >> 4) & 15;
            unsigned expected = 0;
            for (int ch = 0; ch < 4; ch++)
            {
                uint32_t ctrl = core->dma[cpu].readDmaCnt(ch);
                if ((channels & (1u << ch)) && (ctrl & (1u << 31)) &&
                    ((ctrl >> 27) & 7) == unsigned(cpu ? mode * 2 : mode)) expected++;
            }
            assert(scheduled(*core, cpu, mode, channels) == expected);
        }
    }

    // Completion, repeat and live mode/enable changes on real scheduled DMA.
    core = makeCore();
    for (int cpu = 0; cpu < 2; cpu++)
    {
        Dma &dma = core->dma[cpu];
        const int event = cpu ? 2 : 3;
        const unsigned encoded = cpu ? event * 2 : event;
        core->memory.write<uint32_t>(cpu, 0x2000000, 0x89abcdef);
        dma.writeDmaSad(0, ~0u, 0x2000000);
        dma.writeDmaDad(0, ~0u, 0x2001000);
        dma.writeDmaCnt(0, ~0u, 0x84000001u | (encoded << 27));
        assert(scheduled(*core, cpu, event, 1) == 1);
        core->runFrame();
        assert(!core->interpreter[0].shouldRun() && !core->interpreter[1].shouldRun());
        assert(core->memory.read<uint32_t>(cpu, 0x2001000) == 0x89abcdef);
        assert(!(dma.readDmaCnt(0) & (1u << 31)));
        assert(scheduled(*core, cpu, event, 1) == 0);
        dma.writeDmaCnt(0, ~0u, 0x86000001u | (encoded << 27));
        assert(scheduled(*core, cpu, event, 1) == 1);
        core->runFrame();
        assert(dma.readDmaCnt(0) & (1u << 31));
        assert(scheduled(*core, cpu, event, 1) == 1);
        dma.writeDmaCnt(0, 1u << 31, 0);
        assert(scheduled(*core, cpu, event, 1) == 0);
    }

    // Equal-cycle events retain channel order and repeated triggers stay queued.
    core = makeCore();
    for (int cpu = 0; cpu < 2; cpu++)
    {
        Dma &dma = core->dma[cpu];
        const unsigned event = cpu ? 2 : 3, encoded = cpu ? event * 2 : event;
        for (unsigned i = 0; i < 4; i++)
            core->memory.write<uint32_t>(cpu, 0x2000000 + i * 4, 0x11111111u * (i + 1));
        for (int ch = 0; ch < 2; ch++)
        {
            dma.writeDmaSad(ch, ~0u, 0x2000000 + ch * 8);
            dma.writeDmaDad(ch, ~0u, 0x2001000);
            dma.writeDmaCnt(ch, ~0u, 0xc6400001u | (encoded << 27)); // repeat, fixed dest, IRQ
        }
        assert(scheduled(*core, cpu, event, 3) == 2);
        assert(scheduled(*core, cpu, event, 3) == 2);
        core->runFrame();
        assert(core->memory.read<uint32_t>(cpu, 0x2001000) == 0x44444444);
        assert((core->interpreter[cpu].readIrf() & 0x300) == 0x300);
        for (int ch = 0; ch < 2; ch++) dma.writeDmaCnt(ch, 1u << 31, 0);
    }

    // A DMA can write its own control register. Completion must clear the new
    // mode's cache entry, even though this transfer began under a different one.
    core = makeCore();
    core->memory.write<uint32_t>(0, 0x2000000, 0xa4000001u); // enabled, word, mode 4
    core->dma[0].writeDmaSad(0, ~0u, 0x2000000);
    core->dma[0].writeDmaDad(0, ~0u, 0x40000b8);
    core->dma[0].writeDmaCnt(0, ~0u, 0x9c000001u); // enabled, word, mode 3
    assert(scheduled(*core, 0, 3, 1) == 1);
    core->runFrame();
    assert(!(core->dma[0].readDmaCnt(0) & (1u << 31)));
    assert(scheduled(*core, 0, 3, 1) == 0);
    assert(scheduled(*core, 0, 4, 1) == 0);
}
