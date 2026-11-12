/* Copyright (c) 2026. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __CPU_O3_RENAME_RAB_HH__
#define __CPU_O3_RENAME_RAB_HH__

#include <cassert>

#include "cpu/reg_class.hh"

namespace gem5
{
namespace o3
{

/** Capacity accounting for the charged mappings in Rename's history buffer. */
class RenameRAB
{
  public:
    enum class Admission { Ready, Full, Oversized };

    explicit RenameRAB(unsigned capacity) : numEntries(capacity) {}

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

    /** Check an entire bundle without reserving or consuming any entries. */
    Admission
    check(unsigned demand) const
    {
        // Zero disables admission checks without disabling occupancy tracking.
        if (numEntries == 0)
            return Admission::Ready;
        if (demand > numEntries)
            return Admission::Oversized;
        assert(numOccupied <= numEntries);
        return demand <= numEntries - numOccupied ?
            Admission::Ready : Admission::Full;
    }

    void
    allocate()
    {
        assert(check(1) == Admission::Ready);
        ++numOccupied;
    }

    void
    release()
    {
        assert(numOccupied > 0);
        --numOccupied;
    }

    unsigned occupancy() const { return numOccupied; }
    unsigned capacity() const { return numEntries; }

  private:
    const unsigned numEntries;
    unsigned numOccupied = 0;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_RENAME_RAB_HH__
