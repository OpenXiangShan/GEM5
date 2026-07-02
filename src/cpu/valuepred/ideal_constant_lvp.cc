#include "cpu/valuepred/ideal_constant_lvp.hh"

#include <cassert>

#include "cpu/valuepred/valuepred_metadata.hh"

namespace gem5
{

namespace valuepred
{

IdealConstantLVP::IdealConstantLVP(const Params &params)
    : VPUnit(params),
      idealConstTables(params.numThreads),
      satCounterBits(params.satCounterBits),
      resetConfidence(params.resetConfidence)
{
}

VPResult
IdealConstantLVP::doPredict(Addr pc, ThreadID tid) const
{
    assertValidTid(tid);
    const auto &idealConstTable = idealConstTables[tid];
    auto it = idealConstTable.find(pc);
    if (it != idealConstTable.end()) {
        if (it->second.confidence.isSaturated()) {
            return {true, it->second.value};
        }
    }
    return {false, 0};
}

VPPredictionCandidate
IdealConstantLVP::predict(const VPPredictRequest &request)
{
    VPPredictionCandidate candidate;
    candidate.result = doPredict(request.pc, request.tid);
    if (candidate.result.speculative) {
        candidate.record = std::make_unique<VPPredictionRecord>();
        candidate.record->offeredPrediction = true;
        candidate.record->predictedValue = candidate.result.value;
    }
    return candidate;
}

void
IdealConstantLVP::doUpdate(Addr pc, ThreadID tid, RegVal actualValue)
{
    assertValidTid(tid);
    auto &idealConstTable = idealConstTables[tid];
    auto it = idealConstTable.find(pc);
    if (it == idealConstTable.end()) {
        // Not found, allocate a new entry
        auto [it, success] = idealConstTable.emplace(std::piecewise_construct,
            std::forward_as_tuple(pc),
            std::forward_as_tuple(satCounterBits, actualValue));

        assert(success);
    } else {
        // Found
        bool validActualValue = actualValue != 0xdeadbeefULL;
        if (validActualValue && actualValue == it->second.value) {
            it->second.confidence++;
        } else {
            if (resetConfidence) {
                it->second.confidence.reset();
            } else {
                it->second.confidence--;
            }
            it->second.value = actualValue;
        }
    }
}

void
IdealConstantLVP::update(const VPUpdateInfo &updateInfo,
        const VPPredictionRecord *record, const VPFeedback &feedback)
{
    (void)record;
    (void)feedback;
    doUpdate(updateInfo.pc, updateInfo.tid, updateInfo.actualValue);
}

void
IdealConstantLVP::specUpdate(const VPSpecUpdateInfo &specUpdateInfo)
{
    (void)specUpdateInfo;
}

void
IdealConstantLVP::squash(ThreadID tid, const uint64_t seq_no)
{
    (void)tid;
    (void)seq_no;
    // Do nothing
}

} // namespace valuepred

} // namespace gem5
