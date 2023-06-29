#ifndef __CPU_PRED_FTB_JUMP_AHEAD_PREDICTOR_HH__
#define __CPU_PRED_FTB_JUMP_AHEAD_PREDICTOR_HH__

#include <array>
#include <queue>
#include <stack>
#include <utility> 
#include <vector>

#include "cpu/pred/bpred_unit.hh"
#include "cpu/pred/ftb/stream_struct.hh"
#include "debug/JumpAheadPredictor.hh"


namespace gem5
{

namespace branch_prediction
{

namespace ftb_pred
{

class JumpAheadPredictor
{
  public:

    typedef struct JAInfo {
        int noPredBlockCount;
        Addr firstNoPredBlockStart;
        Addr recentPredictedBlockStart;
        FTBEntry recentPredictedFTBEntry;
        JAInfo() : noPredBlockCount(0), firstNoPredBlockStart(0), recentPredictedBlockStart(0) {}

        const std::string &name() {
            static const std::string default_name("JumpAheadInfo");
            return default_name;
        }
        void dump(Addr thisPredictedBlockStart) {
            DPRINTF(JumpAheadPredictor, "npBlockCount: %d, 1stNpBlockStart: %#lx, recentPBlockStart: %#lx, thisPBlockStart: %#lx\n",
                noPredBlockCount, firstNoPredBlockStart, recentPredictedBlockStart, thisPredictedBlockStart);
        }
        void setPredictedBlock(Addr start, FTBEntry entry) {
            if (noPredBlockCount > 1) {
                dump(start);
            }
            recentPredictedBlockStart = start;
            recentPredictedFTBEntry = entry;
            // reset noPredBlockCount
            noPredBlockCount = 0;
        }
        void incrementNoPredBlockCount(Addr start) {
            noPredBlockCount++;
            if (noPredBlockCount == 1) {
                firstNoPredBlockStart = start;
            }
        }
    } JAInfo;
  
    // index using start pc of block

    unsigned tagSize;
    Addr tagMask;

    unsigned numSets;
    Addr idxMask;
    unsigned numWays;

    unsigned maxConf = 7;
    int minNoPredBlockNum = 2;
    
    int blockSize = 32; // TODO: parameterize



    std::vector<std::map<Addr, JAEntry>> jaStorage;
    // std::map<Addr, JAEntry> commitJaStorage;


    

    // TODO: replacement policy

    int getIndex(Addr pc) {return (pc >> 1) & idxMask;}

    Addr getTag(Addr pc) {return (pc >> (1+ceilLog2(numSets))) & tagMask;}

    // hit, conf, entry, target
    std::tuple<bool, bool, JAEntry, Addr> lookup(Addr pc) {
      auto idx = getIndex(pc);
      auto tag = getTag(pc);
      DPRINTF(JumpAheadPredictor, "lookup: pc: %#lx, index: %d, tag %#lx\n", pc, idx, tag);
      auto &set = jaStorage[idx];
      auto it = set.find(tag);
      if (it != set.end()) {
        int conf = it->second.conf;
        Addr target = 0;
        if (conf == maxConf) {
          target = it->second.getJumpTarget(pc, blockSize);
        }
        DPRINTF(JumpAheadPredictor, "found jumpAheadBlockNum: %d, conf: %d, shouldJumpTo: %#lx\n",
          it->second.jumpAheadBlockNum, it->second.conf, target);
        return std::make_tuple(true, conf == maxConf, it->second, target);
      }
      return std::make_tuple(false, false, JAEntry(), 0);
    }

    void invalidate(Addr startPC) {
      DPRINTF(JumpAheadPredictor, "invalidate: pc: %#lx\n", startPC);
      auto idx = getIndex(startPC);
      auto tag = getTag(startPC);
      auto &set = jaStorage[idx];
      auto it = set.find(tag);
      if (it != set.end()) {
        it->second.conf = 0;
      }
    }

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
          if (it->second.jumpAheadBlockNum != info.noPredBlockCount) {
            it->second.jumpAheadBlockNum = info.noPredBlockCount;
            it->second.conf -= 4;
            if (it->second.conf < 0) {
              it->second.conf = 0;
            }
          } else {
            if (it->second.conf < maxConf) {
              it->second.conf++;
            }
          }
          DPRINTF(JumpAheadPredictor, "found, update jumpAheadBlockNum to %d, conf to %d\n", it->second.jumpAheadBlockNum, it->second.conf);
        } else {
          DPRINTF(JumpAheadPredictor, "not found, insert new entry of block num %d\n", info.noPredBlockCount);
          JAEntry entry;
          entry.jumpAheadBlockNum = info.noPredBlockCount;
          entry.conf = 0;
          set[tag] = entry;
        }
      }
    }

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

    JumpAheadPredictor() : JumpAheadPredictor(64, 4) {}
};

}  // namespace ftb_pred
}  // namespace branch_prediction
}  // namespace gem5

#endif  // __CPU_PRED_FTB_JUMP_AHEAD_PREDICTOR_HH__