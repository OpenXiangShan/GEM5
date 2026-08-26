#ifndef __MEM_CACHE_PREFETCH_COMPOITE_WITH_WORKER_L2_HH__
#define __MEM_CACHE_PREFETCH_COMPOITE_WITH_WORKER_L2_HH__

#include <cstdint>
#include <vector>

#include "mem/cache/prefetch/bop.hh"
#include "mem/cache/prefetch/cdp.hh"
#include "mem/cache/prefetch/cmc.hh"
#include "mem/cache/prefetch/composite_with_worker.hh"
#include "mem/cache/prefetch/despacito_stream.hh"
#include "params/L2CompositeWithWorkerPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

class L2CompositeWithWorkerPrefetcher : public CompositeWithWorkerPrefetcher,
                                        private DirectQualityGate::TraceSink
{
  public:
    L2CompositeWithWorkerPrefetcher(const L2CompositeWithWorkerPrefetcherParams &p);

    using CompositeWithWorkerPrefetcher::prefetchUnused;

    void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses) override {}

    void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late,
                           PrefetchSourceType source, bool miss_repeat) override;

    void prefetchUnused(Addr paddr, PrefetchSourceType pfSource) override;
    void prefetchUseful(Addr paddr, PrefetchSourceType pfSource) override;
    PacketPtr getPacket() override;

    void addToQueue(std::list<DeferredPacket> &queue, DeferredPacket &dpp) override;

    void addHintDownStream(Base *down_stream) override
    {
        hintDownStream = down_stream;
        cdp->addHintDownStream(down_stream);
    }
    void rxHint(BaseMMU::Translation *dpp) override;
    void pfHitNotify(float accuracy, PrefetchSourceType pf_source, const PacketPtr &pkt) override;

    void setParentInfo(System *sys, ProbeManager *pm, CacheAccessor* _cache, unsigned blk_size) override;

    void notify(const PacketPtr &pkt, const PrefetchInfo &pfi) override;

    void recvCustomInfoFrmUpStream(CustomPfInfo& info) override;

    void notifyFill(const PacketPtr &pkt) override;
    void notifyIns(int ins_num) override { cdp->notifyIns(ins_num); }

  private:
    CDP* cdp;
    BOP* largeBOP;
    BOP* smallBOP;
    CMCPrefetcher* cmc;
    DespacitoStreamPrefetcher* despacitoStream;

    const bool enableBOP;
    const bool enableCDP;
    const bool enableCMC;
    const bool enableDespacitoStream;

    bool offloadLowAccuracy = true;
    uint64_t nextBOPReplayEventId = 0;
    uint64_t activeBOPReplayEventId = 0;

    void directQualityTraceConfig(
        const DirectQualityGate::Config &config) override;
    void directQualityTraceCandidate(
        uint64_t event_sequence, Addr pc, uint8_t kind,
        Addr trigger_line, Addr candidate_line, DirectQualityGate::State state,
        bool allowed, bool sampled) override;
    void directQualityTraceIssue(
        uint64_t event_sequence, uint64_t feedback_id,
        uint64_t candidate_demand_sequence, Addr line, uint8_t kind) override;
    void directQualityTraceDemand(
        uint64_t event_sequence, uint64_t demand_sequence, Addr line) override;
    void directQualityTraceOutcome(
        uint64_t event_sequence, uint64_t feedback_id,
        uint64_t resolve_demand_sequence, Addr line,
        DirectQualityGate::TraceOutcome outcome) override;
    protected:
    void InsertPFRequestToBuffer(const AddrPriority &addr_prio) override{
      panic("SMS:InsertPFRequestToBuffer not implemented");
    };
  public:
    bool GetPFRequestsFromBuffer(std::vector<AddrPriority> &addresses) override;
    bool hasPFRequestsInBuffer() override;
};

}  // namespace prefetch
}  // namespace gem5


#endif  // __MEM_CACHE_PREFETCH_COMPOITE_WITH_WORKER_L2_HH__
