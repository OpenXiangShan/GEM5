#ifndef __CPU_VALUEPRED_COMPOSITE_VALUE_PREDICTOR_ARB_HH__
#define __CPU_VALUEPRED_COMPOSITE_VALUE_PREDICTOR_ARB_HH__

#include <memory>
#include <vector>

#include "base/sat_counter.hh"
#include "cpu/valuepred/valuepred_metadata.hh"
#include "params/CVPArb.hh"
#include "params/CVPConfidenceArb.hh"
#include "params/CVPFixedPriorityArb.hh"
#include "params/CVPRRArb.hh"
#include "params/CVPRandomArb.hh"
#include "sim/sim_object.hh"

namespace gem5
{

namespace valuepred
{

class CompositeValuePredictorArb : public SimObject
{
  public:
    using Params = CVPArbParams;

    explicit CompositeValuePredictorArb(const Params &params)
        : SimObject(params)
    {
    }

    ~CompositeValuePredictorArb() override = default;

    virtual int choose(const std::vector<VPChooserCandidate> &candidates) = 0;

    virtual void update(const std::vector<VPChooserCandidate> &candidates,
            const std::vector<VPChooserFeedback> &feedbacks)
    {
    }
};

class CompositeValuePredictorFixedPriorityArb :
    public CompositeValuePredictorArb
{
  public:
    using Params = CVPFixedPriorityArbParams;

    explicit CompositeValuePredictorFixedPriorityArb(const Params &params)
        : CompositeValuePredictorArb(params)
    {
    }

    int choose(const std::vector<VPChooserCandidate> &candidates) override;
};

class CompositeValuePredictorRandomArb :
    public CompositeValuePredictorArb
{
  public:
    using Params = CVPRandomArbParams;

    explicit CompositeValuePredictorRandomArb(const Params &params)
        : CompositeValuePredictorArb(params)
    {
    }

    int choose(const std::vector<VPChooserCandidate> &candidates) override;
};

class CompositeValuePredictorRRArb :
    public CompositeValuePredictorArb
{
  private:
    int nextChild;

  public:
    using Params = CVPRRArbParams;

    explicit CompositeValuePredictorRRArb(const Params &params)
        : CompositeValuePredictorArb(params),
          nextChild(0)
    {
    }

    int choose(const std::vector<VPChooserCandidate> &candidates) override;
};

class CompositeValuePredictorConfidenceArb :
    public CompositeValuePredictorArb
{
  private:
    const unsigned counterBits;
    std::vector<SatCounter8> childConfidence;

    void ensureCapacity(size_t num_children);

  public:
    using Params = CVPConfidenceArbParams;

    explicit CompositeValuePredictorConfidenceArb(const Params &params)
        : CompositeValuePredictorArb(params),
          counterBits(params.counterBits)
    {
    }

    int choose(const std::vector<VPChooserCandidate> &candidates) override;

    void update(const std::vector<VPChooserCandidate> &candidates,
            const std::vector<VPChooserFeedback> &feedbacks) override;
};

} // namespace valuepred

} // namespace gem5

#endif
