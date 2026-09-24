#include "cpu/pred/btb/h2p_table.hh"

#include <algorithm>
#include <cassert>

namespace gem5::branch_prediction::btb_pred
{

H2PTable::H2PTable(unsigned entries)
    : numEntries(entries), numSets(std::max(1u, entries / Ways)),
      table(numSets)
{
    assert(entries > 0 && entries % Ways == 0);
}

unsigned
H2PTable::setIndex(Addr pc) const
{
    return (pc / LineBytes) % numSets;
}

H2PTable::SetEntry *
H2PTable::findLine(Addr pc)
{
    const Addr line = pc / LineBytes;
    for (auto &entry : table[setIndex(pc)]) {
        if (entry.valid && entry.line == line)
            return &entry;
    }
    return nullptr;
}

const H2PTable::SetEntry *
H2PTable::findLine(Addr pc) const
{
    const Addr line = pc / LineBytes;
    for (const auto &entry : table[setIndex(pc)]) {
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
            branch.lastUse = useClock;
            return {branch.counter > 2, true};
        }
    }
    return {};
}

H2PTable::TrainResult
H2PTable::trainMispred(Addr pc)
{
    TrainResult result;
    ++useClock;
    auto *line = findLine(pc);
    if (!line) {
        auto &set = table[setIndex(pc)];
        line = &set[0];
        for (auto &candidate : set) {
            if (!candidate.valid) {
                line = &candidate;
                break;
            }
            // Reclaim entries whose branch counters have both aged to zero
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
        *line = SetEntry{};
        line->valid = true;
        line->line = pc / LineBytes;
    }

    line->lastUse = useClock;
    for (auto &branch : line->branches) {
        if (branch.valid && branch.pc == pc) {
            branch.counter = std::min<uint8_t>(CounterMax, branch.counter + 1);
            branch.lastUse = useClock;
            result.incremented = true;
            return result;
        }
    }

    auto *slot = &line->branches[0];
    bool hasFreeSlot = false;
    for (auto &branch : line->branches) {
        if (!branch.valid || branch.counter == 0) {
            slot = &branch;
            hasFreeSlot = true;
            break;
        }
    }
    if (!hasFreeSlot) {
        result.dropped = true;
        return result;
    }
    *slot = BranchEntry{pc, 1, true, useClock};
    result.allocated = true;
    return result;
}

unsigned
H2PTable::age()
{
    unsigned aged = 0;
    for (auto &set : table) {
        for (auto &line : set) {
            if (!line.valid)
                continue;
            for (auto &branch : line.branches) {
                if (!branch.valid || branch.counter == 0)
                    continue;
                --branch.counter;
                ++aged;
                if (branch.counter == 0)
                    branch.valid = false;
            }
        }
    }
    return aged;
}

} // namespace gem5::branch_prediction::btb_pred
