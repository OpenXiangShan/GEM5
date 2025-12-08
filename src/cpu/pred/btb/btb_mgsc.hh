#ifndef __CPU_PRED_BTB_MGSC_HH__
#define __CPU_PRED_BTB_MGSC_HH__

#include <cstdint>
#include <deque>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/dynamic_bitset/dynamic_bitset.hpp>

#include "base/sat_counter.hh"
#include "base/types.hh"
#include "cpu/pred/btb/folded_hist.hh"
#include "cpu/pred/btb/stream_struct.hh"
#include "cpu/pred/btb/timed_base_pred.hh"
#include "params/BTBMGSC.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

class BTBMGSC : public TimedBaseBTBPredictor
{
  public:
    typedef BTBMGSCParams Params;

    // Contains the complete prediction result
    struct MgscPrediction
    {
        Addr btb_pc;                      // BTB entry PC
        int total_sum;                    // Total weighted sum
        bool use_mgsc;                    // Whether to use MGSC prediction
        bool taken;                       // Final prediction = (use sc pred) ? (total_sum >= 0) : tage prediction
        bool taken_before_sc;             // Tage prediction (before SC)
        int16_t total_thres;              // Combined threshold
        std::vector<unsigned> bwIndex;    // BW table indices
        std::vector<unsigned> lIndex;     // L table indices
        std::vector<unsigned> iIndex;     // I table indices
        std::vector<unsigned> gIndex;     // G table indices
        std::vector<unsigned> pIndex;     // P table indices
        std::vector<unsigned> biasIndex;  // Bias table indices
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

        MgscPrediction()
            : btb_pc(0),
              total_sum(0),
              use_mgsc(false),
              taken(false),
              taken_before_sc(false),
              total_thres(0),
              bwIndex(0),
              lIndex(0),
              iIndex(0),
              gIndex(0),
              pIndex(0),
              biasIndex(0),
              bw_weight_scale_diff(false),
              l_weight_scale_diff(false),
              i_weight_scale_diff(false),
              g_weight_scale_diff(false),
              p_weight_scale_diff(false),
              bias_weight_scale_diff(false),
              bw_percsum(0),
              l_percsum(0),
              i_percsum(0),
              g_percsum(0),
              p_percsum(0),
              bias_percsum(0)
        {
        }

        MgscPrediction(Addr btb_pc, int total_sum, bool use_mgsc, bool taken, bool taken_before_sc,
                       int16_t total_thres, std::vector<unsigned> bwIndex, std::vector<unsigned> lIndex,
                       std::vector<unsigned> iIndex, std::vector<unsigned> gIndex, std::vector<unsigned> pIndex,
                       std::vector<unsigned> biasIndex, bool bw_weight_scale_diff, bool l_weight_scale_diff,
                       bool i_weight_scale_diff, bool g_weight_scale_diff, bool p_weight_scale_diff,
                       bool bias_weight_scale_diff, int bw_percsum, int l_percsum, int i_percsum, int g_percsum,
                       int p_percsum, int bias_percsum)
            : btb_pc(btb_pc),
              total_sum(total_sum),
              use_mgsc(use_mgsc),
              taken(taken),
              taken_before_sc(taken_before_sc),
              total_thres(total_thres),
              bwIndex(bwIndex),
              lIndex(lIndex),
              iIndex(iIndex),
              gIndex(gIndex),
              pIndex(pIndex),
              biasIndex(biasIndex),
              bw_weight_scale_diff(bw_weight_scale_diff),
              l_weight_scale_diff(l_weight_scale_diff),
              i_weight_scale_diff(i_weight_scale_diff),
              g_weight_scale_diff(g_weight_scale_diff),
              p_weight_scale_diff(p_weight_scale_diff),
              bias_weight_scale_diff(bias_weight_scale_diff),
              bw_percsum(bw_percsum),
              l_percsum(l_percsum),
              i_percsum(i_percsum),
              g_percsum(g_percsum),
              p_percsum(p_percsum),
              bias_percsum(bias_percsum)
        {
        }
    };

  public:
    BTBMGSC(const Params &p);
    ~BTBMGSC();

    void tickStart() override;

    void tick() override;
    // Make predictions for a stream of instructions and record in stage preds
    void putPCHistory(Addr startAddr, const boost::dynamic_bitset<> &history,
                      std::vector<FullBTBPrediction> &stagePreds) override;

    std::shared_ptr<void> getPredictionMeta() override;

    // speculative update all folded history, according history and pred.taken
    void specUpdateHist(const boost::dynamic_bitset<> &history, FullBTBPrediction &pred) override;
    void specUpdatePHist(const boost::dynamic_bitset<> &history, FullBTBPrediction &pred) override;
    void specUpdateBwHist(const boost::dynamic_bitset<> &history, FullBTBPrediction &pred) override;
    void specUpdateIHist(const boost::dynamic_bitset<> &history, FullBTBPrediction &pred) override;
    void specUpdateLHist(const std::vector<boost::dynamic_bitset<>> &history, FullBTBPrediction &pred) override;

    // Recover all folded history after a misprediction, then update all folded history according to history and
    // pred.taken
    void recoverHist(const boost::dynamic_bitset<> &history, const FetchStream &entry, int shamt,
                     bool cond_taken) override;
    void recoverPHist(const boost::dynamic_bitset<> &history, const FetchStream &entry, int shamt,
                      bool cond_taken) override;
    void recoverBwHist(const boost::dynamic_bitset<> &history, const FetchStream &entry, int shamt,
                       bool cond_taken) override;
    void recoverIHist(const boost::dynamic_bitset<> &history, const FetchStream &entry, int shamt,
                      bool cond_taken) override;
    void recoverLHist(const std::vector<boost::dynamic_bitset<>> &history, const FetchStream &entry, int shamt,
                      bool cond_taken) override;

    // Update predictor state based on actual branch outcomes
    void update(const FetchStream &entry) override;

    void commitBranch(const FetchStream &stream, const DynInstPtr &inst) override;

    void setTrace() override;

    // check folded hists after speculative update and recover
    void checkFoldedHist(const boost::dynamic_bitset<> &Ghistory, const boost::dynamic_bitset<> &PHistory,
                         const std::vector<boost::dynamic_bitset<>> &LHistory, const char *when);  // Check GHR folded

    // Calculate MGSC weight index
    Addr getPcIndex(Addr pc, unsigned tableIndexBits);

  private:
    // Utility functions for reducing code duplication
    /**
     * Calculate percsum from a table for a given PC
     */
    int calculatePercsum(const std::vector<std::vector<std::vector<int16_t>>> &table,
                         const std::vector<unsigned> &tableIndices, unsigned numTables, Addr pc);

    /**
     * Find weight in a weight table for a given PC
     */
    int findWeight(const std::vector<int16_t> &weightTable, Addr pc);

    /**
     * Calculate scaled percsum using weight
     */
    int calculateScaledPercsum(int weight, int percsum);

    /**
     * Find threshold in a threshold table for a given PC
     */
    int findThreshold(const std::vector<int16_t> &thresholdTable, Addr pc);

    /**
     * Calculate if weight scale causes prediction difference
     */
    bool calculateWeightScaleDiff(int total_sum, int scale_percsum, int percsum);

    /**
     * Update a prediction table and allocate new entry if needed
     */
    void updatePredTable(std::vector<std::vector<std::vector<int16_t>>> &table,
                         const std::vector<unsigned> &tableIndices, unsigned numTables, Addr pc, bool actual_taken);

    /**
     * Update a weight table and allocate new entry if needed
     */
    void updateWeightTable(std::vector<int16_t> &weightTable, Addr tableIndex, Addr pc, bool weight_scale_diff,
                           bool percsum_matches_actual);

    /**
     * Update a threshold table and allocate new entry if needed
     */
    void updatePCThresholdTable(Addr pc, bool update_direction);

    /**
     * Update the global threshold table and allocate new entry if needed
     */
    void updateGlobalThreshold(Addr pc, bool update_direction);

    // Look up predictions in MGSC tables for a stream of instructions
    void lookupHelper(const Addr &stream_start, const std::vector<BTBEntry> &btbEntries,
                      const std::unordered_map<Addr, TageInfoForMGSC> &tageInfoForMgscs, CondTakens &results);

    // Calculate MGSC history index with folded history
    Addr getHistIndex(Addr pc, unsigned tableIndexBits, uint64_t foldedHist);

    // Calculate MGSC bias index
    Addr getBiasIndex(Addr pc, unsigned tableIndexBits, bool lowbit0, bool lowbit1);

    // Get offset within a block for a given PC
    Addr getOffset(Addr pc) { return (pc & (blockSize - 1)) >> 1; }

    // Update branch history
    template<typename T>
    void doUpdateHist(const boost::dynamic_bitset<> &history, int shamt, bool taken, std::vector<T> &foldedHist,
                      Addr pc = 0, Addr target = 0);

    /*
     * Mix position and table index into two indexes,
     * first index is used to access SRAM, second one is used to choose from the result
     * @param pc the raw program counter
     * @param tableIdx the table index
     * @return a tuple of two unsigned integers representing the two indexes
     */
    std::tuple<unsigned, unsigned> posHash(Addr pc, unsigned tableIdx)
    {
        return {tableIdx, (pc >> (instShiftAmt + 1)) & (numCtrsPerLine - 1)};
    }

    // Helper method to generate prediction for a single BTB entry
    MgscPrediction generateSinglePrediction(const BTBEntry &btb_entry, const Addr &startPC,
                                            const TageInfoForMGSC &tage_info);

    // Helper method to prepare BTB entries for update
    std::vector<BTBEntry> prepareUpdateEntries(const FetchStream &stream);

    void updateSinglePredictor(const BTBEntry &entry, bool actual_taken, const MgscPrediction &pred,
                               const FetchStream &stream);

    /** global backward branch history indexed tables */
    // number of global backward branch history indexed tables
    unsigned bwTableNum;
    // table index width
    unsigned bwTableIdxWidth;
    // global backward branch history length
    std::vector<int> bwHistLen;

    /** First local history indexed tables param*/
    // number of entries for first local histories
    unsigned numEntriesFirstLocalHistories;
    // number of first local history indexed tables
    unsigned lTableNum;
    // table index width
    unsigned lTableIdxWidth;
    // local history lengths for all first local history indexed tables
    std::vector<int> lHistLen;

    /** loop counter indexed tables param*/
    unsigned iTableNum;
    unsigned iTableIdxWidth;
    std::vector<int> iHistLen;

    /** global history indexed table param*/
    unsigned gTableNum;
    unsigned gTableIdxWidth;
    std::vector<int> gHistLen;

    /** path history indexed table param*/
    unsigned pTableNum;
    unsigned pTableIdxWidth;
    std::vector<int> pHistLen;

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

    /*Number of bits for the extra weights*/
    unsigned extraWeightsWidth;
    /*weight table index width*/
    unsigned weightTableIdxWidth;

    unsigned numCtrsPerLine;
    unsigned numCtrsPerLineBits;

    // Folded history for index calculation
    std::vector<GlobalBwFoldedHist> indexBwFoldedHist;
    std::vector<std::vector<LocalFoldedHist>> indexLFoldedHist;
    std::vector<ImliFoldedHist> indexIFoldedHist;
    std::vector<GlobalFoldedHist> indexGFoldedHist;
    std::vector<PathFoldedHist> indexPFoldedHist;

    // The actual MGSC prediction tables (table x index x line)
    std::vector<std::vector<std::vector<int16_t>>> bwTable;
    // The actual MGSC prediction tables (index x line)
    std::vector<int16_t> bwWeightTable;

    // The actual MGSC prediction tables (table x index x line)
    std::vector<std::vector<std::vector<int16_t>>> lTable;
    // The actual MGSC prediction tables (index x line)
    std::vector<int16_t> lWeightTable;

    // The actual MGSC prediction tables (table x index x line)
    std::vector<std::vector<std::vector<int16_t>>> iTable;
    // The actual MGSC prediction tables (index x line)
    std::vector<int16_t> iWeightTable;

    // The actual MGSC prediction tables (table x index x line)
    std::vector<std::vector<std::vector<int16_t>>> gTable;
    // The actual MGSC prediction tables (index x line)
    std::vector<int16_t> gWeightTable;

    // The actual MGSC prediction tables (table x index x line)
    std::vector<std::vector<std::vector<int16_t>>> pTable;
    // The actual MGSC prediction tables (index x line)
    std::vector<int16_t> pWeightTable;

    // The actual MGSC prediction tables (table x index x line)
    std::vector<std::vector<std::vector<int16_t>>> biasTable;
    // The actual MGSC prediction tables (index x line)
    std::vector<int16_t> biasWeightTable;

    // thres table
    std::vector<int16_t> pUpdateThreshold;  // pc-indexed threshold table
    int16_t updateThreshold;                // global threshold table

    // Instruction shift amount
    unsigned instShiftAmt{1};

    // Update counter with saturation (template for all integer types)
    template<typename T>
    void updateCounter(bool taken, unsigned width, T &counter);

    // Increment counter with saturation (template for all integer types)
    template<typename T>
    bool satIncrement(T max, T &counter);

    // Decrement counter with saturation (template for all integer types)
    template<typename T>
    bool satDecrement(T min, T &counter);

    // Cache for MGSC indices
    std::vector<unsigned> bwIndex;
    std::vector<unsigned> lIndex;
    std::vector<unsigned> iIndex;
    std::vector<unsigned> gIndex;
    std::vector<unsigned> pIndex;
    std::vector<unsigned> biasIndex;

    // Statistics for MGSC predictor
    struct MgscStats : public statistics::Group
    {
        statistics::Scalar scCorrectTageWrong;
        statistics::Scalar scWrongTageCorrect;
        statistics::Scalar scCorrectTageCorrect;
        statistics::Scalar scWrongTageWrong;
        statistics::Scalar scUsed;
        statistics::Scalar scNotUsed;

        statistics::Scalar predHit;
        statistics::Scalar predMiss;
        statistics::Scalar scPredCorrect;
        statistics::Scalar scPredWrong;
        statistics::Scalar scPredMissTaken;
        statistics::Scalar scPredMissNotTaken;
        statistics::Scalar scPredCorrectTageWrong;
        statistics::Scalar scPredWrongTageCorrect;

        MgscStats(statistics::Group *parent);
    };

    MgscStats mgscStats;

    TraceManager *mgscMissTrace;

  public:
    // Recover folded history after misprediction
    void recoverFoldedHist(const boost::dynamic_bitset<> &history);
    unsigned getNumEntriesFirstLocalHistories() { return numEntriesFirstLocalHistories; };

  private:
    // Metadata for MGSC predictions
    typedef struct MgscMeta
    {
        std::unordered_map<Addr, MgscPrediction> preds;
        std::vector<GlobalBwFoldedHist> indexBwFoldedHist;
        std::vector<std::vector<LocalFoldedHist>> indexLFoldedHist;
        std::vector<ImliFoldedHist> indexIFoldedHist;
        std::vector<GlobalFoldedHist> indexGFoldedHist;
        std::vector<PathFoldedHist> indexPFoldedHist;
        MgscMeta(std::unordered_map<Addr, MgscPrediction> preds, std::vector<GlobalBwFoldedHist> indexBwFoldedHist,
                 std::vector<std::vector<LocalFoldedHist>> indexLFoldedHist,
                 std::vector<ImliFoldedHist> indexIFoldedHist, std::vector<GlobalFoldedHist> indexGFoldedHist,
                 std::vector<PathFoldedHist> indexPFoldedHist)
            : preds(preds),
              indexBwFoldedHist(indexBwFoldedHist),
              indexLFoldedHist(indexLFoldedHist),
              indexIFoldedHist(indexIFoldedHist),
              indexGFoldedHist(indexGFoldedHist),
              indexPFoldedHist(indexPFoldedHist)
        {
        }
        MgscMeta() {}
        MgscMeta(const MgscMeta &other)
        {
            preds = other.preds;
            indexBwFoldedHist = other.indexBwFoldedHist;
            indexLFoldedHist = other.indexLFoldedHist;
            indexIFoldedHist = other.indexIFoldedHist;
            indexGFoldedHist = other.indexGFoldedHist;
            indexPFoldedHist = other.indexPFoldedHist;
        }
    } MgscMeta;

    std::shared_ptr<MgscMeta> meta;
};
}

}

}

#endif  // __CPU_PRED_BTB_TAGE_HH__
