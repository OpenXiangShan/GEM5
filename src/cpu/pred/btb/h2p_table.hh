#ifndef __CPU_PRED_BTB_H2P_TABLE_HH__
#define __CPU_PRED_BTB_H2P_TABLE_HH__

#include <cstdint>
#include <vector>

#include "base/types.hh"

namespace gem5::branch_prediction::btb_pred
{

class H2PTable
{
  public:
    // Defaults preserve the original 32-byte, 2-way experiment.
    static constexpr unsigned LineBytes = 32;
    static constexpr unsigned Ways = 2;
    static constexpr unsigned BranchesPerLine = 2;
    static constexpr unsigned CounterMax = 7;

    struct LookupResult
    {
        bool h2p = false;
        bool hit = false;
    };

    explicit H2PTable(unsigned entries = 128, unsigned lineBytes = LineBytes,
                      unsigned ways = Ways,
                      unsigned branchesPerEntry = BranchesPerLine,
                      unsigned counterBits = 3, unsigned counterThreshold = 3);

    LookupResult lookup(Addr pc);
    LookupResult peek(Addr pc) const;
    struct TrainResult
    {
        bool allocated = false;
        bool incremented = false;
        bool replaced = false;
        bool dropped = false;
        bool allocationFiltered = false;
    };

    TrainResult trainMispred(Addr pc, bool allowAllocate = true);
    unsigned age();

    unsigned entries() const { return numEntries; }

  private:
    struct BranchEntry
    {
        Addr pc = 0;
        uint8_t counter = 0;
        bool valid = false;
        uint64_t lastUse = 0;
    };

    struct SetEntry
    {
        Addr line = 0;
        bool valid = false;
        std::vector<BranchEntry> branches;
        uint64_t lastUse = 0;
    };

    const unsigned numEntries;
    const unsigned lineBytes;
    const unsigned numWays;
    const unsigned branchesPerEntry;
    const unsigned counterMax;
    const unsigned counterThreshold;
    const unsigned numSets;
    std::vector<std::vector<SetEntry>> table;
    uint64_t useClock = 0;

    unsigned setIndex(Addr pc) const;
    SetEntry *findLine(Addr pc);
    const SetEntry *findLine(Addr pc) const;
};

} // namespace gem5::branch_prediction::btb_pred

#endif // __CPU_PRED_BTB_H2P_TABLE_HH__
