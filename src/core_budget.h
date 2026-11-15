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

#ifndef CORE_BUDGET_H
#define CORE_BUDGET_H

#include <cstdint>

// How many scheduler cycles one CPU may run ahead of the other CPU while it
// executes a native block. 0 keeps the original lockstep behaviour: a block
// may only run while it fits before the other CPU's next execution slot. The
// next scheduler task is never crossed, whatever the value is, so timers, DMA,
// the GPU and interrupts still fire at their exact cycle.
//
// A larger value lets the JIT run longer blocks when both CPUs are active, at
// the price of the other CPU observing this CPU's memory writes up to that many
// cycles late. Build with -DNOODS_SYNC_QUANTUM_DEFAULT=64 (or call
// Core::setSyncQuantum) to experiment.
#ifndef NOODS_SYNC_QUANTUM_DEFAULT
#define NOODS_SYNC_QUANTUM_DEFAULT 0
#endif

// Cycle budget for one native block.
//   taskBudget: cycles until the next scheduler task
//   otherRuns:  whether the other CPU is currently executing
//   otherDue:   cycles until the other CPU's next execution slot (0 if due now)
//   quantum:    allowed run-ahead, see above
inline uint32_t coreBlockBudget(uint32_t taskBudget, bool otherRuns,
    uint32_t otherDue, uint32_t quantum)
{
    if (!otherRuns)
        return taskBudget;
    const uint64_t limit = static_cast<uint64_t>(otherDue) + quantum;
    return (limit < taskBudget) ? static_cast<uint32_t>(limit) : taskBudget;
}

#endif // CORE_BUDGET_H
