/*
 * Copyright (c) 2024 The gem5 Authors
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __ARCH_RISCV_VTYPE_PRED_HH__
#define __ARCH_RISCV_VTYPE_PRED_HH__

#include <cstdint>
#include <vector>

#include "base/types.hh"

namespace gem5
{

namespace RiscvISA
{

/**
 * Direct-mapped BTB-like predictor for the vtype field of register-form
 * vsetvl instructions.  Each entry maps a PC to the last-seen vtype value
 * written by that instruction.  Because compiler-generated vsetvl loops
 * use a fixed vtype per loop, the prediction accuracy is expected to be
 * very high after the first (cold) iteration.
 *
 * Table is intentionally small (64 entries) to reflect the small number of
 * distinct vsetvl PCs that appear in typical vectorised workloads.
 */
class VtypePredictor
{
  private:
    struct Entry
    {
        Addr    pc    = 0;
        uint8_t vtype = 0;
        bool    valid = false;
    };

    std::vector<Entry> table;
    unsigned mask;  // (entries - 1), for power-of-two wrap

    unsigned idx(Addr pc) const { return (pc >> 2) & mask; }

  public:
    /**
     * Construct the predictor with @p num_entries entries.
     * @p num_entries must be a power of two; if not, it is rounded up to the
     * next power of two.
     */
    explicit VtypePredictor(unsigned num_entries = 64)
    {
        // round up to next power of two
        unsigned n = 1;
        while (n < num_entries) n <<= 1;
        table.resize(n);
        mask = n - 1;
    }

    /**
     * Look up a prediction for the given PC.
     * Returns true and sets @p out if a valid entry matches, false otherwise.
     */
    bool predict(Addr pc, uint8_t &out) const
    {
        const Entry &e = table[idx(pc)];
        if (e.valid && e.pc == pc) {
            out = e.vtype;
            return true;
        }
        return false;
    }

    /** Record the actual vtype computed by the vsetvl at @p pc. */
    void update(Addr pc, uint8_t vtype)
    {
        Entry &e  = table[idx(pc)];
        e.pc      = pc;
        e.vtype   = vtype;
        e.valid   = true;
    }
};

} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_VTYPE_PRED_HH__
