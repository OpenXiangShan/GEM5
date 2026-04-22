#include "cpu/pred/btb/pairtage.hh"

#include <cassert>

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
      numWays(numWays),
      maxBranchPositions(32)
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
      numWays(p.numWays),
      maxBranchPositions(p.maxBranchPositions)
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
PairTAGE::putPCHistory(Addr startAddr, const bitset &history, std::vector<FullBTBPrediction> &stagePreds)
{
    secondPredBlock.clear();

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

    auto tableInfo = lookupEntry(startAddr);
    if (!tableInfo.found) {
        return;
    }

    meta->firstBlockValid = tableInfo.entry.firstBlock().valid;
    meta->secondBlockValid = tableInfo.entry.secondBlock().valid;
    meta->predictedFirstBlock = tableInfo.entry.firstBlock();
    meta->predictedSecondBlock = tableInfo.entry.secondBlock();
    secondPredBlock = tableInfo.entry.secondBlock();

    if (!tableInfo.entry.firstBlock().valid) {
        return;
    }

    for (int s = getDelay(); s < stagePreds.size(); ++s) {
        fillStagePrediction(tableInfo.entry.firstBlock(), stagePreds[s]);
    }
}

std::shared_ptr<void>
PairTAGE::getPredictionMeta()
{
    return meta;
}

PairTAGE::PairBlockInfo
PairTAGE::getSecondPredBlock() const
{
    return secondPredBlock;
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

PairTAGE::TageTableInfo
PairTAGE::lookupEntry(Addr startPC, const TageMeta &predMeta) const
{
    for (int table = numPredictors - 1; table >= 0; --table) {
        const Addr index = getTageIndex(startPC, table, predMeta.indexFoldedHist[table].get());
        auto &set = tageTable[table][index];

        for (unsigned way = 0; way < numWays; ++way) {
            const auto &entry = set[way];
            if (!entry.valid || !entry.firstBlock().valid) {
                continue;
            }

            const unsigned position = getBranchIndexInBlock(entry.firstBlock().branchPC, startPC);
            const Addr tag = getTageTag(startPC, table,
                predMeta.tagFoldedHist[table].get(),
                predMeta.altTagFoldedHist[table].get(), position);

            if (entry.tag == tag) {
                return TageTableInfo{
                    true, entry, static_cast<unsigned>(table), index, tag, way};
            }
        }
    }

    return TageTableInfo{};
}

PairTAGE::TageTableInfo
PairTAGE::lookupEntry(Addr startPC) const
{
    auto predMeta = TageMeta();
    predMeta.tagFoldedHist = tagFoldedHist;
    predMeta.altTagFoldedHist = altTagFoldedHist;
    predMeta.indexFoldedHist = indexFoldedHist;
    return lookupEntry(startPC, predMeta);
}

BTBEntry
PairTAGE::buildBTBEntry(const PairBlockInfo &block) const
{
    BTBEntry entry;
    entry.valid = block.valid;
    entry.pc = block.branchPC;
    entry.target = block.targetPC;
    entry.size = 4;
    entry.isCond = true;
    entry.isDirect = true;
    entry.alwaysTaken = false;
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

    auto entry = buildBTBEntry(block);
    pred.btbEntries.push_back(entry);
    pred.condTakens.push_back({entry.pc, block.taken});
}

PairTAGE::PairBlockInfo
PairTAGE::buildTrainingBlock(const FetchTarget &entry) const
{
    const BTBEntry *trainEntry = nullptr;

    if (entry.predTaken) {
        for (const auto &btbEntry : entry.predBTBEntries) {
            if (btbEntry.valid && btbEntry.pc == entry.predBranchInfo.pc) {
                trainEntry = &btbEntry;
                break;
            }
        }
    } else {
        // This simplified PairTAGE format only stores a single not-taken
        // conditional. Use the furthest not-taken conditional in the block as
        // the representative first block.
        for (auto it = entry.predBTBEntries.rbegin();
             it != entry.predBTBEntries.rend(); ++it) {
            if (it->valid && it->isCond && it->isDirect &&
                !it->isIndirect && !it->isCall && !it->isReturn) {
                trainEntry = &*it;
                break;
            }
        }
    }

    if (!trainEntry || !trainEntry->valid || !trainEntry->isCond ||
        !trainEntry->isDirect || trainEntry->isIndirect ||
        trainEntry->isCall || trainEntry->isReturn) {
        return PairBlockInfo{};
    }

    const Addr targetPC = entry.predTaken ?
        entry.predBranchInfo.target : trainEntry->target;
    return PairBlockInfo(entry.predTaken, trainEntry->pc, targetPC);
}

void
PairTAGE::trainFromActualPred(const FetchTarget &entry)
{
    auto predMeta = std::static_pointer_cast<TageMeta>(
        entry.predMetas[getComponentIdx()]);
    if (!predMeta) {
        return;
    }

    auto provider = lookupEntry(entry.startPC, *predMeta);
    auto trainedBlock = buildTrainingBlock(entry);

    if (!trainedBlock.valid) {
        if (provider.found) {
            tageTable[provider.table][provider.index][provider.way] = TageEntry{};
        }
        return;
    }

    unsigned table = provider.found ? provider.table : 0;
    Addr index = provider.found ? provider.index :
        getTageIndex(entry.startPC, table, predMeta->indexFoldedHist[table].get());
    unsigned way = provider.found ? provider.way : 0;

    if (!provider.found) {
        auto &set = tageTable[table][index];
        for (unsigned candidateWay = 0; candidateWay < numWays; ++candidateWay) {
            if (!set[candidateWay].valid) {
                way = candidateWay;
                break;
            }
        }
    }

    auto &trainedEntry = tageTable[table][index][way];
    trainedEntry = TageEntry{};
    trainedEntry.valid = true;
    trainedEntry.tag = getTageTag(entry.startPC, table,
        predMeta->tagFoldedHist[table].get(),
        predMeta->altTagFoldedHist[table].get(),
        getBranchIndexInBlock(trainedBlock.branchPC, entry.startPC));
    trainedEntry.counter = trainedBlock.taken ? 0 : -1;
    trainedEntry.useful = provider.found ? provider.entry.useful : false;
    trainedEntry.setBlock(0, trainedBlock);
    trainedEntry.clearBlock(1);
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
