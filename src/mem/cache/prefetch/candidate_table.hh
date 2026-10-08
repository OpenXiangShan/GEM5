// Fixed-capacity candidate ownership with generation-checked, O(1) lookup.
#ifndef __MEM_CACHE_PREFETCH_CANDIDATE_TABLE_HH__
#define __MEM_CACHE_PREFETCH_CANDIDATE_TABLE_HH__

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace gem5::prefetch
{

template <typename Owner>
class CandidateTable
{
  private:
    struct Slot
    {
        Owner owner{};
        uint32_t generation{0};
        uint16_t requestor{0};
        bool valid{false};
    };
    std::vector<Slot> slots;
    std::vector<uint16_t> freeSlots;

  public:
    explicit CandidateTable(unsigned capacity) : slots(capacity)
    {
        if (!capacity || capacity > 65536)
            throw std::invalid_argument("candidate capacity must be 1..65536");
        freeSlots.reserve(capacity);
        for (unsigned i = capacity; i > 0; --i)
            freeSlots.push_back(i - 1);
    }

    // ID bits: requestor[63:48], generation[47:16], slot[15:0]. Zero is
    // reserved for untracked requests. No live owner is evicted on overflow.
    uint64_t allocate(uint16_t requestor, const Owner &owner)
    {
        if (freeSlots.empty())
            return 0;
        const unsigned index = freeSlots.back();
        freeSlots.pop_back();
        auto &slot = slots[index];
        if (++slot.generation == 0)
            ++slot.generation;
        slot.owner = owner;
        slot.requestor = requestor;
        slot.valid = true;
        return (uint64_t(requestor) << 48) |
            (uint64_t(slot.generation) << 16) | index;
    }

    Owner *find(uint64_t id)
    {
        const unsigned index = id & 0xffff;
        if (!id || index >= slots.size())
            return nullptr;
        auto &slot = slots[index];
        return slot.valid && slot.requestor == (id >> 48) &&
            slot.generation == uint32_t(id >> 16) ? &slot.owner : nullptr;
    }

    const Owner *find(uint64_t id) const
    {
        return const_cast<CandidateTable *>(this)->find(id);
    }

    bool erase(uint64_t id)
    {
        if (!find(id))
            return false;
        const unsigned index = id & 0xffff;
        slots[index].valid = false;
        slots[index].owner = {};
        freeSlots.push_back(index);
        return true;
    }

    unsigned size() const { return slots.size() - freeSlots.size(); }
    unsigned capacity() const { return slots.size(); }
    bool full() const { return freeSlots.empty(); }
};

// Shared by production owners and the lifecycle tests. A pre-issue terminal
// event consumes its owner, but never releases another candidate's credit.
struct CandidateLifecycle
{
    bool hasIssued{false};
    bool terminal{false};

    bool issue()
    {
        if (hasIssued || terminal)
            return false;
        hasIssued = true;
        return true;
    }

    bool finish()
    {
        if (terminal)
            return false;
        terminal = true;
        return true;
    }
};

} // namespace gem5::prefetch
#endif
