/*
 * VTAGE is largely based on the open-source implementation of the
 * 1st-place Championship Value Prediction (CVP-1) submission.
 *
 * The version in this codebase is adapted and modified for our environment.
 * It is not intended to be a bit-exact copy of the original code.
 *
 * For detailed background and reference material:
 * Paper: https://microarch.org/cvp1/papers/Seznec.pdf
 * Open-source implementation: https://www.microarch.org/cvp1/code/Seznec.tar.gz
 * Official website: https://www.microarch.org/cvp1/
 */

#include "cpu/valuepred/vtage.hh"

#include <algorithm>
#include <limits>

#include "base/logging.hh"
#include "cpu/pred/btb/common.hh"
#include "cpu/valuepred/vtage_metadata.hh"
#include "debug/VTAGE.hh"

namespace gem5
{

namespace valuepred
{

using branch_prediction::btb_pred::FetchTarget;

VTAGE::VTAGE(const Params &params)
    : VPUnit(params),
      numHistories(params.numHistories),
      numBanks(params.numBanks),
      histLengths(params.histLengths.begin(), params.histLengths.end()),
      requireHistoryExt(params.requireHistoryExt),
      logBankSize(params.logBankSize),
      bankSize(1u << params.logBankSize),
      tagBits(params.tagBits),
      confBits(params.confBits),
      usefulBits(params.usefulBits),
      logValueArrayEntries(params.logValueArrayEntries),
      valueEntriesPerWay(1u << params.logValueArrayEntries),
      totalValueEntries(ValueWays * (1u << params.logValueArrayEntries)),
      predictConfThreshold(params.predictConfThreshold),
      hashOnlyUpgradeThreshold(params.hashOnlyUpgradeThreshold),
      mispredBackoffDistance(params.mispredBackoffDistance),
      agingTickMax(params.agingTickMax),
      agingPenaltyOnAlloc(params.agingPenaltyOnAlloc),
      agingPenaltyOnNoAlloc(params.agingPenaltyOnNoAlloc),
      l1HitMaxCycles(params.l1HitMaxCycles),
      l2HitMaxCycles(params.l2HitMaxCycles),
      llcHitMaxCycles(params.llcHitMaxCycles),
      fastInstCycles(params.fastInstCycles),
      mfastInstCycles(params.mfastInstCycles),
      enableStochasticTraining(params.enableStochasticTraining),
      rngSeed(params.rngSeed),
      allocProbLoadL1Hit(params.allocProbLoadL1Hit),
      allocProbLoadMiss(params.allocProbLoadMiss),
      confIncProbLowValue(params.confIncProbLowValue),
      confIncProbFastLoad(params.confIncProbFastLoad),
      uIncProbFastLoad(params.uIncProbFastLoad),
      valueArrayUpgradeProb(params.valueArrayUpgradeProb),
      shortHistoryAllocBias(params.shortHistoryAllocBias),
      deepAllocExtraHopProb(params.deepAllocExtraHopProb),
      rng(),
      tables(numThreads),
      valueArrays(numThreads),
      agingTicks(numThreads, 0),
      lastSelectedMispredictSeq(numThreads, 0),
      hasSelectedMispredictSeq(numThreads, false),
      vtageStats(this)
{
    gem5_assert(numBanks > 0, "VTAGE needs at least one bank");
    gem5_assert(numBanks == numHistories + 1,
            "VTAGE expects numBanks == numHistories + 1");
    gem5_assert(histLengths.size() == numBanks,
            "VTAGE histLengths must cover every bank, including the base bank");
    gem5_assert(tagBits > 0, "VTAGE tagBits must be non-zero");
    gem5_assert(confBits > 0, "VTAGE confBits must be non-zero");
    gem5_assert(usefulBits > 0, "VTAGE usefulBits must be non-zero");
    gem5_assert(logBankSize > 0, "VTAGE logBankSize must be non-zero");
    gem5_assert(logValueArrayEntries > 0,
            "VTAGE logValueArrayEntries must be non-zero");
    gem5_assert(predictConfThreshold <= maxConf(),
            "VTAGE predictConfThreshold exceeds the confidence range");
    gem5_assert(hashOnlyUpgradeThreshold <= maxConf(),
            "VTAGE hashOnlyUpgradeThreshold exceeds the confidence range");
    gem5_assert(fastInstCycles <= mfastInstCycles,
            "VTAGE fastInstCycles must not exceed mfastInstCycles");
    gem5_assert(l1HitMaxCycles <= l2HitMaxCycles,
            "VTAGE l1HitMaxCycles must not exceed l2HitMaxCycles");
    gem5_assert(l2HitMaxCycles <= llcHitMaxCycles,
            "VTAGE l2HitMaxCycles must not exceed llcHitMaxCycles");

    rng.init(rngSeed);

    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        tables[tid].resize(numBanks);
        for (auto &bank : tables[tid]) {
            bank.resize(bankSize);
        }
        valueArrays[tid].resize(totalValueEntries);
    }

    vtageStats.predictHitBank.init(numBanks).flags(statistics::nozero);
    for (unsigned bank = 0; bank < numBanks; ++bank) {
        vtageStats.predictHitBank.subname(bank,
                "bank" + std::to_string(bank) + "_hist" +
                std::to_string(histLengths[bank]));
    }
}

uint32_t
VTAGE::bitMask(unsigned bits) const
{
    if (bits >= 32) {
        return std::numeric_limits<uint32_t>::max();
    }
    return (1u << bits) - 1;
}

uint32_t
VTAGE::maxConf() const
{
    return bitMask(confBits);
}

uint32_t
VTAGE::maxUseful() const
{
    return bitMask(usefulBits);
}

uint64_t
VTAGE::mix64(uint64_t value) const
{
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return value;
}

bool
VTAGE::chance(double probability)
{
    if (!enableStochasticTraining) {
        return true;
    }

    if (probability <= 0.0) {
        return false;
    }
    if (probability >= 1.0) {
        return true;
    }
    return rng.random<double>() < probability;
}

bool
VTAGE::isFastLoad(const TrainInfo &train_info) const
{
    return train_info.latencyValid &&
        train_info.observedLatencyCycles <= fastInstCycles;
}

bool
VTAGE::isMFastLoad(const TrainInfo &train_info) const
{
    return train_info.latencyValid &&
        train_info.observedLatencyCycles <= mfastInstCycles;
}

bool
VTAGE::isLowValue(RegVal value) const
{
    const auto upper = static_cast<uint64_t>(value) >> 32;
    return upper == 0 || upper == std::numeric_limits<uint32_t>::max();
}

uint32_t
VTAGE::foldHistory(const boost::dynamic_bitset<> &history, unsigned length,
        unsigned out_bits, uint64_t salt) const
{
    const auto limit = std::min<size_t>(length, history.size());
    uint64_t accum = mix64(salt ^ limit);

    for (size_t bit = 0; bit < limit; ++bit) {
        if (!history.test(bit)) {
            continue;
        }
        accum ^= mix64((static_cast<uint64_t>(bit) + 1) *
                0x9e3779b97f4a7c15ULL ^ salt);
        accum = (accum << 7) | (accum >> (64 - 7));
    }

    return static_cast<uint32_t>(accum) & bitMask(out_bits);
}

void
VTAGE::fillIndicesAndTags(const FetchTarget &fetch_target, Addr pc,
        std::vector<uint32_t> &indices, std::vector<uint32_t> &tags) const
{
    indices.resize(numBanks);
    tags.resize(numBanks);

    for (unsigned bank = 0; bank < numBanks; ++bank) {
        const unsigned hist_len = histLengths[bank];
        const auto history_fold = foldHistory(fetch_target.history, hist_len,
                logBankSize, 0x1000ULL + bank * 0x51ULL);
        const auto path_fold = foldHistory(fetch_target.phistory,
                std::min<unsigned>(hist_len,
                    static_cast<unsigned>(fetch_target.phistory.size())),
                logBankSize, 0x2000ULL + bank * 0x63ULL);
        const auto tag_history_fold = foldHistory(fetch_target.history,
                hist_len, tagBits, 0x3000ULL + bank * 0x77ULL);
        const auto tag_path_fold = foldHistory(fetch_target.phistory,
                std::min<unsigned>(hist_len,
                    static_cast<unsigned>(fetch_target.phistory.size())),
                tagBits, 0x4000ULL + bank * 0x8bULL);

        const uint64_t index_mix = mix64(pc ^ history_fold ^
                (static_cast<uint64_t>(path_fold) << 11) ^
                (static_cast<uint64_t>(fetch_target.asidHash) << 3) ^
                (bank * 0x9e3779b9ULL));
        const uint64_t tag_mix = mix64((pc >> 2) ^
                (static_cast<uint64_t>(tag_history_fold) << 7) ^
                (static_cast<uint64_t>(tag_path_fold) << 17) ^
                (static_cast<uint64_t>(fetch_target.asidHash) << 1) ^
                (bank * 0xc2b2ae35ULL));

        indices[bank] = static_cast<uint32_t>(index_mix) & bitMask(logBankSize);
        tags[bank] = static_cast<uint32_t>(tag_mix) & bitMask(tagBits);
    }
}

uint32_t
VTAGE::hashActualValue(RegVal value) const
{
    return static_cast<uint32_t>(mix64(static_cast<uint64_t>(value))) &
        bitMask(logValueArrayEntries);
}

uint32_t
VTAGE::makeHashToken(RegVal value) const
{
    return totalValueEntries + hashActualValue(value);
}

bool
VTAGE::entryMatchesActualValue(ThreadID tid, const VTAGEEntry &entry,
        RegVal actual_value) const
{
    if (!entry.valid) {
        return false;
    }

    if (entry.hashOrValue < totalValueEntries) {
        const auto &value_entry = valueArrays[tid][entry.hashOrValue];
        return value_entry.valid && value_entry.data == actual_value;
    }

    return entry.hashOrValue == makeHashToken(actual_value);
}

std::array<uint32_t, VTAGE::ValueWays>
VTAGE::valueArrayIndices(RegVal actual_value) const
{
    std::array<uint32_t, ValueWays> indices = {};
    for (unsigned way = 0; way < ValueWays; ++way) {
        const uint64_t mixed = mix64(static_cast<uint64_t>(actual_value) ^
                (0x9e3779b97f4a7c15ULL * (way + 1)) ^
                (static_cast<uint64_t>(actual_value) >> (7 * (way + 1))));
        indices[way] = (static_cast<uint32_t>(mixed) &
                bitMask(logValueArrayEntries)) +
            way * valueEntriesPerWay;
    }
    return indices;
}

VTAGE::TrainInfo
VTAGE::decodeTrainInfo(const VPUpdateInfo &update_info) const
{
    TrainInfo train_info;
    if (const auto *ext = update_info.getExt<LoadTrainInfoExt>()) {
        train_info.cacheHit = ext->cacheHit;
        train_info.observedLatencyCycles = ext->observedLatencyCycles;
        train_info.latencyValid = ext->latencyValid;
        train_info.numSrcRegs = ext->numSrcRegs;
        train_info.opClass = ext->opClass;
        train_info.criticalLoad = ext->criticalLoad;
    }
    return train_info;
}

bool
VTAGE::shouldIncreaseConfidence(const TrainInfo &train_info, RegVal actual_value)
{
    double probability = 1.0;
    if (enableStochasticTraining) {
        probability = 0.5;
        if (isLowValue(actual_value)) {
            probability = std::max(probability, confIncProbLowValue);
        }
        if (isFastLoad(train_info)) {
            probability = std::max(probability, confIncProbFastLoad);
        }
        if (train_info.criticalLoad) {
            probability = 1.0;
        }
    }
    return chance(probability);
}

bool
VTAGE::shouldIncreaseUseful(const TrainInfo &train_info)
{
    double probability = 1.0;
    if (enableStochasticTraining) {
        probability = isMFastLoad(train_info) ? uIncProbFastLoad : 0.5;
        if (train_info.criticalLoad) {
            probability = 1.0;
        }
    }
    return chance(probability);
}

bool
VTAGE::shouldAllocateEntry(const TrainInfo &train_info)
{
    const bool l1_like = train_info.cacheHit ||
        (train_info.latencyValid &&
         train_info.observedLatencyCycles <= l1HitMaxCycles);
    return chance(l1_like ? allocProbLoadL1Hit : allocProbLoadMiss);
}

bool
VTAGE::shouldUpgradeValueArray(const TrainInfo &train_info)
{
    double probability = valueArrayUpgradeProb;
    if (train_info.criticalLoad) {
        probability = 1.0;
    }
    return chance(probability);
}

unsigned
VTAGE::chooseAllocationStartBank(int hit_bank)
{
    unsigned start_bank = 1;
    if (hit_bank >= 0) {
        start_bank = std::min<unsigned>(static_cast<unsigned>(hit_bank + 1),
                numBanks - 1);
        if (hit_bank == 0 && start_bank + 1 < numBanks) {
            ++start_bank;
        }
    }

    if (enableStochasticTraining && start_bank > 1 &&
            chance(shortHistoryAllocBias)) {
        start_bank = 1;
    }
    if (enableStochasticTraining && start_bank + 1 < numBanks &&
            chance(deepAllocExtraHopProb)) {
        ++start_bank;
    }

    return std::min(start_bank, numBanks - 1);
}

bool
VTAGE::tryUpgradeToPointer(ThreadID tid, VTAGEEntry &entry, RegVal actual_value,
        const TrainInfo &train_info)
{
    if (!shouldUpgradeValueArray(train_info)) {
        return false;
    }

    const auto value_indices = valueArrayIndices(actual_value);
    auto &thread_values = valueArrays[tid];
    for (const auto index : value_indices) {
        auto &value_entry = thread_values[index];
        if (value_entry.valid && value_entry.data == actual_value) {
            entry.hashOrValue = index;
            value_entry.useful = std::min(maxUseful(), value_entry.useful + 1);
            vtageStats.valueArrayHit++;
            vtageStats.upgradeToPointer++;
            return true;
        }
    }

    unsigned begin = rng.random<unsigned>(0, ValueWays - 1);
    for (unsigned offset = 0; offset < ValueWays; ++offset) {
        const auto index = value_indices[(begin + offset) % ValueWays];
        auto &value_entry = thread_values[index];
        if (!value_entry.valid || value_entry.useful == 0) {
            value_entry.data = actual_value;
            value_entry.useful = 1;
            value_entry.valid = true;
            entry.hashOrValue = index;
            vtageStats.valueArraySteal++;
            vtageStats.upgradeToPointer++;
            return true;
        }
    }

    if (enableStochasticTraining) {
        auto &value_entry = thread_values[value_indices[begin]];
        if (value_entry.useful > 0) {
            --value_entry.useful;
        }
    }
    return false;
}

void
VTAGE::ageEntries(ThreadID tid)
{
    for (auto &bank : tables[tid]) {
        for (auto &entry : bank) {
            if (entry.useful > 0) {
                --entry.useful;
            }
        }
    }
    for (auto &value_entry : valueArrays[tid]) {
        if (value_entry.useful > 0) {
            --value_entry.useful;
        }
    }
    vtageStats.agingPasses++;
}

void
VTAGE::advanceAging(ThreadID tid, unsigned delta)
{
    if (agingTickMax == 0) {
        return;
    }

    agingTicks[tid] += delta;
    while (agingTicks[tid] >= agingTickMax) {
        agingTicks[tid] -= agingTickMax;
        ageEntries(tid);
    }
}

bool
VTAGE::backoffActive(ThreadID tid, uint64_t seq_no) const
{
    if (!hasSelectedMispredictSeq[tid]) {
        return false;
    }
    return seq_no - lastSelectedMispredictSeq[tid] < mispredBackoffDistance;
}

VPPredictionCandidate
VTAGE::predict(const VPPredictRequest &request)
{
    assertValidTid(request.tid);

    VPPredictionCandidate candidate;
    auto record = std::make_unique<VTAGEPredictionRecord>();
    record->indices.resize(numBanks);
    record->tags.resize(numBanks);

    const auto *history_ext = request.getExt<VPHistoryRequestExt>();
    if (!history_ext || !history_ext->fetchTarget) {
        vtageStats.missingHistoryReq++;
        if (requireHistoryExt) {
            fatal("VTAGE requires VPHistoryRequestExt when requireHistoryExt is true");
        }
        candidate.record = std::move(record);
        return candidate;
    }

    record->historyAvailable = true;
    fillIndicesAndTags(*history_ext->fetchTarget, request.pc, record->indices,
            record->tags);

    auto &thread_tables = tables[request.tid];
    for (int bank = static_cast<int>(numBanks) - 1; bank >= 0; --bank) {
        const auto &entry = thread_tables[bank][record->indices[bank]];
        if (entry.valid && entry.tag == record->tags[bank]) {
            record->hitBank = bank;
            break;
        }
    }

    if (record->hitBank >= 0) {
        const auto &entry = thread_tables[record->hitBank]
            [record->indices[record->hitBank]];
        vtageStats.predictHitBank[record->hitBank]++;
        if (entry.hashOrValue < totalValueEntries) {
            record->pointerHit = true;
            vtageStats.predictPointerHit++;
        } else {
            record->hashOnlyHit = true;
            vtageStats.predictHashOnlyHit++;
        }

        record->backoffBlocked = backoffActive(request.tid, request.seqNo);
        if (record->pointerHit &&
                valueArrays[request.tid][entry.hashOrValue].valid &&
                entry.conf >= predictConfThreshold &&
                !record->backoffBlocked) {
            candidate.result.speculative = true;
            candidate.result.value =
                valueArrays[request.tid][entry.hashOrValue].data;
            candidate.score = static_cast<uint64_t>(entry.conf) * numBanks +
                static_cast<uint64_t>(record->hitBank);
            record->offeredPrediction = true;
            record->predictedValue = candidate.result.value;
        } else if (record->pointerHit && entry.conf >= predictConfThreshold &&
                record->backoffBlocked) {
            vtageStats.predictBackoffBlocked++;
        }
    }

    candidate.record = std::move(record);
    return candidate;
}

void
VTAGE::update(const VPUpdateInfo &update_info, const VPPredictionRecord *record,
        const VPFeedback &feedback)
{
    assertValidTid(update_info.tid);

    const auto *vtage_record =
        dynamic_cast<const VTAGEPredictionRecord *>(record);
    gem5_assert(vtage_record,
            "VTAGE expects VTAGEPredictionRecord on update");
    if (!vtage_record->historyAvailable) {
        return;
    }

    const auto train_info = decodeTrainInfo(update_info);
    const auto actual_hash = makeHashToken(update_info.actualValue);
    bool allocated = false;
    bool should_allocate = !feedback.overallPredictionCorrect;

    if (vtage_record->hitBank >= 0) {
        const auto bank = static_cast<unsigned>(vtage_record->hitBank);
        auto &entry = tables[update_info.tid][bank][vtage_record->indices[bank]];
        if (entry.valid && entry.tag == vtage_record->tags[bank]) {
            const bool satisfied = entryMatchesActualValue(update_info.tid,
                    entry, update_info.actualValue);
            if (satisfied) {
                vtageStats.commitSatisfiedHit++;
                should_allocate = false;

                if (entry.conf < maxConf() &&
                        shouldIncreaseConfidence(train_info,
                            update_info.actualValue)) {
                    ++entry.conf;
                }
                if (entry.useful < maxUseful() &&
                        shouldIncreaseUseful(train_info)) {
                    ++entry.useful;
                }
                if (entry.hashOrValue < totalValueEntries) {
                    auto &value_entry =
                        valueArrays[update_info.tid][entry.hashOrValue];
                    if (value_entry.valid && entry.conf >= predictConfThreshold &&
                            value_entry.useful < maxUseful()) {
                        ++value_entry.useful;
                    }
                } else if (entry.conf >= hashOnlyUpgradeThreshold) {
                    tryUpgradeToPointer(update_info.tid, entry,
                            update_info.actualValue, train_info);
                }
            } else {
                vtageStats.commitMismatchedHit++;
                if (feedback.applied && !feedback.overallPredictionCorrect) {
                    lastSelectedMispredictSeq[update_info.tid] =
                        update_info.seqNo;
                    hasSelectedMispredictSeq[update_info.tid] = true;
                }

                entry.hashOrValue = actual_hash;
                if (entry.conf > maxConf() / 2) {
                    entry.conf = std::max<uint32_t>(1, entry.conf / 2);
                } else {
                    entry.conf = 0;
                }
                if (entry.useful > 0) {
                    --entry.useful;
                }
            }
        } else {
            vtageStats.commitMismatchedHit++;
        }
    }

    if (should_allocate && numBanks > 1 && shouldAllocateEntry(train_info)) {
        const auto start_bank = chooseAllocationStartBank(vtage_record->hitBank);
        for (unsigned bank = start_bank; bank < numBanks; ++bank) {
            auto &entry =
                tables[update_info.tid][bank][vtage_record->indices[bank]];
            if (!entry.valid || entry.useful == 0 || entry.conf <= 1) {
                entry.valid = true;
                entry.hashOrValue = actual_hash;
                entry.conf = std::max<uint32_t>(1, maxConf() / 2);
                entry.tag = vtage_record->tags[bank];
                entry.useful = 0;
                allocated = true;
                vtageStats.allocHashOnly++;
                if (vtage_record->hitBank < 0 ||
                        bank > static_cast<unsigned>(vtage_record->hitBank)) {
                    vtageStats.allocLongHistory++;
                }
                break;
            }
            if (entry.useful > 0) {
                --entry.useful;
            }
        }
    }

    advanceAging(update_info.tid,
            allocated ? agingPenaltyOnAlloc : agingPenaltyOnNoAlloc);
}

void
VTAGE::specUpdate(const VPSpecUpdateInfo &spec_update_info)
{
    (void)spec_update_info;
}

void
VTAGE::squash(ThreadID tid, const uint64_t seq_no)
{
    (void)tid;
    (void)seq_no;
}

} // namespace valuepred

} // namespace gem5
