#ifndef __MEM_CACHE_PREFETCH_TEMPORAL_STORAGE_HH__
#define __MEM_CACHE_PREFETCH_TEMPORAL_STORAGE_HH__

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace gem5::prefetch::temporal
{
struct PoolRef
{
    static constexpr unsigned Invalid = 0xffff;
    unsigned slot{Invalid};
    uint8_t length{0};
    bool compact{false};
    bool valid() const { return slot != Invalid; }
};

// Fixed-size slots in two bounded pools. Free links occupy the first payload
// word of a free slot, rather than a second per-slot allocation structure.
class RecordPools
{
    const unsigned length, compactCapacity, fullCapacity;
    std::vector<uint16_t> compactData;
    std::vector<uint64_t> fullData;
    unsigned compactHead, fullHead;
    unsigned compactLive{0}, fullLive{0};

    unsigned allocate(bool compact)
    {
        auto &head = compact ? compactHead : fullHead;
        if (head == PoolRef::Invalid)
            return head;
        const unsigned slot = head;
        head = compact ? compactData[slot * length] : fullData[slot * length];
        ++(compact ? compactLive : fullLive);
        return slot;
    }

    void release(PoolRef reference)
    {
        if (!reference.valid())
            return;
        auto &head = reference.compact ? compactHead : fullHead;
        if (reference.compact)
            compactData[reference.slot * length] = head;
        else
            fullData[reference.slot * length] = head;
        head = reference.slot;
        --(reference.compact ? compactLive : fullLive);
    }

  public:
    uint64_t compactWrites{0}, fullWrites{0}, fallbacks{0}, failures{0};
    uint64_t formatSwitches{0};
    RecordPools(unsigned recordLength, unsigned compactSlots, unsigned fullSlots)
        : length(recordLength), compactCapacity(compactSlots),
          fullCapacity(fullSlots), compactData(recordLength * compactSlots),
          fullData(recordLength * fullSlots),
          compactHead(compactSlots ? 0 : PoolRef::Invalid),
          fullHead(fullSlots ? 0 : PoolRef::Invalid)
    {
        if (!length || length > 16 || compactSlots >= PoolRef::Invalid ||
            fullSlots >= PoolRef::Invalid)
            throw std::invalid_argument("Invalid TP record pools");
        for (unsigned i = 0; i < compactSlots; ++i)
            compactData[i * length] = i + 1 == compactSlots ?
                PoolRef::Invalid : i + 1;
        for (unsigned i = 0; i < fullSlots; ++i)
            fullData[i * length] = i + 1 == fullSlots ?
                PoolRef::Invalid : i + 1;
    }

    static bool encodable(uint64_t trigger, const std::vector<uint64_t> &lines)
    {
        for (auto line : lines)
            if (line >= trigger ? line - trigger > 32767 :
                trigger - line > 32768)
                return false;
        return true;
    }

    // Failure leaves both the old payload and its reference intact. A compact
    // record may use a full slot when its preferred pool is full.
    bool store(PoolRef &reference, uint64_t trigger,
               const std::vector<uint64_t> &lines)
    {
        if (lines.empty() || lines.size() > length)
            throw std::invalid_argument("Invalid TP stored record length");
        bool compact = encodable(trigger, lines);
        unsigned slot = reference.valid() && reference.compact == compact ?
            reference.slot : allocate(compact);
        if (slot == PoolRef::Invalid && compact) {
            ++fallbacks;
            compact = false;
            slot = reference.valid() && !reference.compact ? reference.slot :
                allocate(false);
        }
        if (slot == PoolRef::Invalid) {
            ++failures;
            return false;
        }
        if (reference.valid() && (reference.compact != compact ||
                                  reference.slot != slot)) {
            formatSwitches += reference.compact != compact;
            release(reference);
        }
        for (unsigned i = 0; i < lines.size(); ++i) {
            if (compact) {
                const auto delta = lines[i] >= trigger ?
                    int64_t(lines[i] - trigger) : -int64_t(trigger - lines[i]);
                compactData[slot * length + i] = uint16_t(delta);
            } else {
                fullData[slot * length + i] = lines[i];
            }
        }
        reference = {slot, uint8_t(lines.size()), compact};
        ++(compact ? compactWrites : fullWrites);
        return true;
    }

    std::vector<uint64_t> load(PoolRef reference, uint64_t trigger) const
    {
        if (!reference.valid() || !reference.length || reference.length > length ||
            reference.slot >= (reference.compact ? compactCapacity : fullCapacity))
            throw std::invalid_argument("Invalid TP pool reference");
        std::vector<uint64_t> lines;
        lines.reserve(reference.length);
        for (unsigned i = 0; i < reference.length; ++i) {
            if (!reference.compact) {
                lines.push_back(fullData[reference.slot * length + i]);
                continue;
            }
            const auto word = compactData[reference.slot * length + i];
            const int32_t delta = word < 32768 ? word : int32_t(word) - 65536;
            if (delta < 0 ? trigger < uint64_t(-delta) :
                trigger > std::numeric_limits<uint64_t>::max() - delta)
                throw std::overflow_error("TP relative line overflow");
            lines.push_back(delta < 0 ? trigger - uint64_t(-delta) :
                                        trigger + uint64_t(delta));
        }
        return lines;
    }

    unsigned compactOccupancy() const { return compactLive; }
    unsigned fullOccupancy() const { return fullLive; }
    uint64_t allocatedBytes() const
    {
        return compactData.capacity() * sizeof(uint16_t) +
            fullData.capacity() * sizeof(uint64_t);
    }
};
} // namespace gem5::prefetch::temporal
#endif
