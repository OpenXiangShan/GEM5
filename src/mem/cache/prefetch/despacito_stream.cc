#include "mem/cache/prefetch/despacito_stream.hh"

#include "base/output.hh"
#include "debug/DespacitoStreamPrefetcher.hh"
#include "mem/cache/base.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"

namespace gem5
{
namespace prefetch
{

DespacitoStreamPrefetcher::DespacitoStreamPrefetcher(const DespacitoStreamPrefetcherParams &p)
    : Queued(p),
      sampleRate(p.sample_rate),
      minDistance(p.min_distance),
      maxDistance(p.max_distance),
      sampler(p.sampler_assoc, p.sampler_entries, p.sampler_indexing_policy, p.sampler_replacement_policy,
              SamplerEntry()),
      patterns(p.patterns_entries, p.patterns_entries, p.patterns_indexing_policy, p.patterns_replacement_policy,
               PatternEntry(SatCounter8(2, 1))),
      timestamp(0)
{
}

void
DespacitoStreamPrefetcher::updateSampler(const PrefetchInfo &pfi)
{
    Addr block_index = blockIndex(pfi.getAddr());

    SamplerEntry *sampler_entry = sampler.findEntry(block_index - 1, false);

    if (sampler_entry) {
        if (timestamp > sampler_entry->timestamp + minDistance &&
            timestamp <= sampler_entry->timestamp + maxDistance) {
            sampler_entry->touched = true;
        }
        sampler.accessEntry(sampler_entry);
    }

    if (timestamp % sampleRate == 0) {
        SamplerEntry *evict_sampler_entry = sampler.findVictim(block_index);
        updatePatternTable(evict_sampler_entry);
        evict_sampler_entry->timestamp = timestamp;
        evict_sampler_entry->address = block_index;
        evict_sampler_entry->pc = pfi.getPC();
        evict_sampler_entry->touched = false;
        sampler.insertEntry(block_index, false, evict_sampler_entry);
    }

    timestamp++;
}

void
DespacitoStreamPrefetcher::updatePatternTable(SamplerEntry *sampler_entry)
{
    if (sampler_entry->pc) {
        PatternEntry *pattern_entry = patterns.findEntry(sampler_entry->pc, false);
        if (pattern_entry) {
            patterns.accessEntry(pattern_entry);
            if (sampler_entry->touched) {
                pattern_entry->conf++;
            } else {
                pattern_entry->conf--;
            }
        } else {
            if (sampler_entry->touched) {
                pattern_entry = patterns.findVictim(sampler_entry->pc);
                pattern_entry->conf.reset();
                patterns.insertEntry(sampler_entry->pc, false, pattern_entry);
            }
        }
    }
}

void
DespacitoStreamPrefetcher::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late)
{
    if (!pfi.hasPC()) {
        return;
    }

    Addr pc = pfi.getPC();

    Addr block_addr = blockAddress(pfi.getAddr());

    PatternEntry *pattern_entry = patterns.findEntry(pc, false);

    if (pattern_entry && pattern_entry->conf.isSaturated()) {
        Addr pf_addr = block_addr + blkSize;
        sendPFWithFilter(pfi, pf_addr, addresses, 32, PrefetchSourceType::DespacitoStream);
    }

    updateSampler(pfi);

    return;
}

bool
DespacitoStreamPrefetcher::sendPFWithFilter(const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses,
                                            int prio, PrefetchSourceType src)
{
    if (archDBer && cache->level() == 1) {
        archDBer->l1PFTraceWrite(curTick(), pfi.getPC(), pfi.getAddr(), addr, src);
    }
    if (filter->contains(addr)) {
        DPRINTF(DespacitoStreamPrefetcher, "Skip recently prefetched: %lx\n", addr);
        return false;
    } else {
        DPRINTF(DespacitoStreamPrefetcher, "Send pf: %lx\n", addr);
        filter->insert(addr, 0);
        addresses.push_back(AddrPriority(addr, prio, src));
        return true;
    }
}

}
}
