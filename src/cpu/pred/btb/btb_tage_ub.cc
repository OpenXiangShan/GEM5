#include "cpu/pred/btb/btb_tage_ub.hh"

#include <algorithm>
#include <cmath>
#include <iterator>

#include <boost/dynamic_bitset.hpp>

#ifndef UNIT_TEST
#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/TAGE.hh"

#endif

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

#ifdef UNIT_TEST
namespace test {
#endif

std::size_t
BTBTAGEUpperBound::ExactHistoryKeyHash::operator()(
    const ExactHistoryKey &key) const
{
    auto mix = [](uint64_t value) {
        value ^= value >> 30;
        value *= 0xbf58476d1ce4e5b9ULL;
        value ^= value >> 27;
        value *= 0x94d049bb133111ebULL;
        value ^= value >> 31;
        return value;
    };

    uint64_t seed = mix(key.branchPC ^ key.activeWords);
    for (unsigned i = 0; i < key.activeWords; ++i) {
        seed ^= mix(key.words[i] + 0x9e3779b97f4a7c15ULL +
                    (seed << 6) + (seed >> 2));
    }
    return static_cast<std::size_t>(seed);
}

#ifdef UNIT_TEST
BTBTAGEUpperBound::BTBTAGEUpperBound(unsigned numPredictors,
                                     unsigned tableSize,
                                     unsigned numBanks,
                                     HistorySource source)
    : BTBTAGE(numPredictors, 1, tableSize, numBanks),
      ubStats(numPredictors),
      historySource(source)
{
    updateOnRead = false;
    initUpperBoundState();
}
#else
BTBTAGEUpperBound::BTBTAGEUpperBound(const Params &p)
    : BTBTAGE(p),
      ubStats(this, p.numPredictors),
      historySource(p.usePathHashHistory ? HistorySource::PathHash
                                         : HistorySource::Outcome)
{
    updateOnRead = false;
    initUpperBoundState();
}
#endif

#ifndef UNIT_TEST
BTBTAGEUpperBound::UpperBoundStats::UpperBoundStats(
    statistics::Group *parent, unsigned numPredictors)
    : statistics::Group(parent),
      ADD_STAT(liveContextsPerTable, statistics::units::Count::get(),
               "Number of live exact-history contexts in each upper-bound table"),
      ADD_STAT(totalContexts, statistics::units::Count::get(),
               "Total number of live exact-history contexts"),
      ADD_STAT(updateAllocInsert, statistics::units::Count::get(),
               "Number of exact-history entry insertions on update"),
      ADD_STAT(updateAllocAllTablesHit, statistics::units::Count::get(),
               "Updates where all higher tables already had the exact context")
{
    liveContextsPerTable.init(numPredictors);
}
#endif

void
BTBTAGEUpperBound::initUpperBoundState()
{
    exactTables.clear();
    exactTables.resize(numPredictors);
    historyBlocksScratch.reserve(MaxHistoryWords);
    exactPathHistory.resize(maxHistLen, false);

    for (unsigned i = 0; i < numPredictors; ++i) {
#ifdef UNIT_TEST
        assert(histLengths[i] <= MaxSupportedHistBits);
#else
        fatal_if(histLengths[i] > MaxSupportedHistBits,
                 "BTBTAGEUpperBound only supports history lengths up to %u "
                 "bits, got table %u length %u",
                 MaxSupportedHistBits, i, histLengths[i]);
#endif

        unsigned reserveEntries = std::max<unsigned>(16,
            tableSizes[i] * std::max<unsigned>(1, numWays[i]));
        exactTables[i].reserve(reserveEntries);
    }
}

void
BTBTAGEUpperBound::updatePathHistory(bitset &history, bool taken, Addr pc,
                                     Addr target) const
{
    if (!taken || history.empty()) {
        return;
    }

    uint64_t hash = pathHash(pc, target);
    history <<= PathHistoryShift;
    for (unsigned i = 0; i < pathHashLength && i < history.size(); ++i) {
        history[i] = (hash & 1) ^ history[i];
        hash >>= 1;
    }
}

const BTBTAGEUpperBound::bitset &
BTBTAGEUpperBound::selectHistory(const bitset &outcomeHistory) const
{
    if (historySource == HistorySource::PathHash) {
        return exactPathHistory;
    }
    return outcomeHistory;
}

void
BTBTAGEUpperBound::captureHistoryWords(
    const bitset &history,
    std::array<uint64_t, MaxHistoryWords> &words) const
{
    words.fill(0);

    historyBlocksScratch.clear();
    historyBlocksScratch.reserve((history.size() + 63) / 64);
    boost::to_block_range(history, std::back_inserter(historyBlocksScratch));

    const unsigned copyWords = std::min<unsigned>(
        MaxHistoryWords, historyBlocksScratch.size());
    for (unsigned i = 0; i < copyWords; ++i) {
        words[i] = historyBlocksScratch[i];
    }
}

BTBTAGEUpperBound::ExactHistoryKey
BTBTAGEUpperBound::buildKey(
    Addr branchPC,
    const std::array<uint64_t, MaxHistoryWords> &words,
    unsigned histLen) const
{
    ExactHistoryKey key;
    key.branchPC = branchPC;
    key.words.fill(0);
    key.activeWords = (histLen + 63) / 64;

    for (unsigned i = 0; i < key.activeWords; ++i) {
        key.words[i] = words[i];
    }

    const unsigned rem = histLen % 64;
    if (key.activeWords > 0 && rem != 0) {
        key.words[key.activeWords - 1] &= ((1ULL << rem) - 1);
    }

    return key;
}

BTBTAGE::TageTableInfo
BTBTAGEUpperBound::makeTableInfo(bool found, const TageEntry *entry,
                                 unsigned table) const
{
    if (!found || entry == nullptr) {
        return TageTableInfo();
    }
    return TageTableInfo(true, *entry, table, 0, 0, 0);
}

BTBTAGE::TagePrediction
BTBTAGEUpperBound::lookupExactPrediction(
    const BTBEntry &btbEntry,
    const std::array<uint64_t, MaxHistoryWords> &historyWords,
    BranchPredictionMeta *metaOut) const
{
    bool provided = false;
    bool altProvided = false;
    TageTableInfo mainInfo, altInfo;
    BranchPredictionMeta localMeta;
    uint64_t hitTableMask = 0;

    for (int i = numPredictors - 1; i >= 0; --i) {
        auto key = buildKey(btbEntry.pc, historyWords, histLengths[i]);
        auto it = exactTables[i].find(key);
        if (it == exactTables[i].end()) {
            continue;
        }
        if (i < 64) {
            hitTableMask |= (1ULL << i);
        }

        if (!provided) {
            mainInfo = makeTableInfo(true, &it->second, i);
            localMeta.main = {true, static_cast<unsigned>(i), key};
            provided = true;
        } else if (!altProvided) {
            altInfo = makeTableInfo(true, &it->second, i);
            localMeta.alt = {true, static_cast<unsigned>(i), key};
            altProvided = true;
            break;
        }
    }

    if (metaOut != nullptr) {
        *metaOut = localMeta;
    }

    const bool mainTaken = mainInfo.taken();
    const bool altTaken = altInfo.taken();
    const bool baseTaken = btbEntry.ctr >= 0;
    const bool altPred = altProvided ? altTaken : baseTaken;
    Addr useAltIdx = getUseAltIdx(btbEntry.pc);
    short useAltCtr = useAlt[useAltIdx];

    bool useAltPred = false;
    if (!provided) {
        useAltPred = true;
    } else {
        const bool mainWeak =
            (mainInfo.entry.counter == 0 || mainInfo.entry.counter == -1);
        if (mainWeak) {
            useAltPred = (useAltCtr >= 0);
        }
    }

    const bool taken = useAltPred ? altPred : mainTaken;
    const short mainConfidence = mainInfo.entry.confidence();
    const short altConfidence = altProvided ?
        altInfo.entry.confidence() : btbEntry.confidence();
    const short confidence = useAltPred ? altConfidence : mainConfidence;
    int finalProviderTable = -1;
    bool finalProviderIsAlt = false;
    if (!useAltPred && provided) {
        finalProviderTable = mainInfo.table;
    } else if (useAltPred && altProvided) {
        finalProviderTable = altInfo.table;
        finalProviderIsAlt = true;
    }

    return TagePrediction(btbEntry.pc, mainInfo, altInfo, useAltPred, taken,
                          altPred, finalProviderTable, finalProviderIsAlt,
                          useAltIdx, useAltCtr, hitTableMask, confidence);
}

void
BTBTAGEUpperBound::notePredictionResult(
    const BTBEntry &btbEntry,
    const TagePrediction &pred,
    std::unordered_map<Addr, TageInfoForMGSC> &tageInfoForMgscs,
    CondTakens &results) const
{
    results.push_back({btbEntry.pc, pred.taken || btbEntry.alwaysTaken});
    tageInfoForMgscs[btbEntry.pc].tage_pred_taken = pred.taken;
    tageInfoForMgscs[btbEntry.pc].tage_main_taken =
        pred.mainInfo.found ? pred.mainInfo.taken() : false;
    tageInfoForMgscs[btbEntry.pc].tage_pred_conf_high =
        pred.mainInfo.found && abs(pred.mainInfo.entry.counter * 2 + 1) == 7;
    tageInfoForMgscs[btbEntry.pc].tage_pred_conf_mid =
        pred.mainInfo.found &&
        (abs(pred.mainInfo.entry.counter * 2 + 1) < 7 &&
         abs(pred.mainInfo.entry.counter * 2 + 1) > 1);
    tageInfoForMgscs[btbEntry.pc].tage_pred_conf_low =
        !pred.mainInfo.found ||
        (abs(pred.mainInfo.entry.counter * 2 + 1) <= 1);
    tageInfoForMgscs[btbEntry.pc].tage_pred_alt_diff =
        pred.mainInfo.found && pred.mainInfo.taken() != pred.altPred;
}

void
BTBTAGEUpperBound::putPCHistory(Addr startAddr, const bitset &history,
                                std::vector<FullBTBPrediction> &stagePreds)
{
    lastPredBankId = getBankId(startAddr);
    predBankValid = true;

#ifndef UNIT_TEST
    tageStats.predAccessPerBank[lastPredBankId]++;
#endif

    ubMeta = std::make_shared<UpperBoundMeta>();
    const bitset &selectedHistory = selectHistory(history);
    ubMeta->history = selectedHistory;
    captureHistoryWords(selectedHistory, ubMeta->historyWords);

    for (int s = getDelay(); s < stagePreds.size(); ++s) {
        auto &stagePred = stagePreds[s];
        stagePred.condTakens.clear();
        stagePred.condConfidence.clear();
        stagePred.tageInfoForMgscs.clear();

        for (auto &btbEntry : stagePred.btbEntries) {
            if (!(btbEntry.isCond && btbEntry.valid)) {
                continue;
            }

            BranchPredictionMeta branchMeta;
            auto pred = lookupExactPrediction(
                btbEntry, ubMeta->historyWords, &branchMeta);
            ubMeta->preds[btbEntry.pc] = pred;
            ubMeta->branchMeta[btbEntry.pc] = branchMeta;
            tageStats.updateStatsWithTagePrediction(pred, true);
            notePredictionResult(btbEntry, pred, stagePred.tageInfoForMgscs,
                                 stagePred.condTakens);
            stagePred.condConfidence.push_back({btbEntry.pc, pred.confidence});
        }
    }
}

std::shared_ptr<void>
BTBTAGEUpperBound::getPredictionMeta(ThreadID tid)
{
    (void)tid;
    return ubMeta;
}

void
BTBTAGEUpperBound::specUpdatePHist(const boost::dynamic_bitset<> &history,
                                   FullBTBPrediction &pred,
                                   const PathHistoryUpdate &update)
{
    (void)pred;

    if (historySource != HistorySource::PathHash) {
        return;
    }

    exactPathHistory = history;
    updatePathHistory(exactPathHistory, update.taken, update.pc,
                      update.target);
}

void
BTBTAGEUpperBound::recoverHist(const boost::dynamic_bitset<> &history,
                               const FetchTarget &entry, int shamt,
                               bool cond_taken)
{
    (void)history;
    (void)entry;
    (void)shamt;
    (void)cond_taken;
}

void
BTBTAGEUpperBound::recoverPHist(const boost::dynamic_bitset<> &history,
                                const FetchTarget &entry,
                                const PathHistoryUpdate &update)
{
    (void)entry;

    if (historySource != HistorySource::PathHash) {
        return;
    }

    exactPathHistory = history;
    updatePathHistory(exactPathHistory, update.taken, update.pc,
                      update.target);
}

bool
BTBTAGEUpperBound::updatePredictorStateAndCheckAllocation(
    const BTBEntry &entry, bool actualTaken, const TagePrediction &pred,
    const BranchPredictionMeta &meta, const FetchTarget &stream)
{
    tageStats.updateStatsWithTagePrediction(pred, false);

    const auto &mainInfo = pred.mainInfo;
    const auto &altInfo = pred.altInfo;
    const bool usedAlt = pred.useAlt;
    const bool baseTaken = entry.ctr >= 0;
    const bool altTaken = altInfo.found ? altInfo.taken() : baseTaken;

    if (mainInfo.found) {
        const bool mainWeak =
            (mainInfo.entry.counter == 0 || mainInfo.entry.counter == -1);
        if (mainWeak) {
            tageStats.updateProviderNa++;
            Addr uidx = getUseAltIdx(entry.pc);
            bool altCorrect = (altTaken == actualTaken);
            updateCounter(altCorrect, useAltOnNaWidth, useAlt[uidx]);
            tageStats.updateUseAltOnNaUpdated++;
            if (altCorrect) {
                tageStats.updateUseAltOnNaCorrect++;
            } else {
                tageStats.updateUseAltOnNaWrong++;
            }
        }
    }

    if (meta.main.found) {
        auto tableIt = exactTables[meta.main.table].find(meta.main.key);
        assert(tableIt != exactTables[meta.main.table].end());
        auto &way = tableIt->second;

        updateCounter(actualTaken, 3, way.counter);

        const bool mainIsCorrect = mainInfo.taken() == actualTaken;
        const bool altIsCorrectAndStrong = altInfo.found &&
            (altInfo.taken() == actualTaken) &&
            (abs(2 * altInfo.entry.counter + 1) == 7);

        if (altIsCorrectAndStrong && mainIsCorrect) {
            way.useful = 0;
        } else if (mainInfo.taken() != altTaken && mainIsCorrect) {
            way.useful = 1;
        }

        if (way.counter == 0 || way.counter == -1) {
            way.useful = 0;
        }
    }

    if (usedAlt && meta.alt.found) {
        auto tableIt = exactTables[meta.alt.table].find(meta.alt.key);
        assert(tableIt != exactTables[meta.alt.table].end());
        updateCounter(actualTaken, 3, tableIt->second.counter);
    }

    if (usedAlt) {
        bool altCorrect = altTaken == actualTaken;
        if (altCorrect) {
            tageStats.updateUseAltCorrect++;
        } else {
            tageStats.updateUseAltWrong++;
        }
        if (mainInfo.found && mainInfo.taken() != altTaken) {
            tageStats.updateAltDiffers++;
        }
    }

    const bool thisFbMispred =
        stream.squashType == SquashType::SQUASH_CTRL &&
        stream.squashPC == entry.pc;
    if (getDelay() == 2 && thisFbMispred) {
        tageStats.updateMispred++;
        if (!usedAlt && mainInfo.found) {
#ifndef UNIT_TEST
            tageStats.updateTableMispreds[mainInfo.table]++;
#endif
        }
    }

    if (!thisFbMispred) {
        return false;
    }

    if (usedAlt && mainInfo.found && mainInfo.taken() == actualTaken) {
        return false;
    }

    return true;
}

bool
BTBTAGEUpperBound::allocateExactEntry(
    const BTBEntry &entry, bool actualTaken, unsigned startTable,
    const std::array<uint64_t, MaxHistoryWords> &historyWords,
    uint64_t &allocatedTable)
{
    for (unsigned ti = startTable; ti < numPredictors; ++ti) {
        auto key = buildKey(entry.pc, historyWords, histLengths[ti]);
        auto [it, inserted] = exactTables[ti].emplace(
            key, TageEntry(0, actualTaken ? 0 : -1, entry.pc));
        if (!inserted) {
            continue;
        }

        refreshContextStats(ti);
        ubStats.totalContexts++;
        ubStats.updateAllocInsert++;
        tageStats.updateAllocSuccess++;
        allocatedTable = ti;
        return true;
    }

    ubStats.updateAllocAllTablesHit++;
    return false;
}

std::vector<BTBEntry>
BTBTAGEUpperBound::prepareUpperBoundUpdateEntries(const FetchTarget &stream)
{
    auto allEntries = stream.updateBTBEntries;

    if (!stream.updateIsOldEntry) {
        BTBEntry potentialNewEntry = stream.updateNewBTBEntry;
        bool newEntryTaken =
            stream.exeTaken && stream.getControlPC() == potentialNewEntry.pc;
        if (!newEntryTaken) {
            potentialNewEntry.alwaysTaken = false;
        }
        allEntries.push_back(potentialNewEntry);
    }

    if (getResolvedUpdate()) {
        auto removeIt = std::remove_if(
            allEntries.begin(), allEntries.end(),
            [](const BTBEntry &e) {
                return !(e.isCond && !e.alwaysTaken && e.resolved);
            });
        allEntries.erase(removeIt, allEntries.end());
    } else {
        auto removeIt = std::remove_if(
            allEntries.begin(), allEntries.end(),
            [](const BTBEntry &e) {
                return !(e.isCond && !e.alwaysTaken);
            });
        allEntries.erase(removeIt, allEntries.end());
    }

    return allEntries;
}

void
BTBTAGEUpperBound::refreshContextStats(unsigned table)
{
    ubStats.liveContextsPerTable[table] = exactTables[table].size();
}

void
BTBTAGEUpperBound::update(const FetchTarget &stream)
{
    auto entriesToUpdate = prepareUpperBoundUpdateEntries(stream);
    auto predMeta = std::static_pointer_cast<UpperBoundMeta>(
        stream.predMetas[getComponentIdx()]);
    if (!predMeta) {
        return;
    }

    bool hasStoredVsActualDiff = false;
    for (auto &btbEntry : entriesToUpdate) {
        auto predIt = predMeta->preds.find(btbEntry.pc);
        auto metaIt = predMeta->branchMeta.find(btbEntry.pc);
        const bool actualTaken =
            stream.exeTaken && stream.exeBranchInfo == btbEntry;
        TagePrediction storedPred;
        BranchPredictionMeta storedMeta;
        if (predIt != predMeta->preds.end() &&
            metaIt != predMeta->branchMeta.end()) {
            storedPred = predIt->second;
            storedMeta = metaIt->second;
        } else {
            // BTB miss / new conditional branches are absent from prediction-time
            // maps, but they still must be trained using the prediction-time
            // history snapshot carried in predMeta.
            storedPred = lookupExactPrediction(
                btbEntry, predMeta->historyWords, &storedMeta);
        }

        if (storedPred.taken != actualTaken) {
            hasStoredVsActualDiff = true;
        }

        bool needAllocate = updatePredictorStateAndCheckAllocation(
            btbEntry, actualTaken, storedPred, storedMeta, stream);

        if (needAllocate) {
            uint64_t allocatedTable = 0;
            unsigned startTable = 0;
            if (storedMeta.main.found) {
                startTable = storedMeta.main.table + 1;
            }
            allocateExactEntry(btbEntry, actualTaken, startTable,
                               predMeta->historyWords, allocatedTable);
        }
    }

    if (hasStoredVsActualDiff) {
        tageStats.recomputedVsActualDiff++;
    }
    if (getDelay() < 2) {
        checkUtageUpdateMisspred(stream);
    }
}

void
BTBTAGEUpperBound::checkFoldedHist(const bitset &history, const char *when)
{
    (void)when;
    if (historySource == HistorySource::PathHash) {
        assert(exactPathHistory == history);
    }
}

#ifdef UNIT_TEST
BTBTAGEUpperBound::ExactHistoryKey
BTBTAGEUpperBound::makeExactKey(Addr branchPC, const bitset &history,
                                unsigned table) const
{
    std::array<uint64_t, MaxHistoryWords> historyWords{};
    captureHistoryWords(history, historyWords);
    return buildKey(branchPC, historyWords, histLengths[table]);
}

bool
BTBTAGEUpperBound::insertExactEntry(unsigned table, Addr branchPC,
                                    const bitset &history, short counter,
                                    bool useful)
{
    auto key = makeExactKey(branchPC, history, table);
    auto [it, inserted] = exactTables[table].emplace(
        key, TageEntry(0, counter, branchPC));
    it->second.useful = useful;
    if (inserted) {
        refreshContextStats(table);
        ubStats.totalContexts++;
        ubStats.updateAllocInsert++;
    }
    return inserted;
}

bool
BTBTAGEUpperBound::hasExactEntry(unsigned table, Addr branchPC,
                                 const bitset &history) const
{
    auto key = makeExactKey(branchPC, history, table);
    return exactTables[table].find(key) != exactTables[table].end();
}
#endif

#ifdef UNIT_TEST
} // namespace test
#endif

} // namespace btb_pred

} // namespace branch_prediction

} // namespace gem5
