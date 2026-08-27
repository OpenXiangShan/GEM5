#include "mem/cache/prefetch/sms.hh"

#include <climits>
#include <cstdint>
#include <iterator>

#include "base/logging.hh"
#include "base/stats/group.hh"
#include "debug/BOPOffsets.hh"
#include "debug/XSCompositePrefetcher.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"
#include "mem/cache/prefetch/context_key.hh"
#include "mem/cache/prefetch/step.hh"

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
          p.act_replacement_policy, ACTEntry(SatCounter8(2, 1))),
      re_act(p.re_act_entries, p.re_act_entries, p.re_act_indexing_policy,
          p.re_act_replacement_policy,ReACTEntry()),
      streamPFAhead(p.stream_pf_ahead),
      pht(p.pht_assoc, p.pht_entries, p.pht_indexing_policy,
          p.pht_replacement_policy,
          PhtEntry(2 * (regionBlks - 1), SatCounter8(3, 2))),
      phtPFAhead(p.pht_pf_ahead),
      phtPFLevel(std::min(p.pht_pf_level, (int) 3)),
      stats(this),
      pfBlockLRUFilter(pfFilterSize),
      stepBlockLRUFilter(pfFilterSize),
      sms_pfFilter(p.sms_filter_indexing_policy, p.sms_filter_replacement_policy, p.sms_filter_entries,
             p.region_size, p.block_size, this, p.vaddr_hash_width,
             PrefetchSourceType::SPht, "sms_pfFilter"),
      stepPb(p.step_pf_buffer_indexing_policy,
             p.step_pf_buffer_replacement_policy, p.step_pf_buffer_entries,
             p.step_region_size, p.block_size, this, p.vaddr_hash_width,
             PrefetchSourceType::STEP, "step_pb", true, true),
      stridestream_pfFilter_l1(p.stridestream_L1_filter_indexing_policy, p.stridestream_L1_filter_replacement_policy,
                     p.stridestream_L1_filter_entries, p.region_size, p.block_size, this,
                     p.vaddr_hash_width, PrefetchSourceType::SStream,
                     "stridestream_pfFilter_l1"),
      stridestream_pfFilter_l2l3(p.stridestream_L2L3_filter_indexing_policy, p.stridestream_L2L3_filter_replacement_policy,
                       p.stridestream_L2L3_filter_entries, p.region_size, p.block_size, this,
                       p.vaddr_hash_width, PrefetchSourceType::SStream,
                       "stridestream_pfFilter_l2l3"),
      step(nullptr),
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
      enableActivepage(p.enable_activepage),
      enablePht(p.enable_pht && !p.enable_step),
      enableStep(p.enable_step),
      enableCPLX(p.enable_cplx),
      enableSPP(p.enable_spp),
      enableTemporal(p.enable_temporal),
      enableSstride(p.enable_sstride),
      enableBerti(p.enable_berti),
      enableBOP(p.enable_bop),
      enableOpt(p.enable_opt),
      enableXsstream(p.enable_xsstream),
      phtEarlyUpdate(p.pht_early_update),
      neighborPhtUpdate(p.neighbor_pht_update),
      stepRegionSize(p.step_region_size),
      stepPFLevel(p.step_pf_level),
      phtSentPrefetch(),
      phtReqSendEvent([this]{ phtSendEventWrapper(); },
          name()),
      BOPPFlevel(p.bop_pf_level)
{
    assert(largeBOP);
    assert(smallBOP);
    assert(learnedBOP);
    assert(isPowerOf2(regionSize));
    fatal_if(enableStep && !usePFBuffer,
             "STEP requires use_pf_buffer=true; disable-pf-buffer is "
             "unsupported");
    fatal_if(enableStep && p.prefetch_train,
             "STEP requires prefetch_train=false so its PB is the only "
             "prefetch-training path");
    if (enableStep) {
        step = std::make_unique<StepSpatialPrefetcher>(p, this);
    }

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

    DPRINTF(XSCompositePrefetcher, "SMS: region_size: %d regionBlks: %d\n",
            regionSize, regionBlks);
    DPRINTF(XSCompositePrefetcher,
            "STEP: enabled=%d region_size=%u target_level=%d\n",
            enableStep, stepRegionSize, stepPFLevel);
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

XSCompositePrefetcher::~XSCompositePrefetcher() = default;

void
XSCompositePrefetcher::regProbeListeners()
{
    if (enableStep) {
        fatal_if(cache == nullptr || cache->level() != 1,
                 "STEP must be attached to an L1 data cache");
        fatal_if(stepPFLevel >= 2 && !hasHintDownStream(),
                 "STEP target level %d requires an L1-to-L2 hint",
                 stepPFLevel);
        fatal_if(stepPFLevel >= 3 &&
                     (!hasHintDownStream() ||
                      !hintDownStream->hasHintDownStream()),
                 "STEP target level 3 requires an L2-to-L3 hint");
    }
    Queued::regProbeListeners();
}

void
XSCompositePrefetcher::observeRawDemandAccess(const PacketPtr &pkt, bool miss)
{
    if (!enableStep || pkt->req->isPrefetch() || pkt->req->isInstFetch() ||
        pkt->isWrite() || !pkt->isRead() || pkt->isStorePFTrain() ||
        !pkt->req->hasPC()) {
        return;
    }

    // STEP follows the parent prefetcher's address-space setting but does not
    // inherit its miss/prefetch-hit trigger policy.  The hook is intentionally
    // before Base::observeAccess(), so every eligible demand load contributes
    // to FOE/SOE/TOE without changing legacy component training.
    if (useVirtualAddresses && !pkt->req->hasVaddr()) {
        return;
    }
    const Addr address = useVirtualAddresses ? pkt->req->getVaddr() :
        pkt->req->getPaddr();
    const ContextID context_id = pkt->req->hasContextId() ?
        pkt->req->contextId() : InvalidContextID;

    assert(step);
    const auto decision = step->observe(address, pkt->req->getPC(),
                                        context_id, pkt->isSecure(), true);
    if (!decision) {
        return;
    }

    PrefetchSourceType pf_source = PrefetchSourceType::PF_NONE;
    int pf_depth = 0;
    if (miss) {
        pf_source = pkt->getPFSource();
        pf_depth = pkt->getPFDepth();
    } else {
        const auto metadata = cache->getHitBlkXsMetadata(pkt);
        pf_source = metadata.prefetchSource;
        pf_depth = metadata.prefetchDepth;
    }
    PrefetchInfo pfi(pkt, address, miss,
                     Request::XsMetadata(pf_source, pf_depth));
    // The normal Base path snapshots this request attribute into its deferred
    // PrefetchInfo. STEP bypasses that path intentionally, so retain the same
    // provenance in the PB's deep-copied trigger state without consuming the
    // legacy squash marker.
    pfi.setReqAfterSquash(pkt->req->isFirstReqAfterSquash());
    pfi.setEverPrefetched(
        hasEverBeenPrefetched(pkt->getAddr(), pkt->isSecure()));
    pfi.setPfFirstHit(
        !miss && hasBeenPrefetched(pkt->getAddr(), pkt->isSecure()));
    pfi.setPfHit(!miss && pfi.isEverPrefetched());
    pfi.setTriggerInfo(pkt);

    const uint64_t pb_admitted_blocks = bufferStepPrefetches(
        pfi, decision->region, decision->candidates, decision->id);
    step->recordBuffered(*decision, pb_admitted_blocks);
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

        const bool spatial_trigger = pfi.isCacheMiss() ||
            (pfi.isPfFirstHit() &&
             (pf_source == PrefetchSourceType::SStride ||
              pf_source == PrefetchSourceType::HWP_BOP ||
              pf_source == PrefetchSourceType::SPht ||
              pf_source == PrefetchSourceType::STEP ||
              pf_source == PrefetchSourceType::IPCP_CPLX ||
              pf_source == PrefetchSourceType::SPP ||
              pf_source == PrefetchSourceType::Berti));

        const bool use_pht = spatial_trigger && !pfi.isStore() && enablePht;
        bool trigger_pht = false;
        stride_pf_addr = phtPFAhead ? stride_pf_addr : 0;  // trigger addr sent to pht
        if (use_pht) {
            DPRINTF(XSCompositePrefetcher, "Do PHT lookup...\n");
            trigger_pht = phtLookup(pfi, addresses, late && pf_source == PrefetchSourceType::SPht, stride_pf_addr);
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
        if (!enableStep && phtEarlyUpdate)
            updatePht(entry, region_start, re_act_entry, true, region_offset);
        if (!(entry->regionBits & region_bit_accessed)) {
            entry->accessCount += 1;
            is_first_shot = true;
        }
        entry->regionBits |= region_bit_accessed;
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

    if (!enableStep) {
        updatePht(entry, region_start, re_act_mode, false, 0);
    }
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
XSCompositePrefetcher::updatePht(XSCompositePrefetcher::ACTEntry *act_entry, Addr current_region_addr,
                                 bool re_act_mode, bool early_update, Addr region_offset_now)
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
            assert ((region_offset_now - act_entry->regionOffset + regionBlks - 2) > 14);
            assert ((region_offset_now - act_entry->regionOffset + regionBlks - 2) <= 30);
            pht_entry->hist[region_offset_now - act_entry->regionOffset + regionBlks - 2] += 2;
            act_entry->hasIncreasedPht = true;
        }
        if (region_offset_now < act_entry->regionOffset) {
            assert(regionBlks - 1 >= (act_entry->regionOffset - region_offset_now));
            assert((regionBlks - 1 - (act_entry->regionOffset - region_offset_now)) <= 14);
            pht_entry->hist[regionBlks - 1 - (act_entry->regionOffset - region_offset_now)] += 2;
            act_entry->hasIncreasedPht = true;
        }
        return;
    }
    if (early_update) {
        const int access_cnt_thres = 5;
        if (act_entry->accessCount > access_cnt_thres && (!pht_entry)) {
            pht_entry = pht.findVictim(pht_key);
            for (uint8_t i = 0; i < 2 * (regionBlks - 1); i++) {
                pht_entry->hist[i].reset();
            }
            pht_entry->pc = act_entry->pc;
            pht_entry->contextId = act_entry->contextId;
            act_entry->hasIncreasedPht = true;
            pht_entry->decr_mode = act_entry->inBackwardMode;
        } else {
            return;
        }
    }

    if (!pht_entry) {
        pht_entry = pht.findVictim(pht_key);
        DPRINTF(XSCompositePrefetcher, "Evict PHT entry for PC %lx\n", pht_entry->pc);
        for (uint8_t i = 0; i < 2 * (regionBlks - 1); i++) {
            pht_entry->hist[i].reset();
        }
        pht_entry->pc = act_entry->pc;
        pht_entry->contextId = act_entry->contextId;
        pht_entry->decr_mode = act_entry->inBackwardMode;
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
    //  incr part
    if (act_entry_f) {
        for (int i = region_offset + 1, j = 0; j < regionBlks - 1; i++, j++) {
            uint8_t hist_idx = j + (regionBlks - 1);
            bool accessed;
            if (i > 15)
                accessed = (act_entry_f->regionBits >> (i - 16)) & 1;
            else
                accessed = (act_entry->regionBits >> i) & 1;
            updatePhtBits(accessed, early_update, re_act_mode, hist_idx, act_entry, pht_entry);
        }
    } else {
        for (int i = region_offset + 1, j = 0; j < regionBlks - 1; i++, j++) {
            uint8_t hist_idx = j + (regionBlks - 1);
            if (i < regionBlks) {
                bool accessed = (act_entry->regionBits >> i) & 1;
                updatePhtBits(accessed,early_update,re_act_mode,hist_idx,act_entry,pht_entry);
            } else {
                if (!early_update)
                    pht_entry->hist.at(hist_idx) -= 1;
            }
        }
    }

    // decr part
    int i_b = 0;
    if (act_entry_b) {
        for (int i = int(region_offset) - 1, j = regionBlks - 2; j >= 0;
             i--, j--) {
            if (i >= 0) {
                bool accessed = (act_entry->regionBits >> i) & 1;
                updatePhtBits(accessed, early_update, re_act_mode, j, act_entry, pht_entry);
            } else {
                // TODO: unseen should be untouch?
                bool accessed = (act_entry_b->regionBits >> (15 - i_b)) & 1;
                i_b++;
                updatePhtBits(accessed, early_update, re_act_mode, j, act_entry, pht_entry);
            }
        }

    } else {
        for (int i = int(region_offset) - 1, j = regionBlks - 2; j >= 0;
             i--, j--) {
            if (i >= 0) {
                bool accessed = (act_entry->regionBits >> i) & 1;
                updatePhtBits(accessed, early_update, re_act_mode, j, act_entry, pht_entry);
            } else {
                // leave unseen untouched
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
bool
XSCompositePrefetcher::phtLookup(const Base::PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late,
                         Addr look_ahead_addr)
{
    Addr pc = pfi.getPC();
    Addr vaddr = look_ahead_addr ? look_ahead_addr : pfi.getAddr();
    Addr blk_addr = blockAddress(vaddr);
    Addr region_addr = regionAddress(vaddr);
    Addr region_offset = regionOffset(vaddr);
    uint64_t region_bit_cur = 0;
    uint64_t region_bit_inc = 0;
    Addr region_inc_addr = 0;
    uint64_t region_bit_dec = 0;
    Addr region_dec_addr = 0;
    bool secure = pfi.isSecure();
    ContextID context_id = pfi.hasContextId() ?
        pfi.contextId() : InvalidContextID;
    PhtEntry *pht_entry = pht.findEntry(
        contextKey(phtHash(pc, region_offset), context_id), secure);
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
                if(regionAddress(pf_tgt_addr) == region_addr) {
                    region_bit_cur |= (uint64_t(1) << regionOffset(pf_tgt_addr));
                    sendPFWithFilter(pfi, pf_tgt_addr, addresses, priority--, PrefetchSourceType::SPht, phtPFLevel);
                    found = true;
                }
            }
        }
        for (int i = regionBlks - 2, j = 1; i >= 0; i--, j++) {
            if (pht_entry->hist[i].calcSaturation() > 0.5) {
                Addr pf_tgt_addr = blk_addr - j * blkSize;
                if(regionAddress(pf_tgt_addr) == region_addr) {
                    region_bit_cur |= (uint64_t(1) << regionOffset(pf_tgt_addr));
                    sendPFWithFilter(pfi, pf_tgt_addr, addresses, priority--, PrefetchSourceType::SPht, phtPFLevel);
                    found = true;
                }
            }
        }
        if(found){
            if(phtSentPrefetch[0].valid){
                stats.smsCurRegionoverride++;
            }
            phtSentPrefetch[0] = phtsentInfo(region_addr, region_bit_cur ,0, true,pht_entry->decr_mode,secure,phtPFLevel, &pfi.trigger_info);
            phtSentPrefetch[0].trigger.pfSourceType = PrefetchSourceType::SPht;
        }
        found = false;
        for (uint8_t i = 0; i < regionBlks - 1; i++) {
            if (pht_entry->hist[i + regionBlks - 1].calcSaturation() > 0.5) {
                Addr pf_tgt_addr = blk_addr + (i + 1) * blkSize;
                if(regionAddress(pf_tgt_addr) != region_addr) {
                    region_inc_addr = regionAddress(pf_tgt_addr);
                    region_bit_inc |= (uint64_t(1) << regionOffset(pf_tgt_addr));
                    sendPFWithFilter(pfi, pf_tgt_addr, addresses, priority--, PrefetchSourceType::SPht, phtPFLevel);
                    found = true;
                }
            }
        }
        if(found){
            if(phtSentPrefetch[1].valid){
                stats.smsIncrRegionoverride++;
            }
            phtSentPrefetch[1] = phtsentInfo(region_inc_addr, region_bit_inc ,0, true,pht_entry->decr_mode,secure,phtPFLevel, &pfi.trigger_info);
            phtSentPrefetch[1].trigger.pfSourceType = PrefetchSourceType::SPht;
        }
        
        found = false;
        for (int i = regionBlks - 2, j = 1; i >= 0; i--, j++) {
            if (pht_entry->hist[i].calcSaturation() > 0.5) {
                Addr pf_tgt_addr = blk_addr - j * blkSize;
                if(regionAddress(pf_tgt_addr) != region_addr) {
                    region_dec_addr = regionAddress(pf_tgt_addr);
                    region_bit_dec |= (uint64_t(1) << regionOffset(pf_tgt_addr));
                    sendPFWithFilter(pfi, pf_tgt_addr, addresses, priority--, PrefetchSourceType::SPht, phtPFLevel);
                    found = true;
                }
            }
        }
        if(found){
            if(phtSentPrefetch[2].valid){
                stats.smsDecrRegionoverride++;
            }
            phtSentPrefetch[2] = phtsentInfo(region_dec_addr, region_bit_dec ,0, true,pht_entry->decr_mode,secure,phtPFLevel, &pfi.trigger_info);
            phtSentPrefetch[2].trigger.pfSourceType = PrefetchSourceType::SPht;
        }
        if (!phtReqSendEvent.scheduled()){
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
                                        int prio, PrefetchSourceType src,
                                        int ahead_level)
{
    // Keep the legacy SMS/stream/Berti filter contract unchanged. STEP has a
    // separate PB handoff function so enabling it cannot alter this path.
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
        if (!(src == PrefetchSourceType::SStream ||
              src == PrefetchSourceType::StoreStream)) {
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
        DPRINTF(XSCompositePrefetcher, "Send pf: %lx, target level: %i\n",
                addr, ahead_level);
        return true;
    }
}

Addr
XSCompositePrefetcher::stepBlockFilterKey(Addr addr,
                                          ContextID context_id) const
{
    // contextKey() deliberately treats InvalidContextID as context zero for
    // legacy callers. STEP's private filter follows its FT/AT/PHT isolation
    // and keeps those two ownership domains distinct.
    const ContextID table_context = context_id == InvalidContextID ?
        INT_MAX : context_id;
    return contextKey(blockAddress(addr), table_context);
}

bool
XSCompositePrefetcher::stepPrefetchFiltered(const PrefetchInfo &pfi, Addr addr,
                                            int ahead_level)
{
    const ContextID context_id = pfi.hasContextId() ? pfi.contextId() :
        InvalidContextID;
    const Addr step_block_key = stepBlockFilterKey(addr, context_id);
    if (stepBlockLRUFilter.contains(step_block_key)) {
        DPRINTF(XSCompositePrefetcher,
                "STEP skip prefetched or filled line: %lx\n", addr);
        return true;
    }

    const Addr page_key = sharedFilterKey(pfi, regionAddress(addr));
    const Addr block_key = sharedFilterKey(pfi, addr);
    if (ahead_level < 2 && pfPageLRUFilter.contains(page_key)) {
        return true;
    }
    if (ahead_level == 2 && pfPageLRUFilterL2.contains(page_key)) {
        return true;
    }
    if (ahead_level == 3 && pfPageLRUFilterL3.contains(page_key)) {
        return true;
    }
    return pfBlockLRUFilter.contains(block_key);
}

bool
XSCompositePrefetcher::sendStepPFWithFilter(
    const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses,
    int prio, int ahead_level)
{
    // This preflight is intentionally side-effect free. The shared filter,
    // trace, and generated count are committed only after Queued accepts the
    // candidate, so a rejected FOE cannot suppress a later TOE decision.
    if (stepPrefetchFiltered(pfi, addr, ahead_level)) {
        prefetchStats.pfFiltered++;
        return false;
    }

    addresses.emplace_back(addr, prio, PrefetchSourceType::STEP);
    // Preserve the target level even for local L1 placement.  The generic
    // offload path needs this identity to keep a STEP L1 target from leaking
    // to L2 when prefetch_can_offload is enabled.
    addresses.back().pfahead_host = ahead_level;
    if (ahead_level > 1) {
        assert(ahead_level == 2 || ahead_level == 3);
        addresses.back().pfahead = true;
    } else {
        addresses.back().pfahead = false;
    }
    DPRINTF(XSCompositePrefetcher, "STEP send pf: %lx, target level: %i\n",
            addr, ahead_level);
    return true;
}

uint64_t
XSCompositePrefetcher::bufferStepPrefetches(const PrefetchInfo &pfi,
                                             Addr region,
                                             uint64_t candidates,
                                             uint64_t decision_id)
{
    const unsigned region_blks = stepRegionSize / blkSize;
    assert(region_blks > 0 && region_blks <= 64);
    const uint64_t valid_bits = region_blks == 64 ? ~uint64_t(0) :
        ((uint64_t(1) << region_blks) - 1);
    // Admission only probes the filters. The shared-filter insertion, trace
    // write, and generated count happen at PB-to-Queued handoff.
    uint64_t pb_admitted_blocks = 0;
    uint64_t remaining = candidates & valid_bits;
    while (remaining) {
        const unsigned offset = __builtin_ctzll(remaining);
        const uint64_t bit = uint64_t(1) << offset;
        const Addr addr = region * stepRegionSize + offset * blkSize;
        if (!stepPrefetchFiltered(pfi, addr, stepPFLevel)) {
            pb_admitted_blocks |= bit;
        }
        remaining &= remaining - 1;
    }

    if (pb_admitted_blocks == 0) {
        return 0;
    }

    pfi.setTriggerInfo_PFsrc(PrefetchSourceType::STEP);
    pfi.setTriggerInfo_StagedDecisionId(decision_id);
    const auto result = stepPb.insertStaged(
        region, pb_admitted_blocks, 0, true, false, pfi.isSecure(),
        stepPFLevel, pfi.trigger_info, decision_id);
    for (unsigned i = 0; i < result.evictedTokenCount; ++i) {
        const auto &token = result.evictedTokens[i];
        step->recordTerminalFailure(token.region, token.contextId,
                                    token.secure, token.decisionId);
    }
    if (result.newBits != 0 && !PFReqSendEvent.scheduled()) {
        schedule(PFReqSendEvent, nextCycle());
    }
    // A later STEP decision may overlap a still-live PB footprint. Only new
    // bits create a pending FT decision; existing bits retain their original
    // trigger and decision identity until their own terminal disposition.
    return result.newBits;
}

bool
XSCompositePrefetcher::getStepPrefetchFromBuffer(
    std::vector<AddrPriority> &addresses, int target_level)
{
    const auto request = stepPb.reserveStaged(target_level);
    if (!request) {
        return false;
    }

    fatal_if(request->trigger.pfi_old == nullptr,
             "STEP prefetch buffer entry has no trigger state");

    // Reserving hides this PB bit from another arbitration pass until Queued
    // either reaches its target PFQ or reports a terminal rejection.
    PrefetchInfo trigger_pfi(*request->trigger.pfi_old);
    std::vector<AddrPriority> accepted;
    if (!sendStepPFWithFilter(trigger_pfi, request->address, accepted,
                              request->priority, target_level)) {
        if (stepPb.completeStaged(request->token, false)) {
            step->recordBufferHandoffFiltered();
            step->recordTerminalFailure(
                request->token.region, request->token.contextId,
                request->token.secure, request->token.decisionId);
        }
        return false;
    }

    assert(accepted.size() == 1);
    accepted.front().pf_trigger_info = request->trigger;
    // Selecting a PB bit only makes it eligible for Queued. The FT transition
    // is completed after PF control, page checks, and queue admission decide
    // whether this candidate actually has a downstream destination.
    accepted.front().stagedToken = request->token;
    addresses.push_back(accepted.front());
    return true;
}

void
XSCompositePrefetcher::completeStagedPrefetch(const StagedPrefetchToken &token,
                                               bool accepted)
{
    if (!step) {
        return;
    }

    const auto trigger = stepPb.completeStaged(token, accepted);
    if (!trigger) {
        return;
    }
    fatal_if(trigger->pfi_old == nullptr,
             "STEP staged prefetch has no trigger state");
    const auto &trigger_pfi = *trigger->pfi_old;
    const Addr address = token.region * stepRegionSize +
        token.offset * blkSize;

    if (accepted) {
        // This is the legacy composite shared filter. Its key shape remains
        // unchanged; STEP candidates are already cache-line aligned.
        prefetchStats.pfGenerated++;
        pfBlockLRUFilter.insert(sharedFilterKey(trigger_pfi, address),
                                0);
        if (archDBer) {
            archDBer->l1PFTraceWrite(curTick(), trigger_pfi.getPC(),
                                     trigger_pfi.getAddr(), address,
                                     PrefetchSourceType::STEP);
        }
        step->recordHandoff(token.region, token.contextId, token.secure,
                            token.decisionId);
    } else {
        step->recordBufferHandoffRejected();
        step->recordTerminalFailure(token.region, token.contextId,
                                    token.secure, token.decisionId);
    }

    if (stepPb.hasStagedRequests() && !PFReqSendEvent.scheduled()) {
        schedule(PFReqSendEvent, nextCycle());
    }
}

void
XSCompositePrefetcher::releaseStagedPrefetch(const StagedPrefetchToken &token)
{
    // Throttle truncation has not sent this request to Queued. Keep the PB
    // candidate and its FT decision live for a future arbitration turn.
    if (stepPb.releaseStaged(token) && stepPb.hasStagedRequests() &&
        !PFReqSendEvent.scheduled()) {
        schedule(PFReqSendEvent, nextCycle());
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
                                     XSCompositePrefetcher::PhtEntry *pht_entry)
{
    if (accessed) {
        DPRINTF(XSCompositePrefetcher, "Inc conf hist_idx: %d\n", hist_idx);
        if (early_update) {
            pht_entry->hist.at(hist_idx) += 2;
        } else {
            if ((!act_entry->hasIncreasedPht))
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
        // Preserve the legacy shared-filter update exactly. Existing SMS and
        // companion prefetchers depend on this virtual-address behavior.
        stats.refillNotifyCount++;
        berti->notifyFill(pkt);
        ContextID context_id = pkt->req->hasContextId() ?
            pkt->req->contextId() : InvalidContextID;
        pfBlockLRUFilter.insert(
            contextKey(pkt->req->getVaddr(), context_id), 0);
    }

    if (!enableStep || (useVirtualAddresses && !pkt->req->hasVaddr())) {
        return;
    }

    const Addr address = useVirtualAddresses ? pkt->req->getVaddr() :
        pkt->req->getPaddr();
    const ContextID context_id = pkt->req->hasContextId() ?
        pkt->req->contextId() : InvalidContextID;
    // The STEP private filter sees the same address space as its raw-demand
    // observer and always uses cache-line granularity.
    stepBlockLRUFilter.insert(stepBlockFilterKey(address, context_id), 0);
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
      ADD_STAT(totalTrainCount, statistics::units::Count::get(), "total train count")
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
}
bool XSCompositePrefetcher::GetPFRequestsFromBuffer(std::vector<AddrPriority> &addresses) 
{
    //here we decide which to send for this cycle
    //L1 Streamstride>berti>STEP/SMS>CMC>learnedBOP>smallBOP>largeBOP
    //L2 Streamstride>STEP/SMS>BOP>TP
    //first we get 1 L1PF
    bool L1PFsent = false;
    if (stridestream_pfFilter_l1.hasPFRequestsInBuffer()){
        L1PFsent = stridestream_pfFilter_l1.GetPFAddrL1(addresses);
    }
    if (!L1PFsent && berti->hasPFRequestsInBuffer()){
        L1PFsent = berti->GetPFRequestsFromBuffer(addresses);
    }
    if (!L1PFsent && enableStep && stepPb.hasStagedRequests()) {
        L1PFsent = getStepPrefetchFromBuffer(addresses, 1);
    }
    if (!L1PFsent && !enableStep && sms_pfFilter.hasPFRequestsInBuffer()) {
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
    if (!L2PFsent && enableStep && stepPb.hasStagedRequests()) {
        L2PFsent = getStepPrefetchFromBuffer(addresses, 2);
    }
    if (!L2PFsent && !enableStep && sms_pfFilter.hasPFRequestsInBuffer()) {
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
    if (!L3PFsent && enableStep && stepPb.hasStagedRequests()) {
        L3PFsent = getStepPrefetchFromBuffer(addresses, 3);
    }
    if (!L3PFsent && !enableStep && sms_pfFilter.hasPFRequestsInBuffer()) {
        L3PFsent = sms_pfFilter.GetPFAddrL3(addresses);
    }
    return L1PFsent || L2PFsent || L3PFsent;
}
bool XSCompositePrefetcher::hasPFRequestsInBuffer() {
    return (enableStep && stepPb.hasStagedRequests()) ||
            (!enableStep && sms_pfFilter.hasPFRequestsInBuffer()) ||
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
                phtSentPrefetch[i].is_secure,phtSentPrefetch[i].PFlevel, &phtSentPrefetch[i].trigger);
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
