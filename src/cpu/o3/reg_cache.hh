#ifndef __CPU_O3_REG_CACHE_HH__
#define __CPU_O3_REG_CACHE_HH__

#include <algorithm>
#include <array>
#include <cassert>

namespace gem5
{

namespace o3
{

// Register IDs in MRU-to-LRU order. Inserting a resident ID does not touch it.
class RegCache
{
  public:
    static constexpr unsigned MaxEntries = 28;

    explicit RegCache(unsigned capacity) : capacity(capacity) { assert(capacity > 0 && capacity <= MaxEntries); }

    bool contains(int key) const
    {
        return std::find(entries.begin(), entries.begin() + count, key) != entries.begin() + count;
    }

    void insert(int key)
    {
        if (contains(key)) {
            return;
        }
        count = std::min(count + 1, capacity);
        std::move_backward(entries.begin(), entries.begin() + count - 1, entries.begin() + count);
        entries[0] = key;
    }

    bool get(int key)
    {
        auto end = entries.begin() + count;
        auto it = std::find(entries.begin(), end, key);
        if (it == end) {
            return false;
        }
        std::move_backward(entries.begin(), it, it + 1);
        entries[0] = key;
        return true;
    }

  private:
    std::array<int, MaxEntries> entries;
    unsigned count = 0;
    const unsigned capacity;
};

}  // namespace o3
}  // namespace gem5

#endif  // __CPU_O3_REG_CACHE_HH__
