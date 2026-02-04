#include "cpu/pred/btb/btb_tage.hh"

#include <algorithm>
#include <cmath>
#include <ctime>

#ifdef UNIT_TEST
// Define debug flags for unit testing
namespace gem5 {
namespace debug {
    bool TAGEUseful = true;
    bool TAGEHistory = true;
}
}
#endif

#ifndef UNIT_TEST
#include "base/debug_helper.hh"
#include "base/intmath.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "cpu/o3/dyn_inst.hh"
#include "debug/TAGE.hh"
#endif
namespace gem5 {

namespace branch_prediction {

namespace btb_pred{

#ifdef UNIT_TEST
namespace test {
#endif

#ifdef UNIT_TEST
// Test constructor for unit testing mode
BTBTAGE::BTBTAGE(unsigned numPredictors, unsigned numWays, unsigned tableSize, unsigned numBanks)
    : TimedBaseBTBPredictor(),
      numPredictors(numPredictors),
      numWays(numWays),
      maxBranchPositions(32),
      useAltOnNaSize(1024),
      useAltOnNaWidth(7),
      updateOnRead(false),
      numBanks(numBanks),
      bankIdWidth(ceilLog2(numBanks)),
      blockWidth(floorLog2(blockSize)),
      bankBaseShift(instShiftAmt),
      indexShift(bankBaseShift + ceilLog2(numBanks)),
      enableBankConflict(false),
      lastPredBankId(0),
      predBankValid(false),
      tageStats()
{
    setNumDelay(1);

    // Initialize with default parameters for testing
    tableSizes.resize(numPredictors, tableSize);
    tableTagBits.resize(numPredictors, 8);
    tablePcShifts.resize(numPredictors, 1);
    histLengths.resize(numPredictors);
    for (unsigned i = 0; i < numPredictors; ++i) {
        histLengths[i] = (i + 1) * 4;
    }
    maxHistLen = histLengths[numPredictors-1];
    numTablesToAlloc = 1;
    enableSC = false;
#else
// Constructor: Initialize TAGE predictor with given parameters
BTBTAGE::BTBTAGE(const Params& p):
TimedBaseBTBPredictor(p),
numPredictors(p.numPredictors),
tableSizes(p.tableSizes),
tableTagBits(p.TTagBitSizes),
tablePcShifts(p.TTagPcShifts),
histLengths(p.histLengths),
maxHistLen(p.maxHistLen),
numWays(p.numWays),
maxBranchPositions(p.maxBranchPositions),
useAltOnNaSize(p.useAltOnNaSize),
useAltOnNaWidth(p.useAltOnNaWidth),
numTablesToAlloc(p.numTablesToAlloc),
enableSC(p.enableSC),
updateOnRead(p.updateOnRead),
numBanks(p.numBanks),
bankIdWidth(ceilLog2(p.numBanks)),
blockWidth(floorLog2(blockSize)),
bankBaseShift(instShiftAmt), // strip instruction alignment bits before indexing
indexShift(bankBaseShift + ceilLog2(p.numBanks)),
enableBankConflict(p.enableBankConflict),
lastPredBankId(0),
predBankValid(false),
tageStats(this, p.numPredictors, p.numBanks)
{
    this->needMoreHistories = p.needMoreHistories;

    // Warn if updateOnRead is disabled (bank simulation works better with it enabled)
    if (!p.updateOnRead) {
        warn("BTBTAGE: Bank simulation works better with updateOnRead=true");
    }
#endif
    tageTable.resize(numPredictors);
    tableIndexBits.resize(numPredictors);
    tableIndexMasks.resize(numPredictors);
    tableTagBits.resize(numPredictors);
    tableTagMasks.resize(numPredictors);

    for (unsigned int i = 0; i < numPredictors; ++i) {
        //initialize ittage predictor
        assert(tableSizes.size() >= numPredictors);
        tageTable[i].resize(tableSizes[i]);
        for (unsigned int j = 0; j < tableSizes[i]; ++j) {
            tageTable[i][j].resize(numWays);
        }

        tableIndexBits[i] = ceilLog2(tableSizes[i]);
        tableIndexMasks[i].resize(tableIndexBits[i], true);

        assert(histLengths.size() >= numPredictors);

        assert(tableTagBits.size() >= numPredictors);
        tableTagMasks[i].resize(tableTagBits[i], true);

        assert(tablePcShifts.size() >= numPredictors);

        tagFoldedHist.push_back(PathFoldedHist((int)histLengths[i], (int)tableTagBits[i], 16));
        altTagFoldedHist.push_back(PathFoldedHist((int)histLengths[i], (int)tableTagBits[i]-1, 16));
        indexFoldedHist.push_back(PathFoldedHist((int)histLengths[i], (int)tableIndexBits[i], 16));
    }
    usefulResetCnt = 0;

    // initialize use_alt_on_na table
    useAlt.resize(useAltOnNaSize, 0);
#ifndef UNIT_TEST
    hasDB = true;
    switch (getDelay()) {
        case 0: dbName = std::string("microtage"); break;
        default: dbName = std::string("tage"); break;
    }
#endif
}

BTBTAGE::~BTBTAGE()
{
}

// Set up tracing for debugging
void
BTBTAGE::setTrace()
{
#ifndef UNIT_TEST
    if (enableDB) {
        std::vector<std::pair<std::string, DataType>> fields_vec = {
            std::make_pair("startPC", UINT64),
            std::make_pair("branchPC", UINT64),
            std::make_pair("wayIdx", UINT64),
            std::make_pair("mainFound", UINT64),
            std::make_pair("mainCounter", UINT64),
            std::make_pair("mainUseful", UINT64),
            std::make_pair("mainTable", UINT64),
            std::make_pair("mainIndex", UINT64),
            std::make_pair("altFound", UINT64),
            std::make_pair("altCounter", UINT64),
            std::make_pair("altUseful", UINT64),
            std::make_pair("altTable", UINT64),
            std::make_pair("altIndex", UINT64),
            std::make_pair("useAlt", UINT64),
            std::make_pair("predTaken", UINT64),
            std::make_pair("actualTaken", UINT64),
            std::make_pair("allocSuccess", UINT64),
            std::make_pair("allocTable", UINT64),
            std::make_pair("allocIndex", UINT64),
            std::make_pair("allocWay", UINT64),
            std::make_pair("history", TEXT),
            std::make_pair("indexFoldedHist", UINT64),
        };
        tageMissTrace = _db->addAndGetTrace("TAGEMISSTRACE", fields_vec);
        tageMissTrace->init_table();
    }
#endif
}

void
BTBTAGE::tick() {}

void
BTBTAGE::tickStart() {}

namespace
{
inline bool
isWeakConf(short conf)
{
    return conf == 0 || conf == -1;
}

inline bool
isStrongConf(short conf)
{
    return conf >= 2;
}
} // namespace

/**
 * @brief Lookup provider/alt entries for this fetch block.
 */
std::pair<BTBTAGE::TageTableInfo, BTBTAGE::TageTableInfo>
BTBTAGE::lookupProviders(const Addr &startPC, std::shared_ptr<TageMeta> predMeta)
{
    bool provided = false;
    bool alt_provided = false;
    TageTableInfo main_info, alt_info;

    for (int i = numPredictors - 1; i >= 0; --i) {
        Addr index = predMeta ? getTageIndex(startPC, i, predMeta->indexFoldedHist[i].get())
                              : getTageIndex(startPC, i);
        Addr tag = predMeta ? getTageTag(startPC, i,
                                         predMeta->tagFoldedHist[i].get(),
                                         predMeta->altTagFoldedHist[i].get())
                            : getTageTag(startPC, i);

        bool match = false;
        TageEntry matching_entry;
        unsigned matching_way = 0;

        for (unsigned way = 0; way < numWays; way++) {
            auto &entry = tageTable[i][index][way];
            if (entry.valid && tag == entry.tag) {
                matching_entry = entry;
                matching_way = way;
                match = true;
                DPRINTF(TAGE,
                        "hit table %d[%lu][%u]: tag %lu, conf %d, u %d, exitEnc %u\n",
                        i, index, way, entry.tag, entry.conf, entry.useful, entry.exitSlotEnc);
                break;
            }
        }

        if (match) {
            if (!provided) {
                main_info = TageTableInfo(true, matching_entry, i, index, tag, matching_way);
                provided = true;
            } else if (!alt_provided) {
                alt_info = TageTableInfo(true, matching_entry, i, index, tag, matching_way);
                alt_provided = true;
                break;
            }
        }
    }

    return {main_info, alt_info};
}

uint8_t
BTBTAGE::getBaseExitSlotEnc(const Addr &startPC,
                            const std::vector<BTBEntry> &btbEntries) const
{
    // Base: scan cond branches in PC order; choose the first predicted-taken cond.
    for (auto &e : btbEntries) {
        if (!(e.valid && e.isCond)) {
            continue;
        }
        const bool pred_taken = e.alwaysTaken || (e.ctr >= 0);
        if (pred_taken) {
            unsigned slot = getBranchIndexInBlock(e.pc, startPC);
            return static_cast<uint8_t>(slot + 1);
        }
    }
    return 0;
}

Addr
BTBTAGE::mapExitSlotToCondPC(const Addr &startPC,
                             const std::vector<BTBEntry> &btbEntries,
                             uint8_t predEnc) const
{
    if (predEnc == 0 || predEnc > 32) {
        return 0;
    }
    const unsigned pred_slot = predEnc - 1;
    for (auto &e : btbEntries) {
        if (!(e.valid && e.isCond)) {
            continue;
        }
        if (getBranchIndexInBlock(e.pc, startPC) == pred_slot) {
            return e.pc;
        }
    }
    return 0;
}

void
BTBTAGE::lookupHelper(const Addr &startPC, const std::vector<BTBEntry> &btbEntries,
                      std::unordered_map<Addr, TageInfoForMGSC> &tageInfoForMgscs,
                      CondTakens &results)
{
    DPRINTF(TAGE, "lookupHelper(startPC=%#lx)\n", startPC);

    tageInfoForMgscs.clear();

    const uint8_t baseEnc = getBaseExitSlotEnc(startPC, btbEntries);
    auto [main_info, alt_info] = lookupProviders(startPC);

    bool use_alt = false;
    PredSource source = PredSource::Base;
    uint8_t predEnc = baseEnc;

    if (main_info.found) {
        const bool weak = isWeakConf(main_info.entry.conf);
        if (weak) {
            Addr uidx = getUseAltIdx(startPC);
            use_alt = (useAlt[uidx] >= 0);
        }

        if (!weak) {
            source = PredSource::Provider;
            predEnc = main_info.entry.exitSlotEnc;
        } else if (use_alt) {
            if (alt_info.found) {
                source = PredSource::Alt;
                predEnc = alt_info.entry.exitSlotEnc;
            } else {
                source = PredSource::Base;
                predEnc = baseEnc;
            }
        } else {
            source = PredSource::Provider;
            predEnc = main_info.entry.exitSlotEnc;
        }
    } else {
        use_alt = true; // consistent with old "no provider => consult alt/base"
        source = PredSource::Base;
        predEnc = baseEnc;
    }

    Addr predCondPC = mapExitSlotToCondPC(startPC, btbEntries, predEnc);
    bool payloadMapped = (predEnc != 0) && (predCondPC != 0);

    // If payload cannot be mapped to current MBTB entries, fall back to base as PRD suggests.
    if (source != PredSource::Base && predEnc != 0 && !payloadMapped) {
        tageStats.predPayloadMapFail++;
        source = PredSource::Base;
        predEnc = baseEnc;
        predCondPC = mapExitSlotToCondPC(startPC, btbEntries, predEnc);
        payloadMapped = (predEnc != 0) && (predCondPC != 0);
    }

    if (source == PredSource::Base) {
        tageStats.predBaseFallback++;
    }
    if (predEnc == 0) {
        tageStats.predNoCondExit++;
    }

    TagePrediction pred(startPC, main_info, alt_info,
                        use_alt, source, predEnc, baseEnc,
                        payloadMapped, predCondPC);
    meta->pred = pred;
    meta->hasPred = true;

    tageStats.updateStatsWithTagePrediction(pred, true);

    // Fill per-branch TAGE info for MGSC, and condTakens for control-flow selection.
    // - If source==Base: provide a direction prediction for each cond branch (like old behavior).
    // - Else: only mark the predicted exit cond as taken; others are implicitly NT.
    if (source == PredSource::Base) {
        for (auto &e : btbEntries) {
            if (!(e.valid && e.isCond)) {
                continue;
            }
            const bool base_taken = (e.ctr >= 0);
            results.push_back({e.pc, e.alwaysTaken || base_taken});
        }
    } else if (predCondPC != 0) {
        results.push_back({predCondPC, true});
    }

    // MGSC expects an entry for every cond BTB entry.
    const uint8_t altOrBaseEnc = alt_info.found ? alt_info.entry.exitSlotEnc : baseEnc;
    const bool provider_alt_diff = main_info.found && (main_info.entry.exitSlotEnc != altOrBaseEnc);
    const int provider_conf_metric = main_info.found ? std::abs(main_info.entry.conf * 2 + 1) : 0;

    for (auto &e : btbEntries) {
        if (!(e.valid && e.isCond)) {
            continue;
        }
        auto &info = tageInfoForMgscs[e.pc];

        bool pred_taken_no_always = false;
        if (source == PredSource::Base) {
            pred_taken_no_always = (e.ctr >= 0);
        } else {
            pred_taken_no_always = (predCondPC != 0) && (e.pc == predCondPC);
        }

        info.tage_pred_taken = pred_taken_no_always;
        info.tage_main_taken = (source == PredSource::Provider) && pred_taken_no_always;

        if ((source == PredSource::Provider) && pred_taken_no_always && main_info.found) {
            info.tage_pred_conf_high = provider_conf_metric == 7;
            info.tage_pred_conf_mid = (provider_conf_metric < 7) && (provider_conf_metric > 1);
            info.tage_pred_conf_low = provider_conf_metric <= 1;
        } else {
            info.tage_pred_conf_high = false;
            info.tage_pred_conf_mid = false;
            info.tage_pred_conf_low = true;
        }

        info.tage_pred_alt_diff = provider_alt_diff;
    }
}

void
BTBTAGE::dryRunCycle(Addr startPC) {
    // No operation in dry run cycle for BTBTAGE
    // Record prediction bank for next tick's conflict detection
    lastPredBankId = getBankId(startPC);
    predBankValid = true;

    return;
}

/**
 * @brief Makes predictions for a stream of instructions using TAGE predictor
 * 
 * This function is called during the prediction stage and:
 * 1. Uses lookupHelper to get predictions for all BTB entries
 * 2. Stores predictions in the stage prediction structure
 * 3. Handles multiple prediction stages with different delays
 * 
 * @param startPC Starting PC of the instruction stream
 * @param history Current branch history
 * @param stagePreds Vector of predictions for different pipeline stages
 */
void
BTBTAGE::putPCHistory(Addr startPC, const bitset &history, std::vector<FullBTBPrediction> &stagePreds) {
    // Record prediction bank for next tick's conflict detection
    lastPredBankId = getBankId(startPC);
    predBankValid = true;

#ifndef UNIT_TEST
    // Record prediction access per bank
    tageStats.predAccessPerBank[lastPredBankId]++;
#endif

    DPRINTF(TAGE, "putPCHistory startAddr: %#lx, bank: %u\n",
            startPC, lastPredBankId);

    // IMPORTANT: when this function is called,
    // btb entries should already be in stagePreds
    // get prediction and save it

    // Clear old prediction metadata and save current history state
    meta = std::make_shared<TageMeta>();
    meta->tagFoldedHist = tagFoldedHist;
    meta->altTagFoldedHist = altTagFoldedHist;
    meta->indexFoldedHist = indexFoldedHist;
    meta->history = history;

    for (int s = getDelay(); s < stagePreds.size(); s++) {
        // TODO: only lookup once for one btb entry in different stages
        auto &stage_pred = stagePreds[s];
        stage_pred.condTakens.clear();
        lookupHelper(startPC, stage_pred.btbEntries, stage_pred.tageInfoForMgscs, stage_pred.condTakens);
    }

}

std::shared_ptr<void>
BTBTAGE::getPredictionMeta() {
    return meta;
}

/**
 * @brief Handle allocation of new entries (block-level).
 *
 * @param startPC The starting PC address
 * @param realEnc The actual ExitSlotEnc (0..32)
 * @param start_table The starting table for allocation
 * @param meta The metadata of the predictor
 * @return true if allocation is successful
 */
bool
BTBTAGE::handleNewEntryAllocation(const Addr &startPC,
                                 uint8_t realEnc,
                                 unsigned start_table,
                                 std::shared_ptr<TageMeta> meta,
                                 uint64_t &allocated_table,
                                 uint64_t &allocated_index,
                                 uint64_t &allocated_way) {
    // Simple set-associative allocation (no LFSR, no per-way table gating):
    // - For each table from start_table upward, check the set at computed index.
    // - Prefer invalid ways; else choose any way with useful==0 and weak counter.
    // - If none, apply a one-step age penalty to a strong, not-useful way (no allocation).

    for (unsigned ti = start_table; ti < numPredictors; ++ti) {
        Addr newIndex = getTageIndex(startPC, ti, meta->indexFoldedHist[ti].get());
        Addr newTag = getTageTag(startPC, ti,
            meta->tagFoldedHist[ti].get(), meta->altTagFoldedHist[ti].get());

        auto &set = tageTable[ti][newIndex];

        // Allocate into invalid way or not-useful and weak way
        for (unsigned way = 0; way < numWays; ++way) {
            auto &cand = set[way];
            const bool weakish = std::abs(cand.conf * 2 + 1) <= 3; // -2,-1,0,1
            if (!cand.valid || (!cand.useful && weakish)) {
                short newConf = 0; // weak init
                DPRINTF(TAGE,
                        "allocating entry in table %d[%lu][%u], tag %lu, conf %d, exitEnc %u\n",
                        ti, newIndex, way, newTag, newConf, realEnc);
                cand = TageEntry(newTag, newConf, realEnc); // u = 0 default
                tageStats.updateAllocSuccess++;
                allocated_table = ti;
                allocated_index = newIndex;
                allocated_way = way;
                usefulResetCnt = usefulResetCnt <= 0 ? 0 : usefulResetCnt - 1;
                return true;
            }
        }

        // 3) Apply age penalty to one strong, not-useful way to make it replacable later
        for (unsigned way = 0; way < numWays; ++way) {
            auto &cand = set[way];
            const bool weakish = std::abs(cand.conf * 2 + 1) <= 3;
            if (!cand.useful && !weakish) {
                if (cand.conf > 0) cand.conf--; else cand.conf++;
                DPRINTF(TAGE, "age penalty applied on table %d[%lu][%u], new ctr %d\n",
                        ti, newIndex, way, cand.conf);
                break; // one penalty per table per update
            }
        }

        tageStats.updateAllocFailure++;
        usefulResetCnt++;
    }

    if (usefulResetCnt >= 256) {
        usefulResetCnt = 0;
        tageStats.updateResetU++;
        DPRINTF(TAGE, "reset useful bit of all entries\n");
        for (auto &table : tageTable) {
            for (auto &set : table) {
                for (auto &way : set) {
                    way.useful = false;
                }
            }
        }
    }

    DPRINTF(TAGE, "no eligible way found for allocation starting from table %d\n", start_table);
    tageStats.updateAllocFailureNoValidTable++;
    return false;
}

/**
 * @brief Probe resolved update for bank conflicts without mutating state.
 * Returns false if the update cannot proceed due to a bank conflict.
 */
bool
BTBTAGE::canResolveUpdate(const FetchTarget &stream) {
    Addr startAddr = stream.getRealStartPC();
    unsigned updateBank = getBankId(startAddr);

#ifndef UNIT_TEST
    // Record attempted update access per bank (even if it conflicts)
    tageStats.updateAccessPerBank[updateBank]++;
#endif

    if (enableBankConflict && predBankValid && updateBank == lastPredBankId) {
        tageStats.updateBankConflict++;
        tageStats.updateDeferredDueToConflict++;
#ifndef UNIT_TEST
        tageStats.updateBankConflictPerBank[updateBank]++;
#endif
        DPRINTF(TAGE, "Bank conflict detected: update bank %u conflicts with prediction bank %u, "
                      "deferring this update (will retry after blocking prediction)\n",
                      updateBank, lastPredBankId);
        predBankValid = false;
        return false;
    }

    return true;
}

/**
 * @brief Perform resolved update after probe success.
 */
void
BTBTAGE::doResolveUpdate(const FetchTarget &stream) {
    if (enableBankConflict && predBankValid) {
        // Prediction consumed; clear bank tag for next cycle
        predBankValid = false;
    }
    update(stream);
}

/**
 * @brief Updates the TAGE predictor state based on actual branch execution results
 * 
 * @param stream The fetch stream containing branch execution information
 */
void
BTBTAGE::update(const FetchTarget &stream) {
    Addr startAddr = stream.getRealStartPC();
    unsigned updateBank = getBankId(startAddr);

    DPRINTF(TAGE, "update startAddr: %#lx, bank: %u\n", startAddr, updateBank);

    auto predMeta = std::static_pointer_cast<TageMeta>(stream.predMetas[getComponentIdx()]);
    if (!predMeta || !predMeta->hasPred) {
        DPRINTF(TAGE, "update: no prediction meta, skip\n");
        return;
    }

    const TagePrediction &pred_at_pred = predMeta->pred;

    // RealEnc is defined on cond dimension only.
    uint8_t realEnc = 0;
    if (stream.exeTaken && stream.exeBranchInfo.isCond) {
        unsigned real_slot = getBranchIndexInBlock(stream.exeBranchInfo.pc, startAddr);
        realEnc = static_cast<uint8_t>(real_slot + 1);
    }

    const bool correct = (pred_at_pred.predEnc == realEnc);

    // Recompute provider/alt for update-on-read, or use stored info.
    TageTableInfo main_info, alt_info;
    if (updateOnRead) {
        std::tie(main_info, alt_info) = lookupProviders(startAddr, predMeta);
    } else {
        main_info = pred_at_pred.mainInfo;
        alt_info = pred_at_pred.altInfo;
    }

    // Track recomputed-vs-original differences (block-level).
    bool hasRecomputedVsActualDiff = false;
    bool hasRecomputedVsOriginalDiff = false;
    if (updateOnRead) {
        const uint8_t baseEnc = pred_at_pred.baseEnc;
        bool use_alt = false;
        PredSource src = PredSource::Base;
        uint8_t recEnc = baseEnc;
        if (main_info.found) {
            const bool weak = isWeakConf(main_info.entry.conf);
            if (weak) {
                Addr uidx = getUseAltIdx(startAddr);
                use_alt = (useAlt[uidx] >= 0);
            }
            if (!weak) {
                src = PredSource::Provider;
                recEnc = main_info.entry.exitSlotEnc;
            } else if (use_alt) {
                if (alt_info.found) {
                    src = PredSource::Alt;
                    recEnc = alt_info.entry.exitSlotEnc;
                } else {
                    src = PredSource::Base;
                    recEnc = baseEnc;
                }
            } else {
                src = PredSource::Provider;
                recEnc = main_info.entry.exitSlotEnc;
            }
        } else {
            src = PredSource::Base;
            recEnc = baseEnc;
        }
        // Use prediction-time BTB entries for payload mapping check.
        if (src != PredSource::Base && recEnc != 0 &&
            mapExitSlotToCondPC(startAddr, stream.predBTBEntries, recEnc) == 0) {
            src = PredSource::Base;
            recEnc = baseEnc;
        }
        hasRecomputedVsOriginalDiff = (recEnc != pred_at_pred.predEnc);
        hasRecomputedVsActualDiff = (recEnc != realEnc);
    } else {
        hasRecomputedVsActualDiff = (pred_at_pred.predEnc != realEnc);
    }

    if (hasRecomputedVsActualDiff) {
        tageStats.recomputedVsActualDiff++;
    }
    if (hasRecomputedVsOriginalDiff) {
        tageStats.recomputedVsOriginalDiff++;
    }

    // Update basic hit/useAlt statistics on update.
    {
        TagePrediction updPred(startAddr, main_info, alt_info,
                              pred_at_pred.useAlt, pred_at_pred.source,
                              pred_at_pred.predEnc, pred_at_pred.baseEnc,
                              pred_at_pred.payloadMapped, pred_at_pred.predCondPC);
        tageStats.updateStatsWithTagePrediction(updPred, false);
    }

    // Update useAltOnNa (block-level): only when provider was weak at prediction time.
    if (pred_at_pred.mainInfo.found && isWeakConf(pred_at_pred.mainInfo.entry.conf)) {
        tageStats.updateProviderNa++;
        const uint8_t altOrBaseEnc = pred_at_pred.altInfo.found ?
            pred_at_pred.altInfo.entry.exitSlotEnc : pred_at_pred.baseEnc;
        const bool alt_correct = (altOrBaseEnc == realEnc);
        Addr uidx = getUseAltIdx(startAddr);
        updateCounter(alt_correct, useAltOnNaWidth, useAlt[uidx]);
        tageStats.updateUseAltOnNaUpdated++;
        if (alt_correct) {
            tageStats.updateUseAltOnNaCorrect++;
        } else {
            tageStats.updateUseAltOnNaWrong++;
        }
    }

    bool alloc_success = false;
    uint64_t allocated_table = 0;
    uint64_t allocated_index = 0;
    uint64_t allocated_way = 0;

    // Provider update (always update provider entry when found, like old behavior).
    if (main_info.found) {
        auto &way = tageTable[main_info.table][main_info.index][main_info.way];
        const short old_conf = way.conf;
        updateCounter(correct, 3, way.conf);

        const uint8_t altOrBaseEnc = pred_at_pred.altInfo.found ?
            pred_at_pred.altInfo.entry.exitSlotEnc : pred_at_pred.baseEnc;
        const bool provider_used = (pred_at_pred.source == PredSource::Provider);

        // Useful: provider provides gain only when provider is used and correct, and alt/base is wrong.
        if (provider_used && correct && (altOrBaseEnc != realEnc)) {
            way.useful = 1;
        }
        if (!correct && isWeakConf(way.conf)) {
            way.useful = 0;
        }

        if (correct) {
            if (isWeakConf(way.conf)) {
                tageStats.updateNoAllocWeakCorrect++;
            }
        } else {
            // Weak-and-wrong is the typical ping-pong trigger in Exit-Slot mode:
            // multiple exit patterns of the same startPC keep rewriting the same entry.
            // Prefer allocating into longer history tables to separate patterns; fall back
            // to rewrite only when allocation fails.
            const bool provider_was_weak = isWeakConf(old_conf);
            if (provider_was_weak) {
                unsigned start_table = main_info.table + 1;
                alloc_success = handleNewEntryAllocation(startAddr, realEnc, start_table,
                                                         predMeta, allocated_table,
                                                         allocated_index, allocated_way);
                if (!alloc_success) {
                    way.exitSlotEnc = realEnc;
                    way.conf = 0; // weak init
                    way.useful = 0;
                    tageStats.updateRewriteWeakWrong++;
                }
            } else if (isStrongConf(old_conf)) {
                // strong-but-wrong => allocate longer history.
                tageStats.updateAllocStrongWrong++;
                unsigned start_table = main_info.table + 1;
                alloc_success = handleNewEntryAllocation(startAddr, realEnc, start_table,
                                                         predMeta, allocated_table,
                                                         allocated_index, allocated_way);
            }
        }
    } else {
        // Provider miss: allocate only when incorrect (i.e., base can't cover this pattern).
        if (!correct) {
            tageStats.updateAllocOnMiss++;
            alloc_success = handleNewEntryAllocation(startAddr, realEnc, 0,
                                                     predMeta, allocated_table,
                                                     allocated_index, allocated_way);
        }
    }

    // If alt was actually used, train alt entry as well.
    if (pred_at_pred.source == PredSource::Alt && alt_info.found) {
        auto &way = tageTable[alt_info.table][alt_info.index][alt_info.way];
        updateCounter(correct, 3, way.conf);
    }

#ifndef UNIT_TEST
    if (enableDB) {
        TageMissTrace t;
        std::string history_str;
        boost::dynamic_bitset<> history_low50 = predMeta->history;
        if (history_low50.size() > 50) {
            history_low50.resize(50);
        }
        boost::to_string(history_low50, history_str);

        const uint64_t branchPC = stream.exeBranchInfo.isCond ? stream.exeBranchInfo.pc : 0;
        t.set(startAddr, branchPC, main_info.way,
              main_info.found, main_info.entry.conf, main_info.entry.useful,
              main_info.table, main_info.index,
              alt_info.found, alt_info.entry.conf, alt_info.entry.useful,
              alt_info.table, alt_info.index,
              pred_at_pred.useAlt, pred_at_pred.predEnc != 0, stream.exeTaken, alloc_success,
              allocated_table, allocated_index, allocated_way,
              history_str,
              main_info.found ? predMeta->indexFoldedHist[main_info.table].get() : 0);
        tageMissTrace->write_record(t);
    }
#endif

    if (getDelay() < 2) {
        checkUtageUpdateMisspred(stream);
    }

    DPRINTF(TAGE, "end update (PredEnc %u, RealEnc %u, correct %d)\n",
            pred_at_pred.predEnc, realEnc, correct);
}

void
BTBTAGE::checkUtageUpdateMisspred(const FetchTarget &stream) {
    auto predMeta = std::static_pointer_cast<TageMeta>(stream.predMetas[getComponentIdx()]);
    if (!predMeta || !predMeta->hasPred) {
        return;
    }
    // MicroTAGE mispred counting: focus on cond-exit only.
    const Addr first_taken_pc = predMeta->pred.predCondPC;
    const bool actual_cond_taken = stream.exeTaken && stream.exeBranchInfo.isCond;

    bool fallthrough_mispred = (first_taken_pc == 0 && actual_cond_taken) ||
                                (first_taken_pc != 0 && !actual_cond_taken);
    bool branch_mispred = actual_cond_taken && first_taken_pc != stream.exeBranchInfo.pc;
    if (fallthrough_mispred || branch_mispred) {
        tageStats.updateMispred++;
    }
}

// Update prediction counter with saturation
void
BTBTAGE::updateCounter(bool taken, unsigned width, short &counter) {
    int max = (1 << (width-1)) - 1;
    int min = -(1 << (width-1));
    if (taken) {
        satIncrement(max, counter);
    } else {
        satDecrement(min, counter);
    }
}

// Calculate TAGE tag with folded history - optimized version using bitwise operations
Addr
BTBTAGE::getTageTag(Addr pc, int t, uint64_t foldedHist, uint64_t altFoldedHist)
{
    // Create mask for tableTagBits[t] to limit result size
    Addr mask = (1ULL << tableTagBits[t]) - 1;

    unsigned pcShift = enableBankConflict ? indexShift : bankBaseShift;
    pcShift += tableIndexBits[t] - 1;   // since tableIndexBits = log(2048) = 11, RTL is 10
    Addr pcBits = (pc >> pcShift) & mask;

    // Extract and prepare folded history bits
    Addr foldedBits = foldedHist & mask;

    // Extract alt tag bits and shift left by 1
    Addr altTagBits = (altFoldedHist << 1) & mask;

    // XOR all components together (Exit-Slot mode does not include position).
    return pcBits ^ foldedBits ^ altTagBits;
}

Addr
BTBTAGE::getTageTag(Addr pc, int t)
{
    return getTageTag(pc, t, tagFoldedHist[t].get(), altTagFoldedHist[t].get());
}

Addr
BTBTAGE::getTageIndex(Addr pc, int t, uint64_t foldedHist)
{
    // Create mask for tableIndexBits[t] to limit result size
    Addr mask = (1ULL << tableIndexBits[t]) - 1;

    const unsigned pcShift = enableBankConflict ? indexShift : bankBaseShift;
    Addr pcBits = (pc >> pcShift) & mask;
    Addr foldedBits = foldedHist & mask;

    return pcBits ^ foldedBits;
}

Addr
BTBTAGE::getTageIndex(Addr pc, int t)
{
    return getTageIndex(pc, t, indexFoldedHist[t].get());
}

bool
BTBTAGE::matchTag(Addr expected, Addr found)
{
    return expected == found;
}

bool
BTBTAGE::satIncrement(int max, short &counter)
{
    if (counter < max) {
        ++counter;
    }
    return counter == max;
}

bool
BTBTAGE::satDecrement(int min, short &counter)
{
    if (counter > min) {
        --counter;
    }
    return counter == min;
}

Addr
BTBTAGE::getUseAltIdx(Addr pc) {
    Addr shiftedPc = pc >> instShiftAmt;
    return shiftedPc & (useAltOnNaSize - 1);
}

unsigned
BTBTAGE::getBranchIndexInBlock(Addr branchPC, Addr startPC) const {
    // Calculate branch position within the fetch block (0 .. maxBranchPositions-1)
    Addr alignedPC = startPC & ~(blockSize - 1);
    Addr offset = (branchPC - alignedPC) >> instShiftAmt;
    assert(offset < maxBranchPositions);
    return offset;
}

unsigned
BTBTAGE::getBankId(Addr pc) const
{
    // Extract bank ID bits after removing instruction alignment
    return (pc >> bankBaseShift) & ((1 << bankIdWidth) - 1);
}

/**
 * @brief Updates branch history for speculative execution
 * 
 * This function updates three types of folded histories:
 * - Tag folded history: Used for tag computation
 * - Alternative tag folded history: Used for alternative tag computation
 * - Index folded history: Used for table index computation
 * 
 * @param history The current branch history
 * @param shamt The number of bits to shift
 * @param taken Whether the branch was taken
 */
void
BTBTAGE::doUpdateHist(const boost::dynamic_bitset<> &history, bool taken, Addr pc, Addr target)
{
    if (debug::TAGEHistory) {   // if debug flag is off, do not use to_string since it's too slow
        std::string buf;
        boost::to_string(history, buf);
        DPRINTF(TAGEHistory, "in doUpdateHist, taken %d, pc %#lx, history %s\n", taken, pc, buf.c_str());
    }
    if (!taken) {
        DPRINTF(TAGEHistory, "not updating folded history, since FB not taken\n");
        return;
    }

    for (int t = 0; t < numPredictors; t++) {
        for (int type = 0; type < 3; type++) {
            auto &foldedHist = type == 0 ? indexFoldedHist[t] : type == 1 ? tagFoldedHist[t] : altTagFoldedHist[t];
            // since we have folded path history, we can put arbitrary shamt here, and it wouldn't make a difference
            foldedHist.update(history, 2, taken, pc, target);
            DPRINTF(TAGEHistory, "t: %d, type: %d, foldedHist _folded 0x%lx\n", t, type, foldedHist.get());
        }
    }
}

/**
 * @brief Updates branch history for speculative execution
 * 
 * This function updates the branch history for speculative execution
 * based on the provided history and prediction information.
 * 
 * It first retrieves the history information from the prediction metadata
 * and then calls the doUpdateHist function to update the folded histories.
 * 
 * @param history The current branch history
 * @param pred The prediction metadata containing history information
 */
void
BTBTAGE::specUpdatePHist(const boost::dynamic_bitset<> &history, FullBTBPrediction &pred)
{
    auto [pc, target, taken] = pred.getPHistInfo();
    doUpdateHist(history, taken, pc, target);
}

/**
 * @brief Recovers branch history state after a misprediction
 * 
 * This function:
 * 1. Restores the folded histories from the saved metadata
 * 2. Updates the histories with the correct branch outcome
 * 3. Ensures predictor state is consistent after recovery
 * 
 * @param history The branch history to recover to
 * @param entry The fetch stream entry containing recovery information
 * @param shamt Number of bits to shift in history update
 * @param cond_taken The actual branch outcome
 */
void
BTBTAGE::recoverPHist(const boost::dynamic_bitset<> &history,
    const FetchTarget &entry, int shamt, bool cond_taken)
{
    std::shared_ptr<TageMeta> predMeta = std::static_pointer_cast<TageMeta>(entry.predMetas[getComponentIdx()]);
    for (int i = 0; i < numPredictors; i++) {
        tagFoldedHist[i].recover(predMeta->tagFoldedHist[i]);
        altTagFoldedHist[i].recover(predMeta->altTagFoldedHist[i]);
        indexFoldedHist[i].recover(predMeta->indexFoldedHist[i]);
    }
    doUpdateHist(history, cond_taken, entry.getControlPC(), entry.getTakenTarget());
}

// Check folded history after speculative update and recovery
void
BTBTAGE::checkFoldedHist(const boost::dynamic_bitset<> &hist, const char * when)
{
    DPRINTF(TAGE, "checking folded history when %s\n", when);
    if (debug::TAGEHistory) {
        std::string hist_str;
        boost::to_string(hist, hist_str);
        DPRINTF(TAGEHistory, "history:\t%s\n", hist_str.c_str());
    }
    for (int t = 0; t < numPredictors; t++) {
        for (int type = 0; type < 3; type++) {
            std::string buf2, buf3;
            auto &foldedHist = type == 0 ? indexFoldedHist[t] : type == 1 ? tagFoldedHist[t] : altTagFoldedHist[t];
            foldedHist.check(hist);
        }
    }
}

#ifndef UNIT_TEST
// Constructor for TAGE statistics
BTBTAGE::TageStats::TageStats(statistics::Group* parent, int numPredictors, int numBanks):
    statistics::Group(parent),
    ADD_STAT(predNoHitUseBim, statistics::units::Count::get(), "use bimodal when no hit on prediction"),
    ADD_STAT(predUseAlt, statistics::units::Count::get(), "use alt on prediction"),
    ADD_STAT(updateNoHitUseBim, statistics::units::Count::get(), "use bimodal when no hit on update"),
    ADD_STAT(updateUseAlt, statistics::units::Count::get(), "use alt on update"),
    ADD_STAT(updateUseAltCorrect, statistics::units::Count::get(), "use alt on update and correct"),
    ADD_STAT(updateUseAltWrong, statistics::units::Count::get(), "use alt on update and wrong"),
    ADD_STAT(updateAltDiffers, statistics::units::Count::get(), "alt differs on update"),
    ADD_STAT(updateUseAltOnNaUpdated, statistics::units::Count::get(), "use alt on na ctr updated when update"),
    ADD_STAT(updateProviderNa, statistics::units::Count::get(), "provider weak when update"),
    ADD_STAT(updateUseNaCorrect, statistics::units::Count::get(), "use na on update and correct"),
    ADD_STAT(updateUseNaWrong, statistics::units::Count::get(), "use na on update and wrong"),
    ADD_STAT(updateUseAltOnNaCorrect, statistics::units::Count::get(), "use alt on na correct when update"),
    ADD_STAT(updateUseAltOnNaWrong, statistics::units::Count::get(), "use alt on na wrong when update"),
    ADD_STAT(updateAllocFailure, statistics::units::Count::get(), "alloc failure when update"),
    ADD_STAT(updateAllocFailureNoValidTable, statistics::units::Count::get(), "alloc failure no valid table when update"),
    ADD_STAT(updateAllocSuccess, statistics::units::Count::get(), "alloc success when update"),
    ADD_STAT(updateMispred, statistics::units::Count::get(), "mispred when update"),
    ADD_STAT(updateResetU, statistics::units::Count::get(), "reset u when update"),
    ADD_STAT(predNoCondExit, statistics::units::Count::get(), "predicted No-Cond-Exit (ExitSlotEnc==0) blocks"),
    ADD_STAT(predBaseFallback, statistics::units::Count::get(), "blocks that fall back to base (provider miss/weak/ mapfail)"),
    ADD_STAT(predPayloadMapFail, statistics::units::Count::get(), "non-base payload that cannot be mapped to a cond entry in btbEntries"),
    ADD_STAT(updateAllocOnMiss, statistics::units::Count::get(), "allocate on provider miss when base is wrong"),
    ADD_STAT(updateAllocStrongWrong, statistics::units::Count::get(), "allocate on strong-but-wrong provider"),
    ADD_STAT(updateRewriteWeakWrong, statistics::units::Count::get(), "rewrite payload on weak-and-wrong provider"),
    ADD_STAT(updateNoAllocWeakCorrect, statistics::units::Count::get(), "no-alloc on weak-but-correct provider"),
    ADD_STAT(recomputedVsActualDiff, statistics::units::Count::get(), "fetchBlocks where recomputed.taken != actual_taken"),
    ADD_STAT(recomputedVsOriginalDiff, statistics::units::Count::get(), "fetchBlocks where recomputed.taken != original pred.taken"),
    ADD_STAT(updateBankConflict, statistics::units::Count::get(), "number of bank conflicts detected"),
    ADD_STAT(updateDeferredDueToConflict, statistics::units::Count::get(), "number of updates deferred due to bank conflict (retried later)"),
    ADD_STAT(updateBankConflictPerBank, statistics::units::Count::get(), "bank conflicts per bank"),
    ADD_STAT(updateAccessPerBank, statistics::units::Count::get(), "update accesses per bank"),
    ADD_STAT(predAccessPerBank, statistics::units::Count::get(), "prediction accesses per bank"),
    ADD_STAT(predTableHits, statistics::units::Count::get(), "hit of each tage table on prediction"),
    ADD_STAT(updateTableHits, statistics::units::Count::get(), "hit of each tage table on update"),
    ADD_STAT(updateTableMispreds, statistics::units::Count::get(), "mispreds of each table when update"),

    ADD_STAT(condPredwrong, statistics::units::Count::get(), "number of conditional branch mispredictions committed"),
    ADD_STAT(condMissTakens, statistics::units::Count::get(), "number of conditional branch mispredictions committed with no prediction"),
    ADD_STAT(condCorrect, statistics::units::Count::get(), "number of conditional branch correct predictions committed"),
    ADD_STAT(condMissNoTakens, statistics::units::Count::get(), "number of conditional branch correct predictions committed with no prediction"),
    ADD_STAT(predHit, statistics::units::Count::get(), "number of conditional branch predictions that hit"),
    ADD_STAT(predMiss, statistics::units::Count::get(), "number of conditional branch predictions that miss")
{
    predTableHits.init(0, numPredictors-1, 1);
    updateTableHits.init(0, numPredictors-1, 1);
    updateTableMispreds.init(numPredictors);

    // Initialize per-bank statistics vectors
    updateBankConflictPerBank.init(numBanks);
    updateAccessPerBank.init(numBanks);
    predAccessPerBank.init(numBanks);
}
#endif

// Update statistics based on TAGE prediction
void
BTBTAGE::TageStats::updateStatsWithTagePrediction(const TagePrediction &pred, bool when_pred)
{
    bool hit = pred.mainInfo.found;
    unsigned hit_table = pred.mainInfo.table;
    bool useAlt = pred.useAlt;
    if (when_pred) {
        if (hit) {
#ifndef UNIT_TEST
            predTableHits.sample(hit_table, 1);
#endif
        } else {
            predNoHitUseBim++;
        }
        if (!hit || useAlt) {
            predUseAlt++;
        }
    } else {
        if (hit) {
#ifndef UNIT_TEST
            updateTableHits.sample(hit_table, 1);
#endif
        } else {
            updateNoHitUseBim++;
        }
        if (!hit || useAlt) {
            updateUseAlt++;
        }
    }
}

// Update LRU counters for a set
void
BTBTAGE::updateLRU(int table, Addr index, unsigned way)
{
    // Increment LRU counters for all entries in the set
    for (unsigned i = 0; i < numWays; i++) {
        if (i != way && tageTable[table][index][i].valid) {
            tageTable[table][index][i].lruCounter++;
        }
    }
    // Reset LRU counter for the accessed entry
    tageTable[table][index][way].lruCounter = 0;
}

// Find the LRU victim in a set
unsigned
BTBTAGE::getLRUVictim(int table, Addr index)
{
    unsigned victim = 0;
    unsigned maxLRU = 0;

    // Find the entry with the highest LRU counter
    for (unsigned i = 0; i < numWays; i++) {
        if (!tageTable[table][index][i].valid) {
            return i; // Use invalid entry if available
        }
        if (tageTable[table][index][i].lruCounter > maxLRU) {
            maxLRU = tageTable[table][index][i].lruCounter;
            victim = i;
        }
    }
    return victim;
}

#ifndef UNIT_TEST

void
BTBTAGE::commitBranch(const FetchTarget &stream, const DynInstPtr &inst)
{
    if (!inst->isCondCtrl()) {
        // tage olnly deals with conditional branches
        return;
    }
    auto meta = std::static_pointer_cast<TageMeta>(stream.predMetas[getComponentIdx()]);
    const Addr pc = inst->pcState().instAddr();

    // pred_hit: the branch must be present in the BTB entries of this stream.
    const BTBEntry *btb_entry = nullptr;
    for (auto &e : stream.predBTBEntries) {
        if (e.valid && e.isCond && e.pc == pc) {
            btb_entry = &e;
            break;
        }
    }
    const bool pred_hit = (btb_entry != nullptr) && meta && meta->hasPred;

    bool pred_taken = false;
    if (pred_hit) {
        if (meta->pred.source == PredSource::Base) {
            pred_taken = (btb_entry->ctr >= 0);
        } else {
            pred_taken = (meta->pred.predCondPC == pc);
        }
    }

    const bool this_cond_taken = stream.exeTaken && stream.exeBranchInfo.isCond &&
                                 stream.exeBranchInfo.pc == pc;
    bool predcorrect = (pred_taken == this_cond_taken);
    if (!predcorrect) {
        tageStats.condPredwrong++;
        if (!pred_hit) {
            tageStats.condMissTakens++;
        }
    }else{
        tageStats.condCorrect++;
        if (!pred_hit) {
            tageStats.condMissNoTakens++;
        }
    }

    if (pred_hit) {
        tageStats.predHit++;
    } else {
        tageStats.predMiss++;
    }
}
#endif

#ifdef UNIT_TEST
} // namespace test
#endif

} // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5
