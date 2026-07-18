#ifndef __CPU_PRED_BTB_ITTAGE_HH__
#define __CPU_PRED_BTB_ITTAGE_HH__

#include <array>
#include <deque>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "base/sat_counter.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/limits.hh"
#include "cpu/pred/btb/common.hh"
#include "cpu/pred/btb/folded_hist.hh"
#include "cpu/pred/btb/timed_base_pred.hh"
#include "debug/DecoupleBP.hh"
#include "params/BTBITTAGE.hh"
#include "sim/sim_object.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

class BTBITTAGE : public TimedBaseBTBPredictor
{
    using defer = std::shared_ptr<void>;
    using bitset = boost::dynamic_bitset<>;
    static constexpr unsigned MaxThreads = o3::MaxThreads;
  public:
    typedef BTBITTAGEParams Params;

    struct TageEntry
    {
        public:
            bool valid;
            Addr tag;
            Addr target;
            short counter;
            bool useful;
            Addr pc; // TODO: should use lowest bits only

            TageEntry() : valid(false), tag(0), target(0), counter(0), useful(false), pc(0) {}

            TageEntry(Addr tag, Addr target, short counter, Addr pc) :
                        valid(true), tag(tag), target(target), counter(counter), useful(false), pc(pc) {}
            bool taken() {
                return counter >= 2;
            }
    };

    struct TageTableInfo
    {
        public:
            bool found;
            TageEntry entry;
            unsigned table;
            Addr index;
            Addr tag;
            TageTableInfo() : found(false), table(0), index(0), tag(0) {}
            TageTableInfo(bool found, TageEntry entry, unsigned table, Addr index, Addr tag) :
                        found(found), entry(entry), table(table), index(index), tag(tag) {}
            bool taken() { return entry.taken(); }
    };

    struct TagePrediction
    {
        public:
            Addr btb_pc;
            TageTableInfo mainInfo;
            TageTableInfo altInfo;

            bool useAlt;
            // bitset usefulMask;
            // bool taken;
            Addr target;

            TagePrediction() : btb_pc(0), useAlt(false), target(0) {}

            TagePrediction(Addr btb_pc, TageTableInfo mainInfo, TageTableInfo altInfo,
                            bool useAlt, Addr target) :
                            btb_pc(btb_pc), mainInfo(mainInfo), altInfo(altInfo),
                            useAlt(useAlt), target(target) {}

    };

  public:
    BTBITTAGE(const Params& p);

    void tickStart() override;

    void tick() override;
    void dryRunCycle(Addr startAddr) override;
    // make predictions, record in stage preds
    void putPCHistory(Addr startAddr,
                      const boost::dynamic_bitset<> &history,
                      std::vector<FullBTBPrediction> &stagePreds) override;

    std::shared_ptr<void> getPredictionMeta(ThreadID tid = 0) override;
    void refreshPredictionMeta(Addr startAddr,
                               const boost::dynamic_bitset<> &history,
                               FullBTBPrediction &pred) override;

    // Speculatively update path folded histories.
    void specUpdatePHist(const boost::dynamic_bitset<> &history,
                         FullBTBPrediction &pred,
                         const PathHistoryUpdate &update) override;

    // Recover path folded histories after a misprediction.
    void recoverPHist(const boost::dynamic_bitset<> &history,
                      const FetchTarget &entry,
                      const PathHistoryUpdate &update) override;

    void update(const FetchTarget &entry) override;

    void commitBranch(const FetchTarget &stream, const DynInstPtr &inst) override;

    // check folded hists after speculative update and recover
    void checkFoldedHist(const bitset &history, const char *when);
    void checkFoldedHist(const bitset &history, ThreadID tid, const char *when);

  private:

    // return provided
    void lookupHelper(Addr stream_start, const std::vector<BTBEntry> &btbEntries,
                      IndirectTargets& results, ThreadID tid, uint8_t asidHash);

    // use blockPC
    Addr getTageIndex(Addr pc, int table, uint8_t asidHash = 0,
                      ThreadID tid = 0);

    // use blockPC (uint64_t version for performance)
    Addr getTageIndex(Addr pc, int table, uint64_t foldedHist,
                      uint8_t asidHash = 0, ThreadID tid = 0);

    // use blockPC
    Addr getTageTag(Addr pc, int table, uint8_t asidHash = 0);

    // use blockPC (uint64_t version for performance)
    Addr getTageTag(Addr pc, int table, uint64_t foldedHist, uint64_t altFoldedHist,
                    uint8_t asidHash = 0);

    Addr getOffset(Addr pc) {
        return (pc & (blockSize - 1)) >> 1;
    }

    // Update branch history
    void doUpdateHist(const bitset &history, bool taken, Addr pc, Addr target,
                      ThreadID tid);

    const unsigned numPredictors;

    std::vector<unsigned> tableSizes;
    std::vector<unsigned> tableIndexBits;
    std::vector<bitset> tableIndexMasks;
    // std::vector<uint64_t> tablePcMasks;
    std::vector<unsigned> tableTagBits;
    std::vector<bitset> tableTagMasks;
    std::vector<unsigned> tablePcShifts;
    std::vector<unsigned> histLengths;
    struct ThreadHistoryState
    {
        std::vector<PathFoldedHist> tagFoldedHist;
        std::vector<PathFoldedHist> altTagFoldedHist;
        std::vector<PathFoldedHist> indexFoldedHist;
    };

    std::vector<ThreadHistoryState> threadHistory;

    LFSR64 allocLFSR;

    unsigned maxHistLen;

    std::vector<std::vector<TageEntry>> tageTable;

    std::vector<TageEntry> lookupEntries;
    std::vector<Addr> lookupIndices, lookupTags;

    bool matchTag(Addr expected, Addr found);

    void setTag(Addr &dest, Addr src, int table);

    bool debugFlagOn{false};

    unsigned numTablesToAlloc;

    unsigned numBr;

    unsigned instShiftAmt {1};

    void updateCounter(bool taken, unsigned width, short &counter);

    bool satIncrement(int max, short &counter);

    bool satDecrement(int min, short &counter);

    int usefulResetCnt;
    std::array<int, MaxThreads> usefulResetCntByThread{};

#ifdef UNIT_TEST
    typedef uint64_t Scalar;
#else
    typedef statistics::Scalar Scalar;
#endif

    // Statistics for ITTAGE predictor
#ifdef UNIT_TEST
    struct IttageStats
    {
#else
    struct IttageStats : public statistics::Group
    {
#endif
        // Prediction phase counters
        Scalar predNoHitUseBTB;
        Scalar predUseAlt;
        Scalar predTargetHit;

        // Update phase counters
        Scalar updateMispred;
        Scalar updateAllocSuccess;
        Scalar updateAllocFailure;
        Scalar updateResetU;
        Scalar updateUseAltCorrect;

#ifndef UNIT_TEST
        statistics::Distribution predTableHits;
        statistics::Distribution updateTableHits;

        int numPredictors;
        IttageStats(statistics::Group* parent, int numPredictors);
#endif
        Scalar commitHits;
        Scalar callHits;
        Scalar otherHits;
        Scalar commitMisses;
        Scalar callMisses;
        Scalar otherMisses;
        Scalar commitPredCorrect;
        Scalar commitPredWrong;
        Scalar callPredCorrect;
        Scalar otherPredCorrect;
        Scalar callPredWrong;
        Scalar otherPredWrong;
    };

    IttageStats ittageStats;

    typedef struct TageMeta
    {
        std::unordered_map<Addr, TagePrediction> preds;
        bitset usefulMask;
        // Folded-history snapshots stored as raw values (get()); update lookups
        // read them directly and recovery restores them via recoverValue().
        std::vector<uint64_t> tagFoldedHist;
        std::vector<uint64_t> altTagFoldedHist;
        std::vector<uint64_t> indexFoldedHist;
        TageMeta(std::unordered_map<Addr, TagePrediction> preds, bitset usefulMask,
                 std::vector<uint64_t> tagFoldedHist, std::vector<uint64_t> altTagFoldedHist,
                 std::vector<uint64_t> indexFoldedHist)
            : preds(preds),
              usefulMask(usefulMask),
              tagFoldedHist(tagFoldedHist),
              altTagFoldedHist(altTagFoldedHist),
              indexFoldedHist(indexFoldedHist)
        {
        }
        TageMeta() {}
        TageMeta(const TageMeta &other)
        {
            preds = other.preds;
            usefulMask = other.usefulMask;
            tagFoldedHist = other.tagFoldedHist;
            altTagFoldedHist = other.altTagFoldedHist;
            indexFoldedHist = other.indexFoldedHist;
        }
    } TageMeta;

    std::vector<std::shared_ptr<TageMeta>> threadMeta;

    // Free-list of reusable TageMeta objects (see BTBTAGE::acquireTageMeta for
    // rationale): recycling avoids re-allocating the snapshot vectors and the
    // prediction map's bucket storage on every prediction.
    std::shared_ptr<std::vector<std::unique_ptr<TageMeta>>> tageMetaPool{
        std::make_shared<std::vector<std::unique_ptr<TageMeta>>>()};
    std::shared_ptr<TageMeta> acquireTageMeta();
    ThreadID predictorTid(const std::vector<FullBTBPrediction> &stagePreds) const;
    ThreadHistoryState &historyState(ThreadID tid);
    const ThreadHistoryState &historyState(ThreadID tid) const;

public:

    Addr debugPC = 0;
    Addr debugPC2 = 0;
    bool debugFlag = false;

    void recoverFoldedHist(const bitset& history);
    bool tageHit(ThreadID tid);

    // void checkFoldedHist(const bitset& history);
};
}

}

}

#endif  // __CPU_PRED_BTB_ITTAGE_HH__
