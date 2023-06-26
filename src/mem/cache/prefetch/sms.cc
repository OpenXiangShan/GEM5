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
      bop(dynamic_cast<BOP *>(p.bop))
{
    assert(bop);
    assert(isPowerOf2(region_size));
    DPRINTF(SMSPrefetcher, "SMS: region_size: %d region_blocks: %d\n",
            region_size, region_blocks);
}

void
SMSPrefetcher::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late)
{

    bool can_prefetch = !pfi.isWrite() && pfi.hasPC();
    if (!can_prefetch) {
        return;
    }

    Addr pc = pfi.getPC();
    Addr vaddr = pfi.getAddr();
    Addr block_addr = blockAddress(vaddr);
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
                decr ? block_addr - 30 * blkSize : block_addr + 30 * blkSize;
            Addr pf_tgt_region = regionAddress(pf_tgt_addr);
            Addr pf_tgt_offset = regionOffset(pf_tgt_addr);
            DPRINTF(SMSPrefetcher, "tgt addr: %x offset: %d\n", pf_tgt_addr,
                    pf_tgt_offset);
            if (decr) {
                for (int i = (int)region_blocks - 1;
                     i >= pf_tgt_offset && i >= 0; i--) {
                    Addr cur = pf_tgt_region * region_size + i * blkSize;
                    sendPFWithFilter(cur, addresses, i);
                    DPRINTF(SMSPrefetcher, "pf addr: %x [%d]\n", cur, i);
                    fatal_if(i < 0, "i < 0\n");
                }
            } else {
                for (uint8_t i = 0; i <= pf_tgt_offset; i++) {
                    Addr cur = pf_tgt_region * region_size + i * blkSize;
                    sendPFWithFilter(cur, addresses, region_blocks - i);
                    DPRINTF(SMSPrefetcher, "pf addr: %x [%d]\n", cur, i);
                }
            }
        }
    }
    if (!is_active_page) {
        // bop->calculatePrefetch(pfi, addresses);
        DPRINTF(SMSPrefetcher, "Do pht lookup...\n");
        bool found_in_pht = phtLookup(pfi, addresses, late);
        if ((!found_in_pht) || (found_in_pht && pfi.isCacheMiss() && !late)) {
            DPRINTF(SMSPrefetcher, "Do stride lookup...\n");
            strideLookup(pfi, addresses, late);
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

void
SMSPrefetcher::strideLookup(const PrefetchInfo &pfi,
                            std::vector<AddrPriority> &addresses, bool late)
{
    Addr lookupAddr = pfi.getAddr();
    StrideEntry *entry = stride.findEntry(pfi.getPC(), pfi.isSecure());
    // TODO: add DPRINFT for stride
    DPRINTF(SMSPrefetcher, "Stride lookup: pc:%x addr: %x\n", pfi.getPC(),
            lookupAddr);
    if (entry) {
        stride.accessEntry(entry);
        int64_t new_stride = lookupAddr - entry->last_addr;
        if (new_stride == 0) {
            DPRINTF(SMSPrefetcher, "Stride = 0, ignore redundant req\n");
            return;
        }
        bool stride_match = new_stride == entry->stride;
        DPRINTF(SMSPrefetcher, "Stride hit, with stride: %d, old stride: %d\n", new_stride, entry->stride);
        if (stride_match) {
            entry->conf++;
            if (strideDynDepth && late) {
                entry->depth++;
            }
            DPRINTF(SMSPrefetcher, "Stride match, inc conf to %d, late: %i, depth: %i\n", (int)entry->conf, late,
                    entry->depth);
            entry->last_addr = lookupAddr;

        } else if (entry->stride > 64 && new_stride < 64) {  // different stride, but in the same cache line
            DPRINTF(SMSPrefetcher, "Stride unmatch, but access goes to the same line, ignore\n");

        } else {
            if (entry->conf < 2) {
                entry->stride = new_stride;
                entry->depth = 1;
            }
            entry->conf--;
            entry->last_addr = lookupAddr;
            DPRINTF(SMSPrefetcher, "Stride unmatch, dec conf to %d\n", (int) entry->conf);
        }
        if (entry->conf >= 2) {
            Addr pf_addr = lookupAddr + entry->stride * entry->depth;
            DPRINTF(SMSPrefetcher, "Stride conf >= 2, send pf: %x with depth %i\n", pf_addr, entry->depth);
            sendPFWithFilter(pf_addr, addresses, 0);
        }
    } else {
        DPRINTF(SMSPrefetcher, "Stride miss, insert it\n");
        entry = stride.findVictim(0);
        DPRINTF(SMSPrefetcher, "Found victim pc = %x, stride = %i\n", entry->pc, entry->stride);
        // if (entry->conf >= 1 && entry->stride > 1024) { // > 1k
        //     DPRINTF(SMSPrefetcher, "Evicting a useful stride, send it to BOP with offset %i\n", entry->stride / 64);
        //     bop->tryAddOffset(entry->stride / 64);
        // }
        entry->conf.reset();
        entry->last_addr = lookupAddr;
        entry->stride = 0;
        entry->depth = 1;
        entry->pc = pfi.getPC();
        DPRINTF(SMSPrefetcher, "Stride miss, insert with stride 0\n");
        stride.insertEntry(pfi.getPC(), pfi.isSecure(), entry);
    }
    periodStrideDepthDown();
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
SMSPrefetcher::phtLookup(const Base::PrefetchInfo &pfi,
                         std::vector<AddrPriority> &addresses, bool late)
{
    Addr pc = pfi.getPC();
    Addr vaddr = pfi.getAddr();
    Addr blk_addr = blockAddress(vaddr);
    // Addr region_addr = regionAddress(vaddr);
    Addr region_offset = regionOffset(vaddr);
    bool secure = pfi.isSecure();
    PhtEntry *pht_entry = pht.findEntry(pc, secure);
    bool found = pht_entry != nullptr;
    if (pht_entry) {
        pht.accessEntry(pht_entry);
        DPRINTF(SMSPrefetcher,
                "Pht lookup hit: pc: %x, vaddr: %x, offset: %x\n", pc, vaddr,
                region_offset);
        int priority = 2 * (region_blocks - 1);
        // find incr pattern
        for (uint8_t i = 0; i < region_blocks - 1; i++) {
            if (pht_entry->hist[i + region_blocks - 1].calcSaturation() >
                0.5) {
                Addr pf_tgt_addr = blk_addr + (i + 1) * blkSize;
                sendPFWithFilter(pf_tgt_addr, addresses, priority--);
            }
        }
        for (int i = region_blocks - 2, j = 1; i >= 0; i--, j++) {
            if (pht_entry->hist[i].calcSaturation() > 0.5) {
                Addr pf_tgt_addr = blk_addr - j * blkSize;
                sendPFWithFilter(pf_tgt_addr, addresses, priority--);
            }
        }
    }
    return found;
}

bool
SMSPrefetcher::sendPFWithFilter(Addr addr, std::vector<AddrPriority> &addresses, int prio)
{
    if (pfBlockLRUFilter.contains(addr)) {
        DPRINTF(SMSPrefetcher, "Skip recently prefetched: %lx\n", addr);
        return false;
    } else {
        DPRINTF(SMSPrefetcher, "Send pf: %lx\n", addr);
        pfBlockLRUFilter.insert(addr, 0);
        addresses.push_back(AddrPriority(addr, prio));
        return true;
    }
}

void
SMSPrefetcher::notifyFill(const PacketPtr &pkt)
{
    // bop->notifyFill(pkt);
}


}  // prefetch
}  // gem5
