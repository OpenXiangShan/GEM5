/* Copyright (c) 2026. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __CPU_O3_RENAME_RAB_HH__
#define __CPU_O3_RENAME_RAB_HH__

#include <cassert>
#include <deque>

#include "cpu/inst_seq.hh"
#include "cpu/reg_class.hh"

namespace gem5
{
namespace o3
{

/** Single-thread RAB: ROB enqueue order and registered capacity grants. */
class RenameRAB
{
  public:
    RenameRAB(unsigned capacity, unsigned width)
        : numEntries(capacity), renameWidth(width) {}

    /** Classify destinations independently of physical register allocation. */
    static bool
    counts(const RegId &reg)
    {
        // Charge destination mappings even when a move reuses its source Preg.
        // Zero, Misc, RMisc, and CC mappings remain in history but do not count.
        switch (reg.classValue()) {
          case IntRegClass:
          case FloatRegClass:
          case VecRegClass:
          case VecElemClass:
          case VecPredRegClass:
            return true;
          default:
            return false;
        }
    }

    /** Sample after ROB enqueue, before this cycle's commit/squash releases. */
    void
    latchAdmission()
    {
        enqueueGrant = !numEntries || occupancy() <= numEntries - renameWidth;
        dispatchGrant = !numEntries ||
            occupancy() <= numEntries - 2 * renameWidth;
    }

    bool canEnqueue() const { return enqueueGrant; }
    bool canDispatch() const { return dispatchGrant; }

    bool
    fitsWritePorts(unsigned preceding, unsigned mappings, bool first) const
    {
        const unsigned total = preceding + mappings;
        return !numEntries || ((first || total <= renameWidth) &&
                               total <= numEntries - occupancy());
    }

    void
    enqueue(InstSeqNum seq, unsigned mappings)
    {
        assert(entries.empty() || entries.back() <= seq);
        assert(!numEntries || mappings <= numEntries - occupancy());
        entries.insert(entries.end(), mappings, seq);
    }

    void
    commitThrough(InstSeqNum seq)
    {
        while (!entries.empty() && entries.front() <= seq)
            entries.pop_front();
    }

    void
    squashAfter(InstSeqNum seq)
    {
        while (!entries.empty() && entries.back() > seq)
            entries.pop_back();
    }

    unsigned occupancy() const { return entries.size(); }
    unsigned capacity() const { return numEntries; }

  private:
    const unsigned numEntries;
    const unsigned renameWidth;
    std::deque<InstSeqNum> entries;
    bool enqueueGrant = true;
    bool dispatchGrant = true;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_RENAME_RAB_HH__
