// Checks the cycle budget the scheduler hands to native (JIT) blocks.
//  1. coreBlockBudget() against the original lockstep formula and its invariants.
//  2. The real Core::runNdsFrame() with a fake block runner: every budget it is
//     given must match the formula, never cross the next scheduler task, and
//     only exceed the ARM7 deadline when a sync quantum is set.
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
#include "../src/cpu_backend.h"
#include "../src/settings.h"

namespace
{
    uint64_t rngState = 0x2545F4914F6CDD1Dull;
    uint32_t rnd()
    {
        rngState ^= rngState << 13;
        rngState ^= rngState >> 7;
        rngState ^= rngState << 17;
        return static_cast<uint32_t>(rngState >> 16);
    }

    // The ARM9 budget logic of Core::runNdsFrame before the sync quantum existed
    uint32_t legacyBudget(uint32_t taskBudget, bool otherRuns, uint32_t otherDue)
    {
        uint32_t budget = taskBudget;
        if (otherRuns)
        {
            if (otherDue == 0)
                budget = 0;
            else
                budget = std::min(budget, otherDue);
        }
        return budget;
    }

    void testPureFunction()
    {
        for (int i = 0; i < 200000; i++)
        {
            const uint32_t task = rnd() % 5000;
            const bool runs = rnd() & 1;
            const uint32_t due = (rnd() % 4) ? rnd() % 3000 : 0;
            const uint32_t quantum = (rnd() % 3) ? rnd() % 300 : 0;

            // Quantum 0 is exactly the old behaviour
            assert(coreBlockBudget(task, runs, due, 0) == legacyBudget(task, runs, due));

            const uint32_t budget = coreBlockBudget(task, runs, due, quantum);
            assert(budget <= task);                                   // never crosses a task
            assert(budget >= legacyBudget(task, runs, due));          // never smaller than before
            if (!runs)
                assert(budget == task);
            else
                assert(budget == std::min<uint64_t>(task, static_cast<uint64_t>(due) + quantum));
        }
        // No overflow with huge values
        assert(coreBlockBudget(0xFFFFFFFFu, true, 0xFFFFFFFFu, 0xFFFFFFFFu) == 0xFFFFFFFFu);
        assert(coreBlockBudget(100, true, 0xFFFFFFF0u, 0x100) == 100);
    }

    struct Ctx
    {
        Core *core;
        uint32_t quantum;
        long calls, ahead, withArm7Running, cycles;
        bool violation;
    };

    int fakeRunner(void *context, Interpreter &, uint32_t budget, uint32_t *instructions)
    {
        Ctx *ctx = static_cast<Ctx*>(context);
        Core *core = ctx->core;
        const uint32_t task = core->tasks.front().cycles - core->globalCycles;
        const bool arm7Runs = core->interpreter[1].shouldRun();
        const uint32_t arm7Due = (core->arm7Cycles > core->globalCycles) ?
            core->arm7Cycles - core->globalCycles : 0;
        const uint32_t expected = coreBlockBudget(task, arm7Runs, arm7Due, ctx->quantum);
        const uint32_t legacy = legacyBudget(task, arm7Runs, arm7Due);

        ctx->calls++;
        if (arm7Runs) ctx->withArm7Running++;
        if (budget > expected || budget > task)
            ctx->violation = true;
        if (budget > legacy)
            ctx->ahead++;

        *instructions = 1;
        const uint32_t committed = std::min<uint32_t>(budget, 7); // Commit a few cycles
        ctx->cycles += committed;
        return static_cast<int>(committed);
    }

    // BIOS image made of "B ." (branch to self): both CPUs stay busy at a fixed
    // PC, so the scheduler keeps interleaving them like during real code.
    std::string fixture(int size, bool branchToSelf)
    {
        char path[] = "/tmp/noods-budget-XXXXXX";
        int fd = mkstemp(path);
        assert(fd >= 0);
        std::vector<uint8_t> data(size, 0);
        if (branchToSelf)
            for (int i = 0; i < size; i += 4)
            {
                data[i + 0] = 0xFE; data[i + 1] = 0xFF; data[i + 2] = 0xFF; data[i + 3] = 0xEA;
            }
        assert(write(fd, data.data(), data.size()) == static_cast<ssize_t>(data.size()));
        assert(close(fd) == 0);
        return path;
    }

    Ctx runFrame(uint32_t quantum)
    {
        const std::string b9 = fixture(0x1000, true), b7 = fixture(0x4000, true), fw = fixture(0x40000, false);
        Settings::setBios9Path(b9); Settings::setBios7Path(b7); Settings::setFirmwarePath(fw);
        Settings::setThreaded2D(0); Settings::setThreaded3D(0);
        std::unique_ptr<Core> core(new Core());
        unlink(b9.c_str()); unlink(b7.c_str()); unlink(fw.c_str());

        Ctx ctx = {core.get(), quantum, 0, 0, 0, 0, false};
        core->setSyncQuantum(quantum);
        assert(core->getSyncQuantum() == quantum);

        // Make the current ARM9 PC a hot block entry. The fake runner never
        // moves the PC, so every scheduler visit offers the same block.
        const uint32_t key = core->arm9BlockKey(
            CpuBackendAccess::nextInstructionAddress(core->interpreter[0]),
            CpuBackendAccess::isThumb(core->interpreter[0]));
        const size_t slot = core->arm9BlockFilterSlot(key);
        core->arm9BlockHotPc[slot] = key;
        core->arm9BlockMinCycles[slot] = 1;
        core->arm9BlockHotCount = 1;
        core->setArm9BlockRunner(&ctx, fakeRunner);

        core->runNdsFrame();
        return ctx;
    }
}

int main()
{
    testPureFunction();

    Ctx base = runFrame(0);
    assert(!base.violation);
    assert(base.calls > 0);
    assert(base.ahead == 0); // Original lockstep: never beyond the ARM7 deadline

    long aheadTotal = 0;
    const uint32_t quanta[] = {1, 16, 64, 1000};
    for (int q = 0; q < 4; q++)
    {
        Ctx ctx = runFrame(quanta[q]);
        assert(!ctx.violation);
        assert(ctx.cycles >= base.cycles); // At least as much time runs inside native blocks
        aheadTotal += ctx.ahead;
        printf("quantum %4u: block offers=%ld, cycles in blocks=%ld, beyond ARM7 deadline=%ld\n",
            quanta[q], ctx.calls, ctx.cycles, ctx.ahead);
    }
    printf("quantum    0: block offers=%ld, cycles in blocks=%ld, beyond ARM7 deadline=%ld\n",
        base.calls, base.cycles, base.ahead);

    // The knob must actually do something when the ARM7 is active
    if (base.withArm7Running > 0)
        assert(aheadTotal > 0);
    printf("core_budget_test: OK\n");
    return 0;
}
