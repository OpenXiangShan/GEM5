#ifndef __MEM_CACHE_SLICE_HASH_HH__
#define __MEM_CACHE_SLICE_HASH_HH__

#include <string>

#include "base/types.hh"

namespace gem5
{

enum class SliceHashPolicy
{
    None,
    XorFold,
    Murmur3,
    Invalid,
};

SliceHashPolicy parseSliceHashPolicy(const std::string &policy);

/**
 * Map a cache-line address to a physical slice.
 *
 * The None policy preserves the conventional low-bit slice selection. All
 * hash policies compute the slice directly from the complete line address;
 * their inner caches must therefore retain the complete line address in the
 * set and tag fields.
 */
Addr hashSlice(Addr line_addr, unsigned slice_bits,
               SliceHashPolicy policy);

/** Number of low line-address bits omitted from a per-slice set index. */
unsigned sliceSetShift(unsigned slice_bits, SliceHashPolicy policy);

} // namespace gem5

#endif // __MEM_CACHE_SLICE_HASH_HH__
