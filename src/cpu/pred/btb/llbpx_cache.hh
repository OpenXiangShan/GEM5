#ifndef __CPU_PRED_BTB_LLBPX_CACHE_HH__
#define __CPU_PRED_BTB_LLBPX_CACHE_HH__

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <list>
#include <unordered_map>
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
    validEntries() const
    {
        unsigned count = 0;
        for (const auto &set : data) {
            count += std::count_if(set.begin(), set.end(),
                [](const Entry &entry) { return entry.valid; });
        }
        return count;
    }

    unsigned
    setOccupancy(unsigned set) const
    {
        assert(set < data.size());
        return std::count_if(data[set].begin(), data[set].end(),
            [](const Entry &entry) { return entry.valid; });
    }

    const std::vector<Entry> &
    setEntries(unsigned set) const
    {
        assert(set < data.size());
        return data[set];
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

    bool
    hasAnyForKey(Addr key) const
    {
        const auto &set = data[setIndex(key)];
        return std::any_of(set.begin(), set.end(),
            [](const Entry &entry) { return entry.valid; });
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

template <class Entry>
class SparseSetAssociativeStore
{
  private:
    using KeyValue = std::pair<Addr, Entry>;
    using Set = std::list<KeyValue>;
    using Iterator = typename Set::iterator;

  public:
    SparseSetAssociativeStore() = default;

    SparseSetAssociativeStore(unsigned maxSize, unsigned assoc)
    {
        reset(maxSize, assoc);
    }

    void
    reset(unsigned maxSize, unsigned assoc)
    {
        numWays = std::max(1u, assoc);
        unlimited = maxSize == 0;
        if (unlimited) {
            capacity = 0;
            numSets = 1;
        } else {
            capacity = std::max(numWays, maxSize);
            numSets = std::max(1u, capacity / numWays);
            assert((numSets & (numSets - 1)) == 0);
            assert(numSets * numWays == capacity);
        }
        setMask = numSets - 1;
        index.clear();
        data.clear();
        data.resize(numSets);
    }

    unsigned
    maxSize() const
    {
        return capacity;
    }

    unsigned
    ways() const
    {
        return numWays;
    }

    unsigned
    sets() const
    {
        return numSets;
    }

    bool
    isUnlimited() const
    {
        return unlimited;
    }

    unsigned
    entries() const
    {
        return index.size();
    }

    unsigned
    setOccupancy(unsigned set) const
    {
        assert(set < data.size());
        return data[set].size();
    }

    Entry *
    get(Addr key)
    {
        auto it = index.find(key);
        return it == index.end() ? nullptr : &it->second->second;
    }

    const Entry *
    get(Addr key) const
    {
        auto it = index.find(key);
        return it == index.end() ? nullptr : &it->second->second;
    }

    bool
    exists(Addr key) const
    {
        return index.find(key) != index.end();
    }

    Set &
    getSet(Addr key)
    {
        return data[setIndex(key)];
    }

    const Set &
    getSet(Addr key) const
    {
        return data[setIndex(key)];
    }

    Entry *
    getVictim(Addr key)
    {
        auto &set = getSet(key);
        if (unlimited || set.size() < numWays) {
            return nullptr;
        }
        return &set.back().second;
    }

    void
    touch(Addr key)
    {
        auto it = index.find(key);
        if (it == index.end()) {
            return;
        }
        auto &set = getSet(key);
        set.splice(set.begin(), set, it->second);
    }

    void
    bump(Addr key, bool front = true)
    {
        auto it = index.find(key);
        if (it == index.end()) {
            return;
        }
        auto &set = getSet(key);
        if (front) {
            set.splice(set.begin(), set, it->second);
        } else {
            set.splice(set.end(), set, it->second);
        }
    }

    Entry *
    insert(Addr key)
    {
        if (auto *entry = get(key)) {
            return entry;
        }

        auto &set = getSet(key);
        if (!unlimited && set.size() >= numWays) {
            auto last = std::prev(set.end());
            index.erase(last->first);
            set.pop_back();
        }

        auto it = set.emplace(set.begin(), KeyValue(key, Entry()));
        index[key] = it;
        return &it->second;
    }

  private:
    unsigned
    setIndex(Addr key) const
    {
        return key & setMask;
    }

    unsigned capacity{1};
    unsigned numWays{1};
    unsigned numSets{1};
    Addr setMask{0};
    bool unlimited{false};
    std::unordered_map<Addr, Iterator> index;
    std::vector<Set> data;
};

} // namespace llbpx

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_BTB_LLBPX_CACHE_HH__
