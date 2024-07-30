//
// Created by linjiawei on 22-8-27.
//

#ifndef GEM5_SMS_HH
#define GEM5_SMS_HH

#include <vector>

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

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

class XSCompositePrefetcher : public Queued
{
  protected:
    const unsigned int regionSize;
    const unsigned int regionBlks;


    Addr regionAddress(Addr a) { return a / regionSize; };

    Addr regionOffset(Addr a) { return (a / blkSize) % regionBlks; }


    // active generation table
    class ACTEntry : public TaggedEntry
    {
      public:
        Addr pc;
        Addr regionAddr;
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
        ReACTEntry() : TaggedEntry(), pc(0), regionAddr(0) {}
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
        PhtEntry(const size_t sz, const SatCounter8 &conf)
            : TaggedEntry(), hist(sz, conf)
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
    } stats;

  public:
    XSCompositePrefetcher(const XSCompositePrefetcherParams &p);

    // dummy implementation, calc(3 args) will not call it
    void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses) override
    {
        panic("not implemented");
    };

    void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late,
                           PrefetchSourceType pf_source, bool miss_repeat) override;

    /** Update the RR right table after a prefetch fill */
    void notifyFill(const PacketPtr& pkt) override;

  private:
    const unsigned pfFilterSize{256};
    const unsigned pfPageFilterSize{16};
    boost::compute::detail::lru_cache<Addr, Addr> pfBlockLRUFilter;

    boost::compute::detail::lru_cache<Addr, Addr> pfPageLRUFilter;
    boost::compute::detail::lru_cache<Addr, Addr> pfPageLRUFilterL2;
    boost::compute::detail::lru_cache<Addr, Addr> pfPageLRUFilterL3;

    bool sendPFWithFilter(const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses, int prio,
                          PrefetchSourceType src, int ahead_level = -1);
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
    const bool enableCPLX;
    const bool enableSPP;
    const bool enableTemporal;
    const bool enableSstride;
    const bool enableBerti;
    const bool enableOpt;
    const bool enableXsstream;
    const bool phtEarlyUpdate;
    const bool neighborPhtUpdate;

  public:
    void notifyIns(int ins_num) override
    {
        if (hasHintDownStream()){
          hintDownStream->notifyIns(ins_num);
        }
    }
    void setParentInfo(System *sys, ProbeManager *pm, CacheAccessor* _cache, unsigned blk_size) override;
};

}
}  // gem5

#endif  // GEM5_SMS_HH
