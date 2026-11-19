#ifndef __MEM_CACHE_PREFETCH_COMPOUND_BOP_POLICY_HH__
#define __MEM_CACHE_PREFETCH_COMPOUND_BOP_POLICY_HH__

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace gem5
{

namespace prefetch
{
namespace compound_bop
{

enum class Family : uint8_t
{
    VBOP = 0,
    PBOP = 1,
};

enum class Role : uint8_t
{
    Primary = 0,
    Extra = 1,
};

struct FamilyOffset
{
    int64_t offset;
    Family family;
};

inline std::vector<FamilyOffset>
mergeFamilyOffsets(const std::vector<int> &vbop_offsets,
        const std::vector<int> &pbop_offsets)
{
    std::vector<FamilyOffset> merged;
    merged.reserve(vbop_offsets.size() + pbop_offsets.size());

    // The entries before VBOP's minimum are the historical priority prefix.
    // The remainder is a deterministic sorted union of both families.
    const auto body_begin = std::min_element(
        vbop_offsets.begin(), vbop_offsets.end());
    for (auto it = vbop_offsets.begin(); it != body_begin; ++it) {
        merged.push_back({*it, Family::VBOP});
    }
    const size_t prefix_size = merged.size();
    for (auto it = body_begin; it != vbop_offsets.end(); ++it) {
        merged.push_back({*it, Family::VBOP});
    }
    for (const auto offset : pbop_offsets) {
        merged.push_back({offset, Family::PBOP});
    }
    std::sort(merged.begin() + prefix_size, merged.end(),
        [](const FamilyOffset &lhs, const FamilyOffset &rhs) {
            return lhs.offset < rhs.offset;
        });
    return merged;
}

struct StudentEntry
{
    bool valid = false;
    Family family = Family::VBOP;
    int64_t offset = 0;
    double confidence = 0.0;
    uint32_t lastCoverage = 0;
    uint32_t currentCoverage = 0;
};

inline auto
selectionKey(const StudentEntry &entry)
{
    return std::make_tuple(
        entry.currentCoverage,
        entry.confidence,
        -std::llabs(entry.offset),
        -entry.offset);
}

inline auto
worstKey(const StudentEntry &entry)
{
    return std::make_tuple(
        entry.currentCoverage,
        entry.confidence,
        std::llabs(entry.offset),
        entry.offset);
}

inline auto
victimKey(const StudentEntry &entry)
{
    return std::make_tuple(
        entry.confidence,
        entry.lastCoverage,
        -std::llabs(entry.offset),
        entry.offset);
}

inline std::optional<size_t>
pickBest(const std::vector<StudentEntry> &entries, Family family)
{
    std::optional<size_t> best;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (!entries[i].valid || entries[i].family != family) {
            continue;
        }
        if (!best || selectionKey(entries[i]) > selectionKey(entries[*best])) {
            best = i;
        }
    }
    return best;
}

inline std::optional<size_t>
pickWorst(const std::vector<StudentEntry> &entries, Family family)
{
    std::optional<size_t> worst;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (!entries[i].valid || entries[i].family != family) {
            continue;
        }
        if (!worst || worstKey(entries[i]) < worstKey(entries[*worst])) {
            worst = i;
        }
    }
    return worst;
}

inline std::optional<size_t>
pickVictim(const std::vector<StudentEntry> &entries, Family family)
{
    std::optional<size_t> victim;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (!entries[i].valid || entries[i].family != family) {
            continue;
        }
        if (!victim || victimKey(entries[i]) < victimKey(entries[*victim])) {
            victim = i;
        }
    }
    return victim;
}

inline bool
qualifies(unsigned score, unsigned bad_score)
{
    return score > bad_score;
}

inline bool
issueGateAllows(uint8_t confidence, unsigned threshold)
{
    return confidence < threshold;
}

inline uint8_t
updatedIssueConfidence(uint8_t confidence, bool qualified,
        unsigned pass_decrement, unsigned fail_increment,
        unsigned maximum)
{
    assert(maximum <= UINT8_MAX);
    if (qualified) {
        return confidence > pass_decrement ?
            confidence - pass_decrement : 0;
    }
    return static_cast<uint8_t>(std::min<unsigned>(
        maximum, confidence + fail_increment));
}

inline uint32_t
marginalCoverage(uint32_t extra_coverage, uint32_t overlap)
{
    return extra_coverage >= overlap ? extra_coverage - overlap : 0;
}

struct SelectedOffset
{
    int64_t offset = 0;
    Family family = Family::VBOP;
};

inline std::vector<SelectedOffset>
applyFamilyGates(const std::vector<SelectedOffset> &selected,
        bool vbop_allowed, bool pbop_allowed)
{
    std::vector<SelectedOffset> gated;
    gated.reserve(selected.size());
    for (const auto &candidate : selected) {
        const bool allowed = candidate.family == Family::VBOP ?
            vbop_allowed : pbop_allowed;
        if (allowed) {
            gated.push_back(candidate);
        }
    }
    return gated;
}

class WeightedArbiter
{
  public:
    WeightedArbiter(unsigned primary_slots, unsigned extra_slots)
    {
        assert(primary_slots > 0);
        assert(extra_slots > 0);
        schedule.insert(
            schedule.end(), primary_slots, Role::Primary);
        schedule.insert(schedule.end(), extra_slots, Role::Extra);
    }

    std::optional<Role>
    choose(bool primary_available, bool extra_available)
    {
        if (!primary_available && !extra_available) {
            return std::nullopt;
        }

        const Role preferred = schedule[cursor];
        cursor = (cursor + 1) % schedule.size();
        if (preferred == Role::Primary) {
            return primary_available ? Role::Primary : Role::Extra;
        }
        return extra_available ? Role::Extra : Role::Primary;
    }

  private:
    std::vector<Role> schedule;
    size_t cursor = 0;
};

template <typename T>
class OverwriteFifo
{
  public:
    explicit OverwriteFifo(size_t capacity_) : capacity(capacity_)
    {
        assert(capacity > 0);
    }

    std::optional<T>
    push(T value)
    {
        std::optional<T> overwritten;
        if (queue.size() == capacity) {
            overwritten = std::move(queue.front());
            queue.pop_front();
        }
        queue.push_back(std::move(value));
        return overwritten;
    }

    std::optional<T>
    pop()
    {
        if (queue.empty()) {
            return std::nullopt;
        }
        T value = std::move(queue.front());
        queue.pop_front();
        return value;
    }

    bool empty() const { return queue.empty(); }
    size_t size() const { return queue.size(); }

    std::deque<T> &entries() { return queue; }
    const std::deque<T> &entries() const { return queue; }

  private:
    size_t capacity;
    std::deque<T> queue;
};

} // namespace compound_bop
} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_COMPOUND_BOP_POLICY_HH__
