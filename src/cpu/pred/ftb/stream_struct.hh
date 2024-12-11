#ifndef __CPU_PRED_FTB_STREAM_STRUCT_HH__
#define __CPU_PRED_FTB_STREAM_STRUCT_HH__

#include <boost/dynamic_bitset.hpp>

#include "arch/generic/pcstate.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/general_arch_db.hh"
#include "cpu/pred/ftb/stream_common.hh"
#include "cpu/static_inst.hh"
#include "debug/DecoupleBP.hh"
#include "debug/FTB.hh"
#include "debug/Override.hh"
// #include "cpu/pred/ftb/ftb.hh"

namespace gem5 {

namespace branch_prediction {

namespace ftb_pred {

extern unsigned predictWidth;

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
    SQUASH_CTRL,
    SQUASH_OTHER
};

enum SquashSource {
    SQUASH_SRC_DECODE=0,
    SQUASH_SRC_COMMIT
};

typedef struct BranchInfo {
    Addr pc;
    Addr target;
    bool isCond;
    bool isIndirect;
    bool isCall;
    bool isReturn;
    uint8_t size;
    bool isUncond() const { return !this->isCond; }
    Addr getEnd() { return this->pc + this->size; }
    BranchInfo() : pc(0), target(0), isCond(false), isIndirect(false), isCall(false), isReturn(false), size(0) {}
    BranchInfo (const Addr &control_pc,
                const Addr &target_pc,
                const StaticInstPtr &static_inst,
                unsigned size) :
        pc(control_pc),
        target(target_pc),
        isCond(static_inst->isCondCtrl()),
        isIndirect(static_inst->isIndirectCtrl()),
        isCall(static_inst->isCall()),
        isReturn(static_inst->isReturn() && !static_inst->isNonSpeculative() && !static_inst->isDirectCtrl()),
        size(size) {}
    int getType() {
        if (isCond) {
            return 0;
        } else if (!isIndirect) {
            if (isReturn) {
                fatal("jal return detected!\n");
                return 7;
            }
            if (!isCall) {
                return 1;
            } else {
                return 2;
            }
        } else {
            if (!isCall) {
                if (!isReturn) {
                    return 3; // normal indirect
                } else {
                    return 4; // indirect return
                }
            } else {
                if (!isReturn) { // indirect call
                    return 5;
                } else { // call & return
                    return 6;
                }
            }
        }
    }
    // BranchInfo(FTBSlot _e) : pc(_e.pc), target(_e.target), isCond(_e.isCond), isIndirect(_e.isIndirect), isCall(_e.isCall), isReturn(_e.isReturn), size(_e.size) {}

    bool operator < (const BranchInfo &other) const
    {
        return this->pc < other.pc;
    }

    bool operator == (const BranchInfo &other) const
    {
        return this->pc == other.pc;
    }

    bool operator > (const BranchInfo &other) const
    {
        return this->pc > other.pc;
    }

    bool operator != (const BranchInfo &other) const
    {
        return this->pc != other.pc;
    }
}BranchInfo;


typedef struct FTBSlot : BranchInfo
{
    bool valid;
    bool alwaysTaken;
    int ctr;
    bool uncondValid() { return this->isUncond() && this->valid; }
    bool condValid() { return this->isCond && this->valid;}
    FTBSlot() : valid(false) {}
    FTBSlot(const BranchInfo &bi) : BranchInfo(bi), valid(true), alwaysTaken(true), ctr(0) {}
    BranchInfo getBranchInfo() { return BranchInfo(*this); }

}FTBSlot;

typedef struct LFSR64 {
    uint64_t lfsr;
    LFSR64() : lfsr(0x1234567887654321UL) {}
    uint64_t get() {
        next();
        return lfsr;
    }
    void next() {
        if (lfsr == 0) {
            lfsr = 1;
        } else {
            uint64_t bit = ((lfsr >> 0) ^ (lfsr >> 1) ^ (lfsr >> 3) ^ (lfsr >> 4)) & 1;
            lfsr = (lfsr >> 1) | (bit << 63);
        }
    }
}LFSR64;

typedef struct FTBEntry
{
    /** The entry's tag. */
    Addr tag = 0;

    /** The entry's branch info. */
    std::vector<FTBSlot> slots;

    /** The entry's fallthrough address. */
    Addr fallThruAddr;

    /** The entry's thread id. */
    ThreadID tid;

    /** Whether or not the entry is valid. */
    bool valid = false;
    FTBEntry(): fallThruAddr(0), tid(0), valid(false) {}

    int getNumCondInEntryBefore(Addr pc) {
        int num = 0;
        for (auto &slot : this->slots) {
            if (slot.condValid() && slot.pc < pc) {
                num++;
            }
        }
        return num;
    }

    int getTotalNumConds() {
        int num = 0;
        for (auto &slot : this->slots) {
            if (slot.condValid()) {
                num++;
            }
        }
        return num;
    }

    // check if the entry is reasonable with given startPC
    // every branch slot and fallThru should be in the range of (startPC, startPC+34]
    // every 
    bool isReasonable(Addr start) {
        Addr min = start;
        Addr max = start + predictWidth + 2;
        bool reasonable = true;
        for (auto &slot : slots) {
            if (slot.pc < min || slot.pc > max) {
                reasonable = false;
            }
        }
        if (fallThruAddr <= min || fallThruAddr > max) {
            reasonable = false;
        }
        return reasonable;
    }

    FTBSlot getSlot(Addr pc) {
        for (auto &slot : this->slots) {
            if (slot.pc == pc) {
                return slot;
            }
        }
        return FTBSlot();
    }

    bool operator == (const FTBEntry &other) const
    {
        // startPC and slots pc
        if (this->tag != other.tag || this->slots.size() != other.slots.size()) {
            return false;
        }
        for (int i = 0; i < this->slots.size(); i++) {
            if (this->slots[i] != other.slots[i]) {
                return false;
            }
        }
        return true;
    }
}FTBEntry;

struct BlockDecodeInfo {
    std::vector<bool> condMask;
    BranchInfo jumpInfo;
};


using FetchStreamId = uint64_t;
using FetchTargetId = uint64_t;
using PredictionID = uint64_t;

typedef struct LoopEntry {
    bool valid;
    int tripCnt;
    int specCnt;
    int conf;
    bool repair;
    LoopEntry() : valid(false), tripCnt(0), specCnt(0), conf(0), repair(false) {}
} LoopEntry;

typedef struct LoopRedirectInfo {
    LoopEntry e;
    Addr branch_pc;
    bool end_loop;
} LoopRedirectInfo;

typedef struct JAEntry {
    // jump target: indexPC + jumpAheadBlockNum * blockSize
    int jumpAheadBlockNum;
    int conf;
    JAEntry() : jumpAheadBlockNum(0), conf(0) {}
    Addr getJumpTarget(Addr indexPC, int blockSize) {
        return indexPC + jumpAheadBlockNum * blockSize;
    }
} JAEntry;

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
    BranchInfo predBranchInfo;
    // record predicted FTB entry
    bool isHit;
    bool falseHit;
    FTBEntry predFTBEntry;

    bool sentToICache;

    // for commit, write at redirect or fetch
    // bool exeEnded;
    bool exeTaken;
    // Addr exeEndPC;
    BranchInfo exeBranchInfo;

    FTBEntry updateFTBEntry;
    bool updateIsOldEntry;
    bool resolved;

    int squashType;
    Addr squashPC;
    int squashSource;
    unsigned predSource;

    // for loop buffer
    bool fromLoopBuffer;
    bool isDouble;
    bool isExit;

    // for ja predictor
    bool jaHit;
    JAEntry jaEntry;
    int currentSentBlock;

    // prediction metas
    std::vector<std::shared_ptr<void>> predMetas;

    // for loop
    std::vector<LoopRedirectInfo> loopRedirectInfos;
    std::vector<bool> fixNotExits;
    std::vector<LoopRedirectInfo> unseenLoopRedirectInfos;

    Tick predTick{};
    Cycles predCycle{};
    boost::dynamic_bitset<> history;

    // for profiling
    int fetchInstNum;
    int commitInstNum;
    std::map<Addr, bool> commitMispredictions; // per committed branch
    std::map<Addr, std::tuple<SquashType, SquashSource, BranchInfo>> squashInfos; // per committed inst if there is squash

    FetchStream()
        : startPC(0),
          predTaken(false),
          predEndPC(0),
          predBranchInfo(BranchInfo()),
          isHit(false),
          falseHit(false),
          predFTBEntry(FTBEntry()),
          sentToICache(false),
          exeTaken(false),
          exeBranchInfo(BranchInfo()),
          updateFTBEntry(FTBEntry()),
          updateIsOldEntry(false),
          resolved(false),
          squashType(SquashType::SQUASH_NONE),
          squashSource(SquashSource::SQUASH_SRC_COMMIT),
          predSource(0),
          fromLoopBuffer(false),
          isDouble(false),
          isExit(false),
          jaHit(false),
          jaEntry(JAEntry()),
          currentSentBlock(0),
          fetchInstNum(0),
          commitInstNum(0)
    {
    }

    // the default exe result should be consistent with prediction
    void setDefaultResolve() {
        resolved = false;
        exeBranchInfo = predBranchInfo;
        exeTaken = predTaken;
    }

    // bool getEnded() const { return resolved ? exeEnded : predEnded; }
    BranchInfo getBranchInfo() const { return resolved ? exeBranchInfo : predBranchInfo; }
    Addr getControlPC() const { return getBranchInfo().pc; }
    Addr getEndPC() const { return getBranchInfo().getEnd(); } // FIXME: should be end of squash inst when non-control squash of trap squash
    Addr getTaken() const { return resolved ? exeTaken : predTaken; }
    Addr getTakenTarget() const { return getBranchInfo().target; }
    // Addr getFallThruPC() const { return getEndPC(); }
    // Addr getNextStreamStart() const {return getTaken() ? getTakenTarget() : getFallThruPC(); }
    // bool isCall() const { return endType == END_CALL; }
    // bool isReturn() const { return endType == END_RET; }

    // for ja hit blocks, should be the biggest addr of startPC + k*blockSize where k is interger
    Addr getRealStartPC() const {
        if (jaHit && squashType == SQUASH_CTRL) {
            Addr realStart = startPC;
            Addr squashBranchPC = exeBranchInfo.pc;
            while (realStart + predictWidth <= squashBranchPC) {
                realStart += predictWidth;
            }
            return realStart;
        } else {
            return startPC;
        }
    }

    std::pair<int, bool> getHistInfoDuringSquash(Addr squash_pc, bool is_cond, bool actually_taken, unsigned maxShamt)
    {
        bool hit = isHit;
        if (!hit) {
            int shamt = is_cond ? 1 : 0;
            return std::make_pair(shamt, actually_taken);
        } else {
            int shamt = predFTBEntry.getNumCondInEntryBefore(squash_pc);
            assert(shamt <= maxShamt);
            if (is_cond) {
                if (shamt == maxShamt) {
                    // current cond should not be counted into this entry
                    return std::make_pair(2, false);
                } else {
                    return std::make_pair(shamt+1, actually_taken);
                }
            } else {
                return std::make_pair(shamt, false);
            }
        }
    }

};

typedef struct FullFTBPrediction
{
    Addr bbStart;
    FTBEntry ftbEntry; // for FTB
    std::vector<bool> condTakens; // for conditional branch predictors

    Addr indirectTarget; // for indirect predictor
    Addr returnTarget; // for RAS

    bool valid; // hit
    unsigned predSource;
    Tick predTick;
    Cycles predCycle;
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
                i++;
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
            target = bbStart + predictWidth;
        }
        return target;
    }

    Addr getEnd() {
        if (isTaken()) {
            return getTakenSlot().getEnd();
        } else {
            return getFallThrough();
        }
    }



    Addr getFallThrough() {
        auto &ftbEntry = this->ftbEntry;
        if (valid) {
            return ftbEntry.fallThruAddr;
        } else {
            return bbStart + predictWidth;
        }
    }

    Addr controlAddr() {
        return getTakenSlot().pc;
    }

    int getTakenBranchIdx() {
        auto &ftbEntry = this->ftbEntry;
        if (valid) {
            int i = 0;
            for (auto &slot : ftbEntry.slots) {
                if ((slot.condValid() && condTakens[i]) ||
                    slot.uncondValid()) {
                        return i;
                    }
                i++;
            }
        }
        return -1;
    }

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

    std::pair<int, bool> getHistInfo()
    {
        int shamt = 0;
        bool taken = false;
        if (valid) {
            int i = 0;
            for (auto &slot : ftbEntry.slots) {
                DPRINTF(Override, "slot %d: condValid %d, uncondValid %d\n",
                    i, slot.condValid(), slot.uncondValid());
                DPRINTF(Override, "condTakens.size() %d\n", condTakens.size());
                if (slot.condValid()) {
                    shamt++;
                    if (condTakens[i]) {
                        taken = true;
                        break;
                    }
                }
                assert(condTakens.size() >= i+1);
                
                i++;
            }
        }
        return std::make_pair(shamt, taken);
    }

    bool isReasonable() {
        return !valid || ftbEntry.isReasonable(bbStart);
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

    // for loop buffer
    bool inLoop;
    int iter;
    bool isExit;
    Addr loopEndPC;

    // for ja predictor
    int noPredBlocks;

    FtqEntry()
        : startPC(0)
        , endPC(0)
        , takenPC(0)
        , taken(false)
        , target(0)
        , fsqID(0)
        , inLoop(false)
        , iter(0)
        , isExit(false)
        , loopEndPC(0)
        , noPredBlocks(0) {}
    
    bool miss() const { return !taken; }
    // bool filledUp() const { return (endPC & fetchTargetMask) == 0; }
    // unsigned predLoopIteration;
};

}  // namespace ftb_pred

}  // namespace branch_prediction

}  // namespace gem5
#endif  // __CPU_PRED_FTB_STREAM_STRUCT_HH__
