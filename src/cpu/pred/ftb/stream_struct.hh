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
    BranchInfo (const PCStateBase &control_pc,
                const PCStateBase &target_pc,
                const StaticInstPtr &static_inst,
                unsigned size) :
        pc(control_pc.instAddr()),
        target(target_pc.instAddr()),
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
        Addr max = start+34;
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
} LoopEntry;

typedef struct LoopRedirectInfo {
    LoopEntry e;
    Addr branch_pc;
    bool end_loop;
} LoopRedirectInfo;

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
    unsigned predSource;

    // for loop buffer
    bool fromLoopBuffer;
    bool isDouble;
    bool isExit;

    // prediction metas
    std::array<std::shared_ptr<void>, 5> predMetas;

    // for loop
    std::vector<LoopRedirectInfo> loopRedirectInfos;
    std::vector<bool> fixNotExits;

    Tick predTick;
    boost::dynamic_bitset<> history;

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
          predSource(0)
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
    // Addr getEndPC() const { return resolved ? exeEndPC : predEndPC; }
    Addr getTaken() const { return resolved ? exeTaken : predTaken; }
    Addr getTakenTarget() const { return getBranchInfo().target; }
    // Addr getFallThruPC() const { return getEndPC(); }
    // Addr getNextStreamStart() const {return getTaken() ? getTakenTarget() : getFallThruPC(); }
    // bool isCall() const { return endType == END_CALL; }
    // bool isReturn() const { return endType == END_RET; }

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

}FetchStream;

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
        , loopEndPC(0) {}
    
    bool miss() const { return !taken; }
    // bool filledUp() const { return (endPC & fetchTargetMask) == 0; }
    // unsigned predLoopIteration;
};


struct BpTrace : public Record {
    void set(uint64_t startPC, uint64_t controlPC, uint64_t controlType,
        uint64_t taken, uint64_t mispred, uint64_t fallThruPC,
        uint64_t source, uint64_t target) {
        _uint64_data["startPC"] = startPC;
        _uint64_data["controlPC"] = controlPC;
        _uint64_data["controlType"] = controlType;
        _uint64_data["taken"] = taken;
        _uint64_data["mispred"] = mispred;
        _uint64_data["fallThruPC"] = fallThruPC;
        _uint64_data["source"] = source;
        _uint64_data["target"] = target;
    }
    BpTrace(FetchStream &stream) {
        _tick = curTick();
        set(stream.startPC, stream.exeBranchInfo.pc, stream.exeBranchInfo.getType(),
            stream.exeTaken, stream.squashType == SQUASH_CTRL, stream.updateFTBEntry.fallThruAddr,
            stream.predSource, stream.exeBranchInfo.target);
        // for (auto it = _uint64_data.begin(); it != _uint64_data.end(); it++) {
        //     printf("%s: %ld\n", it->first.c_str(), it->second);
        // }
    }
};

struct TageMissTrace : public Record {
    void set(uint64_t startPC, uint64_t branchPC, uint64_t lgcBank, uint64_t phyBank, uint64_t mainFound, uint64_t mainCounter, uint64_t mainUseful,
        uint64_t altCounter, uint64_t mainTable, uint64_t mainIndex, uint64_t altIndex, uint64_t tag,
        uint64_t useAlt, uint64_t predTaken, uint64_t actualTaken, uint64_t allocSuccess, uint64_t allocFailure,
        uint64_t predUseSC, uint64_t predSCDisagree, uint64_t predSCCorrect)
    {
        _tick = curTick();
        _uint64_data["startPC"] = startPC;
        _uint64_data["branchPC"] = branchPC;
        _uint64_data["lgcBank"] = lgcBank;
        _uint64_data["phyBank"] = phyBank;
        _uint64_data["mainFound"] = mainFound;
        _uint64_data["mainCounter"] = mainCounter;
        _uint64_data["mainUseful"] = mainUseful;
        _uint64_data["altCounter"] = altCounter;
        _uint64_data["mainTable"] = mainTable;
        _uint64_data["mainIndex"] = mainIndex;
        _uint64_data["altIndex"] = altIndex;
        _uint64_data["tag"] = tag;
        _uint64_data["useAlt"] = useAlt;
        _uint64_data["predTaken"] = predTaken;
        _uint64_data["actualTaken"] = actualTaken;
        _uint64_data["allocSuccess"] = allocSuccess;
        _uint64_data["allocFailure"] = allocFailure;
        _uint64_data["predUseSC"] = predUseSC;
        _uint64_data["predSCDisagree"] = predSCDisagree;
        _uint64_data["predSCCorrect"] = predSCCorrect;
    }
};

}  // namespace ftb_pred

}  // namespace branch_prediction

}  // namespace gem5
#endif  // __CPU_PRED_FTB_STREAM_STRUCT_HH__
