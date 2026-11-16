// (Drives Spu::stepSample() directly: one call = one 32768Hz sample.)
// The SPU skips the PCM fetch and the volume/pan mixing while nothing consumes
// the mixed samples (no audio buffer, no sound capture). The channel state
// machines must still advance bit-for-bit like the full mixing path, because
// games observe them (busy flags, one-shot ends, loops, ADPCM state).
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <queue>
#include <string>
#include <vector>
#include <unistd.h>

#define private public
#include "../src/spu.h"
#undef private
#include "../src/core.h"
#include "../src/settings.h"

namespace
{
    uint64_t rngState = 0xD1B54A32D192ED03ull;
    uint32_t rnd()
    {
        rngState ^= rngState << 13;
        rngState ^= rngState >> 7;
        rngState ^= rngState << 17;
        return static_cast<uint32_t>(rngState >> 16);
    }

    std::string fixture(int size)
    {
        char path[] = "/tmp/noods-spu-state-XXXXXX";
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

    struct ChannelConfig
    {
        uint32_t sad, cnt, len;
        uint16_t tmr, pnt;
    };

    // Same random configuration for both cores
    void configure(Core &core, const std::vector<uint8_t> &ram, const ChannelConfig *cfg)
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
        assert(!std::memcmp(a.adpcmValue, b.adpcmValue, sizeof(a.adpcmValue)));
        assert(!std::memcmp(a.adpcmLoopValue, b.adpcmLoopValue, sizeof(a.adpcmLoopValue)));
        assert(!std::memcmp(a.adpcmIndex, b.adpcmIndex, sizeof(a.adpcmIndex)));
        assert(!std::memcmp(a.adpcmLoopIndex, b.adpcmLoopIndex, sizeof(a.adpcmLoopIndex)));
        assert(!std::memcmp(a.adpcmToggle, b.adpcmToggle, sizeof(a.adpcmToggle)));
        assert(!std::memcmp(a.dutyCycles, b.dutyCycles, sizeof(a.dutyCycles)));
        assert(!std::memcmp(a.noiseValues, b.noiseValues, sizeof(a.noiseValues)));
        assert(!std::memcmp(a.sndCapCnt, b.sndCapCnt, sizeof(a.sndCapCnt)));
    }
}

int main()
{
    const int kSamples = 4000;
    const int kRounds = 12;
    int oneShotsEnded = 0, loopsRunning = 0;

    for (int round = 0; round < kRounds; round++)
    {
        std::vector<uint8_t> ram(0x4000);
        for (size_t i = 0; i < ram.size(); i++) ram[i] = static_cast<uint8_t>(rnd());

        ChannelConfig cfg[16];
        for (int ch = 0; ch < 16; ch++)
        {
            const uint32_t format = rnd() % 4;       // PCM8, PCM16, ADPCM, pulse/noise
            const uint32_t repeat = 1 + (rnd() % 2); // loop or one-shot
            cfg[ch].sad = 0x02000000 + ((rnd() % 0x800) * 4);
            cfg[ch].pnt = static_cast<uint16_t>(rnd() % 3);
            cfg[ch].len = 2 + (rnd() % 60);          // short, so one-shots end
            cfg[ch].tmr = static_cast<uint16_t>(0xF800 + (rnd() % 0x800)); // 1-2048 cycle periods
            cfg[ch].cnt = (rnd() & 0x7F) | ((rnd() & 3) << 8) | ((rnd() & 0x7F) << 16) |
                (repeat << 27) | (format << 29) | ((rnd() & 7) << 24);
        }

        std::unique_ptr<Core> fast = makeCore(); // no audio buffer: state-only path
        std::unique_ptr<Core> full = makeCore(); // audio buffer present: full mixing
        configure(*fast, ram, cfg);
        configure(*full, ram, cfg);

        const int bufferSize = 1 << 20; // Larger than the run, so no swap/wait happens
        full->spu.bufferIn = new uint32_t[bufferSize];
        full->spu.bufferOut = new uint32_t[bufferSize];
        full->spu.bufferSize = bufferSize;
        assert(fast->spu.bufferSize == 0);
        expectSameState(fast->spu, full->spu);

        for (int sample = 0; sample < kSamples; sample++)
        {
            fast->spu.stepSample();
            full->spu.stepSample();
            if ((sample & 63) == 63)
                expectSameState(fast->spu, full->spu);
        }
        expectSameState(fast->spu, full->spu);
        assert(full->spu.bufferPointer == kSamples); // The full path really produced output

        for (int ch = 0; ch < 16; ch++)
        {
            if (!(fast->spu.soundCnt[ch] & (1u << 31)) && ((cfg[ch].cnt >> 29) & 3) != 3 &&
                ((cfg[ch].cnt >> 27) & 3) == 2)
                oneShotsEnded++;
            if (fast->spu.enabled & (1u << ch))
                loopsRunning++;
        }
    }

    // The test would be vacuous if nothing ever ended or kept playing
    assert(oneShotsEnded > 0);
    assert(loopsRunning > 0);
    printf("spu_state_test: %d rounds x %d samples identical (one-shots ended: %d, channels still running: %d)\n",
        kRounds, kSamples, oneShotsEnded, loopsRunning);
    return 0;
}
