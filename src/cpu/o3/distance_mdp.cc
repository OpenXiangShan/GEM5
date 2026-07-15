#include "cpu/o3/distance_mdp.hh"

#include <cassert>

namespace gem5
{
namespace o3
{

uint16_t
DistanceMDP::hash(Addr pc)
{
    constexpr Addr tag_mask = (Addr(1) << TagBits) - 1;
    constexpr Addr payload_mask = (Addr(1) << (VAddrBits - 1)) - 1;
    Addr payload = (pc >> 1) & payload_mask;
    uint16_t tag = 0;

    for (unsigned offset = 0; offset < VAddrBits - 1; offset += TagBits) {
        tag ^= static_cast<uint16_t>((payload >> offset) & tag_mask);
    }
    return tag & tag_mask;
}

std::optional<uint8_t>
DistanceMDP::encodeDistance(size_t load_boundary, size_t store_index)
{
    if (load_boundary <= store_index) {
        return std::nullopt;
    }

    const size_t age = load_boundary - store_index;
    if (age > MaxDistance) {
        return std::nullopt;
    }

    const auto distance = static_cast<uint8_t>((-age) & MaxDistance);
    assert(distance != 0);
    return distance;
}

std::optional<size_t>
DistanceMDP::decodeTarget(size_t load_boundary, unsigned distance)
{
    if (distance == 0 || distance > MaxDistance) {
        return std::nullopt;
    }

    const size_t age = (MaxDistance + 1) - distance;
    if (load_boundary < age) {
        return std::nullopt;
    }
    return load_boundary - age;
}

DistanceMDP::Prediction
DistanceMDP::lookup(Addr pc, uint64_t result_cycle)
{
    Prediction prediction;
    prediction.tag = hash(pc);

    const auto index = find(prediction.tag);
    if (!index) {
        return prediction;
    }

    Entry &matched = entries[*index];
    prediction.strictExpired = expireStrict(matched, result_cycle);
    prediction.hit = true;
    prediction.entryIndex = *index;
    prediction.counter = matched.counter;
    prediction.hasDistance = matched.hasDistance;
    prediction.multiDistance = matched.multiDistance;
    prediction.waitAllStore = matched.waitAllStore || matched.multiDistance;
    prediction.distance = matched.distance;
    return prediction;
}

bool
DistanceMDP::commitLookup(unsigned entry_index, uint16_t tag)
{
    if (entry_index >= NumEntries || !entries[entry_index].valid ||
        entries[entry_index].tag != tag) {
        return false;
    }

    touch(entry_index);
    return true;
}

DistanceMDP::TrainResult
DistanceMDP::train(Addr load_pc, size_t load_boundary,
                   size_t store_index, uint64_t cycle)
{
    TrainResult result;
    result.tag = hash(load_pc);
    if (load_boundary <= store_index) {
        return result;
    }

    const auto distance = encodeDistance(load_boundary, store_index);
    result.strictFallback = !distance;
    // Zero is reserved for strict-only entries without a target store.
    result.distance = distance.value_or(0);

    const auto matched_index = find(result.tag);
    if (matched_index) {
        result.entryIndex = *matched_index;
        Entry &matched = entries[*matched_index];
        result.strictExpired = expireStrict(matched, cycle);
        result.action = (matched.waitAllStore || matched.multiDistance) ?
            TrainAction::StrictRefresh : TrainAction::StrictUpgrade;
        result.distanceChanged = distance && matched.hasDistance &&
            matched.distance != *distance;
        result.multiDistance = matched.multiDistance ||
            result.distanceChanged ||
            (result.strictFallback && matched.hasDistance);
        matched.multiDistance = result.multiDistance;
        // An overflow has no safe target, so it must discard any old one.
        matched.hasDistance = !result.strictFallback;
        matched.distance = result.distance;
        matched.counter = MaxCounter;
        matched.waitAllStore = true;
        matched.strictExpireCycle = cycle + StrictTimeout;
        touch(*matched_index);
        return result;
    }

    unsigned target = NumEntries;
    for (unsigned i = 0; i < NumEntries; ++i) {
        if (!entries[i].valid) {
            target = i;
            break;
        }
    }

    if (target == NumEntries) {
        target = replacementVictim();
        result.action = TrainAction::AllocateEviction;
    } else {
        result.action = TrainAction::AllocateInvalid;
        ++validEntries;
    }

    result.entryIndex = target;
    entries[target] = Entry{
        .valid = true,
        .tag = result.tag,
        .counter = MaxCounter,
        .hasDistance = !result.strictFallback,
        .multiDistance = false,
        .waitAllStore = result.strictFallback,
        .distance = result.distance,
        .strictExpireCycle = result.strictFallback ? cycle + StrictTimeout : 0,
    };
    touch(target);
    return result;
}

DistanceMDP::FeedbackResult
DistanceMDP::feedback(unsigned entry_index, uint16_t tag,
                      MDPFeedbackSource source)
{
    FeedbackResult result;
    result.entryIndex = entry_index;
    result.tag = tag;

    if (entry_index >= NumEntries) {
        return result;
    }

    Entry &matched = entries[entry_index];
    if (!matched.valid) {
        result.action = FeedbackAction::InvalidEntry;
        return result;
    }
    if (matched.tag != tag) {
        result.action = FeedbackAction::TagMismatch;
        return result;
    }

    result.oldCounter = matched.counter;
    const bool positive = source == MDPFeedbackSource::StoreQueue;
    if (positive) {
        matched.counter = std::min<uint8_t>(matched.counter + 1, MaxCounter);
        result.action = matched.counter == result.oldCounter ?
            FeedbackAction::Saturated : FeedbackAction::Increment;
    } else {
        assert(matched.counter > 0);
        --matched.counter;
        if (matched.counter == 0) {
            matched = Entry{};
            assert(validEntries > 0);
            --validEntries;
            result.action = FeedbackAction::ClearCounterZero;
        } else {
            result.action = FeedbackAction::Decrement;
        }
    }
    result.newCounter = matched.counter;
    return result;
}

void
DistanceMDP::clear()
{
    entries.fill(Entry{});
    plru.reset();
    validEntries = 0;
}

unsigned
DistanceMDP::occupancy() const
{
    return validEntries;
}

const DistanceMDP::Entry &
DistanceMDP::entry(unsigned index) const
{
    assert(index < NumEntries);
    return entries[index];
}

unsigned
DistanceMDP::replacementVictim() const
{
    unsigned node = 0;
    unsigned leaf = 0;
    for (unsigned level = 0; level < PlruLevels; ++level) {
        const unsigned direction = plru[node] ? 1 : 0;
        leaf = (leaf << 1) | direction;
        node = node * 2 + 1 + direction;
    }
    return leaf;
}

std::optional<unsigned>
DistanceMDP::find(uint16_t tag) const
{
    for (unsigned i = 0; i < NumEntries; ++i) {
        if (entries[i].valid && entries[i].tag == tag) {
            return i;
        }
    }
    return std::nullopt;
}

bool
DistanceMDP::expireStrict(Entry &entry, uint64_t cycle)
{
    if (!entry.waitAllStore || cycle < entry.strictExpireCycle) {
        return false;
    }
    entry.waitAllStore = false;
    entry.strictExpireCycle = 0;
    return true;
}

void
DistanceMDP::touch(unsigned index)
{
    assert(index < NumEntries);
    unsigned node = 0;
    for (unsigned level = 0; level < PlruLevels; ++level) {
        const unsigned direction =
            (index >> (PlruLevels - level - 1)) & 1U;
        plru[node] = direction == 0;
        node = node * 2 + 1 + direction;
    }
}

} // namespace o3
} // namespace gem5
