#include "mem/cache/prefetch/sms.hh"

#include "debug/SMSPrefetcher.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"

namespace gem5
{
namespace prefetch
{

SMSPrefetcher::SMSPrefetcher(const SMSPrefetcherParams &p)
    : Queued(p),
      region_size(p.region_size),
      region_blocks(p.region_size / p.block_size),
      act(p.act_entries, p.act_entries, p.act_indexing_policy,
          p.act_replacement_policy, ACTEntry(SatCounter8(2, 1))),
      strideDynDepth(p.stride_dyn_depth),
      stride(p.stride_entries, p.stride_entries, p.stride_indexing_policy,
             p.stride_replacement_policy, StrideEntry()),
      pht(p.pht_assoc, p.pht_entries, p.pht_indexing_policy,
          p.pht_replacement_policy,
          PhtEntry(2 * (region_blocks - 1), SatCounter8(2, 1))),
      pfBlockLRUFilter(pfFilterSize),
      pfPageLRUFilter(pfFilterSize),
      bop(dynamic_cast<BOP *>(p.bop)),
      spp(dynamic_cast<SignaturePath *>(p.spp)),
      ipcp(dynamic_cast<IPCP *>(p.ipcp)),
      enableCPLX(p.enable_cplx),
      enableSPP(p.enable_spp)
{
    assert(bop);
    assert(isPowerOf2(region_size));

    ipcp->rrf = &this->pfBlockLRUFilter;

    DPRINTF(SMSPrefetcher, "SMS: region_size: %d region_blocks: %d\n",
            region_size, region_blocks);
}

void
SMSPrefetcher::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late,
                                 PrefetchSourceType pf_source)
{
    bool can_prefetch = !pfi.isWrite() && pfi.hasPC();
    if (!can_prefetch) {
        return;
    }

    Addr pc = pfi.getPC();
    Addr vaddr = pfi.getAddr();
    Addr block_addr = blockAddress(vaddr);

    DPRINTF(SMSPrefetcher, "blk addr: %lx, prefetch source: %i, miss: %i, late: %i\n", block_addr, pf_source,
            pfi.isCacheMiss(), late);

    if (!pfi.isCacheMiss()) {
        assert(pf_source != PrefetchSourceType::PF_NONE);
    }



    // Addr region_addr = regionAddress(vaddr);
    Addr region_offset = regionOffset(vaddr);
    bool is_active_page = false;
    bool enter_new_region = false;
    ACTEntry *act_match_entry = actLookup(pfi, is_active_page, enter_new_region);
    if (act_match_entry) {
        bool decr = act_match_entry->decr_mode;
        bool is_cross_region_match = act_match_entry->access_cnt == 0;
        if (is_cross_region_match) {
            act_match_entry->access_cnt = 1;
        }
        DPRINTF(SMSPrefetcher,
                "ACT hit or match: pc:%x addr: %x offset: %d active: %d decr: "
                "%d\n",
                pc, vaddr, region_offset, is_active_page, decr);
        if (is_active_page) {
            // active page
            Addr pf_tgt_addr = decr ? block_addr - act_match_entry->depth * blkSize
                                    : block_addr + act_match_entry->depth * blkSize;  // depth here?
            Addr pf_tgt_region = regionAddress(pf_tgt_addr);
            Addr pf_tgt_offset = regionOffset(pf_tgt_addr);
            DPRINTF(SMSPrefetcher, "tgt addr: %x, offset: %d, current depth: %u, page: %lx\n", pf_tgt_addr,
                    pf_tgt_offset, act_match_entry->depth, pf_tgt_region);
            if (decr) {
                // for (int i = (int)region_blocks - 1; i >= pf_tgt_offset && i >= 0; i--) {
                for (int i = region_blocks - 1; i >= 0; i--) {
                    Addr cur = pf_tgt_region * region_size + i * blkSize;
                    sendPFWithFilter(cur, addresses, i, PrefetchSourceType::SStream);
                    DPRINTF(SMSPrefetcher, "pf addr: %x [%d]\n", cur, i);
                    fatal_if(i < 0, "i < 0\n");
                }
            } else {
                // for (int i = std::max(1, ((int) pf_tgt_offset) - 4); i <= pf_tgt_offset; i++) {
                for (int i = 0; i < region_blocks; i++) {
                    Addr cur = pf_tgt_region * region_size + i * blkSize;
                    sendPFWithFilter(cur, addresses, region_blocks - i, PrefetchSourceType::SStream);
                    DPRINTF(SMSPrefetcher, "pf addr: %x [%d]\n", cur, i);
                }
            }
            pfPageLRUFilter.insert(pf_tgt_region, 0);
        }
    }

    if (pf_source == PrefetchSourceType::SStream || act_match_entry) {
        auto it = act.begin();
        while (it != act.end()) {
            ACTEntry *it_entry = &(*it);
            if (late) {
                it_entry->lateConf += 3;
                if (it_entry->lateConf.isSaturated()) {
                    it_entry->depth++;
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
            DPRINTF(SMSPrefetcher, "act entry %lx, late or hit, now depth: %d, lateConf: %d\n",
                    it_entry->getTag(), it_entry->depth, (int)it_entry->lateConf);
        }
    }

    if (enableCPLX) {
        ipcp->doLookup(pfi, pf_source);
    }

    if (pfi.isCacheMiss() || pf_source != PrefetchSourceType::SStream) {

        bool use_bop = pf_source == PrefetchSourceType::HWP_BOP || pf_source == PrefetchSourceType::IPCP_CPLX ||
                       pfi.isCacheMiss();
        if (use_bop) {
            DPRINTF(SMSPrefetcher, "Do BOP traing/prefetching...\n");
            size_t old_addr_size = addresses.size();
            bop->calculatePrefetch(pfi, addresses, late && pf_source == PrefetchSourceType::HWP_BOP);
            bool covered_by_bop;
            if (addresses.size() > old_addr_size) {
                // BOP hit
                AddrPriority addr = addresses.back();
                addresses.pop_back();
                // Filter
                sendPFWithFilter(addr.addr, addresses, addr.priority, PrefetchSourceType::HWP_BOP);
                covered_by_bop = true;
            }
        }

        bool use_stride = pfi.isCacheMiss() || pf_source == PrefetchSourceType::SStride ||
                          pf_source == PrefetchSourceType::HWP_BOP || pf_source == PrefetchSourceType::SPht ||
                          pf_source == PrefetchSourceType::IPCP_CPLX;
        Addr stride_pf_addr = 0;
        bool covered_by_stride = false;
        if (use_stride) {
            DPRINTF(SMSPrefetcher, "Do stride lookup...\n");
            covered_by_stride = strideLookup(pfi, addresses, late, stride_pf_addr, pf_source, enter_new_region);
        }

        bool use_pht = pfi.isCacheMiss() || pf_source == PrefetchSourceType::SStride ||
                       pf_source == PrefetchSourceType::HWP_BOP || pf_source == PrefetchSourceType::SPht ||
                       pf_source == PrefetchSourceType::IPCP_CPLX || pf_source == PrefetchSourceType::SPP;
        bool trigger_pht = false;
        // stride_pf_addr = 0;
        if (use_pht) {
            DPRINTF(SMSPrefetcher, "Do PHT lookup...\n");
            trigger_pht = phtLookup(pfi, addresses, late && pf_source == PrefetchSourceType::SPht, stride_pf_addr);
        }

        bool use_cplx = enableCPLX && true;
        if (use_cplx) {
            Addr cplx_best_offset = 0;
            bool send_cplx_pf = ipcp->doPrefetch(addresses, cplx_best_offset);

            if (send_cplx_pf && cplx_best_offset != 0) {
                bop->tryAddOffset(cplx_best_offset, late);
            }
        }

        bool use_spp = enableSPP && true;
        if (use_spp) {
            int32_t spp_best_offset = 0;
            bool coverd_by_spp = spp->calculatePrefetch(pfi, addresses, pfBlockLRUFilter, spp_best_offset);
            if (coverd_by_spp && spp_best_offset != 0) {
                // TODO: Let BOP to adjust depth by itself
                bop->tryAddOffset(spp_best_offset, late);
            }
        }
    }
}

SMSPrefetcher::ACTEntry *
SMSPrefetcher::actLookup(const PrefetchInfo &pfi, bool &in_active_page, bool &alloc_new_region)
{
    Addr pc = pfi.getPC();
    Addr vaddr = pfi.getAddr();
    Addr region_addr = regionAddress(vaddr);
    Addr region_start = regionAddress(vaddr) * region_size;
    Addr region_offset = regionOffset(vaddr);
    bool secure = pfi.isSecure();

    ACTEntry *entry = act.findEntry(region_addr, secure);
    if (entry) {
        // act hit
        act.accessEntry(entry);
        in_active_page = entry->in_active_page();
        uint64_t region_bit_accessed = 1 << region_offset;
        if (!(entry->region_bits & region_bit_accessed)) {
            entry->access_cnt += 1;
        }
        entry->region_bits |= region_bit_accessed;
        // print bits
        DPRINTF(SMSPrefetcher, "Access region %lx, after access bit %lu, new act entry bits:\n", region_start,
                region_offset);
        for (uint8_t i = 0; i < region_blocks; i++) {
            DPRINTFR(SMSPrefetcher, "%lu ", (entry->region_bits >> i) & 1);
        }
        DPRINTFR(SMSPrefetcher, "\n");
        return entry;
    }

    alloc_new_region = true;

    ACTEntry *old_entry = act.findEntry(region_addr - 1, secure);
    if (old_entry) {
        in_active_page = old_entry->in_active_page();
        // act miss, but cur_region - 1 = entry_region, => cur_region =
        // entry_region + 1
        entry = act.findVictim(0);
        // evict victim entry to pht
        updatePht(entry, region_start);
        // alloc new act entry
        entry->pc = pc;
        entry->regionAddr = region_start;
        entry->is_secure = secure;
        entry->decr_mode = false;
        entry->region_bits = 1 << region_offset;
        entry->access_cnt = 0;
        entry->region_offset = region_offset;
        entry->lateConf = old_entry->lateConf;
        entry->depth = old_entry->depth;
        DPRINTF(SMSPrefetcher, "act miss, but cur_region - 1 = entry_region, copy depth = %u, lateConf = %i\n",
                entry->depth, (int) entry->lateConf);
        act.insertEntry(region_addr, secure, entry);
        return entry;
    }

    old_entry = act.findEntry(region_addr + 1, secure);
    if (old_entry) {
        in_active_page = old_entry->in_active_page();
        // act miss, but cur_region + 1 = entry_region, => cur_region =
        // entry_region - 1
        entry = act.findVictim(0);
        // evict victim entry to pht
        updatePht(entry, region_start);
        // alloc new act entry
        entry->pc = pc;
        entry->regionAddr = region_start;
        entry->is_secure = secure;
        entry->decr_mode = true;
        entry->region_bits = 1 << region_offset;
        entry->access_cnt = 0;
        entry->region_offset = region_offset;
        entry->lateConf = old_entry->lateConf;
        entry->depth = old_entry->depth;
        DPRINTF(SMSPrefetcher, "act miss, but cur_region + 1 = entry_region, copy depth = %u, lateConf = %i\n",
                entry->depth, (int) entry->lateConf);
        act.insertEntry(region_addr, secure, entry);
        return entry;
    }

    // no matched entry, alloc new entry
    entry = act.findVictim(0);
    updatePht(entry, region_start);
    entry->pc = pc;
    entry->regionAddr = region_start;
    entry->is_secure = secure;
    entry->decr_mode = false;
    entry->region_bits = 1 << region_offset;
    entry->access_cnt = 1;
    entry->region_offset = region_offset;
    act.insertEntry(region_addr, secure, entry);
    return nullptr;
}

bool
SMSPrefetcher::strideLookup(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late, Addr &stride_pf,
                            PrefetchSourceType last_pf_source, bool enter_new_region)
{
    Addr lookupAddr = pfi.getAddr();
    StrideEntry *entry = stride.findEntry(pfi.getPC(), pfi.isSecure());
    // TODO: add DPRINFT for stride
    DPRINTF(SMSPrefetcher, "Stride lookup: pc:%x addr: %x\n", pfi.getPC(),
            lookupAddr);
    bool should_cover = false;
    if (entry) {
        stride.accessEntry(entry);
        int64_t new_stride = lookupAddr - entry->last_addr;
        if (new_stride == 0) {
            DPRINTF(SMSPrefetcher, "Stride = 0, ignore redundant req\n");
            return false;
        }
        bool stride_match = new_stride == entry->stride || (entry->stride > 64 && new_stride % entry->stride == 0);
        DPRINTF(SMSPrefetcher, "Stride hit, with stride: %ld(%lx), old stride: %ld(%lx)\n", new_stride, new_stride,
                entry->stride, entry->stride);

        if (labs(new_stride) > region_size/2) {
            entry->longStride += 3;
        } else {
            entry->longStride--;
        }

        if (entry->longStride.calcSaturation() > 0.5 && labs(new_stride) < region_size/2) {
            DPRINTF(SMSPrefetcher, "Ignore short stride %li for long stride pattern\n", new_stride);
            return false;
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
            DPRINTF(SMSPrefetcher, "Stride match, inc conf to %d, late: %i, late sat:%i, depth: %i\n",
                    (int)entry->conf, late, (uint8_t)entry->lateConf, entry->depth);
            entry->last_addr = lookupAddr;

        } else if (labs(entry->stride) > 64L && labs(new_stride) < 64L) {
            // different stride, but in the same cache line
            DPRINTF(SMSPrefetcher, "Stride unmatch, but access goes to the same line, ignore\n");

        } else {
            entry->conf--;
            entry->last_addr = lookupAddr;
            DPRINTF(SMSPrefetcher, "Stride unmatch, dec conf to %d\n", (int) entry->conf);
            if ((int) entry->conf == 0) {
                DPRINTF(SMSPrefetcher, "Stride conf = 0, reset stride to %ld\n", new_stride);
                entry->stride = new_stride;
                entry->depth = 1;
                entry->lateConf.reset();
            }
        }
        if (entry->conf >= 2) {
            // if miss send 1*stride ~ depth*stride, else send depth*stride
            unsigned start_depth = pfi.isCacheMiss() ? std::max(1, (entry->depth - 4)) : entry->depth;
            Addr pf_addr = 0;
            for (unsigned i = start_depth; i <= entry->depth; i++) {
                pf_addr = lookupAddr + entry->stride * i;
                DPRINTF(SMSPrefetcher, "Stride conf >= 2, send pf: %x with depth %i\n", pf_addr, i);
                sendPFWithFilter(pf_addr, addresses, 0, PrefetchSourceType::SStride);
            }
            if (!pfi.isCacheMiss()) {
                stride_pf = pf_addr;
            }
            should_cover = true;
        }
    } else {
        DPRINTF(SMSPrefetcher, "Stride miss, insert it\n");
        entry = stride.findVictim(0);
        DPRINTF(SMSPrefetcher, "Stride found victim pc = %x, stride = %i\n", entry->pc, entry->stride);
        if (entry->conf >= 2 && entry->stride > 1024) { // > 1k
            DPRINTF(SMSPrefetcher, "Stride Evicting a useful stride, send it to BOP with offset %i\n",
                    entry->stride / 64);
            bop->tryAddOffset(entry->stride / 64);
        }
        entry->conf.reset();
        entry->last_addr = lookupAddr;
        entry->stride = 0;
        entry->depth = 1;
        entry->lateConf.reset();
        entry->pc = pfi.getPC();
        DPRINTF(SMSPrefetcher, "Stride miss, insert with stride 0\n");
        stride.insertEntry(pfi.getPC(), pfi.isSecure(), entry);
    }
    periodStrideDepthDown();
    return should_cover;
}

void
SMSPrefetcher::periodStrideDepthDown()
{
    if (depthDownCounter < depthDownPeriod) {
        depthDownCounter++;
    } else {
        for (StrideEntry &entry : stride) {
            if (entry.conf >= 2) {
                entry.depth = std::max(entry.depth - 1, 1);
            }
        }
        depthDownCounter = 0;
    }
}

void
SMSPrefetcher::updatePht(SMSPrefetcher::ACTEntry *act_entry, Addr current_region_addr)
{
    if (popCount(act_entry->region_bits) <= 1) {
        return;
    }
    PhtEntry *pht_entry = pht.findEntry(phtHash(act_entry->pc), act_entry->is_secure);
    bool is_update = pht_entry != nullptr;
    if (!pht_entry) {
        pht_entry = pht.findVictim(phtHash(act_entry->pc));
        DPRINTF(SMSPrefetcher, "Evict PHT entry for PC %lx\n", pht_entry->pc);
        for (uint8_t i = 0; i < 2 * (region_blocks - 1); i++) {
            pht_entry->hist[i].reset();
        }
        pht_entry->pc = act_entry->pc;
    }
    pht.accessEntry(pht_entry);
    Addr region_offset = act_entry->region_offset;
    // incr part
    for (uint8_t i = region_offset + 1, j = 0; i < region_blocks; i++, j++) {
        uint8_t hist_idx = j + (region_blocks - 1);
        bool accessed = (act_entry->region_bits >> i) & 1;
        if (accessed) {
            DPRINTF(SMSPrefetcher, "Inc conf for region offset: %d, hist_idx: %d\n", i, hist_idx);
            pht_entry->hist[hist_idx]++;
        } else {
            pht_entry->hist[hist_idx]--;
        }
    }
    // decr part
    for (int i = int(region_offset) - 1, j = region_blocks - 2; i >= 0;
         i--, j--) {
        bool accessed = (act_entry->region_bits >> i) & 1;
        if (accessed) {
            DPRINTF(SMSPrefetcher, "Inc conf for region offset: %d, hist_idx: %d\n", i, j);
            pht_entry->hist[j]++;
        } else {
            pht_entry->hist[j]--;
        }
    }
    DPRINTF(SMSPrefetcher, "Evict ACT region: %lx, offset: %lx, evicted by region %lx\n", act_entry->regionAddr,
            act_entry->region_offset, current_region_addr);
    if (!is_update) {
        DPRINTF(SMSPrefetcher, "Insert SMS PHT entry for PC %lx\n", act_entry->pc);
        pht.insertEntry(phtHash(act_entry->pc), act_entry->is_secure, pht_entry);
    } else {
        DPRINTF(SMSPrefetcher, "Update SMS PHT entry for PC %lx, after update:\n", act_entry->pc);
    }

    for (uint8_t i = 0; i < 2 * (region_blocks - 1); i++) {
        DPRINTFR(SMSPrefetcher, "%.2f ", pht_entry->hist[i].calcSaturation());
        if (i == region_blocks - 1) {
            DPRINTFR(SMSPrefetcher, "| ");
        }
    }
    DPRINTFR(SMSPrefetcher, "\n");
}
bool
SMSPrefetcher::phtLookup(const Base::PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late,
                         Addr look_ahead_addr)
{
    Addr pc = pfi.getPC();
    Addr vaddr = look_ahead_addr ? look_ahead_addr : pfi.getAddr();
    Addr blk_addr = blockAddress(vaddr);
    // Addr region_addr = regionAddress(vaddr);
    Addr region_offset = regionOffset(vaddr);
    bool secure = pfi.isSecure();
    PhtEntry *pht_entry = pht.findEntry(phtHash(pc), secure);
    bool found = false;
    if (pht_entry) {
        pht.accessEntry(pht_entry);
        DPRINTF(SMSPrefetcher, "Pht lookup hit: pc: %x, vaddr: %x (%s), offset: %x, late: %i\n", pc, vaddr,
                look_ahead_addr ? "ahead" : "current", region_offset, late);
        int priority = 2 * (region_blocks - 1);
        // find incr pattern
        for (uint8_t i = 0; i < region_blocks - 1; i++) {
            if (pht_entry->hist[i + region_blocks - 1].calcSaturation() > 0.5) {
                Addr pf_tgt_addr = blk_addr + (i + 1) * blkSize;
                sendPFWithFilter(pf_tgt_addr, addresses, priority--, PrefetchSourceType::SPht);
                found = true;
            }
        }
        for (int i = region_blocks - 2, j = 1; i >= 0; i--, j++) {
            if (pht_entry->hist[i].calcSaturation() > 0.5) {
                Addr pf_tgt_addr = blk_addr - j * blkSize;
                sendPFWithFilter(pf_tgt_addr, addresses, priority--, PrefetchSourceType::SPht);
                found = true;
            }
        }
        DPRINTF(SMSPrefetcher, "pht entry pattern:\n");
        for (uint8_t i = 0; i < 2 * (region_blocks - 1); i++) {
            DPRINTFR(SMSPrefetcher, "%.2f ", pht_entry->hist[i].calcSaturation());
            if (i == region_blocks - 1) {
                DPRINTFR(SMSPrefetcher, "| ");
            }
        }
        DPRINTFR(SMSPrefetcher, "\n");

        if (late) {
            int period = calcPeriod(pht_entry->hist, late);
        }
    }
    return found;
}

int
SMSPrefetcher::calcPeriod(const std::vector<SatCounter8> &bit_vec, bool late)
{
    std::vector<int> bit_vec_full(2 * (region_blocks - 1) + 1);
    // copy bit_vec to bit_vec_full, with mid point = 1
    for (int i = 0; i < region_blocks - 1; i++) {
        bit_vec_full.at(i) = bit_vec.at(i).calcSaturation() > 0.5;
    }
    bit_vec_full[region_blocks - 1] = 1;
    for (int i = region_blocks, j = region_blocks - 1; i < 2 * (region_blocks - 1);
         i++, j++) {
        bit_vec_full.at(i) = bit_vec.at(j).calcSaturation() > 0.5;
    }

    DPRINTF(SMSPrefetcher, "bit_vec_full: ");
    for (int i = 0; i < 2 * (region_blocks - 1) + 1; i++) {
        DPRINTFR(SMSPrefetcher, "%i ", bit_vec_full[i]);
    }
    DPRINTFR(SMSPrefetcher, "\n");

    int max_dot_prod = 0;
    int max_shamt = -1;
    for (int shamt = 2; shamt < 2 * (region_blocks - 1) + 1; shamt++) {
        int dot_prod = 0;
        for (int i = 0; i < 2 * (region_blocks - 1) + 1; i++) {
            if (i + shamt < 2 * (region_blocks - 1) + 1) {
                dot_prod += bit_vec_full[i] * bit_vec_full[i + shamt];
            }
        }
        if (dot_prod >= max_dot_prod) {
            max_dot_prod = dot_prod;
            max_shamt = shamt;
        }
    }
    DPRINTF(SMSPrefetcher, "max_dot_prod: %i, max_shamt: %i\n", max_dot_prod,
            max_shamt);
    if (max_dot_prod > 0 && max_shamt > 3) {
        bop->tryAddOffset(max_shamt, late);
    }
    return max_shamt;
}

bool
SMSPrefetcher::sendPFWithFilter(Addr addr, std::vector<AddrPriority> &addresses, int prio, PrefetchSourceType src)
{
    if (pfPageLRUFilter.contains(regionAddress(addr))) {
        DPRINTF(SMSPrefetcher, "Skip recently prefetched page: %lx\n", regionAddress(addr));
        return false;
    } else if (pfBlockLRUFilter.contains(addr)) {
        DPRINTF(SMSPrefetcher, "Skip recently prefetched: %lx\n", addr);
        return false;
    } else {
        DPRINTF(SMSPrefetcher, "Send pf: %lx\n", addr);
        pfBlockLRUFilter.insert(addr, 0);
        addresses.push_back(AddrPriority(addr, prio, src));
        return true;
    }
}

void
SMSPrefetcher::notifyFill(const PacketPtr &pkt)
{
    bop->notifyFill(pkt);
}


}  // prefetch
}  // gem5
