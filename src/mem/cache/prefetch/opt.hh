#ifndef __MEM_CACHE_PREFETCH_OPT_HH__
#define __MEM_CACHE_PREFETCH_OPT_HH__

#include <unordered_map>
#include <vector>

#include <boost/compute/detail/lru_cache.hpp>

#include "base/sat_counter.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "debug/OptPrefetcher.hh"
#include "mem/cache/prefetch/associative_set.hh"
#include "mem/cache/prefetch/queued.hh"
#include "mem/packet.hh"
#include "params/OptPrefetcher.hh"

namespace gem5
{
struct OptPrefetcherParams;
GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

// Opt is short for Offset pattern table prefetcher, a variant of SMS' pattern history table
class OptPrefetcher : public Queued
{
    protected:
      const unsigned int regionSize64;
      const unsigned int regionBlks64;
      const int optPFLevel;
      const uint OptLines =64;

      Addr regionAddress_64(Addr a) { return a / regionSize64; };
      Addr regionOffset_64(Addr a) { return (a / blkSize) % regionBlks64; }

      class ACT64Entry : public TaggedEntry
      {
        public:
          Addr pc;
          Addr regionAddr;
          bool is_secure;
          uint64_t region_bits_64;
          bool decr_mode;
          uint8_t access_cnt;
          uint64_t region_offset_64;
          ACT64Entry()
            : TaggedEntry(),
              region_bits_64(0),
              decr_mode(false),
              access_cnt(0),
              region_offset_64(0)
        {
        }
      };

      AssociativeSet<ACT64Entry> act_64;


      class OptEntry:public TaggedEntry
      {
        public:
         std::vector<SatCounter8> hist;
         Addr offset;
         Addr cof_4;
         Addr cof_3;
         Addr cof_2;
         Addr cof_1;
         OptEntry(const size_t sz ,const SatCounter8 &conf)
          :TaggedEntry(),hist(sz,conf)
          {
          }
      };
      AssociativeSet<OptEntry> opt;
      bool sendPFWithFilter(const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses, int prio,
                            PrefetchSourceType src, int ahead_level);

    public:
      boost::compute::detail::lru_cache<Addr, Addr> *filter;
      OptPrefetcher(const OptPrefetcherParams &p);

      using Queued::calculatePrefetch;

      void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses) override
      {
          panic("not implemented");
      };
      void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool &is_first_64);

      void updateOpt(ACT64Entry *act_64_entry, Addr region_addr, Addr region_bit_accessed_64);
      void cofNum(OptEntry *opt_entry, int j);
      bool optLookup(const Base::PrefetchInfo &pfi, std::vector<AddrPriority> &addresses);
};
}
}


#endif
