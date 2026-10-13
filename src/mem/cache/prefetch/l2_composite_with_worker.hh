#ifndef __MEM_CACHE_PREFETCH_COMPOITE_WITH_WORKER_L2_HH__
#define __MEM_CACHE_PREFETCH_COMPOITE_WITH_WORKER_L2_HH__

#include <array>
#include <functional>
#include <limits>
#include <list>
#include <memory>
#include <vector>

#include "mem/cache/prefetch/bop.hh"
#include "mem/cache/prefetch/cdp.hh"
#include "mem/cache/prefetch/cmc.hh"
#include "mem/cache/prefetch/composite_with_worker.hh"
#include "mem/cache/prefetch/despacito_stream.hh"
#include "mem/cache/prefetch/lldp.hh"
#include "mem/cache/prefetch/sliced.hh"
#include "params/L2CompositeWithWorkerPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

class L2CompositeWithWorkerPrefetcher :
    public CompositeWithWorkerPrefetcher,
    public SlicedPrefetcher
{
  public:
    L2CompositeWithWorkerPrefetcher(const L2CompositeWithWorkerPrefetcherParams &p);
    ~L2CompositeWithWorkerPrefetcher() override;

    using CompositeWithWorkerPrefetcher::prefetchUnused;

    void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses) override {}

    void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late,
                           PrefetchSourceType source, bool miss_repeat) override;

    void prefetchUnused(Addr paddr, PrefetchSourceType pfSource) override;

    void addToQueue(std::list<DeferredPacket> &queue, DeferredPacket &dpp) override;

    void addHintDownStream(Base *down_stream) override
    {
        hintDownStream = down_stream;
        cdp->addHintDownStream(down_stream);
    }
    void rxHint(BaseMMU::Translation *dpp) override;
    void pfHitNotify(float accuracy, PrefetchSourceType pf_source, const PacketPtr &pkt) override;

    void setParentInfo(System *sys, ProbeManager *pm, CacheAccessor* _cache, unsigned blk_size) override;
    void addTLB(BaseTLB *tlb, bool functional) override;
    void regProbeListeners() override;
    void setPacketReadyCallback(std::function<void(Tick)> callback) override;
    bool hasPendingPacket() override;
    PacketPtr getPacket() override;
    Tick nextPrefetchReadyTime() const override;
    lldp::Hint loadTrain(const PacketPtr &pkt, bool miss) override;
    void hintData(const lldp::Hint &hint, const PacketPtr &demand,
                  const uint8_t *data, unsigned size) override;

    void notify(const PacketPtr &pkt, const PrefetchInfo &pfi) override;

    void recvCustomInfoFrmUpStream(CustomPfInfo& info) override;

    void notifyFill(const PacketPtr &pkt) override;
    void notifyIns(int ins_num) override { cdp->notifyIns(ins_num); }
    void resetStats() override;
    void preDumpStats() override;

    void configureSlices(unsigned num_slices, SliceMapper mapper) override;
    unsigned numSlices() const override { return sliceCount; }
    bool hasPendingPacket(unsigned slice) const override;
    Tick nextPrefetchReadyTime(unsigned slice) const override;
    PacketPtr getPacket(unsigned slice) override;
    bool hasPendingIngress() const override;
    void processIngress() override;

  private:
    enum class InputSource : uint8_t
    {
        L1,
        LargeBOP,
        SmallBOP,
        DespacitoStream,
        LLDP,
        CDP,
        CMC,
        NumSources
    };

    static constexpr unsigned NumInputSources =
        static_cast<unsigned>(InputSource::NumSources);

    static constexpr unsigned
    sourceIndex(InputSource source)
    {
        return static_cast<unsigned>(source);
    }

    CDP* cdp;
    BOP* largeBOP;
    BOP* smallBOP;
    CMCPrefetcher* cmc;
    DespacitoStreamPrefetcher* despacitoStream;

    const bool enableBOP;
    const bool enableCDP;
    const bool enableCMC;
    const bool enableDespacitoStream;
    LLDPrefetcher *lldp;
    const bool enableLLDP;
    bool preferLLDP{false};

    const bool offloadLowAccuracy;

    const unsigned entriesPerSlice;
    unsigned sliceCount{0};
    SliceMapper sliceMapper;
    std::array<std::list<DeferredPacket>, NumInputSources> sourceReady;
    std::vector<unsigned> maxSliceOccupancy;
    uint64_t lastIngressCycle{std::numeric_limits<uint64_t>::max()};
    std::vector<uint64_t> lastDequeueCycleBySlice;

    struct SlicePfqStats : public statistics::Group
    {
        SlicePfqStats(statistics::Group *parent, unsigned num_slices);

        statistics::Vector pfqEnqueued;
        statistics::Vector pfqDequeued;
        statistics::Vector pfqDroppedOldest;
        statistics::Vector pfqInputConflict;
        statistics::Vector pfqOccupancy;
        statistics::Vector pfqMaxOccupancy;
        statistics::Vector pfqInputWonBySource;
        statistics::Distribution pfqEnqueueWidth;
    };
    std::unique_ptr<SlicePfqStats> slicePfqStats;

    bool sliced() const { return sliceCount != 0; }
    unsigned packetSlice(const DeferredPacket &dpp) const;
    iterator sliceHead(unsigned slice);
    const_iterator sliceHead(unsigned slice) const;
    unsigned sliceOccupancy(unsigned slice) const;
    bool queuedInPfq(const PrefetchInfo &pfi) const;
    void updateSliceOccupancyStats();
    void dropStagedFront(InputSource source);
    void enqueueWinner(InputSource source, unsigned slice);
    void tagNewRequests(std::vector<AddrPriority> &addresses,
                        size_t previous_size, InputSource source) const;
    bool pullSourceRequest(Queued *source, InputSource input_source,
                           std::vector<AddrPriority> &addresses);

  protected:
    bool canTransferHint() const override
    {
        return !sliced() ||
            sourceReady[sourceIndex(InputSource::L1)].empty();
    }

    void InsertPFRequestToBuffer(const AddrPriority &addr_prio) override
    {
        panic("L2CompositeWithWorkerPrefetcher: local buffer not implemented");
    }

    bool reorderOnPriorityUpdate(
        const std::list<DeferredPacket> &queue) const override
    {
        return !sliced() || &queue != &pfq;
    }

  public:
    bool GetPFRequestsFromBuffer(std::vector<AddrPriority> &addresses) override;
    bool hasPFRequestsInBuffer() override;
};

}  // namespace prefetch
}  // namespace gem5


#endif  // __MEM_CACHE_PREFETCH_COMPOITE_WITH_WORKER_L2_HH__
