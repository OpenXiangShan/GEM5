#include "cpu/pred/btb/pairtage.hh"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <numeric>
#include <string>

#include "base/intmath.hh"

#ifndef UNIT_TEST
#include "base/trace.hh"

#endif

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

#ifdef UNIT_TEST
namespace test
{
#endif

void
PairTAGE::TageEntry::setBlock(unsigned blockIdx, const PairBlockInfo &block)
{
    assert(blockIdx < NumBlocksPerEntry);
    blocks[blockIdx] = block;
}

void
PairTAGE::TageEntry::clearBlock(unsigned blockIdx)
{
    assert(blockIdx < NumBlocksPerEntry);
    blocks[blockIdx].clear();
}

unsigned
PairTAGE::TageEntry::numValidBlocks() const
{
    unsigned validBlocks = 0;
    for (const auto &block : blocks) {
        if (block.valid) {
            ++validBlocks;
        }
    }
    return validBlocks;
}

#ifdef UNIT_TEST
namespace
{

std::vector<unsigned>
defaultHistLengths(unsigned numPredictors)
{
    std::vector<unsigned> lengths(numPredictors);
    for (unsigned i = 0; i < numPredictors; ++i) {
        lengths[i] = (i + 1) * 4;
    }
    return lengths;
}

}  // anonymous namespace

PairTAGE::PairTAGE(unsigned numPredictors, unsigned numWays, unsigned tableSize)
    : TimedBaseBTBPredictor(),
      numPredictors(numPredictors),
      tableSizes(numPredictors, tableSize),
      tableIndexBits(numPredictors),
      tableTagBits(numPredictors, 16),
      tablePcShifts(numPredictors, 1),
      histLengths(defaultHistLengths(numPredictors)),
      maxHistLen(histLengths.empty() ? 0 : histLengths.back()),
      numTablesToAlloc(1),
      numWays(numWays),
      maxBranchPositions(32),
      enableSecondBlock(true),
      allowOddPhase(false),
      trainStandaloneFallThrough(false)
{
    setNumDelay(0);
    needMoreHistories = true;
    blockSize = 32;

    tageTable.resize(numPredictors);
    for (unsigned i = 0; i < numPredictors; ++i) {
        tageTable[i].resize(tableSizes[i]);
        for (unsigned j = 0; j < tableSizes[i]; ++j) {
            tageTable[i][j].resize(numWays);
        }
        tableIndexBits[i] = ceilLog2(tableSizes[i]);
        tagFoldedHist.emplace_back(histLengths[i], tableTagBits[i], 16);
        altTagFoldedHist.emplace_back(histLengths[i], tableTagBits[i] - 1, 16);
        indexFoldedHist.emplace_back(histLengths[i], tableIndexBits[i], 16);
    }
}
#else
PairTAGE::PairTAGE(const Params &p)
    : TimedBaseBTBPredictor(p),
      numPredictors(p.numPredictors),
      tableSizes(p.tableSizes),
      tableIndexBits(p.numPredictors),
      tableTagBits(p.TTagBitSizes),
      tablePcShifts(p.TTagPcShifts),
      histLengths(p.histLengths),
      maxHistLen(p.maxHistLen),
      numTablesToAlloc(p.numTablesToAlloc),
      numWays(p.numWays),
      maxBranchPositions(p.maxBranchPositions),
      enableSecondBlock(p.enableSecondBlock),
      allowOddPhase(p.allowOddPhase),
      trainStandaloneFallThrough(p.trainStandaloneFallThrough),
      pairTageStats(this, numPredictors, numWays, tableSizes)
{
    needMoreHistories = p.needMoreHistories;

    if (tablePcShifts.size() < numPredictors) {
        tablePcShifts.resize(numPredictors, 1);
    }

    assert(tableSizes.size() >= numPredictors);
    assert(tableTagBits.size() >= numPredictors);
    assert(histLengths.size() >= numPredictors);

    tageTable.resize(numPredictors);
    for (unsigned i = 0; i < numPredictors; ++i) {
        tageTable[i].resize(tableSizes[i]);
        for (unsigned j = 0; j < tableSizes[i]; ++j) {
            tageTable[i][j].resize(numWays);
        }
        tableIndexBits[i] = ceilLog2(tableSizes[i]);
        tagFoldedHist.emplace_back(histLengths[i], tableTagBits[i], 16);
        altTagFoldedHist.emplace_back(histLengths[i], tableTagBits[i] - 1, 16);
        indexFoldedHist.emplace_back(histLengths[i], tableIndexBits[i], 16);
    }
}
#endif

PairTAGE::~PairTAGE() = default;

#ifndef UNIT_TEST
PairTAGE::PairTageStats::PairTageStats(
    statistics::Group *parent, unsigned numPredictors, unsigned numWays,
    const std::vector<unsigned> &tableSizes)
    : statistics::Group(parent),
      ADD_STAT(predCalls, statistics::units::Count::get(),
               "prediction lookups issued to PairTAGE"),
      ADD_STAT(predOddPhaseSkipped, statistics::units::Count::get(),
               "prediction lookups skipped because PairTAGE phase is Odd"),
      ADD_STAT(predHit, statistics::units::Count::get(),
               "prediction lookups that hit a valid PairTAGE first block"),
      ADD_STAT(predMiss, statistics::units::Count::get(),
               "prediction lookups that missed in PairTAGE"),
      ADD_STAT(predSecondBlockAvailable, statistics::units::Count::get(),
               "prediction hits that also carried a valid second block"),
      ADD_STAT(refreshCalls, statistics::units::Count::get(),
               "second-boundary meta refresh lookups issued to PairTAGE"),
      ADD_STAT(refreshOddPhaseSkipped, statistics::units::Count::get(),
               "meta refresh lookups skipped because PairTAGE phase is Odd"),
      ADD_STAT(refreshHit, statistics::units::Count::get(),
               "meta refresh lookups that hit a valid PairTAGE first block"),
      ADD_STAT(refreshMiss, statistics::units::Count::get(),
               "meta refresh lookups that missed in PairTAGE"),
      ADD_STAT(trainCalls, statistics::units::Count::get(),
               "training calls issued to PairTAGE"),
      ADD_STAT(trainOddPhaseSkipped, statistics::units::Count::get(),
               "training calls skipped because the FTQ entry phase is Odd"),
      ADD_STAT(trainNoMeta, statistics::units::Count::get(),
               "training calls skipped because prediction meta is unavailable"),
      ADD_STAT(trainProviderHit, statistics::units::Count::get(),
               "training calls whose provider lookup hit an existing entry"),
      ADD_STAT(trainProviderMiss, statistics::units::Count::get(),
               "training calls whose provider lookup missed and required allocation"),
      ADD_STAT(trainFirstBlockValid, statistics::units::Count::get(),
               "training calls with a valid first block to install"),
      ADD_STAT(trainFirstBlockInvalid, statistics::units::Count::get(),
               "training calls whose first block could not be formed"),
      ADD_STAT(trainFirstBlockInvalidNoTakenEntry,
               statistics::units::Count::get(),
               "invalid first-block trains with no taken entry to encode"),
      ADD_STAT(trainFirstBlockInvalidNoNotTakenDirectCond,
               statistics::units::Count::get(),
               "invalid first-block trains with no not-taken direct conditional"),
      ADD_STAT(trainFirstBlockInvalidFilteredIndirect,
               statistics::units::Count::get(),
               "invalid first-block trains filtered by indirect branches"),
      ADD_STAT(trainFirstBlockInvalidFilteredCall,
               statistics::units::Count::get(),
               "invalid first-block trains filtered by call entries"),
      ADD_STAT(trainFirstBlockInvalidFilteredReturn,
               statistics::units::Count::get(),
               "invalid first-block trains filtered by return entries"),
      ADD_STAT(trainFirstBlockInvalidUnsupportedFormat,
               statistics::units::Count::get(),
               "invalid first-block trains rejected by unsupported formats"),
      ADD_STAT(trainSecondBlockValid, statistics::units::Count::get(),
               "training calls with a valid second block to install"),
      ADD_STAT(trainFallThroughSkippedNoSecondBlock,
               statistics::units::Count::get(),
               "fallthrough first-block trains skipped because no second block was available"),
      ADD_STAT(clearEntryOnInvalidTrain, statistics::units::Count::get(),
               "existing provider entries cleared because no valid first block was trainable"),
      ADD_STAT(updateExistingProvider, statistics::units::Count::get(),
               "writes that updated an existing provider entry in place"),
      ADD_STAT(allocIntoInvalidSlot, statistics::units::Count::get(),
               "allocations that found an invalid way"),
      ADD_STAT(allocOverwriteValid, statistics::units::Count::get(),
               "allocations that had to overwrite a valid entry"),
      ADD_STAT(allocOverwriteValidSecondBlock, statistics::units::Count::get(),
               "allocations that overwrote a valid entry carrying a second block"),
      ADD_STAT(allocFailureNoCandidate, statistics::units::Count::get(),
               "allocations that could not find a victim in higher tables"),
      ADD_STAT(usefulReset, statistics::units::Count::get(),
               "global useful-bit resets triggered by allocation pressure"),
      ADD_STAT(installSecondBlock, statistics::units::Count::get(),
               "writes that installed a second block into the table"),
      ADD_STAT(clearSecondBlock, statistics::units::Count::get(),
               "writes that removed a previously stored second block"),
      ADD_STAT(liveValidEntries, statistics::units::Count::get(),
               "current number of valid PairTAGE entries"),
      ADD_STAT(liveSecondBlockEntries, statistics::units::Count::get(),
               "current number of PairTAGE entries carrying a valid second block"),
      ADD_STAT(predTableHits, statistics::units::Count::get(),
               "prediction hits per PairTAGE table"),
      ADD_STAT(trainProviderTableHits, statistics::units::Count::get(),
               "provider hits per PairTAGE table during training"),
      ADD_STAT(allocTableInstalls, statistics::units::Count::get(),
               "new allocations installed per PairTAGE table"),
      ADD_STAT(tableWrites, statistics::units::Count::get(),
               "writes per PairTAGE table"),
      ADD_STAT(tableOverwrites, statistics::units::Count::get(),
               "valid-entry overwrites per PairTAGE table"),
      ADD_STAT(liveValidEntriesPerTable, statistics::units::Count::get(),
               "current valid entries per PairTAGE table"),
      ADD_STAT(liveSecondBlockEntriesPerTable, statistics::units::Count::get(),
               "current second-block-carrying entries per PairTAGE table"),
      ADD_STAT(liveOccupancyRate, statistics::units::Ratio::get(),
               "current live PairTAGE occupancy ratio"),
      ADD_STAT(liveSecondBlockEntryRate, statistics::units::Ratio::get(),
               "current ratio of second-block-carrying entries over all PairTAGE slots")
{
    const auto totalTableSlots =
        std::accumulate(tableSizes.begin(), tableSizes.end(), uint64_t(0)) * numWays;

    predTableHits.init(numPredictors);
    trainProviderTableHits.init(numPredictors);
    allocTableInstalls.init(numPredictors);
    tableWrites.init(numPredictors);
    tableOverwrites.init(numPredictors);
    liveValidEntriesPerTable.init(numPredictors);
    liveSecondBlockEntriesPerTable.init(numPredictors);

    for (unsigned i = 0; i < numPredictors; ++i) {
        const auto tableName = "T" + std::to_string(i);
        predTableHits.subname(i, tableName);
        trainProviderTableHits.subname(i, tableName);
        allocTableInstalls.subname(i, tableName);
        tableWrites.subname(i, tableName);
        tableOverwrites.subname(i, tableName);
        liveValidEntriesPerTable.subname(i, tableName);
        liveSecondBlockEntriesPerTable.subname(i, tableName);
    }

    liveOccupancyRate = liveValidEntries / statistics::constant(totalTableSlots);
    liveSecondBlockEntryRate =
        liveSecondBlockEntries / statistics::constant(totalTableSlots);
}
#endif

void
PairTAGE::tickStart()
{
}

void
PairTAGE::tick()
{
}

void
PairTAGE::dryRunCycle(Addr startAddr)
{
    (void)startAddr;
}

void
PairTAGE::setPredictionPhase(PairPhase phase)
{
    predictionPhase = phase;
}

void
PairTAGE::putPCHistory(Addr startAddr, const bitset &history, std::vector<FullBTBPrediction> &stagePreds)
{
    secondPredBlock.clear();
#ifndef UNIT_TEST
    pairTageStats.predCalls++;
#endif

    meta = std::make_shared<TageMeta>();
    meta->tagFoldedHist = tagFoldedHist;
    meta->altTagFoldedHist = altTagFoldedHist;
    meta->indexFoldedHist = indexFoldedHist;
    meta->aheadIndexFoldedHistValid = !aheadIndexFoldedHist.empty();
    if (meta->aheadIndexFoldedHistValid) {
        meta->aheadIndexFoldedHist = aheadIndexFoldedHist.front();
    }
    meta->history = history;
    meta->predictedFirstBlock.clear();
    meta->predictedSecondBlock.clear();

    if (!phaseEnabled(predictionPhase)) {
#ifndef UNIT_TEST
        pairTageStats.predOddPhaseSkipped++;
#endif
        return;
    }

    auto tableInfo = lookupEntry(startAddr);
    if (!tableInfo.found) {
#ifndef UNIT_TEST
        pairTageStats.predMiss++;
#endif
        return;
    }
#ifndef UNIT_TEST
    pairTageStats.predHit++;
    pairTageStats.predTableHits[tableInfo.table]++;
#endif

    meta->firstBlockValid = tableInfo.entry.firstBlock().valid;
    meta->secondBlockValid = tableInfo.entry.secondBlock().valid;
    meta->predictedFirstBlock = tableInfo.entry.firstBlock();
    meta->predictedSecondBlock = tableInfo.entry.secondBlock();
    secondPredBlock = tableInfo.entry.secondBlock();
#ifndef UNIT_TEST
    if (tableInfo.entry.secondBlock().valid) {
        pairTageStats.predSecondBlockAvailable++;
    }
#endif

    if (!tableInfo.entry.firstBlock().valid) {
        return;
    }

    for (int s = getDelay(); s < stagePreds.size(); ++s) {
        fillStagePrediction(tableInfo.entry.firstBlock(), stagePreds[s]);
    }
}

std::shared_ptr<void>
PairTAGE::getPredictionMeta(ThreadID tid)
{
    (void)tid;
    return meta;
}

void
PairTAGE::refreshPredictionMeta(Addr startAddr,
                                const bitset &history,
                                FullBTBPrediction &pred)
{
    (void)pred;
#ifndef UNIT_TEST
    pairTageStats.refreshCalls++;
#endif

    meta = std::make_shared<TageMeta>();
    meta->tagFoldedHist = tagFoldedHist;
    meta->altTagFoldedHist = altTagFoldedHist;
    meta->indexFoldedHist = indexFoldedHist;
    meta->aheadIndexFoldedHistValid = !aheadIndexFoldedHist.empty();
    if (meta->aheadIndexFoldedHistValid) {
        meta->aheadIndexFoldedHist = aheadIndexFoldedHist.front();
    }
    meta->history = history;
    meta->predictedFirstBlock.clear();
    meta->predictedSecondBlock.clear();

    if (!phaseEnabled(predictionPhase)) {
#ifndef UNIT_TEST
        pairTageStats.refreshOddPhaseSkipped++;
#endif
        return;
    }

    auto tableInfo = lookupEntry(startAddr);
    if (!tableInfo.found) {
#ifndef UNIT_TEST
        pairTageStats.refreshMiss++;
#endif
        return;
    }
#ifndef UNIT_TEST
    pairTageStats.refreshHit++;
#endif

    meta->firstBlockValid = tableInfo.entry.firstBlock().valid;
    meta->secondBlockValid = tableInfo.entry.secondBlock().valid;
    meta->predictedFirstBlock = tableInfo.entry.firstBlock();
    meta->predictedSecondBlock = tableInfo.entry.secondBlock();
}

PairTAGE::PairBlockInfo
PairTAGE::getSecondPredBlock() const
{
    return secondPredBlock;
}

void
PairTAGE::noteEntryRewrite(unsigned table, const TageEntry &oldEntry,
                           const TageEntry &newEntry)
{
#ifdef UNIT_TEST
    (void)table;
    (void)oldEntry;
    (void)newEntry;
#else
    if (!oldEntry.valid && newEntry.valid) {
        pairTageStats.liveValidEntries++;
        pairTageStats.liveValidEntriesPerTable[table]++;
    } else if (oldEntry.valid && !newEntry.valid) {
        pairTageStats.liveValidEntries--;
        pairTageStats.liveValidEntriesPerTable[table]--;
    }

    const bool oldHasSecond = oldEntry.hasSecondBlock();
    const bool newHasSecond = newEntry.hasSecondBlock();
    if (!oldHasSecond && newHasSecond) {
        pairTageStats.liveSecondBlockEntries++;
        pairTageStats.liveSecondBlockEntriesPerTable[table]++;
        pairTageStats.installSecondBlock++;
    } else if (oldHasSecond && !newHasSecond) {
        pairTageStats.liveSecondBlockEntries--;
        pairTageStats.liveSecondBlockEntriesPerTable[table]--;
        pairTageStats.clearSecondBlock++;
    }
#endif
}

void
PairTAGE::updateCounter(bool taken, unsigned width, short &counter)
{
    const int max = (1 << (width - 1)) - 1;
    const int min = -(1 << (width - 1));
    if (taken) {
        satIncrement(max, counter);
    } else {
        satDecrement(min, counter);
    }
}

bool
PairTAGE::satIncrement(int max, short &counter)
{
    if (counter < max) {
        ++counter;
        return true;
    }
    return false;
}

bool
PairTAGE::satDecrement(int min, short &counter)
{
    if (counter > min) {
        --counter;
        return true;
    }
    return false;
}

int
PairTAGE::selectAllocationWay(const std::vector<TageEntry> &set) const
{
    for (unsigned way = 0; way < set.size(); ++way) {
        if (!set[way].valid) {
            return way;
        }
    }

    for (unsigned way = 0; way < set.size(); ++way) {
        const auto &cand = set[way];
        const bool weakish = std::abs(cand.counter * 2 + 1) <= 3;
        if (!cand.useful && weakish) {
            return way;
        }
    }

    for (unsigned way = 0; way < set.size(); ++way) {
        if (!set[way].useful) {
            return way;
        }
    }

    return -1;
}

void
PairTAGE::resetUsefulBits()
{
#ifndef UNIT_TEST
    pairTageStats.usefulReset++;
#endif
    usefulResetCnt = 0;
    for (auto &table : tageTable) {
        for (auto &set : table) {
            for (auto &entry : set) {
                entry.useful = false;
            }
        }
    }
}

bool
PairTAGE::allocateEntries(Addr startPC, const TageMeta &predMeta,
                          const PairBlockInfo &trainedBlock,
                          const PairBlockInfo &trainedSecondBlock,
                          unsigned startTable)
{
    if (startTable >= numPredictors) {
        return false;
    }

    std::vector<unsigned> candidateTables;
    std::vector<unsigned> candidateWays;
    candidateTables.reserve(numPredictors - startTable);
    candidateWays.reserve(numPredictors - startTable);
    for (unsigned table = startTable; table < numPredictors; ++table) {
        const Addr index =
            getTageIndex(startPC, table, predMeta.indexFoldedHist[table].get());
        const auto &set = tageTable[table][index];
        const int selectedWay = selectAllocationWay(set);
        if (selectedWay >= 0) {
            candidateTables.push_back(table);
            candidateWays.push_back(static_cast<unsigned>(selectedWay));
        }
    }

    if (candidateTables.empty()) {
#ifndef UNIT_TEST
        pairTageStats.allocFailureNoCandidate++;
#endif
        usefulResetCnt++;
        if (usefulResetCnt >= 256) {
            resetUsefulBits();
        }
        return false;
    }

    usefulResetCnt = usefulResetCnt <= 0 ? 0 : usefulResetCnt - 1;

    const unsigned rotateBy = allocLFSR.get() % candidateTables.size();
    std::rotate(candidateTables.begin(), candidateTables.begin() + rotateBy,
                candidateTables.end());
    std::rotate(candidateWays.begin(), candidateWays.begin() + rotateBy,
                candidateWays.end());

    const unsigned allocCount =
        std::min<unsigned>(numTablesToAlloc, candidateTables.size());
    for (unsigned allocIdx = 0; allocIdx < allocCount; ++allocIdx) {
        const unsigned table = candidateTables[allocIdx];
        const unsigned way = candidateWays[allocIdx];
        const Addr index =
            getTageIndex(startPC, table, predMeta.indexFoldedHist[table].get());
        auto &targetEntry = tageTable[table][index][way];
        const auto oldEntry = targetEntry;

#ifndef UNIT_TEST
        if (!oldEntry.valid) {
            pairTageStats.allocIntoInvalidSlot++;
        } else {
            pairTageStats.allocOverwriteValid++;
            pairTageStats.tableOverwrites[table]++;
            if (oldEntry.hasSecondBlock()) {
                pairTageStats.allocOverwriteValidSecondBlock++;
            }
        }
        pairTageStats.allocTableInstalls[table]++;
        pairTageStats.tableWrites[table]++;
#endif

        TageEntry newEntry;
        newEntry.valid = true;
        newEntry.tag = getTageTag(
            startPC, table, predMeta.tagFoldedHist[table].get(),
            predMeta.altTagFoldedHist[table].get(),
            getBranchIndexInBlock(trainedBlock.branchPC, startPC));
        newEntry.counter = trainedBlock.taken ? 0 : -1;
        newEntry.useful = false;
        newEntry.setBlock(0, trainedBlock);
        if (trainedSecondBlock.valid) {
            newEntry.setBlock(1, trainedSecondBlock);
        } else {
            newEntry.clearBlock(1);
        }

        noteEntryRewrite(table, oldEntry, newEntry);
        targetEntry = newEntry;
    }

    return true;
}

void
PairTAGE::specUpdateHist(const bitset &history, FullBTBPrediction &pred)
{
    auto [pc, target, taken] = pred.getPHistInfo();
    doUpdateHist(history, taken, pc, target);
}

void
PairTAGE::recoverHist(const bitset &history, const FetchTarget &entry, int shamt, bool cond_taken)
{
    (void)shamt;

    auto predMeta = std::static_pointer_cast<TageMeta>(entry.predMetas[getComponentIdx()]);
    if (!predMeta) {
        return;
    }

    for (unsigned i = 0; i < numPredictors; ++i) {
        indexFoldedHist[i].recover(predMeta->indexFoldedHist[i]);
        altTagFoldedHist[i].recover(predMeta->altTagFoldedHist[i]);
        tagFoldedHist[i].recover(predMeta->tagFoldedHist[i]);
    }

    while (!aheadIndexFoldedHist.empty()) {
        aheadIndexFoldedHist.pop();
    }
    if (predMeta->aheadIndexFoldedHistValid) {
        assert(predMeta->aheadIndexFoldedHist.size() == numPredictors);
        aheadIndexFoldedHist.push(predMeta->aheadIndexFoldedHist);
    }

    doUpdateHist(history, cond_taken, entry.getControlPC(), entry.getTakenTarget());
}

void
PairTAGE::update(const FetchTarget &entry)
{
    (void)entry;

    // Training is intentionally deferred. The table storage is fully
    // initialized and exposed, but no state mutation happens on update yet.
}

PairTAGE::ProviderInfo
PairTAGE::lookupProviders(Addr startPC, const TageMeta &predMeta) const
{
    ProviderInfo providers;

    for (int table = numPredictors - 1; table >= 0; --table) {
        const Addr index = getTageIndex(startPC, table, predMeta.indexFoldedHist[table].get());
        auto &set = tageTable[table][index];

        for (unsigned way = 0; way < numWays; ++way) {
            const auto &entry = set[way];
            if (!entry.valid || !entry.firstBlock().valid) {
                continue;
            }

            // Todo: why this check is needed? If the first block is invalid or meet
            // aliasing, the entry should be override by higher predictors and should
            // not in final stage pred.
            const auto &firstBlock = entry.firstBlock();
            if (!isBranchInFirstBlock(firstBlock.branchPC, startPC)) {
                continue;
            }

            const unsigned position = getBranchIndexInBlock(firstBlock.branchPC, startPC);
            const Addr tag = getTageTag(startPC, table,
                predMeta.tagFoldedHist[table].get(),
                predMeta.altTagFoldedHist[table].get(), position);

            if (entry.tag == tag) {
                auto info = TageTableInfo{
                    true, entry, static_cast<unsigned>(table), index, tag, way};
                if (!providers.main.found) {
                    providers.main = info;
                } else if (!providers.alt.found) {
                    providers.alt = info;
                    break;
                }
            }
        }

        if (providers.alt.found) {
            break;
        }
    }

    return providers;
}

PairTAGE::ProviderInfo
PairTAGE::lookupProviders(Addr startPC) const
{
    auto predMeta = TageMeta();
    predMeta.tagFoldedHist = tagFoldedHist;
    predMeta.altTagFoldedHist = altTagFoldedHist;
    predMeta.indexFoldedHist = indexFoldedHist;
    return lookupProviders(startPC, predMeta);
}

PairTAGE::TageTableInfo
PairTAGE::lookupEntry(Addr startPC, const TageMeta &predMeta) const
{
    return lookupProviders(startPC, predMeta).main;
}

PairTAGE::TageTableInfo
PairTAGE::lookupEntry(Addr startPC) const
{
    return lookupProviders(startPC).main;
}

BTBEntry
PairTAGE::buildBTBEntry(const PairBlockInfo &block) const
{
    BTBEntry entry;
    entry.valid = block.valid && !block.isFallThrough();
    entry.pc = block.branchPC;
    entry.target = block.targetPC;
    entry.size = block.isFallThrough() ? 0 : block.size;
    entry.isCond = block.isCond;
    entry.isDirect = block.isDirect;
    entry.isIndirect = block.isIndirect;
    entry.isCall = block.isCall;
    entry.isReturn = block.isReturn;
    entry.alwaysTaken = entry.valid && !block.isCond;
    entry.ctr = block.taken ? 0 : -1;
    entry.source = componentIdx;
    return entry;
}

void
PairTAGE::fillStagePrediction(const PairBlockInfo &block, FullBTBPrediction &pred) const
{
    pred.btbEntries.clear();
    pred.condTakens.clear();
    pred.indirectTargets.clear();
    pred.tageInfoForMgscs.clear();
    pred.returnTarget = 0;
#ifdef UNIT_TEST
    pred.predTick = 0;
#else
    pred.predTick = curTick();
#endif

    if (!block.valid) {
        return;
    }

    if (block.isFallThrough()) {
        return;
    }

    auto entry = buildBTBEntry(block);
    pred.btbEntries.push_back(entry);
    if (entry.isCond) {
        pred.condTakens.push_back({entry.pc, block.taken});
    }
    if (entry.isIndirect) {
        if (entry.isReturn) {
            pred.returnTarget = block.targetPC;
        } else {
            pred.indirectTargets.push_back({entry.pc, block.targetPC});
        }
    }
}

PairTAGE::PairBlockInfo
PairTAGE::buildTrainingBlock(const FetchTarget &entry) const
{
    return buildTrainingBlockResult(entry).block;
}

PairTAGE::PairBlockInfo
PairTAGE::buildTrainingBlock(const FullBTBPrediction &pred) const
{
    return buildTrainingBlockResult(pred).block;
}

PairTAGE::TrainingBlockBuildStatus
PairTAGE::classifyUnsupportedTrainingEntry(const BTBEntry &entry) const
{
    if (!entry.valid) {
        return TrainingBlockBuildStatus::UnsupportedFormat;
    }
    if (entry.isIndirect) {
        return TrainingBlockBuildStatus::FilteredIndirect;
    }
    if (entry.isCall) {
        return TrainingBlockBuildStatus::FilteredCall;
    }
    if (entry.isReturn) {
        return TrainingBlockBuildStatus::FilteredReturn;
    }
    return TrainingBlockBuildStatus::UnsupportedFormat;
}

PairTAGE::TrainingBlockBuildResult
PairTAGE::buildTrainingBlockResult(const FetchTarget &entry) const
{
    const BTBEntry *trainEntry = nullptr;
    const BTBEntry *fallbackEntry = nullptr;

    if (entry.predTaken) {
        for (const auto &btbEntry : entry.predBTBEntries) {
            if (btbEntry.valid && btbEntry.pc == entry.predBranchInfo.pc) {
                trainEntry = &btbEntry;
                break;
            }
        }
        if (!trainEntry) {
            return TrainingBlockBuildResult(
                PairBlockInfo{}, TrainingBlockBuildStatus::NoTakenEntry);
        }
    } else {
        // PairTAGE stores only one not-taken direct conditional per block.
        for (auto it = entry.predBTBEntries.rbegin();
             it != entry.predBTBEntries.rend(); ++it) {
            if (!it->valid) {
                continue;
            }
            if (!fallbackEntry) {
                fallbackEntry = &*it;
            }
            if (it->isCond && it->isDirect &&
                !it->isIndirect && !it->isCall && !it->isReturn) {
                trainEntry = &*it;
                break;
            }
        }
        if (!trainEntry) {
            if (fallbackEntry) {
                return TrainingBlockBuildResult(
                    PairBlockInfo{},
                    classifyUnsupportedTrainingEntry(*fallbackEntry));
            }
            return TrainingBlockBuildResult(
                PairBlockInfo(false, entry.startPC, entry.predEndPC, true),
                TrainingBlockBuildStatus::Valid);
        }
    }

    if (!entry.predTaken &&
        (!trainEntry->valid || !trainEntry->isCond ||
         !trainEntry->isDirect || trainEntry->isIndirect ||
         trainEntry->isCall || trainEntry->isReturn)) {
        return TrainingBlockBuildResult(
            PairBlockInfo{}, classifyUnsupportedTrainingEntry(*trainEntry));
    }

    if (entry.predTaken) {
        const auto &branchInfo = entry.predBranchInfo;
        return TrainingBlockBuildResult(
            PairBlockInfo(true, branchInfo.pc, branchInfo.target,
                          branchInfo.isCond, branchInfo.isDirect,
                          branchInfo.isIndirect, branchInfo.isCall,
                          branchInfo.isReturn, branchInfo.size),
            TrainingBlockBuildStatus::Valid);
    }

    return TrainingBlockBuildResult(
        PairBlockInfo(false, trainEntry->pc, trainEntry->target),
        TrainingBlockBuildStatus::Valid);
}

PairTAGE::TrainingBlockBuildResult
PairTAGE::buildTrainingBlockResult(const FullBTBPrediction &pred) const
{
    auto predCopy = pred;
    const BTBEntry *trainEntry = nullptr;
    const BTBEntry *fallbackEntry = nullptr;

    if (predCopy.isTaken()) {
        auto takenEntry = predCopy.getTakenEntry();
        if (!takenEntry.valid) {
            return TrainingBlockBuildResult(
                PairBlockInfo{}, TrainingBlockBuildStatus::NoTakenEntry);
        }
        for (const auto &btbEntry : predCopy.btbEntries) {
            if (btbEntry.valid && btbEntry.pc == takenEntry.pc) {
                trainEntry = &btbEntry;
                break;
            }
        }
        if (!trainEntry) {
            return TrainingBlockBuildResult(
                PairBlockInfo{}, TrainingBlockBuildStatus::NoTakenEntry);
        }
    } else {
        for (auto it = predCopy.btbEntries.rbegin();
             it != predCopy.btbEntries.rend(); ++it) {
            if (!it->valid) {
                continue;
            }
            if (!fallbackEntry) {
                fallbackEntry = &*it;
            }
            if (it->isCond && it->isDirect &&
                !it->isIndirect && !it->isCall && !it->isReturn) {
                trainEntry = &*it;
                break;
            }
        }
        if (!trainEntry) {
            if (fallbackEntry) {
                return TrainingBlockBuildResult(
                    PairBlockInfo{},
                    classifyUnsupportedTrainingEntry(*fallbackEntry));
            }
            return TrainingBlockBuildResult(
                PairBlockInfo{},
                TrainingBlockBuildStatus::NoNotTakenDirectCond);
        }
    }

    if (!trainEntry->valid || !trainEntry->isCond ||
        !trainEntry->isDirect || trainEntry->isIndirect ||
        trainEntry->isCall || trainEntry->isReturn) {
        return TrainingBlockBuildResult(
            PairBlockInfo{}, classifyUnsupportedTrainingEntry(*trainEntry));
    }

    return TrainingBlockBuildResult(
        PairBlockInfo(predCopy.isTaken(), trainEntry->pc, trainEntry->target),
        TrainingBlockBuildStatus::Valid);
}

bool
PairTAGE::blocksMatch(const PairBlockInfo &lhs, const PairBlockInfo &rhs) const
{
    if (lhs.valid != rhs.valid) {
        return false;
    }
    if (!lhs.valid) {
        return true;
    }
    return lhs.taken == rhs.taken && lhs.branchPC == rhs.branchPC &&
           lhs.fallThrough == rhs.fallThrough &&
           lhs.targetPC == rhs.targetPC &&
           lhs.isCond == rhs.isCond &&
           lhs.isDirect == rhs.isDirect &&
           lhs.isIndirect == rhs.isIndirect &&
           lhs.isCall == rhs.isCall &&
           lhs.isReturn == rhs.isReturn &&
           lhs.size == rhs.size;
}

bool
PairTAGE::entryMatchesTraining(const TageEntry &entry,
                               const PairBlockInfo &firstBlock,
                               const PairBlockInfo &secondBlock) const
{
    if (!blocksMatch(entry.firstBlock(), firstBlock)) {
        return false;
    }
    if (!enableSecondBlock) {
        return true;
    }
    return blocksMatch(entry.secondBlock(), secondBlock);
}

void
PairTAGE::trainFromActualPred(const FetchTarget &entry,
                              const FullBTBPrediction *secondPred)
{
#ifndef UNIT_TEST
    pairTageStats.trainCalls++;
#endif
    if (!phaseEnabled(entry.pairPhase)) {
#ifndef UNIT_TEST
        pairTageStats.trainOddPhaseSkipped++;
#endif
        return;
    }

    auto predMeta = std::static_pointer_cast<TageMeta>(
        entry.predMetas[getComponentIdx()]);
    if (!predMeta) {
#ifndef UNIT_TEST
        pairTageStats.trainNoMeta++;
#endif
        return;
    }

    auto providers = lookupProviders(entry.startPC, *predMeta);
    auto provider = providers.main;
    auto altProvider = providers.alt;
    auto trainedBlockResult = buildTrainingBlockResult(entry);
    auto trainedBlock = trainedBlockResult.block;
    auto trainedSecondBlock =
        (enableSecondBlock && secondPred) ? buildTrainingBlock(*secondPred)
                                          : PairBlockInfo{};
#ifndef UNIT_TEST
    if (provider.found) {
        pairTageStats.trainProviderHit++;
        pairTageStats.trainProviderTableHits[provider.table]++;
    } else {
        pairTageStats.trainProviderMiss++;
    }
#endif

    if (!trainedBlock.valid) {
#ifndef UNIT_TEST
        pairTageStats.trainFirstBlockInvalid++;
        switch (trainedBlockResult.status) {
          case TrainingBlockBuildStatus::NoTakenEntry:
            pairTageStats.trainFirstBlockInvalidNoTakenEntry++;
            break;
          case TrainingBlockBuildStatus::NoNotTakenDirectCond:
            pairTageStats.trainFirstBlockInvalidNoNotTakenDirectCond++;
            break;
          case TrainingBlockBuildStatus::FilteredIndirect:
            pairTageStats.trainFirstBlockInvalidFilteredIndirect++;
            break;
          case TrainingBlockBuildStatus::FilteredCall:
            pairTageStats.trainFirstBlockInvalidFilteredCall++;
            break;
          case TrainingBlockBuildStatus::FilteredReturn:
            pairTageStats.trainFirstBlockInvalidFilteredReturn++;
            break;
          case TrainingBlockBuildStatus::UnsupportedFormat:
            pairTageStats.trainFirstBlockInvalidUnsupportedFormat++;
            break;
          case TrainingBlockBuildStatus::Valid:
            break;
        }
#endif
        if (provider.found) {
            auto &providerEntry =
                tageTable[provider.table][provider.index][provider.way];
#ifndef UNIT_TEST
            pairTageStats.clearEntryOnInvalidTrain++;
            noteEntryRewrite(provider.table, providerEntry, TageEntry{});
#endif
            providerEntry = TageEntry{};
        }
        return;
    }
    const bool skipStandaloneFallThrough =
        trainedBlock.isFallThrough() && !trainedSecondBlock.valid &&
        !trainStandaloneFallThrough;
    if (skipStandaloneFallThrough) {
#ifndef UNIT_TEST
        pairTageStats.trainFallThroughSkippedNoSecondBlock++;
#endif
        return;
    }
#ifndef UNIT_TEST
    pairTageStats.trainFirstBlockValid++;
    if (trainedSecondBlock.valid) {
        pairTageStats.trainSecondBlockValid++;
    }
#endif

    const bool providerMatchesTraining = provider.found &&
        entryMatchesTraining(provider.entry, trainedBlock, trainedSecondBlock);
    const bool providerFirstBlockMatches = provider.found &&
        blocksMatch(provider.entry.firstBlock(), trainedBlock);
    const bool altMatchesTraining = altProvider.found &&
        entryMatchesTraining(altProvider.entry, trainedBlock, trainedSecondBlock);
    const bool canAllocHigher = provider.found &&
        provider.table < numPredictors - 1;
    const bool preserveProviderOnMismatch = provider.found &&
        !providerMatchesTraining && canAllocHigher;

    if (provider.found && !preserveProviderOnMismatch) {
        auto &providerEntry =
            tageTable[provider.table][provider.index][provider.way];
        auto oldEntry = providerEntry;
        TageEntry newEntry = oldEntry;

        short trainedCounter = oldEntry.counter;
        if (blocksMatch(oldEntry.firstBlock(), trainedBlock)) {
            updateCounter(trainedBlock.taken, 3, trainedCounter);
        } else {
            trainedCounter = trainedBlock.taken ? 0 : -1;
        }

        newEntry.valid = true;
        newEntry.tag = getTageTag(
            entry.startPC, provider.table,
            predMeta->tagFoldedHist[provider.table].get(),
            predMeta->altTagFoldedHist[provider.table].get(),
            getBranchIndexInBlock(trainedBlock.branchPC, entry.startPC));
        newEntry.counter = trainedCounter;
        newEntry.useful = oldEntry.useful;
        if (providerMatchesTraining && altProvider.found && !altMatchesTraining) {
            newEntry.useful = true;
        }
        newEntry.setBlock(0, trainedBlock);
        if (trainedSecondBlock.valid) {
            newEntry.setBlock(1, trainedSecondBlock);
        } else {
            newEntry.clearBlock(1);
        }

#ifndef UNIT_TEST
        pairTageStats.updateExistingProvider++;
        pairTageStats.tableWrites[provider.table]++;
#endif
        noteEntryRewrite(provider.table, oldEntry, newEntry);
        providerEntry = newEntry;
    } else if (provider.found && providerFirstBlockMatches) {
        auto &providerEntry =
            tageTable[provider.table][provider.index][provider.way];
        auto oldEntry = providerEntry;
        TageEntry newEntry = oldEntry;
        short trainedCounter = oldEntry.counter;

        updateCounter(trainedBlock.taken, 3, trainedCounter);
        newEntry.counter = trainedCounter;
        if (altProvider.found && !altMatchesTraining) {
            newEntry.useful = true;
        }

        if (newEntry.counter != oldEntry.counter ||
            newEntry.useful != oldEntry.useful) {
#ifndef UNIT_TEST
            pairTageStats.updateExistingProvider++;
            pairTageStats.tableWrites[provider.table]++;
#endif
            noteEntryRewrite(provider.table, oldEntry, newEntry);
            providerEntry = newEntry;
        }
    }

    bool needHigherAlloc = false;
    unsigned allocStartTable = 0;
    if (!provider.found) {
        needHigherAlloc = true;
        allocStartTable = 0;
    } else if (!providerMatchesTraining &&
               provider.table < numPredictors - 1) {
        needHigherAlloc = true;
        allocStartTable = provider.table + 1;
    }

    if (needHigherAlloc) {
        allocateEntries(entry.startPC, *predMeta, trainedBlock,
                        trainedSecondBlock, allocStartTable);
    }
}

Addr
PairTAGE::getTageTag(Addr pc, int table, uint64_t foldedHist, uint64_t altFoldedHist, Addr position) const
{
    const Addr mask = (1ULL << tableTagBits[table]) - 1;
    const unsigned pcShift = tablePcShifts[table];
    const Addr pcBits = (pc >> pcShift) & mask;
    const Addr foldedBits = foldedHist & mask;
    const Addr altTagBits = (altFoldedHist << 1) & mask;

    return pcBits ^ foldedBits ^ altTagBits ^ position;
}

Addr
PairTAGE::getTageIndex(Addr pc, int table, uint64_t foldedHist) const
{
    const Addr mask = (1ULL << tableIndexBits[table]) - 1;
    const Addr pcBits = (pc >> instShiftAmt) & mask;
    const Addr foldedBits = foldedHist & mask;

    return pcBits ^ foldedBits;
}

Addr
PairTAGE::getTageIndex(Addr pc, int table) const
{
    return getTageIndex(pc, table, indexFoldedHist[table].get());
}

unsigned
PairTAGE::getBranchIndexInBlock(Addr branchPC, Addr startPC) const
{
    const Addr alignedPC = startPC & ~(blockSize - 1);
    unsigned position = 0;

    if (branchPC >= alignedPC) {
        position = (branchPC - alignedPC) >> instShiftAmt;
    }

    if (position >= maxBranchPositions) {
        position %= maxBranchPositions;
    }

    return position;
}

Addr
PairTAGE::getFallThrough(Addr startPC) const
{
    return (startPC + predictWidth) & ~mask(floorLog2(predictWidth) - 1);
}

bool
PairTAGE::isBranchInFirstBlock(Addr branchPC, Addr startPC) const
{
    return branchPC >= startPC && branchPC < getFallThrough(startPC);
}

void
PairTAGE::doUpdateHist(const bitset &history, bool taken, Addr pc, Addr target)
{
    if (!aheadIndexFoldedHist.empty()) {
        indexFoldedHist = aheadIndexFoldedHist.front();
    }

    if (!taken) {
        return;
    }

    for (unsigned t = 0; t < numPredictors; ++t) {
        tagFoldedHist[t].update(history, 2, taken, pc, target);
        altTagFoldedHist[t].update(history, 2, taken, pc, target);
    }

    auto nextIndexFoldedHist = indexFoldedHist;
    for (unsigned t = 0; t < numPredictors; ++t) {
        nextIndexFoldedHist[t].update(history, 2, taken, pc, target);
    }

    aheadIndexFoldedHist.push(nextIndexFoldedHist);
    if (aheadIndexFoldedHist.size() > 1) {
        aheadIndexFoldedHist.pop();
    }
}

#ifdef UNIT_TEST
}  // namespace test
#endif

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5
