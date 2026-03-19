#include "cpu/addresspred/addresspred_unit.hh"

#include "base/stats/group.hh"
#include "base/stats/units.hh"

namespace gem5
{

namespace addresspred
{

APUnit::APUnit(const Params &params) : ClockedObject(params), stats(this) {}

APUnit::AddressPredUnitStats::AddressPredUnitStats(APUnit *ap)
    : statistics::Group(ap),
      ADD_STAT(APcorrected, statistics::units::Count::get(),
               "number of correct ap"),
      ADD_STAT(APpredicted, statistics::units::Count::get(),
               "number of predicted ap"),
      ADD_STAT(APaccuracy, statistics::units::Ratio::get(),
               "the accuracy of address predictor",
               APcorrected / APpredicted),
      ADD_STAT(APsupported, statistics::units::Count::get(),
               "number of ap support"),
      ADD_STAT(APcoverage, statistics::units::Ratio::get(),
               "the coverage of ap",
               APpredicted / APsupported),
      ADD_STAT(APupdateRequests, statistics::units::Count::get(),
               "number of address predictor update requests"),
      ADD_STAT(APupdateFromDcache, statistics::units::Count::get(),
               "number of address predictor updates with fromDcache=true"),
      ADD_STAT(APupdateFromDcacheRatio, statistics::units::Ratio::get(),
               "ratio of fromDcache updates among all AP update requests",
               APupdateFromDcache / APupdateRequests),
      ADD_STAT(APmispredTotal, statistics::units::Count::get(),
               "number of AP mispredictions"),
      ADD_STAT(APmispredAddrOnly, statistics::units::Count::get(),
               "number of AP mispredictions with only address mismatch"),
      ADD_STAT(APmispredDataOnly, statistics::units::Count::get(),
               "number of AP mispredictions with only data mismatch"),
      ADD_STAT(APmispredAddrAndData, statistics::units::Count::get(),
               "number of AP mispredictions with both address and data mismatches"),
      ADD_STAT(APmispredAddrErrorRatio, statistics::units::Ratio::get(),
               "ratio of AP mispredictions that contain address mismatch",
               (APmispredAddrOnly + APmispredAddrAndData) / APmispredTotal),
      ADD_STAT(APmispredDataErrorRatio, statistics::units::Ratio::get(),
               "ratio of AP mispredictions that contain data mismatch",
               (APmispredDataOnly + APmispredAddrAndData) / APmispredTotal)
{
    APmispredTotal.flags(statistics::nozero);
    APmispredAddrOnly.flags(statistics::nozero);
    APmispredDataOnly.flags(statistics::nozero);
    APmispredAddrAndData.flags(statistics::nozero);
}

} // namespace addresspred

} // namespace gem5
