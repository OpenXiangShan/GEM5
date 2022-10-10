//
// Created by linjiawei on 22-8-27.
//

#ifndef GEM5_SMS_HH
#define GEM5_SMS_HH

#include <vector>

#include "base/sat_counter.hh"
#include "base/types.hh"
#include "mem/cache/prefetch/associative_set.hh"
#include "mem/cache/prefetch/queued.hh"
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

    // filter table
    class FilterTableEntry : public TaggedEntry
    {
      public:
        Addr pc;
        bool is_secure;
        Addr region_offset;
        FilterTableEntry()
            : TaggedEntry(){

              };
    };

    AssociativeSet<FilterTableEntry> filter_table;

    FilterTableEntry *filterLookup(const PrefetchInfo &pfi);

    // active generation table
    class ACTEntry : public TaggedEntry
    {
      public:
        Addr pc;
        bool is_secure;
        uint64_t region_bits;
        SatCounter8 decr_counter;
        uint8_t access_cnt;
        ACTEntry(const SatCounter8 &conf)
            : TaggedEntry(), region_bits(0), decr_counter(conf), access_cnt(0)
        {
        }
    };

    AssociativeSet<ACTEntry> act;

    ACTEntry *actLookup(const PrefetchInfo &pfi);

    void updatePht(ACTEntry *act_entry);

    // pattern history table
    class PhtEntry : public TaggedEntry
    {
      public:
        std::vector<SatCounter8> hist;
        bool decr_mode;
        PhtEntry(const size_t sz, const SatCounter8 &conf)
            : TaggedEntry(), hist(sz, conf), decr_mode(false)
        {
        }
    };

    AssociativeSet<PhtEntry> pht;

    void phtLookup(const PrefetchInfo &pfi,
                   std::vector<AddrPriority> &addresses);

  public:
    SMSPrefetcher(const SMSPrefetcherParams &p);

    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses) override;
};

}
}  // gem5

#endif  // GEM5_SMS_HH
