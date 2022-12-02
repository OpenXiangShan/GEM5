#ifndef __CPU_PRED_FTB_STREAM_STRUCT_HH__
#define __CPU_PRED_FTB_STREAM_STRUCT_HH__

#include <boost/dynamic_bitset.hpp>

#include "arch/generic/pcstate.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/ftb/stream_common.hh"
#include "cpu/static_inst.hh"
#include "debug/DecoupleBP.hh"
#include "debug/Override.hh"
// #include "cpu/pred/ftb/ftb.hh"

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


typedef struct BranchInfo {
    Addr pc;
    Addr target;
    bool isCond;
    bool isIndirect;
    bool isCall;
    bool isReturn;
    uint8_t size;
    bool isUncond() { return !this->isCond; }
    Addr getEnd() { return this->pc + this->size; }
    BranchInfo() : pc(0), target(0), isCond(false), isIndirect(false), isCall(false), isReturn(false), size(0) {}
    BranchInfo (const PCStateBase &control_pc,
                const PCStateBase &target_pc,
                const StaticInstPtr &static_inst,
                unsigned size) :
        pc(control_pc.instAddr()),
        target(target_pc.instAddr()),
        isCond(static_inst->isCondCtrl()),
        isIndirect(static_inst->isIndirectCtrl()),
        isCall(static_inst->isCall()),
        isReturn(static_inst->isReturn()),
        size(size) {}
    // BranchInfo(FTBSlot _e) : pc(_e.pc), target(_e.target), isCond(_e.isCond), isIndirect(_e.isIndirect), isCall(_e.isCall), isReturn(_e.isReturn), size(_e.size) {}
}BranchInfo;


typedef struct FTBSlot : BranchInfo
{
    bool valid;
    bool uncondValid() { return this->isUncond() && this->valid; }
    bool condValid() { return this->isCond && this->valid;}
    FTBSlot() : valid(false) {}
    FTBSlot(const BranchInfo &bi) : BranchInfo(bi), valid(true) {}
    BranchInfo getBranchInfo() { return BranchInfo(*this); }

    bool operator < (const FTBSlot &other) const
    {
        return this->pc < other.pc;
    }

    bool operator == (const FTBSlot &other) const
    {
        return this->pc == other.pc;
    }

    bool operator > (const FTBSlot &other) const
    {
        return this->pc > other.pc;
    }
}FTBSlot;

typedef struct FTBEntry
{
    /** The entry's tag. */
    // TODO: limit width to tagBits
    Addr tag = 0;

    // TODO: parameterzie numBr
    /** The entry's branch info. */
    std::vector<FTBSlot> slots;

    /** The entry's fallthrough address. */
    Addr fallThruAddr;

    /** The entry's thread id. */
    ThreadID tid;

    /** Whether or not the entry is valid. */
    bool valid = false;
    // TODO: always taken
    FTBEntry(): fallThruAddr(0), tid(0), valid(false) {}
    void dump() {
        DPRINTF(DecoupleBP, "FTB entry: valid %d, tag %#lx, fallThruAddr:%#lx, slots:\n",
            valid, tag, fallThruAddr);
        for (auto &slot : slots) {
            DPRINTF(DecoupleBP, "    pc:%#lx, size:%d, target:%#lx, cond:%d, indirect:%d, call:%d, return:%d\n",
                slot.pc, slot.size, slot.target, slot.isCond, slot.isIndirect, slot.isCall, slot.isReturn);
        }
    }
}FTBEntry;

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
typedef struct FetchStream
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
    bool isHit;
    FTBEntry predFTBEntry;

    bool sentToICache;

    // for commit, write at redirect or fetch
    // bool exeEnded;
    bool exeTaken;
    // Addr exeEndPC;
    // TODO: use PCState for target(gem5 specific)
    BranchInfo exeBranchInfo;
    // TODO: remove signals below
    bool resolved;

    int squashType;
    // int tripCount;
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
          isHit(false),
          predFTBEntry(FTBEntry()),
          sentToICache(false),
        //   exeEnded(false),
          exeTaken(false),
        //   exeEndPC(0),
        //   exeTarget(0),
        //   exeBranchPC(0),
          exeBranchInfo(BranchInfo()),
          resolved(false),
          squashType(SquashType::SQUASH_NONE),
        //   tripCount(-1),
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
        // exeEndPC = predEndPC;
        // exeTarget = predTarget;
        // exeBranchPC = predBranchPC;
        exeBranchInfo = predBranchInfo;
        // exeBranchType = predBranchType;
    }

    // bool getEnded() const { return resolved ? exeEnded : predEnded; }
    BranchInfo getBranchInfo() const { return resolved ? exeBranchInfo : predBranchInfo; }
    Addr getControlPC() const { return getBranchInfo().pc; }
    // Addr getEndPC() const { return resolved ? exeEndPC : predEndPC; }
    Addr getTaken() const { return resolved ? exeTaken : predTaken; }
    Addr getTakenTarget() const { return getBranchInfo().target; }
    // Addr getFallThruPC() const { return getEndPC(); }
    // Addr getNextStreamStart() const {return getTaken() ? getTakenTarget() : getFallThruPC(); }
    // bool isCall() const { return endType == END_CALL; }
    // bool isReturn() const { return endType == END_RET; }
}FetchStream;

typedef struct FullFTBPrediction
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

    bool isTaken() {
        auto &ftbEntry = this->ftbEntry;
        if (valid) {
            int i = 0;
            for (auto &slot : ftbEntry.slots) {
                if ((slot.condValid() && condTakens[i]) ||
                    slot.uncondValid()) {
                        return true;
                    }
                i++;
            }
        }
        return false;
    }

    FTBSlot getTakenSlot() {
        auto &ftbEntry = this->ftbEntry;
        if (valid) {
            int i = 0;
            for (auto &slot : ftbEntry.slots) {
                if ((slot.condValid() && condTakens[i]) ||
                    slot.uncondValid()) {
                        return slot;
                    }
            }
        }
        return FTBSlot();
    }

    Addr getTarget() {
        Addr target;
        auto &ftbEntry = this->ftbEntry;
        if (valid) {
            auto slot = getTakenSlot();
            if (slot.condValid()) {
                target = slot.target;
            } else if (slot.uncondValid()) {
                target = slot.target;
                if (slot.isIndirect) {
                    target = indirectTarget;
                }
                if (slot.isReturn) {
                    target = returnTarget;
                }
            } else {
                target = ftbEntry.fallThruAddr;
            }
            
        } else {
            target = bbStart + 32; //TODO: +predictWidth
        }
        return target;
    }



    Addr getFallThrough() {
        auto &ftbEntry = this->ftbEntry;
        if (valid) {
            return ftbEntry.fallThruAddr;
        } else {
            return bbStart + 32; //TODO: +predictWidth
        }
    }

    Addr controlAddr() {
        auto &ftbEntry = this->ftbEntry;
        for (int i = 0; i < 2; i++) {
            if (ftbEntry.slots[i].condValid() && condTakens[i]) {
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
                bool this_taken, other_taken;
                int this_cond_num, other_cond_num;
                std::tie(this_cond_num, this_taken) = this->getHistInfo();
                std::tie(other_cond_num, other_taken) = other.getHistInfo();
                Addr this_control_addr = this->controlAddr();
                Addr other_control_addr = other.controlAddr();
                Addr this_npc = this->getTarget();
                Addr other_npc = other.getTarget();
                // both this and chosen valid
                return this_taken == other_taken &&
                       this_control_addr == other_control_addr &&
                       this_cond_num == other_cond_num &&
                       this_npc == other_npc;
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
            int i = 0;
            for (auto &slot : ftbEntry.slots) {
                DPRINTF(Override, "slot %d: condValid %d, uncondValid %d\n",
                    i, slot.condValid(), slot.uncondValid());
                DPRINTF(Override, "condTakens.size() %d\n", condTakens.size());
                if (slot.condValid()) {
                    shamt++;
                }
                assert(condTakens.size() >= i+1);
                if (condTakens[i]) {
                    taken = true;
                    break;
                }
                i++;
            }
        }
        return std::make_pair(shamt, taken);
    }

    void dump() {
        DPRINTF(Override, "dumping FullFTBPrediction\n");
        DPRINTF(Override, "bbStart: %#lx, ftbEntry:\n", bbStart);
        ftbEntry.dump();
        DPRINTF(Override, "condTakens: ");
        for (auto taken : condTakens) {
            DPRINTFR(Override, "%d ", taken);
        }
        DPRINTFR(Override, "\n");
        DPRINTF(Override, "indirectTarget: %#lx, returnTarget: %#lx\n",
            indirectTarget, returnTarget);
    }
}FullFTBPrediction;

// each entry corresponds to a 32Byte unaligned block
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
