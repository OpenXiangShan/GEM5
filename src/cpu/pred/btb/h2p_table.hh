#ifndef __CPU_PRED_BTB_H2P_TABLE_HH__
#define __CPU_PRED_BTB_H2P_TABLE_HH__

#include <array>
#include <cstdint>
#include <vector>

#include "base/types.hh"

namespace gem5::branch_prediction::btb_pred
{

class H2PTable
{
  public:
    static constexpr unsigned LineBytes = 64;
    static constexpr unsigned BranchesPerLine = 2;
    static constexpr unsigned CounterMax = 7;

    struct LookupResult
    {
        bool h2p = false;
        bool hit = false;
    };

    explicit H2PTable(unsigned entries = 128);

    LookupResult lookup(Addr pc);
    struct TrainResult
    {
        bool allocated = false;
        bool incremented = false;
        bool replaced = false;
        bool dropped = false;
    };

    TrainResult trainMispred(Addr pc);
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
        std::array<BranchEntry, BranchesPerLine> branches{};
        uint64_t lastUse = 0;
    };

    const unsigned numEntries;
    std::vector<SetEntry> table;
    uint64_t useClock = 0;

    SetEntry *findLine(Addr pc);
    const SetEntry *findLine(Addr pc) const;
};

} // namespace gem5::branch_prediction::btb_pred

#endif // __CPU_PRED_BTB_H2P_TABLE_HH__
