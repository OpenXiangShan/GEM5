#ifndef __MEM_CACHE_PREFETCH_XS_STRIDE_STATE_HH__
#define __MEM_CACHE_PREFETCH_XS_STRIDE_STATE_HH__

#include <cassert>
#include <cstdint>

namespace gem5
{
namespace prefetch
{

/** Prediction state for KMH's StrideMetaBundle (64-byte cache lines).
 *
 * Address truncation happens before subtraction, not after it. The sign
 * therefore changes when the truncated address wraps. Invalid short steps
 * still update history, but do not change the stride or confidence.
 */
struct XSStrideState
{
    uint64_t lastAddr = 0;
    int64_t stride = 0;
    uint8_t confidence = 0;

    struct Decision
    {
        int64_t observed;
        bool valid;
        bool match;
        bool send;
    };

    static uint64_t
    historyAddress(uint64_t address, unsigned bits)
    {
        assert(bits > 0 && bits <= 63);
        return address & ((uint64_t(1) << bits) - 1);
    }

    void
    allocate(uint64_t address, unsigned bits)
    {
        lastAddr = historyAddress(address, bits);
        stride = 0;
        confidence = 0;
    }

    Decision
    observe(uint64_t address, unsigned bits)
    {
        const uint64_t current = historyAddress(address, bits);
        const int64_t delta = current >= lastAddr ?
            static_cast<int64_t>(current - lastAddr) :
            -static_cast<int64_t>(lastAddr - current);
        const bool valid = delta >= 128 || delta <= -128;
        const bool match = delta == stride;
        // Both sending and replacement use the old confidence.
        const bool send = valid && match && confidence >= 3;
        if (valid) {
            if (match) {
                if (confidence < 7)
                    ++confidence;
            } else {
                if (confidence <= 1)
                    stride = delta;
                if (confidence > 0)
                    --confidence;
            }
        }
        lastAddr = current;
        return {delta, valid, match, send};
    }
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_XS_STRIDE_STATE_HH__
