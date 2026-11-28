#include "cpu/pred/btb/h2p_table.hh"

#include <algorithm>
#include <stdexcept>

namespace gem5::branch_prediction::btb_pred
{

H2PTable::H2PTable(unsigned entries, unsigned lineBytes, unsigned ways,
                   unsigned branchesPerEntry, unsigned counterBits,
                   unsigned counterThreshold)
    : lineBytes(lineBytes),
      counterMax(counterBits >= 1 && counterBits <= 8 ?
                 (1u << counterBits) - 1 : 0),
      counterThreshold(counterThreshold),
      numSets(ways ? entries / ways : 0)
{
    if (entries == 0 || ways == 0 || entries % ways != 0) {
        throw std::invalid_argument(
            "H2P entries must be non-zero and divisible by non-zero ways");
    }
    if (lineBytes < 2 || (lineBytes & (lineBytes - 1)) != 0) {
        throw std::invalid_argument(
            "H2P line bytes must be a power of two and at least two");
    }
    if (branchesPerEntry == 0 || branchesPerEntry > lineBytes / 2) {
        throw std::invalid_argument(
            "H2P branches per entry must be in [1, line bytes / 2]");
    }
    if (counterMax == 0 || counterThreshold == 0 ||
        counterThreshold > counterMax) {
        throw std::invalid_argument(
            "H2P counter bits must be in [1, 8] and threshold in [1, max]");
    }
    table.resize(numSets);
    for (auto &set : table) {
        set.resize(ways);
        for (auto &line : set)
            line.branches.resize(branchesPerEntry);
    }
}

unsigned
H2PTable::setIndex(Addr pc) const
{
    return (pc / lineBytes) % numSets;
}

H2PTable::SetEntry *
H2PTable::findLine(Addr pc)
{
    const Addr line = pc / lineBytes;
    for (auto &entry : table[setIndex(pc)]) {
        if (entry.valid && entry.line == line)
            return &entry;
    }
    return nullptr;
}

H2PTable::LookupResult
H2PTable::lookup(Addr pc)
{
    auto *line = findLine(pc);
    if (!line)
        return {};

    ++useClock;
    line->lastUse = useClock;
    for (auto &branch : line->branches) {
        if (branch.valid && branch.pc == pc) {
            return {branch.counter >= counterThreshold, true};
        }
    }
    return {};
}

H2PTable::TrainResult
H2PTable::trainMispred(Addr pc, bool allowAllocate)
{
    TrainResult result;
    ++useClock;
    auto *line = findLine(pc);
    if (line) {
        line->lastUse = useClock;
        for (auto &branch : line->branches) {
            if (branch.valid && branch.pc == pc) {
                branch.counter = std::min<unsigned>(
                    counterMax, unsigned(branch.counter) + 1);
                return result;
            }
        }
    }

    if (!allowAllocate) {
        result.allocationFiltered = true;
        return result;
    }

    if (!line) {
        auto &set = table[setIndex(pc)];
        line = &set[0];
        for (auto &candidate : set) {
            if (!candidate.valid) {
                line = &candidate;
                break;
            }
            // Reclaim entries whose branch counters have all aged to zero
            // before evicting an entry that still tracks active branches.
            const bool countersAvailable = std::all_of(
                candidate.branches.begin(), candidate.branches.end(),
                [](const auto &branch) {
                    return !branch.valid || branch.counter == 0;
                });
            if (countersAvailable) {
                line = &candidate;
                break;
            }
            if (candidate.lastUse < line->lastUse)
                line = &candidate;
        }
        result.replaced = line->valid;
        std::fill(line->branches.begin(), line->branches.end(), BranchEntry{});
        line->valid = true;
        line->line = pc / lineBytes;
    }

    line->lastUse = useClock;
    auto slot = std::find_if(line->branches.begin(), line->branches.end(),
        [](const auto &branch) { return !branch.valid || branch.counter == 0; });
    if (slot == line->branches.end()) {
        result.dropped = true;
        return result;
    }
    *slot = BranchEntry{pc, 1, true};
    result.allocated = true;
    return result;
}

void
H2PTable::age()
{
    for (auto &set : table) {
        for (auto &line : set) {
            if (!line.valid)
                continue;
            for (auto &branch : line.branches) {
                if (!branch.valid || branch.counter == 0)
                    continue;
                --branch.counter;
                if (branch.counter == 0)
                    branch.valid = false;
            }
        }
    }
}

} // namespace gem5::branch_prediction::btb_pred
