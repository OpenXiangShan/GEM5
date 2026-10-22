#ifndef __MEM_CACHE_PREFETCH_TEMPORAL_FEEDBACK_HH__
#define __MEM_CACHE_PREFETCH_TEMPORAL_FEEDBACK_HH__

#include <cstdint>
#include <stdexcept>

namespace gem5::prefetch::temporal
{

enum class CandidatePhase : uint32_t
{
    Queued, Sent, SettledQueued, SettledSent
};
enum class FeedbackKind { Timely, Late, PermissionMiss, Unused };

// Up to 32768 Meta slots, a 15-bit generation, and two lifecycle bits.
// The adapter invalidates owners before generation wrap. The slot field
// covers all 24576 Meta entries in the compact/full pool implementation.
struct RecordRef
{
    uint32_t bits{0xffffffff};

    RecordRef() = default;
    RecordRef(unsigned slot, uint16_t generation)
        : bits(slot | (uint32_t(generation) << 15))
    {
        if (slot > 0x7fff || generation > 0x7fff)
            throw std::out_of_range("TP record reference exceeds its fields");
    }
    unsigned slot() const { return bits & 0x7fff; }
    uint16_t generation() const { return (bits >> 15) & 0x7fff; }
    CandidatePhase phase() const { return CandidatePhase(bits >> 30); }
    bool settled() const { return (bits >> 31) != 0; }
    bool sent() const { return (bits & (1U << 30)) != 0; }
    void settle() { bits |= 1U << 31; }
    void markSent() { bits |= 1U << 30; }
    void phase(CandidatePhase value)
    { bits = (bits & 0x3fffffff) | (uint32_t(value) << 30); }
};

// Side state belongs only to Meta, not to Recorder or the copied line list.
struct RecordState
{
    uint16_t bits{0};
    uint16_t generation() const { return bits & 0x7fff; }
    // The caller invalidates outstanding references before generation reuse.
    void renew() { bits = (generation() + 1) & 0x7fff; }
};

static_assert(sizeof(RecordRef) == 4);
static_assert(sizeof(RecordState) == 2);

} // namespace gem5::prefetch::temporal
#endif
