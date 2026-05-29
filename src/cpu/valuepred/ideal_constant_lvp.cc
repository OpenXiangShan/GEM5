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
IdealConstantLVP::valuePredict(VPPredMetaData *predMetaData)
{
    assertValidTid(predMetaData->tid);
    auto &idealConstTable = idealConstTables[predMetaData->tid];
    auto it = idealConstTable.find(predMetaData->pc);
    if (it != idealConstTable.end()) {
        if (it->second.confidence.isSaturated()) {
            return {true, it->second.value};
        }
    }
    return {false, 0};
}

void
IdealConstantLVP::updateValuePredictor(VPUpdateMetaData *updateMetaData)
{
    assertValidTid(updateMetaData->tid);
    auto &idealConstTable = idealConstTables[updateMetaData->tid];
    auto it = idealConstTable.find(updateMetaData->pc);
    if (it == idealConstTable.end()) {
        // Not found, allocate a new entry
        auto [it, success] = idealConstTable.emplace(std::piecewise_construct,
            std::forward_as_tuple(updateMetaData->pc),
            std::forward_as_tuple(satCounterBits, updateMetaData->actualValue));

        assert(success);
    } else {
        // Found
        bool validActualValue = updateMetaData->actualValue != 0xdeadbeefULL;
        if (validActualValue && updateMetaData->actualValue == it->second.value) {
            it->second.confidence++;
        } else {
            if (resetConfidence) {
                it->second.confidence.reset();
            } else {
                it->second.confidence--;
            }
            it->second.value = updateMetaData->actualValue;
        }
    }
}

void
IdealConstantLVP::specUpdateValuePredictor(VPSpecUpdateMetaData *specUpdateMetaData)
{
    // Do nothing
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
