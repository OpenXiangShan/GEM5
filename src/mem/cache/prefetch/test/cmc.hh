#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/circular_buffer.hpp>
#include <boost/compute/detail/lru_cache.hpp>

#include "base/types.hh"
#include "common/events.hh"
#include "common/mock_types.hh"
#include "common/simple_associative_set.hh"
#include "mem/cache/prefetch/test/common/simple_associative_set.hh"

namespace gem5
{
namespace prefetch
{
namespace test
{

// Forward declare the main class
class CMCPrefetcher;

// Mock parameters
struct CMCPrefetcherParams
{
    std::string name;
    unsigned degree;
    unsigned storage_assoc;
    unsigned cache_level;
    unsigned filter_size;
    unsigned nr_entry;
    size_t storage_entries;
    uint64_t block_size;

    CMCPrefetcherParams() :
      name("CMCPrefetcherTest"), degree(4),
      storage_assoc(16), cache_level(1),
      filter_size(256), nr_entry(16), storage_entries(16384),
      block_size(64) {}
};

class CMCPrefetcher
{
  public:
    CMCPrefetcher(const CMCPrefetcherParams &p);

    void doPrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late,
                           PrefetchSourceType pf_source, bool is_first_shot);

  private:
    // A simplified recorder for the test environment
    class Recorder
    {
      public:
        Recorder(unsigned nr_entry) : index(0), nr_entry(nr_entry) {}

        bool train_entry(Addr addr, bool is_secure, bool *finished);
        void reset();
        bool entry_empty() const { return entries.empty(); }

        std::vector<Addr> entries;
        int index;
        const unsigned nr_entry;
    };

    Recorder* recorder;

    // A simplified storage entry for the test environment
    struct CMCStorageEntry: public TestEntry
    {
      std::vector<Addr> addresses;
      // Add other members if needed for testing, e.g., refcnt, id
      int refcnt = 0;
      uint32_t id = 0;

      CMCStorageEntry() : TestEntry() {}

      void invalidate() override;
    };

    // Use a standard unordered_map instead of gem5's AssociativeSet
    SimpleAssociativeSet<CMCStorageEntry> storage;

    const unsigned cacheLevel;
    const unsigned degree;

    struct RecordEntry
    {
        Addr pc;
        Addr addr;
        bool is_secure;
        RecordEntry(Addr pc, Addr addr, bool is_secure) :
            pc(pc), addr(addr), is_secure(is_secure) {}
    };

    static const int STACK_SIZE = 8;
    boost::circular_buffer<RecordEntry> trigger;
    uint64_t acc_id = 0;

    Addr hash(Addr addr, Addr pc) const { return (addr >> 6) ^ pc; }

    bool sendPFWithFilter(const PrefetchInfo &pfi, Addr addr,
                          std::vector<AddrPriority> &addresses, int prio,
                          PrefetchSourceType src);

    boost::compute::detail::lru_cache<Addr, Addr> filter;
    uint64_t blockSize;

    Addr blockAddress(Addr addr) const { return addr & ~((Addr)blockSize-1); }
    Addr blockIndex(Addr addr) const { return addr >> blockSize; }
};

} // namespace test
} // namespace prefetch
} // namespace gem5
