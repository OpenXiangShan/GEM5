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
             p.stride_replacement_policy, StrideEntry(SatCounter8(2, 1))),
      pht(p.pht_assoc, p.pht_entries, p.pht_indexing_policy,
          p.pht_replacement_policy,
          PhtEntry(2 * (region_blocks - 1), SatCounter8(2, 0))),
          pfBlockLRUFilter(pfFilterSize),
      bop(dynamic_cast<BOP *>(p.bop)),
      spp(dynamic_cast<SignaturePath *>(p.spp))
{
    assert(bop);
    assert(isPowerOf2(region_size));
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
    ACTEntry *act_match_entry = actLookup(pfi, is_active_page);
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
            Addr pf_tgt_addr =
                decr ? block_addr - 32 * blkSize : block_addr + 32 * blkSize;
            Addr pf_tgt_region = regionAddress(pf_tgt_addr);
            Addr pf_tgt_offset = regionOffset(pf_tgt_addr);
            DPRINTF(SMSPrefetcher, "tgt addr: %x offset: %d\n", pf_tgt_addr,
                    pf_tgt_offset);
            if (decr) {
                for (int i = (int)region_blocks - 1; i >= pf_tgt_offset && i >= 0; i--) {
                    Addr cur = pf_tgt_region * region_size + i * blkSize;
                    sendPFWithFilter(cur, addresses, i, PrefetchSourceType::SStream);
                    DPRINTF(SMSPrefetcher, "pf addr: %x [%d]\n", cur, i);
                    fatal_if(i < 0, "i < 0\n");
                }
            } else {
                for (uint8_t i = 0; i <= pf_tgt_offset; i++) {
                    Addr cur = pf_tgt_region * region_size + i * blkSize;
                    sendPFWithFilter(cur, addresses, region_blocks - i, PrefetchSourceType::SStream);
                    DPRINTF(SMSPrefetcher, "pf addr: %x [%d]\n", cur, i);
                }
            }
        }
    }

    if (pfi.isCacheMiss() || pf_source != PrefetchSourceType::SStream) {
        bool use_bop = pf_source == PrefetchSourceType::HWP_BOP || pfi.isCacheMiss();
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

        bool use_stride =
            pf_source == PrefetchSourceType::SStride || pf_source == PrefetchSourceType::HWP_BOP || pfi.isCacheMiss();
        Addr stride_pf_addr = 0;
        bool covered_by_stride = false;
        if (use_stride) {
            DPRINTF(SMSPrefetcher, "Do stride lookup...\n");
            covered_by_stride = strideLookup(pfi, addresses, late, stride_pf_addr, pf_source);
        }

        bool use_pht = pf_source == PrefetchSourceType::SPP || pf_source == PrefetchSourceType::SPht ||
                       pf_source == PrefetchSourceType::SStride || pfi.isCacheMiss();
        bool trigger_pht = false;
        // stride_pf_addr = 0;
        if (use_pht) {
            DPRINTF(SMSPrefetcher, "Do PHT lookup...\n");
            bool trigger_pht = phtLookup(pfi, addresses, late && pf_source == PrefetchSourceType::SPht, stride_pf_addr);
        }

        bool use_spp = false;
        if (!pfi.isCacheMiss()) {
            if (pf_source == PrefetchSourceType::SPP) {
                use_spp = true;
            }
        } else {
            // cache miss
            if (late) {
                if (pf_source != PrefetchSourceType::SStride && pf_source != PrefetchSourceType::HWP_BOP) {
                    // other components cannot adjust depth
                    use_spp = true;
                }
            } else {  // no prefetch issued
                if (!covered_by_stride) {
                    use_spp = true;
                }
            }
        }
        use_spp = false;
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
SMSPrefetcher::actLookup(const PrefetchInfo &pfi, bool &in_active_page)
{
    Addr pc = pfi.getPC();
    Addr vaddr = pfi.getAddr();
    Addr region_addr = regionAddress(vaddr);
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
        return entry;
    }

    entry = act.findEntry(region_addr - 1, secure);
    if (entry) {
        in_active_page = entry->in_active_page();
        // act miss, but cur_region - 1 = entry_region, => cur_region =
        // entry_region + 1
        entry = act.findVictim(0);
        // evict victim entry to pht
        updatePht(entry);
        // alloc new act entry
        entry->pc = pc;
        entry->is_secure = secure;
        entry->decr_mode = false;
        entry->region_bits = 1 << region_offset;
        entry->access_cnt = 0;
        entry->region_offset = region_offset;
        act.insertEntry(region_addr, secure, entry);
        return entry;
    }

    entry = act.findEntry(region_addr + 1, secure);
    if (entry) {
        in_active_page = entry->in_active_page();
        // act miss, but cur_region + 1 = entry_region, => cur_region =
        // entry_region - 1
        entry = act.findVictim(0);
        // evict victim entry to pht
        updatePht(entry);
        // alloc new act entry
        entry->pc = pc;
        entry->is_secure = secure;
        entry->decr_mode = true;
        entry->region_bits = 1 << region_offset;
        entry->access_cnt = 0;
        entry->region_offset = region_offset;
        act.insertEntry(region_addr, secure, entry);
        return entry;
    }

    // no matched entry, alloc new entry
    entry = act.findVictim(0);
    updatePht(entry);
    entry->pc = pc;
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
                            PrefetchSourceType last_pf_source)
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

        } else if (entry->stride > 64 && new_stride < 64) {  // different stride, but in the same cache line
            DPRINTF(SMSPrefetcher, "Stride unmatch, but access goes to the same line, ignore\n");

        } else {
            if (entry->conf < 2) {
                entry->stride = new_stride;
                entry->depth = 1;
                entry->lateConf.reset();
            }
            entry->conf--;
            entry->last_addr = lookupAddr;
            DPRINTF(SMSPrefetcher, "Stride unmatch, dec conf to %d\n", (int) entry->conf);
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
        DPRINTF(SMSPrefetcher, "Found victim pc = %x, stride = %i\n", entry->pc, entry->stride);
        if (entry->conf >= 2 && entry->stride > 1024) { // > 1k
            DPRINTF(SMSPrefetcher, "Evicting a useful stride, send it to BOP with offset %i\n", entry->stride / 64);
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
SMSPrefetcher::updatePht(SMSPrefetcher::ACTEntry *act_entry)
{
    if (!act_entry->region_bits) {
        return;
    }
    PhtEntry *pht_entry = pht.findEntry(act_entry->pc, act_entry->is_secure);
    bool is_update = pht_entry != nullptr;
    if (!pht_entry) {
        pht_entry = pht.findVictim(act_entry->pc);
        for (uint8_t i = 0; i < 2 * (region_blocks - 1); i++) {
            pht_entry->hist[i].reset();
        }
    } else {
        pht.accessEntry(pht_entry);
    }
    Addr region_offset = act_entry->region_offset;
    // incr part
    for (uint8_t i = region_offset + 1, j = 0; i < region_blocks; i++, j++) {
        uint8_t hist_idx = j + (region_blocks - 1);
        bool accessed = (act_entry->region_bits >> i) & 1;
        if (accessed) {
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
            pht_entry->hist[j]++;
        } else {
            pht_entry->hist[j]--;
        }
    }
    if (!is_update) {
        pht.insertEntry(act_entry->pc, act_entry->is_secure, pht_entry);
    }
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
    PhtEntry *pht_entry = pht.findEntry(pc, secure);
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
    if (pfBlockLRUFilter.contains(addr)) {
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
