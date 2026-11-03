#ifndef __CPU_PRED_BTB_TEST_PREDICTION_HELPERS_HH__
#define __CPU_PRED_BTB_TEST_PREDICTION_HELPERS_HH__

#include <memory>

#include "cpu/pred/btb/abtb.hh"
#include "cpu/pred/btb/btb_mgsc.hh"
#include "cpu/pred/btb/btb_tage.hh"
#include "cpu/pred/btb/mbtb.hh"
#include "cpu/pred/btb/microtage.hh"
#include "cpu/pred/btb/ras.hh"

namespace gem5
{
namespace branch_prediction
{
namespace btb_pred
{
namespace test
{

// Component tests supply the stage composition normally owned by DecoupledBPU.
// These helpers are not part of the production component interface.
template<class Predictor>
void
fillCandidatesForTest(Predictor *predictor,
                      const std::vector<BranchPrediction> &candidates,
                      std::vector<FullBTBPrediction> &stages)
{
    for (unsigned s = predictor->getDelay(); s < stages.size(); ++s) {
        stages[s].btbEntries = candidates;
        stages[s].hit = !candidates.empty();
        stages[s].predTick = predictor->curTick();
    }
}

inline void
predictForTest(MBTB *predictor, Addr pc,
               const boost::dynamic_bitset<> &history,
               std::vector<FullBTBPrediction> &stages)
{
    const auto &first = stages.front();
    fillCandidatesForTest(predictor,
        predictor->predictCandidates(pc, first.tid, first.asidHash), stages);
}

inline void
predictForTest(AheadBTB *predictor, Addr pc,
               const boost::dynamic_bitset<> &history,
               std::vector<FullBTBPrediction> &stages)
{
    const auto &first = stages.front();
    auto candidates = predictor->predictCandidates(
        pc, history, first.tid, first.asidHash);
    const bool nativeHit = !candidates.empty();
    const bool ubtbHit = !first.btbEntries.empty();
    predictor->recordPredictionSelection(nativeHit, ubtbHit);
    if (!nativeHit && ubtbHit) {
        candidates.push_back(first.btbEntries.front());
    }
    fillCandidatesForTest(predictor, candidates, stages);
}

inline void
predictForTest(BTBTAGE *predictor, Addr pc,
               const boost::dynamic_bitset<> &history,
               std::vector<FullBTBPrediction> &stages)
{
    const auto tid = stages.front().tid;
    const auto asidHash = stages.front().asidHash;
    predictor->beginPrediction(pc, history, tid);
    for (unsigned s = predictor->getDelay(); s < stages.size(); ++s) {
        auto &pred = stages[s];
        const auto directions = predictor->predictDirections(
            pc, pred.btbEntries, pred.tageInfoForMgscs, tid, asidHash);
        for (size_t i = 0; i < pred.btbEntries.size(); ++i) {
            auto &entry = pred.btbEntries[i];
            if (entry.valid && entry.isCond) {
                entry.taken = directions[i].valid ?
                    directions[i].taken : entry.baseTaken;
            }
        }
    }
}

inline void
predictForTest(MicroTAGE *predictor, Addr pc,
               const boost::dynamic_bitset<> &history,
               std::vector<FullBTBPrediction> &stages)
{
    const auto tid = stages.front().tid;
    const auto asidHash = stages.front().asidHash;
    const std::vector<BranchPrediction> empty;
    const auto &first = predictor->getDelay() < stages.size() ?
        stages[predictor->getDelay()].btbEntries : empty;
    predictor->beginPrediction(pc, history, first, tid);
    for (unsigned s = predictor->getDelay(); s < stages.size(); ++s) {
        auto &entries = stages[s].btbEntries;
        const auto directions = predictor->predictDirections(
            pc, entries, tid, asidHash);
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].valid && entries[i].isCond &&
                    predictor->isAbtbEntry(entries[i])) {
                entries[i].taken = directions[i].valid ?
                        directions[i].taken : entries[i].baseTaken;
            }
        }
    }
}

inline void
predictForTest(BTBMGSC *predictor, Addr pc,
               const boost::dynamic_bitset<> &history,
               std::vector<FullBTBPrediction> &stages)
{
    if (!predictor->isEnabled()) {
        return;
    }
    const auto tid = stages.front().tid;
    const auto asidHash = stages.front().asidHash;
    predictor->beginPrediction(pc, history, tid);
    for (unsigned s = predictor->getDelay(); s < stages.size(); ++s) {
        auto &pred = stages[s];
        const auto directions = predictor->predictDirections(
            pc, pred.btbEntries, pred.tageInfoForMgscs, tid, asidHash);
        for (size_t i = 0; i < pred.btbEntries.size(); ++i) {
            auto &entry = pred.btbEntries[i];
            if (entry.valid && entry.isCond) {
                const auto info = pred.tageInfoForMgscs.find(entry.pc);
                const bool fallback = info != pred.tageInfoForMgscs.end() &&
                    info->second.tage_pred_taken;
                entry.taken = directions[i].valid ? directions[i].taken : fallback;
            }
        }
    }
}

inline void
predictForTest(BTBRAS *predictor, Addr pc,
               const boost::dynamic_bitset<> &history,
               std::vector<FullBTBPrediction> &stages)
{
    const auto target = predictor->predictReturnTarget(pc, stages.front().tid);
    for (unsigned s = predictor->getDelay(); s < stages.size(); ++s) {
        for (auto &entry : stages[s].btbEntries) {
            if (entry.isIndirect && entry.isReturn) {
                entry.predictedTarget = target;
            }
        }
    }
}

template<class Predictor>
void
predictForTest(const std::unique_ptr<Predictor> &predictor, Addr pc,
               const boost::dynamic_bitset<> &history,
               std::vector<FullBTBPrediction> &stages)
{
    predictForTest(predictor.get(), pc, history, stages);
}

} // namespace test
} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_BTB_TEST_PREDICTION_HELPERS_HH__
