#ifndef __IDEAL_CONSTANT_AP_HH__
#define __IDEAL_CONSTANT_AP_HH__

#include <unordered_map>

#include "base/sat_counter.hh"
#include "base/types.hh"
#include "cpu/addresspred/addresspred_unit.hh"
#include "params/IdealConstantAP.hh"

namespace gem5
{

namespace addresspred
{

class IdealConstantAP : public APUnit
{
  private:
    using Params = IdealConstantAPParams;

    struct ICEntry
    {
        SatCounter16 confidence;
        Addr addr;

        ICEntry(unsigned sat_counter_bits, Addr a)
            : confidence(sat_counter_bits, 0), addr(a)
        {
        }
    };

    std::unordered_map<Addr, ICEntry> idealConstTable;

    const unsigned satCounterBits;
    const bool resetConfidence;

  public:
    IdealConstantAP(const Params &params);

    std::string name() const override { return "IdealConstantAP"; }

    APResult addressPredict(APPredMetaData *predMetaData) override;

    void updateAddressPredictor(APUpdateMetaData *updateMetaData) override;

    void specUpdateAddressPredictor(
            APSpecUpdateMetaData *specUpdateMetaData) override;

    void squash(const uint64_t seq_no) override;

    AddressPredType getAddressPredictorType() override
    {
        return AddressPredType::IdealConstantAP;
    }
};

} // namespace addresspred

} // namespace gem5

#endif // __IDEAL_CONSTANT_AP_HH__
