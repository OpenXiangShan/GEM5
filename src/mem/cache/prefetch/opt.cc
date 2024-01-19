#include "mem/cache/prefetch/opt.hh"

#include "debug/OptPrefetcher.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"

namespace gem5
{
namespace prefetch
{
OptPrefetcher::OptPrefetcher(const OptPrefetcherParams &p)
    : Queued(p),regionSize64(p.region_size_64),
          regionBlks64(p.region_size_64 / p.block_size),optPFLevel(p.opt_pf_level),
          act_64(p.act_64_entries, p.act_64_entries, p.act_64_indexing_policy,
          p.act_64_replacement_policy,ACT64Entry()),
          opt(p.opt_entries,p.opt_entries,p.opt_indexing_policy,
          p.opt_replacement_policy,OptEntry(2*(64-1),SatCounter8(3,0)))
{
}
void
OptPrefetcher::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool &is_first_64)
{
    Addr pc = pfi.getPC();
    Addr vaddr = pfi.getAddr();
    Addr region_addr_64 = regionAddress_64(vaddr);
    Addr region_start = regionAddress_64(vaddr) * regionSize64;
    Addr region_offset_64 = regionOffset_64(vaddr);
    bool secure = pfi.isSecure();
    bool re_act_mode = false;

    ACT64Entry *entry = act_64.findEntry(region_addr_64, secure);
    if (entry) {
        // act hit
        if (region_offset_64 == entry->region_offset_64) {
            is_first_64 = true;
        } else {
            is_first_64 = false;
        }
        is_first_64 = false;
        act_64.accessEntry(entry);
        uint64_t region_bit_accessed_64 = 1UL << region_offset_64;
        if (!(entry->region_bits_64 & region_bit_accessed_64)) {
            entry->access_cnt += 1;
        }
        updateOpt(entry, region_start, region_bit_accessed_64);
        entry->region_bits_64 |= region_bit_accessed_64;

    } else {
        bool found = false;
        bool forward = true;
        is_first_64 = true;

        ACT64Entry *old_neighbor_entry = act_64.findEntry(region_addr_64 - 1, secure);
        if (old_neighbor_entry) {
            found = true;
            forward = true;
        } else if ((old_neighbor_entry = act_64.findEntry(region_addr_64 + 1, secure))) {
            found = true;
            forward = false;
        }

        entry = act_64.findVictim(0);
        entry->pc = pc;
        entry->is_secure = secure;
        entry->decr_mode = !forward;
        entry->regionAddr = region_start;
        entry->region_offset_64 = region_offset_64;
        entry->region_bits_64 = 1UL << region_offset_64;
        entry->access_cnt = 1;
        act_64.insertEntry(region_addr_64, secure, entry);

        // print bits
        DPRINTF(OptPrefetcher, "Access new region_64 %lx, after access bit %lu, new act entry bits:\n", region_start,
                region_offset_64);
        for (uint8_t i = 0; i < regionBlks64; i++) {
            DPRINTFR(OptPrefetcher, "%lu ", (entry->region_bits_64 >> i) & 1);
        }
        DPRINTFR(OptPrefetcher, "\n");
    }
}
void
OptPrefetcher::updateOpt(OptPrefetcher::ACT64Entry *act_64_entry, Addr current_region_addr,
                         Addr region_bit_accessed_64)
{
    if (popCount(act_64_entry->region_bits_64) <= 1)
        return;
    Addr region_offset = act_64_entry->region_offset_64;
    bool secure = act_64_entry->is_secure;
    uint64_t region_bits = act_64_entry->region_bits_64;
    Addr region_addr_64 = regionAddress_64(act_64_entry->regionAddr);
    Addr region_tag = region_offset;
    OptEntry *opt_entry = opt.findEntry(region_tag, act_64_entry->is_secure);

    bool is_update = opt_entry != nullptr;
    if (!opt_entry) {
        opt_entry = opt.findVictim(region_tag);
        for (uint8_t i = 0; i < 2 * (OptLines - 1); i++) {
            opt_entry->hist[i].reset();
        }
        opt_entry->offset = region_offset;
    }
    opt.accessEntry(opt_entry);

    bool incr_full = false;
    Addr front_all_num = 0;
    Addr back_all_num = 0;
    opt_entry->cof_4 = 0;
    opt_entry->cof_3 = 0;
    opt_entry->cof_2 = 0;
    opt_entry->cof_1 = 0;

    for (int i = region_offset + 1, j = 0; i < OptLines - 1; i++, j++) {
        uint8_t hist_idx = j + (OptLines - 1);
        bool accessed_64 = (region_bit_accessed_64 >> i) & 1;
        if (accessed_64) {
            opt_entry->hist.at(hist_idx) += 1;
            if (opt_entry->hist[hist_idx].rawCounter() > 4)
                incr_full = true;
        }
    }

    for (int j = 0; j < OptLines - 1; j++) {
        uint8_t hist_idx = j + (OptLines - 1);
        if (incr_full) {
            opt_entry->hist[hist_idx] /= 2;
        }
        cofNum(opt_entry, hist_idx);
        front_all_num = front_all_num + opt_entry->hist[hist_idx].rawCounter();
    }

    bool decr_full = false;
    for (int i = int(region_offset) - 1, j = 64 - 2; j >= 0; i--, j--) {
        if (i >= 0) {
            bool accessed_64 = (region_bit_accessed_64 >> i) & 1;
            if (accessed_64) {
                opt_entry->hist.at(j) += 1;
                if (opt_entry->hist[j].rawCounter() > 4)
                    decr_full = true;
            }
        }
    }

    for (int j = OptLines - 2; j >= 0; j--) {
        if (decr_full) {
            opt_entry->hist[j] /= 2;
        }
        cofNum(opt_entry, j);

        back_all_num = back_all_num + opt_entry->hist[j].rawCounter();
    }
    if (!is_update) {
        opt.insertEntry(region_tag, secure, opt_entry);
    } else {
    }
}
void
OptPrefetcher::cofNum(OptEntry *opt_entry, int j)
{
    if (opt_entry->hist[j].rawCounter() == 4)
        opt_entry->cof_4++;
    else if (opt_entry->hist[j].rawCounter() == 3)
        opt_entry->cof_3++;
    else if (opt_entry->hist[j].rawCounter() == 2)
        opt_entry->cof_2++;
    else if (opt_entry->hist[j].rawCounter() == 1)
        opt_entry->cof_1++;
}
bool
OptPrefetcher::optLookup(const Base::PrefetchInfo &pfi, std::vector<AddrPriority> &addresses)
{
    Addr vaddr = pfi.getAddr();
    Addr blk_addr = blockAddress(vaddr);
    Addr region_offset_64 = regionOffset_64(vaddr);
    bool secure = pfi.isSecure();
    Addr region_tag = region_offset_64;
    OptEntry *opt_entry = opt.findEntry(region_tag, secure);
    bool found = false;
    int send_num_front = 0;
    int send_num_back = 0;
    if (opt_entry) {
        opt.accessEntry(opt_entry);
        DPRINTF(OptPrefetcher, "opt lookup hit: vaddr: %x offset: %x\n", vaddr, region_offset_64);
        int con_opt = 0;
        if (opt_entry->cof_4 > 8) {
            con_opt = 4;
        } else if ((opt_entry->cof_4 + opt_entry->cof_3) > 8) {
            con_opt = 3;
        } else if ((opt_entry->cof_4 + opt_entry->cof_3 + opt_entry->cof_2) > 8) {
            con_opt = 2;
        } else {
            con_opt = 1;
        }

        int priority = 2 * (OptLines - 1);
        for (int i = 0; i < OptLines - 1; i++) {
            if (opt_entry->hist[i + OptLines - 1].rawCounter() >= con_opt) {
                Addr pf_tgt_addr = blk_addr + (i + 1) * blkSize;
                bool send_r = sendPFWithFilter(pfi, pf_tgt_addr, addresses, 1, PrefetchSourceType::SOpt, optPFLevel);
                DPRINTF(OptPrefetcher, "opt sub blk size %lx i %d pf_tgt_addr %lx region_offset %lx ca %d\n", blk_addr,
                        i, pf_tgt_addr, region_offset_64, opt_entry->hist[i + 64 - 1].rawCounter());
                found = true;
                send_num_front++;
            }
        }
        for (int i = OptLines - 2, j = 1; i >= 0; i--, j++) {
            if (opt_entry->hist[i].rawCounter() >= con_opt) {
                Addr pf_tgt_addr = blk_addr - j * blkSize;
                bool send_r = sendPFWithFilter(pfi, pf_tgt_addr, addresses, 1, PrefetchSourceType::SOpt, optPFLevel);
                DPRINTF(OptPrefetcher, "opt sub blk size %lx j %d pf_tgt_addr %lx region_offset %lx ca %d\n", blk_addr,
                        j, pf_tgt_addr, region_offset_64, opt_entry->hist[i].rawCounter());
                found = true;
                send_num_back++;
            }
        }
    }
    return found;
}
bool
OptPrefetcher::sendPFWithFilter(const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses, int prio,
                                PrefetchSourceType src, int ahead_level)
{
    if (filter->contains(addr)) {
        DPRINTF(OptPrefetcher, "Skip recently prefetched: %lx\n", addr);
        return false;
    } else {
        DPRINTF(OptPrefetcher, "Send pf: %lx\n", addr);
        filter->insert(addr, 0);
        if (ahead_level > 1) {
            assert(ahead_level == 2 || ahead_level == 3);
            addresses.back().pfahead_host = ahead_level;
            addresses.back().pfahead = true;
        } else {
            addresses.back().pfahead = false;
        }
        DPRINTF(OptPrefetcher, "Send pf: %lx, target level: %i\n", addr, ahead_level);
        return true;
    }
}




}
}
