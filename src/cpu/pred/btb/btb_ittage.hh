#ifndef __CPU_PRED_BTB_ITTAGE_HH__
#define __CPU_PRED_BTB_ITTAGE_HH__

#include <deque>
#include <map>
#include <vector>
#include <utility>

#include "base/statistics.hh"
#include "base/types.hh"
#include "base/sat_counter.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/btb/folded_hist.hh"
#include "cpu/pred/btb/stream_struct.hh"
#include "cpu/pred/btb/timed_base_pred.hh"
#include "params/BTBITTAGE.hh"
#include "debug/DecoupleBP.hh"
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
    // make predictions, record in stage preds
    void putPCHistory(Addr startAddr,
                      const boost::dynamic_bitset<> &history,
                      std::vector<FullBTBPrediction> &stagePreds) override;

    std::shared_ptr<void> getPredictionMeta() override;

    void specUpdateHist(const boost::dynamic_bitset<> &history, FullBTBPrediction &pred) override;

    void recoverHist(const boost::dynamic_bitset<> &history, const FetchStream &entry, int shamt, bool cond_taken) override;

    void update(const FetchStream &entry) override;

    void commitBranch(const FetchStream &stream, const DynInstPtr &inst) override;

    // check folded hists after speculative update and recover
    void checkFoldedHist(const bitset &history, const char *when);

  private:

    // return provided
    void lookupHelper(Addr stream_start, const std::vector<BTBEntry> &btbEntries, IndirectTargets& results);

    // use blockPC
    Addr getTageIndex(Addr pc, int table);

    // use blockPC
    Addr getTageIndex(Addr pc, int table, bitset &foldedHist);

    // use blockPC
    Addr getTageTag(Addr pc, int table);

    // use blockPC
    Addr getTageTag(Addr pc, int table, bitset &foldedHist, bitset &altFoldedHist);

    Addr getOffset(Addr pc) {
        return (pc & (blockSize - 1)) >> 1;
    }

    void doUpdateHist(const bitset &history, int shamt, bool taken);

    const unsigned numPredictors;

    std::vector<unsigned> tableSizes;
    std::vector<unsigned> tableIndexBits;
    std::vector<bitset> tableIndexMasks;
    // std::vector<uint64_t> tablePcMasks;
    std::vector<unsigned> tableTagBits;
    std::vector<bitset> tableTagMasks;
    std::vector<unsigned> tablePcShifts;
    std::vector<unsigned> histLengths;
    std::vector<FoldedHist> tagFoldedHist;
    std::vector<FoldedHist> altTagFoldedHist;
    std::vector<FoldedHist> indexFoldedHist;

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

    typedef struct TageMeta {
        std::unordered_map<Addr, TagePrediction> preds;
        bitset usefulMask;
        std::vector<FoldedHist> tagFoldedHist;
        std::vector<FoldedHist> altTagFoldedHist;
        std::vector<FoldedHist> indexFoldedHist;
        TageMeta(std::unordered_map<Addr, TagePrediction> preds, bitset usefulMask, std::vector<FoldedHist> tagFoldedHist,
            std::vector<FoldedHist> altTagFoldedHist, std::vector<FoldedHist> indexFoldedHist) :
            preds(preds), usefulMask(usefulMask), tagFoldedHist(tagFoldedHist), altTagFoldedHist(altTagFoldedHist), indexFoldedHist(indexFoldedHist) {}
        TageMeta() {}
        TageMeta(const TageMeta &other) {
            preds = other.preds;
            usefulMask = other.usefulMask;
            tagFoldedHist = other.tagFoldedHist;
            altTagFoldedHist = other.altTagFoldedHist;
            indexFoldedHist = other.indexFoldedHist;
        }
    } TageMeta;

    std::shared_ptr<TageMeta> meta;

public:

    Addr debugPC = 0;
    Addr debugPC2 = 0;
    bool debugFlag = false;

    void recoverFoldedHist(const bitset& history);

    // void checkFoldedHist(const bitset& history);
};
}

}

}

#endif  // __CPU_PRED_BTB_ITTAGE_HH__