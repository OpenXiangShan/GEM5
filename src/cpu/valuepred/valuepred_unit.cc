#include "cpu/valuepred/valuepred_unit.hh"

#include <utility>

#include "base/logging.hh"
#include "base/stats/group.hh"
#include "base/stats/units.hh"

namespace gem5
{

namespace valuepred
{

VPUnit::VPUnit(const Params &params)
    : SimObject(params),
      numThreads(params.numThreads),
      stats(this)
{
    gem5_assert(numThreads > 0, "Value predictor needs at least one thread\n");
}

void
VPUnit::assertValidTid(ThreadID tid) const
{
    gem5_assert(tid < numThreads, "%s got invalid tid %u, numThreads=%u\n",
                name().c_str(), static_cast<unsigned>(tid), numThreads);
}

VPResult
VPUnit::valuePredict(const VPPredictRequest &request,
        std::unique_ptr<VPPredictionRecord> &record)
{
    auto candidate = predict(request);
    record = std::move(candidate.record);
    return candidate.result;
}

VPUnit::ValuePredUnitStats::ValuePredUnitStats(VPUnit *vp)
    : statistics::Group(vp),
      ADD_STAT(VPcorrected, statistics::units::Count::get(), "number of correct vp"),
      ADD_STAT(VPpredicted, statistics::units::Count::get(), "number of predicted vp"),
      ADD_STAT(VPaccuracy, statistics::units::Ratio::get(), "the accuracy of value predictor",
               VPcorrected / VPpredicted),
      ADD_STAT(VPsupported, statistics::units::Count::get(), "number of vp support"),
      ADD_STAT(VPcoverage, statistics::units::Ratio::get(), "the coverage of vp", VPcorrected / VPsupported)
{
}


}

}
