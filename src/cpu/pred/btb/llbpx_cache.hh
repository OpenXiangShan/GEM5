#ifndef __CPU_PRED_BTB_LLBPX_CACHE_HH__
#define __CPU_PRED_BTB_LLBPX_CACHE_HH__

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>

#include "base/types.hh"

namespace gem5
{
namespace branch_prediction
{
namespace btb_pred
{

namespace llbpx
{

template <class Entry>
class SetAssociativeStore
{
  public:
    SetAssociativeStore() = default;

    SetAssociativeStore(unsigned sets, unsigned ways)
    {
        reset(sets, ways);
    }

    void
    reset(unsigned sets, unsigned ways)
    {
        numSets = std::max(1u, sets);
        numWays = std::max(1u, ways);
        data.assign(numSets, std::vector<Entry>(numWays));
        tick = 0;
    }

    unsigned
    sets() const
    {
        return numSets;
    }

    unsigned
    ways() const
    {
        return numWays;
    }

    unsigned
    setIndex(Addr key) const
    {
        assert(numSets > 0);
        return key % numSets;
    }

    Entry *
    find(Addr key, Addr tag)
    {
        auto &set = data[setIndex(key)];
        for (auto &entry : set) {
            if (entry.valid && entry.tag == tag) {
                entry.lastTouch = ++tick;
                return &entry;
            }
        }
        return nullptr;
    }

    const Entry *
    find(Addr key, Addr tag) const
    {
        const auto &set = data[setIndex(key)];
        for (const auto &entry : set) {
            if (entry.valid && entry.tag == tag) {
                return &entry;
            }
        }
        return nullptr;
    }

    Entry &
    allocate(Addr key, Addr tag)
    {
        auto &set = data[setIndex(key)];
        auto victim = std::min_element(set.begin(), set.end(),
            [](const Entry &lhs, const Entry &rhs) {
                if (lhs.valid != rhs.valid) {
                    return !lhs.valid;
                }
                const auto lhsScore = lhs.replacementScore();
                const auto rhsScore = rhs.replacementScore();
                if (lhsScore != rhsScore) {
                    return lhsScore < rhsScore;
                }
                return lhs.lastTouch < rhs.lastTouch;
            });
        victim->reset(tag);
        victim->lastTouch = ++tick;
        return *victim;
    }

  private:
    unsigned numSets{1};
    unsigned numWays{1};
    uint64_t tick{0};
    std::vector<std::vector<Entry>> data;
};

} // namespace llbpx

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_BTB_LLBPX_CACHE_HH__
