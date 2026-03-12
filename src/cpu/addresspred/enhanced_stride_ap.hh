#ifndef __ENHANCED_STRIDE_AP_HH__
#define __ENHANCED_STRIDE_AP_HH__

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "base/sat_counter.hh"
#include "base/types.hh"
#include "cpu/addresspred/addresspred_unit.hh"
#include "params/EStrideAP.hh"

namespace gem5
{

namespace addresspred
{

class EStrideAP : public APUnit
{
  private:
    using Params = EStrideAPParams;
    using UpdateConfDecision = std::pair<bool, int>;

    class ESEntry
    {
      public:
        uint32_t tag = 0;
        int confidence = 0;
        int64_t stride = 0;
        Addr lastAddr = 0;
        int useful = 0;
        unsigned notFirstAppear = 0;
        SatCounter8 dcacheConfidence;

        explicit ESEntry(unsigned dcache_counter_bits = 3)
            : dcacheConfidence(dcache_counter_bits, 0)
        {
        }
    };

    class InflightWindow
    {
      public:
        InflightWindow(int windowTagLength, bool idealWindow);

        int addToInflightWindow(Addr pc);
        void removeFromWindow(Addr pc, uint64_t seq_no);
        void squash(uint64_t seq_no);

      private:
        using HashMethod = uint64_t (*)(uint64_t, int);

        int windowTagLength;
        HashMethod hashMethod;
        std::unordered_map<uint64_t, int> windows;
        uint64_t lastSeqNo = 0ul;
    };

  private:
    const int ways;
    const int strideWidth;
    const int tagWidth;
    const int logESTBEntrys;
    const int entryCounts;
    const int logMaxConfidence;
    const int MAXCONFIDENCE;
    const int confidenceThreshold;
    const unsigned dcacheCounterBits;
    const double dcacheThresholdPercent;
    InflightWindow inflightWindow;
    std::vector<std::vector<ESEntry>> ESTables;

  private:
    APResult doPredict(APPredMetaData *predMetaData, int inflights);
    int64_t extendStride(int64_t entryStride);
    uint32_t pcHashToWayIndex(Addr pc, int way);
    uint32_t pcHashToTag(Addr pc, int way);
    uint32_t compareTags(uint32_t tag1, uint32_t tag2);
    UpdateConfDecision decideToUpdate(int64_t stride);
    uint32_t tryDecUseful(const ESEntry &entry);
    bool isDcacheConfidenceLow(const ESEntry &entry) const;

  public:
    EStrideAP(const Params &params);

    std::string name() const override { return "EStrideAP"; }

    APResult addressPredict(APPredMetaData *predMetaData) override;
    void updateAddressPredictor(APUpdateMetaData *updateMetaData) override;
    void specUpdateAddressPredictor(
            APSpecUpdateMetaData *specUpdateMetaData) override;
    void squash(const uint64_t seq_no) override;

    AddressPredType getAddressPredictorType() override
    {
        return AddressPredType::EStrideAP;
    }
};

} // namespace addresspred

} // namespace gem5

#endif // __ENHANCED_STRIDE_AP_HH__
