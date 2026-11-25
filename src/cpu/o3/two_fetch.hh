// Copyright (c) 2026 Beijing Institute of Open Source Chip (BOSC)
// SPDX-License-Identifier: BSD-3-Clause

#ifndef __CPU_O3_TWO_FETCH_HH__
#define __CPU_O3_TWO_FETCH_HH__

#include <cstdint>
#include <optional>

namespace gem5::o3::two_fetch
{

enum class Reject { None, NoNext, Range, Link, Size, Page, CrossPage, Bank, Exception, Count };

// end is exclusive; the prediction window and the supplied bytes are distinct.
inline std::optional<uint64_t>
effectiveEnd(uint64_t start, uint64_t window_end, bool taken, uint64_t control, unsigned inst_size)
{
    if ((start & 1) || window_end <= start || (window_end & 1))
        return std::nullopt;
    if (!taken)
        return window_end;
    if ((inst_size != 2 && inst_size != 4) || (control & 1) || control < start || control >= window_end ||
        inst_size > window_end - control)
        return std::nullopt;
    return control + inst_size;
}

inline Reject
canPair(uint64_t start0, uint64_t end0, uint64_t next0, uint64_t start1, uint64_t end1, unsigned budget)
{
    if (end0 <= start0 || end1 <= start1)
        return Reject::Range;
    if (next0 != start1)
        return Reject::Link;
    if (end0 - start0 > budget || end1 - start1 > budget - (end0 - start0))
        return Reject::Size;
    if ((start0 >> 12) != (start1 >> 12))
        return Reject::Page;
    if ((start0 >> 12) != ((end0 - 1) >> 12) || (start1 >> 12) != ((end1 - 1) >> 12))
        return Reject::CrossPage;
    return Reject::None;
}

inline bool
controlAlreadyPassed(bool taken, uint64_t control, uint64_t pc)
{
    return taken && control < pc;
}

inline bool
skippedControl(bool taken, uint64_t control, uint64_t pc, uint64_t next)
{
    return taken && pc < control && next > control;
}

}  // namespace gem5::o3::two_fetch
#endif
