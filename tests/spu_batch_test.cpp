// The SPU runs several samples per scheduler task while nothing consumes them.
// This test checks that batching is invisible to the game: a core stepped one
// sample per 1024 cycles (the original behaviour, no catch-up) and a core driven
// by the batched task plus catch-up on every register access must show the same
// channel state, the same register reads and react identically to register
// writes that land between samples.
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#define private public
#include "../src/core.h"
#undef private
#include "../src/settings.h"

namespace
{
    uint64_t rngState = 0x9E3779B97F4A7C15ull;
    uint32_t rnd()
    {
        rngState ^= rngState << 13;
        rngState ^= rngState >> 7;
        rngState ^= rngState << 17;
        return static_cast<uint32_t>(rngState >> 16);
    }

    std::string fixture(int size)
    {
        char path[] = "/tmp/noods-spu-batch-XXXXXX";
        int fd = mkstemp(path);
        assert(fd >= 0 && ftruncate(fd, size) == 0 && close(fd) == 0);
        return path;
    }

    std::unique_ptr<Core> makeCore()
    {
        const std::string b9 = fixture(0x1000), b7 = fixture(0x4000), fw = fixture(0x40000);
        Settings::setBios9Path(b9); Settings::setBios7Path(b7); Settings::setFirmwarePath(fw);
        Settings::setThreaded2D(0); Settings::setThreaded3D(0);
        std::unique_ptr<Core> core(new Core());
        unlink(b9.c_str()); unlink(b7.c_str()); unlink(fw.c_str());
        core->interpreter[0].halt(7); core->interpreter[1].halt(7); // No guest code runs
        return core;
    }

    struct Config { uint32_t sad, cnt, len; uint16_t tmr, pnt; };

    Config randomConfig()
    {
        const uint32_t format = rnd() % 4;       // PCM8, PCM16, ADPCM, pulse/noise
        const uint32_t repeat = 1 + (rnd() % 2); // loop or one-shot
        Config c;
        c.sad = 0x02000000 + ((rnd() % 0x800) * 4);
        c.pnt = static_cast<uint16_t>(rnd() % 3);
        c.len = 2 + (rnd() % 60);
        c.tmr = static_cast<uint16_t>(0xF800 + (rnd() % 0x800));
        c.cnt = (rnd() & 0x7F) | ((rnd() & 3) << 8) | ((rnd() & 0x7F) << 16) |
            (repeat << 27) | (format << 29) | ((rnd() & 7) << 24);
        return c;
    }

    void configure(Core &core, const std::vector<uint8_t> &ram, const Config *cfg)
    {
        for (size_t i = 0; i < ram.size(); i++)
            core.memory.write<uint8_t>(1, 0x02000000 + static_cast<uint32_t>(i), ram[i]);
        core.spu.writeMainSoundCnt(0xFFFF, 0x807F);
        for (int ch = 0; ch < 16; ch++)
        {
            core.spu.writeSoundSad(ch, ~0u, cfg[ch].sad);
            core.spu.writeSoundTmr(ch, 0xFFFF, cfg[ch].tmr);
            core.spu.writeSoundPnt(ch, 0xFFFF, cfg[ch].pnt);
            core.spu.writeSoundLen(ch, ~0u, cfg[ch].len);
            core.spu.writeSoundCnt(ch, ~0u, cfg[ch].cnt | (1u << 31));
        }
    }

    void expectSameState(const Spu &a, const Spu &b)
    {
        assert(a.enabled == b.enabled);
        assert(!std::memcmp(a.soundCnt, b.soundCnt, sizeof(a.soundCnt)));
        assert(!std::memcmp(a.soundCurrent, b.soundCurrent, sizeof(a.soundCurrent)));
        assert(!std::memcmp(a.soundTimers, b.soundTimers, sizeof(a.soundTimers)));
        assert(!std::memcmp(a.soundSad, b.soundSad, sizeof(a.soundSad)));
        assert(!std::memcmp(a.soundTmr, b.soundTmr, sizeof(a.soundTmr)));
        assert(!std::memcmp(a.soundPnt, b.soundPnt, sizeof(a.soundPnt)));
        assert(!std::memcmp(a.soundLen, b.soundLen, sizeof(a.soundLen)));
        assert(!std::memcmp(a.adpcmValue, b.adpcmValue, sizeof(a.adpcmValue)));
        assert(!std::memcmp(a.adpcmLoopValue, b.adpcmLoopValue, sizeof(a.adpcmLoopValue)));
        assert(!std::memcmp(a.adpcmIndex, b.adpcmIndex, sizeof(a.adpcmIndex)));
        assert(!std::memcmp(a.adpcmLoopIndex, b.adpcmLoopIndex, sizeof(a.adpcmLoopIndex)));
        assert(!std::memcmp(a.adpcmToggle, b.adpcmToggle, sizeof(a.adpcmToggle)));
        assert(!std::memcmp(a.dutyCycles, b.dutyCycles, sizeof(a.dutyCycles)));
        assert(!std::memcmp(a.noiseValues, b.noiseValues, sizeof(a.noiseValues)));
        assert(a.mainSoundCnt == b.mainSoundCnt);
        assert(a.soundBias == b.soundBias);
    }

    // Applies one random SPU register access to both cores and compares reads
    void randomAccess(Core &ref, Core &bat)
    {
        const int ch = rnd() % 16;
        switch (rnd() % 8)
        {
            case 0: { const Config c = randomConfig();
                const uint32_t v = c.cnt | ((rnd() & 1) << 31);
                ref.spu.writeSoundCnt(ch, ~0u, v);
                bat.spu.writeSoundCnt(ch, ~0u, v); break; }
            case 1: { const Config c = randomConfig();
                ref.spu.writeSoundSad(ch, ~0u, c.sad); bat.spu.writeSoundSad(ch, ~0u, c.sad); break; }
            case 2: { const Config c = randomConfig();
                ref.spu.writeSoundTmr(ch, 0xFFFF, c.tmr); bat.spu.writeSoundTmr(ch, 0xFFFF, c.tmr); break; }
            case 3: { const Config c = randomConfig();
                ref.spu.writeSoundPnt(ch, 0xFFFF, c.pnt); bat.spu.writeSoundPnt(ch, 0xFFFF, c.pnt);
                ref.spu.writeSoundLen(ch, ~0u, c.len); bat.spu.writeSoundLen(ch, ~0u, c.len); break; }
            case 4: { const uint16_t v = static_cast<uint16_t>((rnd() & 0x7F) | ((rnd() % 5 ? 1u : 0u) << 15));
                ref.spu.writeMainSoundCnt(0xFFFF, v); bat.spu.writeMainSoundCnt(0xFFFF, v); break; }
            case 5: { const uint16_t v = static_cast<uint16_t>(rnd());
                ref.spu.writeSoundBias(0xFFFF, v); bat.spu.writeSoundBias(0xFFFF, v); break; }
            default: // Reads: the busy flags a game polls
                assert(ref.spu.readSoundCnt(ch) == bat.spu.readSoundCnt(ch));
                break;
        }
    }
}

int main()
{
    const int kRounds = 10, kSamples = 3000;
    long registerAccesses = 0, batchedTasks = 0;

    for (int round = 0; round < kRounds; round++)
    {
        std::vector<uint8_t> ram(0x4000);
        for (size_t i = 0; i < ram.size(); i++) ram[i] = static_cast<uint8_t>(rnd());
        Config cfg[16];
        for (int ch = 0; ch < 16; ch++) cfg[ch] = randomConfig();

        std::unique_ptr<Core> refCore = makeCore(); // One sample per 1024 cycles, no catch-up
        std::unique_ptr<Core> batCore = makeCore(); // Batched task + catch-up
        configure(*refCore, ram, cfg);
        configure(*batCore, ram, cfg);
        refCore->spu.lastSampleCycles = INT64_C(1) << 60; // Disables the catch-up hooks

        int64_t lastTaskDelay = 0;
        for (int k = 1; k <= kSamples; k++)
        {
            const uint32_t t = static_cast<uint32_t>(k) * Spu::SAMPLE_CYCLES;

            // Register accesses that land between samples
            while (rnd() % 100 < 35)
            {
                const uint32_t tt = t - 1 - (rnd() % (Spu::SAMPLE_CYCLES - 1)); // Strictly inside (t-1024, t)
                refCore->globalCycles = tt;
                batCore->globalCycles = tt;
                randomAccess(*refCore, *batCore);
                registerAccesses++;
            }

            // The sample itself
            refCore->globalCycles = t;
            batCore->globalCycles = t;
            refCore->spu.stepSample();
            if (k % NOODS_SPU_BATCH_SAMPLES == 0)
            {
                batCore->spu.runSample(); // The scheduler task
                batchedTasks++;

                // It must have rescheduled itself one batch ahead
                int64_t latest = 0;
                batCore->tasks.forEachPending([&](Task &task)
                {
                    if (task.task == &batCore->spu.runSampleTask)
                        latest = std::max<int64_t>(latest, task.cycles);
                });
                lastTaskDelay = latest - t;
                assert(lastTaskDelay == static_cast<int64_t>(Spu::SAMPLE_CYCLES) * NOODS_SPU_BATCH_SAMPLES);
            }

            if (k % 64 == 0)
            {
                batCore->spu.catchUp();
                expectSameState(refCore->spu, batCore->spu);
            }
        }
        batCore->spu.catchUp();
        expectSameState(refCore->spu, batCore->spu);
    }

    // With an audio buffer the task goes back to one sample per run
    {
        std::unique_ptr<Core> core = makeCore();
        core->spu.bufferIn = new uint32_t[1 << 16];
        core->spu.bufferOut = new uint32_t[1 << 16];
        core->spu.bufferSize = 1 << 16;
        core->globalCycles = Spu::SAMPLE_CYCLES;
        core->spu.runSample();
        int64_t latest = 0;
        core->tasks.forEachPending([&](Task &task)
        {
            if (task.task == &core->spu.runSampleTask)
                latest = std::max<int64_t>(latest, task.cycles);
        });
        assert(latest - Spu::SAMPLE_CYCLES == Spu::SAMPLE_CYCLES);
        assert(core->spu.bufferPointer == 1); // and it mixed that sample
    }

    // The core rebases its cycle counter now and then; the SPU must follow
    {
        std::unique_ptr<Core> core = makeCore();
        core->globalCycles = 100 * Spu::SAMPLE_CYCLES;
        core->spu.catchUp();
        assert(core->spu.lastSampleCycles == 100 * Spu::SAMPLE_CYCLES);
        core->spu.resetCycles(core->globalCycles);
        core->globalCycles = 0;
        assert(core->spu.lastSampleCycles == 0);
        core->globalCycles = 3 * Spu::SAMPLE_CYCLES + 5;
        core->spu.catchUp();
        assert(core->spu.lastSampleCycles == 3 * Spu::SAMPLE_CYCLES);
    }

    printf("spu_batch_test: %d rounds x %d samples identical (batch=%d, %ld register accesses, %ld tasks instead of %d)\n",
        kRounds, kSamples, NOODS_SPU_BATCH_SAMPLES, registerAccesses, batchedTasks, kRounds * kSamples);
    return 0;
}
