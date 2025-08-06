#ifndef __CPU_PRED_BTB_MGSC_HH__
#define __CPU_PRED_BTB_MGSC_HH__

#include <deque>
#include <map>
#include <vector>
#include <utility>

#include "base/sat_counter.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/btb/folded_hist.hh"
#include "cpu/pred/btb/stream_struct.hh"
#include "cpu/pred/btb/timed_base_pred.hh"
#include "debug/DecoupleBP.hh"
#include "params/BTBMGSC.hh"
#include "sim/sim_object.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

class BTBMGSC : public TimedBaseBTBPredictor
{
    using defer = std::shared_ptr<void>;
    using bitset = boost::dynamic_bitset<>;
  public:
    typedef BTBMGSCParams Params;

    // Represents a single entry in the MGSC prediction table
    struct MgscEntry
    {
        public:
            bool valid;
            short counter;  // Prediction counter (-32 to 31), 6bits
            Addr pc;        // branch pc, like branch position, for btb entry pc check
            unsigned lruCounter;

            MgscEntry() : valid(false), counter(0), pc(0), lruCounter(0) {}

            MgscEntry(bool valid, short counter, Addr pc, unsigned lruCounter) :
            valid(valid), counter(counter), pc(pc), lruCounter(lruCounter) {}
            bool taken() const {
                return counter >= 0;
            }
    };

    // Represents a single entry in the MGSC weight table
    struct MgscWeightEntry
    {
        public:
            bool valid;
            Addr pc;           // btb entry pc, same as mgsc entry pc
            short counter;  // weight counter (-32 to 31), 6bits
            unsigned lruCounter;

            MgscWeightEntry() : valid(false), pc(0), counter(0), lruCounter(0) {}

            MgscWeightEntry(bool valid, Addr pc, short counter, unsigned lruCounter) :
                            valid(valid), pc(pc), counter(counter), lruCounter(lruCounter) {}
    };

    struct MgscThresEntry
    {
        public:
            bool valid;
            Addr pc;           // btb entry pc, same as mgsc entry pc
            unsigned counter;         // thres counter (0 to 255), 6bits
            unsigned lruCounter;

            MgscThresEntry() : valid(false), pc(0), counter(0), lruCounter(0) {}

            MgscThresEntry(bool valid, Addr pc, unsigned counter, unsigned lruCounter) :
                            valid(valid), pc(pc), counter(counter), lruCounter(lruCounter) {}
    };

    // Contains the complete prediction result
    struct MgscPrediction
    {
        Addr btb_pc;                    // BTB entry PC
        int total_sum;                  // Total weighted sum
        bool use_mgsc;                  // Whether to use MGSC prediction
        bool taken;                     // Final prediction = (use sc pred) ? (total_sum >= 0) : tage prediction
        bool taken_before_sc;           // Tage prediction (before SC)
        unsigned total_thres;           // Combined threshold
        std::vector<Addr> bwIndex;      // BW table indices
        std::vector<Addr> lIndex;       // L table indices
        std::vector<Addr> iIndex;       // I table indices
        std::vector<Addr> gIndex;       // G table indices
        std::vector<Addr> pIndex;       // P table indices
        std::vector<Addr> biasIndex;    // Bias table indices
        // Weight scale difference flags and percsum values
            bool bw_weight_scale_diff;
            bool l_weight_scale_diff;
            bool i_weight_scale_diff;
            bool g_weight_scale_diff;
            bool p_weight_scale_diff;
            bool bias_weight_scale_diff;
            int bw_percsum;
            int l_percsum;
            int i_percsum;
            int g_percsum;
            int p_percsum;
            int bias_percsum;

            MgscPrediction() : btb_pc(0), total_sum(0), use_mgsc(false), taken(false),
                               taken_before_sc(false), total_thres(0), bwIndex(0),
                               lIndex(0), iIndex(0), gIndex(0), pIndex(0), biasIndex(0), bw_weight_scale_diff(false),
                               l_weight_scale_diff(false), i_weight_scale_diff(false),
                               g_weight_scale_diff(false), p_weight_scale_diff(false), bias_weight_scale_diff(false),
                               bw_percsum(0), l_percsum(0), i_percsum(0), g_percsum(0), p_percsum(0), bias_percsum(0){}

            MgscPrediction(Addr btb_pc, int total_sum, bool use_mgsc, bool taken,
                           bool taken_before_sc, unsigned total_thres,
                           std::vector<Addr> bwIndex, std::vector<Addr> lIndex,
                           std::vector<Addr> iIndex, std::vector<Addr> gIndex,
                           std::vector<Addr> pIndex, std::vector<Addr> biasIndex,
                           bool bw_weight_scale_diff, bool l_weight_scale_diff, bool i_weight_scale_diff,
                           bool g_weight_scale_diff, bool p_weight_scale_diff,
                           bool bias_weight_scale_diff, int bw_percsum,
                           int l_percsum, int i_percsum, int g_percsum, int p_percsum, int bias_percsum) :
                            btb_pc(btb_pc), total_sum(total_sum), use_mgsc(use_mgsc),
                            taken(taken), taken_before_sc(taken_before_sc),
                            total_thres(total_thres), bwIndex(bwIndex),
                            lIndex(lIndex), iIndex(iIndex), gIndex(gIndex), pIndex(pIndex),
                            biasIndex(biasIndex), bw_weight_scale_diff(bw_weight_scale_diff),
                            l_weight_scale_diff(l_weight_scale_diff), i_weight_scale_diff(i_weight_scale_diff),
                            g_weight_scale_diff(g_weight_scale_diff), p_weight_scale_diff(p_weight_scale_diff),
                            bias_weight_scale_diff(bias_weight_scale_diff),
                            bw_percsum(bw_percsum), l_percsum(l_percsum), i_percsum(i_percsum), g_percsum(g_percsum),
                            p_percsum(p_percsum), bias_percsum(bias_percsum){}
    };

  public:
    BTBMGSC(const Params& p);
    ~BTBMGSC();

    void tickStart() override;

    void tick() override;
    // Make predictions for a stream of instructions and record in stage preds
    void putPCHistory(Addr startAddr,
                      const boost::dynamic_bitset<> &history,
                      std::vector<FullBTBPrediction> &stagePreds) override;

    std::shared_ptr<void> getPredictionMeta() override;

    // speculative update all folded history, according history and pred.taken
    void specUpdateHist(const boost::dynamic_bitset<> &history, FullBTBPrediction &pred) override;
    void specUpdatePHist(const boost::dynamic_bitset<> &history, FullBTBPrediction &pred) override;
    void specUpdateBwHist(const boost::dynamic_bitset<> &history, FullBTBPrediction &pred) override;
    void specUpdateIHist(const boost::dynamic_bitset<> &history, FullBTBPrediction &pred) override;
    void specUpdateLHist(const std::vector<boost::dynamic_bitset<>> &history, FullBTBPrediction &pred) override;

    // Recover all folded history after a misprediction, then update all folded history according to history and pred.taken
    void recoverHist(const boost::dynamic_bitset<> &history, const FetchStream &entry, int shamt, bool cond_taken) override;
    void recoverPHist(const boost::dynamic_bitset<> &history, const FetchStream &entry, int shamt, bool cond_taken) override;
    void recoverBwHist(const boost::dynamic_bitset<> &history, const FetchStream &entry, int shamt, bool cond_taken) override;
    void recoverIHist(const boost::dynamic_bitset<> &history, const FetchStream &entry, int shamt, bool cond_taken) override;
    void recoverLHist(const std::vector<boost::dynamic_bitset<>> &history, const FetchStream &entry, int shamt, bool cond_taken) override;

    // Update predictor state based on actual branch outcomes
    void update(const FetchStream &entry) override;

    void commitBranch(const FetchStream &stream, const DynInstPtr &inst) override;

    void setTrace() override;

    // check folded hists after speculative update and recover
    void checkFoldedHist(const bitset &history, const char *when);

    // Calculate MGSC weight index
    Addr getPcIndex(Addr pc, unsigned tableIndexBits);

    // Utility functions for reducing code duplication
    /**
     * Calculate percsum from a table for a given PC
     */
    int calculatePercsum(const std::vector<std::vector<std::vector<MgscEntry>>> &table,
                         const std::vector<Addr> &tableIndices,
                         unsigned numTables,
                         Addr pc);

    /**
     * Find weight in a weight table for a given PC
     */
    int findWeight(const std::vector<std::vector<MgscWeightEntry>> &weightTable,
                   Addr tableIndex,
                   Addr pc);

    /**
     * Calculate scaled percsum using weight
     */
    int calculateScaledPercsum(int weight, int percsum);

    /**
     * Find threshold in a threshold table for a given PC
     */
    int findThreshold(const std::vector<std::vector<MgscThresEntry>> &thresholdTable,
                      Addr tableIndex,
                      Addr pc,
                      int defaultValue);

    /**
     * Calculate if weight scale causes prediction difference
     */
    bool calculateWeightScaleDiff(int total_sum, int scale_percsum, int percsum);

    /**
     * Update a prediction table and allocate new entry if needed
     */
    void updateAndAllocatePredTable(std::vector<std::vector<std::vector<MgscEntry>>> &table,
                                   const std::vector<Addr> &tableIndices,
                                   unsigned numTables,
                                   Addr pc,
                                   bool actual_taken);

    /**
     * Update a weight table and allocate new entry if needed
     */
    void updateAndAllocateWeightTable(std::vector<std::vector<MgscWeightEntry>> &weightTable,
                                    Addr tableIndex,
                                    Addr pc,
                                    bool weight_scale_diff,
                                    bool percsum_matches_actual);

    /**
     * Update a threshold table and allocate new entry if needed
     */
    void updatePCThresholdTable(Addr tableIndex,
                                Addr pc,
                                bool update_condition,
                                bool update_direction);

    /**
     * Update the global threshold table and allocate new entry if needed
     */
    void updateGlobalThreshold(Addr pc,
                             bool update_condition,
                             bool update_direction);

  private:

    // Look up predictions in MGSC tables for a stream of instructions
    void lookupHelper(const Addr &stream_start, const std::vector<BTBEntry> &btbEntries,
                                      const std::unordered_map<Addr, TageInfoForMGSC> &tageInfoForMgscs, CondTakens& results);

    // Calculate MGSC history index with folded history
    Addr getHistIndex(Addr pc, unsigned tableIndexBits, uint64_t foldedHist);

    // Calculate MGSC bias index
    Addr getBiasIndex(Addr pc, unsigned tableIndexBits, bool lowbit0, bool lowbit1);

    // Get offset within a block for a given PC
    Addr getOffset(Addr pc) {
        return (pc & (blockSize - 1)) >> 1;
    }

    // Update branch history
    void doUpdateHist(const boost::dynamic_bitset<> &history, int shamt,
        bool taken, std::vector<FoldedHist> &foldedHist, Addr pc=0);

    /** global backward branch history indexed tables */
    // number of global backward branch history indexed tables
    unsigned bwTableNum;
    // table index width
    unsigned bwTableIdxWidth;
    // global backward branch history length
    std::vector<int> bwHistLen;
    int bwWeightInitValue;

    /** First local history indexed tables param*/
    // number of entries for first local histories
    unsigned numEntriesFirstLocalHistories;
    // number of first local history indexed tables
    unsigned lTableNum;
    // table index width
    unsigned lTableIdxWidth;
    // local history lengths for all first local history indexed tables
    std::vector<int> lHistLen;
    int lWeightInitValue;

    /** loop counter indexed tables param*/
    unsigned iTableNum;
    unsigned iTableIdxWidth;
    std::vector<int> iHistLen;
    int iWeightInitValue;

    /** global history indexed table param*/
    unsigned gTableNum;
    unsigned gTableIdxWidth;
    std::vector<int> gHistLen;
    int gWeightInitValue;

    /** path history indexed table param*/
    unsigned pTableNum;
    unsigned pTableIdxWidth;
    std::vector<int> pHistLen;
    int pWeightInitValue;

    /** bias table param*/
    unsigned biasTableNum;
    unsigned biasTableIdxWidth;

    /*Statistical corrector counters width*/
    unsigned scCountersWidth;

    /*Log size of update threshold counters tables*/
    unsigned thresholdTablelogSize;
    /*Number of bits for the update threshold counter*/
    unsigned updateThresholdWidth;
    /*Number of bits for the pUpdate threshold counters*/
    unsigned pUpdateThresholdWidth;
    /*Initial pUpdate threshold counter value*/
    unsigned initialUpdateThresholdValue;

    /*Number of bits for the extra weights*/
    unsigned extraWeightsWidth;
    /*weight table index width*/
    unsigned weightTableIdxWidth;

    // Number of ways for set associative design
    const unsigned numWays;

    // Whether MGSC is enabled
    bool enableMGSC;

    // Folded history for index calculation
    std::vector<FoldedHist> indexBwFoldedHist;
    std::vector<std::vector<FoldedHist>> indexLFoldedHist;
    std::vector<FoldedHist> indexIFoldedHist;
    std::vector<FoldedHist> indexGFoldedHist;
    std::vector<FoldedHist> indexPFoldedHist;

    // The actual MGSC prediction tables (table x index x way)
    std::vector<std::vector<std::vector<MgscEntry>>> bwTable;
    // The actual MGSC prediction tables (index x way)
    std::vector<std::vector<MgscWeightEntry>> bwWeightTable;

    // The actual MGSC prediction tables (table x index x way)
    std::vector<std::vector<std::vector<MgscEntry>>> lTable;
    // The actual MGSC prediction tables (index x way)
    std::vector<std::vector<MgscWeightEntry>> lWeightTable;

    // The actual MGSC prediction tables (table x index x way)
    std::vector<std::vector<std::vector<MgscEntry>>> iTable;
    // The actual MGSC prediction tables (index x way)
    std::vector<std::vector<MgscWeightEntry>> iWeightTable;

    // The actual MGSC prediction tables (table x index x way)
    std::vector<std::vector<std::vector<MgscEntry>>> gTable;
    // The actual MGSC prediction tables (index x way)
    std::vector<std::vector<MgscWeightEntry>> gWeightTable;

    // The actual MGSC prediction tables (table x index x way)
    std::vector<std::vector<std::vector<MgscEntry>>> pTable;
    // The actual MGSC prediction tables (index x way)
    std::vector<std::vector<MgscWeightEntry>> pWeightTable;

    // The actual MGSC prediction tables (table x index x way)
    std::vector<std::vector<std::vector<MgscEntry>>> biasTable;
    // The actual MGSC prediction tables (index x way)
    std::vector<std::vector<MgscWeightEntry>> biasWeightTable;

    // thres table
    std::vector<std::vector<MgscThresEntry>> pUpdateThreshold;  // pc-indexed threshold table
    std::vector<MgscThresEntry> updateThreshold;  // global threshold table


    // Debug flag
    bool debugFlagOn{false};

    // Instruction shift amount
    unsigned instShiftAmt {1};

    // Update signed counter with saturation
    void updateCounter(bool taken, unsigned width, short &counter);

    // Update unsigned counter with saturation
    void updateCounter(bool taken, unsigned width, unsigned &counter);

    // Increment counter with saturation
    bool satIncrement(int max, short &counter);
    bool satIncrement(int max, unsigned &counter);

    // Decrement counter with saturation
    bool satDecrement(int min, short &counter);
    bool satDecrement(int min, unsigned &counter);

    // Cache for MGSC indices
    std::vector<Addr> bwIndex;
    std::vector<Addr> lIndex;
    std::vector<Addr> iIndex;
    std::vector<Addr> gIndex;
    std::vector<Addr> pIndex;
    std::vector<Addr> biasIndex;

    // Statistics for MGSC predictor
    struct MgscStats : public statistics::Group {
        statistics::Scalar scCorrectTageWrong;
        statistics::Scalar scWrongTageCorrect;
        statistics::Scalar scCorrectTageCorrect;
        statistics::Scalar scWrongTageWrong;
        statistics::Scalar scUsed;
        statistics::Scalar scNotUsed;

        MgscStats(statistics::Group* parent);
    } ;

    MgscStats mgscStats;

    TraceManager *mgscMissTrace;

public:

    // Recover folded history after misprediction
    void recoverFoldedHist(const bitset& history);
    unsigned getNumEntriesFirstLocalHistories(){
        return numEntriesFirstLocalHistories;
    };

public:

private:
    // Metadata for MGSC predictions
    typedef struct MgscMeta {
        std::unordered_map<Addr, MgscPrediction> preds;
        std::vector<FoldedHist> indexBwFoldedHist;
        std::vector<std::vector<FoldedHist>> indexLFoldedHist;
        std::vector<FoldedHist> indexIFoldedHist;
        std::vector<FoldedHist> indexGFoldedHist;
        std::vector<FoldedHist> indexPFoldedHist;
        MgscMeta(std::unordered_map<Addr, MgscPrediction> preds, std::vector<FoldedHist> indexBwFoldedHist,
                 std::vector<std::vector<FoldedHist>> indexLFoldedHist, std::vector<FoldedHist> indexIFoldedHist,
                 std::vector<FoldedHist> indexGFoldedHist, std::vector<FoldedHist> indexPFoldedHist) :
            preds(preds), indexBwFoldedHist(indexBwFoldedHist), indexLFoldedHist(indexLFoldedHist),
            indexIFoldedHist(indexIFoldedHist), indexGFoldedHist(indexGFoldedHist), indexPFoldedHist(indexPFoldedHist) {}
        MgscMeta() {}
        MgscMeta(const MgscMeta &other) {
            preds = other.preds;
            indexBwFoldedHist = other.indexBwFoldedHist;
            indexLFoldedHist = other.indexLFoldedHist;
            indexIFoldedHist = other.indexIFoldedHist;
            indexGFoldedHist = other.indexGFoldedHist;
            indexPFoldedHist = other.indexPFoldedHist;
        }
    } MgscMeta;

private:
    // Helper method to generate prediction for a single BTB entry
    bool tagMatch(Addr pc_a, Addr pc_b, unsigned matchBits);
    MgscPrediction generateSinglePrediction(const BTBEntry &btb_entry,
                                           const Addr &startPC,
                                           const TageInfoForMGSC &tage_info);

    // Helper method to prepare BTB entries for update
    std::vector<BTBEntry> prepareUpdateEntries(const FetchStream &stream);

    void updateAndAllocateSinglePredictor(const BTBEntry &entry,
                                          bool actual_taken,
                                          const MgscPrediction &pred,
                                          const FetchStream &stream);
    template <typename T>
    void updateLRU(std::vector<std::vector<T>> &table, Addr index, unsigned way);

    template <typename T>
    void updateLRU(std::vector<T> &table, unsigned way);

    template <typename T>
    unsigned getLRUVictim(std::vector<std::vector<T>> &table, Addr index);

    template <typename T>
    unsigned getLRUVictim(std::vector<T> &table);

    std::shared_ptr<MgscMeta> meta;
};
}

}

}

#endif  // __CPU_PRED_BTB_TAGE_HH__
