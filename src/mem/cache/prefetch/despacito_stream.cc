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
      timestamp(0),
      stats(this)
{
}



void
DespacitoStreamPrefetcher::updateSampler(const PrefetchInfo &pfi)
{
    Addr block_index = blockIndex(pfi.getAddr());

    SamplerEntry *sampler_entry = sampler.findEntry(block_index - 1, false);
    stats.nlSampleTrainTimes++;
    if (sampler_entry) {
        if (timestamp > sampler_entry->timestamp + minDistance &&
            timestamp <= sampler_entry->timestamp + maxDistance) {
            sampler_entry->touched = true;
            stats.nlSampleUpdateTimes++;
        } else {
            stats.nlSampleUpdateReqHitOverBoardTimes++; // Hit but not within the update range
        }
        sampler.accessEntry(sampler_entry);
    }else{
        stats.nlSampleUpdateReqNotHitTimes++; // If not present, it means no hit
    }

    if (timestamp % sampleRate == 0) {
        SamplerEntry *evict_sampler_entry = sampler.findVictim(block_index);
        updatePatternTable(evict_sampler_entry);
        stats.nlSampleVictimTouchedTrueTimes += evict_sampler_entry->touched ? 1 : 0; // Analyze victim touched times
        stats.nlSampleVictimTouchedFalseTimes += evict_sampler_entry->touched ? 0 : 1;
            
        // if an existing entry is present in the victim slot, snapshot it
        if (evict_sampler_entry->pc) {
            stats.nlSampleReplaceTimes++;
        } else {
            // empty slot -> will be insertion
            stats.nlSampleInsertTimes++;
        }

        // now write new sampler contents into the slot
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
    stats.nlPatternTrainTimes++;
    if (sampler_entry->pc) {
    stats.nlPatternUpdateTimes++;// Here we check the PC, when findVictim returns an entry, it sets valid to false
        PatternEntry *pattern_entry = patterns.findEntry(sampler_entry->pc, false);
        if (pattern_entry) {
            patterns.accessEntry(pattern_entry);
            if (sampler_entry->touched) {
                pattern_entry->conf++;
               stats.nlPatternUpdateTouchedTrueTimes++;
            } else {
                pattern_entry->conf--;
                stats.nlPatternUpdateTouchedFalseTimes++;
            }
        } else {
            if (sampler_entry->touched) {
                pattern_entry = patterns.findVictim(sampler_entry->pc);
                bool was_valid = pattern_entry->isValid();
                pattern_entry->conf.reset();
                patterns.insertEntry(sampler_entry->pc, false, pattern_entry);
                if (was_valid)
                    stats.nlPatternReplaceTimes++;
                else
                    stats.nlPatternInsertTimes++;
            }
        }
    }
}

void
DespacitoStreamPrefetcher::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late)
{
    if (archDBer){
            archDBer->despacitoTraceWrite(curTick(), pfi.getAddr(), pfi.getPaddr(), pfi.hasPC() ? pfi.getPC() : 0,
     pfi.hasPC(), pfi.isCacheMiss(), true);
    }

    if (!pfi.hasPC()) {
        return;
    }

    Addr pc = pfi.getPC();

    Addr block_addr = blockAddress(pfi.getAddr());

    PatternEntry *pattern_entry = patterns.findEntry(pc, false);

    if (pattern_entry) {
    stats.nlPatternPcHitTimes++;
    stats.nlPatternPcHitValidEntryTimes++;
        if (pattern_entry->conf.isSaturated()){
            stats.nlPatternPcHitValidEntrySatEq3Times++;
        } else if (pattern_entry->conf.rawCounter() == 2) {
            stats.nlPatternPcHitValidEntrySatEq2Times++;
        } else if (pattern_entry->conf.rawCounter() == 1) {
            stats.nlPatternPcHitValidEntrySatEq1Times++;
        }else{
            stats.nlPatternPcHitValidEntrySatEq0Times++;
        }
    }

    if (pattern_entry && pattern_entry->conf.isSaturated()) {
        Addr pf_addr = block_addr + blkSize;
        sendPFWithFilter(pfi, pf_addr, addresses, 32, PrefetchSourceType::DespacitoStream);
    stats.nlTransmitPrefetchReqTimes++;
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
    if (archDBer){
        archDBer->despacitoTraceWrite(curTick(), addr, 0, pfi.hasPC() ? pfi.getPC() : 0,
        pfi.hasPC(), pfi.isCacheMiss(), false);
    }
    InsertPFRequestToBuffer(AddrPriority(addr, prio, src, pfi.trigger_info));
    Addr filter_key = sharedFilterKey(pfi, addr);
    if (filter->contains(filter_key)) {
        DPRINTF(DespacitoStreamPrefetcher, "Skip recently prefetched: %lx\n", addr);
        return false;
    } else {
        DPRINTF(DespacitoStreamPrefetcher, "Send pf: %lx\n", addr);
        filter->insert(filter_key, 0);
        addresses.push_back(AddrPriority(addr, prio, src));
        return true;
    }
}

DespacitoStreamPrefetcher::DespacitoStats::DespacitoStats(statistics::Group *parent)
        : statistics::Group(parent),
            /* sampler */
            ADD_STAT(nlSampleTrainTimes, statistics::units::Count::get(), "Number of times sampler was trained"),
            ADD_STAT(nlSampleInsertTimes, statistics::units::Count::get(),
             "Number of times sampler entries were inserted"),
            ADD_STAT(nlSampleReplaceTimes, statistics::units::Count::get(),
             "Number of times sampler entries were replaced"),

            ADD_STAT(nlSampleUpdateTimes, statistics::units::Count::get(),
             "Number of times sampler entries were updated"),
            ADD_STAT(nlSampleUpdateReqNotHitTimes, statistics::units::Count::get(),
            "update request not hit times"),
            ADD_STAT(nlSampleUpdateReqHitOverBoardTimes, statistics::units::Count::get(),
             "update request hit but over board times"),

            ADD_STAT(nlSampleVictimTouchedTrueTimes, statistics::units::Count::get(),
             "Number of times sample victim touched true times"),
            ADD_STAT(nlSampleVictimTouchedFalseTimes, statistics::units::Count::get(),
             "Number of times sample victim touched false times"),

            /* pattern */
            ADD_STAT(nlPatternTrainTimes, statistics::units::Count::get(),
             "Number of times pattern table was trained"),
            ADD_STAT(nlPatternReplaceTimes, statistics::units::Count::get(), "Number of patterns replaced in table"),
            ADD_STAT(nlPatternInsertTimes, statistics::units::Count::get(), "Number of patterns inserted into table"),

            ADD_STAT(nlPatternUpdateTimes, statistics::units::Count::get(),
            "Number of times a pattern's confidence changed"),
            ADD_STAT(nlPatternUpdateTouchedTrueTimes, statistics::units::Count::get(),
            "Number of times a pattern's training data was touched"),
            ADD_STAT(nlPatternUpdateTouchedFalseTimes, statistics::units::Count::get(),
            "Number of times a pattern's training data was not touched"),

            ADD_STAT(nlPatternPcHitTimes, statistics::units::Count::get(), "Number of times a pattern's pc was hit"),
            ADD_STAT(nlPatternPcHitValidEntryTimes, statistics::units::Count::get(),
            "Number of times a pattern's pc hit a valid entry"),
            ADD_STAT(nlPatternPcHitValidEntrySatEq3Times, statistics::units::Count::get(),
            "Number of times a pattern's pc hit a valid entry with Sat==3"),
            ADD_STAT(nlPatternPcHitValidEntrySatEq2Times, statistics::units::Count::get(),
            "Number of times a pattern's pc hit a valid entry with Sat==2"),
            ADD_STAT(nlPatternPcHitValidEntrySatEq1Times, statistics::units::Count::get(),
            "Number of times a pattern's pc hit a valid entry with Sat==1"),
            ADD_STAT(nlPatternPcHitValidEntrySatEq0Times, statistics::units::Count::get(),
             "Number of times a pattern's pc hit a valid entry with Sat==0"),

            /* prefetch */
            ADD_STAT(nlTransmitPrefetchReqTimes, statistics::units::Count::get(), "Number of prefetches sent")
{}

}
}
