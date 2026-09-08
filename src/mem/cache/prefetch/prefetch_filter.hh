// PrefetchFilter header moved out of sms.hh

#ifndef GEM5_PREFETCH_FILTER_HH
#define GEM5_PREFETCH_FILTER_HH

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "base/statistics.hh"
#include "base/types.hh"
#include "mem/cache/prefetch/associative_set.hh"
#include "mem/cache/prefetch/context_key.hh"
#include "mem/cache/prefetch/queued.hh"  // for AddrPriority and PrefetchSourceType
#include "mem/cache/tags/tagged_entry.hh"

namespace gem5 {
namespace prefetch {
class BaseIndexingPolicy;
class Base;
namespace replacement_policy { class Base; }
using AddrPriority = gem5::prefetch::Queued::AddrPriority;
class PrefetchFilter
{
  public:
  using TriggerInfo = Base::PFtriggerInfo;

    struct Entry : public TaggedEntry
    {
        Addr region_addr;    // region number (Vaddr[38:10] or Paddr[35:10] when paddr_valid)
        uint64_t region_bits; // which blocks in region should be prefetched (runtime width)
        uint64_t filter_bits; // which prefetch requests have been issued
        uint8_t alias_bits;   // Vaddr[13:12] for VIPT aliasing,not needed
        bool paddr_valid;     // true if region_addr is physical
        bool decr_mode;       // 1 if decrementing prefetch mode
        uint64_t PFlevel;      // prefetch level for this region, L1/L2/L3
        ContextID contextId;   // VA namespace of this region
        std::vector<std::unique_ptr<TriggerInfo>> bitTriggers;

        Entry()
            : TaggedEntry(), region_addr(0), region_bits(0), filter_bits(0), alias_bits(0),
              paddr_valid(false), decr_mode(false), PFlevel(0),
              contextId(InvalidContextID) {}

        Entry(const Entry &other)
            : TaggedEntry(other),
              region_addr(other.region_addr),
              region_bits(other.region_bits),
              filter_bits(other.filter_bits),
              alias_bits(other.alias_bits),
              paddr_valid(other.paddr_valid),
              decr_mode(other.decr_mode),
              PFlevel(other.PFlevel),
              contextId(other.contextId)
        {
            copyTriggers(other);
        }

        Entry& operator=(const Entry &other)
        {
            if (this != &other) {
                TaggedEntry::operator=(other);
                region_addr = other.region_addr;
                region_bits = other.region_bits;
                filter_bits = other.filter_bits;
                alias_bits = other.alias_bits;
                paddr_valid = other.paddr_valid;
                decr_mode = other.decr_mode;
                PFlevel = other.PFlevel;
                contextId = other.contextId;
                copyTriggers(other);
            }
            return *this;
        }

        Entry(Entry &&) noexcept = default;
        Entry& operator=(Entry &&) noexcept = default;
        ~Entry() = default;

        void _setSecure(bool is_secure) {
            if (is_secure) TaggedEntry::setSecure();
        }

      private:
        void copyTriggers(const Entry &other)
        {
            bitTriggers.clear();
            bitTriggers.reserve(other.bitTriggers.size());
            for (const auto &src : other.bitTriggers) {
                if (src) {
                    PacketPtr pkt = src->pkt;
                    bitTriggers.emplace_back(
                        std::make_unique<TriggerInfo>(pkt, *(src->pfi_old)));
                } else {
                    bitTriggers.emplace_back(nullptr);
                }
            }
        }
    };

    static constexpr unsigned DEFAULT_REGION_SIZE = 1024; // 1KB

    PrefetchFilter(gem5::BaseIndexingPolicy *idx_policy, gem5::replacement_policy::Base *rpl_policy,
      unsigned entries = 16, unsigned region_size = DEFAULT_REGION_SIZE,
      unsigned blk_size = 64, statistics::Group *parent = nullptr,
      unsigned vaddr_hash_width = 2, PrefetchSourceType pf_source_type = PrefetchSourceType::PF_NONE,
      const std::string &name = "prefetch_filter");
    ~PrefetchFilter();

    // Lookup entry by virtual address (uses VA->region conversion and TaggedEntry tag)
    Entry* findByVaddr(Addr vaddr, bool is_secure = false);

    // Lookup by region number (region = vaddr / REGION_SIZE)
    Entry* findByRegion(Addr region, bool is_secure = false);

    // Allocate or replace an entry for this vaddr/region. Returns the entry pointer.
    Entry* allocateForVaddr(Addr vaddr, bool is_secure = false, Addr region_addr = 0);

    // Mark that a specific block index in region has been issued as prefetch.
    void markBlockSent(Entry *e, unsigned blk_idx);

    // Add region_bits (OR) to the entry, marking predicted blocks for prefetch
    void addRegionBits(Entry *e, uint64_t bits);

    // Insert or update an entry for a given region. If an entry for `region`
    // exists, OR `region_bits` into the existing entry and return it. If it
    // doesn't, allocate/overwrite a victim entry and initialize its fields
    // (filter_bits defaults to 0) and insert it into the table. Returns the
    // updated or newly inserted Entry pointer.
    Entry* Insert(Addr region_addr = 0, uint64_t region_bits = 0, uint8_t alias_bits = 0,
      bool paddr_valid = false, bool decr_mode = false,
      bool is_secure = false, uint64_t PFlevel = 1,
      const TriggerInfo *trigger = nullptr);
    // Get blocks still pending prefetch (region_bits & ~filter_bits)
    uint64_t pendingBlocks(Entry *e) const;

    // Compute alias bits from virtual address
    static uint8_t aliasFromVaddr(Addr vaddr) { return (vaddr >> 12) & 0x3; }

  private:
    AssociativeSet<Entry> table;
    unsigned regionSize;
    unsigned blkSize;
    unsigned regionBlks;
    unsigned rrIndex{0};
    const unsigned REGION_ADDR_RAW_WIDTH;
    const unsigned vaddrHashWidth; // width for vaddr hash (per chisel spec)

    void ensureTriggerStorage(Entry &e);
    void storeTriggersForBits(Entry &e, uint64_t bits, const TriggerInfo *trigger);

    // Compute region-hash tag as described by chisel:
    // low  = region_tag[BLK_ADDR_RAW_WIDTH-1:0]
    // high = region_tag[BLK_ADDR_RAW_WIDTH-1+3*VADDR_HASH_WIDTH : BLK_ADDR_RAW_WIDTH]
    // high_hash = xor of 3 segments of VADDR_HASH_WIDTH bits from 'high'
    // tag = concat(high_hash, low)
    Addr regionHashTag(Addr vaddr) const;

  public:
    // Statistics for the PrefetchFilter. Parent should be provided by the
    // owner (e.g. XSCompositePrefetcher::stats) so counters are exposed in
    // gem5's statistics framework.
    struct Stats : public statistics::Group
    {
      Stats(statistics::Group *parent, const std::string &name);
        statistics::Scalar insertCount;
        statistics::Scalar queryHitCount;
        statistics::Scalar prefetchIssued;
        statistics::Scalar replacementCount;
        statistics::Scalar l1Calls;
        statistics::Scalar l1Issued;
        statistics::Scalar l2Calls;
        statistics::Scalar l2Issued;
        statistics::Scalar l3Calls;
        statistics::Scalar l3Issued;
        statistics::Scalar hashcollisionCount;
        statistics::Scalar contextAliasCount;
    } stats;
    PrefetchSourceType pfSourceType;
    const std::string table_name;
  public:
  // Select next prefetch address from the filter table using a round-robin
  // arbiter. Returns true and pushes an AddrPriority into `addresses` if a candidate is found.
  bool GetPFAddrL1(std::vector<AddrPriority> &addresses);
  bool GetPFAddrL2(std::vector<AddrPriority> &addresses);
  bool GetPFAddrL3(std::vector<AddrPriority> &addresses);
  bool hasPFRequestsInBuffer();
};

} // namespace prefetch
} // namespace gem5

#endif // GEM5_PREFETCH_FILTER_HH
