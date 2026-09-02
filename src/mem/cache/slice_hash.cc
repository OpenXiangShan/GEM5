#include "mem/cache/slice_hash.hh"

#include <cassert>
#include <limits>

namespace gem5
{

namespace
{

Addr
lowMask(unsigned bits)
{
    assert(bits < std::numeric_limits<Addr>::digits);
    return bits == 0 ? 0 : (Addr(1) << bits) - 1;
}

Addr
foldXor(Addr value, unsigned width)
{
    if (width == 0) {
        return 0;
    }

    // Fold all width-bit chunks in logarithmic, fixed-width steps.
    for (unsigned shift = width;
         shift < std::numeric_limits<Addr>::digits; shift *= 2) {
        value ^= value >> shift;
    }
    return value & lowMask(width);
}

Addr
murmur3Finalizer(Addr value)
{
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33;
    return value;
}

Addr
hashUpperLine(Addr upper_line_addr, unsigned slice_bits,
              SliceHashPolicy policy)
{
    const Addr mask = lowMask(slice_bits);

    switch (policy) {
      case SliceHashPolicy::None:
        return 0;
      case SliceHashPolicy::Xor:
        return upper_line_addr & mask;
      case SliceHashPolicy::XorFold:
        return foldXor(upper_line_addr, slice_bits);
      case SliceHashPolicy::Murmur3:
        return murmur3Finalizer(upper_line_addr) & mask;
      case SliceHashPolicy::Invalid:
        break;
    }

    assert(false && "invalid slice hash policy");
    return 0;
}

} // anonymous namespace

SliceHashPolicy
parseSliceHashPolicy(const std::string &policy)
{
    if (policy == "none") {
        return SliceHashPolicy::None;
    }
    if (policy == "xor") {
        return SliceHashPolicy::Xor;
    }
    if (policy == "xor-fold") {
        return SliceHashPolicy::XorFold;
    }
    if (policy == "murmur3") {
        return SliceHashPolicy::Murmur3;
    }
    return SliceHashPolicy::Invalid;
}

Addr
hashSlice(Addr line_addr, unsigned slice_bits, SliceHashPolicy policy)
{
    if (slice_bits == 0) {
        return 0;
    }

    const Addr mask = lowMask(slice_bits);
    const Addr low_bits = line_addr & mask;
    const Addr upper_line_addr = line_addr >> slice_bits;
    return low_bits ^ hashUpperLine(upper_line_addr, slice_bits, policy);
}

Addr
recoverSliceLowBits(Addr upper_line_addr, Addr slice_id,
                    unsigned slice_bits, SliceHashPolicy policy)
{
    if (slice_bits == 0) {
        return 0;
    }

    const Addr mask = lowMask(slice_bits);
    return (slice_id & mask) ^
           hashUpperLine(upper_line_addr, slice_bits, policy);
}

} // namespace gem5
