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

#include <algorithm>
#include <cstring>
#include <ctime>
#include <thread>

#include "core.h"
#include "cpu_backend.h"
#include "settings.h"

Core::Core(std::string ndsPath, std::string gbaPath):
    cartridgeNds(this),
    cartridgeGba(this),
    cp15(this),
    divSqrt(this),
    dldi(this),
    dma { Dma(this, 0), Dma(this, 1) },
    gpu(this),
    gpu2D { Gpu2D(this, 0), Gpu2D(this, 1) },
    gpu3D(this),
    gpu3DRenderer(this),
    input(this),
    interpreter { Interpreter(this, 0), Interpreter(this, 1) },
    ipc(this),
    memory(this),
    rtc(this),
    spi(this),
    spu(this),
    timers { Timers(this, 0), Timers(this, 1) },
    wifi(this)
{
    // Schedule initial tasks for NDS mode
    resetCyclesTask = std::bind(&Core::resetCycles, this);
    schedule(Task(&resetCyclesTask, 0x7FFFFFFF, TASK_PROFILE_CORE));
    gpu.scheduleInit();
    spu.scheduleInit();

    // Load the NDS BIOS and firmware unless directly booting a GBA ROM
    if (ndsPath != "" || gbaPath == "" || !Settings::getDirectBoot())
    {
        memory.loadBios();
        spi.loadFirmware();
    }

    // Initialize the memory and CPUs
    memory.updateMap9(0x00000000, 0xFFFFFFFF);
    memory.updateMap7(0x00000000, 0xFFFFFFFF);
    interpreter[0].init();
    interpreter[1].init();

    if (gbaPath != "")
    {
        // Load the GBA BIOS unless directly booting an NDS ROM
        if (ndsPath == "" || !Settings::getDirectBoot())
            memory.loadGbaBios();

        // Load a GBA ROM
        cartridgeGba.loadRom(gbaPath);

        // Enable GBA mode right away if direct boot is enabled
        if (ndsPath == "" && Settings::getDirectBoot())
        {
            memory.write<uint16_t>(0, 0x4000304, 0x8003); // POWCNT1
            enterGbaMode();
        }
    }

    if (ndsPath != "")
    {
        // Load an NDS ROM
        cartridgeNds.loadRom(ndsPath);

        // Prepare to boot the NDS ROM directly if direct boot is enabled
        if (Settings::getDirectBoot())
        {
            // Set some registers as the BIOS/firmware would
            cp15.write(1, 0, 0, 0x0005707D); // CP15 Control
            cp15.write(9, 1, 0, 0x0300000A); // Data TCM base/size
            cp15.write(9, 1, 1, 0x00000020); // Instruction TCM size
            memory.write<uint8_t>(0,  0x4000247,   0x03); // WRAMCNT
            memory.write<uint8_t>(0,  0x4000300,   0x01); // POSTFLG (ARM9)
            memory.write<uint8_t>(1,  0x4000300,   0x01); // POSTFLG (ARM7)
            memory.write<uint16_t>(0, 0x4000304, 0x0001); // POWCNT1
            memory.write<uint16_t>(1, 0x4000504, 0x0200); // SOUNDBIAS

            // Set some memory values as the BIOS/firmware would
            memory.write<uint32_t>(0, 0x27FF800, 0x00001FC2); // Chip ID 1
            memory.write<uint32_t>(0, 0x27FF804, 0x00001FC2); // Chip ID 2
            memory.write<uint16_t>(0, 0x27FF850,     0x5835); // ARM7 BIOS CRC
            memory.write<uint16_t>(0, 0x27FF880,     0x0007); // Message from ARM9 to ARM7
            memory.write<uint16_t>(0, 0x27FF884,     0x0006); // ARM7 boot task
            memory.write<uint32_t>(0, 0x27FFC00, 0x00001FC2); // Copy of chip ID 1
            memory.write<uint32_t>(0, 0x27FFC04, 0x00001FC2); // Copy of chip ID 2
            memory.write<uint16_t>(0, 0x27FFC10,     0x5835); // Copy of ARM7 BIOS CRC
            memory.write<uint16_t>(0, 0x27FFC40,     0x0001); // Boot indicator

            cartridgeNds.directBoot();
            interpreter[0].directBoot();
            interpreter[1].directBoot();
            spi.directBoot();
        }
    }
}

void Core::resetCycles()
{
    // Reset the global cycle count periodically to prevent overflow
    tasks.forEachPending([&](Task &task)
    {
        task.cycles -= globalCycles;
    });
    arm9Cycles -= std::min(globalCycles, arm9Cycles);
    arm7Cycles -= std::min(globalCycles, arm7Cycles);
    timers[0].resetCycles();
    timers[1].resetCycles();
    globalCycles -= globalCycles;
    schedule(Task(&resetCyclesTask, 0x7FFFFFFF, TASK_PROFILE_CORE));
}

void Core::runGbaFrame()
{
    // Run a frame in GBA mode
    while (frameCycles < 228 * 308 * 4) // 228 scanlines, 308 dots, 4 ARM7 cycles
    {
        uint32_t i = globalCycles;

        // Run the ARM7 until the next scheduled task
        if (arm7Cycles > globalCycles) globalCycles = arm7Cycles;
        while (interpreter[1].shouldRun() && tasks.front().cycles > arm7Cycles)
        {
#ifdef NOODS_PROFILE
            if (profileEnabled)
                runProfile.arm7Opcodes++;
#endif
            arm7Cycles = (globalCycles += interpreter[1].runOpcode());
        }

        // Jump to the next scheduled task
        globalCycles = tasks.front().cycles;
        frameCycles += globalCycles - i;

        // Run all tasks that are scheduled now
        while (tasks.front().cycles <= globalCycles)
        {
            Task currentTask = tasks.front();
#ifdef NOODS_PROFILE
            TaskProfileClass profileClass = currentTask.profileClass;
            bool sampleTask = false;
            clock_t taskStart = 0;
            if (profileEnabled)
            {
                runProfile.taskExecutions++;
                runProfile.taskClassCount[profileClass]++;
                sampleTask = profileSample64(
                    runProfile.taskClassSampleState[profileClass], profileClass);
                if (sampleTask)
                    taskStart = clock();
            }
#endif
            (*currentTask.task)();
#ifdef NOODS_PROFILE
            if (profileEnabled && sampleTask)
            {
                runProfile.taskClassTicks[profileClass] +=
                    static_cast<uint64_t>(clock() - taskStart);
                runProfile.taskClassSamples[profileClass]++;
            }
            if (profileEnabled)
                runProfile.estimatedLegacyFrontMoves += tasks.size() - 1;
#endif
#ifdef NOODS_PROFILE
            bool samplePop = profileEnabled && profileSample64(
                runProfile.schedulerPopSampleState, 0x601u);
            clock_t popStart = samplePop ? clock() : 0;
#endif
            tasks.popFront();
#ifdef NOODS_PROFILE
            if (samplePop)
            {
                runProfile.schedulerPopTicks +=
                    static_cast<uint64_t>(clock() - popStart);
                runProfile.schedulerPopSamples++;
            }
#endif
        }
    }

    // Count a frame
    frameCycles -= 228 * 308 * 4;
    fpsCount++;

    // Update the FPS and reset the counter every second
    std::chrono::duration<double> fpsTime = std::chrono::steady_clock::now() - lastFpsTime;
    if (fpsTime.count() >= 1.0f)
    {
        fps = fpsCount;
        fpsCount = 0;
        lastFpsTime = std::chrono::steady_clock::now();
    }
}

void Core::runNdsFrame()
{
    // Run a frame in NDS mode
    while (frameCycles < 263 * 355 * 6) // 263 scanlines, 355 dots, 6 ARM9 cycles
    {
        uint32_t i = globalCycles;

        // Run the CPUs until the next scheduled task
        while (tasks.front().cycles > globalCycles)
        {
            // Run the ARM9. A block backend may execute only while the whole
            // block fits before the next scheduler task and before the ARM7's
            // next execution slot. Otherwise preserve the original one-opcode
            // interpreter ordering exactly.
            if (interpreter[0].shouldRun() && globalCycles >= arm9Cycles)
            {
                int arm9StepCycles = 0;
                uint32_t arm9StepInstructions = 0;

                if (arm9BlockRunner && arm9BlockHotCount)
                {
                    uint32_t guestPc =
                        CpuBackendAccess::nextInstructionAddress(interpreter[0]);
                    size_t hotSlot = arm9BlockFilterSlot(guestPc);
                    if (arm9BlockHotPc[hotSlot] == guestPc)
                    {
#ifdef NOODS_PROFILE
                        if (profileEnabled)
                            runProfile.arm9BlockFilterHits++;
#endif
                        uint32_t cycleBudget = tasks.front().cycles - globalCycles;
                        if (interpreter[1].shouldRun())
                        {
                            if (arm7Cycles <= globalCycles)
                                cycleBudget = 0;
                            else
                                cycleBudget = std::min(cycleBudget,
                                    arm7Cycles - globalCycles);
                        }

                        uint32_t minCycles = arm9BlockMinCycles[hotSlot];
                        if (!minCycles)
                            minCycles = 1;

                        if (!cycleBudget || cycleBudget < minCycles)
                        {
#ifdef NOODS_PROFILE
                            if (profileEnabled)
                                runProfile.arm9BlockBudgetSkips++;
#endif
                        }
                        else
                        {
                            arm9StepCycles = arm9BlockRunner(
                                arm9BlockRunnerContext, interpreter[0],
                                cycleBudget, &arm9StepInstructions);
                            if (arm9StepCycles > 0)
                            {
                                arm9BlockHotFailures[hotSlot] = 0;
#ifdef NOODS_PROFILE
                                if (profileEnabled)
                                    runProfile.arm9BlockExecutions++;
#endif

                                // Stay inside this ARM9 scheduler slot while
                                // there is still room before the next ARM7
                                // execution/task deadline. This removes repeated
                                // Core loop + backend dispatch overhead without
                                // crossing any externally visible event.
                                constexpr uint32_t MAX_CHAINED_BLOCKS = 8;
                                uint32_t chainedBlocks = 1;
                                uint32_t usedCycles =
                                    static_cast<uint32_t>(arm9StepCycles);

                                while (chainedBlocks < MAX_CHAINED_BLOCKS &&
                                    usedCycles < cycleBudget)
                                {
                                    uint32_t nextPc =
                                        CpuBackendAccess::nextInstructionAddress(
                                            interpreter[0]);
                                    size_t nextSlot = arm9BlockFilterSlot(nextPc);
                                    if (arm9BlockHotPc[nextSlot] != nextPc)
                                        break;

#ifdef NOODS_PROFILE
                                    if (profileEnabled)
                                        runProfile.arm9BlockFilterHits++;
#endif
                                    uint32_t remaining = cycleBudget - usedCycles;
                                    uint32_t nextMin = arm9BlockMinCycles[nextSlot];
                                    if (!nextMin)
                                        nextMin = 1;
                                    if (remaining < nextMin)
                                        break;

                                    uint32_t nextInstructions = 0;
                                    int nextCycles = arm9BlockRunner(
                                        arm9BlockRunnerContext, interpreter[0],
                                        remaining, &nextInstructions);

                                    if (nextCycles > 0)
                                    {
                                        usedCycles +=
                                            static_cast<uint32_t>(nextCycles);
                                        arm9StepInstructions += nextInstructions
                                            ? nextInstructions : 1;
                                        arm9BlockHotFailures[nextSlot] = 0;
                                        chainedBlocks++;
#ifdef NOODS_PROFILE
                                        if (profileEnabled)
                                        {
                                            runProfile.arm9BlockExecutions++;
                                            runProfile.arm9BlockChainedExecutions++;
                                        }
#endif
                                        continue;
                                    }

                                    if (nextCycles < 0)
                                    {
                                        uint32_t required =
                                            static_cast<uint32_t>(-nextCycles);
                                        if (required > 0xFFu)
                                            required = 0xFFu;
                                        if (required >
                                            arm9BlockMinCycles[nextSlot])
                                        {
                                            arm9BlockMinCycles[nextSlot] =
                                                static_cast<uint8_t>(required);
                                        }
#ifdef NOODS_PROFILE
                                        if (profileEnabled)
                                            runProfile.arm9BlockBudgetSkips++;
#endif
                                    }
                                    else
                                    {
                                        uint8_t &failures =
                                            arm9BlockHotFailures[nextSlot];
                                        if (failures < 0xFFu)
                                            failures++;
                                        if (failures >= 8)
                                        {
                                            arm9BlockHotPc[nextSlot] = 0;
                                            arm9BlockMinCycles[nextSlot] = 0;
                                            failures = 0;
                                            if (arm9BlockHotCount)
                                                arm9BlockHotCount--;
#ifdef NOODS_PROFILE
                                            if (profileEnabled)
                                                runProfile.arm9BlockFilterDrops++;
#endif
                                        }
                                    }
                                    break;
                                }

                                arm9StepCycles =
                                    static_cast<int>(usedCycles);
                            }
                            else if (arm9StepCycles < 0)
                            {
                                uint32_t required = static_cast<uint32_t>(
                                    -arm9StepCycles);
                                if (required > 0xFFu)
                                    required = 0xFFu;
                                if (required > arm9BlockMinCycles[hotSlot])
                                    arm9BlockMinCycles[hotSlot] =
                                        static_cast<uint8_t>(required);
#ifdef NOODS_PROFILE
                                if (profileEnabled)
                                    runProfile.arm9BlockBudgetSkips++;
#endif
                            }
                            else
                            {
                                uint8_t &failures = arm9BlockHotFailures[hotSlot];
                                if (failures < 0xFFu)
                                    failures++;

                                if (failures >= 8)
                                {
                                    arm9BlockHotPc[hotSlot] = 0;
                                    arm9BlockMinCycles[hotSlot] = 0;
                                    failures = 0;
                                    if (arm9BlockHotCount)
                                        arm9BlockHotCount--;
#ifdef NOODS_PROFILE
                                    if (profileEnabled)
                                        runProfile.arm9BlockFilterDrops++;
#endif
                                }
                            }
                        }
                    }
                }

                if (arm9StepCycles > 0)
                {
                    if (!arm9StepInstructions)
                        arm9StepInstructions = 1;
#ifdef NOODS_PROFILE
                    if (profileEnabled)
                        runProfile.arm9Opcodes += arm9StepInstructions;
#endif
                    arm9Cycles = globalCycles +
                        static_cast<uint32_t>(arm9StepCycles);
                }
                else
                {
                    sampleArm9HotPc(
                        CpuBackendAccess::nextInstructionAddress(interpreter[0]));
#ifdef NOODS_PROFILE
                    if (profileEnabled)
                        runProfile.arm9Opcodes++;
#endif
                    arm9Cycles = globalCycles + interpreter[0].runOpcode();
                }
            }

            // Run the ARM7 at half the speed of the ARM9
            if (interpreter[1].shouldRun() && globalCycles >= arm7Cycles)
            {
#ifdef NOODS_PROFILE
                if (profileEnabled)
                    runProfile.arm7Opcodes++;
#endif
                arm7Cycles = globalCycles + (interpreter[1].runOpcode() << 1);
            }

            // Count cycles up to the next soonest event
            globalCycles = std::min((interpreter[0].shouldRun() ? arm9Cycles : tasks.front().cycles),
                (interpreter[1].shouldRun() ? arm7Cycles : tasks.front().cycles));
        }

        // Jump to the next scheduled task
        globalCycles = tasks.front().cycles;
        frameCycles += globalCycles - i;

        // Run all tasks that are scheduled now
        while (tasks.front().cycles <= globalCycles)
        {
            Task currentTask = tasks.front();
#ifdef NOODS_PROFILE
            TaskProfileClass profileClass = currentTask.profileClass;
            bool sampleTask = false;
            clock_t taskStart = 0;
            if (profileEnabled)
            {
                runProfile.taskExecutions++;
                runProfile.taskClassCount[profileClass]++;
                sampleTask = profileSample64(
                    runProfile.taskClassSampleState[profileClass], profileClass);
                if (sampleTask)
                    taskStart = clock();
            }
#endif
            (*currentTask.task)();
#ifdef NOODS_PROFILE
            if (profileEnabled && sampleTask)
            {
                runProfile.taskClassTicks[profileClass] +=
                    static_cast<uint64_t>(clock() - taskStart);
                runProfile.taskClassSamples[profileClass]++;
            }
            if (profileEnabled)
                runProfile.estimatedLegacyFrontMoves += tasks.size() - 1;
#endif
#ifdef NOODS_PROFILE
            bool samplePop = profileEnabled && profileSample64(
                runProfile.schedulerPopSampleState, 0x601u);
            clock_t popStart = samplePop ? clock() : 0;
#endif
            tasks.popFront();
#ifdef NOODS_PROFILE
            if (samplePop)
            {
                runProfile.schedulerPopTicks +=
                    static_cast<uint64_t>(clock() - popStart);
                runProfile.schedulerPopSamples++;
            }
#endif
        }
    }

    // Count a frame
    frameCycles -= 263 * 355 * 6;
    fpsCount++;

    // Update the FPS and reset the counter every second
    std::chrono::duration<double> fpsTime = std::chrono::steady_clock::now() - lastFpsTime;
    if (fpsTime.count() >= 1.0f)
    {
        fps = fpsCount;
        fpsCount = 0;
        lastFpsTime = std::chrono::steady_clock::now();
    }
}

void Core::sampleArm9HotPc(uint32_t pc)
{
    if (!pc || ++arm9HotSampleDivider != 64)
        return;
    arm9HotSampleDivider = 0;

    size_t sampleSlot = arm9HotSampleSlot(pc);
    uint32_t &samplePc = arm9HotSamplePc[sampleSlot];
    uint8_t &score = arm9HotSampleScore[sampleSlot];

    if (samplePc == pc)
    {
        if (score < 0xFFu)
            score++;
    }
    else if (score)
    {
        score--;
        return;
    }
    else
    {
        samplePc = pc;
        score = 1;
    }

    // Eight matching 1/64 samples means roughly 512 observed interpreter
    // opportunities at this entry. Promote it, then reset the score so a
    // backend-rejected PC must prove hot again before another attempt.
    if (score < 8)
        return;

    size_t hotSlot = arm9BlockFilterSlot(pc);
    if (arm9BlockHotPc[hotSlot] != pc)
    {
        addArm9BlockHotPcs(&pc, 1);
#ifdef NOODS_PROFILE
        if (profileEnabled)
            runProfile.arm9AutoHotPromotions++;
#endif
    }
    score = 0;
}

void Core::addArm9BlockHotPcs(const uint32_t *pcs, size_t count)
{
    if (!pcs)
        return;

    for (size_t i = 0; i < count; i++)
    {
        uint32_t pc = pcs[i];
        if (!pc)
            continue;

        size_t slot = arm9BlockFilterSlot(pc);
        if (!arm9BlockHotPc[slot])
            arm9BlockHotCount++;

        if (arm9BlockHotPc[slot] != pc)
        {
            arm9BlockHotFailures[slot] = 0;
            arm9BlockMinCycles[slot] = 1;
        }

        arm9BlockHotPc[slot] = pc;
    }
}

void Core::schedule(Task task)
{
    // Add a task to the scheduler, sorted by least to most cycles until execution
    task.cycles += globalCycles;
#ifdef NOODS_PROFILE
    bool sampleInsert = profileEnabled && profileSample64(
        runProfile.schedulerInsertSampleState, 0x600u);
    clock_t insertStart = sampleInsert ? clock() : 0;
#endif
    SchedulerInsertResult insertResult = tasks.insert(task,
#ifdef NOODS_PROFILE
        profileEnabled
#else
        false
#endif
    );

#ifdef NOODS_PROFILE
    if (profileEnabled)
    {
        runProfile.scheduleCalls++;
        if (sampleInsert)
        {
            uint64_t elapsed = static_cast<uint64_t>(clock() - insertStart);
            // Compactions are measured exhaustively below. Remove their time
            // from the sampled insert estimate to avoid double counting.
            runProfile.schedulerInsertTicks +=
                (elapsed > insertResult.compactionTicks)
                    ? elapsed - insertResult.compactionTicks : 0;
            runProfile.schedulerInsertSamples++;
        }
        if (insertResult.compactedMoves)
        {
            runProfile.queueCompactions++;
            runProfile.queueCompactionMoves += insertResult.compactedMoves;
            runProfile.queueCompactionTicks += insertResult.compactionTicks;
        }
        if (insertResult.tiedWithExisting)
            runProfile.schedulerTieInserts++;
        if (tasks.size() > runProfile.maxTaskDepth)
            runProfile.maxTaskDepth = static_cast<uint32_t>(tasks.size());
    }
#endif
}

void Core::enterGbaMode()
{
    // Switch to GBA mode
    gbaMode = true;
    runFunc = &Core::runGbaFrame;

    // Reset the scheduler and schedule initial tasks for GBA mode
    tasks.clear();
    schedule(Task(&resetCyclesTask, 1, TASK_PROFILE_CORE));
    gpu.gbaScheduleInit();
    spu.gbaScheduleInit();

    // Reset the ARM7 for GBA mode
    memory.updateMap7(0x00000000, 0xFFFFFFFF);
    interpreter[1].init();

    // Set VRAM blocks A and B to plain access mode
    // This is used by the GPU to access the VRAM borders
    memory.write<uint8_t>(0, 0x4000240, 0x80); // VRAMCNT_A
    memory.write<uint8_t>(0, 0x4000241, 0x80); // VRAMCNT_B
}
