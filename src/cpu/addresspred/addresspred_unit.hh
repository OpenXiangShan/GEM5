#ifndef __ADDRESSPRED_UNIT_HH__
#define __ADDRESSPRED_UNIT_HH__

#include <string>

#include "base/statistics.hh"
#include "cpu/addresspred/addresspred_metadata.hh"
#include "enums/AddressPredType.hh"
#include "params/AddressPredictor.hh"
#include "sim/sim_object.hh"
#include "sim/stats.hh"

namespace gem5
{

namespace addresspred
{

class APUnit : public SimObject
{
  private:
    using Params = AddressPredictorParams;

  public:
    APUnit(const Params &params);

    std::string name() const { return "addressPredict.base"; }

    // Do the address prediction.
    virtual APResult addressPredict(APPredMetaData *predMetadata) = 0;

    // In commit time, update address predictor.
    virtual void updateAddressPredictor(APUpdateMetaData *updateMetadata) = 0;

    // Some predictors may need speculative update to support back-to-back
    // prediction.
    virtual void specUpdateAddressPredictor(
            APSpecUpdateMetaData *specupdateMetadata) = 0;

    // If predict error, squash the inflight instructions in address
    // predictor.
    virtual void squash(const uint64_t seq_no) = 0;

    // Get the address predictor type.
    virtual AddressPredType getAddressPredictorType() = 0;

  public:
    struct AddressPredUnitStats : public statistics::Group
    {
        AddressPredUnitStats(APUnit *ap);

        statistics::Scalar APcorrected;
        statistics::Scalar APpredicted;
        statistics::Formula APaccuracy;
        statistics::Scalar APsupported;
        statistics::Formula APcoverage;
        statistics::Scalar APupdateRequests;
        statistics::Scalar APupdateFromDcache;
        statistics::Formula APupdateFromDcacheRatio;
    } stats;
};

} // namespace addresspred

} // namespace gem5

#endif // __ADDRESSPRED_UNIT_HH__
