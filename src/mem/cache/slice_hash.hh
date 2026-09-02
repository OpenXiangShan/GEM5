#ifndef __MEM_CACHE_SLICE_HASH_HH__
#define __MEM_CACHE_SLICE_HASH_HH__

#include <string>

#include "base/types.hh"

namespace gem5
{

enum class SliceHashPolicy
{
    None,
    Xor,
    XorFold,
    Murmur3,
    Invalid,
};

SliceHashPolicy parseSliceHashPolicy(const std::string &policy);

/**
 * Map a cache-line address to a physical slice.
 *
 * All non-trivial policies XOR a hash of the upper line-address bits into
 * the original low slice bits. This keeps the mapping bijective over the low
 * bits for every fixed set/tag value.
 */
Addr hashSlice(Addr line_addr, unsigned slice_bits,
               SliceHashPolicy policy);

/** Recover the original low line-address bits from a physical slice ID. */
Addr recoverSliceLowBits(Addr upper_line_addr, Addr slice_id,
                         unsigned slice_bits, SliceHashPolicy policy);

} // namespace gem5

#endif // __MEM_CACHE_SLICE_HASH_HH__
