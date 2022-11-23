#ifndef __CPU_PRED_FTB_STREAM_STRUCT_HH__
#define __CPU_PRED_FTB_STREAM_STRUCT_HH__

#include <boost/dynamic_bitset.hpp>

#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/ftb/stream_common.hh"
#include "cpu/pred/ftb/ftb.hh"

namespace gem5 {

namespace branch_prediction {

namespace ftb_pred {

enum EndType {
    END_CALL=0,
    END_RET,
    END_OTHER_TAKEN,
    END_NOT_TAKEN,
    END_CONT,  // to be continued
    END_NONE
};

enum SquashType {
    SQUASH_NONE=0,
    SQUASH_TRAP,
    SQUASH_CTRL
};

struct BranchInfo {
    Addr pc;
    Addr target;
    bool isCond;
    bool isIndirect;
    bool isCall;
    bool isReturn;
    uint8_t size;
    bool isUncond() { return !isCond; }
    BranchInfo() : pc(0), target(0), isCond(false), isIndirect(false), isCall(false), isReturn(false), size(0) {}
};

struct BlockDecodeInfo {
    std::vector<bool> condMask;
    BranchInfo jumpInfo;
};


using FetchStreamId = uint64_t;
using FetchTargetId = uint64_t;
using PredictionID = uint64_t;

// struct DivideEntry
// {
//     bool taken;
//     Addr start;
//     Addr branch;
//     Addr next;
//     Addr fallThruPC;

//     DivideEntry() : taken(false), start(0), branch(0), next(0), fallThruPC(0) {}
//     DivideEntry(bool taken, Addr start, Addr branch, Addr next, Addr fallThruPC) : taken(taken), start(start), branch(branch), next(next),
//                                                                                    fallThruPC(fallThruPC) {}

// };

// struct LoopEntry
// {
//     // may need to add a valid bit
//     bool valid;
//     Addr branch;
//     Addr target;
//     Addr outTarget;
//     Addr fallThruPC;
//     int tripCount;
//     int detectedCount;
//     bool intraTaken;
//     unsigned age;

//     LoopEntry() : valid(true), branch(0), target(0), outTarget(0), fallThruPC(0), tripCount(0), detectedCount(0), intraTaken(false), age(3) {}
//     LoopEntry(Addr branch, Addr target, Addr outTarget, Addr fallThruPC, int detectedCount, bool intraTaken) : 
//               valid(true), branch(branch), target(target), outTarget(outTarget), fallThruPC(fallThruPC), 
//               tripCount(0), detectedCount(detectedCount), intraTaken(intraTaken), age(3) {}
// };

// NOTE: now this corresponds to an ftq entry in
//       XiangShan nanhu architecture
struct FetchStream
{
    Addr startPC;

    // indicating whether a backing prediction has finished
    // bool predEnded;
    bool predTaken;

    // predicted stream end pc (fall through pc)
    Addr predEndPC;
    // TODO: use PCState for target(gem5 specific)
    BranchInfo predBranchInfo;
    // record predicted FTB entry
    FTBEntry predFTBEntry;

    // for commit, write at redirect or fetch
    // bool exeEnded;
    bool exeTaken;
    Addr exeEndPC;
    // TODO: use PCState for target(gem5 specific)
    BranchInfo exeBranchInfo;
    // TODO: remove signals below
    bool resolved;

    int squashType;
    // int tripCount;
    bool isHit;
    // bool useLoopPrediction;
    // bool isLoop;
    // Addr loopTarget;
    // Addr tageTarget;
    unsigned predSource;
    // std::list<std::pair<Addr, unsigned int>> mruLoop;

    // TODO: collect spec info into one struct?
    boost::dynamic_bitset<> history;

    std::vector<boost::dynamic_bitset<>> indexFoldedHist;
    std::vector<boost::dynamic_bitset<>> tagFoldedHist;

    FetchStream()
        : startPC(0),
        //   predEnded(false),
          predTaken(false),
          predEndPC(0),
        //   predTarget(0),
        //   predBranchPC(0),
          predBranchInfo(BranchInfo()),
          predFTBEntry(FTBEntry()),
        //   exeEnded(false),
          exeTaken(false),
          exeEndPC(0),
        //   exeTarget(0),
        //   exeBranchPC(0),
          exeBranchInfo(BranchInfo()),
          resolved(false),
          squashType(SquashType::SQUASH_NONE),
        //   tripCount(-1),
          isHit(false),
        //   useLoopPrediction(false),
        //   isLoop(false),
        //   loopTarget(0),
        //   tageTarget(0),
          predSource(0)
    {
    }

    // the default exe result should be consistent with prediction
    void setDefaultResolve() {
        resolved = false;
        // exeEnded = predEnded;
        exeEndPC = predEndPC;
        // exeTarget = predTarget;
        // exeBranchPC = predBranchPC;
        exeBranchInfo = predBranchInfo;
        // exeBranchType = predBranchType;
    }

    // bool getEnded() const { return resolved ? exeEnded : predEnded; }
    Addr getControlPC() const { return resolved ? exeBranchInfo.pc : predBranchInfo.pc; }
    Addr getEndPC() const { return resolved ? exeEndPC : predEndPC; }
    Addr getTaken() const { return resolved ? exeTaken : predTaken; }
    Addr getTakenTarget() const { return resolved ? exeBranchInfo.target : predBranchInfo.target; }
    Addr getFallThruPC() const { return getEndPC(); }
    Addr getNextStreamStart() const {return getTaken() ? getTakenTarget() : getFallThruPC(); }
    // bool isCall() const { return endType == END_CALL; }
    // bool isReturn() const { return endType == END_RET; }
};

struct FullFTBPrediction
{
    Addr bbStart;
    // Addr fallThru;
    // BranchInfo controlInfo;
    FTBEntry ftbEntry; // for FTB
    std::vector<bool> condTakens; // for conditional branch predictors

    Addr indirectTarget; // for indirect predictor
    Addr returnTarget; // for RAS

    // bool isCall() const { return isTaken && controlInfo.isCall; }
    // bool isReturn() const { return isTaken && controlInfo.isReturn; }

    bool valid; // hit
    // bool useLoopPrediction;
    // Addr tageTarget;
    unsigned predSource;
    boost::dynamic_bitset<> history;

    bool isTaken() const {
        if (valid) {
            for (int i = 0; i < 2; i++) {
                if ((ftbEntry.slots[i].condValid() && condTakens[i]) ||
                    ftbEntry.slots[i].uncondValid) {
                    return true;
                }
            }
        }
        return false;
    }

    Addr controlAddr() const {
        for (int i = 0; i < 2; i++) {
            if (ftbEntry.slots[i].condValid() && condTakens(i)) {
                return ftbEntry.slots[i].pc;
            }
        }
        if (ftbEntry.slots[1].valid && !ftbEntry.slots[1].isCond) {
            return ftbEntry.slots[1].pc;
        }
        return 0; 
    }

    // TODO: brNum, taken, takenPC, target, fallThruPC
    bool match(FullFTBPrediction &other)
    {
        if (!other.valid) {
            // chosen is invalid, means all predictors invalid, match
            return true;
        } else {
            // chosen is valid
            if (!this->valid) {
                // if this is invalid and chosen is valid, sure no match
                return false;
            } else {
                // both this and chosen valid
                if (this->isTaken != other.isTaken) {
                    return false;
                } else if (this->isTaken && other.isTaken) {
                    // here bb.start is not a compare criteria, since ubtb and tage's bbStart is different
                    return controlAddr == other.controlAddr && nextStream == other.nextStream;   // TODO consider endType and controlAddr
                } else {
                    return controlAddr == other.controlAddr && controlSize == other.controlSize && endType == other.endType;

                }
            }
        }
    }

    std::vector<boost::dynamic_bitset<>> indexFoldedHist;
    std::vector<boost::dynamic_bitset<>> tagFoldedHist;

    std::pair<int, bool> getHistInfo() {
        int shamt = 0;
        bool taken = false;
        if (valid) {
            // TODO: numBr
            for (int i = 0; i < 2; i++) {
                if (ftbEntry.slots[i].condValid()) {
                    shamt++;
                }
                if (condTakens[i]) {
                    taken = true;
                    break;
                }
            }
        }
        return std::make_pair(shamt, taken);
    }
};

// each entry corrsponds to a cache line
struct FtqEntry
{
    Addr startPC;
    Addr endPC;    // TODO: use PCState and it can be included in takenPC

    // When it is a taken branch, takenPC is the control (starting) PC
    // When it is yet missing, takenPC is the ``known'' PC,
    // decoupledPredict cannot goes beyond takenPC and should be blocked
    // when current PC == takenPC
    Addr takenPC;

    bool taken;
    Addr target;  // TODO: use PCState
    FetchStreamId fsqID;
    FtqEntry()
        : startPC(0)
        , endPC(0)
        , takenPC(0)
        , taken(false)
        , target(0)
        , fsqID(0) {}
    
    bool miss() const { return !taken; }
    // bool filledUp() const { return (endPC & fetchTargetMask) == 0; }
    // unsigned predLoopIteration;
};

}  // namespace ftb_pred

}  // namespace branch_prediction

}  // namespace gem5
#endif  // __CPU_PRED_FTB_STREAM_STRUCT_HH__
