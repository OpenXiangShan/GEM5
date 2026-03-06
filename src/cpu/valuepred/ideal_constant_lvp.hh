#ifndef __IDEAL_CONSTANT_LVP_HH__
#define __IDEAL_CONSTANT_LVP_HH__

#include <unordered_map>

#include "base/sat_counter.hh"
#include "base/types.hh"
#include "cpu/valuepred/gated_vp_unit.hh"
#include "params/IdealConstantLVP.hh"

namespace gem5
{

namespace valuepred
{

class IdealConstantLVP : public GatedVPUnit
{
  private:
    using Params = IdealConstantLVPParams;

    struct ICEntry
    {
        SatCounter16 confidence;
        RegVal value;

        ICEntry(unsigned sat_counter_bits, RegVal v)
            : confidence(sat_counter_bits, 0), value(v)
        {
        }
    };

    std::unordered_map<Addr, ICEntry> idealConstTable;

    const unsigned satCounterBits;
    const bool resetConfidence;

  public:
    IdealConstantLVP(const Params &params);

    std::string name() const override { return "IdealConstantLVP"; }

    void specUpdateValuePredictor(VPSpecUpdateMetaData *specUpdateMetaData) override;

    void squash(const uint64_t seq_no) override;

    virtual ValuePredType getValuePredictorType() override { return ValuePredType::IdealConstantLVP; }

  private:
    VPResult valuePredictInternal(VPPredMetaData *predMetaData) override;

    void updateValuePredictorInternal(VPUpdateMetaData *updateMetaData) override;
};

} // namespace valuepred

} // namespace gem5

#endif // __IDEAL_CONSTANT_LVP_HH__
