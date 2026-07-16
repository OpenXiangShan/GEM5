#include "cpu/pred/btb/pairtage.hh"

#include <algorithm>
#include <cassert>
#include <cstdlib>

#include "base/intmath.hh"
#include "sim/core.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

void
PairTAGE::PairTAGEEntry::setBlock(unsigned blockIdx, const PairBlockInfo &block)
{
    assert(blockIdx < NumBlocksPerEntry);
    blocks[blockIdx] = block;
}

void
PairTAGE::PairTAGEEntry::clearBlock(unsigned blockIdx)
{
    assert(blockIdx < NumBlocksPerEntry);
    blocks[blockIdx].clear();
}

void
PairTAGE::PairTAGEEntry::strengthenIdentity()
{
    if (identityConfidence < MaxIdentityConfidence) {
        ++identityConfidence;
    }
}

bool
PairTAGE::PairTAGEEntry::weakenIdentity()
{
    if (identityConfidence > 0) {
        --identityConfidence;
    }
    return identityConfidence == 0;
}

PairTAGE::PairTAGE(const Params &p)
    : TimedBaseBTBPredictor(p),
      numPredictors(p.numPredictors),
      tableSizes(p.tableSizes),
      tableIndexBits(p.numPredictors),
      tableTagBits(p.TTagBitSizes),
      tablePcShifts(p.TTagPcShifts),
      histLengths(p.histLengths),
      numTablesToAlloc(p.numTablesToAlloc),
      numWays(p.numWays),
      enableSecondBlock(p.enableSecondBlock),
      allowOddPhase(p.allowOddPhase),
      trainStandaloneFallThrough(p.trainStandaloneFallThrough)
{
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

void
PairTAGE::setPredictionPhase(PairPhase phase)
{
    predictionPhase = phase;
}

void
PairTAGE::putPCHistory(Addr startAddr, const bitset &history, std::vector<FullBTBPrediction> &stagePreds)
{
    (void)history;
    secondPredBlock.clear();

    meta = std::make_shared<TageMeta>();
    meta->tagFoldedHist = tagFoldedHist;
    meta->altTagFoldedHist = altTagFoldedHist;
    meta->indexFoldedHist = indexFoldedHist;
    meta->aheadIndexFoldedHistValid = !aheadIndexFoldedHist.empty();
    if (meta->aheadIndexFoldedHistValid) {
        meta->aheadIndexFoldedHist = aheadIndexFoldedHist.front();
    }

    if (!phaseEnabled(predictionPhase)) {
        return;
    }

    auto tableInfo = lookupEntry(startAddr);
    if (!tableInfo.found) {
        return;
    }

    meta->predictedFirstBlock = tableInfo.entry.firstBlock();
    secondPredBlock = tableInfo.entry.secondBlock();

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
    (void)history;
    (void)pred;

    meta = std::make_shared<TageMeta>();
    meta->tagFoldedHist = tagFoldedHist;
    meta->altTagFoldedHist = altTagFoldedHist;
    meta->indexFoldedHist = indexFoldedHist;
    meta->aheadIndexFoldedHistValid = !aheadIndexFoldedHist.empty();
    if (meta->aheadIndexFoldedHistValid) {
        meta->aheadIndexFoldedHist = aheadIndexFoldedHist.front();
    }

    if (!phaseEnabled(predictionPhase)) {
        return;
    }

    auto tableInfo = lookupEntry(startAddr);
    if (!tableInfo.found) {
        return;
    }

    meta->predictedFirstBlock = tableInfo.entry.firstBlock();
}

PairTAGE::PairBlockInfo
PairTAGE::getSecondPredBlock() const
{
    return secondPredBlock;
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

bool
PairTAGE::betterProviderCandidate(const TageTableInfo &candidate,
                                  const TageTableInfo &current) const
{
    if (!candidate.found) {
        return false;
    }
    if (!current.found) {
        return true;
    }

    const auto &cand = candidate.entry;
    const auto &cur = current.entry;
    if (cand.identityConfidence != cur.identityConfidence) {
        return cand.identityConfidence > cur.identityConfidence;
    }
    if (cand.useful != cur.useful) {
        return cand.useful;
    }
    if (cand.hasSecondBlock() != cur.hasSecondBlock()) {
        return cand.hasSecondBlock();
    }
    const int candStrength = std::abs(cand.counter * 2 + 1);
    const int curStrength = std::abs(cur.counter * 2 + 1);
    if (candStrength != curStrength) {
        return candStrength > curStrength;
    }
    return candidate.way < current.way;
}

int
PairTAGE::selectMatchingReplacementWay(
    const std::vector<PairTAGEEntry> &set, Addr tag,
    const PairBlockInfo &trainedBlock,
    const PairBlockInfo &trainedSecondBlock) const
{
    auto selectVictim = [&](auto predicate) {
        int selectedWay = -1;
        TageTableInfo selectedInfo;
        for (unsigned way = 0; way < set.size(); ++way) {
            const auto &entry = set[way];
            if (!entry.valid || entry.tag != tag ||
                !blockIdentityMatches(entry.firstBlock(), trainedBlock) ||
                entryMatchesTraining(entry, trainedBlock,
                                     trainedSecondBlock) ||
                !predicate(entry)) {
                continue;
            }

            auto info = TageTableInfo{true, entry, 0, 0, tag, way};
            if (!betterProviderCandidate(selectedInfo, info)) {
                selectedInfo = info;
                selectedWay = way;
            }
        }
        return selectedWay;
    };

    const int firstBlockVictim = selectVictim([&](const PairTAGEEntry &entry) {
        return !blocksMatch(entry.firstBlock(), trainedBlock);
    });
    if (firstBlockVictim >= 0) {
        return firstBlockVictim;
    }

    if (trainedSecondBlock.valid) {
        return selectVictim([&](const PairTAGEEntry &entry) {
            return !blocksMatch(entry.secondBlock(), trainedSecondBlock);
        });
    }

    return -1;
}

int
PairTAGE::selectAllocationWay(const std::vector<PairTAGEEntry> &set) const
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
        const Addr tag = getTageTag(
            startPC, table, predMeta.tagFoldedHist[table].get(),
            predMeta.altTagFoldedHist[table].get());
        const auto &set = tageTable[table][index];
        int selectedWay = selectMatchingReplacementWay(
            set, tag, trainedBlock, trainedSecondBlock);
        if (selectedWay < 0) {
            selectedWay = selectAllocationWay(set);
        }
        if (selectedWay >= 0) {
            candidateTables.push_back(table);
            candidateWays.push_back(static_cast<unsigned>(selectedWay));
        }
    }

    if (candidateTables.empty()) {
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

        PairTAGEEntry newEntry;
        newEntry.valid = true;
        newEntry.tag = getTageTag(
            startPC, table, predMeta.tagFoldedHist[table].get(),
            predMeta.altTagFoldedHist[table].get());
        newEntry.counter = trainedBlock.taken ? 0 : -1;
        newEntry.useful = false;
        newEntry.identityConfidence = PairTAGEEntry::InitialIdentityConfidence;
        newEntry.setBlock(0, trainedBlock);
        if (trainedSecondBlock.valid) {
            newEntry.setBlock(1, trainedSecondBlock);
        } else {
            newEntry.clearBlock(1);
        }

        targetEntry = newEntry;
    }

    return true;
}

void
PairTAGE::specUpdatePHist(const bitset &history, FullBTBPrediction &pred,
                          const PathHistoryUpdate &update)
{
    (void)pred;
    doUpdateHist(history, update.taken, update.pc, update.target);
}

void
PairTAGE::recoverPHist(const bitset &history, const FetchTarget &entry,
                       const PathHistoryUpdate &update)
{
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

    doUpdateHist(history, update.taken, update.pc, update.target);
}

PairTAGE::ProviderInfo
PairTAGE::lookupProviders(Addr startPC, const TageMeta &predMeta) const
{
    ProviderInfo providers;

    for (int table = numPredictors - 1; table >= 0; --table) {
        const Addr index = getTageIndex(startPC, table, predMeta.indexFoldedHist[table].get());
        auto &set = tageTable[table][index];
        TageTableInfo bestInTable;

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

            const Addr tag = getTageTag(startPC, table,
                predMeta.tagFoldedHist[table].get(),
                predMeta.altTagFoldedHist[table].get());

            if (entry.tag == tag) {
                auto info = TageTableInfo{
                    true, entry, static_cast<unsigned>(table), index, tag, way};
                if (betterProviderCandidate(info, bestInTable)) {
                    bestInTable = info;
                }
            }
        }

        if (!bestInTable.found) {
            continue;
        }
        if (!providers.main.found) {
            providers.main = bestInTable;
            continue;
        }
        providers.alt = bestInTable;
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
PairTAGE::lookupEntry(Addr startPC) const
{
    return lookupProviders(startPC).main;
}

void
PairTAGE::fillStagePrediction(const PairBlockInfo &block, FullBTBPrediction &pred) const
{
    pred.btbEntries.clear();
    pred.condTakens.clear();
    pred.indirectTargets.clear();
    pred.tageInfoForMgscs.clear();
    pred.returnTarget = 0;
    pred.predTick = curTick();

    if (!block.valid) {
        return;
    }

    if (block.isFallThrough()) {
        return;
    }

    auto entry = block.buildBTBEntry(componentIdx);
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
PairTAGE::buildTrainingBlock(const TrainPacket &packet) const
{
    if (packet.kind == TrainPacket::BlockKind::Invalid) {
        return PairBlockInfo{};
    }

    if (packet.kind == TrainPacket::BlockKind::FallThrough) {
        return PairBlockInfo(false, packet.branchPC, packet.targetPC, true);
    }

    return PairBlockInfo(
        packet.taken, packet.branchPC, packet.targetPC,
        packet.hasBranchFlag(TrainPacket::BranchFlag::Conditional),
        packet.hasBranchFlag(TrainPacket::BranchFlag::Direct),
        packet.hasBranchFlag(TrainPacket::BranchFlag::Indirect),
        packet.hasBranchFlag(TrainPacket::BranchFlag::Call),
        packet.hasBranchFlag(TrainPacket::BranchFlag::Return), packet.size);
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
PairTAGE::blockIdentityMatches(const PairBlockInfo &lhs,
                               const PairBlockInfo &rhs) const
{
    if (lhs.valid != rhs.valid) {
        return false;
    }
    if (!lhs.valid) {
        return true;
    }
    return lhs.fallThrough == rhs.fallThrough &&
           lhs.branchPC == rhs.branchPC &&
           lhs.isCond == rhs.isCond &&
           lhs.isDirect == rhs.isDirect &&
           lhs.isIndirect == rhs.isIndirect &&
           lhs.isCall == rhs.isCall &&
           lhs.isReturn == rhs.isReturn &&
           lhs.size == rhs.size;
}

bool
PairTAGE::entryMatchesTraining(const PairTAGEEntry &entry,
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
PairTAGE::trainFromS3Pred(
    const TrainPacket &finalTrainPacket,
    const TrainPacket *twoTakenTrainPacket)
{
    if (!phaseEnabled(finalTrainPacket.phase)) {
        return;
    }

    const auto &predMeta = finalTrainPacket.meta;
    if (!predMeta) {
        return;
    }

    const Addr pairEntryStartPC = finalTrainPacket.startPC;
    auto providers = lookupProviders(pairEntryStartPC, *predMeta);
    auto provider = providers.main;
    auto altProvider = providers.alt;

    auto trainedFirstBlock = buildTrainingBlock(finalTrainPacket);

    auto secondBlockTrainInfo =
        (enableSecondBlock && twoTakenTrainPacket) ?
            buildTrainingBlock(*twoTakenTrainPacket) : PairBlockInfo{};

    if (!trainedFirstBlock.valid) {
        if (provider.found) {
            auto &providerEntry =
                tageTable[provider.table][provider.index][provider.way];
            providerEntry = PairTAGEEntry{};
        }
        return;
    }
    const bool skipStandaloneFallThrough =
        trainedFirstBlock.isFallThrough() && !secondBlockTrainInfo.valid &&
        !trainStandaloneFallThrough;
    if (skipStandaloneFallThrough) {
        return;
    }

    const bool providerMatchesTraining = provider.found &&
        entryMatchesTraining(provider.entry, trainedFirstBlock, secondBlockTrainInfo);
    const bool providerFirstBlockMatches = provider.found &&
        blocksMatch(provider.entry.firstBlock(), trainedFirstBlock);
    const bool providerFirstBlockIdentityMatches = provider.found &&
        blockIdentityMatches(provider.entry.firstBlock(), trainedFirstBlock);
    const bool altMatchesTraining = altProvider.found &&
        entryMatchesTraining(altProvider.entry, trainedFirstBlock, secondBlockTrainInfo);
    const bool canAllocHigher = provider.found &&
        provider.table < numPredictors - 1;
    bool preserveProviderOnMismatch = provider.found &&
        !providerMatchesTraining && canAllocHigher;
    bool providerRewrittenForIdentity = false;

    if (provider.found && !providerFirstBlockIdentityMatches &&
        canAllocHigher) {
        auto &providerEntry =
            tageTable[provider.table][provider.index][provider.way];
        const auto oldEntry = providerEntry;
        const bool shouldRewrite = providerEntry.weakenIdentity();
        if (shouldRewrite && oldEntry.identityConfidence == 0) {
            PairTAGEEntry newEntry;
            newEntry.valid = true;
            newEntry.tag = getTageTag(
                pairEntryStartPC, provider.table,
                predMeta->tagFoldedHist[provider.table].get(),
                predMeta->altTagFoldedHist[provider.table].get());
            newEntry.counter = trainedFirstBlock.taken ? 0 : -1;
            newEntry.useful = false;
            newEntry.identityConfidence = PairTAGEEntry::InitialIdentityConfidence;
            newEntry.setBlock(0, trainedFirstBlock);
            if (secondBlockTrainInfo.valid) {
                newEntry.setBlock(1, secondBlockTrainInfo);
            } else {
                newEntry.clearBlock(1);
            }

            providerEntry = newEntry;
            preserveProviderOnMismatch = false;
            providerRewrittenForIdentity = true;
        }
    } else if (provider.found && providerFirstBlockIdentityMatches &&
               !providerFirstBlockMatches && preserveProviderOnMismatch) {
        auto &providerEntry =
            tageTable[provider.table][provider.index][provider.way];
        providerEntry.strengthenIdentity();
    }

    if (provider.found && !preserveProviderOnMismatch &&
        !providerRewrittenForIdentity) {
        auto &providerEntry =
            tageTable[provider.table][provider.index][provider.way];
        auto oldEntry = providerEntry;
        PairTAGEEntry newEntry = oldEntry;

        short trainedCounter = oldEntry.counter;
        if (blocksMatch(oldEntry.firstBlock(), trainedFirstBlock)) {
            updateCounter(trainedFirstBlock.taken, 3, trainedCounter);
        } else {
            trainedCounter = trainedFirstBlock.taken ? 0 : -1;
        }

        newEntry.valid = true;
        newEntry.tag = getTageTag(
            pairEntryStartPC, provider.table,
            predMeta->tagFoldedHist[provider.table].get(),
            predMeta->altTagFoldedHist[provider.table].get());
        newEntry.counter = trainedCounter;
        newEntry.useful = oldEntry.useful;
        if (providerMatchesTraining && altProvider.found && !altMatchesTraining) {
            newEntry.useful = true;
        }
        if (providerFirstBlockIdentityMatches) {
            newEntry.strengthenIdentity();
        } else {
            newEntry.identityConfidence = PairTAGEEntry::InitialIdentityConfidence;
        }
        newEntry.setBlock(0, trainedFirstBlock);
        if (secondBlockTrainInfo.valid) {
            newEntry.setBlock(1, secondBlockTrainInfo);
        } else {
            newEntry.clearBlock(1);
        }

        providerEntry = newEntry;
    } else if (provider.found && providerFirstBlockMatches) {
        auto &providerEntry =
            tageTable[provider.table][provider.index][provider.way];
        auto oldEntry = providerEntry;
        PairTAGEEntry newEntry = oldEntry;
        short trainedCounter = oldEntry.counter;

        updateCounter(trainedFirstBlock.taken, 3, trainedCounter);
        newEntry.counter = trainedCounter;
        if (altProvider.found && !altMatchesTraining) {
            newEntry.useful = true;
        }
        newEntry.strengthenIdentity();

        if (newEntry.counter != oldEntry.counter ||
            newEntry.useful != oldEntry.useful ||
            newEntry.identityConfidence != oldEntry.identityConfidence) {
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
        allocateEntries(pairEntryStartPC, *predMeta, trainedFirstBlock,
                        secondBlockTrainInfo, allocStartTable);
    }
}

Addr
PairTAGE::getTageTag(Addr pc, int table, uint64_t foldedHist, uint64_t altFoldedHist) const
{
    const Addr mask = (1ULL << tableTagBits[table]) - 1;
    const unsigned pcShift = tablePcShifts[table];
    const Addr pcBits = (pc >> pcShift) & mask;
    const Addr foldedBits = foldedHist & mask;
    const Addr altTagBits = (altFoldedHist << 1) & mask;

    return pcBits ^ foldedBits ^ altTagBits;
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

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5
