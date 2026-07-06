/*
 * EStride is largely based on the open-source implementation of the
 * 1st-place Championship Value Prediction (CVP-1) submission.
 *
 * The version in this codebase is adapted and modified for our environment
 * by Yibo Zhang <yb_zhang@mail.ustc.edu.cn>; it is not intended to be a
 * bit-exact copy of the original code.
 *
 * For detailed background and reference material:
 * Paper: https://microarch.org/cvp1/papers/Seznec.pdf
 * Open-source implementation: https://www.microarch.org/cvp1/code/Seznec.tar.gz
 * Official website: https://www.microarch.org/cvp1/
 */

#ifndef __ENHANCED_STTIDE_HH__
#define __ENHANCED_STTIDE_HH__

#include <climits>
#include <cstdint>
#include <set>
#include <unordered_map>
#include <vector>

#include "base/types.hh"
#include "cpu/valuepred/es_metadata.hh"
#include "cpu/valuepred/valuepred_unit.hh"
#include "params/EStride.hh"

namespace gem5
{

namespace valuepred
{

class EStride : public VPUnit
{

  private:
    using Params = EStrideParams;
    using UpdateConfDecision = std::pair<bool, int>;

    // some instruction latency, these are pre-existing data.
    // todo: This data needs to be updated to the real observed data
    // in the simulator and used when the ES is updated.
    static constexpr uint64_t FASTINSTTIME = 100u;
    static constexpr uint64_t L1HITMAXTIME = 100u;
    static constexpr uint64_t L2HITMAXTIME = 100u;
    static constexpr uint64_t L3HITMAXTIME = 100u;

  private:
    class ESEntry
    {
      public:
        uint32_t tag = 0;
        int confidence = 0;
        // todo: we can also control the bits of stride
        int64_t stride = 0;
        RegVal lastValue = 0;
        // now only use 2 bits useful
        // todo: we can also control the bits of useful
        int useful = 0;
        unsigned NotFirstAppear = 0;
    };

    // The core of the separately wrapped inflight instruction window is just
    // a map. **Note that the number of instructions in inflight is not the same
    // as the number of instructions with the same pc in the ROB, and many
    // instructions inflighted at the front of the pipeline have not yet
    // entered the ROB.**
    class InflightWindow : public Named
    {
      public:
        int windowTagLength;
        bool idealWindow;
        std::unordered_map<uint64_t, int> windows;

        // just a count for debug
        int windowActiveCount;

        uint64_t lastSeqNo;

      public:
        std::string name() const override { return "inflight window"; }

        InflightWindow(int windowTagLength, bool idealWindow);

        // Returns the number of inflights and updates
        // the inflight window.
        int addToInflightWindow(Addr pc);

        // remove from window in commit stage
        void removeFromWindow(Addr pc, uint64_t seq_no);

        // clear in squash
        void squash(uint64_t seq_no);

      private:
        // now not support hashMethod for inflight window
        using HashMethod = uint64_t (*)(uint64_t, int);
        HashMethod hashMethod;
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
    std::vector<InflightWindow> inflightWindows;
    const bool enableTimeMsgInUpdate;

  private:

    // [tid][way][index]
    std::vector<std::vector<std::vector<ESEntry>>> ESTables;

  private:
    // This function really implements the prediction function.
    VPResult doPredict(Addr pc, uint64_t seqNo, ThreadID tid, int inflights);

    struct UpdateContext
    {
        Addr pc = 0;
        uint64_t seqNo = 0;
        ThreadID tid = 0;
        RegVal actualValue = 0;
        bool isLoadInst = false;
        uint64_t inflightTime = 0;
        std::string_view disas;
    };

    void doUpdate(const UpdateContext &updateContext);

    // extend strideWidth-bits stride to 32-bits stride for calculate
    int64_t extendStride(int64_t entryStride);

    // Pass in pc and the nth way, and hash to get the index of the nth way.
    uint32_t pcHashToWayIndex(Addr pc, int way);
    // Pass in pc and the nth way, and hash to get the tag of the nth way.
    uint32_t pcHashToTag(Addr pc, int way);

    // return not-zero value if not equals
    uint32_t compareTags(uint32_t tag1, uint32_t tag2);

    // Returns a decision about whether to update the entry.
    UpdateConfDecision decideToUpdate(bool isLoadInst, uint64_t inflightTime,
            int64_t stride);

    // in update time, when we can't allocate new entry, try dec useful count
    uint32_t tryDecUseful(const ESEntry &entry);

  public:
    EStride(const Params &params);

    std::string name() const override { return "EStride"; }

    VPPredictionCandidate predict(const VPPredictRequest &request) override;

    void update(const VPUpdateInfo &updateInfo, const VPPredictionRecord *record,
            const VPFeedback &feedback) override;

    void specUpdate(const VPSpecUpdateInfo &specUpdateInfo) override;

    virtual void squash(ThreadID tid, const uint64_t seq_no) override;

    virtual ValuePredType getValuePredictorType() override { return ValuePredType::EStride; }

    // for dump some data
  private:
    std::unordered_map<Addr, std::pair<int32_t, std::string>> fluentInstructions;

  public:
    struct EStrideStats : public statistics::Group
    {
        statistics::Vector2d allocate;
        statistics::Vector2d strideNotEquals;
        statistics::Vector2d strideEquals;

        // Records the number of inflight instructions with the
        // same hash value in the inflight window when the
        // inflight window is accessed for value prediction.
        statistics::SparseHistogram inflightSH;

        EStrideStats(statistics::Group *parent)
            : statistics::Group(parent),
              ADD_STAT(allocate, "Record the assignment of valuepred_unit entries"),
              ADD_STAT(strideNotEquals, "Record the situations of stride not equals"),
              ADD_STAT(strideEquals, "Record the situations of stride equals"),
              ADD_STAT(inflightSH, "Records the number of inflight instructions")
        {
        }
    } esstats;
};

}

}


#endif
