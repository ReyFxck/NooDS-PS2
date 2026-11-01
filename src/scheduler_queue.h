#ifndef SCHEDULER_QUEUE_H
#define SCHEDULER_QUEUE_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <utility>
#include <vector>

struct SchedulerInsertResult
{
    size_t compactedMoves = 0;
    uint64_t compactionTicks = 0;
    bool tiedWithExisting = false;
};

// Stable sorted queue with an amortized O(1) front pop. std::vector::erase at
// begin shifts every pending event on every scheduler tick; retaining a cursor
// performs that compaction only once per batch while preserving upper_bound's
// deterministic FIFO ordering for equal cycle values.
template <typename T>
class SchedulerQueue
{
    public:
        bool empty() const { return head == entries.size(); }
        size_t size() const { return entries.size() - head; }
        size_t storageSize() const { return entries.size(); }

        T &front() { return entries[head]; }
        const T &front() const { return entries[head]; }

        // Returns the number of live entries moved by an occasional prefix
        // compaction. This lets PROFILE builds quantify the work that replaced
        // per-event erase(begin()) shifts without timing every event.
        SchedulerInsertResult insert(T value, bool measure = false)
        {
            SchedulerInsertResult result;
            result.compactedMoves = compactConsumedPrefix(measure,
                &result.compactionTicks);
            typename std::vector<T>::iterator first =
                entries.begin() + static_cast<std::ptrdiff_t>(head);
            typename std::vector<T>::iterator position =
                std::upper_bound(first, entries.end(), value);
            if (measure && position != first)
            {
                const T &previous = *(position - 1);
                result.tiedWithExisting = !(previous < value) && !(value < previous);
            }
            entries.insert(position, std::move(value));
            return result;
        }

        void popFront()
        {
            head++;
            if (head == entries.size())
            {
                entries.clear();
                head = 0;
            }
        }

        void clear()
        {
            entries.clear();
            head = 0;
        }

        template <typename Function>
        void forEachPending(Function function)
        {
            for (size_t i = head; i < entries.size(); i++)
                function(entries[i]);
        }

    private:
        std::vector<T> entries;
        size_t head = 0;

        size_t compactConsumedPrefix(bool measure, uint64_t *ticks)
        {
            // With the observed PS2 maximum depth of 13, batching 64 pops
            // replaces roughly 64 front shifts with one short compaction.
            if (head < 64 || head * 2 < entries.size())
                return 0;

            size_t live = size();
            clock_t start = measure ? clock() : 0;
            entries.erase(entries.begin(),
                entries.begin() + static_cast<std::ptrdiff_t>(head));
            if (measure && ticks)
                *ticks = static_cast<uint64_t>(clock() - start);
            head = 0;
            return live;
        }
};

#endif
