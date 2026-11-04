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
#include <cstddef>
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
    uint64_t arm9BlockFilterHits = 0;
    uint64_t arm9BlockExecutions = 0;
    uint64_t arm9BlockChainedExecutions = 0;
    uint64_t arm9BlockFilterDrops = 0;
    uint64_t arm9BlockBudgetSkips = 0;
    uint64_t arm9AutoHotPromotions = 0;
    uint64_t arm7BlockFilterHits = 0;
    uint64_t arm7BlockExecutions = 0;
    uint64_t arm7BlockChainedExecutions = 0;
    uint64_t arm7BlockFilterDrops = 0;
    uint64_t arm7BlockBudgetSkips = 0;
    uint64_t arm7AutoHotPromotions = 0;
    uint64_t taskExecutions = 0;
    uint64_t scheduleCalls = 0;
    uint64_t estimatedLegacyFrontMoves = 0;
    uint64_t queueCompactions = 0;
    uint64_t queueCompactionMoves = 0;
    uint64_t queueCompactionTicks = 0;
    uint64_t schedulerTieInserts = 0;
    uint64_t schedulerInsertTicks = 0;
    uint64_t schedulerInsertSamples = 0;
    uint64_t schedulerPopTicks = 0;
    uint64_t schedulerPopSamples = 0;
    uint32_t schedulerInsertSampleState = 0;
    uint32_t schedulerPopSampleState = 0;
    uint32_t maxTaskDepth = 0;

    uint64_t taskClassCount[TASK_PROFILE_COUNT] = {};
    uint64_t taskClassTicks[TASK_PROFILE_COUNT] = {};
    uint64_t taskClassSamples[TASK_PROFILE_COUNT] = {};
    // Sample each class with an independent deterministic PRNG stream so a
    // periodic task schedule cannot repeatedly select or skip one class.
    uint32_t taskClassSampleState[TASK_PROFILE_COUNT] = {};
};

class Core
{
    public:
        Core(std::string ndsPath = "", std::string gbaPath = "");

        void runFrame() { (this->*runFunc)(); }

        bool isGbaMode() { return gbaMode; }
        int  getFps()    { return fps;     }

        uint32_t getGlobalCycles() { return globalCycles; }

        // Optional architecture-neutral ARM9 block runner. Positive values are
        // exact committed cycles. Zero is a transient non-budget fallback.
        // Normal negative values mean the block is valid but needs at least
        // abs(value) cycles before the backend should be called again. A
        // backend may return ARM9_BLOCK_RUNNER_PERMANENT_REJECT (declared in
        // cpu_backend.h) so the scheduler drops that hot PC immediately.
        using Arm9BlockRunner =
            int (*)(void*, Interpreter&, uint32_t, uint32_t*);
        void setArm9BlockRunner(void *context, Arm9BlockRunner runner)
        {
            arm9BlockRunnerContext = context;
            arm9BlockRunner = runner;
        }

        // Add architecture-neutral hot block entry PCs. The scheduler checks
        // this tiny direct-mapped filter before calling a platform backend, so
        // a dynarec cannot impose a function-call/lookup tax on every opcode.
        void addArm9BlockHotPcs(const uint32_t *pcs, size_t count);

        using Arm7BlockRunner = Arm9BlockRunner;
        void setArm7BlockRunner(void *context, Arm7BlockRunner runner)
        {
            arm7BlockRunnerContext = context;
            arm7BlockRunner = runner;
        }
        void addArm7BlockHotPcs(const uint32_t *pcs, size_t count);

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
            memory.setProfileEnabled(enabled);
            gpu.setProfileEnabled(enabled);
            gpu2D[0].setProfileEnabled(enabled);
            gpu2D[1].setProfileEnabled(enabled);
#endif
        }

        void resetProfile()
        {
            runProfile = CoreRunProfile();
            interpreter[0].resetProfile();
            interpreter[1].resetProfile();
#ifdef NOODS_PROFILE
            memory.resetProfile();
            gpu.resetProfile();
            gpu2D[0].resetProfile();
            gpu2D[1].resetProfile();
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
        void *arm9BlockRunnerContext = nullptr;
        Arm9BlockRunner arm9BlockRunner = nullptr;

        static constexpr size_t ARM9_BLOCK_FILTER_SLOTS = 128;
        uint32_t arm9BlockHotPc[ARM9_BLOCK_FILTER_SLOTS] = {};
        uint8_t arm9BlockHotFailures[ARM9_BLOCK_FILTER_SLOTS] = {};
        uint8_t arm9BlockMinCycles[ARM9_BLOCK_FILTER_SLOTS] = {};
        uint8_t arm9BlockHotCount = 0;

        // Always-on, bounded hot-PC discovery for PS2 dynarec backends. One
        // sample per 64 interpreted ARM9 opcodes is cheap enough for RELEASE
        // and removes the old dependency on 60-frame PROFILE windows.
        static constexpr size_t ARM9_HOT_SAMPLE_SLOTS = 128;
        uint32_t arm9HotSamplePc[ARM9_HOT_SAMPLE_SLOTS] = {};
        uint8_t arm9HotSampleScore[ARM9_HOT_SAMPLE_SLOTS] = {};
        uint8_t arm9HotSampleDivider = 0;

        size_t arm9BlockFilterSlot(uint32_t pc) const
        {
            return ((pc >> 2) ^ (pc >> 11)) & (ARM9_BLOCK_FILTER_SLOTS - 1);
        }

        size_t arm9HotSampleSlot(uint32_t pc) const
        {
            return ((pc >> 2) ^ (pc >> 10) ^ (pc >> 18)) &
                (ARM9_HOT_SAMPLE_SLOTS - 1);
        }

        void sampleArm9HotPc(uint32_t pc);

        void *arm7BlockRunnerContext = nullptr;
        Arm7BlockRunner arm7BlockRunner = nullptr;
        static constexpr size_t ARM7_BLOCK_FILTER_SLOTS = 128;
        uint32_t arm7BlockHotPc[ARM7_BLOCK_FILTER_SLOTS] = {};
        uint8_t arm7BlockHotFailures[ARM7_BLOCK_FILTER_SLOTS] = {};
        uint8_t arm7BlockMinCycles[ARM7_BLOCK_FILTER_SLOTS] = {};
        uint8_t arm7BlockHotCount = 0;

        static constexpr size_t ARM7_HOT_SAMPLE_SLOTS = 128;
        uint32_t arm7HotSamplePc[ARM7_HOT_SAMPLE_SLOTS] = {};
        uint8_t arm7HotSampleScore[ARM7_HOT_SAMPLE_SLOTS] = {};
        uint8_t arm7HotSampleDivider = 0;

        size_t arm7BlockFilterSlot(uint32_t pc) const
        {
            return ((pc >> 2) ^ (pc >> 11)) & (ARM7_BLOCK_FILTER_SLOTS - 1);
        }
        size_t arm7HotSampleSlot(uint32_t pc) const
        {
            return ((pc >> 2) ^ (pc >> 10) ^ (pc >> 18)) &
                (ARM7_HOT_SAMPLE_SLOTS - 1);
        }
        void sampleArm7HotPc(uint32_t pc);

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
