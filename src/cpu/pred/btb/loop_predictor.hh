#ifndef __CPU_PRED_BTB_LOOP_PREDICTOR_HH__
#define __CPU_PRED_BTB_LOOP_PREDICTOR_HH__

#include <array>
#include <queue>
#include <stack>
#include <utility> 
#include <vector>

#include "cpu/pred/btb/stream_struct.hh"

#ifdef UNIT_TEST
#include "cpu/pred/btb/test/test_dprintf.hh"

#else
#include "debug/LoopPredictor.hh"
#include "debug/LoopPredictorVerbose.hh"

#endif


namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

class LoopPredictor
{
  public:
    // index using start pc of block where the loop branch is in


    unsigned tagSize;
    Addr tagMask;

    unsigned numSets;
    Addr idxMask;
    unsigned numWays;

    unsigned maxConf = 7;
    // do not cover loop with less than 100 iterations, since tage may predict it well
    unsigned minTripCnt = 1;
    
    bool enableDB;

    std::vector<std::unordered_map<Addr, LoopEntry>> loopStorage;
    std::unordered_map<Addr, LoopEntry> commitLoopStorage;

    // TODO: replacement policy

    int getIndex(Addr pc) {return (pc >> 1) & idxMask;}

    Addr getTag(Addr pc) {return (pc >> (1)) & tagMask;}

    int getRemainingIter(Addr loopPC) {return 0;}

    // since we record pc in loop entry
    // we need to check whether given prediction block
    // has this loop branch
    // returns <end, info, is_double, conf>
    std::tuple<bool, LoopRedirectInfo, bool, bool> shouldEndLoop(bool taken, Addr branch_pc, bool may_be_double) {
      DPRINTF(LoopPredictor, "query loop branch: taken: %d, pc: %#lx, is_double: %d\n",
        taken, branch_pc, may_be_double);
      LoopRedirectInfo info;
      info.branch_pc = branch_pc;
      info.end_loop = false;
      int idx = getIndex(branch_pc);
      Addr tag = getTag(branch_pc);
      const auto &it = loopStorage[idx].find(tag);
      if (it != loopStorage[idx].end()) {
        auto &way = it->second;
        info.e = way;

        int remaining_iter = way.tripCnt - way.specCnt;
        DPRINTF(LoopPredictor, "found loop entry idx %d, tag %#lx: tripCnt: %d, specCnt: %d, conf: %d\n", idx, tag, way.tripCnt, way.specCnt, way.conf);
        bool exit = false;
        bool is_double = false;
        bool conf = way.conf == maxConf;
        // if unconf and bpu predict not taken, we trust bpu to sychronize specCnt to 0
        if (conf || taken) {
          // completely use loop predictor result to predict
          if (remaining_iter == 0 || (remaining_iter == 1 && may_be_double)) {
            DPRINTF(LoopPredictor, "loop end detected, doubling is %d, exiting loop, setting specCnt to 0\n",
              remaining_iter == 1 && may_be_double);
            way.specCnt = 0;
            info.end_loop = true;
            exit = true;
            is_double = remaining_iter == 1 && may_be_double;
          } else if ((remaining_iter == 1 && !may_be_double) || remaining_iter >= 2) {
            way.specCnt += may_be_double ? 2 : 1;
            is_double = may_be_double;
          }
        } else {
          // unconf and bpu predict not taken, use bpu to synchronize specCnt
          DPRINTF(LoopPredictor, "bpu prediction is not taken, loop predictor unconf, exiting loop, setting specCnt to 0\n");
          way.specCnt = 0;
          is_double = false;
        }
        return std::make_tuple(exit, info, is_double, conf);
      }
      return std::make_tuple(false, info, false, false);
    }

    // called when loop branch is committed, identification is done before calling
    // return true if loop branch is in main storage and is exit
    bool commitLoopBranch(Addr pc, Addr target, Addr fallThruPC, bool mispredicted, LoopTrace &rec) {
      DPRINTF(LoopPredictor,
        "Commit loop branch: pc: %#lx, target: %#lx, fallThruPC: %#lx\n",
        pc, target, fallThruPC);
      bool takenBackward = target < pc;
      Addr tag = getTag(pc);
      bool loopExit = false;

      // if already trained, update conf in loopStorage
      int idx = getIndex(pc);
      const auto &it = loopStorage[idx].find(tag);
      const auto &it2 = commitLoopStorage.find(tag);
      bool main_found = it != loopStorage[idx].end();
      bool train_found = it2 != commitLoopStorage.end();
      const auto main_entry = main_found ? it->second : LoopEntry();
      const auto train_entry = train_found ? it2->second : LoopEntry();
      // do not need to train commit storage when loop branch is in mainStorage
      // found training entry
      if (train_found) {
        auto &way = it2->second;
        DPRINTF(LoopPredictor, "found training entry: tripCnt: %d, specCnt: %d, conf: %d\n",
          way.tripCnt, way.specCnt, way.conf);
        if (takenBackward) {
          // still in loop, inc specCnt
          way.specCnt++;
        } else {
          loopExit = true;
          // check if this tripCnt is identical to the last trip
          auto currentTripCnt = way.specCnt;
          auto identical = currentTripCnt == way.tripCnt;
          if (way.conf < maxConf && identical) {
            way.conf++;
          } else if (way.conf > 0 && !identical) {
            way.conf -= 4;
            if (way.conf < 0) {
              way.conf = 0;
            }
          }
          int tripCnt = way.specCnt;
          DPRINTF(LoopPredictor, "new tripCnt %d\n", tripCnt);
          if (tripCnt >= minTripCnt) {
            if (!main_found) {
              // not in main storage, write into main storage
              // if (way.specCnt > minTripCnt) {
                int idx = getIndex(pc);
                DPRINTF(LoopPredictor, "loop end detected, specCnt %d, writting to loopStorage idx %d, tag %ld\n",
                  way.specCnt, idx, tag);
                loopStorage[idx][tag].valid = true;
                loopStorage[idx][tag].specCnt = 0;
                loopStorage[idx][tag].tripCnt = tripCnt;
                loopStorage[idx][tag].conf = 0;
              // }
            } else {
              // in main storage, update conf and tripCnt
              DPRINTF(LoopPredictor, "loop end and in storage, updating conf and tripCnt, mispred %d\n", mispredicted);
              loopStorage[idx][tag].conf = way.conf;
              loopStorage[idx][tag].tripCnt = way.specCnt;
            }
          } else { // if tripCnt < minTripCnt
            // we only update main storage when this branch is in it.
            // the tripCnt could change from n to a value less than minTripCnt,
            // we should invalidate it
            if (main_found) {
              int idx = getIndex(pc);
              DPRINTF(LoopPredictor, "loop end with tripCnt less than %d, invalidating loopStorage idx %d, tag %ld\n",
                minTripCnt, idx, tag);
              loopStorage[idx][tag].valid = false;
              loopStorage[idx][tag].conf = 0;
            }
            // FIXME: what if invalidate a branch that is supplying by loop buffer?
            // provide a specCnt (remaining iteration) to loop buffer at the beginning of a loop
            // and loop buffer will use it to predict the loop branch, as well as informing
            // loop predictor to update specCnt
            // this requires decoupling lookup from speculative update
          }
          way.tripCnt = way.specCnt;
          way.specCnt = 0;
          // TODO: log
          // TODO: consider conf to avoid overwriting
        }
      } else {
        // not found, create new entry
        DPRINTF(LoopPredictor, "creating new entry for loop branch %#lx, tag %#lx\n", pc, tag);
        LoopEntry entry;
        entry.valid = true;
        entry.tripCnt = 0;
        entry.specCnt = 1;
        entry.conf = 0;
        commitLoopStorage[tag] = entry;
      }


      if (enableDB) {
        rec.set_in_lp(train_found, train_entry.specCnt, train_entry.tripCnt, train_entry.conf,
          main_found, main_entry.tripCnt, main_entry.conf);
      }
      DPRINTF(LoopPredictor, "commit loop branch, pc: %#lx, target: %#lx, fallThruPC: %#lx, takenBackward %d, mispred %d; \
training_entry: %d, tripCnt %d, specCnt %d, conf %d; in_main: %d, tripCnt %d, conf %d\n",
        pc, target, fallThruPC, takenBackward, mispredicted, train_found, train_entry.tripCnt, train_entry.specCnt, train_entry.conf,
        main_found, main_entry.tripCnt, main_entry.conf);
      return loopExit;
    }

    void recover(LoopRedirectInfo info, bool actually_taken, Addr squash_inst_pc, bool is_control, bool in_walk, unsigned intraIter) {
      if (info.e.valid) {
        DPRINTF(LoopPredictor, "redirecting loop branch: taken: %d, pc: %#lx, tripCnt: %d, specCnt: %d, conf: %d, pred use pc: %#lx\n",
          actually_taken, squash_inst_pc, info.e.tripCnt, info.e.specCnt, info.e.conf, info.branch_pc);
        auto &loop_pc = info.branch_pc;
        int idx = getIndex(loop_pc);
        const auto &it = loopStorage[idx].find(getTag(loop_pc));
        if (it != loopStorage[idx].end()) {
          DPRINTF(LoopPredictor, "found idx %d\n", idx);
          auto &way = it->second;
          if (in_walk) {
            if (way.repair) {
              DPRINTF(LoopPredictor, "find unrepaired entry in walk, recover specCnt to %d\n", info.e.specCnt);
              way.specCnt = info.e.specCnt;
            }
          } else {
            // TODO: what if a loop branch incurs non-control squash or trap squash?
            if (squash_inst_pc == info.branch_pc && !actually_taken) {
              // reset specCnt to 0
              DPRINTF(LoopPredictor, "mispredicted loop end of, sychronizing specCnt to 0\n");
              way.specCnt = 0;
            } else if (squash_inst_pc < info.branch_pc) {
              way.specCnt = info.e.specCnt + intraIter;
              DPRINTF(LoopPredictor, "loop branch is not in this stream, intraIter %u, recovering specCnt to %d\n", intraIter, way.specCnt);
            }
          }
          way.repair = false;
        }
      }
    }

    void startRepair() {
      DPRINTF(LoopPredictorVerbose, "start repair, setting all repair bits\n");
      for (auto &set : loopStorage) {
        for (auto &way : set) {
          if (way.second.valid) {
            way.second.repair = true;
          }
        }
      }
    }

    void endRepair() {
      DPRINTF(LoopPredictorVerbose, "end repair, clearing all remaining repair bits\n");
      for (auto &set : loopStorage) {
        for (auto &way : set) {
          if (way.second.valid) {
            way.second.repair = false;
          }
        }
      }
    }

    bool findLoopBranchInStorage(Addr pc) {
      Addr tag = getTag(pc);
      int idx = getIndex(pc);
      return commitLoopStorage.find(tag) != commitLoopStorage.end() ||
             loopStorage[idx].find(tag) != loopStorage[idx].end();
    }

    bool isLoopBranchConf(Addr pc) {
      Addr tag = getTag(pc);
      int idx = getIndex(pc);
      if (loopStorage[idx].find(tag) != loopStorage[idx].end()) {
        return loopStorage[idx][tag].conf == maxConf;
      } else {
        return false;
      }
    }

    LoopPredictor(unsigned sets, unsigned ways, bool e) {
      numSets = sets;
      numWays = ways;
      idxMask = (1 << ceilLog2(numSets)) - 1;
      loopStorage.resize(numSets);
      for (unsigned i = 0; i < numWays; i++) {
        for (auto set : loopStorage) {
          set[0xffffff-i];
        }
        commitLoopStorage[0xfffffef-i];
      }
      //       VaddrBits   instOffsetBits  log2Ceil(PredictWidth)
      tagSize = 39 - 1 - 4 - ceilLog2(numSets);
      tagMask = (1 << tagSize) - 1;
      enableDB = e;
    }

    LoopPredictor() : LoopPredictor(64, 4, false) {}
};

}  // namespace btb_pred
}  // namespace branch_prediction
}  // namespace gem5

#endif  // __CPU_PRED_BTB_LOOP_PREDICTOR_HH__
