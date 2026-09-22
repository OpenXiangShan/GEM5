#include "mem/cache/prefetch/sms.hh"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <iterator>
#include <utility>

#include "base/stats/group.hh"
#include "debug/BOPOffsets.hh"
#include "debug/XSCompositePrefetcher.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"

namespace gem5
{
namespace prefetch
{

// PrefetchFilter implementation moved to prefetch_filter.{hh,cc}


XSCompositePrefetcher::XSCompositePrefetcher(const XSCompositePrefetcherParams &p)
    : Queued(p),
      regionSize(p.region_size),
      regionBlks(p.region_size / p.block_size),
      enableTrainFilter(p.enable_train_filter),
      act(p.act_entries, p.act_entries, p.act_indexing_policy,
          p.act_replacement_policy, ACTEntry(regionBlks, SatCounter8(2, 1))),
      re_act(p.re_act_entries, p.re_act_entries, p.re_act_indexing_policy,
          p.re_act_replacement_policy,ReACTEntry()),
      streamPFAhead(p.stream_pf_ahead),
      pht(p.pht_assoc, p.pht_entries, p.pht_indexing_policy,
          p.pht_replacement_policy,
          PhtEntry(2 * (regionBlks - 1), SatCounter8(3, 2))),
      phtPFAhead(p.pht_pf_ahead),
      phtPFLevel(std::min(p.pht_pf_level, (int) 3)),
      enablePhtConfDest(p.enable_pht_conf_dest),
      phtHighConfThreshold(p.pht_high_conf_threshold),
      phtMedConfThreshold(p.pht_med_conf_threshold),
      phtLowConfThreshold(p.pht_low_conf_threshold),
      stats(this),
      pfBlockLRUFilter(pfFilterSize),
      sms_pfFilter(p.sms_filter_indexing_policy, p.sms_filter_replacement_policy, p.sms_filter_entries,
             p.region_size, p.block_size, this, p.vaddr_hash_width,
             PrefetchSourceType::SPht, "sms_pfFilter",
             p.enable_sms_first_touch_order),
      stridestream_pfFilter_l1(p.stridestream_L1_filter_indexing_policy, p.stridestream_L1_filter_replacement_policy,
                     p.stridestream_L1_filter_entries, p.region_size, p.block_size, this,
                     p.vaddr_hash_width, PrefetchSourceType::SStream,
                     "stridestream_pfFilter_l1"),
      stridestream_pfFilter_l2l3(p.stridestream_L2L3_filter_indexing_policy, p.stridestream_L2L3_filter_replacement_policy,
                       p.stridestream_L2L3_filter_entries, p.region_size, p.block_size, this,
                       p.vaddr_hash_width, PrefetchSourceType::SStream,
                       "stridestream_pfFilter_l2l3"),
      pfPageLRUFilter(pfPageFilterSize),
      pfPageLRUFilterL2(pfPageFilterSize),
      pfPageLRUFilterL3(pfPageFilterSize),
      largeBOP(dynamic_cast<BOP *>(p.bop_large)),
      smallBOP(dynamic_cast<BOP *>(p.bop_small)),
      learnedBOP(dynamic_cast<BOP *>(p.bop_learned)),
      spp(dynamic_cast<SignaturePath *>(p.spp)),
      ipcp(dynamic_cast<IPCP *>(p.ipcp)),
      cmc(p.cmc),
      berti(p.berti),
      Sstride(p.sstride),
      Opt(p.opt),
      Xsstream(p.xsstream),
      lldp(dynamic_cast<LLDPrefetcher *>(p.lldp)),
      enableActivepage(p.enable_activepage),
      enablePht(p.enable_pht),
      enableCPLX(p.enable_cplx),
      enableSPP(p.enable_spp),
      enableTemporal(p.enable_temporal),
      enableSstride(p.enable_sstride),
      enableBerti(p.enable_berti),
      enableBOP(p.enable_bop),
      enableOpt(p.enable_opt),
      enableXsstream(p.enable_xsstream),
      enableLLDP(p.enable_lldp),
      phtEarlyUpdate(p.pht_early_update),
      neighborPhtUpdate(p.neighbor_pht_update),
      enableSmsFirstTouchOrder(p.enable_sms_first_touch_order),
      phtSentPrefetch(),
      phtReqSendEvent([this]{ phtSendEventWrapper(); },
          name()),
      BOPPFlevel(p.bop_pf_level)
{
    assert(largeBOP);
    assert(smallBOP);
    assert(learnedBOP);
    assert(lldp);
    assert(isPowerOf2(regionSize));
    assert(regionBlks <= 64);
    assert(phtHighConfThreshold >= phtMedConfThreshold);
    assert(phtMedConfThreshold >= phtLowConfThreshold);

    setSharedFilterContextQualified(true);
    largeBOP->setSharedFilterContextQualified(true);
    smallBOP->setSharedFilterContextQualified(true);
    learnedBOP->setSharedFilterContextQualified(true);
    largeBOP->filter = &this->pfBlockLRUFilter;
    smallBOP->filter = &this->pfBlockLRUFilter;
    learnedBOP->filter = &this->pfBlockLRUFilter;
    if (berti) {
        berti->setSharedFilterContextQualified(true);
        berti->filter = &this->pfBlockLRUFilter;
    }
    if (Sstride) {
        Sstride->setSharedFilterContextQualified(true);
        Sstride->filter = &this->pfBlockLRUFilter;
        Sstride->filterL2 = &this->pfPageLRUFilterL2;
    }

    if (cmc) {
        cmc->setSharedFilterContextQualified(true);
        cmc->filter = &this->pfBlockLRUFilter;
    }

    if (ipcp) {
        ipcp->setSharedFilterContextQualified(true);
        ipcp->rrf = &this->pfBlockLRUFilter;
    }
    if (Opt) {
        Opt->setSharedFilterContextQualified(true);
        Opt->filter = &this->pfBlockLRUFilter;
    }
    if (spp) {
        spp->setSharedFilterContextQualified(true);
    }
    if (Xsstream) {
        Xsstream->setSharedFilterContextQualified(true);
        Xsstream->filter = &this->pfBlockLRUFilter;
    }
    lldp->setSharedFilterContextQualified(true);

    DPRINTF(XSCompositePrefetcher, "SMS: region_size: %d regionBlks: %d\n",
            regionSize, regionBlks);
    if (Xsstream)
    {
        Xsstream->stridestream_pfFilter_l1 = &this->stridestream_pfFilter_l1;
        Xsstream->stridestream_pfFilter_l2l3 = &this->stridestream_pfFilter_l2l3;
    }
    if (Sstride)
    {
        Sstride->stridestream_pfFilter_l1 = &this->stridestream_pfFilter_l1;
        Sstride->stridestream_pfFilter_l2l3 = &this->stridestream_pfFilter_l2l3;
    }
    assert(phtSentPrefetch.size() == 0);
    for(unsigned i = 0; i < 3; i++)
        phtSentPrefetch.push_back(phtsentInfo());
    
}

void
XSCompositePrefetcher::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late,
                                 PrefetchSourceType pf_source, bool miss_repeat)
{
    bool can_prefetch = !pfi.isWrite() && pfi.hasPC();
    if (!can_prefetch) {
        return;
    }
    stats.totalTrainCount++;

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

    Addr region_offset = regionOffset(vaddr);
    bool is_active_page = false;
    bool enter_new_region = false;
    bool is_first_shot = false;
    ACTEntry *act_match_entry = nullptr;
    Addr pf_tgt_addr = 0;
    bool decr = false;
    bool is_first_64 = false;
    if (pfi.isCacheMiss() || pfi.isPfFirstHit()) {
        assert(!(enableActivepage && enableXsstream));
        if (enableXsstream) {
            Xsstream->calculatePrefetch(pfi, addresses, streamlatenum);
            stats.streamTrainCount++;
        }
        act_match_entry = actLookup(pfi, is_active_page, enter_new_region, is_first_shot);
        if (enableOpt){
            assert(Opt);
            Opt->calculatePrefetch(pfi, addresses, is_first_64);
        }
        int origin_depth = 0;
        if (act_match_entry) {
            decr = act_match_entry->inBackwardMode;
            DPRINTF(XSCompositePrefetcher, "ACT hit or match: pc:%x addr: %x offset: %d active: %d decr: %d\n", pc,
                    vaddr, region_offset, is_active_page, decr);
            if (is_active_page && enableActivepage) {
                origin_depth = act_match_entry->depth;
                int depth = 16;
                // active page
                pf_tgt_addr = decr ? block_addr - depth * blkSize : block_addr + depth * blkSize;  // depth here?
                sendStreamPF(pfi, pf_tgt_addr, addresses, pfPageLRUFilter, decr, 1);
            }
        }
    }

    if (act_match_entry && is_active_page && pf_tgt_addr && enter_new_region && enableActivepage) {
        if (streamPFAhead) {
            Addr pf_tgt_addr_l2 = decr ? pf_tgt_addr - 48 * blkSize : pf_tgt_addr + 48 * blkSize;  // depth here?
            sendStreamPF(pfi, pf_tgt_addr_l2, addresses, pfPageLRUFilterL2, decr, 2);

            Addr pf_tgt_addr_l3 = decr ? pf_tgt_addr - 256 * blkSize : pf_tgt_addr + 256 * blkSize;  // depth here?
            sendStreamPF(pfi, pf_tgt_addr_l3, addresses, pfPageLRUFilterL3, decr, 3);
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


    Addr stride_pf_addr = 0;
    if (pf_source != PrefetchSourceType::SStream && !is_active_page) {
        bool use_bop = enableBOP && ((pfi.isPfFirstHit() && (pf_source == PrefetchSourceType::HWP_BOP ||
                                                             pf_source == PrefetchSourceType::IPCP_CPLX ||
                                                             pf_source == PrefetchSourceType::Berti)) ||
                                     pfi.isCacheMiss());
        use_bop &= !miss_repeat && is_first_shot; // miss repeat should not be handled by stride
        if (use_bop) {
            DPRINTF(XSCompositePrefetcher, "Do BOP traing/prefetching...\n");
            largeBOP->calculatePrefetch(pfi, addresses, late && pf_source == PrefetchSourceType::HWP_BOP);

            smallBOP->calculatePrefetch(pfi, addresses, late && pf_source == PrefetchSourceType::HWP_BOP);

            stats.bopTrainCount++;
        }

        bool covered_by_stride = false;
        //NOTICE:don't open berti & stride at the same time
        assert(!(enableBerti && enableSstride));
        bool use_berti = !pfi.isStore() && (pfi.isCacheMiss() || pfi.isPfFirstHit()) && enableBerti;
        if (use_berti) {
            DPRINTF(XSCompositePrefetcher, "Do Berti traing/prefetching...\n");
            berti->calculatePrefetch(pfi, addresses, late, pf_source, miss_repeat, stride_pf_addr);
            int t;
            if ((t = berti->getEvictBestDelta()) != 0) {
                DPRINTF(BOPOffsets, "PC %lx add evict delta %u\n", pfi.getPC(), t);
                if (labs(t) > 64) {
                    largeBOP->tryAddOffset(t);
                } else if (labs(t) > 8) {
                    smallBOP->tryAddOffset(t);
                }
            }
        }

        bool use_pht = pfi.isCacheMiss() ||
                       (pfi.isPfFirstHit() &&
                        (pf_source == PrefetchSourceType::SStride || pf_source == PrefetchSourceType::HWP_BOP ||
                         pf_source == PrefetchSourceType::SPht || pf_source == PrefetchSourceType::IPCP_CPLX ||
                         pf_source == PrefetchSourceType::SPP || pf_source == PrefetchSourceType::Berti));

        use_pht &= (!pfi.isStore()) && enablePht;

        bool trigger_pht = false;
        stride_pf_addr = phtPFAhead ? stride_pf_addr : 0;  // trigger addr sent to pht
        if (use_pht) {
            DPRINTF(XSCompositePrefetcher, "Do PHT lookup...\n");
            trigger_pht = phtLookup(pfi, addresses,
                                    late && pf_source == PrefetchSourceType::SPht,
                                    stride_pf_addr, enter_new_region);
        }
        bool use_opt = enableOpt && !pfi.isStore() && is_first_64;
        if (use_opt){
            Opt->optLookup(pfi, addresses);
        }

        bool use_cplx = enableCPLX && !pfi.isStore();
        if (use_cplx) {
            Addr cplx_best_offset = 0;
            bool send_cplx_pf = ipcp->doPrefetch(pfi, addresses, cplx_best_offset);

            if (send_cplx_pf && cplx_best_offset != 0) {
                learnedBOP->tryAddOffset(cplx_best_offset, late);
            }
        }

        bool use_spp = enableSPP && !pfi.isStore();
        if (use_spp) {
            int32_t spp_best_offset = 0;
            bool coverd_by_spp = spp->calculatePrefetch(pfi, addresses, pfBlockLRUFilter, spp_best_offset);
            if (coverd_by_spp && spp_best_offset != 0) {
                // TODO: Let BOP to adjust depth by itself
                learnedBOP->tryAddOffset(spp_best_offset, late);
            }
        }

        bool use_cmc = enableTemporal;
        if (use_cmc) {
            if (is_first_shot && (pfi.isCacheMiss() || pfi.isPfFirstHit() || pf_source == PrefetchSourceType::CMC)) {
                cmc->doPrefetch(pfi, addresses, late, pf_source, false);
            }
        }
    }

    bool use_stride = !pfi.isStore() && (pfi.isCacheMiss() || pfi.isPfFirstHit()) && enableSstride;
    if (use_stride){
        DPRINTF(XSCompositePrefetcher, "Do Sstride traing/prefetching...\n");
        int64_t learned_bop_offset = 0;
        stats.strideTrainCount++;
        Sstride->calculatePrefetch(pfi, addresses, late, pf_source, miss_repeat, enter_new_region, is_first_shot,
                                   stride_pf_addr, learned_bop_offset);
        if (learned_bop_offset != 0)
            learnedBOP->tryAddOffset(learned_bop_offset);
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
    ContextID context_id = pfi.hasContextId() ?
        pfi.contextId() : InvalidContextID;
    ReACTEntry *re_act_entry = nullptr;
    bool re_act_mode = false;


    ACTEntry *entry =
        act.findEntry(contextKey(region_addr, context_id), secure);
    if (entry) {
        // act hit
        act.accessEntry(entry);
        in_active_page = entry->inActivePage(regionBlks);
        uint64_t region_bit_accessed = 1UL << region_offset;
        const bool first_touch = !(entry->regionBits & region_bit_accessed);
        if (enablePhtConfDest) {
            if (first_touch) {
                entry->touchOrder.at(region_offset) = entry->accessCount;
                entry->accessCount += 1;
                is_first_shot = true;
            }
            entry->regionBits |= region_bit_accessed;
            if (phtEarlyUpdate && first_touch)
                updatePht(entry, region_start, false, true, region_offset);
        } else {
            if (phtEarlyUpdate)
                updatePht(entry, region_start, false, true, region_offset);
            if (first_touch) {
                entry->touchOrder.at(region_offset) = entry->accessCount;
                entry->accessCount += 1;
                is_first_shot = true;
            }
            entry->regionBits |= region_bit_accessed;
        }
        if (phtEarlyUpdate)
            trainPhtOrder(entry);
        // print bits
        DPRINTF(XSCompositePrefetcher, "Access region %lx, after access bit %lu, new act entry bits:\n", region_start,
                region_offset);
        for (uint8_t i = 0; i < regionBlks; i++) {
            DPRINTFR(XSCompositePrefetcher, "%lu ", (entry->regionBits >> i) & 1);
        }
        DPRINTFR(XSCompositePrefetcher, "\n");
        return entry;
    }

    alloc_new_region = true;
    is_first_shot = true;

    bool found = false;
    bool forward = true;

    ACTEntry *old_neighbor_entry =
        act.findEntry(contextKey(region_addr - 1, context_id), secure);
    if (old_neighbor_entry) {
        // act miss, but cur_region - 1 = entry_region found, => cur_region = entry_region + 1
        in_active_page = old_neighbor_entry->inActivePage(regionBlks);
        found = true;
        forward = true;
    } else if ((old_neighbor_entry =
                    act.findEntry(contextKey(region_addr + 1, context_id),
                                  secure))) {
        // act miss, but cur_region + 1 = entry_region found, => cur_region = entry_region - 1
        in_active_page = old_neighbor_entry->inActivePage(regionBlks);
        found = true;
        forward = false;
    }

    bool victim_secure = false;
    entry = act.findVictim(
        contextKey(region_addr, context_id), &victim_secure);

    re_act_entry = re_act.findEntry(
        contextKey(entry->regionAddr, entry->contextId), victim_secure);
    if (re_act_entry) {
        re_act_mode = true;
        stats.actMNum++;
        entry->pc = re_act_entry->pc;
    } else {
        stats.allCntNum++;
        re_act_entry = re_act.findVictim(
            contextKey(entry->regionAddr, entry->contextId));
        re_act_entry->pc = entry->pc;
        re_act_entry->regionAddr = entry->regionAddr;
        re_act_entry->contextId = entry->contextId;
        re_act_entry->_setSecure(victim_secure);
        re_act.insertEntry(
            contextKey(re_act_entry->regionAddr, re_act_entry->contextId),
            re_act_entry->isSecure(), re_act_entry);
    }

    updatePht(entry, region_start, re_act_mode, false, 0);  // update pht with evicted entry
    trainPhtOrder(entry);
    entry->pc = pc;
    entry->contextId = context_id;
    entry->_setSecure(secure);
    entry->inBackwardMode = !forward;
    entry->regionAddr = region_start;
    entry->regionOffset = region_offset;
    entry->regionBits = 1UL << region_offset;
    //entry->repeat_region_bits = 0;
    entry->accessCount = 1;
    entry->hasIncreasedPht = false;
    entry->phtUpdatedBits = 0;
    std::fill(entry->touchOrder.begin(), entry->touchOrder.end(), UINT8_MAX);
    entry->touchOrder.at(region_offset) = 0;
    entry->orderTrainedBits = 0;
    entry->orderPhtEpoch = 0;
    act.insertEntry(contextKey(region_addr, context_id), secure, entry);

    // print bits
    DPRINTF(XSCompositePrefetcher, "Access new region %lx, after access bit %lu, new act entry bits:\n", region_start,
            region_offset);
    for (uint8_t i = 0; i < regionBlks; i++) {
        DPRINTFR(XSCompositePrefetcher, "%lu ", (entry->regionBits >> i) & 1);
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
XSCompositePrefetcher::resetPhtEntry(PhtEntry *pht_entry, const ACTEntry *act_entry)
{
    for (uint8_t i = 0; i < 2 * (regionBlks - 1); i++) {
        pht_entry->hist[i].reset();
    }
    resetPhtOrder(pht_entry);
    pht_entry->pc = act_entry->pc;
    pht_entry->contextId = act_entry->contextId;
    pht_entry->decr_mode = act_entry->inBackwardMode;
}

bool
XSCompositePrefetcher::regionOffsetToHistIdx(unsigned trigger_offset, unsigned offset,
                                             uint8_t &hist_idx) const
{
    if (offset == trigger_offset || offset >= regionBlks) {
        return false;
    }
    if (offset > trigger_offset) {
        hist_idx = offset - trigger_offset + regionBlks - 2;
    } else {
        hist_idx = regionBlks - 1 - (trigger_offset - offset);
    }
    return hist_idx < 2 * (regionBlks - 1);
}

void
XSCompositePrefetcher::updateInRegionPhtOffsets(ACTEntry *act_entry, PhtEntry *pht_entry,
                                                bool is_eviction, bool re_act_mode)
{
    const unsigned trigger_offset = act_entry->regionOffset;
    for (unsigned offset = 0; offset < regionBlks; ++offset) {
        uint8_t hist_idx = 0;
        if (!regionOffsetToHistIdx(trigger_offset, offset, hist_idx)) {
            continue;
        }
        const uint64_t bit = uint64_t(1) << offset;
        const bool accessed = act_entry->regionBits & bit;
        const bool updated = act_entry->phtUpdatedBits & bit;
        if (accessed) {
            if (!updated) {
                pht_entry->hist.at(hist_idx) += 2;
                act_entry->phtUpdatedBits |= bit;
            }
            if (is_eviction && re_act_mode) {
                pht_entry->hist.at(hist_idx) += 2;
            }
        } else if (is_eviction && !re_act_mode) {
            pht_entry->hist.at(hist_idx) -= 2;
        }
    }
}

void
XSCompositePrefetcher::updateNeighborPhtOnEvict(ACTEntry *act_entry, PhtEntry *pht_entry,
                                                bool re_act_mode, bool already_early_updated)
{
    const Addr region_offset = act_entry->regionOffset;
    const Addr region_addr_find = act_entry->regionAddr / regionSize;
    ACTEntry *act_entry_f = nullptr;
    ACTEntry *act_entry_b = nullptr;
    if (neighborPhtUpdate) {
        act_entry_f = act.findEntry(
            contextKey(region_addr_find + 1, act_entry->contextId),
            act_entry->isSecure());
        act_entry_b = act.findEntry(
            contextKey(region_addr_find - 1, act_entry->contextId),
            act_entry->isSecure());
    }

    for (int i = region_offset + 1, j = 0; j < int(regionBlks) - 1; i++, j++) {
        uint8_t hist_idx = j + (regionBlks - 1);
        if (i < int(regionBlks)) {
            continue;
        }
        if (act_entry_f) {
            bool accessed = (act_entry_f->regionBits >> (i - regionBlks)) & 1;
            updatePhtBits(accessed, false, re_act_mode, hist_idx, act_entry, pht_entry,
                          already_early_updated);
        } else {
            pht_entry->hist.at(hist_idx) -= 1;
        }
    }

    int i_b = 0;
    for (int i = int(region_offset) - 1, j = int(regionBlks) - 2; j >= 0; i--, j--) {
        if (i >= 0) {
            continue;
        }
        if (act_entry_b) {
            bool accessed = (act_entry_b->regionBits >> (regionBlks - 1 - i_b)) & 1;
            i_b++;
            updatePhtBits(accessed, false, re_act_mode, j, act_entry, pht_entry,
                          already_early_updated);
        }
    }
}

void
XSCompositePrefetcher::updatePht(XSCompositePrefetcher::ACTEntry *act_entry, Addr current_region_addr,
                                 bool re_act_mode, bool early_update, Addr region_offset_now)
{
    if (!enablePhtConfDest) {
        updatePhtLegacy(act_entry, current_region_addr, re_act_mode,
                        early_update, region_offset_now);
        return;
    }
    if (popCount(act_entry->regionBits) <= 1) {
        return;
    }
    (void)region_offset_now;
    Addr pht_key = contextKey(
        phtHash(act_entry->pc, act_entry->regionOffset),
        act_entry->contextId);
    PhtEntry *pht_entry =
        pht.findEntry(pht_key, act_entry->isSecure());
    bool is_update = pht_entry != nullptr;

    if (early_update) {
        const int access_cnt_thres = 5;
        if (!pht_entry) {
            if (act_entry->accessCount <= access_cnt_thres) {
                return;
            }
            pht_entry = pht.findVictim(pht_key);
            resetPhtEntry(pht_entry, act_entry);
        }
        pht.accessEntry(pht_entry);
        updateInRegionPhtOffsets(act_entry, pht_entry, false, false);
        if (!is_update) {
            DPRINTF(XSCompositePrefetcher, "Insert SMS PHT entry for PC %lx\n", act_entry->pc);
            pht.insertEntry(pht_key, act_entry->isSecure(), pht_entry);
        }
        return;
    }

    if (!pht_entry) {
        pht_entry = pht.findVictim(pht_key);
        DPRINTF(XSCompositePrefetcher, "Evict PHT entry for PC %lx\n", pht_entry->pc);
        resetPhtEntry(pht_entry, act_entry);
    }

    pht.accessEntry(pht_entry);
    const bool already_early_updated = act_entry->phtUpdatedBits != 0;
    updateInRegionPhtOffsets(act_entry, pht_entry, true, re_act_mode);
    updateNeighborPhtOnEvict(act_entry, pht_entry, re_act_mode,
                             already_early_updated);

    DPRINTF(XSCompositePrefetcher, "Evict ACT region: %lx, offset: %lx, evicted by region %lx\n",
            act_entry->regionAddr, act_entry->regionOffset, current_region_addr);
    if (!is_update) {
        DPRINTF(XSCompositePrefetcher, "Insert SMS PHT entry for PC %lx\n", act_entry->pc);
        pht.insertEntry(pht_key, act_entry->isSecure(), pht_entry);
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

void
XSCompositePrefetcher::updatePhtLegacy(ACTEntry *act_entry, Addr current_region_addr,
                                       bool re_act_mode, bool early_update,
                                       Addr region_offset_now)
{
    if (popCount(act_entry->regionBits) <= 1) {
        return;
    }
    Addr pht_key = contextKey(
        phtHash(act_entry->pc, act_entry->regionOffset),
        act_entry->contextId);
    PhtEntry *pht_entry =
        pht.findEntry(pht_key, act_entry->isSecure());
    bool is_update = pht_entry != nullptr;
    if (pht_entry && early_update) {
        if (region_offset_now > act_entry->regionOffset) {
            pht_entry->hist[region_offset_now - act_entry->regionOffset +
                            regionBlks - 2] += 2;
            act_entry->hasIncreasedPht = true;
        }
        if (region_offset_now < act_entry->regionOffset) {
            pht_entry->hist[regionBlks - 1 -
                            (act_entry->regionOffset - region_offset_now)] += 2;
            act_entry->hasIncreasedPht = true;
        }
        return;
    }
    if (early_update) {
        const int access_cnt_thres = 5;
        if (act_entry->accessCount > access_cnt_thres && (!pht_entry)) {
            pht_entry = pht.findVictim(pht_key);
            resetPhtEntry(pht_entry, act_entry);
            act_entry->hasIncreasedPht = true;
        } else {
            return;
        }
    }

    if (!pht_entry) {
        pht_entry = pht.findVictim(pht_key);
        DPRINTF(XSCompositePrefetcher, "Evict PHT entry for PC %lx\n", pht_entry->pc);
        resetPhtEntry(pht_entry, act_entry);
    }

    pht.accessEntry(pht_entry);
    Addr region_offset = act_entry->regionOffset;
    Addr region_addr_find = act_entry->regionAddr / regionSize;
    ACTEntry *act_entry_f = nullptr;
    ACTEntry *act_entry_b = nullptr;
    if (neighborPhtUpdate){
        act_entry_f = act.findEntry(
            contextKey(region_addr_find + 1, act_entry->contextId),
            act_entry->isSecure());
        act_entry_b = act.findEntry(
            contextKey(region_addr_find - 1, act_entry->contextId),
            act_entry->isSecure());
    }
    const bool already_early = act_entry->hasIncreasedPht;
    if (act_entry_f) {
        for (int i = region_offset + 1, j = 0; j < regionBlks - 1; i++, j++) {
            uint8_t hist_idx = j + (regionBlks - 1);
            bool accessed;
            if (i > 15)
                accessed = (act_entry_f->regionBits >> (i - 16)) & 1;
            else
                accessed = (act_entry->regionBits >> i) & 1;
            updatePhtBits(accessed, early_update, re_act_mode, hist_idx,
                          act_entry, pht_entry, already_early);
        }
    } else {
        for (int i = region_offset + 1, j = 0; j < regionBlks - 1; i++, j++) {
            uint8_t hist_idx = j + (regionBlks - 1);
            if (i < regionBlks) {
                bool accessed = (act_entry->regionBits >> i) & 1;
                updatePhtBits(accessed, early_update, re_act_mode, hist_idx,
                              act_entry, pht_entry, already_early);
            } else {
                if (!early_update)
                    pht_entry->hist.at(hist_idx) -= 1;
            }
        }
    }

    int i_b = 0;
    if (act_entry_b) {
        for (int i = int(region_offset) - 1, j = regionBlks - 2; j >= 0;
             i--, j--) {
            if (i >= 0) {
                bool accessed = (act_entry->regionBits >> i) & 1;
                updatePhtBits(accessed, early_update, re_act_mode, j,
                              act_entry, pht_entry, already_early);
            } else {
                bool accessed = (act_entry_b->regionBits >> (15 - i_b)) & 1;
                i_b++;
                updatePhtBits(accessed, early_update, re_act_mode, j,
                              act_entry, pht_entry, already_early);
            }
        }
    } else {
        for (int i = int(region_offset) - 1, j = regionBlks - 2; j >= 0;
             i--, j--) {
            if (i >= 0) {
                bool accessed = (act_entry->regionBits >> i) & 1;
                updatePhtBits(accessed, early_update, re_act_mode, j,
                              act_entry, pht_entry, already_early);
            }
        }
    }
    DPRINTF(XSCompositePrefetcher, "Evict ACT region: %lx, offset: %lx, evicted by region %lx\n",
            act_entry->regionAddr, act_entry->regionOffset, current_region_addr);
    if (!is_update) {
        DPRINTF(XSCompositePrefetcher, "Insert SMS PHT entry for PC %lx\n", act_entry->pc);
        pht.insertEntry(pht_key, act_entry->isSecure(), pht_entry);
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

void
XSCompositePrefetcher::resetPhtOrder(PhtEntry *pht_entry)
{
    if (!enableSmsFirstTouchOrder) {
        return;
    }

    std::fill(pht_entry->orderScore.begin(), pht_entry->orderScore.end(),
              sms::InvalidOrder);
    pht_entry->orderValid = 0;
    pht_entry->orderEpoch = ++nextPhtOrderEpoch;
}

void
XSCompositePrefetcher::trainPhtOrderDelta(
    ACTEntry *act_entry, PhtEntry *pht_entry, ACTEntry *source_entry,
    unsigned source_offset, unsigned hist_idx)
{
    if (!source_entry || source_offset >= source_entry->touchOrder.size() ||
        hist_idx >= pht_entry->orderScore.size() || hist_idx >= 64 ||
        (act_entry->orderTrainedBits & (uint64_t(1) << hist_idx))) {
        return;
    }

    const uint64_t source_bit = uint64_t(1) << source_offset;
    const uint8_t rank = source_entry->touchOrder[source_offset];
    if (!(source_entry->regionBits & source_bit) || rank == UINT8_MAX) {
        return;
    }

    const uint64_t order_bit = uint64_t(1) << hist_idx;
    const bool valid = pht_entry->orderValid & order_bit;
    pht_entry->orderScore[hist_idx] = sms::updateOrderScore(
        pht_entry->orderScore[hist_idx], valid, rank);
    pht_entry->orderValid |= order_bit;
    act_entry->orderTrainedBits |= order_bit;
    stats.smsOrderUpdates++;
}

void
XSCompositePrefetcher::trainPhtOrder(ACTEntry *act_entry)
{
    if (!enableSmsFirstTouchOrder || popCount(act_entry->regionBits) <= 1) {
        return;
    }

    const Addr pht_key = contextKey(
        phtHash(act_entry->pc, act_entry->regionOffset),
        act_entry->contextId);
    PhtEntry *pht_entry =
        pht.findEntry(pht_key, act_entry->isSecure());
    if (!pht_entry) {
        return;
    }

    if (act_entry->orderPhtEpoch != pht_entry->orderEpoch) {
        act_entry->orderPhtEpoch = pht_entry->orderEpoch;
        act_entry->orderTrainedBits = 0;
    }

    ACTEntry *forward_entry = nullptr;
    ACTEntry *backward_entry = nullptr;
    if (neighborPhtUpdate) {
        const Addr region = act_entry->regionAddr / regionSize;
        forward_entry = act.findEntry(
            contextKey(region + 1, act_entry->contextId),
            act_entry->isSecure());
        backward_entry = act.findEntry(
            contextKey(region - 1, act_entry->contextId),
            act_entry->isSecure());
    }

    const unsigned trigger_offset = act_entry->regionOffset;
    for (unsigned delta = 1; delta < regionBlks; ++delta) {
        const unsigned hist_idx = regionBlks - 2 + delta;
        const unsigned target = trigger_offset + delta;
        if (target < regionBlks) {
            trainPhtOrderDelta(act_entry, pht_entry, act_entry, target,
                               hist_idx);
        } else if (forward_entry) {
            trainPhtOrderDelta(act_entry, pht_entry, forward_entry,
                               target - regionBlks, hist_idx);
        }
    }

    for (unsigned delta = 1; delta < regionBlks; ++delta) {
        const unsigned hist_idx = regionBlks - 1 - delta;
        const int target = int(trigger_offset) - int(delta);
        if (target >= 0) {
            trainPhtOrderDelta(act_entry, pht_entry, act_entry, target,
                               hist_idx);
        } else if (backward_entry) {
            trainPhtOrderDelta(act_entry, pht_entry, backward_entry,
                               target + regionBlks, hist_idx);
        }
    }
}

bool
XSCompositePrefetcher::phtLookup(const Base::PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late,
                         Addr look_ahead_addr, bool is_trigger)
{
    Addr pc = pfi.getPC();
    Addr vaddr = look_ahead_addr ? look_ahead_addr : pfi.getAddr();
    Addr blk_addr = blockAddress(vaddr);
    Addr region_addr = regionAddress(vaddr);
    Addr region_offset = regionOffset(vaddr);
    uint64_t cur_l1 = 0, cur_l2 = 0, cur_l3 = 0;
    uint64_t inc_l1 = 0, inc_l2 = 0, inc_l3 = 0;
    Addr region_inc_addr = 0;
    uint64_t dec_l1 = 0, dec_l2 = 0, dec_l3 = 0;
    Addr region_dec_addr = 0;
    std::vector<sms::OrderScore> order_cur(regionBlks, sms::InvalidOrder);
    std::vector<sms::OrderScore> order_inc(regionBlks, sms::InvalidOrder);
    std::vector<sms::OrderScore> order_dec(regionBlks, sms::InvalidOrder);
    bool secure = pfi.isSecure();
    ContextID context_id = pfi.hasContextId() ?
        pfi.contextId() : InvalidContextID;
    PhtEntry *pht_entry = pht.findEntry(
        contextKey(phtHash(pc, region_offset), context_id), secure);
    bool found = false;
    if (pht_entry) {
        const auto learned_order = [this, pht_entry](unsigned hist_idx) {
            if (!enableSmsFirstTouchOrder || hist_idx >= 64 ||
                !(pht_entry->orderValid & (uint64_t(1) << hist_idx))) {
                return sms::InvalidOrder;
            }
            return pht_entry->orderScore.at(hist_idx);
        };
        const auto add_level_bit = [](uint64_t &l1, uint64_t &l2, uint64_t &l3,
                                      unsigned offset, int level) {
            const uint64_t bit = uint64_t(1) << offset;
            if (level == 1) {
                l1 |= bit;
            } else if (level == 2) {
                l2 |= bit;
            } else if (level == 3) {
                l3 |= bit;
            }
        };
        pht.accessEntry(pht_entry);
        DPRINTF(XSCompositePrefetcher,
                "Pht lookup hit: pc: %x, vaddr: %x (%s), offset: %x, late: %i, trigger: %i\n",
                pc, vaddr, look_ahead_addr ? "ahead" : "current", region_offset,
                late, is_trigger);
        int priority = 2 * (regionBlks - 1);
        auto consider = [&](unsigned hist_idx, Addr pf_tgt_addr,
                            uint64_t &l1, uint64_t &l2, uint64_t &l3,
                            std::vector<sms::OrderScore> &orders) {
            const unsigned raw = pht_entry->hist[hist_idx];
            int level = 0;
            if (enablePhtConfDest) {
                level = sms::phtDestLevel(
                    raw, is_trigger, phtHighConfThreshold,
                    phtMedConfThreshold, phtLowConfThreshold);
            } else if (pht_entry->hist[hist_idx].calcSaturation() > 0.5) {
                level = phtPFLevel;
            }
            if (level == 0) {
                return false;
            }
            const unsigned target_offset = regionOffset(pf_tgt_addr);
            add_level_bit(l1, l2, l3, target_offset, level);
            orders[target_offset] = learned_order(hist_idx);
            sendPFWithFilter(pfi, pf_tgt_addr, addresses, priority--,
                             PrefetchSourceType::SPht, level);
            if (level == 1) {
                stats.smsPhtIssuedL1++;
            } else if (level == 2) {
                stats.smsPhtIssuedL2++;
            } else {
                stats.smsPhtIssuedL3++;
            }
            return true;
        };

        for (uint8_t i = 0; i < regionBlks - 1; i++) {
            Addr pf_tgt_addr = blk_addr + (i + 1) * blkSize;
            if (regionAddress(pf_tgt_addr) == region_addr) {
                found |= consider(i + regionBlks - 1, pf_tgt_addr,
                                  cur_l1, cur_l2, cur_l3, order_cur);
            }
        }
        for (int i = regionBlks - 2, j = 1; i >= 0; i--, j++) {
            Addr pf_tgt_addr = blk_addr - j * blkSize;
            if (regionAddress(pf_tgt_addr) == region_addr) {
                found |= consider(i, pf_tgt_addr, cur_l1, cur_l2, cur_l3,
                                  order_cur);
            }
        }
        const uint64_t region_bit_cur = cur_l1 | cur_l2 | cur_l3;
        if (region_bit_cur) {
            if (phtSentPrefetch[0].valid) {
                stats.smsCurRegionoverride++;
            }
            phtSentPrefetch[0] = phtsentInfo(
                region_addr, region_bit_cur, 0, true, pht_entry->decr_mode,
                secure, 0, &pfi.trigger_info, cur_l1, cur_l2, cur_l3);
            phtSentPrefetch[0].orderScores = order_cur;
            phtSentPrefetch[0].trigger.pfSourceType = PrefetchSourceType::SPht;
        }
        for (uint8_t i = 0; i < regionBlks - 1; i++) {
            Addr pf_tgt_addr = blk_addr + (i + 1) * blkSize;
            if (regionAddress(pf_tgt_addr) != region_addr) {
                region_inc_addr = regionAddress(pf_tgt_addr);
                found |= consider(i + regionBlks - 1, pf_tgt_addr,
                                  inc_l1, inc_l2, inc_l3, order_inc);
            }
        }
        const uint64_t region_bit_inc = inc_l1 | inc_l2 | inc_l3;
        if (region_bit_inc) {
            if (phtSentPrefetch[1].valid) {
                stats.smsIncrRegionoverride++;
            }
            phtSentPrefetch[1] = phtsentInfo(
                region_inc_addr, region_bit_inc, 0, true, pht_entry->decr_mode,
                secure, 0, &pfi.trigger_info, inc_l1, inc_l2, inc_l3);
            phtSentPrefetch[1].orderScores = order_inc;
            phtSentPrefetch[1].trigger.pfSourceType = PrefetchSourceType::SPht;
        }
        for (int i = regionBlks - 2, j = 1; i >= 0; i--, j++) {
            Addr pf_tgt_addr = blk_addr - j * blkSize;
            if (regionAddress(pf_tgt_addr) != region_addr) {
                region_dec_addr = regionAddress(pf_tgt_addr);
                found |= consider(i, pf_tgt_addr, dec_l1, dec_l2, dec_l3,
                                  order_dec);
            }
        }
        const uint64_t region_bit_dec = dec_l1 | dec_l2 | dec_l3;
        if (region_bit_dec) {
            if (phtSentPrefetch[2].valid) {
                stats.smsDecrRegionoverride++;
            }
            phtSentPrefetch[2] = phtsentInfo(
                region_dec_addr, region_bit_dec, 0, true, pht_entry->decr_mode,
                secure, 0, &pfi.trigger_info, dec_l1, dec_l2, dec_l3);
            phtSentPrefetch[2].orderScores = order_dec;
            phtSentPrefetch[2].trigger.pfSourceType = PrefetchSourceType::SPht;
        }
        if (!phtReqSendEvent.scheduled()) {
            phtSendEventWrapper();
        }

        DPRINTF(XSCompositePrefetcher, "pht entry pattern:\n");
        for (uint8_t i = 0; i < 2 * (regionBlks - 1); i++) {
            DPRINTFR(XSCompositePrefetcher, "%.2f ", pht_entry->hist[i].calcSaturation());
            if (i == regionBlks - 1) {
                DPRINTFR(XSCompositePrefetcher, "| ");
            }
        }
        DPRINTFR(XSCompositePrefetcher, "\n");
    }
    return found;
}

bool
XSCompositePrefetcher::sendPFWithFilter(const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses,
                                        int prio, PrefetchSourceType src, int ahead_level)
{
    // Count generated prefetch
    prefetchStats.pfGenerated++;
    Addr page_key = sharedFilterKey(pfi, regionAddress(addr));
    Addr block_key = sharedFilterKey(pfi, addr);

    if (ahead_level < 2 && pfPageLRUFilter.contains(page_key)) {
        DPRINTF(XSCompositePrefetcher, "Skip recently L1 prefetched page: %lx\n", regionAddress(addr));
        // Count filtered prefetch
        prefetchStats.pfFiltered++;
        return false;

    } else if (ahead_level == 2 && pfPageLRUFilterL2.contains(page_key)) {
        DPRINTF(XSCompositePrefetcher, "Skip recently L2 prefetched page: %lx\n", regionAddress(addr));
        // Count filtered prefetch
        prefetchStats.pfFiltered++;
        return false;

    } else if (ahead_level == 3 && pfPageLRUFilterL3.contains(page_key)) {
        DPRINTF(XSCompositePrefetcher, "Skip recently L3 prefetched page: %lx\n", regionAddress(addr));
        // Count filtered prefetch
        prefetchStats.pfFiltered++;
        return false;

    } else if (pfBlockLRUFilter.contains(block_key)) {
        DPRINTF(XSCompositePrefetcher, "Skip recently prefetched: %lx\n", addr);
        // Count filtered prefetch
        prefetchStats.pfFiltered++;
        return false;

    } else {
        if (!(src == PrefetchSourceType::SStream || src == PrefetchSourceType::StoreStream)) {
            pfBlockLRUFilter.insert(block_key, 0);
        }
        if (archDBer) {
            archDBer->l1PFTraceWrite(curTick(), pfi.getPC(), pfi.getAddr(), addr, src);
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
XSCompositePrefetcher::sendStreamPF(const PrefetchInfo &pfi, Addr pf_tgt_addr, std::vector<AddrPriority> &addresses,
                                    boost::compute::detail::lru_cache<Addr, Addr> &Filter, bool decr, int pf_level)
{
    uint64_t region_bit = 0;
    Addr pf_tgt_region = regionAddress(pf_tgt_addr);
    Addr pf_tgt_offset = regionOffset(pf_tgt_addr);
    PrefetchSourceType stream_type = PrefetchSourceType::SStream;
    if (pfi.isStore()) {
        stream_type = PrefetchSourceType::StoreStream;
        DPRINTF(XSCompositePrefetcher, "prefetch trigger come from store unit\n");
    }
    DPRINTF(XSCompositePrefetcher, "tgt addr: %x, offset: %d ,page: %lx\n", pf_tgt_addr, pf_tgt_offset, pf_tgt_region);
    for (int i = 0; i < regionBlks; i++) {
        Addr cur = pf_tgt_region * regionSize + i * blkSize;
        region_bit |= (uint64_t(1) << regionOffset(cur));
        sendPFWithFilter(pfi, cur, addresses, regionBlks - i, stream_type, pf_level);
        DPRINTF(XSCompositePrefetcher, "pf addr: %x [%d] pf_level %d\n", cur, i, pf_level);
        fatal_if(i < 0, "i < 0\n");
    }
    //use for act to insert PFfilter
    pfi.setTriggerInfo_PFsrc(stream_type);
    if (pf_level > 1) {
        stridestream_pfFilter_l2l3.Insert(regionAddress(pf_tgt_addr),
        region_bit,0,true,decr,pfi.isSecure(),pf_level, &pfi.trigger_info);
    } else {
        stridestream_pfFilter_l1.Insert(regionAddress(pf_tgt_addr),
        region_bit,0,true,decr,pfi.isSecure(),pf_level, &pfi.trigger_info);
    }

    ContextID context_id = pfi.hasContextId() ?
        pfi.contextId() : InvalidContextID;
    Filter.insert(contextKey(pf_tgt_region, context_id), 0);
}

void
XSCompositePrefetcher::updatePhtBits(bool accessed, bool early_update, bool re_act_mode, uint8_t hist_idx,
                                     XSCompositePrefetcher::ACTEntry *act_entry,
                                     XSCompositePrefetcher::PhtEntry *pht_entry,
                                     bool already_early_updated)
{
    (void)act_entry;
    if (accessed) {
        DPRINTF(XSCompositePrefetcher, "Inc conf hist_idx: %d\n", hist_idx);
        if (early_update) {
            pht_entry->hist.at(hist_idx) += 2;
        } else {
            if (!already_early_updated)
                pht_entry->hist.at(hist_idx) += 2;
            if (re_act_mode)
                pht_entry->hist.at(hist_idx) += 2;
        }
    } else {
        if ((!re_act_mode) && (!early_update))
            pht_entry->hist.at(hist_idx) -= 2;
    }
}

void
XSCompositePrefetcher::notifyFill(const PacketPtr &pkt)
{
    if (pkt->req->hasVaddr()) {
        stats.refillNotifyCount++;
        berti->notifyFill(pkt);
        ContextID context_id = pkt->req->hasContextId() ?
            pkt->req->contextId() : InvalidContextID;
        pfBlockLRUFilter.insert(
            contextKey(pkt->req->getVaddr(), context_id), 0);
    }
}

XSCompositePrefetcher::XSCompositeStats::XSCompositeStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(allCntNum, statistics::units::Count::get(), "victim act access num"),
      ADD_STAT(actMNum, statistics::units::Count::get(), "victim act match num"),
      ADD_STAT(refillNotifyCount, statistics::units::Count::get(), "refill notify count"),
      ADD_STAT(bopTrainCount, statistics::units::Count::get(), "bop train count"),
      ADD_STAT(smsCurRegionoverride, statistics::units::Count::get(), "sms current region override prefetches"),
      ADD_STAT(smsIncrRegionoverride, statistics::units::Count::get(), "sms increased region override prefetches"),
      ADD_STAT(smsDecrRegionoverride, statistics::units::Count::get(), "sms decreased region override prefetches"),
      ADD_STAT(strideTrainCount, statistics::units::Count::get(), "stride train count"),
      ADD_STAT(streamTrainCount, statistics::units::Count::get(), "stream train count"),
      ADD_STAT(totalTrainCount, statistics::units::Count::get(), "total train count"),
      ADD_STAT(smsOrderUpdates, statistics::units::Count::get(),
               "SMS first-touch order EWMA updates"),
      ADD_STAT(smsPhtIssuedL1, statistics::units::Count::get(),
               "SMS PHT candidates classified to L1"),
      ADD_STAT(smsPhtIssuedL2, statistics::units::Count::get(),
               "SMS PHT candidates classified to L2"),
      ADD_STAT(smsPhtIssuedL3, statistics::units::Count::get(),
               "SMS PHT candidates classified to L3")
{
}

void
XSCompositePrefetcher::setParentInfo(System *sys, ProbeManager *pm, CacheAccessor* _cache, unsigned blk_size)
{
    Base::setParentInfo(sys, pm, _cache, blk_size);

    largeBOP->setParentInfo(sys, pm, _cache, blk_size);
    smallBOP->setParentInfo(sys, pm, _cache, blk_size);
    learnedBOP->setParentInfo(sys, pm, _cache, blk_size);

    berti->setParentInfo(sys, pm, _cache, blk_size);

    if (cmc)
        cmc->setParentInfo(sys, pm, _cache, blk_size);

    if (ipcp)
        ipcp->setParentInfo(sys, pm, _cache, blk_size);

    lldp->setParentInfo(sys, pm, _cache, blk_size);
}

void
XSCompositePrefetcher::addTLB(BaseTLB *tlb, bool functional)
{
    Base::addTLB(tlb, functional);
    lldp->addTLB(tlb, functional);
}

void
XSCompositePrefetcher::regProbeListeners()
{
    Queued::regProbeListeners();
    if (enableLLDP)
        lldp->regProbeListeners();
}

void
XSCompositePrefetcher::setPacketReadyCallback(
    std::function<void(Tick)> callback)
{
    if (enableLLDP)
        lldp->setPacketReadyCallback(callback);
    Base::setPacketReadyCallback(std::move(callback));
}

bool
XSCompositePrefetcher::hasPendingPacket()
{
    return Queued::hasPendingPacket() ||
        (enableLLDP && lldp->hasPendingPacket());
}

PacketPtr
XSCompositePrefetcher::getPacket()
{
    const bool lldpReady = enableLLDP &&
        lldp->nextPrefetchReadyTime() <= curTick();
    const bool queuedReady = Queued::nextPrefetchReadyTime() <= curTick();
    if (lldpReady && (!queuedReady || preferLLDP)) {
        preferLLDP = false;
        return lldp->getPacket();
    }
    if (queuedReady) {
        preferLLDP = true;
        return Queued::getPacket();
    }
    return nullptr;
}

Tick
XSCompositePrefetcher::nextPrefetchReadyTime() const
{
    const Tick parent = Queued::nextPrefetchReadyTime();
    return enableLLDP ? std::min(parent, lldp->nextPrefetchReadyTime()) : parent;
}

lldp::Hint
XSCompositePrefetcher::loadTrain(const PacketPtr &pkt, bool miss)
{
    return enableLLDP ? lldp->loadTrain(pkt, miss) : lldp::Hint();
}

void
XSCompositePrefetcher::hintData(const lldp::Hint &hint,
                                 const PacketPtr &demand,
                                 const uint8_t *data, unsigned size)
{
    if (enableLLDP)
        lldp->hintData(hint, demand, data, size);
}
bool XSCompositePrefetcher::GetPFRequestsFromBuffer(std::vector<AddrPriority> &addresses) 
{
    //here we decide which to send for this cycle
    //L1 Streamstride>berti>SMS>CMC>learnedBOP>smallBOP>largeBOP
    //L2 Streamstride>SMS>BOP>TP
    //first we get 1 L1PF
    bool L1PFsent = false;
    if (stridestream_pfFilter_l1.hasPFRequestsInBuffer()){
        L1PFsent = stridestream_pfFilter_l1.GetPFAddrL1(addresses);
    }
    if (!L1PFsent && berti->hasPFRequestsInBuffer()){
        L1PFsent = berti->GetPFRequestsFromBuffer(addresses);
    }
    if(!L1PFsent && sms_pfFilter.hasPFRequestsInBuffer()){
        L1PFsent = sms_pfFilter.GetPFAddrL1(addresses);
    }
    if(!L1PFsent && cmc->hasPFRequestsInBuffer()){
        L1PFsent = cmc->GetPFRequestsFromBuffer(addresses);
    }
    if (BOPPFlevel == 1 && !L1PFsent && learnedBOP->hasPFRequestsInBuffer()){
        L1PFsent = learnedBOP->GetPFRequestsFromBuffer(addresses);
    }
    if (BOPPFlevel == 1 && !L1PFsent && smallBOP->hasPFRequestsInBuffer()){
        L1PFsent = smallBOP->GetPFRequestsFromBuffer(addresses);
    }
    if (BOPPFlevel == 1 && !L1PFsent && largeBOP->hasPFRequestsInBuffer()){
        L1PFsent = largeBOP->GetPFRequestsFromBuffer(addresses);
    }
    if (!L1PFsent && spp->hasPFRequestsInBuffer()){
        L1PFsent = spp->GetPFRequestsFromBuffer(addresses);
    }
    if (!L1PFsent && ipcp->hasPFRequestsInBuffer()){
        L1PFsent = ipcp->GetPFRequestsFromBuffer(addresses);
    }
    if (!L1PFsent && Opt->hasPFRequestsInBuffer()){
        L1PFsent = Opt->GetPFRequestsFromBuffer(addresses);
    }
    bool L2PFsent = false;
    if (stridestream_pfFilter_l2l3.hasPFRequestsInBuffer()){
        L2PFsent = stridestream_pfFilter_l2l3.GetPFAddrL2(addresses);
    }
    if (!L2PFsent && sms_pfFilter.hasPFRequestsInBuffer()){
        L2PFsent = sms_pfFilter.GetPFAddrL2(addresses);
    }
    if (BOPPFlevel == 2 && !L2PFsent && largeBOP->hasPFRequestsInBuffer()){
        L2PFsent = largeBOP->GetPFRequestsFromBuffer(addresses);
        addresses.back().pfahead_host = 2;
        addresses.back().pfahead = true;
    }
    if (BOPPFlevel == 2 && !L2PFsent && smallBOP->hasPFRequestsInBuffer()){
        L2PFsent = smallBOP->GetPFRequestsFromBuffer(addresses);
        addresses.back().pfahead_host = 2;
        addresses.back().pfahead = true;
    }
    if (BOPPFlevel == 2 && !L2PFsent && learnedBOP->hasPFRequestsInBuffer()){
        L2PFsent = learnedBOP->GetPFRequestsFromBuffer(addresses);
        addresses.back().pfahead_host = 2;
        addresses.back().pfahead = true;
    }
    bool L3PFsent = false;
    L3PFsent = stridestream_pfFilter_l2l3.GetPFAddrL3(addresses);
    if (!L3PFsent && sms_pfFilter.hasPFRequestsInBuffer()){
        L3PFsent = sms_pfFilter.GetPFAddrL3(addresses);
    }
    return L1PFsent || L2PFsent || L3PFsent;
}
bool XSCompositePrefetcher::hasPFRequestsInBuffer() {
    return sms_pfFilter.hasPFRequestsInBuffer() ||
            stridestream_pfFilter_l1.hasPFRequestsInBuffer() ||
            stridestream_pfFilter_l2l3.hasPFRequestsInBuffer() ||
            largeBOP->hasPFRequestsInBuffer() ||
            smallBOP->hasPFRequestsInBuffer() ||
            learnedBOP->hasPFRequestsInBuffer() ||
            berti->hasPFRequestsInBuffer() ||
            cmc->hasPFRequestsInBuffer() ||
            spp->hasPFRequestsInBuffer() ||
            ipcp->hasPFRequestsInBuffer() ||
            Opt->hasPFRequestsInBuffer() ;
}
void 
XSCompositePrefetcher::phtSendEventWrapper(){
    for(int i=0; i<3; i++){
        if (phtSentPrefetch[i].valid){
            sms_pfFilter.Insert(phtSentPrefetch[i].region_addr, phtSentPrefetch[i].region_bits,
                phtSentPrefetch[i].alias_bits,phtSentPrefetch[i].paddr_valid, phtSentPrefetch[i].decr_mode,
                phtSentPrefetch[i].is_secure,phtSentPrefetch[i].PFlevel, &phtSentPrefetch[i].trigger,
                &phtSentPrefetch[i].orderScores, phtSentPrefetch[i].l1_bits,
                phtSentPrefetch[i].l2_bits, phtSentPrefetch[i].l3_bits);
            phtSentPrefetch[i].valid = false;
            break;
        }
    }
    if (!phtReqSendEvent.scheduled()){
        if(phtSentPrefetch[0].valid || phtSentPrefetch[1].valid || phtSentPrefetch[2].valid)
            schedule(phtReqSendEvent, nextCycle());
    }
        
}
}  // prefetch
}  // gem5
