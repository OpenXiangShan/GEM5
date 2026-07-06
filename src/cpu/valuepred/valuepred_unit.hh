#ifndef __VALUEPRED_UNIT_HH__
#define __VALUEPRED_UNIT_HH__

#include <memory>
#include <string>

#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/valuepred/valuepred_metadata.hh"
#include "enums/ValuePredType.hh"
#include "params/ValuePredictor.hh"
#include "sim/sim_object.hh"
#include "sim/stats.hh"

namespace gem5
{

namespace valuepred
{



class VPUnit : public SimObject
{
  private:
    using Params = ValuePredictorParams;

  protected:
    const unsigned numThreads;

    void assertValidTid(ThreadID tid) const;

  public:
    VPUnit(const Params &params);

    std::string name() const { return "valuePredict.base"; }

    // Do the value predict.
    virtual VPPredictionCandidate predict(const VPPredictRequest &request) = 0;

    VPResult valuePredict(const VPPredictRequest &request,
            std::unique_ptr<VPPredictionRecord> &record);

    // In commit time, update value predictor
    virtual void update(const VPUpdateInfo &updateInfo,
            const VPPredictionRecord *record, const VPFeedback &feedback) = 0;

    // Some value predictor need speculative update to support back-to-back predict.
    virtual void specUpdate(const VPSpecUpdateInfo &specUpdateInfo) = 0;

    // If predict error, squash the inflight instructions in value predictor.
    virtual void squash(ThreadID tid, const uint64_t seq_no) = 0;

    // Get the value predictor type
    virtual ValuePredType getValuePredictorType() = 0;

  public:
    struct ValuePredUnitStats : public statistics::Group
    {
        ValuePredUnitStats(VPUnit *vp);

        statistics::Scalar VPcorrected;
        statistics::Scalar VPpredicted;
        statistics::Formula VPaccuracy;
        statistics::Scalar VPsupported;
        statistics::Formula VPcoverage;

    } stats;
};


}


}

#endif
