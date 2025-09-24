#ifndef __MEM_CACHE_PREFETCH_FORWARDER_HH__
#define __MEM_CACHE_PREFETCH_FORWARDER_HH__

#include <queue>

#include "base/types.hh"
#include "mem/cache/prefetch/base.hh"
#include "params/PrefetcherForwarder.hh"
#include "sim/sim_object.hh"

namespace gem5
{

namespace prefetch
{

/**
 * @brief Forwarder for prefetcher
 *
 * This class is used to forward the prefetch requests to the real prefetcher.
 * It is used to implement the prefetcher for the L2 cache.
 */
class PrefetcherForwarder : public Base
{
  private:
    Base *real_pf;
    std::queue<PacketPtr> prefetch_queue;

  public:
    PrefetcherForwarder(const PrefetcherForwarderParams &p);

    void setRealPrefetcher(Base* real_pf_ptr) { real_pf = real_pf_ptr; }

    bool observeAccess(const PacketPtr &pkt, bool miss) const override;

    void notify(const PacketPtr &pkt, const PrefetchInfo &pfi) override;
    void probeNotify(const PacketPtr &pkt, bool miss) override;

    PacketPtr getPacket() override;
    bool hasPendingPacket() override;
    Tick nextPrefetchReadyTime() const override;

    void notifyFill(const PacketPtr &pkt) override;
    void coreDirectAddrNotify(const PacketPtr &pkt) override;
    bool hasHintDownStream() const override;
    void nofityHitToDownStream(const PacketPtr &pkt) override;

    void rxHint(BaseMMU::Translation *dpp) override;
    void notifyIns(int ins_num) override;
    void pfHitNotify(float accuracy, PrefetchSourceType pf_source, const PacketPtr &pkt) override;

    void sendCustomInfoToDownStream() override;
    void recvCustomInfoFrmUpStream(CustomPfInfo &info) override;
    void offloadToDownStream() override;
    bool hasHintsWaiting() override;

    // stats
    void incrDemandMhsrMisses() override;
    void prefetchUnused(PrefetchSourceType pf_type) override;
    void pfHitInMSHR(PrefetchSourceType pf_type) override;
    void pfHitInCache(PrefetchSourceType pf_type) override;
    void pfHitInWB(PrefetchSourceType pf_type) override;

    void recvPrefetchFromCache(const PacketPtr &pkt) override;
};

} // namespace prefetch
} // namespace gem5

#endif //__MEM_CACHE_PREFETCH_FORWARDER_HH__
