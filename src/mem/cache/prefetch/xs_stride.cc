//Created on 24-01-03
//choose stride or berti in sms

#include "mem/cache/prefetch/xs_stride.hh"

#include "debug/XSStridePrefetcher.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"

namespace gem5
{
namespace prefetch
{

XSStridePrefetcher::XSStridePrefetcher(const XSStridePrefetcherParams &p)
    : Queued(p),useXsDepth(p.use_xs_depth),fuzzyStrideMatching(p.fuzzy_stride_matching),
      shortStrideThres(p.short_stride_thres),
      strideDynDepth(p.stride_dyn_depth),
      enableNonStrideFilter(p.enable_non_stride_filter),
      strideUnique(p.stride_entries, p.stride_entries, p.stride_indexing_policy,
             p.stride_replacement_policy, StrideEntry()),
      strideRedundant(p.stride_entries, p.stride_entries, p.stride_indexing_policy,
             p.stride_replacement_policy, StrideEntry()),
      nonStridePCs(p.non_stride_assoc, p.non_stride_entries, p.non_stride_indexing_policy,
             p.non_stride_replacement_policy, NonStrideEntry())
{
}


void
XSStridePrefetcher::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late,
                                       PrefetchSourceType pf_source, bool miss_repeat, bool enter_new_region,
                                       bool is_first_shot, Addr &pf_addr, int64_t &learned_bop_offset)
{
    if (is_first_shot) {
        DPRINTF(XSStridePrefetcher, "Do stride lookup for first shot acc ...\n");
        strideLookup(strideUnique, pfi, addresses, late, pf_addr, pf_source, enter_new_region, miss_repeat,
                     learned_bop_offset);
    } else {
        DPRINTF(XSStridePrefetcher, "Do stride lookup for repeat acc ...\n");
        strideLookup(strideRedundant, pfi, addresses, late, pf_addr, pf_source, enter_new_region, miss_repeat,
                     learned_bop_offset);
    }
}
bool
XSStridePrefetcher::strideLookup(AssociativeSet<StrideEntry> &stride, const PrefetchInfo &pfi,
                                  std::vector<AddrPriority> &addresses, bool late, Addr &stride_pf,
                                  PrefetchSourceType last_pf_source, bool enter_new_region, bool miss_repeat,
                                  int64_t &learned_bop_offset)
{
    Addr lookupAddr = pfi.getAddr();
    Addr stride_hash_pc = strideHashPc(pfi.getPC());
    StrideEntry *entry = stride.findEntry(stride_hash_pc, pfi.isSecure());
    learned_bop_offset = 0;
    // TODO: add DPRINFT for stride
    DPRINTF(XSStridePrefetcher, "Stride lookup: pc:%x addr: %x, miss repeat: %i\n", pfi.getPC(), lookupAddr,
            miss_repeat);
    bool should_cover = false;
    if (entry) {
        stride.accessEntry(entry);
        int64_t new_stride = lookupAddr - entry->lastAddr;
        if (new_stride == 0 || (labs(new_stride) < 64 && (miss_repeat || entry->longStride.calcSaturation() >= 0.5))) {
            DPRINTF(XSStridePrefetcher, "Stride touch in the same blk, ignore redundant req\n");
            return false;
        }
        bool stride_match = fuzzyStrideMatching ? (entry->stride > 64 && new_stride % entry->stride == 0) : false;
        stride_match |= new_stride == entry->stride;
        DPRINTF(XSStridePrefetcher, "Stride hit, with stride: %ld(%lx), old stride: %ld(%lx), long stride: %.2f\n",
                new_stride, new_stride, entry->stride, entry->stride, entry->longStride.calcSaturation());

        if (shortStrideThres) {
            if (labs(new_stride) > shortStrideThres) {
                entry->longStride.saturate();
            } else {
                entry->longStride--;
            }
        }

        if (shortStrideThres && entry->longStride.calcSaturation() > 0.5 && labs(new_stride) < shortStrideThres) {
            DPRINTF(XSStridePrefetcher, "Ignore short stride %li for long stride pattern\n", new_stride);
            return false;
        } else {
            DPRINTF(XSStridePrefetcher, "Stride long stride pattern: %.2f, short thres: %lu\n",
                    entry->longStride.calcSaturation(), shortStrideThres);
        }

        if (stride_match) {
            entry->conf++;
            if (strideDynDepth) {
                if (!pfi.isCacheMiss() && last_pf_source == PrefetchSourceType::SStride) {  // stride pref hit
                    entry->lateConf--;
                } else if (late) {  // stride pf late or other prefetcher late
                    entry->lateConf += 3;
                }
                if (entry->lateConf.isSaturated()) {
                    entry->depth++;
                    entry->lateConf.reset();
                } else if ((uint8_t)entry->lateConf == 0) {
                    entry->depth = std::max(1, entry->depth - 1);
                    entry->lateConf.reset();
                }
            }
            DPRINTF(XSStridePrefetcher, "Stride match, inc conf to %d, late: %i, late sat:%i, depth: %i\n",
                    (int)entry->conf, late, (uint8_t)entry->lateConf, entry->depth);
            entry->lastAddr = lookupAddr;
            entry->histStrides.clear();
            entry->matchedSinceAlloc = true;

        } else if (labs(entry->stride) > 64L && labs(new_stride) < 64L) {
            // different stride, but in the same cache line
            DPRINTF(XSStridePrefetcher, "Stride unmatch, but access goes to the same line, ignore\n");

        } else {
            entry->conf--;
            entry->lastAddr = lookupAddr;
            DPRINTF(XSStridePrefetcher, "Stride unmatch, dec conf to %d\n", (int)entry->conf);
            if ((int)entry->conf == 0) {
                DPRINTF(XSStridePrefetcher, "Stride conf = 0, reset stride to %ld\n", new_stride);

                bool found_in_hist = false;

                if (enableNonStrideFilter) {
                    if (entry->stride != 0) {
                        entry->histStrides.push_back(entry->stride);
                    }
                    for (auto it = entry->histStrides.begin(); it != entry->histStrides.end(); it++) {
                        DPRINTF(XSStridePrefetcher, "Stride hist: %ld, match: %i\n", *it, *it == new_stride);
                        if (*it == new_stride) {
                            found_in_hist = true;
                            entry->histStrides.erase(it);
                            break;
                        }
                    }
                    if (found_in_hist) {
                        entry->histStrides.clear();
                    }
                }

                if (enableNonStrideFilter && !found_in_hist && entry->histStrides.size() >= maxHistStrides) {
                    markNonStridePC(entry->pc);
                    entry->histStrides.clear();
                    entry->invalidate();
                } else {
                    entry->stride = new_stride;
                    entry->depth = 1;
                    entry->lateConf.reset();
                }
            }
        }
        if (entry->conf >= 2) {
            // if miss send 1*stride ~ depth*stride, else send depth*stride
            unsigned start_depth = pfi.isCacheMiss() ? std::max(1, (entry->depth - 4)) : entry->depth;
            Addr pf_addr = 0;
            if (useXsDepth) {
                sendPFWithFilter(pfi, blockAddress(lookupAddr + entry->stride * 2), addresses, 0,
                                 PrefetchSourceType::SStride);
                sendPFWithFilter(pfi, blockAddress(lookupAddr + entry->stride * 5), addresses, 0,
                                 PrefetchSourceType::SStride);
            } else {
                for (unsigned i = start_depth; i <= entry->depth; i++) {
                    pf_addr = lookupAddr + entry->stride * i;
                    DPRINTF(XSStridePrefetcher, "Stride conf >= 2, send pf: %x with depth %i\n", pf_addr, i);
                    sendPFWithFilter(pfi, blockAddress(pf_addr), addresses, 0, PrefetchSourceType::SStride);
                }
                stride_pf = pf_addr;  // the longest lookahead
            }

            should_cover = true;
        }
    } else {
        DPRINTF(XSStridePrefetcher, "Stride miss, insert it\n");
        entry = stride.findVictim(0);
        DPRINTF(XSStridePrefetcher, "Stride found victim pc = %x, stride = %i\n", entry->pc, entry->stride);
        if (enableNonStrideFilter && (entry->histStrides.size() >= maxHistStrides - 1 || !entry->matchedSinceAlloc)) {
            DPRINTF(XSStridePrefetcher, "Stride hist %u >= %u, mark pc %x as non-stride\n", entry->histStrides.size(),
                    maxHistStrides - 1, entry->pc);
            markNonStridePC(entry->pc);
        }
        if (entry->conf >= 2 && entry->stride > 1024) {  // > 1k
            DPRINTF(XSStridePrefetcher, "Stride Evicting a useful stride, send it to BOP with offset %i\n",
                    entry->stride / 64);
            // learnedBOP->tryAddOffset(entry->stride / 64);
            learned_bop_offset = entry->stride / 64;
        }
        entry->conf.reset();
        entry->lastAddr = lookupAddr;
        entry->stride = 0;
        entry->depth = 1;
        entry->lateConf.reset();
        entry->pc = pfi.getPC();
        entry->histStrides.clear();
        entry->matchedSinceAlloc = false;
        DPRINTF(XSStridePrefetcher, "Stride miss, insert with stride 0\n");
        stride.insertEntry(stride_hash_pc, pfi.isSecure(), entry);
    }
    periodStrideDepthDown();
    return should_cover;
}

void
XSStridePrefetcher::periodStrideDepthDown()
{
    if (depthDownCounter < depthDownPeriod) {
        depthDownCounter++;
    } else {
        for (auto stride : {&strideUnique, &strideRedundant}) {
            for (StrideEntry &entry : *stride) {
                if (entry.conf >= 2) {
                    entry.depth = std::max(entry.depth - 1, 1);
                }
            }
        }
        depthDownCounter = 0;
    }
}

void
XSStridePrefetcher::markNonStridePC(Addr pc)
{
    DPRINTF(XSStridePrefetcher, "Mark non-stride pc %x\n", pc);
    auto *entry = nonStridePCs.findEntry(nonStrideHash(pc), false);
    if (entry) {
        nonStridePCs.accessEntry(entry);
    } else {
        entry = nonStridePCs.findVictim(nonStrideHash(pc));
        assert(entry);
        entry->pc = pc;
        nonStridePCs.insertEntry(nonStrideHash(pc), false, entry);
    }
}

bool
XSStridePrefetcher::isNonStridePC(Addr pc)
{
    auto *entry = nonStridePCs.findEntry(nonStrideHash(pc), false);
    return entry != nullptr;
}

bool
XSStridePrefetcher::sendPFWithFilter(const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses,
                                      int prio, PrefetchSourceType src)
{
    if (filter->contains(addr)) {
        DPRINTF(XSStridePrefetcher, "Skip recently prefetched: %lx\n", addr);
        return false;
    } else {
        DPRINTF(XSStridePrefetcher, "Send pf: %lx\n", addr);
        filter->insert(addr, 0);
        addresses.push_back(AddrPriority(addr, prio, src));
        return true;
    }
}
Addr
XSStridePrefetcher::strideHashPc(Addr pc)
{
    Addr pc_high_1 = (pc >> 20) & (0x1f);
    Addr pc_high_2 = (pc >> 15) & (0x1f);
    Addr pc_high_3 = (pc >> 10) & (0x1f);
    Addr pc_high = pc_high_1 ^ pc_high_2 ^ pc_high_3;
    Addr pc_low = pc & (0x1ff);
    return (pc_high << 10) | pc_low;
}

}

}
