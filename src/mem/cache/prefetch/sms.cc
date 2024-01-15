#include "mem/cache/prefetch/sms.hh"

#include "debug/BOPOffsets.hh"
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
      learnedBOP(dynamic_cast<BOP *>(p.bop_learned)),
      spp(dynamic_cast<SignaturePath *>(p.spp)),
      ipcp(dynamic_cast<IPCP *>(p.ipcp)),
      cmc(p.cmc),
      berti(p.berti),
      Sstride(p.sstride),
      Opt(p.opt),
      enableCPLX(p.enable_cplx),
      enableSPP(p.enable_spp),
      enableTemporal(p.enable_temporal),
      enableSstride(p.enable_sstride),
      enableBerti(p.enable_berti),
      enableOpt(p.enable_opt),
      phtEarlyUpdate(p.pht_early_update),
      neighborPhtUpdate(p.neighbor_pht_update)
{
    assert(largeBOP);
    assert(smallBOP);
    assert(learnedBOP);
    assert(isPowerOf2(regionSize));


    largeBOP->filter = &this->pfBlockLRUFilter;
    smallBOP->filter = &this->pfBlockLRUFilter;
    learnedBOP->filter = &this->pfBlockLRUFilter;
    if (berti)
        berti->filter = &this->pfBlockLRUFilter;
    if (Sstride)
        Sstride->filter = &this->pfBlockLRUFilter;

    if (cmc)
        cmc->filter = &this->pfBlockLRUFilter;

    if (ipcp)
        ipcp->rrf = &this->pfBlockLRUFilter;
    if (Opt)
        Opt->filter = &this->pfBlockLRUFilter;

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

    Addr region_offset = regionOffset(vaddr);
    bool is_active_page = false;
    bool enter_new_region = false;
    bool is_first_shot = false;
    ACTEntry *act_match_entry = nullptr;
    Addr pf_tgt_addr = 0;
    bool decr = false;
    bool is_first_64 = false;
    if (pfi.isCacheMiss() || pfi.isPfFirstHit()) {
        act_match_entry = actLookup(pfi, is_active_page, enter_new_region, is_first_shot);
        if (enableOpt){
            assert(Opt);
            Opt->calculatePrefetch(pfi, addresses, is_first_64);
        }
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
                sendStreamPF(pfi, pf_tgt_addr, addresses, pfPageLRUFilter, decr, 1);
            }
        }
    }

    if (act_match_entry && is_active_page && pf_tgt_addr && enter_new_region) {
        if (streamPFAhead) {
            Addr pf_tgt_addr_l2 = decr ? pf_tgt_addr - 48 * blkSize : pf_tgt_addr + 48 * blkSize;  // depth here?
            sendStreamPF(pfi, pf_tgt_addr_l2, addresses, pfPageLRUFilterL2, decr, 2);
            Addr pf_tgt_addr_l3 = decr ? pf_tgt_addr - 256 * blkSize : pf_tgt_addr + 256 * blkSize;  // depth here?
            sendStreamPF(pfi,pf_tgt_addr_l3,addresses,pfPageLRUFilterL3,decr,3);
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
                        (pf_source == PrefetchSourceType::HWP_BOP || pf_source == PrefetchSourceType::IPCP_CPLX || pf_source == PrefetchSourceType::Berti)) ||
                       pfi.isCacheMiss();
        use_bop &= !miss_repeat && is_first_shot; // miss repeat should not be handled by stride
        if (use_bop) {
            DPRINTF(XSCompositePrefetcher, "Do BOP traing/prefetching...\n");
            largeBOP->calculatePrefetch(pfi, addresses, late && pf_source == PrefetchSourceType::HWP_BOP);

            smallBOP->calculatePrefetch(pfi, addresses, late && pf_source == PrefetchSourceType::HWP_BOP);
        }

        Addr stride_pf_addr = 0;
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

        bool use_stride = !pfi.isStore() && (pfi.isCacheMiss() || pfi.isPfFirstHit()) && enableSstride;
        if (use_stride){
            DPRINTF(XSCompositePrefetcher, "Do Sstride traing/prefetching...\n");
            int64_t learned_bop_offset = 0;
            Sstride->calculatePrefetch(pfi, addresses, late, pf_source, miss_repeat, enter_new_region, is_first_shot,
                                       stride_pf_addr, learned_bop_offset);
            if (learned_bop_offset != 0)
                learnedBOP->tryAddOffset(learned_bop_offset);
        }

        bool use_pht = pfi.isCacheMiss() ||
                       (pfi.isPfFirstHit() &&
                        (pf_source == PrefetchSourceType::SStride || pf_source == PrefetchSourceType::HWP_BOP ||
                         pf_source == PrefetchSourceType::SPht || pf_source == PrefetchSourceType::IPCP_CPLX ||
                         pf_source == PrefetchSourceType::SPP || pf_source == PrefetchSourceType::Berti));

        use_pht &= !pfi.isStore();

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
        if (phtEarlyUpdate)
            updatePht(entry, region_start, re_act_entry, true, region_offset);
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

    updatePht(entry, region_start, re_act_mode, false, 0);  // update pht with evicted entry
    entry->pc = pc;
    entry->is_secure = secure;
    entry->decr_mode = !forward;
    entry->regionAddr = region_start;
    entry->region_offset = region_offset;
    entry->region_bits = 1UL << region_offset;
    //entry->repeat_region_bits = 0;
    entry->access_cnt = 1;
    entry->signal_update = false;
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
XSCompositePrefetcher::updatePht(XSCompositePrefetcher::ACTEntry *act_entry, Addr current_region_addr,
                                 bool re_act_mode, bool early_update, Addr region_offset_now)
{
    if (popCount(act_entry->region_bits) <= 1) {
        return;
    }
    PhtEntry *pht_entry = pht.findEntry(phtHash(act_entry->pc, act_entry->region_offset), act_entry->is_secure);
    bool is_update = pht_entry != nullptr;
    if (pht_entry && early_update) {
        if (region_offset_now > act_entry->region_offset) {
            assert ((region_offset_now - act_entry->region_offset + regionBlks - 2) > 14);
            assert ((region_offset_now - act_entry->region_offset + regionBlks - 2) <= 30);
            pht_entry->hist[region_offset_now - act_entry->region_offset + regionBlks - 2] += 2;
            act_entry->signal_update = true;
        }
        if (region_offset_now < act_entry->region_offset) {
            assert(regionBlks - 1 >= (act_entry->region_offset - region_offset_now));
            assert((regionBlks - 1 - (act_entry->region_offset - region_offset_now)) <= 14);
            pht_entry->hist[regionBlks - 1 - (act_entry->region_offset - region_offset_now)] += 2;
            act_entry->signal_update = true;
        }
        return;
    }
    if (early_update) {
        const int access_cnt_thres = 5;
        if (act_entry->access_cnt > access_cnt_thres && (!pht_entry)) {
            pht_entry = pht.findVictim(phtHash(act_entry->pc, act_entry->region_offset));
            for (uint8_t i = 0; i < 2 * (regionBlks - 1); i++) {
                pht_entry->hist[i].reset();
            }
            pht_entry->pc = act_entry->pc;
            act_entry->signal_update = true;
        } else {
            return;
        }
    }

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
    Addr region_addr_find = act_entry->regionAddr / regionSize;
    ACTEntry *act_entry_f = nullptr;
    ACTEntry *act_entry_b = nullptr;
    if (neighborPhtUpdate){
        act_entry_f = act.findEntry(region_addr_find + 1, act_entry->is_secure);
        act_entry_b = act.findEntry(region_addr_find - 1, act_entry->is_secure);
    }
    //  incr part
    if (act_entry_f) {
        for (int i = region_offset + 1, j = 0; j < regionBlks - 1; i++, j++) {
            uint8_t hist_idx = j + (regionBlks - 1);
            bool accessed;
            if (i > 15)
                accessed = (act_entry_f->region_bits >> (i - 16)) & 1;
            else
                accessed = (act_entry->region_bits >> i) & 1;
            updatePhtBits(accessed, early_update, re_act_mode, hist_idx, act_entry, pht_entry);
        }
    } else {
        for (int i = region_offset + 1, j = 0; j < regionBlks - 1; i++, j++) {
            uint8_t hist_idx = j + (regionBlks - 1);
            if (i < regionBlks) {
                bool accessed = (act_entry->region_bits >> i) & 1;
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
                bool accessed = (act_entry->region_bits >> i) & 1;
                updatePhtBits(accessed, early_update, re_act_mode, j, act_entry, pht_entry);
            } else {
                // TODO: unseen should be untouch?
                bool accessed = (act_entry_b->region_bits >> (15 - i_b)) & 1;
                i_b++;
                updatePhtBits(accessed, early_update, re_act_mode, j, act_entry, pht_entry);
            }
        }

    } else {
        for (int i = int(region_offset) - 1, j = regionBlks - 2; j >= 0;
             i--, j--) {
            if (i >= 0) {
                bool accessed = (act_entry->region_bits >> i) & 1;
                updatePhtBits(accessed, early_update, re_act_mode, j, act_entry, pht_entry);
            } else {
                // leave unseen untouched
            }
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
                sendPFWithFilter(pfi, pf_tgt_addr, addresses, priority--, PrefetchSourceType::SPht, phtPFLevel);
                found = true;
            }
        }
        for (int i = regionBlks - 2, j = 1; i >= 0; i--, j++) {
            if (pht_entry->hist[i].calcSaturation() > 0.5) {
                Addr pf_tgt_addr = blk_addr - j * blkSize;
                sendPFWithFilter(pfi, pf_tgt_addr, addresses, priority--, PrefetchSourceType::SPht, phtPFLevel);
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
    }
    return found;
}

bool
XSCompositePrefetcher::sendPFWithFilter(const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses,
                                        int prio, PrefetchSourceType src, int ahead_level)
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
        sendPFWithFilter(pfi, cur, addresses, regionBlks - i, stream_type, pf_level);
        DPRINTF(XSCompositePrefetcher, "pf addr: %x [%d] pf_level %d\n", cur, i, pf_level);
        fatal_if(i < 0, "i < 0\n");
    }
    Filter.insert(pf_tgt_region, 0);
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
            if ((!act_entry->signal_update))
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
    stats.refillNotifyCount++;
    berti->notifyFill(pkt);
    pfBlockLRUFilter.insert(pkt->req->getVaddr(), 0);
}

XSCompositePrefetcher::XSCompositeStats::XSCompositeStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(allCntNum, statistics::units::Count::get(), "victim act access num"),
      ADD_STAT(actMNum, statistics::units::Count::get(), "victim act match num"),
      ADD_STAT(refillNotifyCount, statistics::units::Count::get(), "refill notify count")
{
}

void
XSCompositePrefetcher::setCache(BaseCache *_cache)
{
    Base::setCache(_cache);

    largeBOP->setCache(_cache);
    smallBOP->setCache(_cache);
    learnedBOP->setCache(_cache);

    berti->setCache(_cache);

    if (cmc)
        cmc->setCache(_cache);

    if (ipcp)
        ipcp->setCache(_cache);
}

}  // prefetch
}  // gem5
