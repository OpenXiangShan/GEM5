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
  public:
    enum Classifier
    {
      NO_PREFETCH,
      CLASS_CS,
      CLASS_CPLX
    };
  private:
    int degree;
    int cs_degree=0;
    int cs_thre = 1;
    int cplx_thre = 0;
    int signature_width = 8;
    int signature_shift = 2;

    int ipt_size;
    int lipt_size;
    int cspt_size;
    int rst_size = 0;
    int stride_mask = (1<<8) - 1;
    int tag_width = 10;


    uint16_t getIndex(Addr pc);
    uint16_t getTag(Addr pc);



    class IPEntry
    {
      public:
        uint32_t tag;
        bool hysteresis;
        Addr last_addr;
        uint8_t cs_stride;
        uint8_t cs_confidence;
        uint32_t signature;
        void cs_incConf() {cs_confidence = cs_confidence == 3 ? 3 : cs_confidence + 1;}
        void cs_decConf() {cs_confidence = cs_confidence == 0 ? 0 : cs_confidence - 1;}
    };

    class CSPEntry
    {
      public:
        int stride; // 7 bits
        uint8_t confidence; // 2bits
        bool abort;
        void incConf() {confidence = confidence == 3 ? 3 : confidence + 1;}
        void decConf() {confidence = confidence == 0 ? 0 : confidence - 1;}
    };
    std::vector<IPEntry> ipt;
    std::vector<CSPEntry> cspt;

    struct StatGroup : public statistics::Group
    {
        StatGroup(statistics::Group *parent);
        statistics::Scalar class_cs;
        statistics::Scalar class_cplx;
        statistics::Scalar cplx_issued;
        statistics::Scalar pf_filtered;
    } ipcpStats;

    IPEntry* saved_ip;
    Addr saved_pfAddr;

    void sign(uint32_t &signature, int stride) {signature = ((signature << signature_shift) ^ stride) & (cspt_size - 1);}
    //IPCP lookup pc: 47fda
    uint32_t compressSignature(uint32_t signature)
    {
        return signature;
    }
  public:
    Classifier saved_type;
    int saved_stride;

    // prefetch filter (32RR filter)
    boost::compute::detail::lru_cache<Addr, Addr> *rrf = nullptr;

    IPCP(const IPCPrefetcherParams &p);

    CSPEntry* cspLookup(uint32_t signature, int new_stride, bool update);

    // lookup ip table and return the best ip-class
    IPEntry* ipLookup(Addr pc, Addr pf_addr, Classifier &type, int &new_stride);

    bool sendPFWithFilter(Addr addr, std::vector<AddrPriority> &addresses, int prio, PrefetchSourceType pfSource);

    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses) override;

    void doLookup(const PrefetchInfo &pfi, PrefetchSourceType pf_source);

    void doPrefetch(std::vector<AddrPriority> &addresses);

};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_IPCP_HH__
