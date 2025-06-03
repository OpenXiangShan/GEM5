#ifndef __CPU_PRED_BTB_JUMP_AHEAD_PREDICTOR_HH__
#define __CPU_PRED_BTB_JUMP_AHEAD_PREDICTOR_HH__

#include <array>
#include <queue>
#include <stack>
#include <utility> 
#include <vector>

#include "cpu/pred/btb/stream_struct.hh"

#ifdef UNIT_TEST
#include "cpu/pred/btb/test/test_dprintf.hh"

#else
#include "debug/JumpAheadPredictor.hh"

#endif


namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

/**
 * Jump Ahead Predictor (JAP) attempts to identify patterns where several
 * consecutive blocks have no branch prediction, followed by a predictable block.
 * When such a pattern is detected with confidence, it allows the fetch unit to
 * "jump ahead" to the predictable block, bypassing the unpredictable blocks.
 */
class JumpAheadPredictor
{
  public:
    /**
     * Information structure to track sequences of unpredicted blocks
     * and identify patterns for jump-ahead prediction
     */
    typedef struct JAInfo
    {
        int noPredBlockCount;               // Number of consecutive blocks without prediction
        Addr firstNoPredBlockStart;         // Address of the first block without prediction
        Addr recentPredictedBlockStart;     // Address of most recent predicted block
        BTBEntry recentPredictedBTBEntry;   // BTB entry of most recent predicted block

        // Constructor - initialize with default values
        JAInfo() : noPredBlockCount(0), firstNoPredBlockStart(0), recentPredictedBlockStart(0) {}

        const std::string &name() {
            static const std::string default_name("JumpAheadInfo");
            return default_name;
        }

        /**
         * Debug output for JAInfo state
         * @param thisPredictedBlockStart Address of current predicted block
         */
        void dump(Addr thisPredictedBlockStart) {
            DPRINTF(JumpAheadPredictor, "npBlockCount: %d, 1stNpBlockStart: %#lx, recentPBlockStart: %#lx, thisPBlockStart: %#lx\n",
                noPredBlockCount, firstNoPredBlockStart, recentPredictedBlockStart, thisPredictedBlockStart);
        }

        /**
         * Record a predicted block and reset the no-prediction counter
         * @param start Starting address of the predicted block
         * @param entry BTB entry for the predicted block
         */
        void setPredictedBlock(Addr start, BTBEntry entry) {
            if (noPredBlockCount > 1) {
                dump(start);
            }
            recentPredictedBlockStart = start;
            recentPredictedBTBEntry = entry;
            // reset noPredBlockCount
            noPredBlockCount = 0;
        }

        /**
         * Increment counter for blocks without prediction
         * @param start Starting address of the unpredicted block
         */
        void incrementNoPredBlockCount(Addr start) {
            noPredBlockCount++;
            if (noPredBlockCount == 1) {
                firstNoPredBlockStart = start;
            }
        }
    } JAInfo;

    // Cache organization parameters
    unsigned tagSize;     // Size of tag in bits
    Addr tagMask;         // Mask for extracting tag bits

    unsigned numSets;     // Number of sets in the JAP cache
    Addr idxMask;         // Mask for extracting index bits
    unsigned numWays;     // Number of ways per set

    // Prediction confidence parameters
    unsigned maxConf = 7;          // Maximum confidence value
    int minNoPredBlockNum = 2;     // Minimum number of no-prediction blocks to trigger JAP

    bool enableDB;               // Debug flag
    int blockSize = 32;          // Block size for address alignment

    // Storage structure for jump-ahead entries indexed by set, use tag as key(multi-way in set)
    std::vector<std::unordered_map<Addr, JAEntry>> jaStorage;

    /**
     * Extract set index from PC
     * @param pc Program counter
     * @return Set index
     */
    int getIndex(Addr pc) {return (pc >> 1) & idxMask;}

    /**
     * Extract tag from PC
     * @param pc Program counter
     * @return Tag bits
     */
    Addr getTag(Addr pc) {return (pc >> (1+ceilLog2(numSets))) & tagMask;}

    /**
     * Look up an entry in the Jump Ahead Predictor
     * @param pc Program counter to look up
     * @return Tuple containing: (hit, confidence_met, entry, target_address)
     */
    std::tuple<bool, bool, JAEntry, Addr> lookup(Addr pc) {
      auto idx = getIndex(pc);
      auto tag = getTag(pc);
      DPRINTF(JumpAheadPredictor, "lookup: pc: %#lx, index: %d, tag %#lx\n", pc, idx, tag);
      auto &set = jaStorage[idx];
      auto it = set.find(tag);
      if (it != set.end()) { // hit
        int conf = it->second.conf;
        Addr target = 0;
        if (conf == maxConf) { // confidence met
          target = it->second.getJumpTarget(pc, blockSize); // get jump target
        }
        DPRINTF(JumpAheadPredictor, "found jumpAheadBlockNum: %d, conf: %d, shouldJumpTo: %#lx\n",
          it->second.jumpAheadBlockNum, it->second.conf, target);
        // return hit, confidence met, entry, target
        return std::make_tuple(true, conf == maxConf, it->second, target);
      }
      return std::make_tuple(false, false, JAEntry(), 0);
    }

    /**
     * Invalidate a JAP entry's confidence when a control squash occurs
     * @param startPC The starting PC of the block to invalidate
     */
    void invalidate(Addr startPC) {
      DPRINTF(JumpAheadPredictor, "invalidate: pc: %#lx\n", startPC);
      auto idx = getIndex(startPC);
      auto tag = getTag(startPC);
      auto &set = jaStorage[idx];
      auto it = set.find(tag);
      if (it != set.end()) { // hit
        it->second.conf = 0; // invalidate confidence
      }
    }

    /**
     * Update JAP prediction based on observed pattern of unpredicted blocks
     *
     * When we observe a sequence of unpredicted blocks followed by a predicted block,
     * either update an existing entry or create a new one.
     *
     * @param info Information about the sequence of blocks
     * @param nextPredictedBlockToJumpTo Address of the next predicted block
     */
    void tryUpdate(JAInfo info, Addr nextPredictedBlockToJumpTo) {
      if (info.noPredBlockCount >= minNoPredBlockNum) {
        auto pc = info.firstNoPredBlockStart;
        auto idx = getIndex(pc);
        auto tag = getTag(pc);
        auto &set = jaStorage[idx];
        auto it = set.find(tag);
        DPRINTF(JumpAheadPredictor, "tryUpdate: pc %#lx, idx: %d, tag: %#lx, noPredBlockCount: %d\n",
          pc, idx, tag, info.noPredBlockCount);
        if (it != set.end()) {
          // Entry exists - check if block count matches
          if (it->second.jumpAheadBlockNum != info.noPredBlockCount) {
            // Pattern has changed - penalize confidence
            it->second.jumpAheadBlockNum = info.noPredBlockCount;
            it->second.conf -= 4;
            if (it->second.conf < 0) {
              it->second.conf = 0;
            }
          } else {
            // Pattern confirmed - increase confidence
            if (it->second.conf < maxConf) {
              it->second.conf++;
            }
          }
          DPRINTF(JumpAheadPredictor, "found, update jumpAheadBlockNum to %d, conf to %d\n", it->second.jumpAheadBlockNum, it->second.conf);
        } else {
          // New pattern identified - create new entry
          DPRINTF(JumpAheadPredictor, "not found, insert new entry of block num %d\n", info.noPredBlockCount);
          JAEntry entry;
          entry.jumpAheadBlockNum = info.noPredBlockCount;
          entry.conf = 0;
          set[tag] = entry;
        }
      }
    }

    /**
     * Constructor with parameters
     * @param sets Number of sets in the JAP cache
     * @param ways Number of ways per set
     */
    JumpAheadPredictor(unsigned sets, unsigned ways) {
      numSets = sets;
      numWays = ways;
      idxMask = (1 << ceilLog2(numSets)) - 1;
      jaStorage.resize(numSets);
      for (unsigned i = 0; i < numWays; i++) {
        for (auto set : jaStorage) {
          set[0xffffff-i];
        }
        // commitJaStorage[0xfffffef-i];
      }
      //       VaddrBits   instOffsetBits  log2Ceil(PredictWidth)
      tagSize = 39 - 1 - ceilLog2(numSets);
      tagMask = (1ULL << tagSize) - 1;
      DPRINTF(JumpAheadPredictor, "JumpAheadPredictor: sets: %d, ways: %d, tagSize: %d, tagMask: %#lx, idxMask: %#lx\n",
        numSets, numWays, tagSize, tagMask, idxMask);
    }

    /** Default constructor */
    JumpAheadPredictor() : JumpAheadPredictor(64, 4) {}
};

}  // namespace btb_pred
}  // namespace branch_prediction
}  // namespace gem5

#endif  // __CPU_PRED_BTB_JUMP_AHEAD_PREDICTOR_HH__
