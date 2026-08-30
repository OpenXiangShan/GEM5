#include "cpu/pred/btb/btb_llbpx.hh"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <string>

#include "base/intmath.hh"
#include "base/logging.hh"
#include "cpu/pred/btb/btb_tage.hh"
#include "sim/cur_tick.hh"

#ifndef UNIT_TEST
#include "base/trace.hh"
#endif

#ifdef UNIT_TEST
#include "cpu/pred/btb/test/test_dprintf.hh"

#endif

namespace gem5
{
namespace branch_prediction
{
namespace btb_pred
{

namespace
{

#ifdef UNIT_TEST
Tick
llbpxCurrentTick()
{
    return 0;
}
#else
Tick
llbpxCurrentTick()
{
    return curTick();
}
#endif

} // anonymous namespace

#ifdef UNIT_TEST
namespace test
{
#endif

#ifndef UNIT_TEST
BTBLLBPX::LLBPXStats::LLBPXStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(lookup, statistics::units::Count::get(),
               "LLBP-X branch lookups"),
      ADD_STAT(lookupCidZero, statistics::units::Count::get(),
               "LLBP-X lookups with zero context id"),
      ADD_STAT(lookupCidRepeatApprox, statistics::units::Count::get(),
               "LLBP-X lookups that reused the last context id"),
      ADD_STAT(contextMiss, statistics::units::Count::get(),
               "LLBP-X context misses"),
      ADD_STAT(contextHit, statistics::units::Count::get(),
               "LLBP-X context hits"),
      ADD_STAT(patternMissAfterContextHit, statistics::units::Count::get(),
               "LLBP-X pattern misses after a context hit"),
      ADD_STAT(patternHit, statistics::units::Count::get(),
               "LLBP-X pattern hits"),
      ADD_STAT(lookupByDepthClass, statistics::units::Count::get(),
               "LLBP-X lookups by adaptive depth class"),
      ADD_STAT(providerUse, statistics::units::Count::get(),
               "LLBP-X candidates published before TAGE provider arbitration"),
      ADD_STAT(override, statistics::units::Count::get(),
               "LLBP-X candidate direction writes before TAGE arbitration"),
      ADD_STAT(directionChange, statistics::units::Count::get(),
               "LLBP-X candidates that differ from the previous direction prediction"),
      ADD_STAT(allocContext, statistics::units::Count::get(),
               "LLBP-X context allocations"),
      ADD_STAT(allocContextRevisit, statistics::units::Count::get(),
               "LLBP-X context allocation attempts that found an existing context"),
      ADD_STAT(allocPattern, statistics::units::Count::get(),
               "LLBP-X pattern allocations"),
      ADD_STAT(allocPatternRevisit, statistics::units::Count::get(),
               "LLBP-X pattern allocation attempts that found an existing pattern"),
      ADD_STAT(allocPatternByTable, statistics::units::Count::get(),
               "LLBP-X pattern allocations by TAGE table"),
      ADD_STAT(allocPatternByDepthClass, statistics::units::Count::get(),
               "LLBP-X pattern allocations by adaptive depth class"),
      ADD_STAT(contextTotalEntries, statistics::units::Count::get(),
               "Total LLBP-X context entry capacity"),
      ADD_STAT(contextValidEntries, statistics::units::Count::get(),
               "Live LLBP-X context entries"),
      ADD_STAT(contextFreeEntries, statistics::units::Count::get(),
               "Free LLBP-X context entries"),
      ADD_STAT(contextFullSets, statistics::units::Count::get(),
               "LLBP-X context sets whose ways are all occupied"),
      ADD_STAT(contextEmptySets, statistics::units::Count::get(),
               "LLBP-X context sets with no live entries"),
      ADD_STAT(contextSetOccupancy, statistics::units::Count::get(),
               "LLBP-X context-set occupancy distribution by used ways"),
      ADD_STAT(patternTotalEntries, statistics::units::Count::get(),
               "Live LLBP-X pattern entries across all contexts"),
      ADD_STAT(patternBoundedCapacity, statistics::units::Count::get(),
               "Total bounded LLBP-X pattern capacity across live contexts"),
      ADD_STAT(patternFreeEntries, statistics::units::Count::get(),
               "Free bounded LLBP-X pattern slots across live contexts"),
      ADD_STAT(patternTotalSets, statistics::units::Count::get(),
               "Total LLBP-X pattern sets across live contexts"),
      ADD_STAT(patternFullSets, statistics::units::Count::get(),
               "LLBP-X pattern sets whose ways are all occupied"),
      ADD_STAT(patternEmptySets, statistics::units::Count::get(),
               "LLBP-X pattern sets with no live entries"),
      ADD_STAT(contextsWithAnyFullPatternSet, statistics::units::Count::get(),
               "LLBP-X live contexts containing at least one full pattern set"),
      ADD_STAT(patternSetOccupancy, statistics::units::Count::get(),
               "LLBP-X pattern-set occupancy distribution by used ways"),
      ADD_STAT(updateCalls, statistics::units::Count::get(),
               "LLBP-X update calls"),
      ADD_STAT(updateEntriesSeen, statistics::units::Count::get(),
               "LLBP-X update entries considered"),
      ADD_STAT(updateEntriesWithMeta, statistics::units::Count::get(),
               "LLBP-X update entries that found prediction-time meta"),
      ADD_STAT(directTageAllocCalls, statistics::units::Count::get(),
               "LLBP-X direct TAGE allocation callbacks"),
      ADD_STAT(directTageAllocWithMeta, statistics::units::Count::get(),
               "LLBP-X direct TAGE allocation callbacks that found prediction-time meta"),
      ADD_STAT(directTageAllocNoMeta, statistics::units::Count::get(),
               "LLBP-X direct TAGE allocation callbacks that missed prediction-time meta"),
      ADD_STAT(updatePattern, statistics::units::Count::get(),
               "LLBP-X pattern counter updates"),
      ADD_STAT(rcrRecover, statistics::units::Count::get(),
               "LLBP-X RCR recoveries"),
      ADD_STAT(providerFallback, statistics::units::Count::get(),
               "LLBP-X provider depth fallback uses"),
      ADD_STAT(providerRejectDepth, statistics::units::Count::get(),
               "LLBP-X pattern hits rejected by TAGE provider depth"),
      ADD_STAT(providerRejectTiming, statistics::units::Count::get(),
               "LLBP-X pattern hits rejected by Pattern Buffer timing"),
      ADD_STAT(patternBufferHit, statistics::units::Count::get(),
               "LLBP-X Pattern Buffer ready hits"),
      ADD_STAT(patternBufferMiss, statistics::units::Count::get(),
               "LLBP-X Pattern Buffer misses"),
      ADD_STAT(patternBufferNotReady, statistics::units::Count::get(),
               "LLBP-X Pattern Buffer hits that are not ready yet"),
      ADD_STAT(patternBufferInstall, statistics::units::Count::get(),
               "LLBP-X Pattern Buffer installs"),
      ADD_STAT(patternBufferDemandPatternMiss, statistics::units::Count::get(),
               "LLBP-X Pattern Buffer ready contexts without a matching pattern"),
      ADD_STAT(patternBufferPrefetchIssue, statistics::units::Count::get(),
               "LLBP-X Pattern Buffer speculative prefetch installs"),
      ADD_STAT(patternBufferPrefetchHit, statistics::units::Count::get(),
               "LLBP-X Pattern Buffer prefetches skipped because the context was already present"),
      ADD_STAT(patternBufferPrefetchDrop, statistics::units::Count::get(),
               "LLBP-X Pattern Buffer prefetches dropped because no evictable entry existed"),
      ADD_STAT(cttEntryCreate, statistics::units::Count::get(),
               "LLBP-X context tracking table entries created"),
      ADD_STAT(cttLookupMiss, statistics::units::Count::get(),
               "LLBP-X adaptive context depth lookups that missed in CTT"),
      ADD_STAT(cttDepthSwitchToShallow, statistics::units::Count::get(),
               "LLBP-X adaptive context depth switches to shallow"),
      ADD_STAT(cttDepthSwitchToDeep, statistics::units::Count::get(),
               "LLBP-X adaptive context depth switches to deep")
{
}

BTBLLBPX::BTBLLBPX(const Params &p)
    : TimedBaseBTBPredictor(p),
      numThreads(p.numThreads),
      tageNumPredictors(p.tageNumPredictors),
      patternSetCapacity(p.patternSets),
      patternSetAssoc(p.patternWays),
      tagBits(p.tagBits),
      keyBits(p.keyBits),
      rcrEntries(p.rcrEntries),
      rcrType(static_cast<RCRType>(p.rcrType)),
      rcrWindow(p.rcrWindow),
      rcrDist(p.rcrDist),
      rcrShift(p.rcrShift),
      rcrTagBits(p.rcrTagBits),
      rcrBaseTagBits(p.rcrBaseTagBits),
      useOriginalRcr(p.useOriginalRcr),
      cttSets(p.cttSets),
      cttWays(p.cttWays),
      shallowContextDepth(p.shallowContextDepth),
      deepContextDepth(p.deepContextDepth),
      trackingThreshold(p.trackingThreshold),
      adaptThreshold(p.adaptThreshold),
      histLenThreshold(p.histLenThreshold),
      counterBits(p.counterBits),
      enableTiming(p.enableTiming),
      adaptCtxDepth(p.adaptCtxDepth),
      overrideOnlyOnDiff(p.overrideOnlyOnDiff),
      patternBufferLatency(p.patternBufferLatency),
      contexts(p.contextSets, p.contextWays),
      ctt(p.cttSets, p.cttWays),
      patternBufferSize(p.patternBufferSize),
      threadState(std::max<unsigned>(p.numThreads, MaxThreads)),
      llbpxStats(this)
{
    panic_if(tagBits == 0 || tagBits >= 64, "BTBLLBPX tagBits must be in [1, 63]");
    panic_if(keyBits == 0 || keyBits >= 64, "BTBLLBPX keyBits must be in [1, 63]");
    panic_if(rcrTagBits == 0 || rcrTagBits >= 64,
             "BTBLLBPX rcrTagBits must be in [1, 63]");
    panic_if(rcrBaseTagBits == 0 || rcrBaseTagBits >= 64,
             "BTBLLBPX rcrBaseTagBits must be in [1, 63]");
    panic_if(rcrDist + rcrWindow > rcrEntries,
             "BTBLLBPX requires rcrDist + rcrWindow <= rcrEntries");
    panic_if(shallowContextDepth == 0 || deepContextDepth < shallowContextDepth,
             "BTBLLBPX requires shallowContextDepth > 0 and deepContextDepth >= shallowContextDepth");
    panic_if(static_cast<unsigned>(rcrType) > 4,
             "BTBLLBPX rcrType must be in [0, 4]");
    panic_if(enableTiming && patternBufferSize == 0,
             "BTBLLBPX timing mode requires a non-zero patternBufferSize");
    ContextEntry::defaultPatternSetCapacity = p.patternSets;
    ContextEntry::defaultPatternSetAssoc = p.patternWays;
#ifndef UNIT_TEST
    llbpxStats.allocPatternByTable.init(std::max<unsigned>(tageNumPredictors, 1));
    llbpxStats.allocPatternByDepthClass.init(2);
    llbpxStats.lookupByDepthClass.init(2);
    llbpxStats.contextSetOccupancy.init(contexts.ways() + 1);
    llbpxStats.patternSetOccupancy.init(patternSetAssoc + 2);
    hasDB = true;
    dbName = std::string("llbpx");
#endif
}
#else
BTBLLBPX::BTBLLBPX(bool adaptCtxDepthParam)
    : TimedBaseBTBPredictor(),
      numThreads(MaxThreads),
      tageNumPredictors(8),
      patternSetCapacity(64),
      patternSetAssoc(4),
      tagBits(16),
      keyBits(32),
      rcrEntries(40),
      rcrType(RCRType::UnconditionalOnly),
      rcrWindow(8),
      rcrDist(4),
      rcrShift(1),
      rcrTagBits(32),
      rcrBaseTagBits(12),
      useOriginalRcr(false),
      cttSets(1024),
      cttWays(4),
      shallowContextDepth(8),
      deepContextDepth(36),
      trackingThreshold(7),
      adaptThreshold(7),
      histLenThreshold(22),
      counterBits(3),
      enableTiming(false),
      adaptCtxDepth(adaptCtxDepthParam),
      overrideOnlyOnDiff(false),
      patternBufferLatency(6),
      contexts(2048, 4),
      ctt(1024, 4),
      patternBufferSize(64),
      threadState(MaxThreads)
{
    ContextEntry::defaultPatternSetCapacity = 64;
    ContextEntry::defaultPatternSetAssoc = 4;
    setNumDelay(2);
}
#endif

#ifndef UNIT_TEST
void
BTBLLBPX::preDumpStats()
{
    TimedBaseBTBPredictor::preDumpStats();
    refreshStorageStats();
}

void
BTBLLBPX::refreshStorageStats()
{
    llbpxStats.contextTotalEntries = contexts.sets() * contexts.ways();
    llbpxStats.contextValidEntries = contexts.validEntries();
    llbpxStats.contextFreeEntries =
        llbpxStats.contextTotalEntries.value() - llbpxStats.contextValidEntries.value();
    llbpxStats.contextFullSets = 0;
    llbpxStats.contextEmptySets = 0;
    llbpxStats.patternTotalEntries = 0;
    llbpxStats.patternBoundedCapacity = 0;
    llbpxStats.patternFreeEntries = 0;
    llbpxStats.patternTotalSets = 0;
    llbpxStats.patternFullSets = 0;
    llbpxStats.patternEmptySets = 0;
    llbpxStats.contextsWithAnyFullPatternSet = 0;

    for (unsigned occ = 0; occ < llbpxStats.contextSetOccupancy.size(); ++occ) {
        llbpxStats.contextSetOccupancy[occ] = 0;
    }
    for (unsigned occ = 0; occ < llbpxStats.patternSetOccupancy.size(); ++occ) {
        llbpxStats.patternSetOccupancy[occ] = 0;
    }

    for (unsigned set = 0; set < contexts.sets(); ++set) {
        const unsigned occ = contexts.setOccupancy(set);
        llbpxStats.contextSetOccupancy[occ]++;
        if (occ == 0) {
            llbpxStats.contextEmptySets++;
        } else if (occ == contexts.ways()) {
            llbpxStats.contextFullSets++;
        }

        for (const auto &ctx : contexts.setEntries(set)) {
            if (!ctx.valid) {
                continue;
            }
            const auto *patterns = ctx.patternSetIfPresent();
            if (!patterns) {
                continue;
            }

            bool anyFullPatternSet = false;
            llbpxStats.patternTotalEntries += patterns->entries();
            llbpxStats.patternTotalSets += patterns->sets();

            if (!patterns->isUnlimited()) {
                llbpxStats.patternBoundedCapacity += patterns->maxSize();
                llbpxStats.patternFreeEntries +=
                    patterns->maxSize() - patterns->entries();
            }

            for (unsigned pset = 0; pset < patterns->sets(); ++pset) {
                const unsigned pocc = patterns->setOccupancy(pset);
                const unsigned bucket = std::min<unsigned>(
                    pocc, llbpxStats.patternSetOccupancy.size() - 1);
                llbpxStats.patternSetOccupancy[bucket]++;

                if (pocc == 0) {
                    llbpxStats.patternEmptySets++;
                }
                if (!patterns->isUnlimited() && pocc == patterns->ways()) {
                    llbpxStats.patternFullSets++;
                    anyFullPatternSet = true;
                }
            }

            if (anyFullPatternSet) {
                llbpxStats.contextsWithAnyFullPatternSet++;
            }
        }
    }
}
#endif

ThreadID
BTBLLBPX::predictorTid(const std::vector<FullBTBPrediction> &stagePreds) const
{
    assert(!stagePreds.empty());
    return stagePreds.front().tid;
}

void
BTBLLBPX::setTrace()
{
#ifndef UNIT_TEST
    if (enableDB) {
        std::vector<std::pair<std::string, DataType>> fields_vec = {
            std::make_pair("startPC", UINT64),
            std::make_pair("branchPC", UINT64),
            std::make_pair("actualTaken", UINT64),
            std::make_pair("basePred", UINT64),
            std::make_pair("llbpxPred", UINT64),
            std::make_pair("providerUsed", UINT64),
            std::make_pair("overridden", UINT64),
            std::make_pair("directionChanged", UINT64),
            std::make_pair("providerDepthEligible", UINT64),
            std::make_pair("providerTimingReady", UINT64),
            std::make_pair("providerInfoMissing", UINT64),
            std::make_pair("contextHit", UINT64),
            std::make_pair("patternHit", UINT64),
            std::make_pair("patternBufferInFlight", UINT64),
            std::make_pair("wi", UINT64),
            std::make_pair("allocTargetWi", UINT64),
            std::make_pair("baseProviderHistIdxBias", UINT64),
            std::make_pair("hitHistIdxBias", UINT64),
            std::make_pair("keyTable", UINT64),
            std::make_pair("cid", UINT64),
            std::make_pair("allocTargetCid", UINT64),
            std::make_pair("bcid", UINT64),
            std::make_pair("pcid", UINT64),
            std::make_pair("pbcid", UINT64),
            std::make_pair("key", UINT64),
            std::make_pair("contextTag", UINT64),
            std::make_pair("patternTag", UINT64),
            std::make_pair("tageAllocTriggered", UINT64),
            std::make_pair("tageAllocTableMask", UINT64),
            std::make_pair("allocTableMask", UINT64),
            std::make_pair("allocContextCreated", UINT64),
            std::make_pair("allocContextRevisit", UINT64),
            std::make_pair("allocPatternCreated", UINT64),
            std::make_pair("allocPatternRevisit", UINT64),
            std::make_pair("allocSkipNoKey", UINT64),
            std::make_pair("updatePatternCalled", UINT64),
            std::make_pair("updatePatternFound", UINT64),
            std::make_pair("counterBias", UINT64),
            std::make_pair("counterBeforeBiased", UINT64),
            std::make_pair("counterAfterBiased", UINT64),
        };
        llbpxTrace = _db->addAndGetTrace("LLBPXTRACE", fields_vec);
        llbpxTrace->init_table();
    }
#endif
}

void
BTBLLBPX::setTage(const BTBTAGE *tage)
{
    tagePredictor = tage;
    initFilterTables();
}

uint64_t
BTBLLBPX::tableMaskFor(const std::vector<unsigned> &tables) const
{
    uint64_t mask = 0;
    for (auto table : tables) {
        if (table < 64) {
            mask |= 1ULL << table;
        }
    }
    return mask;
}

Addr
BTBLLBPX::patternKeyForSnapshotTable(const FetchTarget &entry, Addr startPC,
                                     Addr branchPC, Addr contextKey,
                                     unsigned wi, unsigned table,
                                     uint8_t asidHash) const
{
    const int code = filterCode(wi, table);
    const Addr lowBits = static_cast<Addr>(std::max(code, 0)) & mask(10);
    if (!tagePredictor) {
        const Addr base = patternKey(contextKey, branchPC, asidHash);
        return (((base & mask(keyBits)) << 10) | lowBits) & mask(keyBits);
    }
    const Addr base = tagePredictor->getLlbpxPatternKeyFromSnapshot(
        entry, startPC, branchPC, table, contextKey, asidHash);
    return (((base & mask(keyBits)) << 10) | lowBits) & mask(keyBits);
}

bool
BTBLLBPX::synthesizeMissingBranchMeta(const FetchTarget &entry,
                                      const LLBPXMeta &llbpxMeta,
                                      const BTBEntry &btbEntry,
                                      BranchMeta &branchMeta) const
{
    if (!btbEntry.valid || !btbEntry.isCond || btbEntry.alwaysTaken) {
        return false;
    }

    branchMeta.startPC = llbpxMeta.startPC;
    branchMeta.branchPC = btbEntry.pc;
    branchMeta.tid = llbpxMeta.tid;
    branchMeta.asidHash = llbpxMeta.asidHash;
    branchMeta.basePred = btbEntry.ctr >= 0;
    branchMeta.llbpxPred = branchMeta.basePred;
    branchMeta.baseProviderHistIdx = -2;
    branchMeta.providerInfoMissing = true;
    branchMeta.hitHistIdx = -1;
    branchMeta.bcid = llbpxMeta.rcrIdsSnapshot.bcid;
    branchMeta.pcid = llbpxMeta.rcrIdsSnapshot.pcid;
    branchMeta.pbcid = llbpxMeta.rcrIdsSnapshot.pbcid;
    branchMeta.cids = llbpxMeta.cidsSnapshot;
    branchMeta.wi = adaptCtxDepth ? selectContextDepth(branchMeta.bcid) : 0;
    branchMeta.cid = (useOriginalRcr || adaptCtxDepth) ?
        branchMeta.cids[branchMeta.wi] :
        contextKey(branchMeta.tid, llbpxMeta.startPC, btbEntry.pc,
                   llbpxMeta.historySnapshot, llbpxMeta.asidHash);
    branchMeta.contextTag = branchMeta.cid;

    const unsigned numTables = tagePredictor ?
        tagePredictor->getNumPredictors() : 1;
    for (unsigned snapshotWi = 0; snapshotWi < branchMeta.tableKeysByWi.size();
         ++snapshotWi) {
        branchMeta.tableKeysByWi[snapshotWi].assign(numTables, 0);
        branchMeta.tableKeyValidByWi[snapshotWi].assign(numTables, false);
        const Addr snapshotCid =
            (useOriginalRcr || adaptCtxDepth || snapshotWi != 0) ?
            branchMeta.cids[snapshotWi] :
            contextKey(branchMeta.tid, llbpxMeta.startPC, btbEntry.pc,
                       llbpxMeta.historySnapshot, llbpxMeta.asidHash);
        for (unsigned table = 0; table < numTables; ++table) {
            if (filterCode(snapshotWi, table) < 0) {
                continue;
            }
            const Addr key = patternKeyForSnapshotTable(
                entry, llbpxMeta.startPC, btbEntry.pc, snapshotCid, snapshotWi,
                table, llbpxMeta.asidHash);
            if (key == 0 && tagePredictor) {
                continue;
            }
            branchMeta.tableKeysByWi[snapshotWi][table] = key;
            branchMeta.tableKeyValidByWi[snapshotWi][table] = true;
        }
    }

    return true;
}

void
BTBLLBPX::writeTraceRecord(const BranchMeta &meta, bool actualTaken)
{
#ifndef UNIT_TEST
    if (!enableDB || !llbpxTrace) {
        return;
    }

    constexpr uint64_t histIdxBias = 128;
    LlbpxTrace record;
    record.set(meta.startPC, meta.branchPC, actualTaken, meta.basePred,
        meta.llbpxPred, meta.providerUsed, meta.overridden,
        meta.directionChanged, meta.providerDepthEligible,
        meta.providerTimingReady, meta.providerInfoMissing, meta.contextHit,
        meta.patternHit, meta.patternBufferInFlight, meta.wi,
        meta.trace.allocTargetWi,
        static_cast<uint64_t>(meta.baseProviderHistIdx + histIdxBias),
        static_cast<uint64_t>(meta.hitHistIdx + histIdxBias), meta.keyTable,
        meta.cid, meta.trace.allocTargetCid, meta.bcid, meta.pcid, meta.pbcid,
        meta.key, meta.contextTag, meta.patternTag,
        meta.trace.tageAllocTriggered, meta.trace.tageAllocTableMask,
        meta.trace.allocTableMask, meta.trace.allocContextCreated,
        meta.trace.allocContextRevisit, meta.trace.allocPatternCreated,
        meta.trace.allocPatternRevisit, meta.trace.allocSkipNoKey,
        meta.trace.updatePatternCalled, meta.trace.updatePatternFound,
        meta.trace.counterBias, meta.trace.counterBeforeBiased,
        meta.trace.counterAfterBiased);
    llbpxTrace->write_record(record);
#endif
}

std::shared_ptr<void>
BTBLLBPX::getPredictionMeta(ThreadID tid)
{
    if (tid >= threadState.size()) {
        return nullptr;
    }
    return threadState[tid].meta;
}

void
BTBLLBPX::putPCHistory(Addr startAddr,
                       const boost::dynamic_bitset<> &history,
                       std::vector<FullBTBPrediction> &stagePreds)
{
    if (stagePreds.empty()) {
        return;
    }

    const ThreadID tid = predictorTid(stagePreds);
    const unsigned firstStage = std::min<unsigned>(getDelay(), stagePreds.size());
    if (tid >= threadState.size() || firstStage >= stagePreds.size()) {
        return;
    }

    auto meta = std::make_shared<LLBPXMeta>();
    meta->tid = tid;
    meta->startPC = startAddr;
    meta->asidHash = stagePreds[firstStage].asidHash;
    meta->historySnapshot = history;
    meta->rcrSnapshot = threadState[tid].rcr;
    recomputeRCRContextIds(tid);
    meta->rcrIdsSnapshot = threadState[tid].rcrIds;
    meta->cidsSnapshot[0] = contextIdForDepth(tid, 0, false);
    meta->cidsSnapshot[1] = contextIdForDepth(tid, 1, false);

    for (unsigned stage = firstStage; stage < stagePreds.size(); ++stage) {
        auto &stagePred = stagePreds[stage];
        for (const auto &entry : stagePred.btbEntries) {
            if (!entry.valid || !entry.isCond || entry.alwaysTaken) {
                continue;
            }

            const auto branchPC = entry.pc;
            auto it = CondTakens_find(stagePred.condTakens, branchPC);
            bool basePred = entry.ctr >= 0;
            if (it != stagePred.condTakens.end()) {
                basePred = it->second;
            }

            int baseProviderHistIdx = -2;
            auto tageInfoIt = stagePred.tageInfoForMgscs.find(branchPC);
            if (tageInfoIt != stagePred.tageInfoForMgscs.end()) {
                baseProviderHistIdx = tageInfoIt->second.tage_provider_table;
            }

            auto branchMeta = lookup(tid, startAddr, entry, basePred,
                                     history, stagePred.asidHash,
                                     baseProviderHistIdx);
            if (!branchMeta.patternHit) {
                meta->branches[entry.pc] = branchMeta;
                continue;
            }

            if (!branchMeta.providerDepthEligible) {
#ifndef UNIT_TEST
                llbpxStats.providerRejectDepth++;
#endif
                meta->branches[entry.pc] = branchMeta;
                DPRINTF(LLBPX,
                        "reject by depth start %#lx branch %#lx llbpx depth %d tage depth %d\n",
                        startAddr, entry.pc, branchMeta.hitHistIdx,
                        branchMeta.baseProviderHistIdx);
                continue;
            }

            if (!branchMeta.providerTimingReady) {
#ifndef UNIT_TEST
                llbpxStats.providerRejectTiming++;
#endif
                meta->branches[entry.pc] = branchMeta;
                DPRINTF(LLBPX,
                        "reject by timing start %#lx branch %#lx key %#lx tag %#lx\n",
                        startAddr, entry.pc, branchMeta.key,
                        branchMeta.patternTag);
                continue;
            }

            branchMeta.providerUsed = true;
            branchMeta.directionChanged = branchMeta.llbpxPred != basePred;
#ifndef UNIT_TEST
            llbpxStats.providerUse++;
            if (branchMeta.directionChanged) {
                llbpxStats.directionChange++;
            }
#endif

            auto &providerInfo = stagePred.tageInfoForMgscs[branchPC];
            providerInfo.llbpx_pred_valid = true;
            providerInfo.llbpx_pred_taken = branchMeta.llbpxPred;
            providerInfo.llbpx_provider_table = branchMeta.hitHistIdx;
            providerInfo.primary_pred_taken = branchMeta.llbpxPred;
            providerInfo.primary_provider_is_llbpx = true;
            providerInfo.primary_provider_table = branchMeta.hitHistIdx;
            providerInfo.llbpx_provider_used = true;

            if (overrideOnlyOnDiff && !branchMeta.directionChanged) {
                meta->branches[entry.pc] = branchMeta;
                continue;
            }

            if (it != stagePred.condTakens.end()) {
                it->second = branchMeta.llbpxPred;
            } else {
                stagePred.condTakens.emplace_back(entry.pc, branchMeta.llbpxPred);
            }
            branchMeta.overridden = true;
            meta->branches[entry.pc] = branchMeta;
#ifndef UNIT_TEST
            llbpxStats.override++;
#endif
            DPRINTF(LLBPX,
                    "override start %#lx branch %#lx base %d llbpx %d diff %d stage %u\n",
                    startAddr, entry.pc, basePred, branchMeta.llbpxPred,
                    branchMeta.directionChanged, stage);
        }
    }

    threadState[tid].meta = meta;
}

BTBLLBPX::BranchMeta
BTBLLBPX::lookup(ThreadID tid, Addr startPC, const BTBEntry &entry,
                 bool basePred, const boost::dynamic_bitset<> &history,
                 uint8_t asidHash, int baseProviderHistIdx)
{
#ifndef UNIT_TEST
    llbpxStats.lookup++;
#endif

    BranchMeta meta;
    meta.startPC = startPC;
    meta.branchPC = entry.pc;
    meta.tid = tid;
    meta.asidHash = asidHash;
    meta.basePred = basePred;
    meta.llbpxPred = basePred;
    meta.baseProviderHistIdx = baseProviderHistIdx;
    meta.providerInfoMissing = baseProviderHistIdx < -1;
    meta.hitHistIdx = -1;
    const auto &ids = threadState[tid].rcrIds;
    meta.bcid = ids.bcid;
    meta.pcid = ids.pcid;
    meta.pbcid = ids.pbcid;
    meta.cids[0] = contextIdForDepth(tid, 0, false);
    meta.cids[1] = contextIdForDepth(tid, 1, false);
    const unsigned numTables = tagePredictor ?
        tagePredictor->getNumPredictors() : 1;
    for (unsigned snapshotWi = 0; snapshotWi < meta.tableKeysByWi.size();
         ++snapshotWi) {
        meta.tableKeysByWi[snapshotWi].assign(numTables, 0);
        meta.tableKeyValidByWi[snapshotWi].assign(numTables, false);
        const Addr snapshotCid =
            (useOriginalRcr || adaptCtxDepth || snapshotWi != 0) ?
            meta.cids[snapshotWi] :
            contextKey(tid, startPC, entry.pc, history, asidHash);
        for (unsigned table = 0; table < numTables; ++table) {
            if (filterCode(snapshotWi, table) < 0) {
                continue;
            }
            meta.tableKeysByWi[snapshotWi][table] = patternKeyForTable(
                tid, startPC, entry.pc, snapshotCid, snapshotWi, table,
                asidHash);
            meta.tableKeyValidByWi[snapshotWi][table] = true;
        }
    }
    meta.wi = adaptCtxDepth ? selectContextDepth(meta.bcid) : 0;
#ifndef UNIT_TEST
    if (meta.wi < llbpxStats.lookupByDepthClass.size()) {
        llbpxStats.lookupByDepthClass[meta.wi]++;
    }
#endif
    meta.cid = (useOriginalRcr || adaptCtxDepth) ?
        meta.cids[meta.wi] :
        contextKey(tid, startPC, entry.pc, history, asidHash);
    meta.contextTag = meta.cid;
    auto *ctx = contexts.find(meta.cid, meta.contextTag);
    if (!ctx) {
        DPRINTF(LLBPX, "context miss start %#lx branch %#lx cid %#lx\n",
                startPC, entry.pc, meta.cid);
        return meta;
    }

    meta.contextHit = true;
#ifndef UNIT_TEST
    llbpxStats.contextHit++;
#endif
    PatternEntry *pattern = nullptr;
    bool fallbackKeyValid = false;
    for (unsigned rev = 0; rev < numTables; ++rev) {
        const unsigned table = numTables - 1 - rev;
        if (table >= meta.tableKeyValidByWi[meta.wi].size() ||
            !meta.tableKeyValidByWi[meta.wi][table]) {
            continue;
        }
        const Addr key = meta.tableKeysByWi[meta.wi][table];
        const Addr patternTag = key;
        if (!fallbackKeyValid) {
            meta.key = key;
            meta.keyTable = table;
            meta.patternTag = patternTag;
            fallbackKeyValid = true;
        }
        pattern = ctx->patternSet().get(key);
        if (pattern) {
            meta.key = key;
            meta.keyTable = table;
            meta.patternTag = patternTag;
            break;
        }
    }
    if (!pattern) {
#ifndef UNIT_TEST
        if (enableTiming) {
            llbpxStats.patternBufferDemandPatternMiss++;
        }
#endif
        DPRINTF(LLBPX, "pattern miss start %#lx branch %#lx key %#lx\n",
                startPC, entry.pc, meta.key);
        return meta;
    }

    meta.patternHit = true;
    meta.hitHistIdx = pattern->providerDepth;
    const int compareProviderDepth = meta.providerInfoMissing ?
                                     -1 : meta.baseProviderHistIdx;
    meta.providerDepthEligible = compareProviderDepth < 0 ||
                                 meta.hitHistIdx >= compareProviderDepth;
    meta.llbpxPred = pattern->taken();
#ifndef UNIT_TEST
    llbpxStats.patternHit++;
    if (meta.providerInfoMissing) {
        llbpxStats.providerFallback++;
    }
#endif
    if (enableTiming) {
        auto *bufferEntry = findPatternBuffer(tid, meta.cid);
        if (!bufferEntry) {
#ifndef UNIT_TEST
            llbpxStats.patternBufferMiss++;
#endif
            rememberPatternBuffer(tid, meta.cid, false,
                                  llbpxCurrentTick() + patternBufferLatency,
                                  false);
            meta.providerTimingReady = false;
        } else if (bufferEntry->readyTick > llbpxCurrentTick()) {
#ifndef UNIT_TEST
            llbpxStats.patternBufferNotReady++;
#endif
            meta.providerTimingReady = false;
        } else {
#ifndef UNIT_TEST
            llbpxStats.patternBufferHit++;
#endif
            meta.providerTimingReady = true;
            usePatternBuffer(tid, meta.cid);
            meta.patternBufferInFlight = true;
        }
    } else {
        meta.providerTimingReady = true;
    }
    DPRINTF(LLBPX,
            "pattern hit start %#lx branch %#lx key %#lx ctr %d pred %d "
            "llbpx depth %d tage depth %d depth-ok %d timing-ok %d\n",
            startPC, entry.pc, meta.key, pattern->counter, meta.llbpxPred,
            meta.hitHistIdx, meta.baseProviderHistIdx,
            meta.providerDepthEligible, meta.providerTimingReady);
    return meta;
}

void
BTBLLBPX::specUpdateGHist(const boost::dynamic_bitset<> &history,
                          FullBTBPrediction &pred,
                          const DirectionHistoryUpdate &update)
{
    if (pred.tid >= threadState.size()) {
        return;
    }

    auto meta = threadState[pred.tid].meta;
    if (!meta) {
        return;
    }

    const auto takenEntry = pred.getTakenEntry();
    if (!takenEntry.valid) {
        meta->rcrUpdated = true;
        return;
    }

    pushRCR(pred.tid, takenEntry, true);
    meta->rcrUpdated = true;
}

void
BTBLLBPX::recoverHist(const boost::dynamic_bitset<> &history,
                      const FetchTarget &entry, int shamt,
                      bool cond_taken)
{
    auto meta = std::static_pointer_cast<LLBPXMeta>(entry.predMetas[getComponentIdx()]);
    if (!meta || entry.tid >= threadState.size()) {
        return;
    }
    for (const auto &[pc, branchMeta] : meta->branches) {
        if (branchMeta.patternBufferInFlight) {
            squashPatternBuffer(entry.tid, branchMeta.cid);
        }
    }
    restoreRCR(entry.tid, *meta);
    if (entry.exeTaken) {
        pushRCR(entry.tid, entry.exeBranchInfo, true);
        DPRINTF(LLBPX,
                "recover RCR with actual taken branch pc %#lx target %#lx\n",
                entry.exeBranchInfo.pc, entry.exeBranchInfo.target);
    }
#ifndef UNIT_TEST
    llbpxStats.rcrRecover++;
#endif
}

void
BTBLLBPX::recoverPHist(const boost::dynamic_bitset<> &history,
                       const FetchTarget &entry,
                       const PathHistoryUpdate &update)
{
}

std::vector<BTBEntry>
BTBLLBPX::prepareUpdateEntries(const FetchTarget &entry) const
{
    auto allEntries = entry.updateBTBEntries;

    if (!entry.updateIsOldEntry) {
        BTBEntry potentialNewEntry = entry.updateNewBTBEntry;
        const bool newEntryTaken =
            entry.exeTaken && entry.getControlPC() == potentialNewEntry.pc;
        if (!newEntryTaken) {
            potentialNewEntry.alwaysTaken = false;
        }
        allEntries.push_back(potentialNewEntry);
    }

    auto removeIt = std::remove_if(allEntries.begin(), allEntries.end(),
        [](const BTBEntry &btbEntry) {
            return !(btbEntry.valid && btbEntry.isCond && !btbEntry.alwaysTaken);
        });
    allEntries.erase(removeIt, allEntries.end());
    return allEntries;
}

void
BTBLLBPX::onTageAllocation(const FetchTarget &entry, const BTBEntry &btbEntry,
                           const std::vector<unsigned> &providerDepths,
                           bool actualTaken)
{
    auto meta = std::static_pointer_cast<LLBPXMeta>(
        entry.predMetas[getComponentIdx()]);
    if (!meta) {
        return;
    }

    auto it = meta->branches.find(btbEntry.pc);
    if (it == meta->branches.end()) {
        BranchMeta synthesizedMeta;
        if (synthesizeMissingBranchMeta(entry, *meta, btbEntry, synthesizedMeta)) {
            auto insertResult =
                meta->branches.emplace(btbEntry.pc, std::move(synthesizedMeta));
            it = insertResult.first;
        }
    }
    if (it != meta->branches.end()) {
#ifndef UNIT_TEST
        llbpxStats.directTageAllocWithMeta++;
        llbpxStats.directTageAllocCalls++;
#endif
        it->second.trace.tageAllocTriggered = true;
        it->second.trace.tageAllocTableMask = tableMaskFor(providerDepths);
        allocateFor(it->second, actualTaken, providerDepths);
        return;
    }

#ifndef UNIT_TEST
    llbpxStats.directTageAllocCalls++;
    llbpxStats.directTageAllocNoMeta++;
#endif
}

void
BTBLLBPX::update(const FetchTarget &entry)
{
#ifndef UNIT_TEST
    llbpxStats.updateCalls++;
#endif
    auto meta = std::static_pointer_cast<LLBPXMeta>(entry.predMetas[getComponentIdx()]);
    if (!meta) {
        DPRINTF(LLBPX, "update skip: no meta for start %#lx\n", entry.startPC);
        return;
    }

    const auto updateEntries = prepareUpdateEntries(entry);
    for (const auto &btbEntry : updateEntries) {
#ifndef UNIT_TEST
        llbpxStats.updateEntriesSeen++;
#endif
        const bool actualTaken = entry.exeTaken && entry.exeBranchInfo == btbEntry;
        auto it = meta->branches.find(btbEntry.pc);
        if (it == meta->branches.end()) {
            BranchMeta synthesizedMeta;
            if (synthesizeMissingBranchMeta(entry, *meta, btbEntry,
                                            synthesizedMeta)) {
                auto insertResult = meta->branches.emplace(
                    btbEntry.pc, std::move(synthesizedMeta));
                it = insertResult.first;
            }
        }
        if (it == meta->branches.end()) {
            continue;
        }
        auto *branchMeta = &it->second;
#ifndef UNIT_TEST
        llbpxStats.updateEntriesWithMeta++;
#endif

        if (branchMeta->providerUsed) {
            updatePattern(*branchMeta, actualTaken);
        }

        if (branchMeta->patternBufferInFlight) {
            commitPatternBuffer(entry.tid, branchMeta->cid);
        }
#ifndef UNIT_TEST
        writeTraceRecord(*branchMeta, actualTaken);
#endif
    }
}

bool
BTBLLBPX::getProviderUpdateInfo(const FetchTarget &entry, Addr branchPC,
                                bool &llbpxPred, int &providerDepth)
{
    auto meta = std::static_pointer_cast<LLBPXMeta>(
        entry.predMetas[getComponentIdx()]);
    if (!meta) {
        return false;
    }

    auto it = meta->branches.find(branchPC);
    if (it == meta->branches.end() || !it->second.providerUsed) {
        return false;
    }

    llbpxPred = it->second.llbpxPred;
    providerDepth = it->second.hitHistIdx;
    return true;
}

void
BTBLLBPX::allocateFor(BranchMeta &meta, bool actualTaken,
                      const std::vector<unsigned> &providerDepths)
{
    if (providerDepths.empty()) {
        return;
    }

    unsigned targetWi = meta.wi;
    Addr targetCid = meta.cid;
    if (adaptCtxDepth) {
        targetWi = adaptContextDepth(meta, providerDepths);
        targetCid = meta.cids[targetWi];
    }
    meta.trace.allocTargetWi = targetWi;
    meta.trace.allocTargetCid = targetCid;

    std::vector<unsigned> allocTables;
    allocTables.reserve(providerDepths.size());
    const unsigned numTables = tagePredictor ?
        tagePredictor->getNumPredictors() : 1;
    for (auto providerDepth : providerDepths) {
        const unsigned table = std::min(providerDepth, numTables - 1);
        if (filterCode(targetWi, table) >= 0) {
            allocTables.push_back(table);
        }
    }
    if (allocTables.empty()) {
        return;
    }

    const Addr contextTag = targetCid;
    auto *ctx = contexts.find(targetCid, contextTag);
    if (!ctx) {
        ctx = &contexts.allocate(targetCid, contextTag);
        ctx->patternKey = patternKey(targetCid, meta.branchPC, meta.asidHash);
        meta.trace.allocContextCreated++;
#ifndef UNIT_TEST
        llbpxStats.allocContext++;
#endif
        DPRINTF(LLBPX, "alloc context branch %#lx cid %#lx tag %#lx\n",
                meta.branchPC, targetCid, contextTag);
    }
#ifndef UNIT_TEST
    else {
        meta.trace.allocContextRevisit++;
        llbpxStats.allocContextRevisit++;
    }
#endif

    for (auto table : allocTables) {
        if (targetWi >= meta.tableKeysByWi.size() ||
            table >= meta.tableKeysByWi[targetWi].size() ||
            table >= meta.tableKeyValidByWi[targetWi].size() ||
            !meta.tableKeyValidByWi[targetWi][table]) {
            DPRINTF(LLBPX,
                    "skip alloc branch %#lx wi %u table %u due to missing "
                    "prediction-time key snapshot\n",
                    meta.branchPC, targetWi, table);
            meta.trace.allocSkipNoKey++;
            continue;
        }
        const Addr key = meta.tableKeysByWi[targetWi][table];
        const Addr ptag = key;
        auto *pattern = ctx->patternSet().get(key);
        const int allocDepth = static_cast<int>(table);
        if (!pattern) {
            ctx->sortPatterns(key);
            pattern = ctx->patternSet().insert(key);
            pattern->tag = ptag;
            pattern->counter = actualTaken ? 0 : -1;
            pattern->providerDepth = allocDepth;
            meta.trace.allocPatternCreated++;
            if (table < 64) {
                meta.trace.allocTableMask |= 1ULL << table;
            }
#ifndef UNIT_TEST
            llbpxStats.allocPattern++;
            if (table < llbpxStats.allocPatternByTable.size()) {
                llbpxStats.allocPatternByTable[table]++;
            }
            if (targetWi < llbpxStats.allocPatternByDepthClass.size()) {
                llbpxStats.allocPatternByDepthClass[targetWi]++;
            }
#endif
            DPRINTF(LLBPX,
                    "alloc pattern branch %#lx key %#lx tag %#lx ctr %d depth %d\n",
                    meta.branchPC, key, ptag, pattern->counter,
                    pattern->providerDepth);
        } else {
            meta.trace.allocPatternRevisit++;
#ifndef UNIT_TEST
            llbpxStats.allocPatternRevisit++;
#endif
            if (allocDepth > pattern->providerDepth) {
                pattern->providerDepth = allocDepth;
            }
        }
    }
    rememberPatternBuffer(meta.tid, targetCid, true, llbpxCurrentTick(), false);
}

void
BTBLLBPX::updatePattern(BranchMeta &meta, bool actualTaken)
{
    meta.trace.updatePatternCalled = true;
    auto *ctx = contexts.find(meta.cid, meta.contextTag);
    if (!ctx) {
        return;
    }
    auto *pattern = ctx->patternSet().get(meta.key);
    if (!pattern) {
        return;
    }
    meta.trace.updatePatternFound = true;
    const short counterBefore = pattern->counter;
    const uint64_t counterBias = 1ULL << (counterBits - 1);
    meta.trace.counterBias = counterBias;
    meta.trace.counterBeforeBiased =
        static_cast<uint64_t>(counterBefore + static_cast<short>(counterBias));
    updateCounter(actualTaken, pattern->counter);
    meta.trace.counterAfterBiased =
        static_cast<uint64_t>(pattern->counter + static_cast<short>(counterBias));
    updateContextTracking(meta, counterBefore, pattern->counter, actualTaken);
#ifndef UNIT_TEST
    llbpxStats.updatePattern++;
#endif
    rememberPatternBuffer(meta.tid, meta.cid, true, llbpxCurrentTick(), false);
    DPRINTF(LLBPX, "update pattern branch %#lx key %#lx actual %d ctr %d depth %d\n",
            meta.branchPC, meta.key, actualTaken, pattern->counter,
            pattern->providerDepth);
}

BTBLLBPX::PatternBufferEntry *
BTBLLBPX::findPatternBuffer(ThreadID tid, Addr cid)
{
    if (tid >= threadState.size()) {
        return nullptr;
    }
    auto &buffer = threadState[tid].patternBuffer;
    auto it = std::find_if(buffer.begin(), buffer.end(),
        [cid](const PatternBufferEntry &entry) {
            return entry.cid == cid;
        });
    return it == buffer.end() ? nullptr : &(*it);
}

bool
BTBLLBPX::rememberPatternBuffer(ThreadID tid, Addr cid, bool dirty,
                                Tick readyTick, bool isPrefetch)
{
    if (tid >= threadState.size() || patternBufferSize == 0) {
        return false;
    }
    auto &buffer = threadState[tid].patternBuffer;
    auto *entry = findPatternBuffer(tid, cid);
    if (entry) {
        entry->dirty = entry->dirty || dirty;
        entry->readyTick = std::min(entry->readyTick, readyTick);
        entry->lastUsed = llbpxCurrentTick();
#ifndef UNIT_TEST
        if (isPrefetch) {
            llbpxStats.patternBufferPrefetchHit++;
        }
#endif
        return true;
    }
    if (buffer.size() >= patternBufferSize) {
        auto victim = std::find_if(buffer.begin(), buffer.end(),
            [](const PatternBufferEntry &entry) {
                return !entry.locked && entry.dependants == 0;
            });
        if (victim == buffer.end()) {
#ifndef UNIT_TEST
            if (isPrefetch) {
                llbpxStats.patternBufferPrefetchDrop++;
            }
#endif
            return false;
        }
        auto lruVictim = victim;
        for (auto it = victim; it != buffer.end(); ++it) {
            if (!it->locked && it->dependants == 0 &&
                it->lastUsed < lruVictim->lastUsed) {
                lruVictim = it;
            }
        }
        buffer.erase(lruVictim);
    }
    buffer.push_back(PatternBufferEntry{
        cid,
        llbpxCurrentTick(),
        dirty,
        false,
        false,
        false,
        0,
        readyTick});
#ifndef UNIT_TEST
    llbpxStats.patternBufferInstall++;
    if (isPrefetch) {
        llbpxStats.patternBufferPrefetchIssue++;
    }
#endif
    return true;
}

void
BTBLLBPX::usePatternBuffer(ThreadID tid, Addr cid)
{
    auto *entry = findPatternBuffer(tid, cid);
    if (!entry) {
        return;
    }
    entry->used = true;
    entry->dependants++;
    entry->lastUsed = llbpxCurrentTick();
}

void
BTBLLBPX::commitPatternBuffer(ThreadID tid, Addr cid)
{
    auto *entry = findPatternBuffer(tid, cid);
    if (!entry) {
        return;
    }
    entry->usedRetired = true;
    if (entry->dependants > 0) {
        entry->dependants--;
    }
    entry->lastUsed = llbpxCurrentTick();
}

void
BTBLLBPX::squashPatternBuffer(ThreadID tid, Addr cid)
{
    auto *entry = findPatternBuffer(tid, cid);
    if (!entry) {
        return;
    }
    if (entry->dependants > 0) {
        entry->dependants--;
    }
    entry->lastUsed = llbpxCurrentTick();
}

void
BTBLLBPX::restoreRCR(ThreadID tid, const LLBPXMeta &meta)
{
    threadState[tid].rcr = meta.rcrSnapshot;
    recomputeRCRContextIds(tid);
}

void
BTBLLBPX::pushRCR(ThreadID tid, const BranchInfo &branch, bool taken)
{
    if (tid >= threadState.size() || rcrEntries == 0) {
        return;
    }
    if (!shouldRecordRCR(branch, taken)) {
        return;
    }
    auto &rcr = threadState[tid].rcr;
    if (rcr.size() >= rcrEntries) {
        rcr.pop_front();
    }
    rcr.push_back(RCRRecord{
        branch.pc >> InstShiftAmt,
        branch.target >> InstShiftAmt,
        taken,
        branch.isCond,
        branch.isCall,
        branch.isReturn});
    recomputeRCRContextIds(tid);
    prefetchContext(tid);
}

bool
BTBLLBPX::shouldRecordRCR(const BranchInfo &branch, bool taken) const
{
    switch (rcrType) {
      case RCRType::AllBranches:
        return true;
      case RCRType::CallsOnly:
        return branch.isCall;
      case RCRType::CallsAndReturns:
        return branch.isCall || branch.isReturn;
      case RCRType::UnconditionalOnly:
        return branch.isUncond();
      case RCRType::AllTakenBranches:
        return taken;
    }
    return false;
}

Addr
BTBLLBPX::calcRCRHash(ThreadID tid, unsigned n, unsigned skip, unsigned shift,
                      unsigned outBits) const
{
    if (tid >= threadState.size() || outBits == 0) {
        return 0;
    }
    const auto &rcr = threadState[tid].rcr;
    if (rcr.size() < (skip + n)) {
        return 0;
    }

    Addr hash = 0;
    unsigned sh = 0;
    auto it = rcr.rbegin();
    std::advance(it, skip);
    for (; it != rcr.rend() && n > 0; ++it, --n) {
        hash ^= (it->pc << sh);
        sh += shift;
        if (sh >= outBits) {
            sh -= outBits;
        }
    }
    return hash & mask(outBits);
}

void
BTBLLBPX::recomputeRCRContextIds(ThreadID tid)
{
    if (tid >= threadState.size()) {
        return;
    }
    auto &ids = threadState[tid].rcrIds;
    ids.ccid = calcRCRHash(tid, rcrWindow, rcrDist, rcrShift, rcrTagBits);
    ids.pcid = calcRCRHash(tid, rcrWindow, 0, rcrShift, rcrTagBits);
    constexpr unsigned baseWindow = 2;
    ids.bcid = calcRCRHash(tid, baseWindow, rcrDist, rcrShift, rcrBaseTagBits);
    ids.pbcid = calcRCRHash(tid, baseWindow, 0, rcrShift, rcrBaseTagBits);
}

bool
BTBLLBPX::contextExistsForPrefetch(Addr cid) const
{
    return contexts.find(cid, cid) != nullptr;
}

void
BTBLLBPX::prefetchContext(ThreadID tid)
{
    if (!enableTiming || patternBufferSize == 0 ||
        tid >= threadState.size()) {
        return;
    }
    const Addr baseCtx = threadState[tid].rcrIds.pbcid;
    const unsigned wi = adaptCtxDepth ? selectContextDepth(baseCtx) : 0;
    const Addr cid = contextIdForDepth(tid, wi, true);
    if (cid == 0 || !contextExistsForPrefetch(cid)) {
        return;
    }
    rememberPatternBuffer(tid, cid, false,
                          llbpxCurrentTick() + patternBufferLatency, true);
}

void
BTBLLBPX::initFilterTables()
{
    constexpr unsigned bucketAssoc = 4;
    const auto encodeBucket = [](unsigned ordinal) -> int {
        const unsigned bucket = (ordinal / bucketAssoc) % bucketAssoc;
        return static_cast<int>((ordinal << 2) | bucket);
    };

    for (auto &table : fltTables) {
        table.clear();
    }
    if (!tagePredictor) {
        return;
    }

    const unsigned numTables = tagePredictor->getNumPredictors();
    for (auto &table : fltTables) {
        table.assign(numTables, -1);
    }

    if (!adaptCtxDepth) {
        // Original non-adaptive LLBP uses a sparse history subset instead of
        // all available TAGE tables. With the local 8-table TAGE, skip the
        // shortest history and preserve grouped bucket encoding.
        unsigned ordinal = 0;
        for (unsigned table = 0; table < numTables; ++table) {
            if (tagePredictor->getHistoryLength(table) < 8) {
                continue;
            }
            fltTables[0][table] = encodeBucket(ordinal++);
        }
        return;
    }

    // Adaptive mode follows original LLBP-X semantics:
    // shallow contexts admit histories up to 22, deep contexts admit
    // histories from 12 onward, and deep ordinals continue after shallow.
    unsigned ordinal = 0;
    for (unsigned table = 0; table < numTables; ++table) {
        const unsigned histLen = tagePredictor->getHistoryLength(table);
        if (histLen <= histLenThreshold) {
            fltTables[0][table] = encodeBucket(ordinal++);
        }
    }

    for (unsigned table = 0; table < numTables; ++table) {
        const unsigned histLen = tagePredictor->getHistoryLength(table);
        if (histLen >= 12) {
            fltTables[1][table] = encodeBucket(ordinal++);
        }
    }
}

unsigned
BTBLLBPX::selectContextDepth(Addr baseCtx) const
{
    if (!adaptCtxDepth) {
        return 0;
    }
    auto *info = ctt.find(baseCtx, baseCtx);
    if (!info || info->fullPatternSets == 0) {
        return 0;
    }
    return std::min<unsigned>(info->wi, 1);
}

Addr
BTBLLBPX::contextIdForDepth(ThreadID tid, unsigned wi, bool prefetch) const
{
    const unsigned window = wi == 0 ? shallowContextDepth : deepContextDepth;
    const unsigned skip = prefetch ? 0 : rcrDist;
    return calcRCRHash(tid, window, skip, rcrShift, rcrTagBits);
}

int
BTBLLBPX::filterCode(unsigned wi, unsigned table) const
{
    if (!tagePredictor) {
        return table == 0 ? 0 : -1;
    }
    if (wi >= fltTables.size() || table >= fltTables[wi].size()) {
        return -1;
    }
    return fltTables[wi][table];
}

void
BTBLLBPX::updateContextTracking(const BranchMeta &meta, short counterBefore,
                                short counterAfter, bool actualTaken)
{
    auto *ctx = contexts.find(meta.cid, meta.contextTag);
    if (!ctx) {
        return;
    }

    auto *info = ctt.find(meta.bcid, meta.bcid);
    if (counterAfter == (actualTaken ? 1 : -2)) {
        ctx->confidence = std::min<unsigned>(ctx->confidence + 1, 255);
        if (adaptCtxDepth && ctx->confidentPatterns == trackingThreshold) {
            if (!info) {
                info = &ctt.allocate(meta.bcid, meta.bcid);
#ifndef UNIT_TEST
                llbpxStats.cttEntryCreate++;
#endif
            }
            if (info->fullPatternSets < 2) {
                info->fullPatternSets++;
            }
        }
        ctx->confidentPatterns++;
    } else if (counterAfter == (actualTaken ? -1 : 0)) {
        if (ctx->confidence > 0) {
            ctx->confidence--;
        }
        if (adaptCtxDepth &&
            ctx->confidentPatterns + 1 == trackingThreshold) {
            if (!info) {
                info = &ctt.allocate(meta.bcid, meta.bcid);
#ifndef UNIT_TEST
                llbpxStats.cttEntryCreate++;
#endif
            }
            if (info->fullPatternSets > 0) {
                info->fullPatternSets--;
            }
        }
        if (ctx->confidentPatterns > 0) {
            ctx->confidentPatterns--;
        }
    }

    if (counterBefore != counterAfter) {
        rememberPatternBuffer(meta.tid, meta.cid, true, llbpxCurrentTick(),
                              false);
    }
}

unsigned
BTBLLBPX::adaptContextDepth(const BranchMeta &meta,
                            const std::vector<unsigned> &providerTables)
{
    if (!adaptCtxDepth || !tagePredictor) {
        return meta.wi;
    }

    auto *info = ctt.find(meta.bcid, meta.bcid);
    if (!info) {
#ifndef UNIT_TEST
        llbpxStats.cttLookupMiss++;
#endif
        return meta.wi;
    }

    const unsigned oldWi = std::min<unsigned>(info->wi, 1);

    for (auto providerTable : providerTables) {
        const unsigned table = std::min(providerTable,
            tagePredictor->getNumPredictors() - 1);
        const unsigned histLen = tagePredictor->getHistoryLength(table);
        if (histLen < histLenThreshold) {
            if (info->wi == 0) {
                if (info->allocVsDrop > 0) {
                    info->allocVsDrop--;
                }
            } else {
                info->allocVsDrop++;
            }
        } else {
            if (info->wi == 1) {
                if (info->allocVsDrop > 0) {
                    info->allocVsDrop--;
                }
            } else {
                info->allocVsDrop++;
            }
        }
    }

    if (info->fullPatternSets == 0) {
        info->wi = 0;
    } else if (info->allocVsDrop > adaptThreshold) {
        info->wi = info->wi == 0 ? 1 : 0;
        info->allocVsDrop = 0;
    }

    const unsigned newWi = std::min<unsigned>(info->wi, 1);
#ifndef UNIT_TEST
    if (newWi != oldWi) {
        if (newWi == 0) {
            llbpxStats.cttDepthSwitchToShallow++;
        } else {
            llbpxStats.cttDepthSwitchToDeep++;
        }
    }
#endif
    return newWi;
}

Addr
BTBLLBPX::hashBits(const boost::dynamic_bitset<> &history, unsigned bits) const
{
    Addr value = 0;
    const unsigned limit = std::min<unsigned>(history.size(), bits);
    for (unsigned i = 0; i < limit; ++i) {
        if (history[i]) {
            value ^= mix(static_cast<Addr>(i + 1));
        }
    }
    return value;
}

Addr
BTBLLBPX::mix(Addr value) const
{
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33;
    return value;
}

Addr
BTBLLBPX::contextKey(ThreadID tid, Addr startPC, Addr branchPC,
                     const boost::dynamic_bitset<> &history,
                     uint8_t asidHash) const
{
    if (useOriginalRcr) {
        return originalContextKey(tid, branchPC, rcrWindow, rcrDist);
    }
    Addr key = mix(startPC >> 1) ^ mix(branchPC << 1) ^
               mix(static_cast<Addr>(asidHash) << 48) ^ hashBits(history, keyBits);

    const auto &rcr = threadState[tid].rcr;
    unsigned idx = 0;
    for (const auto &record : rcr) {
        key ^= mix(record.pc + (record.target << 1) + (record.taken ? 0x9e37 : 0) + idx);
        idx++;
    }
    return key & mask(keyBits);
}

Addr
BTBLLBPX::originalContextKey(ThreadID tid, Addr branchPC, unsigned window,
                             unsigned dist) const
{
    return calcRCRHash(tid, window, dist, rcrShift, rcrTagBits) &
           mask(keyBits);
}

Addr
BTBLLBPX::patternKey(Addr contextKey, Addr branchPC, uint8_t asidHash) const
{
    return mix(contextKey ^ (branchPC >> 1) ^ (static_cast<Addr>(asidHash) << 7)) &
           mask(keyBits);
}

Addr
BTBLLBPX::patternKeyForTable(ThreadID tid, Addr startPC, Addr branchPC,
                             Addr contextKey, unsigned wi, unsigned table,
                             uint8_t asidHash) const
{
    const int code = filterCode(wi, table);
    const Addr lowBits = static_cast<Addr>(std::max(code, 0)) & mask(10);
    if (!tagePredictor) {
        const Addr base = patternKey(contextKey, branchPC, asidHash);
        return (((base & mask(keyBits)) << 10) | lowBits) & mask(keyBits);
    }
    const Addr base = tagePredictor->getLlbpxPatternKey(
        tid, startPC, branchPC, table, contextKey, asidHash);
    return (((base & mask(keyBits)) << 10) | lowBits) & mask(keyBits);
}

Addr
BTBLLBPX::tagFromKey(Addr key) const
{
    return mix(key) & mask(tagBits);
}

void
BTBLLBPX::updateCounter(bool taken, short &counter) const
{
    const short max = (1 << (counterBits - 1)) - 1;
    const short min = -(1 << (counterBits - 1));
    if (taken && counter < max) {
        counter++;
    } else if (!taken && counter > min) {
        counter--;
    }
}

#ifdef UNIT_TEST
} // namespace test
#endif

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5
