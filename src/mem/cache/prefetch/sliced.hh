#ifndef __MEM_CACHE_PREFETCH_SLICED_HH__
#define __MEM_CACHE_PREFETCH_SLICED_HH__

#include <functional>

#include "base/types.hh"
#include "mem/packet.hh"

namespace gem5
{

namespace prefetch
{

/**
 * Interface used by a sliced cache to access independent prefetch queues.
 * The prefetcher remains responsible for queue storage and input arbitration.
 */
class SlicedPrefetcher
{
  public:
    using SliceMapper = std::function<unsigned(Addr)>;

    virtual ~SlicedPrefetcher() = default;

    virtual void configureSlices(unsigned num_slices, SliceMapper mapper) = 0;
    virtual unsigned numSlices() const = 0;

    virtual bool hasPendingPacket(unsigned slice) const = 0;
    virtual Tick nextPrefetchReadyTime(unsigned slice) const = 0;
    virtual PacketPtr getPacket(unsigned slice) = 0;

    /** Whether physically ready source candidates await input arbitration. */
    virtual bool hasPendingIngress() const = 0;

    /**
     * Commit at most one source candidate to each slice PFQ. The sliced cache
     * calls this after completing the cycle's dequeue phase.
     */
    virtual void processIngress() = 0;
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_SLICED_HH__
