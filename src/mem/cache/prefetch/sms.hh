//
// Created by linjiawei on 22-8-27.
//

#ifndef GEM5_SMS_HH
#define GEM5_SMS_HH

#include <vector>
#include <cstdint>
#include <memory>

#include <boost/compute/detail/lru_cache.hpp>

#include "base/sat_counter.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "mem/cache/prefetch/associative_set.hh"
#include "mem/cache/prefetch/berti.hh"
#include "mem/cache/prefetch/bop.hh"
#include "mem/cache/prefetch/cmc.hh"
#include "mem/cache/prefetch/ipcp.hh"
#include "mem/cache/prefetch/opt.hh"
#include "mem/cache/prefetch/queued.hh"
#include "mem/cache/prefetch/signature_path.hh"
#include "mem/cache/prefetch/stride.hh"
#include "mem/cache/prefetch/xs_stream.hh"
#include "mem/cache/prefetch/xs_stride.hh"
#include "mem/cache/tags/tagged_entry.hh"
#include "mem/packet.hh"
#include "params/XSCompositePrefetcher.hh"

namespace gem5
{
struct XSCompositePrefetcherParams;
class BaseIndexingPolicy;
namespace replacement_policy { class Base; }

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

// PrefetchFilter is implemented in its own header/source to keep sms.
#include "mem/cache/prefetch/prefetch_filter.hh"

class StepSpatialPrefetcher;

class XSCompositePrefetcher : public Queued
{
  protected:
    const unsigned int regionSize;
    const unsigned int regionBlks;

    const bool enableTrainFilter;  // Enable TrainFilter for ROB-order training

    // STEP observes raw demand accesses through Base's dedicated hook.  The
    // existing composite components must retain their original ROB-ordered
    // training path for a controlled STEP-versus-SMS comparison.
    bool useTrainingBuffer() const override
    {
        return enableTrainFilter;
    }

    Addr regionAddress(Addr a) { return a / regionSize; };

    Addr regionOffset(Addr a) { return (a / blkSize) % regionBlks; }


    // active generation table
    class ACTEntry : public TaggedEntry
    {
      public:
        Addr pc;
        Addr regionAddr;
        ContextID contextId;
        uint64_t regionBits;
        bool inBackwardMode;
        uint8_t accessCount;
        uint64_t regionOffset;
        uint32_t depth;
        SatCounter8 lateConf;
        bool hasIncreasedPht;
        ACTEntry(const SatCounter8 &conf)
            : TaggedEntry(),
              pc(~(0UL)),
              regionAddr(0),
              contextId(InvalidContextID),
              regionBits(0),
              inBackwardMode(false),
              accessCount(0),
              regionOffset(0),
              depth(0),
              lateConf(4, 7),
              hasIncreasedPht(false)
        {
        }
        bool inActivePage(unsigned region_blocks) {
            return accessCount > region_blocks / 4 * 3;
        }
        void _setSecure(bool is_secure) {
            if (is_secure) TaggedEntry::setSecure();
        }
    };

    AssociativeSet<ACTEntry> act;

    class ReACTEntry : public TaggedEntry
    {
      public:
        Addr pc;
        Addr regionAddr;
        ContextID contextId;
        ReACTEntry()
            : TaggedEntry(), pc(0), regionAddr(0),
              contextId(InvalidContextID)
        {}
        void _setSecure(bool is_secure) {
            if (is_secure) TaggedEntry::setSecure();
        }
    };
    AssociativeSet<ReACTEntry> re_act;

    const bool streamPFAhead;

    ACTEntry *actLookup(const PrefetchInfo &pfi, bool &in_active_page, bool &alloc_new_region, bool &is_first_shot);

    const unsigned streamDepthStep{4};  // # block changed in one step

    void updatePht(ACTEntry *act_entry, Addr region_addr,bool re_act_mode,bool signal_update,Addr region_offset_now);

    // pattern history table
    class PhtEntry : public TaggedEntry
    {
      public:
        std::vector<SatCounter8> hist;
        Addr pc;
        ContextID contextId;
        bool decr_mode;
        PhtEntry(const size_t sz, const SatCounter8 &conf)
            : TaggedEntry(), hist(sz, conf),
              contextId(InvalidContextID), decr_mode(false)
        {
        }
    };

    AssociativeSet<PhtEntry> pht;

    const bool phtPFAhead;

    const int phtPFLevel;

    Addr pcHash(Addr pc) { return pc >> 1; }

    Addr phtHash(Addr pc, Addr region_offset) { return pc >> 1; }

    bool phtLookup(const PrefetchInfo &pfi,
                   std::vector<AddrPriority> &addresses, bool late, Addr look_ahead_addr);

    struct XSCompositeStats : public statistics::Group
    {
        XSCompositeStats(statistics::Group *parent);
        statistics::Scalar allCntNum;
        statistics::Scalar actMNum;
        statistics::Scalar refillNotifyCount;
        statistics::Scalar bopTrainCount;
        statistics::Scalar smsCurRegionoverride;
        statistics::Scalar smsIncrRegionoverride;
        statistics::Scalar smsDecrRegionoverride;
        statistics::Scalar strideTrainCount;
        statistics::Scalar streamTrainCount;
        statistics::Scalar totalTrainCount;
    } stats;

  public:
    XSCompositePrefetcher(const XSCompositePrefetcherParams &p);
    ~XSCompositePrefetcher() override;

    // dummy implementation, calc(3 args) will not call it
    void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses) override
    {
        panic("not implemented");
    };

    void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late,
                           PrefetchSourceType pf_source, bool miss_repeat) override;

    /** Update the RR right table after a prefetch fill */
    void notifyFill(const PacketPtr& pkt) override;

    /** Validate the final cache and hint topology before probes are wired. */
    void regProbeListeners() override;

  private:
    const unsigned pfFilterSize{256};
    const unsigned pfPageFilterSize{16};
    boost::compute::detail::lru_cache<Addr, Addr> pfBlockLRUFilter;
    // STEP uses its own line-aligned fill filter. The legacy shared filter
    // intentionally retains its existing key and fill behavior.
    boost::compute::detail::lru_cache<Addr, Addr> stepBlockLRUFilter;

    PrefetchFilter sms_pfFilter;
    PrefetchFilter stepPb;
    PrefetchFilter stridestream_pfFilter_l1;
    PrefetchFilter stridestream_pfFilter_l2l3;
    std::unique_ptr<StepSpatialPrefetcher> step;

    boost::compute::detail::lru_cache<Addr, Addr> pfPageLRUFilter;
    boost::compute::detail::lru_cache<Addr, Addr> pfPageLRUFilterL2;
    boost::compute::detail::lru_cache<Addr, Addr> pfPageLRUFilterL3;

    bool sendPFWithFilter(const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses, int prio,
                          PrefetchSourceType src, int ahead_level = -1);
    bool sendStepPFWithFilter(const PrefetchInfo &pfi, Addr addr,
                              std::vector<AddrPriority> &addresses, int prio,
                              int ahead_level);
    bool stepPrefetchFiltered(const PrefetchInfo &pfi, Addr addr,
                              int ahead_level);
    Addr stepBlockFilterKey(Addr addr, ContextID context_id) const;
    uint64_t bufferStepPrefetches(const PrefetchInfo &pfi, Addr region,
                                  uint64_t candidates,
                                  uint64_t decision_id);
    bool getStepPrefetchFromBuffer(std::vector<AddrPriority> &addresses,
                                   int target_level);
    void completeStagedPrefetch(const StagedPrefetchToken &token,
                                bool accepted) override;
    void releaseStagedPrefetch(const StagedPrefetchToken &token) override;
    void observeRawDemandAccess(const PacketPtr &pkt, bool miss) override;
    void sendStreamPF(const PrefetchInfo &pfi, Addr pf_tgt_addr, std::vector<AddrPriority> &addresses,
                      boost::compute::detail::lru_cache<Addr, Addr> &Filter, bool decr, int pf_level);
    void updatePhtBits(bool accessed, bool early_update, bool re_act_mode, uint8_t hist_idx,
                       XSCompositePrefetcher::ACTEntry *act_entry, XSCompositePrefetcher::PhtEntry *pht_entry);

    BOP *largeBOP;

    BOP *smallBOP;

    BOP *learnedBOP;

    SignaturePath  *spp;

    IPCP *ipcp;

    CMCPrefetcher* cmc;
    BertiPrefetcher *berti;
    XSStridePrefetcher *Sstride;
    OptPrefetcher *Opt;
    XsStreamPrefetcher *Xsstream;


    const bool enableActivepage;
    const bool enablePht;
    const bool enableStep;
    const bool enableCPLX;
    const bool enableSPP;
    const bool enableTemporal;
    const bool enableSstride;
    const bool enableBerti;
    const bool enableBOP;
    const bool enableOpt;
    const bool enableXsstream;
    const bool phtEarlyUpdate;
    const bool neighborPhtUpdate;
    const unsigned stepRegionSize;
    const int stepPFLevel;

  public:
    void notifyIns(int ins_num) override
    {
        if (hasHintDownStream()){
          hintDownStream->notifyIns(ins_num);
        }
    }
    void setParentInfo(System *sys, ProbeManager *pm, CacheAccessor* _cache, unsigned blk_size) override;

  protected:
    using TriggerInfo = Base::PFtriggerInfo;
    struct phtsentInfo {
        bool valid;
        Addr region_addr;
        uint64_t region_bits;
        uint8_t alias_bits;
        bool paddr_valid;
        bool decr_mode;
        bool is_secure;
        uint64_t PFlevel;
        TriggerInfo trigger;
        // phtsentInfo()
        //     : valid(false), region_addr(0), region_bits(0), alias_bits(0), paddr_valid(false),
        //       decr_mode(false), is_secure(false), PFlevel(0), trigger() {};
        phtsentInfo(Addr region_addr = 0, uint64_t region_bits = 0, uint8_t alias_bits = 0,
              bool paddr_valid = false, bool decr_mode = false,
              bool is_secure = false, uint64_t PFlevel = 0,
              const TriggerInfo *trigger = nullptr)
            : valid(true), region_addr(region_addr), region_bits(region_bits), alias_bits(alias_bits),
              paddr_valid(paddr_valid), decr_mode(decr_mode), is_secure(is_secure),
              PFlevel(PFlevel), trigger(trigger == nullptr ? TriggerInfo() : *trigger) {};
        ~phtsentInfo() = default;
    };
    std::vector<phtsentInfo> phtSentPrefetch;//0 cur ,1 inc ,2 dec
    /** Event to handle the pht sending */
    void phtSendEventWrapper();
    EventFunctionWrapper phtReqSendEvent;
  protected:
    void InsertPFRequestToBuffer(const AddrPriority &addr_prio) override{
      panic("SMS:InsertPFRequestToBuffer not implemented");
    };
  public:
    bool GetPFRequestsFromBuffer(std::vector<AddrPriority> &addresses) override;
    bool hasPFRequestsInBuffer() override;
  protected:
    const int BOPPFlevel;
};

}
}  // gem5

#endif  // GEM5_SMS_HH
