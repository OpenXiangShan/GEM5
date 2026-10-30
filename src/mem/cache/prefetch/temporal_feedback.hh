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

// Effect feedback has a separate lifetime from legacy first-use feedback.
// A store can settle RecordRef while a subsequent load still uses the line.
enum class OutcomeEvent { Sent, HitReady, HitDelayed, Late, Evicted };
struct OutcomeState
{
    enum Credit : unsigned
    { Send = 1, Useful = 2, Timely = 4, Late = 8, Unused = 16 };
    uint16_t bits{0};

    unsigned advance(OutcomeEvent event)
    {
        constexpr unsigned sent = 1, hit = 2, ready = 4, late = 8, closed = 16;
        const unsigned before = bits & 31;
        if (event == OutcomeEvent::Sent)
            bits |= sent;
        else if (!(before & closed)) {
            if (event == OutcomeEvent::Evicted)
                bits |= closed;
            else if (event == OutcomeEvent::Late) {
                if (!(before & hit))
                    bits |= late;
            } else if (!(before & hit)) {
                bits |= hit;
                if (event == OutcomeEvent::HitReady)
                    bits |= ready;
                else
                    bits |= late;
            }
        }
        unsigned credit = 0;
        if (bits & sent) {
            credit = Send;
            if (bits & hit)
                credit |= Useful;
            if ((bits & ready) && !(bits & late))
                credit |= Timely;
            if (bits & late)
                credit |= Late;
            if ((bits & closed) && !(bits & hit))
                credit |= Unused;
        }
        const unsigned fresh = credit & ~(bits >> 8);
        bits |= credit << 8;
        return fresh;
    }
    bool sent() const { return bits & 1; }
    bool resolved() const { return bits & (2 | 16); }
};

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
