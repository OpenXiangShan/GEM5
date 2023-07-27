#ifndef __MEM_CACHE_PREFETCH_IPCP_HH__
#define __MEM_CACHE_PREFETCH_IPCP_HH__

#include <vector>

#include <boost/compute/detail/lru_cache.hpp>

#include "base/compiler.hh"
#include "base/sat_counter.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "mem/cache/prefetch/queued.hh"
#include "mem/cache/prefetch/signature_path.hh"
#include "mem/cache/prefetch/stride.hh"
#include "mem/packet.hh"
#include "params/IPCPrefetcher.hh"

namespace gem5
{

struct IPCPrefetcherParams;

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);

namespace prefetch{

class IPCP : public Queued
{

    int degree;
    int ipt_size;
    int lipt_size;
    int cspt_size;
    int rst_size = 0;
    int stride_mask = (1<<7) - 1;
    int tag_width = 7;


    uint16_t getIndex(Addr pc);
    uint16_t getTag(Addr pc);

    enum Classifier
    {
      NO_PREFETCH,
      CLASS_GS,
      CLASS_CS,
      CLASS_CPLX,
      CLASS_NL
    };

    class IPEntry
    {
      public:
        uint16_t tag; // 9 bits
        bool hysteresis; // 1 bit
        Addr last_addr;
        uint8_t cs_stride; // 7 bits
        uint8_t cs_confidence; // 2 bits (used for cs class)
        bool stream_valid;
        bool direction;
        uint16_t signature;
        void cs_incConf() {cs_confidence = cs_confidence == 3 ? 3 : cs_confidence + 1;}
        void cs_decConf() {cs_confidence = cs_confidence == 0 ? 0 : cs_confidence - 1;}
        void sign(Addr stride, int cspt_size) {signature = ((signature << 1) ^ stride) & (cspt_size - 1);}
    };

    class CSPEntry
    {
      public:
        int stride; // 7 bits
        uint8_t confidence; // 2bits

        void incConf() {confidence = confidence == 3 ? 3 : confidence + 1;}
        void decConf() {confidence = confidence == 0 ? 0 : confidence - 1;}
    };
    class RSEntry
    {
      public:
        uint8_t region_id;
        Addr last_addr;
        uint32_t bit_vector;
        uint16_t counter;
        bool dense;
        bool trained;
        bool tentative;
        bool direction;

    };

    Addr last_addr;
    std::vector<IPEntry> ipt;
    std::vector<CSPEntry> cspt;
    std::vector<RSEntry> rst;




    struct StatGroup : public statistics::Group
    {
        StatGroup(statistics::Group *parent);
        statistics::Scalar class_none;
        statistics::Scalar class_cs;
        statistics::Scalar class_cplx;
        statistics::Scalar class_nl;
        statistics::Scalar cplx_issued;
        statistics::Scalar cplx_filtered;
    } ipcpStats;

    IPEntry trained_ip;
    CSPEntry trained_csp;

  public:

    // prefetch filter (32RR filter)
    boost::compute::detail::lru_cache<Addr, Addr> *rrf = nullptr;

    IPCP(const IPCPrefetcherParams &p);

    CSPEntry* cspLookup(uint32_t signature, int new_stride, bool update);

    // lookup ip table and return the best ip-class
    IPEntry* ipLookup(Addr pc, Addr pf_addr, Classifier &type, int &new_stride);

    bool sendPFWithFilter(Addr addr, std::vector<AddrPriority> &addresses, int prio);

    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses) override;


    // void traing(const PrefetchInfo &pfi);

    // void calculatePrefetch_forSMS(const PrefetchInfo &pfi,
    //                        std::vector<AddrPriority> &addresses);

};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_IPCP_HH__
