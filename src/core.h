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

#ifndef CORE_H
#define CORE_H

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "cartridge.h"
#include "cp15.h"
#include "defines.h"
#include "div_sqrt.h"
#include "dldi.h"
#include "dma.h"
#include "gpu.h"
#include "gpu_2d.h"
#include "gpu_3d.h"
#include "gpu_3d_renderer.h"
#include "input.h"
#include "interpreter.h"
#include "ipc.h"
#include "memory.h"
#include "rtc.h"
#include "scheduler_queue.h"
#include "spi.h"
#include "spu.h"
#include "timers.h"
#include "wifi.h"

enum TaskProfileClass : uint8_t
{
    TASK_PROFILE_CORE = 0,
    TASK_PROFILE_GPU_DRAW,
    TASK_PROFILE_GPU_TIMING,
    TASK_PROFILE_GPU3D,
    TASK_PROFILE_SPU,
    TASK_PROFILE_DMA,
    TASK_PROFILE_TIMER,
    TASK_PROFILE_IRQ,
    TASK_PROFILE_OTHER,
    TASK_PROFILE_COUNT
};

struct Task
{
    Task(std::function<void()> *task, uint32_t cycles,
        TaskProfileClass profileClass = TASK_PROFILE_OTHER):
        task(task), cycles(cycles), profileClass(profileClass) {}

    std::function<void()> *task;
    uint32_t cycles;
    TaskProfileClass profileClass;

    bool operator<(const Task &task) const { return cycles < task.cycles; }
};

struct CoreRunProfile
{
    uint64_t arm9Opcodes = 0;
    uint64_t arm7Opcodes = 0;
    uint64_t taskExecutions = 0;
    uint64_t scheduleCalls = 0;
    uint64_t legacyFrontShiftMoves = 0;
    uint64_t queueCompactions = 0;
    uint64_t queueCompactionMoves = 0;
    uint32_t maxTaskDepth = 0;

    uint64_t taskClassCount[TASK_PROFILE_COUNT] = {};
    uint64_t taskClassTicks[TASK_PROFILE_COUNT] = {};
    uint64_t taskClassSamples[TASK_PROFILE_COUNT] = {};
    // Sample each class independently. A single global modulo counter can
    // alias with the scheduler's repeating event pattern and repeatedly time
    // the same cheap/expensive phase.
    uint64_t taskClassSampleCounter[TASK_PROFILE_COUNT] = {};
};

class Core
{
    public:
        Core(std::string ndsPath = "", std::string gbaPath = "");

        void runFrame() { (this->*runFunc)(); }

        bool isGbaMode() { return gbaMode; }
        int  getFps()    { return fps;     }

        uint32_t getGlobalCycles() { return globalCycles; }

#ifdef NOODS_PROFILE
        uint32_t getCyclesUntilNextEvent() const
        {
            if (tasks.empty() || tasks.front().cycles <= globalCycles)
                return 0;
            return tasks.front().cycles - globalCycles;
        }
#endif

        void setProfileEnabled(bool enabled)
        {
            profileEnabled = enabled;
            interpreter[0].setProfileEnabled(enabled);
            interpreter[1].setProfileEnabled(enabled);
#ifdef NOODS_PROFILE
            gpu.setProfileEnabled(enabled);
#endif
        }

        void resetProfile()
        {
            runProfile = CoreRunProfile();
            interpreter[0].resetProfile();
            interpreter[1].resetProfile();
#ifdef NOODS_PROFILE
            gpu.resetProfile();
#endif
        }

        const CoreRunProfile &getRunProfile() const { return runProfile; }

        void schedule(Task task);
        void enterGbaMode();

        CartridgeNds cartridgeNds;
        CartridgeGba cartridgeGba;
        Cp15 cp15;
        DivSqrt divSqrt;
        Dldi dldi;
        Dma dma[2];
        Gpu gpu;
        Gpu2D gpu2D[2];
        Gpu3D gpu3D;
        Gpu3DRenderer gpu3DRenderer;
        Input input;
        Interpreter interpreter[2];
        Ipc ipc;
        Memory memory;
        Rtc rtc;
        Spi spi;
        Spu spu;
        Timers timers[2];
        Wifi wifi;

    private:
        bool gbaMode = false;
        void (Core::*runFunc)() = &Core::runNdsFrame;

        SchedulerQueue<Task> tasks;
        bool profileEnabled = false;
        CoreRunProfile runProfile;

        uint32_t frameCycles = 0, globalCycles = 0;
        uint32_t arm9Cycles = 0, arm7Cycles = 0;

        int fps = 0, fpsCount = 0;
        std::chrono::steady_clock::time_point lastFpsTime;

        std::function<void()> resetCyclesTask;

        void resetCycles();

        void runNdsFrame();
        void runGbaFrame();
};

#endif // CORE_H
