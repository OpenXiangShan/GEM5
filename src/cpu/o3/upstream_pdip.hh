/*
 * Priority Directed Instruction Prefetching (PDIP).
 *
 * This controller deliberately lives beside the decoupled fetch stage.  It
 * records front-end-critical instruction lines and associates them with the
 * instruction block that caused the resteer (or the last taken branch).
 */
#ifndef __CPU_O3_UPSTREAM_PDIP_HH__
#define __CPU_O3_UPSTREAM_PDIP_HH__

#include <cstdint>
#include <deque>
#include <vector>

#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/o3/limits.hh"

namespace gem5
{
namespace o3
{

class UpstreamPDIP
{
  public:
    struct Config
    {
        unsigned sets = 512;
        unsigned assoc = 8;
        unsigned targetsPerEntry = 2;
        unsigned queueSize = 16;
        unsigned queueThreshold = 2;
        unsigned blockSize = 64;
        unsigned tagBits = 10;
        unsigned insertionProbability = 25;
    };

    struct Stats
    {
        uint64_t fecPromotions = 0;
        uint64_t tableInsertions = 0;
        uint64_t tableHits = 0;
        uint64_t tableMisses = 0;
        uint64_t triggerPrefetches = 0;
        uint64_t queueDrops = 0;
        uint64_t duplicateDrops = 0;
    };

    explicit UpstreamPDIP(const Config &config);

    void clearQueue();
    void clearQueue(ThreadID tid);
    void notifyResteer(Addr trigger, ThreadID tid);
    void clearTrigger(ThreadID tid);
    Addr currentTrigger(ThreadID tid) const;
    bool promoteFec(Addr candidate, Addr trigger, ThreadID tid,
                    unsigned mask = 1);
    std::vector<Addr> lookup(Addr trigger, ThreadID tid);
    bool enqueue(Addr addr, ThreadID tid);
    bool dequeue(Addr &addr, ThreadID tid);
    bool empty(ThreadID tid) const;
    unsigned queueSize(ThreadID tid) const;
    const Stats &stats() const { return stats_; }

  private:
    struct Target
    {
        Addr line = 0;
        uint8_t mask = 1;
    };
    struct Entry
    {
        Addr tag = 0;
        bool valid = false;
        uint64_t lru = 0;
        std::vector<Target> targets;
    };

    Config config;
    uint64_t lruTick = 0;
    Stats stats_;
    std::vector<std::vector<Entry>> table;
    std::deque<Addr> queues[MaxThreads];
    Addr lastTrigger[MaxThreads]{};

    unsigned setIndex(Addr block) const;
    Addr tagFor(Addr block) const;
    Addr tagMask() const;
    Entry *find(Addr trigger);
    Entry *allocate(Addr trigger);
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_UPSTREAM_PDIP_HH__
