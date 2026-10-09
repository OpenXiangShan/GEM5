#ifndef __MEM_CACHE_PREFETCH_BANKED_QUEUE_HH__
#define __MEM_CACHE_PREFETCH_BANKED_QUEUE_HH__

#include <cassert>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace gem5
{
namespace prefetch
{

/** Independent overwrite FIFOs; an entry cannot borrow another bank's space. */
template <class T>
class BankedQueue
{
  private:
    const unsigned capacity;
    std::vector<std::deque<T>> queues;

  public:
    BankedQueue(unsigned banks, unsigned entries)
        : capacity(entries), queues(banks)
    {
        assert(banks && entries);
    }

    unsigned banks() const { return queues.size(); }
    unsigned size(unsigned bank) const { return queues.at(bank).size(); }
    bool empty(unsigned bank) const { return queues.at(bank).empty(); }
    const T &front(unsigned bank) const { return queues.at(bank).front(); }

    std::optional<T>
    push(unsigned bank, T value)
    {
        auto &queue = queues.at(bank);
        std::optional<T> victim;
        if (queue.size() == capacity) {
            victim.emplace(std::move(queue.front()));
            queue.pop_front();
        }
        queue.push_back(std::move(value));
        return victim;
    }

    T
    pop(unsigned bank)
    {
        auto &queue = queues.at(bank);
        assert(!queue.empty());
        T value = std::move(queue.front());
        queue.pop_front();
        return value;
    }

    /** One head per source, highest priority source first, one winner per bank. */
    static std::vector<int>
    select(unsigned banks, const std::vector<int> &offered_banks)
    {
        std::vector<int> winners(banks, -1);
        for (unsigned source = 0; source < offered_banks.size(); ++source) {
            const int bank = offered_banks[source];
            if (bank < 0)
                continue;
            assert(static_cast<unsigned>(bank) < banks);
            if (winners[bank] < 0)
                winners[bank] = source;
        }
        return winners;
    }
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_BANKED_QUEUE_HH__
