

#include "mem/cache/prefetch/ipcp.hh"

#include <cassert>

#include "debug/IPCP.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

IPCP::IPCP(const IPCPrefetcherParams &p)
    : Queued(p),
      degree(p.degree),
      ipt_size(p.ipt_size),
      cspt_size(p.cspt_size),
      ipcpStats(this)
{
    assert((ipt_size & (ipt_size - 1)) == 0);
    assert((cspt_size & (cspt_size - 1)) == 0);
    if (p.use_rrf) {
        rrf = new boost::compute::detail::lru_cache<Addr, Addr>(32);
    }

    ipt.resize(ipt_size);
    cspt.resize(cspt_size);

    lipt_size = floorLog2(ipt_size);
    for (auto &it : ipt) {
        it.hysteresis = false;
        it.last_addr = 0;
    }
    for (auto &it : cspt) {
        it.confidence = 0;
    }
}

IPCP::StatGroup::StatGroup(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(class_cs, statistics::units::Count::get(),
            "demands not covered by prefetchs"),
      ADD_STAT(class_cplx, statistics::units::Count::get(),
            "demands not covered by prefetchs"),
      ADD_STAT(cplx_issued, statistics::units::Count::get(),
            "demands not covered by prefetchs")
{

}


bool
IPCP::sendPFWithFilter(const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses, int prio,
                       PrefetchSourceType pfSource)
{
    assert(rrf);
    if (rrf->contains(addr)) {
        DPRINTF(IPCP, "IPCP PF filtered\n");
        ipcpStats.pf_filtered++;
        return false;
    } else {
        rrf->insert(addr, 0);
        addresses.push_back(AddrPriority(addr, prio, pfSource));
        return true;
    }
    return false;
}



IPCP::CSPEntry*
IPCP::cspLookup(uint32_t signature, int new_stride, bool update)
{
    auto& csp = cspt[compressSignature(signature)];

    if (csp.stride == new_stride) {
        csp.incConf();
    }
    else {
        csp.decConf();
        if (csp.confidence == 0) {
            // alloc new csp entry
            if (update) {
                csp.abort = false;
            }
            else {
                csp.abort = true;
            }
            csp.stride = new_stride;
        }
    }

    return &csp;
}


IPCP::IPEntry *
IPCP::ipLookup(Addr pc, Addr pf_addr, Classifier &type, int &new_stride)
{
    auto &ip = ipt[getIndex(pc)];
    IPEntry *ret = nullptr;

    new_stride = ((pf_addr - ip.last_addr) >> lBlkSize) & stride_mask;

    bool update = (pf_addr > ip.last_addr) && (((pf_addr - ip.last_addr) >> lBlkSize) <= stride_mask);
    if (!update) {
        new_stride = 0;
    }

    DPRINTF(IPCP, "IPCP cplx last_addr: %lx, cur_addr: %lx, stride: %d\n", ip.last_addr, pf_addr, new_stride);
    if (ip.tag == getTag(pc)) {

        if (!ip.hysteresis) {
            ip.hysteresis = true;
        }

        CSPEntry* csp = nullptr;

        // cs class
        if (update) {
            if (ip.cs_stride == new_stride) {
                ip.cs_incConf();
            } else {
                ip.cs_decConf();
                if (ip.cs_confidence == 0) {
                    ip.cs_stride = new_stride;
                }
            }
            if (ip.cs_confidence > cs_thre) {
                cs_degree = 4;
                if (ip.cs_confidence == 2) {
                    cs_degree = 2;
                }
                //type = CLASS_CS;
            }
        }
        else {
            ip.cs_decConf();
        }

        // cplx class
        csp = cspLookup(ip.signature, new_stride, update);

        // select
        if (csp) {
            if ((type == NO_PREFETCH) || (ip.cs_confidence < csp->confidence)) {
                type = CLASS_CPLX;
            }
        }

        if (type == CLASS_CPLX && !(csp->confidence > cplx_thre)) {
            type = NO_PREFETCH;
        }

        if (type == CLASS_CS) {
            ipcpStats.class_cs++;
        }
        else if (type == CLASS_CPLX) {
            ipcpStats.class_cplx++;
        }

        sign(ip.signature, new_stride);

        ip.last_addr = pf_addr;

        ret = &ip;
    } else {  // not match
        if (ip.hysteresis) {
            ip.hysteresis = false;
        } else {
            // alloc new entry
            ip.tag = getTag(pc);
            ip.hysteresis = false;
            ip.signature = 0;
            ip.cs_stride = 0;
            ip.cs_confidence = 0;
            ip.last_addr = pf_addr;
            ret = &ip;
        }
    }
    DPRINTF(IPCP,"IPCP IP lookup class: %d\n", (int)type);

    return ret;
}


void
IPCP::doLookup(const PrefetchInfo &pfi, PrefetchSourceType pf_source)
{
    bool can_prefetch = !pfi.isWrite() && pfi.hasPC();
    if (!can_prefetch) {
        return;
    }
    DPRINTF(IPCP, "IPCP lookup pc: %lx, vaddr: %lx\n", pfi.getPC(), pfi.getAddr());
    Addr pf_addr = blockAddress(pfi.getAddr());
    int new_stride = -1;

    Classifier type = NO_PREFETCH;
    IPEntry *ip = ipLookup(pfi.getPC(), pf_addr, type, new_stride);
    assert(new_stride != -1);

    saved_ip = ip;
    saved_type = type;
    saved_stride = new_stride;
    saved_pfAddr = pf_addr;
}

void
IPCP::sign(IPEntry &ipe, int stride)
{
    ipe.signature = ((ipe.signature << signature_shift) ^ stride) & (cspt_size - 1);
}

void
IPCP::sign(uint32_t &signature, int stride)
{
    signature = ((signature << signature_shift) ^ stride) & (cspt_size - 1);
}

bool
IPCP::doPrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, Addr &best_block_offset)
{
    bool send_cplx_pf = false;
    if (saved_type == CLASS_CS) {
        assert(saved_ip);
        Addr base_addr = saved_pfAddr;
        for (int i = 1; i <= cs_degree; i++) {
            base_addr = base_addr + (saved_ip->cs_stride << lBlkSize);
            DPRINTF(IPCP, "IPCP CS Send pf: %lx, cur stride: %d, conf: %d\n", base_addr, saved_ip->cs_stride, saved_ip->cs_confidence);
            sendPFWithFilter(pfi, base_addr, addresses, 1, PrefetchSourceType::IPCP_CS);
        }
    } else if (saved_type == CLASS_CPLX) {
        assert(saved_ip);
        uint32_t signature = saved_ip->signature;
        Addr base_addr = blockAddress(saved_pfAddr);
        int high_conf = 0;
        uint32_t init_signature = signature;
        Addr total_block_stride = 0;
        DPRINTF(IPCP, "IPCP prefetching\n");
        for (int i = 1; i <= signature_width / signature_shift; i++) {
            auto &csp = cspt[compressSignature(signature)];
            if (csp.abort || !(csp.confidence > cplx_thre)) {
                DPRINTF(IPCP, "IPCP CPLX forced abort\n");
                break;
            }
            base_addr = base_addr + (csp.stride << lBlkSize);
            total_block_stride += csp.stride;
            DPRINTF(IPCP, "IPCP CPLX Send pf: %lx, cur stride: %d, conf: %d\n", base_addr, csp.stride, csp.confidence);
            if (sendPFWithFilter(pfi, base_addr, addresses, 32, PrefetchSourceType::IPCP_CPLX)) {
                ipcpStats.cplx_issued++;
            }
            send_cplx_pf = true;
            if (csp.confidence == 3 && high_conf < 4) {
                high_conf++;
            }
            sign(signature, csp.stride);

            if ((signature & signMask) == (init_signature & signMask)) {
                DPRINTF(IPCP, "IPCP CPLX init sign: %lx, current sign: %lx\n", init_signature, signature);
                best_block_offset = total_block_stride;
                DPRINTF(IPCP, "CPLX found best blk offset: %u, best offset: %u\n", best_block_offset,
                        best_block_offset << lBlkSize);
            } else {
                DPRINTF(IPCP, "IPCP CPLX init sign: %lx, current sign: %lx\n", init_signature, signature);
            }
        }
    }
    return send_cplx_pf;
}
// void
// IPCP::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, Addr &best_block_offset)
// {
//     doLookup(pfi, PrefetchSourceType::IPCP);
//     doPrefetch(addresses, best_block_offset);
// }

uint16_t
IPCP::getIndex(Addr pc)
{
    return (pc >> 1) & (ipt_size - 1);
}
uint16_t
IPCP::getTag(Addr pc)
{
    return (pc >> (1 + lipt_size)) & ((1 << tag_width) - 1);
}

}

}
