#ifndef __MEMORY_RENAMING_HH__
#define __MEMORY_RENAMING_HH__

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "base/types.hh"
#include "cpu/valuepred/valuepred_unit.hh"
#include "params/MemoryRenaming.hh"

namespace gem5
{

namespace valuepred
{

class MemoryRenaming : public VPUnit
{
  private:
    using Params = MemoryRenamingParams;

    class StoreLoadCacheEntry
    {
      public:
        bool valid = false;
        uint32_t tag = 0;
        uint64_t valueFileIndex = 0;
        int confidence = 0;
        int useful = 0;
        bool hasProducerStorePC = false;
        Addr producerStorePC = 0;
    };

    class InflightWindow : public Named
    {
      public:
        int windowTagLength;
        bool idealWindow;
        std::unordered_map<uint64_t, int> windows;

        int windowActiveCount;
        uint64_t lastSeqNo;

      public:
        std::string name() const override { return "memory renaming inflight window"; }

        InflightWindow(int windowTagLength, bool idealWindow);

        int addToInflightWindow(Addr pc);
        void removeFromWindow(Addr pc, uint64_t seq_no);
        void squash(uint64_t seq_no);

      private:
        using HashMethod = uint64_t (*)(uint64_t, int);
        HashMethod hashMethod;
    };

  private:
    const int ways;
    const int tagWidth;
    const int logESTBEntrys;
    const int entryCounts;
    const int logMaxConfidence;
    const int MAXCONFIDENCE;
    const int confidenceThreshold;

    const int logStoreLoadValueFileEntries;
    uint64_t nextValueFileIndex;

    InflightWindow inflightWindow;
    std::vector<std::vector<StoreLoadCacheEntry>> storeLoadCacheTables;

  private:
    uint32_t pcHashToWayIndex(Addr pc, int way);
    uint32_t pcHashToTag(Addr pc, int way);
    uint32_t compareTags(uint32_t tag1, uint32_t tag2);

    VPResult doStoreLoadCacheLookup(VPPredMetaData *predMetaData);
    void updateStoreLoadCache(VPUpdateMetaData *updateMetaData);

  public:
    MemoryRenaming(const Params &params);

    std::string name() const override { return "MemoryRenaming"; }

    VPResult valuePredict(VPPredMetaData *predMetaData) override;

    void updateValuePredictor(VPUpdateMetaData *updateMetaData) override;

    void specUpdateValuePredictor(VPSpecUpdateMetaData *specUpdateMetaData) override;

    void squash(const uint64_t seq_no) override;

    ValuePredType getValuePredictorType() override
    {
        return ValuePredType::MemoryRenaming;
    }
};

} // namespace valuepred

} // namespace gem5

#endif
