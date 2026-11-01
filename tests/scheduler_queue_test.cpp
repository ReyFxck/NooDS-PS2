#include <cassert>
#include <cstdint>

#include "../src/scheduler_queue.h"

namespace
{
struct Event
{
    Event(uint32_t cycles, uint32_t id): cycles(cycles), id(id) {}

    uint32_t cycles;
    uint32_t id;

    bool operator<(const Event &event) const
    {
        return cycles < event.cycles;
    }
};
}

int main()
{
    SchedulerQueue<Event> ordered;
    ordered.insert(Event(10, 1));
    ordered.insert(Event(5, 2));
    ordered.insert(Event(10, 3));
    assert(ordered.front().id == 2);
    ordered.popFront();
    assert(ordered.front().id == 1);

    // A callback that schedules another event for the current cycle must go
    // after events that were already tied at that cycle.
    SchedulerInsertResult tied = ordered.insert(Event(10, 4), true);
    assert(tied.tiedWithExisting);
    ordered.popFront();
    assert(ordered.front().id == 3);
    ordered.popFront();
    assert(ordered.front().id == 4);
    ordered.popFront();
    assert(ordered.empty());

    SchedulerQueue<Event> periodic;
    for (uint32_t i = 0; i < 13; i++)
        periodic.insert(Event(i, i));

    uint64_t legacyMoves = 0;
    uint64_t cursorMoves = 0;
    uint32_t lastCycle = 0;
    for (uint32_t i = 0; i < 100000; i++)
    {
        Event current = periodic.front();
        assert(i == 0 || current.cycles >= lastCycle);
        lastCycle = current.cycles;

        // Match Core callback ordering: reschedule while the current event is
        // still at the front, then consume it.
        legacyMoves += periodic.size();
        cursorMoves += periodic.insert(Event(current.cycles + 13, current.id)).compactedMoves;
        periodic.popFront();
        assert(periodic.size() == 13);
    }

    // The cursor should move less than 2% as many Event objects as the former
    // erase(begin()) path at the observed maximum queue depth.
    assert(cursorMoves * 50 < legacyMoves);
    assert(periodic.storageSize() < 80);

    uint32_t pending = 0;
    periodic.forEachPending([&](Event &) { pending++; });
    assert(pending == periodic.size());
    return 0;
}
