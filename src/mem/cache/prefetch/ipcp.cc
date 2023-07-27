

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
      ADD_STAT(class_none, statistics::units::Count::get(),
            "demands not covered by prefetchs"),
      ADD_STAT(class_cs, statistics::units::Count::get(),
            "demands not covered by prefetchs"),
      ADD_STAT(class_cplx, statistics::units::Count::get(),
            "demands not covered by prefetchs"),
      ADD_STAT(class_nl, statistics::units::Count::get(),
            "demands not covered by prefetchs"),
      ADD_STAT(cplx_issued, statistics::units::Count::get(),
            "demands not covered by prefetchs"),
      ADD_STAT(cplx_filtered, statistics::units::Count::get(),
            "demands not covered by prefetchs")
{

}


bool
IPCP::sendPFWithFilter(Addr addr, std::vector<AddrPriority> &addresses, int prio)
{
    assert(rrf);
    if (rrf->contains(addr)) {
        DPRINTF(IPCP, "IPCP PF filtered\n");
        return false;
    } else {
        rrf->insert(addr, 0);
        addresses.push_back(AddrPriority(addr, prio, PrefetchSourceType::IPCP));
        return true;
    }
    return false;
}



IPCP::CSPEntry*
IPCP::cspLookup(uint32_t signature, int new_stride, bool update)
{
    auto& csp = cspt[signature];
    if (update) {
        if (csp.stride == new_stride) {
            csp.incConf();
        }
        else {
            // no hit
            csp.decConf();
            if (csp.confidence == 0) {
                // alloc new csp entry
                csp.stride = new_stride;
            }
            return nullptr;
        }
    }
    else if (csp.stride != new_stride) {
        return nullptr;
    }
    return &csp;
}


IPCP::IPEntry *
IPCP::ipLookup(Addr pc, Addr pf_addr, Classifier &type, int &new_stride)
{
    auto &ip = ipt[getIndex(pc)];
    IPEntry *ret = nullptr;
    new_stride = ((pf_addr - ip.last_addr) >> lBlkSize) & stride_mask;
    DPRINTF(IPCP, "IPCP last_addr: %lx, cur_addr: %lx, stride: %d\n", ip.last_addr, pf_addr, new_stride);
    if (ip.tag == getTag(pc)) {
        bool update = true;
        if (new_stride == 0) {
            update = false;
        }

        if (!ip.hysteresis) {
            ip.hysteresis = true;
        }
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
        }

        // cplx class
        auto csp = cspLookup(ip.signature, new_stride, update);

        // select
        // close CLASS NL, CS
        if (csp) {
            if (ip.cs_confidence == 0 && csp->confidence == 0) {
                type = CLASS_NL;
                ipcpStats.class_nl++;
            }
            else if (ip.cs_confidence >= csp->confidence) {
                type = CLASS_CS;
                ipcpStats.class_cs++;
            } else {
                type = CLASS_CPLX;
                ipcpStats.class_cplx++;
            }
        }
        else {
            if (ip.cs_confidence == 0) {
                type = CLASS_NL;
                ipcpStats.class_nl++;
            }
            else {
                type = CLASS_CS;
                ipcpStats.class_cs++;
            }
        }

        ret = &ip;
    } else {  // not match
        ipcpStats.class_none++;
        if (ip.hysteresis) {
            ip.hysteresis = false;
        } else {
            // alloc new entry
            ip.tag = getTag(pc);
            ip.hysteresis = false;
            ip.signature = 0;
            ip.cs_stride = new_stride;
            ip.cs_confidence = 0;

            type = CLASS_NL;
            ret = &ip;
        }
    }
    DPRINTF(IPCP,"IPCP IP lookup class: %d\n", (int)type);
    ip.last_addr = pf_addr;
    return ret;
}



void
IPCP::calculatePrefetch(const PrefetchInfo &pfi,
                        std::vector<AddrPriority> &addresses)
{
    if (!pfi.hasPC()) {
        return;
    }
    DPRINTF(IPCP, "IPCP lookup pc: %lx\n", pfi.getPC());
    Addr pf_addr = blockAddress(pfi.getAddr());
    int new_stride = -1;

    Classifier type = CLASS_NL;
    IPEntry *ip = ipLookup(pfi.getPC(), pf_addr, type, new_stride);
    assert(new_stride != -1);

    if (type == CLASS_CS) {
        assert(ip);
        Addr base_addr = pf_addr;
        for (int i = 1; i <= degree; i++) {
            base_addr = base_addr + (ip->cs_stride << lBlkSize);
            DPRINTF(IPCP, "IPCP CS Send pf: %lx, cur stride: %d, conf: %d\n", base_addr, ip->cs_stride, ip->cs_confidence);
            sendPFWithFilter(base_addr, addresses, 1);
        }
    } else if (type == CLASS_CPLX) {
        uint16_t signature = ip->signature;
        Addr base_addr = pf_addr;
        int high_conf = 0;
        for (int i = 1; i <= (high_conf < 3 ? degree : (degree << 1)); i++) {
            auto &csp = cspt[signature];
            base_addr = base_addr + (csp.stride << lBlkSize);
            if (csp.confidence > 0) {
                ipcpStats.cplx_issued++;
                DPRINTF(IPCP, "IPCP CPLX Send pf: %lx, cur stride: %d, conf: %d\n", base_addr, csp.stride, csp.confidence);
                if (!sendPFWithFilter(base_addr, addresses, 1)) {
                    ipcpStats.cplx_filtered++;
                }
            }
            if (csp.confidence == 3) {
                high_conf++;
            }
            signature = ((signature << 1) ^ csp.stride) & (cspt_size - 1);
        }
    } else if (type == CLASS_NL) {
        Addr base_addr = pf_addr;
        for (int i = 1; i <= degree; i++) {
            base_addr = base_addr + blkSize;
            DPRINTF(IPCP, "IPCP NL Send pf: %lx\n", base_addr);
            sendPFWithFilter(base_addr, addresses, 1);
        }
    }

    if (ip) {
        ip->sign(new_stride, cspt_size);
    }

    last_addr = pf_addr;
}

// void
// IPCP::calculatePrefetch_forSMS(const PrefetchInfo &pfi,
//                         std::vector<AddrPriority> &addresses)
// {
//     if (!pfi.hasPC()) {
//         // DPRINTF(StridePrefetcher, "Ignoring request with no PC.\n");
//         return;
//     }
//     Addr pf_addr = blockAddress(pfi.getAddr());
//     int new_stride = -1;

//     Classifier type = NO_PREFETCH;
//     IPEntry *ip = ipLookup(pfi.getPC(), pf_addr, type, new_stride);
//     assert(new_stride != -1);

//     if (type == CLASS_CS) {
//         assert(ip);
//         Addr base_addr = pf_addr;
//         for (int i = 1; i <= degree; i++) {
//             base_addr = base_addr + (ip->cs_stride << lBlkSize);
//             sendPFWithFilter(base_addr, addresses, 1);
//         }
//     } else if (type == CLASS_CPLX) {
//         uint16_t signature = ip->signature;
//         Addr base_addr = pf_addr;
//         int high_conf = 0;
//         for (int i = 1; i <= (high_conf < 3 ? degree : (degree << 1)); i++) {
//             auto &csp = cspt[signature];
//             base_addr = base_addr + (csp.stride << lBlkSize);
//             if (csp.confidence > 0) {
//                 ipcpStats.cplx_issued++;
//                 if (!sendPFWithFilter(base_addr, addresses, 1)) {
//                     ipcpStats.cplx_filtered++;
//                 }
//             }
//             if (csp.confidence == 3) {
//                 high_conf++;
//             }
//             signature = ((signature << 1) ^ csp.stride) & (cspt_size - 1);
//         }
//     } else if (type == CLASS_NL) {
//         assert(ip);
//         Addr base_addr = pf_addr;
//         for (int i = 1; i <= degree; i++) {
//             base_addr = base_addr + blkSize;
//             sendPFWithFilter(base_addr, addresses, 1);
//         }
//     }

//     if (ip) {
//         ip->sign(new_stride, cspt_size);
//     }

//     last_addr = pf_addr;
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
