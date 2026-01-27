#include "mem/cache/prefetch/despacito_stream.hh"

#include "base/output.hh"
#include "cpu/pred/general_arch_db.hh"
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
    , stats(this)
{
    // Enable DB tracing by default (will save to "despacito_stream.db" on exit).
    // This makes it easy to collect traces without extra Python plumbing.
    initDB(p.enable_despacito_db, "despacito_stream.db");
}

//初始化db
void
DespacitoStreamPrefetcher::initDB(bool enable, const std::string &filename)
{
    enableDB = enable;
    if (!enableDB)
        return;

    db.init_db();
    std::vector<std::pair<std::string, gem5::DataType>> fields = {
        std::make_pair(std::string("pc"), gem5::UINT64),
        std::make_pair(std::string("vaddr"), gem5::UINT64),
        std::make_pair(std::string("timeSample"), gem5::UINT64),
        std::make_pair(std::string("sampleTrainEn"), gem5::UINT64),
        std::make_pair(std::string("sampleHit"), gem5::UINT64),
        // victim (old) snapshot
        std::make_pair(std::string("victim_timestamp"), gem5::UINT64),
        std::make_pair(std::string("victim_addr"), gem5::UINT64),
        std::make_pair(std::string("victim_pc"), gem5::UINT64),
        std::make_pair(std::string("victim_touched"), gem5::UINT64),
        // inserted (new) snapshot
        std::make_pair(std::string("insert_timestamp"), gem5::UINT64),
        std::make_pair(std::string("insert_addr"), gem5::UINT64),
        std::make_pair(std::string("insert_pc"), gem5::UINT64),
        std::make_pair(std::string("insert_touched"), gem5::UINT64),
        // update (touched) snapshot
        std::make_pair(std::string("update_timestamp"), gem5::UINT64),
        std::make_pair(std::string("update_addr"), gem5::UINT64),
        std::make_pair(std::string("update_pc"), gem5::UINT64),
        std::make_pair(std::string("update_touched"), gem5::UINT64),
        // pattern snapshots
        std::make_pair(std::string("patternTrain_conf"), gem5::UINT64),
        std::make_pair(std::string("patternTrain_pc"), gem5::UINT64),
        std::make_pair(std::string("patternUpdate_conf"), gem5::UINT64),
        std::make_pair(std::string("patternUpdate_pc"), gem5::UINT64),
        std::make_pair(std::string("patternInsert_conf"), gem5::UINT64),
        std::make_pair(std::string("patternInsert_pc"), gem5::UINT64),
        std::make_pair(std::string("event"), gem5::TEXT),
    };

    despacitoTrace = db.addAndGetTrace("DESPACITOSTREAMTRACE", fields);
    despacitoTrace->init_table();

    // Save DB on simulator exit
    registerExitCallback([this, filename]() {
        if (enableDB) {
            db.save_db(simout.resolve(filename).c_str());
        }
    });
}

//调用函数保存数据
void
DespacitoStreamPrefetcher::writeDespacitoAggregate(uint64_t pc, uint64_t vaddr, uint64_t timeSample,
                                                   uint64_t sampleTrainEn, uint64_t sampleHit,
                                                   const SamplerSnapshot *victim, const SamplerSnapshot *inserted,
                                                   const SamplerSnapshot *updated,
                                                   const PatternSnapshot *patternTrain, const PatternSnapshot *patternUpdate,
                                                   const PatternSnapshot *patternInsert)
{
    if (!enableDB || !despacitoTrace)
        return;
    DespacitoTraceRecord rec(pc, vaddr, timeSample, sampleTrainEn, sampleHit,
                             victim, inserted, updated, patternTrain, patternUpdate, patternInsert);
    despacitoTrace->write_record(rec);
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
            // record update snapshot (will be included in aggregated record)
            // populate update snapshot below
        } else {
            stats.nlSampleUpdateReqHitOverBoardTimes++; // 命中但不在更新范围
        }
        sampler.accessEntry(sampler_entry);
    }else{
        stats.nlSampleUpdateReqNotHitTimes++; //如果不存在则表示没有命中
    }

    if (timestamp % sampleRate == 0) {
        stats.nlSampleInsertTimes++;//分析插入次数
        SamplerEntry *evict_sampler_entry = sampler.findVictim(block_index);
        stats.nlSampleVictimTouchedTrueTimes += evict_sampler_entry->touched ? 1 : 0; //分析victim被触摸次数
        stats.nlSampleVictimTouchedFalseTimes += evict_sampler_entry->touched ? 0 : 1;
        // create local snapshots to capture victim/insert data for aggregated record
        SamplerSnapshot victimSnap;
        bool haveVictim = false;
        SamplerSnapshot insertSnap;
        bool haveInsert = false;
        SamplerSnapshot updateSnap;
        bool haveUpdate = false;

        // if an existing entry is present in the victim slot, snapshot it
        if (evict_sampler_entry->pc) {
            victimSnap.timestamp = evict_sampler_entry->timestamp;
            victimSnap.address = evict_sampler_entry->address;
            victimSnap.pc = evict_sampler_entry->pc;
            victimSnap.touched = evict_sampler_entry->touched;
            haveVictim = true;
            stats.nlSampleReplaceTimes++;//分析替换次数
        } else {
            // empty slot -> will be insertion
            stats.nlSampleInsertTimes++;//分析插入次数
        }

        // keep original behavior: updatePatternTable may depend on the slot's
        // prior contents (matching original code placement)
        // prepare pattern snapshots (will be filled by updatePatternTable)
        PatternSnapshot patternTrainSnap;
        PatternSnapshot patternUpdateSnap;
        PatternSnapshot patternInsertSnap;
        updatePatternTable(evict_sampler_entry, &patternTrainSnap, &patternUpdateSnap, &patternInsertSnap);

        // now write new sampler contents into the slot
        evict_sampler_entry->timestamp = timestamp;
        evict_sampler_entry->address = block_index;
        evict_sampler_entry->pc = pfi.getPC();
        evict_sampler_entry->touched = false;
        sampler.insertEntry(block_index, false, evict_sampler_entry);

        // snapshot the newly inserted data
        insertSnap.timestamp = evict_sampler_entry->timestamp;
        insertSnap.address = evict_sampler_entry->address;
        insertSnap.pc = evict_sampler_entry->pc;
        insertSnap.touched = evict_sampler_entry->touched;
        haveInsert = true;

        // if we earlier touched an existing sampler_entry (update path), snapshot it
        if (sampler_entry && timestamp > sampler_entry->timestamp + minDistance &&
            timestamp <= sampler_entry->timestamp + maxDistance) {
            updateSnap.timestamp = sampler_entry->timestamp;
            updateSnap.address = sampler_entry->address;
            updateSnap.pc = sampler_entry->pc;
            updateSnap.touched = sampler_entry->touched;
            haveUpdate = true;
        }

        //记录trace到db里面
        writeDespacitoAggregate(pfi.getPC(), block_index, timestamp, 0, 0,
                    haveVictim ? &victimSnap : nullptr,
                    haveInsert ? &insertSnap : nullptr,
                    haveUpdate ? &updateSnap : nullptr,
                    patternTrainSnap.valid ? &patternTrainSnap : nullptr,
                    patternUpdateSnap.valid ? &patternUpdateSnap : nullptr,
                    patternInsertSnap.valid ? &patternInsertSnap : nullptr);
    }

    timestamp++;
    if (timestamp == 0){
    stats.nlTimeSampleCountResetTimes++;//
    }
    if ( timestamp > 16383){
    stats.nlTimeSampleCountOverTimes++; //计算越界次数
    }
}

void
DespacitoStreamPrefetcher::updatePatternTable(SamplerEntry *sampler_entry,
                                              PatternSnapshot *patternTrain,
                                              PatternSnapshot *patternUpdate,
                                              PatternSnapshot *patternInsert)
{
    stats.nlPatternTrainTimes++;
    if (sampler_entry->pc) {
        stats.nlPatternUpdateTimes++;//这里查pc，英文findVictim返回entry的时候，就会将其设valid设置为false
        PatternEntry *pattern_entry = patterns.findEntry(sampler_entry->pc, false);
        if (pattern_entry) {
            // capture pre-update snapshot if requested
            if (patternTrain) {
                patternTrain->conf = pattern_entry->conf;
                patternTrain->pc = pattern_entry->getTag();
                patternTrain->valid = true;
            }

            patterns.accessEntry(pattern_entry);
            if (sampler_entry->touched) {
                pattern_entry->conf++;
               stats.nlPatternUpdateTouchedTrueTimes++;
            } else {
                pattern_entry->conf--;
                stats.nlPatternUpdateTouchedFalseTimes++;
            }
            // capture post-update snapshot if requested
            if (patternUpdate) {
                patternUpdate->conf = pattern_entry->conf;
                patternUpdate->pc = pattern_entry->getTag();
                patternUpdate->valid = true;
            }
        } else {// 插入
            if (sampler_entry->touched) {
                pattern_entry = patterns.findVictim(sampler_entry->pc);
                bool was_valid = pattern_entry->isValid();
                pattern_entry->conf.reset();
                patterns.insertEntry(sampler_entry->pc, false, pattern_entry);
                if (was_valid)
                    stats.nlPatternReplaceTimes++;
                else
                    stats.nlPatternInsertTimes++;

                // capture inserted snapshot
                if (patternInsert) {
                    patternInsert->conf = pattern_entry->conf;
                    patternInsert->pc = pattern_entry->getTag();
                    patternInsert->valid = true;
                }
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
    InsertPFRequestToBuffer(AddrPriority(addr, prio, src, pfi.trigger_info));
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

//计数器
DespacitoStreamPrefetcher::DespacitoStats::DespacitoStats(statistics::Group *parent)
        : statistics::Group(parent),
            /* sampler */
            ADD_STAT(nlSampleTrainTimes, statistics::units::Count::get(), "Number of times sampler was trained"),
            ADD_STAT(nlSampleInsertTimes, statistics::units::Count::get(), "Number of times sampler entries were inserted"),
            ADD_STAT(nlSampleReplaceTimes, statistics::units::Count::get(), "Number of times sampler entries were replaced"),

            ADD_STAT(nlSampleUpdateTimes, statistics::units::Count::get(), "Number of times sampler entries were updated"),
            ADD_STAT(nlSampleUpdateReqNotHitTimes, statistics::units::Count::get(), "update request not hit times"),
            ADD_STAT(nlSampleUpdateReqHitOverBoardTimes, statistics::units::Count::get(), "update request hit but over board times"),

            ADD_STAT(nlSampleVictimTouchedTrueTimes, statistics::units::Count::get(), "Number of times sample victim touched true times"),
            ADD_STAT(nlSampleVictimTouchedFalseTimes, statistics::units::Count::get(), "Number of times sample victim touched false times"),

            /* pattern table */
            ADD_STAT(nlPatternTrainTimes, statistics::units::Count::get(), "Number of times pattern table was trained"),
            ADD_STAT(nlPatternReplaceTimes, statistics::units::Count::get(), "Number of patterns replaced in table"),
            ADD_STAT(nlPatternInsertTimes, statistics::units::Count::get(), "Number of patterns inserted into table"),

            ADD_STAT(nlPatternUpdateTimes, statistics::units::Count::get(), "Number of times a pattern's confidence changed"),
            ADD_STAT(nlPatternUpdateTouchedTrueTimes, statistics::units::Count::get(), "Number of times a pattern's training data was touched"),
            ADD_STAT(nlPatternUpdateTouchedFalseTimes, statistics::units::Count::get(), "Number of times a pattern's training data was not touched"),

            ADD_STAT(nlPatternPcHitTimes, statistics::units::Count::get(), "Number of times a pattern's pc was hit"),
            ADD_STAT(nlPatternPcHitValidEntryTimes, statistics::units::Count::get(), "Number of times a pattern's pc hit a valid entry"),
            ADD_STAT(nlPatternPcHitValidEntrySatEq3Times, statistics::units::Count::get(), "Number of times a pattern's pc hit a valid entry with Sat==3"),
            ADD_STAT(nlPatternPcHitValidEntrySatEq2Times, statistics::units::Count::get(), "Number of times a pattern's pc hit a valid entry with Sat==2"),
            ADD_STAT(nlPatternPcHitValidEntrySatEq1Times, statistics::units::Count::get(), "Number of times a pattern's pc hit a valid entry with Sat==1"),
            ADD_STAT(nlPatternPcHitValidEntrySatEq0Times, statistics::units::Count::get(), "Number of times a pattern's pc hit a valid entry with Sat==0"),

            /* prefetch */
            ADD_STAT(nlTimeSampleCountResetTimes, statistics::units::Count::get(), "Total number of samples processed"),
            ADD_STAT(nlTimeSampleCountOverTimes, statistics::units::Count::get(), "Number of times sample count exceeded threshold"),
            ADD_STAT(nlTransmitPrefetchReqTimes, statistics::units::Count::get(), "Number of prefetches sent"),
            ADD_STAT(nlFilterHits, statistics::units::Count::get(), "Number of prefetches skipped due to filter")
{}

}
}
