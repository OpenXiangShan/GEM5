#include "mem/cache/prefetch/sms.hh"

#include "debug/XSCompositePrefetcher.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"

namespace gem5
{
namespace prefetch
{

XSCompositePrefetcher::XSCompositePrefetcher(const XSCompositePrefetcherParams &p)
    : Queued(p),
      regionSize(p.region_size),
      regionBlks(p.region_size / p.block_size),
      act(p.act_entries, p.act_entries, p.act_indexing_policy,
          p.act_replacement_policy, ACTEntry(SatCounter8(2, 1))),
      re_act(p.re_act_entries, p.re_act_entries, p.re_act_indexing_policy,
          p.re_act_replacement_policy,ReACTEntry()),
      streamPFAhead(p.stream_pf_ahead),
      strideDynDepth(p.stride_dyn_depth),
      strideUnique(p.stride_entries, p.stride_entries, p.stride_indexing_policy,
             p.stride_replacement_policy, StrideEntry()),
      strideRedundant(p.stride_entries, p.stride_entries, p.stride_indexing_policy,
             p.stride_replacement_policy, StrideEntry()),
      nonStridePCs(p.non_stride_assoc, p.non_stride_entries, p.non_stride_indexing_policy,
             p.non_stride_replacement_policy, NonStrideEntry()),
      fuzzyStrideMatching(p.fuzzy_stride_matching),
      pht(p.pht_assoc, p.pht_entries, p.pht_indexing_policy,
          p.pht_replacement_policy,
          PhtEntry(2 * (regionBlks - 1), SatCounter8(3, 2))),
      phtPFAhead(p.pht_pf_ahead),
      phtPFLevel(p.pht_pf_level),
      stats(this),
      pfBlockLRUFilter(pfFilterSize),
      pfPageLRUFilter(pfPageFilterSize),
      pfPageLRUFilterL2(pfPageFilterSize),
      pfPageLRUFilterL3(pfPageFilterSize),
      largeBOP(dynamic_cast<BOP *>(p.bop_large)),
      smallBOP(dynamic_cast<BOP *>(p.bop_small)),
      spp(dynamic_cast<SignaturePath *>(p.spp)),
      ipcp(dynamic_cast<IPCP *>(p.ipcp)),
      cmc(p.cmc),
      enableNonStrideFilter(p.enable_non_stride_filter),
      enableCPLX(p.enable_cplx),
      enableSPP(p.enable_spp),
      shortStrideThres(p.short_stride_thres)
{
    assert(largeBOP);
    assert(smallBOP);
    assert(isPowerOf2(regionSize));


    largeBOP->filter = &this->pfBlockLRUFilter;
    smallBOP->filter = &this->pfBlockLRUFilter;

    cmc->filter = &this->pfBlockLRUFilter;

    ipcp->rrf = &this->pfBlockLRUFilter;

    DPRINTF(XSCompositePrefetcher, "SMS: region_size: %d regionBlks: %d\n",
            regionSize, regionBlks);
}

void
XSCompositePrefetcher::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late,
                                 PrefetchSourceType pf_source, bool miss_repeat)
{
    bool can_prefetch = !pfi.isWrite() && pfi.hasPC();
    if (!can_prefetch) {
        return;
    }

    Addr pc = pfi.getPC();
    Addr vaddr = pfi.getAddr();
    Addr block_addr = blockAddress(vaddr);
    PrefetchSourceType stream_type = PrefetchSourceType::SStream;
    if (pfi.isStore()) {
        stream_type = PrefetchSourceType::StoreStream;
        DPRINTF(XSCompositePrefetcher, "prefetch trigger come from store unit\n");
    }
    DPRINTF(XSCompositePrefetcher, "blk addr: %lx, prefetch source: %i, miss: %i, late: %i, ever pf: %i, pc: %lx\n",
            block_addr, pf_source, pfi.isCacheMiss(), late, pfi.isEverPrefetched(), pfi.getPC());

    // if (!pfi.isCacheMiss()) {
    //     assert(pf_source != PrefetchSourceType::PF_NONE);
    // }

    // Addr region_addr = regionAddress(vaddr);
    Addr region_offset = regionOffset(vaddr);
    bool is_active_page = false;
    bool enter_new_region = false;
    bool is_first_shot = false;
    ACTEntry *act_match_entry = nullptr;
    Addr pf_tgt_addr = 0;
    bool decr = false;
    if (pfi.isCacheMiss() || pfi.isPfFirstHit()) {
        act_match_entry = actLookup(pfi, is_active_page, enter_new_region, is_first_shot);
        int origin_depth = 0;
        if (act_match_entry) {
            decr = act_match_entry->decr_mode;
            DPRINTF(XSCompositePrefetcher, "ACT hit or match: pc:%x addr: %x offset: %d active: %d decr: %d\n", pc,
                    vaddr, region_offset, is_active_page, decr);
            if (is_active_page) {
                origin_depth = act_match_entry->depth;
                int depth = 16;
                // active page
                pf_tgt_addr = decr ? block_addr - depth * blkSize : block_addr + depth * blkSize;  // depth here?
                Addr pf_tgt_region = regionAddress(pf_tgt_addr);
                Addr pf_tgt_offset = regionOffset(pf_tgt_addr);
                DPRINTF(XSCompositePrefetcher, "tgt addr: %x, offset: %d, current depth: %u, page: %lx\n", pf_tgt_addr,
                        pf_tgt_offset, depth, pf_tgt_region);
                if (decr) {
                    // for (int i = (int)regionBlks - 1; i >= pf_tgt_offset && i >= 0; i--) {
                    for (int i = regionBlks - 1; i >= 0; i--) {
                        Addr cur = pf_tgt_region * regionSize + i * blkSize;
                        sendPFWithFilter(cur, addresses, i, stream_type);
                        DPRINTF(XSCompositePrefetcher, "pf addr: %x [%d]\n", cur, i);
                        fatal_if(i < 0, "i < 0\n");
                    }
                } else {
                    // for (int i = std::max(1, ((int) pf_tgt_offset) - 4); i <= pf_tgt_offset; i++) {
                    for (int i = 0; i < regionBlks; i++) {
                        Addr cur = pf_tgt_region * regionSize + i * blkSize;
                        sendPFWithFilter(cur, addresses, regionBlks - i, stream_type);
                        DPRINTF(XSCompositePrefetcher, "pf addr: %x [%d]\n", cur, i);
                    }
                }
                pfPageLRUFilter.insert(pf_tgt_region, 0);
            }
        }
    }

    if (act_match_entry && is_active_page && pf_tgt_addr && enter_new_region) {
        if (streamPFAhead) {
            pf_tgt_addr = decr ? pf_tgt_addr - 48 * blkSize
                               : pf_tgt_addr + 48 * blkSize;  // depth here?
            Addr pf_tgt_region = regionAddress(pf_tgt_addr);
            DPRINTF(XSCompositePrefetcher, "ACT pf ahead region: %lx\n", pf_tgt_region);
            for (int i = 0; i < regionBlks; i++) {
                Addr cur = pf_tgt_region * regionSize + i * blkSize;
                sendPFWithFilter(cur, addresses, regionBlks - i, stream_type, 2);
            }
            pfPageLRUFilterL2.insert(pf_tgt_region, 0);
        }

        if (streamPFAhead) {
            pf_tgt_addr = decr ? pf_tgt_addr - 256 * blkSize
                               : pf_tgt_addr + 256 * blkSize;  // depth here?
            Addr pf_tgt_region = regionAddress(pf_tgt_addr);
            DPRINTF(XSCompositePrefetcher, "ACT pf ahead region: %lx\n", pf_tgt_region);
            for (int i = 0; i < regionBlks; i++) {
                Addr cur = pf_tgt_region * regionSize + i * blkSize;
                sendPFWithFilter(cur, addresses, regionBlks - i, stream_type, 3);
            }
            pfPageLRUFilterL3.insert(pf_tgt_region, 0);
        }
    }

    if ((pf_source == PrefetchSourceType::SStream || pf_source == PrefetchSourceType::StoreStream) || act_match_entry) {
        auto it = act.begin();
        while (it != act.end()) {
            ACTEntry *it_entry = &(*it);
            if (late) {
                it_entry->lateConf += 3;
                if (it_entry->lateConf.isSaturated()) {
                    it_entry->depth = std::min(128U, (unsigned)it_entry->depth + 1);
                    it_entry->lateConf.reset();
                }
            } else if (!pfi.isCacheMiss()) {
                it_entry->lateConf--;
                if ((int)it_entry->lateConf == 0) {
                    it_entry->depth = std::max(1U, (unsigned)it_entry->depth - 1);
                    it_entry->lateConf.reset();
                }
            }

            it++;
        }
        it = act.begin();
        ACTEntry *it_entry = &(*it);
        if (late || !pfi.isCacheMiss()) {
            DPRINTF(XSCompositePrefetcher, "act entry %lx, late or hit, now depth: %d, lateConf: %d\n",
                    it_entry->getTag(), it_entry->depth, (int)it_entry->lateConf);
        }
    }

    if (enableCPLX) {
        ipcp->doLookup(pfi, pf_source);
    }

    if (pf_source != PrefetchSourceType::SStream && !is_active_page) {

        bool use_bop = (pfi.isPfFirstHit() &&
                        (pf_source == PrefetchSourceType::HWP_BOP || pf_source == PrefetchSourceType::IPCP_CPLX)) ||
                       pfi.isCacheMiss();
        use_bop &= !miss_repeat && is_first_shot; // miss repeat should not be handled by stride
        if (use_bop) {
            DPRINTF(XSCompositePrefetcher, "Do BOP traing/prefetching...\n");
            size_t old_addr_size = addresses.size();
            largeBOP->calculatePrefetch(pfi, addresses, late && pf_source == PrefetchSourceType::HWP_BOP);

            old_addr_size = addresses.size();
            smallBOP->calculatePrefetch(pfi, addresses, late && pf_source == PrefetchSourceType::HWP_BOP);
        }

        bool use_stride = pfi.isCacheMiss() ||
                          (pfi.isPfFirstHit() &&
                           (pf_source == PrefetchSourceType::SStride || pf_source == PrefetchSourceType::HWP_BOP ||
                            pf_source == PrefetchSourceType::SPht || pf_source == PrefetchSourceType::IPCP_CPLX));
        use_stride &= !pfi.isStore();

        if (enableNonStrideFilter) {
            use_stride &= !isNonStridePC(pc);
            if (isNonStridePC(pc)) {
                DPRINTF(XSCompositePrefetcher, "Skip stride lookup for non-stride pc %x\n", pc);
            }
        }
        Addr stride_pf_addr = 0, stride_pf_addr2 = 0;
        bool covered_by_stride = false;
        if (use_stride) {
            if (is_first_shot) {
                DPRINTF(XSCompositePrefetcher, "Do stride lookup for first shot acc ...\n");
                covered_by_stride |= strideLookup(strideUnique, pfi, addresses, late, stride_pf_addr, pf_source,
                                                  enter_new_region, miss_repeat);
            } else {
                DPRINTF(XSCompositePrefetcher, "Do stride lookup for repeat acc ...\n");
                covered_by_stride |= strideLookup(strideRedundant, pfi, addresses, late, stride_pf_addr2, pf_source,
                                                  enter_new_region, miss_repeat);
            }
        }

        bool use_pht = pfi.isCacheMiss() ||
                       (pfi.isPfFirstHit() &&
                        (pf_source == PrefetchSourceType::SStride || pf_source == PrefetchSourceType::HWP_BOP ||
                         pf_source == PrefetchSourceType::SPht || pf_source == PrefetchSourceType::IPCP_CPLX ||
                         pf_source == PrefetchSourceType::SPP));

        use_pht &= !pfi.isStore();

        bool trigger_pht = false;
        stride_pf_addr =
            phtPFAhead ? (stride_pf_addr ? stride_pf_addr : stride_pf_addr2) : 0;  // trigger addr sent to pht
        if (use_pht) {
            DPRINTF(XSCompositePrefetcher, "Do PHT lookup...\n");
            trigger_pht = phtLookup(pfi, addresses, late && pf_source == PrefetchSourceType::SPht, stride_pf_addr);
        }

        bool use_cplx = enableCPLX && !pfi.isStore();
        if (use_cplx) {
            Addr cplx_best_offset = 0;
            bool send_cplx_pf = ipcp->doPrefetch(addresses, cplx_best_offset);

            if (send_cplx_pf && cplx_best_offset != 0) {
                largeBOP->tryAddOffset(cplx_best_offset, late);
            }
        }

        bool use_spp = enableSPP && !pfi.isStore();
        if (use_spp) {
            int32_t spp_best_offset = 0;
            bool coverd_by_spp = spp->calculatePrefetch(pfi, addresses, pfBlockLRUFilter, spp_best_offset);
            if (coverd_by_spp && spp_best_offset != 0) {
                // TODO: Let BOP to adjust depth by itself
                largeBOP->tryAddOffset(spp_best_offset, late);
            }
        }
    }

    if (is_first_shot && (pfi.isCacheMiss() || pfi.isPfFirstHit())) {
        cmc->doPrefetch(pfi, addresses, late, pf_source, false);
    }

}

XSCompositePrefetcher::ACTEntry *
XSCompositePrefetcher::actLookup(const PrefetchInfo &pfi, bool &in_active_page, bool &alloc_new_region,
                                 bool &is_first_shot)
{
    Addr pc = pfi.getPC();
    Addr vaddr = pfi.getAddr();
    Addr region_addr = regionAddress(vaddr);
    Addr region_start = regionAddress(vaddr) * regionSize;
    Addr region_offset = regionOffset(vaddr);
    bool secure = pfi.isSecure();
    ReACTEntry *re_act_entry = nullptr;
    bool re_act_mode = false;


    ACTEntry *entry = act.findEntry(region_addr, secure);
    if (entry) {
        // act hit
        act.accessEntry(entry);
        in_active_page = entry->in_active_page(regionBlks);
        uint64_t region_bit_accessed = 1UL << region_offset;
        if (!(entry->region_bits & region_bit_accessed)) {
            entry->access_cnt += 1;
            is_first_shot = true;
        }
        entry->region_bits |= region_bit_accessed;
        // print bits
        DPRINTF(XSCompositePrefetcher, "Access region %lx, after access bit %lu, new act entry bits:\n", region_start,
                region_offset);
        for (uint8_t i = 0; i < regionBlks; i++) {
            DPRINTFR(XSCompositePrefetcher, "%lu ", (entry->region_bits >> i) & 1);
        }
        DPRINTFR(XSCompositePrefetcher, "\n");
        return entry;
    }

    alloc_new_region = true;
    is_first_shot = true;

    bool found = false;
    bool forward = true;

    ACTEntry *old_neighbor_entry = act.findEntry(region_addr - 1, secure);
    if (old_neighbor_entry) {
        // act miss, but cur_region - 1 = entry_region found, => cur_region = entry_region + 1
        in_active_page = old_neighbor_entry->in_active_page(regionBlks);
        found = true;
        forward = true;
    } else if ((old_neighbor_entry = act.findEntry(region_addr + 1, secure))) {
        // act miss, but cur_region + 1 = entry_region found, => cur_region = entry_region - 1
        in_active_page = old_neighbor_entry->in_active_page(regionBlks);
        found = true;
        forward = false;
    }

    entry = act.findVictim(0);

    re_act_entry = re_act.findEntry(entry->regionAddr, secure);
    if (re_act_entry) {
        re_act_mode = true;
        stats.actMNum++;
        entry->pc = re_act_entry->pc;
    } else {
        stats.allCntNum++;
        re_act_entry = re_act.findVictim(0);
        re_act_entry->pc = entry->pc;
        re_act_entry->regionAddr = entry->regionAddr;
        re_act_entry->is_secure = entry->is_secure;
        re_act.insertEntry(re_act_entry->regionAddr, re_act_entry->is_secure,
                           re_act_entry);
    }

    updatePht(entry, region_start,re_act_mode);  // update pht with evicted entry
    entry->pc = pc;
    entry->is_secure = secure;
    entry->decr_mode = !forward;
    entry->regionAddr = region_start;
    entry->region_offset = region_offset;
    entry->region_bits = 1UL << region_offset;
    entry->access_cnt = 1;
    act.insertEntry(region_addr, secure, entry);

    // print bits
    DPRINTF(XSCompositePrefetcher, "Access new region %lx, after access bit %lu, new act entry bits:\n", region_start,
            region_offset);
    for (uint8_t i = 0; i < regionBlks; i++) {
        DPRINTFR(XSCompositePrefetcher, "%lu ", (entry->region_bits >> i) & 1);
    }
    DPRINTFR(XSCompositePrefetcher, "\n");

    if (found) {
        DPRINTF(XSCompositePrefetcher, "ACT miss, but %s region is active, copy depth = %u, lateConf = %i\n",
                forward ? "last" : "next", entry->depth, (int)entry->lateConf);
        entry->lateConf = old_neighbor_entry->lateConf;
        entry->depth = old_neighbor_entry->depth;
        return entry;

    } else {
        DPRINTF(XSCompositePrefetcher, "ACT miss, allocate new region\n");
        return nullptr;
    }
}

void
XSCompositePrefetcher::markNonStridePC(Addr pc)
{
    DPRINTF(XSCompositePrefetcher, "Mark non-stride pc %x\n", pc);
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
XSCompositePrefetcher::isNonStridePC(Addr pc)
{
    auto *entry = nonStridePCs.findEntry(nonStrideHash(pc), false);
    return entry != nullptr;
}

bool
XSCompositePrefetcher::strideLookup(AssociativeSet<StrideEntry> &stride, const PrefetchInfo &pfi,
                                    std::vector<AddrPriority> &addresses, bool late, Addr &stride_pf,
                                    PrefetchSourceType last_pf_source, bool enter_new_region, bool miss_repeat)
{
    Addr lookupAddr = pfi.getAddr();
    StrideEntry *entry = stride.findEntry(pfi.getPC(), pfi.isSecure());
    // TODO: add DPRINFT for stride
    DPRINTF(XSCompositePrefetcher, "Stride lookup: pc:%x addr: %x, miss repeat: %i\n", pfi.getPC(), lookupAddr,
            miss_repeat);
    bool should_cover = false;
    if (entry) {
        stride.accessEntry(entry);
        int64_t new_stride = lookupAddr - entry->last_addr;
        if (new_stride == 0 || (labs(new_stride) < 64 && (miss_repeat || entry->longStride.calcSaturation() >= 0.5))) {
            DPRINTF(XSCompositePrefetcher, "Stride touch in the same blk, ignore redundant req\n");
            return false;
        }
        bool stride_match = fuzzyStrideMatching ? (entry->stride > 64 && new_stride % entry->stride == 0) : false;
        stride_match |= new_stride == entry->stride;
        DPRINTF(XSCompositePrefetcher, "Stride hit, with stride: %ld(%lx), old stride: %ld(%lx), long stride: %.2f\n",
                new_stride, new_stride, entry->stride, entry->stride, entry->longStride.calcSaturation());

        if (shortStrideThres) {
            if (labs(new_stride) > shortStrideThres) {
                entry->longStride.saturate();
            } else {
                entry->longStride--;
            }
        }

        if (shortStrideThres && entry->longStride.calcSaturation() > 0.5 && labs(new_stride) < shortStrideThres) {
            DPRINTF(XSCompositePrefetcher, "Ignore short stride %li for long stride pattern\n", new_stride);
            return false;
        } else {
            DPRINTF(XSCompositePrefetcher, "Stride long stride pattern: %.2f, short thres: %lu\n",
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
            DPRINTF(XSCompositePrefetcher, "Stride match, inc conf to %d, late: %i, late sat:%i, depth: %i\n",
                    (int)entry->conf, late, (uint8_t)entry->lateConf, entry->depth);
            entry->last_addr = lookupAddr;
            entry->histStrides.clear();
            entry->matchedSinceAlloc = true;

        } else if (labs(entry->stride) > 64L && labs(new_stride) < 64L) {
            // different stride, but in the same cache line
            DPRINTF(XSCompositePrefetcher, "Stride unmatch, but access goes to the same line, ignore\n");

        } else {
            entry->conf--;
            entry->last_addr = lookupAddr;
            DPRINTF(XSCompositePrefetcher, "Stride unmatch, dec conf to %d\n", (int) entry->conf);
            if ((int) entry->conf == 0) {
                DPRINTF(XSCompositePrefetcher, "Stride conf = 0, reset stride to %ld\n", new_stride);

                bool found_in_hist = false;

                if (enableNonStrideFilter) {
                    if (entry->stride != 0) {
                        entry->histStrides.push_back(entry->stride);
                    }
                    for (auto it = entry->histStrides.begin(); it != entry->histStrides.end(); it++) {
                        DPRINTF(XSCompositePrefetcher, "Stride hist: %ld, match: %i\n", *it, *it == new_stride);
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
            for (unsigned i = start_depth; i <= entry->depth; i++) {
                pf_addr = lookupAddr + entry->stride * i;
                DPRINTF(XSCompositePrefetcher, "Stride conf >= 2, send pf: %x with depth %i\n", pf_addr, i);
                sendPFWithFilter(blockAddress(pf_addr), addresses, 0, PrefetchSourceType::SStride);
            }
            stride_pf = pf_addr;  // the longest lookahead
            should_cover = true;
        }
    } else {
        DPRINTF(XSCompositePrefetcher, "Stride miss, insert it\n");
        entry = stride.findVictim(0);
        DPRINTF(XSCompositePrefetcher, "Stride found victim pc = %x, stride = %i\n", entry->pc, entry->stride);
        if (enableNonStrideFilter && (entry->histStrides.size() >= maxHistStrides - 1 || !entry->matchedSinceAlloc)) {
            DPRINTF(XSCompositePrefetcher, "Stride hist %u >= %u, mark pc %x as non-stride\n",
                    entry->histStrides.size(), maxHistStrides - 1, entry->pc);
            markNonStridePC(entry->pc);
        }
        if (entry->conf >= 2 && entry->stride > 1024) { // > 1k
            DPRINTF(XSCompositePrefetcher, "Stride Evicting a useful stride, send it to BOP with offset %i\n",
                    entry->stride / 64);
            largeBOP->tryAddOffset(entry->stride / 64);
        }
        entry->conf.reset();
        entry->last_addr = lookupAddr;
        entry->stride = 0;
        entry->depth = 1;
        entry->lateConf.reset();
        entry->pc = pfi.getPC();
        entry->histStrides.clear();
        entry->matchedSinceAlloc = false;
        DPRINTF(XSCompositePrefetcher, "Stride miss, insert with stride 0\n");
        stride.insertEntry(pfi.getPC(), pfi.isSecure(), entry);
    }
    periodStrideDepthDown();
    return should_cover;
}

void
XSCompositePrefetcher::periodStrideDepthDown()
{
    if (depthDownCounter < depthDownPeriod) {
        depthDownCounter++;
    } else {
        for (auto stride: {&strideUnique, &strideRedundant}) {
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
XSCompositePrefetcher::updatePht(XSCompositePrefetcher::ACTEntry *act_entry, Addr current_region_addr,bool re_act_mode)
{
    if (popCount(act_entry->region_bits) <= 1) {
        return;
    }
    PhtEntry *pht_entry = pht.findEntry(phtHash(act_entry->pc, act_entry->region_offset), act_entry->is_secure);
    bool is_update = pht_entry != nullptr;
    if (!pht_entry) {
        pht_entry = pht.findVictim(phtHash(act_entry->pc, act_entry->region_offset));
        DPRINTF(XSCompositePrefetcher, "Evict PHT entry for PC %lx\n", pht_entry->pc);
        for (uint8_t i = 0; i < 2 * (regionBlks - 1); i++) {
            pht_entry->hist[i].reset();
        }
        pht_entry->pc = act_entry->pc;
    }
    pht.accessEntry(pht_entry);
    Addr region_offset = act_entry->region_offset;
    // incr part
    for (int i = region_offset + 1, j = 0; j < regionBlks - 1; i++, j++) {
        uint8_t hist_idx = j + (regionBlks - 1);
        if (i < regionBlks) {
            bool accessed = (act_entry->region_bits >> i) & 1;
            if (accessed) {
                DPRINTF(XSCompositePrefetcher, "Inc conf for region offset: %d, hist_idx: %d\n", i, hist_idx);
                pht_entry->hist.at(hist_idx) += 2;
                if (re_act_mode)
                    pht_entry->hist.at(hist_idx) += 2;
            } else {
                if (!re_act_mode)
                    pht_entry->hist.at(hist_idx) -= 2;
            }
        } else {
            pht_entry->hist.at(hist_idx) -= 1;
        }
    }
    // decr part
    for (int i = int(region_offset) - 1, j = regionBlks - 2; j >= 0; i--, j--) {
        if (i >= 0) {
            bool accessed = (act_entry->region_bits >> i) & 1;
            if (accessed) {
                DPRINTF(XSCompositePrefetcher,
                        "Inc conf for region offset: %d, hist_idx: %d\n", i,
                        j);
                pht_entry->hist.at(j) += 2;
                if (re_act_mode)
                    pht_entry->hist.at(j) += 2;
            } else {
                if (!re_act_mode)
                    pht_entry->hist.at(j) -= 2;
            }
        } else {
            // leave unseen untouched
        }
    }
    DPRINTF(XSCompositePrefetcher, "Evict ACT region: %lx, offset: %lx, evicted by region %lx\n",
            act_entry->regionAddr, act_entry->region_offset, current_region_addr);
    if (!is_update) {
        DPRINTF(XSCompositePrefetcher, "Insert SMS PHT entry for PC %lx\n", act_entry->pc);
        pht.insertEntry(phtHash(act_entry->pc, act_entry->region_offset), act_entry->is_secure, pht_entry);
    } else {
        DPRINTF(XSCompositePrefetcher, "Update SMS PHT entry for PC %lx, after update:\n", act_entry->pc);
    }

    for (uint8_t i = 0; i < 2 * (regionBlks - 1); i++) {
        DPRINTFR(XSCompositePrefetcher, "%.2f ", pht_entry->hist[i].calcSaturation());
        if (i == regionBlks - 1) {
            DPRINTFR(XSCompositePrefetcher, "| ");
        }
    }
    DPRINTFR(XSCompositePrefetcher, "\n");
}
bool
XSCompositePrefetcher::phtLookup(const Base::PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late,
                         Addr look_ahead_addr)
{
    Addr pc = pfi.getPC();
    Addr vaddr = look_ahead_addr ? look_ahead_addr : pfi.getAddr();
    Addr blk_addr = blockAddress(vaddr);
    // Addr region_addr = regionAddress(vaddr);
    Addr region_offset = regionOffset(vaddr);
    bool secure = pfi.isSecure();
    PhtEntry *pht_entry = pht.findEntry(phtHash(pc, region_offset), secure);
    bool found = false;
    if (pht_entry) {
        pht.accessEntry(pht_entry);
        DPRINTF(XSCompositePrefetcher, "Pht lookup hit: pc: %x, vaddr: %x (%s), offset: %x, late: %i\n", pc, vaddr,
                look_ahead_addr ? "ahead" : "current", region_offset, late);
        int priority = 2 * (regionBlks - 1);
        // find incr pattern
        for (uint8_t i = 0; i < regionBlks - 1; i++) {
            if (pht_entry->hist[i + regionBlks - 1].calcSaturation() > 0.5) {
                Addr pf_tgt_addr = blk_addr + (i + 1) * blkSize;
                sendPFWithFilter(pf_tgt_addr, addresses, priority--, PrefetchSourceType::SPht, phtPFLevel);
                found = true;
            }
        }
        for (int i = regionBlks - 2, j = 1; i >= 0; i--, j++) {
            if (pht_entry->hist[i].calcSaturation() > 0.5) {
                Addr pf_tgt_addr = blk_addr - j * blkSize;
                sendPFWithFilter(pf_tgt_addr, addresses, priority--, PrefetchSourceType::SPht, phtPFLevel);
                found = true;
            }
        }
        DPRINTF(XSCompositePrefetcher, "pht entry pattern:\n");
        for (uint8_t i = 0; i < 2 * (regionBlks - 1); i++) {
            DPRINTFR(XSCompositePrefetcher, "%.2f ", pht_entry->hist[i].calcSaturation());
            if (i == regionBlks - 1) {
                DPRINTFR(XSCompositePrefetcher, "| ");
            }
        }
        DPRINTFR(XSCompositePrefetcher, "\n");

        // if (late) {
        //     int period = calcPeriod(pht_entry->hist, late);
        // }
    }
    return found;
}

int
XSCompositePrefetcher::calcPeriod(const std::vector<SatCounter8> &bit_vec, bool late)
{
    std::vector<int> bit_vec_full(2 * (regionBlks - 1) + 1);
    // copy bit_vec to bit_vec_full, with mid point = 1
    for (int i = 0; i < regionBlks - 1; i++) {
        bit_vec_full.at(i) = bit_vec.at(i).calcSaturation() > 0.5;
    }
    bit_vec_full[regionBlks - 1] = 1;
    for (int i = regionBlks, j = regionBlks - 1; i < 2 * (regionBlks - 1);
         i++, j++) {
        bit_vec_full.at(i) = bit_vec.at(j).calcSaturation() > 0.5;
    }

    DPRINTF(XSCompositePrefetcher, "bit_vec_full: ");
    for (int i = 0; i < 2 * (regionBlks - 1) + 1; i++) {
        DPRINTFR(XSCompositePrefetcher, "%i ", bit_vec_full[i]);
    }
    DPRINTFR(XSCompositePrefetcher, "\n");

    int max_dot_prod = 0;
    int max_shamt = -1;
    for (int shamt = 2; shamt < 2 * (regionBlks - 1) + 1; shamt++) {
        int dot_prod = 0;
        for (int i = 0; i < 2 * (regionBlks - 1) + 1; i++) {
            if (i + shamt < 2 * (regionBlks - 1) + 1) {
                dot_prod += bit_vec_full[i] * bit_vec_full[i + shamt];
            }
        }
        if (dot_prod >= max_dot_prod) {
            max_dot_prod = dot_prod;
            max_shamt = shamt;
        }
    }
    DPRINTF(XSCompositePrefetcher, "max_dot_prod: %i, max_shamt: %i\n", max_dot_prod,
            max_shamt);
    if (max_dot_prod > 0 && max_shamt > 3) {
        largeBOP->tryAddOffset(max_shamt, late);
    }
    return max_shamt;
}

bool
XSCompositePrefetcher::sendPFWithFilter(Addr addr, std::vector<AddrPriority> &addresses, int prio,
                                        PrefetchSourceType src, int ahead_level)
{
    if (ahead_level < 2 && pfPageLRUFilter.contains(regionAddress(addr))) {
        DPRINTF(XSCompositePrefetcher, "Skip recently L1 prefetched page: %lx\n", regionAddress(addr));
        return false;

    } else if (ahead_level == 2 && pfPageLRUFilterL2.contains(regionAddress(addr))) {
        DPRINTF(XSCompositePrefetcher, "Skip recently L2 prefetched page: %lx\n", regionAddress(addr));
        return false;

    } else if (ahead_level == 3 && pfPageLRUFilterL3.contains(regionAddress(addr))) {
        DPRINTF(XSCompositePrefetcher, "Skip recently L3 prefetched page: %lx\n", regionAddress(addr));
        return false;

    } else if (pfBlockLRUFilter.contains(addr)) {
        DPRINTF(XSCompositePrefetcher, "Skip recently prefetched: %lx\n", addr);
        return false;

    } else {
        if (!(src == PrefetchSourceType::SStream || src == PrefetchSourceType::StoreStream)) {
            pfBlockLRUFilter.insert(addr, 0);
        }
        addresses.push_back(AddrPriority(addr, prio, src));
        if (ahead_level > 1) {
            assert(ahead_level == 2 || ahead_level == 3);
            addresses.back().pfahead_host = ahead_level;
            addresses.back().pfahead = true;
        } else {
            addresses.back().pfahead = false;
        }
        DPRINTF(XSCompositePrefetcher, "Send pf: %lx, target level: %i\n", addr, ahead_level);
        return true;
    }
}

void
XSCompositePrefetcher::notifyFill(const PacketPtr &pkt)
{
    pfBlockLRUFilter.insert(pkt->req->getVaddr(), 0);
}

XSCompositePrefetcher::XSCompositeStats::XSCompositeStats(statistics::Group *parent)
    :statistics::Group(parent),
    ADD_STAT(allCntNum,statistics::units::Count::get(),"victim act access num"),
    ADD_STAT(actMNum,statistics::units::Count::get(),"victim act match num")
    {
    }


}  // prefetch
}  // gem5
