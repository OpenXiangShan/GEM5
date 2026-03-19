#include "cpu/addresspred/ideal_constant_ap.hh"

#include <cassert>

#include "cpu/addresspred/addresspred_metadata.hh"

namespace gem5
{

namespace addresspred
{

IdealConstantAP::IdealConstantAP(const Params &params)
    : APUnit(params),
      satCounterBits(params.satCounterBits),
      resetConfidence(params.resetConfidence)
{
}

APResult
IdealConstantAP::addressPredict(APPredMetaData *predMetaData)
{
    auto it = idealConstTable.find(predMetaData->pc);
    if (it != idealConstTable.end()) {
        if (it->second.confidence.isSaturated()) {
            return {true, it->second.addr};
        }
    }
    return {false, 0};
}

void
IdealConstantAP::updateAddressPredictor(APUpdateMetaData *updateMetaData)
{
    auto it = idealConstTable.find(updateMetaData->pc);
    if (it == idealConstTable.end()) {
        auto [new_it, success] = idealConstTable.emplace(
                std::piecewise_construct,
                std::forward_as_tuple(updateMetaData->pc),
                std::forward_as_tuple(
                    satCounterBits, updateMetaData->actualAddr));
        assert(success);
        (void)new_it;
    } else {
        // Train only on loads whose final data is sourced from dcache, not
        // store forwarding/sbuffer/bus side channels.
        if (updateMetaData->fromDcache && updateMetaData->actualAddr == it->second.addr) {
            it->second.confidence++;
        } else {
            if (resetConfidence) {
                it->second.confidence.reset();
            } else {
                it->second.confidence--;
            }
            it->second.addr = updateMetaData->actualAddr;
        }
    }
}

void
IdealConstantAP::specUpdateAddressPredictor(
        APSpecUpdateMetaData *specUpdateMetaData)
{
    // Do nothing
}

void
IdealConstantAP::squash(const uint64_t seq_no)
{
    (void)seq_no;
    // Do nothing
}

void
IdealConstantAP::squash(const uint64_t seq_no, uint8_t squash_version)
{
    (void)seq_no;
    (void)squash_version;
    // Do nothing
}

} // namespace addresspred

} // namespace gem5
