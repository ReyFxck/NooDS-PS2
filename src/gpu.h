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

#ifndef GPU_H
#define GPU_H

#include <atomic>
#include <cstdint>
#include <ctime>
#include <functional>
#include <thread>
#include <mutex>
#include <queue>

#include "defines.h"

class Core;

#ifdef NOODS_PROFILE
enum GpuProfilePhase : uint8_t
{
    GPU_PROFILE_2D_A = 0,
    GPU_PROFILE_2D_B,
    GPU_PROFILE_CAPTURE,
    GPU_PROFILE_3D_RENDER,
    GPU_PROFILE_VBLANK,
    GPU_PROFILE_PHASE_COUNT
};

struct GpuRunProfile
{
    uint64_t phaseCount[GPU_PROFILE_PHASE_COUNT] = {};
    uint64_t phaseTicks[GPU_PROFILE_PHASE_COUNT] = {};
    uint64_t phaseSamples[GPU_PROFILE_PHASE_COUNT] = {};
    uint32_t phaseSampleState[GPU_PROFILE_PHASE_COUNT] = {};
};
#endif

class Gpu
{
    public:
        Gpu(Core *core);
        ~Gpu();

        void scheduleInit();
        void gbaScheduleInit();

        bool getFrame(uint32_t *out, bool gbaCrop);
#ifdef NOODS_PS2
        // Native GS CT16 output for the PS2 frontend. The queued emulated
        // framebuffer is still kept in full precision for capture and effects.
        bool getFrame16(uint16_t *out);
#endif

        void invalidate3D() { dirty3D |= BIT(0); }

        uint16_t readDispStat(bool cpu) { return dispStat[cpu]; }
        uint16_t readVCount()           { return vCount;        }
        uint32_t readDispCapCnt()       { return dispCapCnt;    }
        uint16_t readPowCnt1()          { return powCnt1;       }

        void writeDispStat(bool cpu, uint16_t mask, uint16_t value);
        void writeDispCapCnt(uint32_t mask, uint32_t value);
        void writePowCnt1(uint16_t mask, uint16_t value);

#ifdef NOODS_PROFILE
        void setProfileEnabled(bool enabled) { profileEnabled = enabled; }
        void resetProfile() { runProfile = GpuRunProfile(); }
        const GpuRunProfile &getProfile() const { return runProfile; }
#endif

    private:
        Core *core;

        void recycleFrame();

        std::queue<uint32_t*> framebuffers;
        std::queue<uint32_t*> recycledFramebuffers;
        std::atomic<bool> ready;
        std::mutex mutex;

        bool running = false;
        std::atomic<int> drawing;
        std::thread *thread = nullptr;

        bool gbaBlock = true;
        bool displayCapture = false;
        uint8_t dirty3D = 0;

        uint16_t dispStat[2] = {};
        uint16_t vCount = 0;
        uint32_t dispCapCnt = 0;
        uint16_t powCnt1 = 0;

        std::function<void()> gbaScanline240Task;
        std::function<void()> gbaScanline308Task;
        std::function<void()> scanline256Task;
        std::function<void()> scanline355Task;

#ifdef NOODS_PROFILE
        bool profileEnabled = false;
        GpuRunProfile runProfile;

        bool beginProfilePhase(GpuProfilePhase phase, clock_t &start);
        void endProfilePhase(GpuProfilePhase phase, clock_t start, bool sampled);
#endif

        static uint32_t rgb5ToRgb8(uint32_t color);
        static uint32_t rgb6ToRgb8(uint32_t color);
        static uint16_t rgb6ToRgb5(uint32_t color);

        void gbaScanline240();
        void gbaScanline308();
        void scanline256();
        void scanline355();

        void drawGbaThreaded();
        void drawThreaded();
};

#endif // GPU_H
