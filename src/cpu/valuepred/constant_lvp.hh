#ifndef __CPU_VALUEPRED_CONSTANT_LVP_HH__
#define __CPU_VALUEPRED_CONSTANT_LVP_HH__

#include <cstdint>
#include <vector>

#include "base/sat_counter.hh"
#include "base/types.hh"
#include "cpu/valuepred/valuepred_unit.hh"
#include "params/ConstantLVP.hh"

namespace gem5
{

namespace valuepred
{

/**
 * A bounded, skewed-associative constant value predictor. Each PC has one
 * independently hashed candidate location per way. A zero confidence counter
 * encodes an invalid entry, so the logical entry contains only the hashed tag,
 * value, confidence counter, and useful counter.
 */
class ConstantLVP : public VPUnit
{
  private:
    using Params = ConstantLVPParams;

    struct Entry
    {
        uint64_t tag = 0;
        RegVal value = 0;
        SatCounter16 confidence;
        SatCounter16 useful;

        Entry(unsigned confidence_bits, unsigned useful_bits)
            : confidence(confidence_bits), useful(useful_bits)
        {
        }
    };

    struct Location
    {
        unsigned way = 0;
        unsigned index = 0;
        uint64_t tag = 0;
    };

    const unsigned numWays;
    const unsigned numSets;
    const unsigned setBits;
    const unsigned tagBits;
    const unsigned confidenceBits;
    const unsigned usefulBits;
    const bool resetConfidence;
    const uint16_t maxConfidence;
    const uint16_t confidenceThreshold;
    const unsigned confidencePenalty;

    // A zero confidence counter denotes an invalid entry.
    // [tid][way][set]
    std::vector<std::vector<std::vector<Entry>>> tables;

    unsigned pcHashToWayIndex(Addr pc, unsigned way) const;
    uint64_t pcHashToTag(Addr pc, unsigned way) const;
    Location locationForWay(Addr pc, unsigned way) const;

    Entry *findEntry(Addr pc, ThreadID tid, Location &location);
    void allocate(Entry &entry, uint64_t tag, RegVal value);
    bool tryDecUseful(Entry &entry);

  public:
    explicit ConstantLVP(const Params &params);

    std::string name() const override { return "ConstantLVP"; }

    VPPredictionCandidate predict(const VPPredictRequest &request) override;

    void update(const VPUpdateInfo &updateInfo,
            const VPPredictionRecord *record,
            const VPFeedback &feedback) override;

    void specUpdate(const VPSpecUpdateInfo &specUpdateInfo) override;

    void squash(ThreadID tid, const uint64_t seq_no) override;

    ValuePredType getValuePredictorType() override
    {
        return ValuePredType::ConstantLVP;
    }

    struct ConstantLVPStats : public statistics::Group
    {
        statistics::Scalar lookups;
        statistics::Scalar lookupHits;
        statistics::Scalar lookupMisses;
        statistics::Scalar lowConfidenceHits;
        statistics::Scalar updates;
        statistics::Scalar updateHits;
        statistics::Scalar updateMisses;
        statistics::Scalar valueMatches;
        statistics::Scalar valueMismatches;
        statistics::Scalar mismatchInvalidations;
        statistics::Scalar invalidAllocations;
        statistics::Scalar usefulReplacements;
        statistics::Scalar allocationFailures;
        statistics::Scalar usefulDecrements;

        explicit ConstantLVPStats(statistics::Group *parent);
    } constantStats;
};

} // namespace valuepred

} // namespace gem5

#endif // __CPU_VALUEPRED_CONSTANT_LVP_HH__
