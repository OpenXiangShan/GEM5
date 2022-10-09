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
      filter_table(p.filter_entries, p.filter_entries,
                   p.filter_indexing_policy, p.filter_replacement_policy),
      act(p.act_entries, p.act_entries, p.act_indexing_policy,
          p.act_replacement_policy, ACTEntry(SatCounter8(2, 1))),
      pht(p.pht_assoc, p.pht_entries, p.pht_indexing_policy,
          p.pht_replacement_policy, PhtEntry(region_blocks, SatCounter8(2, 0)))
{
    assert(isPowerOf2(region_size));
    DPRINTF(SMSPrefetcher, "SMS: region_size: %d region_blocks: %d\n",
            region_size, region_blocks);
}

void
SMSPrefetcher::calculatePrefetch(const PrefetchInfo &pfi,
                                 std::vector<AddrPriority> &addresses)
{

    bool can_prefetch = !pfi.isWrite() && pfi.hasPC();
    if (!can_prefetch) {
        return;
    }

    Addr pc = pfi.getPC();
    Addr vaddr = pfi.getAddr();
    Addr region_addr = regionAddress(vaddr);
    Addr region_offset = regionOffset(vaddr);

    ACTEntry *act_match_entry = actLookup(pfi);
    if (act_match_entry) {
        DPRINTF(SMSPrefetcher, "ACT hit or match: pc:%x addr: %x offset: %d\n",
                pc, vaddr, region_offset);
        bool decr = act_match_entry->decr_counter.calcSaturation() > 0.5;
        uint32_t priority = decr ? 0 : region_blocks - 1;
        if (act_match_entry->access_cnt == 1 ||
            act_match_entry->access_cnt > (region_blocks / 2)) {
            // active page
            Addr pf_region_addr = decr ? region_addr - 1 : region_addr + 1;
            for (uint8_t i = 0; i < region_blocks; i++) {
                if (i == region_offset)
                    continue;
                Addr pf_addr = pf_region_addr * region_size + i * blkSize;
                addresses.push_back(AddrPriority(pf_addr, priority));
                DPRINTF(SMSPrefetcher, "Page mode fetching addr: %x (%d)\n",
                        pf_addr, i);
                if (decr) {
                    priority++;
                } else {
                    priority--;
                }
            }
        } else {
            // TODO: add another prefetcher?
        }
    } else {
        FilterTableEntry *filter_entry = filterLookup(pfi);
        if (filter_entry) {
            DPRINTF(SMSPrefetcher,
                    "Filter second hit: pc:%x addr: %x offset: %d\n", pc,
                    vaddr, region_offset);
            // second hit in filter, move filter entry to act
            // 1. evict act entry
            ACTEntry *act_entry = act.findVictim(0);
            updatePht(act_entry);
            // 2. insert filter entry to act
            act_entry->pc = filter_entry->pc;
            act_entry->is_secure = filter_entry->is_secure;
            act_entry->region_bits =
                (1 << region_offset) | (1 << filter_entry->region_offset);
            act_entry->access_cnt = 2;
            if (region_offset > filter_entry->region_offset) {
                // not decr mode
                act_entry->decr_counter.reset();
                act_entry->decr_counter--;
            } else {
                // in decr mode
                act_entry->decr_counter.reset();
                act_entry->decr_counter++;
            }
            act.insertEntry(region_addr, filter_entry->is_secure, act_entry);
            filter_table.invalidate(filter_entry);
        }
        // check pht, generate prefetch if possible
        phtLookup(pfi, addresses);
        // TODO: add another prefetcher?
    }
}


SMSPrefetcher::FilterTableEntry *
SMSPrefetcher::filterLookup(const Base::PrefetchInfo &pfi)
{
    Addr pc = pfi.getPC();
    Addr vaddr = pfi.getAddr();
    Addr region_addr = regionAddress(vaddr);
    Addr region_offset = regionOffset(vaddr);

    FilterTableEntry *entry =
        filter_table.findEntry(region_addr, pfi.isSecure());
    if (entry) {
        // second access to a region
        return entry;
    } else {
        // first access to a region
        // insert it to filter table
        entry = filter_table.findVictim(0);
        entry->pc = pc;
        entry->is_secure = pfi.isSecure();
        entry->region_offset = region_offset;
        filter_table.insertEntry(region_addr, pfi.isSecure(), entry);
        return nullptr;
    }
}

SMSPrefetcher::ACTEntry *
SMSPrefetcher::actLookup(const PrefetchInfo &pfi)
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
        uint64_t region_bit_accessed = 1 << region_offset;
        if (region_bit_accessed > entry->region_bits) {
            entry->decr_counter--;
        } else if (region_bit_accessed < entry->region_bits) {
            entry->decr_counter++;
        }
        entry->region_bits |= region_bit_accessed;
        entry->access_cnt += 1;
        return entry;
    }

    entry = act.findEntry(region_addr - 1, secure);
    if (entry) {
        act.accessEntry(entry);
        // act miss, but cur_region - 1 = entry_region, => cur_region =
        // entry_region + 1
        entry = act.findVictim(0);
        // evict victim entry to pht
        updatePht(entry);
        // alloc new act entry
        entry->pc = pc;
        entry->is_secure = secure;
        entry->decr_counter.reset();
        entry->region_bits = 1 << region_offset;
        entry->access_cnt = 1;
        act.insertEntry(region_addr, secure, entry);
        return entry;
    }

    entry = act.findEntry(region_addr + 1, secure);
    if (entry) {
        act.accessEntry(entry);
        // act miss, but cur_region + 1 = entry_region, => cur_region =
        // entry_region - 1
        entry = act.findVictim(0);
        // evict victim entry to pht
        updatePht(entry);
        // alloc new act entry
        entry->pc = pc;
        entry->is_secure = secure;
        entry->decr_counter.reset();
        entry->decr_counter++;
        entry->region_bits = 1 << region_offset;
        entry->access_cnt = 1;
        act.insertEntry(region_addr, secure, entry);
        return entry;
    }
    return nullptr;
}
void
SMSPrefetcher::updatePht(SMSPrefetcher::ACTEntry *act_entry)
{
    if (!act_entry->region_bits) {
        return;
    }
    PhtEntry *pht_entry = pht.findEntry(act_entry->pc, act_entry->is_secure);
    if (pht_entry) {
        pht.accessEntry(pht_entry);
        DPRINTF(SMSPrefetcher, "Pht update hit: pc: %x region_bits: %x\n",
                act_entry->pc, act_entry->region_bits);
        // pht hit, update it
        for (uint8_t i = 0; i < region_blocks; i++) {
            bool accessed = (act_entry->region_bits >> i) & 1;
            if (accessed) {
                pht_entry->hist[i]++;
            } else {
                pht_entry->hist[i]--;
            }
        }
    } else {
        // pht miss, evict old entry
        pht_entry = pht.findVictim(act_entry->pc);
        pht_entry->decr_mode = act_entry->decr_counter.calcSaturation() > 0.5;
        for (uint8_t i = 0; i < region_blocks; i++) {
            bool accessed = (act_entry->region_bits >> i) & 1;
            if (accessed) {
                pht_entry->hist[i].reset();
                pht_entry->hist[i]++;
            } else {
                pht_entry->hist[i].reset();
            }
        }
        DPRINTF(SMSPrefetcher, "Pht update miss: pc: %x region_bits: %x\n",
                act_entry->pc, act_entry->region_bits);
        pht.insertEntry(act_entry->pc, act_entry->is_secure, pht_entry);
    }
}
void
SMSPrefetcher::phtLookup(const Base::PrefetchInfo &pfi,
                         std::vector<AddrPriority> &addresses)
{
    Addr pc = pfi.getPC();
    Addr vaddr = pfi.getAddr();
    Addr region_addr = regionAddress(vaddr);
    Addr region_offset = regionOffset(vaddr);
    bool secure = pfi.isSecure();
    PhtEntry *pht_entry = pht.findEntry(pc, secure);
    if (pht_entry) {
        pht.accessEntry(pht_entry);
        DPRINTF(SMSPrefetcher, "Pht lookup hit: pc: %x, vaddr: %x\n", pc,
                vaddr);
        bool decr = pht_entry->decr_mode;
        uint32_t priority = decr ? 0 : region_blocks - 1;
        for (uint8_t i = 0; i < region_blocks; i++) {
            if (i == region_offset)
                continue;
            if (pht_entry->hist[i].calcSaturation() > 0.5) {
                Addr pf_addr = region_addr * region_size + i * blkSize;
                DPRINTF(SMSPrefetcher, "Pht fetching addr: %x (%d)\n", pf_addr,
                        i);
                addresses.push_back(AddrPriority(pf_addr, priority));
                if (decr) {
                    priority++;
                } else {
                    priority--;
                }
            }
        }
    }
}

}  // prefetch
}  // gem5
