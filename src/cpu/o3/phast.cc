/*
 * Copyright (c) 2026
 * All rights reserved.
 */

#include "cpu/o3/phast.hh"

#include <algorithm>

#include "base/intmath.hh"
#include "base/logging.hh"
#include "params/BaseO3CPU.hh"

namespace gem5
{

namespace o3
{

namespace
{

uint64_t
hash_combine(uint64_t seed, uint64_t value)
{
    return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
}

} // namespace

void
PHAST::init(const BaseO3CPUParams &params)
{
    fatal_if(params.phast_num_rows == 0 ||
                 !isPowerOf2(params.phast_num_rows),
             "PHAST rows per table must be a non-zero power of two.\n");
    fatal_if(params.phast_associativity == 0,
             "PHAST table associativity must be non-zero.\n");
    fatal_if(params.phast_max_counter == 0,
             "PHAST confidence counter maximum must be non-zero.\n");
    fatal_if(params.phast_counter_threshold == 0 ||
                 params.phast_counter_threshold > params.phast_max_counter,
             "PHAST confidence threshold must be in [1, max counter].\n");
    fatal_if(params.phast_tag_bits >= 64,
             "PHAST tag bits must be less than 64.\n");
    fatal_if(params.phast_selected_target_bits > 64,
             "PHAST selected target bits must be at most 64.\n");
    fatal_if(params.phast_history_lengths.empty(),
             "PHAST must have at least one history table.\n");

    for (unsigned i = 1; i < params.phast_history_lengths.size(); ++i) {
        fatal_if(params.phast_history_lengths[i - 1] >=
                     params.phast_history_lengths[i],
                 "PHAST history lengths must be strictly increasing.\n");
    }

    depCheckShift = params.LSQDepCheckShift;
    // PHAST stores distances in the virtual store queue used by MemDepUnit.
    SQEntries = params.SQEntries * params.StoreQueueMultiple;

    unsigned set_bits = 0;
    while ((1ULL << set_bits) < params.phast_num_rows) {
        ++set_bits;
    }

    selectedTargetBits = params.phast_selected_target_bits;
    selectedTargetMask = (selectedTargetBits == 64)
        ? ~0ULL
        : ((1ULL << selectedTargetBits) - 1);

    historySizes = params.phast_history_lengths;
    const unsigned second_target_max_distance =
        params.phast_second_target_max_distance == 0
            ? SQEntries / 2
            : params.phast_second_target_max_distance;
    paths.clear();
    paths.resize(historySizes.size());
    for (auto &path : paths) {
        path.init(set_bits, params.phast_associativity,
                  params.phast_tag_bits, params.phast_max_counter,
                  params.phast_counter_threshold,
                  params.phast_counter_increment,
                  params.phast_counter_decrement,
                  second_target_max_distance);
    }
}

void
PHAST::clear()
{
    for (auto &path : paths) {
        path.clear();
    }
}

BranchHistory
PHAST::filteredHistory(InstSeqNum load_seq_num,
                       const BranchHistory &branch_history) const
{
    BranchHistory filtered;
    for (const auto &entry : branch_history) {
        if (entry.seqNum >= load_seq_num) {
            continue;
        }
        filtered.push_back(entry);
    }
    return filtered;
}

uint64_t
PHAST::makePathHash(Addr load_pc, const BranchHistory &branch_history,
                    unsigned history_len) const
{
    uint64_t hash = hash_combine(load_pc ^ (load_pc >> 11), history_len);
    history_len =
        std::min(history_len, static_cast<unsigned>(branch_history.size()));

    for (unsigned i = 0; i < history_len; ++i) {
        const auto &branch = branch_history[i];
        uint64_t branch_bits = branch.pc ^ (branch.pc >> 7);
        branch_bits ^= branch.target & selectedTargetMask;
        branch_bits ^= static_cast<uint64_t>(branch.taken) << 1;
        branch_bits ^= static_cast<uint64_t>(branch.indirect) << 2;
        branch_bits ^= branch.target >> selectedTargetBits;
        hash = hash_combine(hash, branch_bits);
    }

    return hash;
}

PHASTPredictionResult
PHAST::checkInst(Addr load_pc, InstSeqNum load_seq_num,
                 const BranchHistory &branch_history, bool is_load)
{
    PHASTPredictionResult result;
    if (!is_load) {
        return result;
    }

    BranchHistory filtered = filteredHistory(load_seq_num, branch_history);

    for (int i = static_cast<int>(historySizes.size()) - 1; i >= 0; --i) {
        const unsigned hist_len = historySizes[i];
        if (hist_len > filtered.size()) {
            continue;
        }

        const uint64_t hash = makePathHash(load_pc, filtered, hist_len);
        const auto distances = paths[i].predict(load_pc, hash);
        if (distances.first >= 0 || distances.second >= 0) {
            result.storeQueueDistances = distances;
            result.predBranchHistLength = static_cast<unsigned>(i);
            result.predictorHash = hash;
            return result;
        }
    }

    return result;
}

void
PHAST::violation(Addr load_pc, InstSeqNum load_seq_num,
                 InstSeqNum store_seq_num, Addr,
                 std::ptrdiff_t store_queue_distance, bool predicted,
                 unsigned predicted_path_index, uint64_t predicted_hash,
                 const BranchHistory &branch_history)
{
    BranchHistory filtered = filteredHistory(load_seq_num, branch_history);

    unsigned available_branches = 0;
    for (const auto &entry : filtered) {
        ++available_branches;
        if (entry.seqNum <= store_seq_num) {
            break;
        }
    }

    unsigned actual_index = 0;
    for (unsigned i = 0; i < historySizes.size(); ++i) {
        if (historySizes[i] <= available_branches) {
            actual_index = i;
        } else {
            break;
        }
    }

    const uint64_t actual_hash =
        makePathHash(load_pc, filtered, historySizes[actual_index]);

    if (predicted && predicted_path_index < paths.size() &&
        (predicted_path_index != actual_index ||
         predicted_hash != actual_hash)) {
        paths[predicted_path_index].updateCommit(load_pc, predicted_hash, true);
    }

    if (store_queue_distance >= 0) {
        paths[actual_index].update(load_pc, actual_hash, store_queue_distance);
    }
}

void
PHAST::commit(Addr load_pc, Addr load_addr, unsigned load_size,
              const std::pair<Addr, Addr> &store_addrs,
              const std::pair<unsigned, unsigned> &store_sizes,
              unsigned path_index, uint64_t predictor_hash)
{
    if (path_index >= paths.size()) {
        return;
    }

    auto overlaps = [this](Addr a0, unsigned s0, Addr a1, unsigned s1) {
        if (s0 == 0 || s1 == 0) {
            return false;
        }
        const Addr l0 = a0 >> depCheckShift;
        const Addr l1 = (a0 + s0 - 1) >> depCheckShift;
        const Addr r0 = a1 >> depCheckShift;
        const Addr r1 = (a1 + s1 - 1) >> depCheckShift;
        return r1 >= l0 && r0 <= l1;
    };

    bool misprediction = true;
    if (overlaps(load_addr, load_size, store_addrs.first,
                 store_sizes.first) ||
        overlaps(load_addr, load_size, store_addrs.second,
                 store_sizes.second)) {
        misprediction = false;
    }

    paths[path_index].updateCommit(load_pc, predictor_hash, misprediction);
}

void
PHAST::invalidSQDistance(Addr load_pc, unsigned path_index,
                         uint64_t predictor_hash)
{
    if (path_index >= paths.size()) {
        return;
    }

    paths[path_index].updateCommit(load_pc, predictor_hash, true);
}

int
PHAST::SimplBlockCache::init(uint32_t set_bits, uint32_t _associativity,
                            uint32_t tag_bits, uint32_t max_counter_value,
                            uint32_t counter_threshold,
                            uint32_t counter_increment,
                            uint32_t counter_decrement,
                            unsigned second_target_max_distance)
{
    setBits = set_bits;
    tagBits = tag_bits;
    associativity = _associativity;
    maxCounterValue = max_counter_value;
    counterThreshold = counter_threshold;
    counterIncrement = counter_increment;
    counterDecrement = counter_decrement;
    secondTargetMaxDistance = second_target_max_distance;
    lruCounter = 0;

    cache.clear();
    cache.resize(1ULL << setBits);
    for (auto &set : cache) {
        set.resize(associativity);
    }

    return (1ULL << setBits) * associativity;
}

uint64_t
PHAST::SimplBlockCache::xorFold(uint64_t pc, uint64_t history,
                                unsigned size) const
{
    if (size == 0) {
        return 0;
    }

    const uint64_t mask = (1ULL << size) - 1;
    uint64_t fold = (history & mask) ^ (pc & mask);
    history >>= size;

    while (history) {
        fold ^= (history & mask);
        history >>= size;
    }

    return fold;
}

uint64_t
PHAST::SimplBlockCache::getIndex(Addr pc, uint64_t history) const
{
    pc = (pc ^ (pc >> 2) ^ (pc >> 5));
    return xorFold(pc, history, setBits);
}

uint64_t
PHAST::SimplBlockCache::getTag(Addr pc, uint64_t history) const
{
    pc = (pc ^ (pc >> 3) ^ (pc >> 7));
    return xorFold(pc, history, tagBits);
}

PHAST::SimplBlockCache::Entry *
PHAST::SimplBlockCache::findEntry(Addr pc, uint64_t history)
{
    const auto set = getIndex(pc, history);
    const auto tag = getTag(pc, history);
    for (uint32_t i = 0; i < associativity; ++i) {
        if (cache[set][i].valid && cache[set][i].tag == tag) {
            return &cache[set][i];
        }
    }
    return nullptr;
}

const PHAST::SimplBlockCache::Entry *
PHAST::SimplBlockCache::findEntry(Addr pc, uint64_t history) const
{
    const auto set = getIndex(pc, history);
    const auto tag = getTag(pc, history);
    for (uint32_t i = 0; i < associativity; ++i) {
        if (cache[set][i].valid && cache[set][i].tag == tag) {
            return &cache[set][i];
        }
    }
    return nullptr;
}

PHAST::SimplBlockCache::Entry *
PHAST::SimplBlockCache::getLRUEntry(uint64_t set)
{
    for (uint32_t i = 0; i < associativity; ++i) {
        if (!cache[set][i].valid) {
            return &cache[set][i];
        }
    }

    uint32_t lru_way = 0;
    uint64_t lru_value = cache[set][0].lru;
    for (uint32_t i = 1; i < associativity; ++i) {
        if (cache[set][i].lru < lru_value) {
            lru_way = i;
            lru_value = cache[set][i].lru;
        }
    }
    return &cache[set][lru_way];
}

void
PHAST::SimplBlockCache::updateLRU(Entry *entry)
{
    entry->lru = lruCounter++;
}

std::pair<std::ptrdiff_t, std::ptrdiff_t>
PHAST::SimplBlockCache::predict(Addr pc, uint64_t history) const
{
    const auto *entry = findEntry(pc, history);
    if (entry == nullptr || entry->counter < counterThreshold ||
        (entry->distances.first < 0 && entry->distances.second < 0)) {
        return {-1, -1};
    }

    return entry->distances;
}

void
PHAST::SimplBlockCache::update(Addr pc, uint64_t history,
                               std::ptrdiff_t distance)
{
    if (distance < 0) {
        return;
    }

    const auto set = getIndex(pc, history);
    auto *entry = findEntry(pc, history);
    if (entry == nullptr) {
        entry = getLRUEntry(set);
        entry->valid = true;
        entry->tag = getTag(pc, history);
        entry->distances = {distance, -1};
        entry->counter = maxCounterValue;
    } else if (entry->distances.second < 0 &&
               entry->distances.first != distance &&
               distance < static_cast<std::ptrdiff_t>(secondTargetMaxDistance) &&
               entry->distances.first <
                   static_cast<std::ptrdiff_t>(secondTargetMaxDistance)) {
        entry->distances.second = distance;
        entry->counter = maxCounterValue;
    } else {
        entry->distances = {distance, -1};
        entry->counter = maxCounterValue;
    }

    updateLRU(entry);
}

void
PHAST::SimplBlockCache::updateCommit(Addr pc, uint64_t history,
                                     bool prediction_wrong)
{
    auto *entry = findEntry(pc, history);
    if (entry == nullptr || entry->counter == 0) {
        return;
    }

    if (prediction_wrong) {
        entry->counter = entry->counter > counterDecrement
            ? entry->counter - counterDecrement
            : 0;
    } else if (counterIncrement == 0) {
        entry->counter = maxCounterValue;
    } else {
        entry->counter = std::min(maxCounterValue, entry->counter + counterIncrement);
    }

    updateLRU(entry);
}

void
PHAST::SimplBlockCache::clear()
{
    for (auto &set : cache) {
        for (auto &entry : set) {
            entry.tag = 0;
            entry.distances = {-1, -1};
            entry.lru = 0;
            entry.counter = 0;
            entry.valid = false;
        }
    }
    lruCounter = 0;
}

} // namespace o3
} // namespace gem5
