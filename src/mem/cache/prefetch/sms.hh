//
// Created by linjiawei on 22-8-27.
//

#ifndef GEM5_SMS_HH
#define GEM5_SMS_HH

#include <vector>

#include <boost/compute/detail/lru_cache.hpp>

#include "base/sat_counter.hh"
#include "base/types.hh"
#include "mem/cache/prefetch/associative_set.hh"
#include "mem/cache/prefetch/bop.hh"
#include "mem/cache/prefetch/queued.hh"
#include "mem/cache/prefetch/signature_path.hh"
#include "mem/cache/prefetch/stride.hh"
#include "mem/cache/tags/tagged_entry.hh"
#include "mem/packet.hh"
#include "params/SMSPrefetcher.hh"

namespace gem5
{
struct SMSPrefetcherParams;

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

class SMSPrefetcher : public Queued
{
  protected:
    const unsigned int region_size;
    const unsigned int region_blocks;


    Addr regionAddress(Addr a) { return a / region_size; };

    Addr regionOffset(Addr a) { return (a / blkSize) % region_blocks; }


    // active generation table
    class ACTEntry : public TaggedEntry
    {
      public:
        Addr pc;
        bool is_secure;
        uint64_t region_bits;
        bool decr_mode;
        uint8_t access_cnt;
        uint64_t region_offset;
        uint32_t depth;
        SatCounter8 lateConf;
        ACTEntry(const SatCounter8 &conf)
            : TaggedEntry(),
              region_bits(0),
              decr_mode(false),
              access_cnt(0),
              region_offset(0),
              depth(32),
              lateConf(4, 7)
        {
        }
        bool in_active_page() {
            // FIXME: remove hard-code 12
            return access_cnt > 12;
        }
    };

    AssociativeSet<ACTEntry> act;

    ACTEntry *actLookup(const PrefetchInfo &pfi, bool &in_active_page);

    const unsigned streamDepthStep{4};  // # block changed in one step

    // stride table
    class StrideEntry : public TaggedEntry
    {
      public:
        int64_t stride;
        uint64_t last_addr;
        SatCounter8 conf;
        int32_t depth;
        SatCounter8 lateConf;
        Addr pc;
        StrideEntry(const SatCounter8 & _conf)
            : TaggedEntry(),
              stride(0),
              last_addr(0),
              conf(_conf),
              depth(1),
              lateConf(4, 7),
              pc(0)
        {}
    };
    const bool strideDynDepth{false};

    int depthDownCounter{0};

    const int depthDownPeriod{256};

    void periodStrideDepthDown();

    bool strideLookup(const PrefetchInfo &pfi, std::vector<AddrPriority> &address, bool late, Addr &pf_addr,
                      PrefetchSourceType src);

    AssociativeSet<StrideEntry> stride;

    void updatePht(ACTEntry *act_entry);

    // pattern history table
    class PhtEntry : public TaggedEntry
    {
      public:
        std::vector<SatCounter8> hist;
        PhtEntry(const size_t sz, const SatCounter8 &conf)
            : TaggedEntry(), hist(sz, conf)
        {
        }
    };

    AssociativeSet<PhtEntry> pht;

    bool phtLookup(const PrefetchInfo &pfi,
                   std::vector<AddrPriority> &addresses, bool late, Addr look_ahead_addr);

    int calcPeriod(const std::vector<SatCounter8> &bit_vec, bool late);

  public:
    SMSPrefetcher(const SMSPrefetcherParams &p);

    // dummy implementation, calc(3 args) will not call it
    void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses) override
    {
        panic("not implemented");
    };

    void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late,
                           PrefetchSourceType pf_source) override;

    /** Update the RR right table after a prefetch fill */
    void notifyFill(const PacketPtr& pkt) override;

  private:
    const unsigned pfFilterSize{128};
    boost::compute::detail::lru_cache<Addr, Addr> pfBlockLRUFilter;
    bool sendPFWithFilter(Addr addr, std::vector<AddrPriority> &addresses, int prio, PrefetchSourceType src);

    BOP *bop;

    SignaturePath  *spp;
};

}
}  // gem5

#endif  // GEM5_SMS_HH
