/*
 * Copyright (c) 2026
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

#ifndef __CPU_O3_SPEC_STORE_FWD_HH__
#define __CPU_O3_SPEC_STORE_FWD_HH__

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/types.hh"

namespace gem5
{

namespace o3
{

/**
 * Speculative store-to-load forwarding (Spec-STLF) predictor.
 *
 * This module only owns the PC-indexed meta table and its training/feedback
 * policy. The LSQ is responsible for scanning candidate stores, performing
 * the actual data copy, and triggering squashes on mispredictions.
 */
class SpecStoreFwdPredictor
{
  public:
    struct Meta
    {
        // Store-queue distance (load.sqIt.idx() - store.sqIdx).
        uint16_t distance = 0;
        uint8_t ctr = 0;
    };

    void
    init(bool enable, size_t table_size, unsigned ctr_bits)
    {
        _enabled = enable;
        if (!_enabled) {
            _ctrMax = 0;
            _indexBits = 0;
            _indexMask = 0;
            _table.clear();
            return;
        }

        panic_if(ctr_bits == 0 || ctr_bits > 8,
                 "SpecStoreFwdCtrBits must be in [1, 8] (got %u)\n", ctr_bits);
        panic_if(table_size == 0, "SpecStoreFwdTableSize must be > 0\n");
        panic_if(!isPowerOf2(table_size),
                 "SpecStoreFwdTableSize must be power of 2 (got %zu)\n",
                 table_size);
        panic_if(table_size < (1u << PcLowBits),
                 "SpecStoreFwdTableSize must be >= %u (got %zu)\n",
                 (1u << PcLowBits), table_size);

        _ctrMax = static_cast<uint8_t>((1u << ctr_bits) - 1);
        _indexBits = floorLog2(table_size);
        _indexMask = static_cast<size_t>(table_size - 1);
        _table.assign(table_size, Meta{});
    }

    bool enabled() const { return _enabled; }
    bool ready() const { return _enabled && !_table.empty(); }

    std::optional<uint16_t>
    predict(Addr pc) const
    {
        if (!ready()) {
            return std::nullopt;
        }
        const auto &meta = _table[index(pc)];
        if (meta.ctr != _ctrMax) {
            return std::nullopt;
        }
        return meta.distance;
    }

    void
    train(Addr pc, uint16_t distance)
    {
        if (!ready()) {
            return;
        }

        auto &meta = _table[index(pc)];
        if (meta.distance == distance) {
            if (meta.ctr < _ctrMax) {
                meta.ctr++;
            }
        } else {
            meta.distance = distance;
            // Reset counter on mismatch (per design doc).
            meta.ctr = 0;
        }
    }

    void
    reset(Addr pc)
    {
        if (!ready()) {
            return;
        }
        _table[index(pc)].ctr = 0;
    }

    /** Apply one saturating negative feedback update to the indexed entry. */
    void
    decrement(Addr pc)
    {
        if (!ready()) {
            return;
        }
        auto &meta = _table[index(pc)];
        if (meta.ctr > 0) {
            --meta.ctr;
        }
    }

    /** Update distance metadata, then decrement. */
    void
    updateDistanceAndDecrement(Addr pc, uint16_t distance)
    {
        if (!ready()) {
            return;
        }
        auto &meta = _table[index(pc)];
        meta.distance = distance;
        if (meta.ctr > 0) {
            --meta.ctr;
        }
    }

  private:
    static constexpr unsigned PcLowBits = 4;

    bool _enabled = false;
    uint8_t _ctrMax = 0;
    unsigned _indexBits = 0;
    size_t _indexMask = 0;
    std::vector<Meta> _table;

    size_t
    index(Addr pc) const
    {
        assert(!_table.empty());
        // Index format (e.g. 10-bit table): {XORFOLD(pc[max:5]), pc[4:1]}.
        //
        // - Low 4 bits keep the original PC[4:1] (after removing bit 0).
        // - High bits are XOR-folded from the remaining upper PC bits.
        const Addr pc_no_lsb = pc & ~static_cast<Addr>(0x1);
        const Addr low = (pc_no_lsb >> 1) & ((1u << PcLowBits) - 1);

        if (_indexBits <= PcLowBits) {
            return static_cast<size_t>(low) & _indexMask;
        }

        const unsigned high_bits = _indexBits - PcLowBits;
        const Addr upper = pc_no_lsb >> (PcLowBits + 1);
        const Addr high = xorFold(upper, high_bits);

        const Addr idx = (high << PcLowBits) | low;
        return static_cast<size_t>(idx) & _indexMask;
    }

    static Addr
    xorFold(Addr value, unsigned width)
    {
        const unsigned chunks = (sizeof(value) * 8 + width - 1) / width;
        Addr folded = 0;
        for (unsigned i = 0; i < chunks; ++i) {
            folded ^= value & ((static_cast<Addr>(1) << width) - 1);
            value >>= width;
        }
        return folded;
    }
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_SPEC_STORE_FWD_HH__
