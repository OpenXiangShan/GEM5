#ifndef __MEM_CACHE_PREFETCH_DESPACITO_STREAM_HH__
#define __MEM_CACHE_PREFETCH_DESPACITO_STREAM_HH__

#include <cstdint>
#include <string>
#include <vector>

#include <boost/compute/detail/lru_cache.hpp>

#include "base/sat_counter.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/pred/general_arch_db.hh"
#include "debug/DespacitoStreamPrefetcher.hh"
#include "mem/cache/prefetch/associative_set.hh"
#include "mem/cache/prefetch/queued.hh"
#include "mem/packet.hh"
#include "params/DespacitoStreamPrefetcher.hh"

namespace gem5
{

struct DespacitoStreamPrefetcherParams;

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);

namespace prefetch
{

/**
 * @brief A specialized prefetcher for tracking memory access patterns with multiple interleaved data streams.
 *
 * @details The DespacitoStreamPrefetcher targets memory access patterns where a single instruction
 * accesses multiple data streams in a short time period, with the next element of each stream
 * typically accessed by other instructions much later. From a data stream perspective, these
 * accesses exhibit next-line patterns, but from an instruction perspective, they appear random.
 *
 * This prefetcher addresses cases where:
 * - Traditional stride/BOP prefetchers fail because they cannot establish stable PC-localized offsets
 * - Conventional stream prefetchers cannot maintain state for the large number of interleaved streams
 *
 * The implementation uses a sampling approach to identify potential stream patterns and tracks
 * confidence in detected patterns to generate prefetches.
 *
 * Key components:
 * - Sampler: Samples recent memory accesses to detect instructions that access two consecutive
 *   memory blocks within a specified time window
 * - Pattern table: Records identified instruction PCs that exhibit the target access pattern
 *   along with their confidence levels
 * - LRU filter: Prevents redundant prefetches
 */
class DespacitoStreamPrefetcher : public Queued
{
  protected:
    struct SamplerEntry : TaggedEntry
    {
        uint64_t timestamp;
        Addr address;
        Addr pc;
        bool touched;
        SamplerEntry() : TaggedEntry(), timestamp(0), address(0), pc(0), touched(false) {}
    };

    struct PatternEntry : TaggedEntry
    {
        SatCounter8 conf;
        PatternEntry(SatCounter8 cnt) : TaggedEntry(), conf(cnt) {}
    };

    const uint64_t sampleRate;
    const uint64_t minDistance;
    const uint64_t maxDistance;

    AssociativeSet<SamplerEntry> sampler;
    AssociativeSet<PatternEntry> patterns;
    uint64_t timestamp;

    void updateSampler(const PrefetchInfo &pfi);

    /* Forward declare PatternSnapshot so it can be used in the updatePatternTable
     * declaration which appears before the full PatternSnapshot definition. */
    struct PatternSnapshot;

    void updatePatternTable(SamplerEntry *sampler_entry,
                            PatternSnapshot *patternTrain, PatternSnapshot *patternUpdate,
                            PatternSnapshot *patternInsert);

    //性能计数器
    struct DespacitoStats : public statistics::Group
    {
        DespacitoStats(statistics::Group *parent);

        // Sampler stats
        statistics::Scalar nlSampleTrainTimes;

        statistics::Scalar nlSampleInsertTimes;
        statistics::Scalar nlSampleReplaceTimes;

        statistics::Scalar nlSampleUpdateTimes;
        statistics::Scalar nlSampleUpdateReqNotHitTimes;
        statistics::Scalar nlSampleUpdateReqHitOverBoardTimes;

        statistics::Scalar nlSampleVictimTouchedTrueTimes;
        statistics::Scalar nlSampleVictimTouchedFalseTimes;


        // Pattern table stats
        statistics::Scalar nlPatternTrainTimes;
        statistics::Scalar nlPatternReplaceTimes;
        statistics::Scalar nlPatternInsertTimes;

        statistics::Scalar nlPatternUpdateTimes;
        statistics::Scalar nlPatternUpdateTouchedTrueTimes;
        statistics::Scalar nlPatternUpdateTouchedFalseTimes;

        statistics::Scalar nlPatternPcHitTimes;
        statistics::Scalar nlPatternPcHitValidEntryTimes;
        statistics::Scalar nlPatternPcHitValidEntrySatEq3Times;
        statistics::Scalar nlPatternPcHitValidEntrySatEq2Times;
        statistics::Scalar nlPatternPcHitValidEntrySatEq1Times;
        statistics::Scalar nlPatternPcHitValidEntrySatEq0Times;

        // Prefetch stats
        statistics::Scalar nlTimeSampleCountResetTimes; // total samples processed
        statistics::Scalar nlTimeSampleCountOverTimes;
        statistics::Scalar nlTransmitPrefetchReqTimes;
        statistics::Scalar nlFilterHits;
    } stats;

    /* Database for tracing events related to DespacitoStreamPrefetcher.
    * Usage:
    *   // enable and set output filename (will be saved on simulator exit)
    *   initDB(true, "despacito_stream.db");
    *   // write an event at runtime
    *   writeDespacitoTrace(pc, block_addr, "sample_insert");
    */
    bool enableDB = false;
    gem5::DataBase db;
    gem5::TraceManager *despacitoTrace = nullptr;

    // chisel的数据类型
    struct SamplerSnapshot
    {
        uint64_t timestamp = 0;
        Addr address = 0;
        Addr pc = 0;
        bool touched = false;
    };

    struct PatternSnapshot
    {
        SatCounter8 conf;
        Addr pc;
        bool valid = false;
        PatternSnapshot() : conf(2, 0), pc(0), valid(false) {}
    };

    //将数据保存到变量里面
    struct DespacitoTraceRecord : public gem5::Record
    {
        DespacitoTraceRecord(uint64_t pc, uint64_t vaddr, uint64_t timeSample,
                            uint64_t sampleTrainEn, uint64_t sampleHit,
                            const SamplerSnapshot *victim, const SamplerSnapshot *inserted,
                            const SamplerSnapshot *updated,
                            const PatternSnapshot *patternTrain, const PatternSnapshot *patternUpdate,
                            const PatternSnapshot *patternInsert) {
        _tick = curTick();

        _uint64_data["pc"] = pc;
        _uint64_data["vaddr"] = vaddr;
        _uint64_data["timeSample"] = timeSample;
        _uint64_data["sampleTrainEn"] = sampleTrainEn;
        _uint64_data["sampleHit"] = sampleHit;

        if (victim) {
            _uint64_data["victim_timestamp"] = victim->timestamp;
            _uint64_data["victim_addr"] = victim->address;
            _uint64_data["victim_pc"] = victim->pc;
            _uint64_data["victim_touched"] = victim->touched ? 1 : 0;
        } else {
            _uint64_data["victim_timestamp"] = 0;
            _uint64_data["victim_addr"] = 0;
            _uint64_data["victim_pc"] = 0;
            _uint64_data["victim_touched"] = 0;
        }

        if (inserted) {
            _uint64_data["insert_timestamp"] = inserted->timestamp;
            _uint64_data["insert_addr"] = inserted->address;
            _uint64_data["insert_pc"] = inserted->pc;
            _uint64_data["insert_touched"] = inserted->touched ? 1 : 0;
        } else {
            _uint64_data["insert_timestamp"] = 0;
            _uint64_data["insert_addr"] = 0;
            _uint64_data["insert_pc"] = 0;
            _uint64_data["insert_touched"] = 0;
        }

        if (updated) {
            _uint64_data["update_timestamp"] = updated->timestamp;
            _uint64_data["update_addr"] = updated->address;
            _uint64_data["update_pc"] = updated->pc;
            _uint64_data["update_touched"] = updated->touched ? 1 : 0;
        } else {
            _uint64_data["update_timestamp"] = 0;
            _uint64_data["update_addr"] = 0;
            _uint64_data["update_pc"] = 0;
            _uint64_data["update_touched"] = 0;
        }

        if (patternTrain && patternTrain->valid) {
            _uint64_data["patternTrain_conf"] = static_cast<uint64_t>(patternTrain->conf);
            _uint64_data["patternTrain_pc"] = patternTrain->pc;
        } else {
            _uint64_data["patternTrain_conf"] = 0;
            _uint64_data["patternTrain_pc"] = 0;
        }

        if (patternUpdate && patternUpdate->valid) {
            _uint64_data["patternUpdate_conf"] = static_cast<uint64_t>(patternUpdate->conf);
            _uint64_data["patternUpdate_pc"] = patternUpdate->pc;
        } else {
            _uint64_data["patternUpdate_conf"] = 0;
            _uint64_data["patternUpdate_pc"] = 0;
        }

        if (patternInsert && patternInsert->valid) {
            _uint64_data["patternInsert_conf"] = static_cast<uint64_t>(patternInsert->conf);
            _uint64_data["patternInsert_pc"] = patternInsert->pc;
        } else {
            _uint64_data["patternInsert_conf"] = 0;
            _uint64_data["patternInsert_pc"] = 0;
        }

        _text_data["event"] = std::string("aggregated");
        }
    };

    // Initialize in-memory DB and create trace table. If enable==false this
    // does nothing. The filename is used when saving DB on exit.
    void initDB(bool enable, const std::string &filename = "despacito_stream.db");

    void writeDespacitoAggregate(uint64_t pc, uint64_t vaddr, uint64_t timeSample,
                                uint64_t sampleTrainEn ,uint64_t sampleHit,
                                const SamplerSnapshot *victim, const SamplerSnapshot *inserted,
                                const SamplerSnapshot *updated,
                                const PatternSnapshot *patternTrain,const PatternSnapshot *patternUpdate,
                                const PatternSnapshot *patternInsert
                                );

  public:
    boost::compute::detail::lru_cache<Addr, Addr> *filter;

    DespacitoStreamPrefetcher(const DespacitoStreamPrefetcherParams &p);

    void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses) override
    {
        panic("not implemented");
    };

    void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late,
                           PrefetchSourceType pf_source, bool miss_repeat) override
    {
        panic("not implemented");
    };

    void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late);

    bool sendPFWithFilter(const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses, int prio,
                          PrefetchSourceType src);
};

}

}


#endif
