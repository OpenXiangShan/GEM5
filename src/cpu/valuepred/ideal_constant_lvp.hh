#ifndef __IDEAL_CONSTANT_LVP_HH__
#define __IDEAL_CONSTANT_LVP_HH__

#include <unordered_map>
#include <vector>

#include "base/sat_counter.hh"
#include "base/types.hh"
#include "cpu/valuepred/valuepred_unit.hh"
#include "params/IdealConstantLVP.hh"

namespace gem5
{

namespace valuepred
{

class IdealConstantLVP : public VPUnit
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

    std::vector<std::unordered_map<Addr, ICEntry>> idealConstTables;

    const unsigned satCounterBits;
    const bool resetConfidence;

    VPResult doPredict(Addr pc, ThreadID tid) const;
    void doUpdate(Addr pc, ThreadID tid, RegVal actualValue);

  public:
    IdealConstantLVP(const Params &params);

    std::string name() const override { return "IdealConstantLVP"; }

    VPPredictionCandidate predict(const VPPredictRequest &request) override;

    void update(const VPUpdateInfo &updateInfo, const VPPredictionRecord *record,
            const VPFeedback &feedback) override;

    void specUpdate(const VPSpecUpdateInfo &specUpdateInfo) override;

    void squash(ThreadID tid, const uint64_t seq_no) override;

    virtual ValuePredType getValuePredictorType() override { return ValuePredType::IdealConstantLVP; }
};

} // namespace valuepred

} // namespace gem5

#endif // __IDEAL_CONSTANT_LVP_HH__
