#ifndef __CPU_PRED_BTB_STREAM_STRUCT_HH__
#define __CPU_PRED_BTB_STREAM_STRUCT_HH__

#include <queue>

#include <boost/dynamic_bitset.hpp>

// #include "arch/generic/pcstate.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/general_arch_db.hh"
#include "cpu/static_inst.hh"

namespace gem5 {

namespace branch_prediction {

namespace btb_pred {

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

enum BranchType
{
    BR_COND=0,
    BR_DIRECT_NORMAL=1,
    BR_DIRECT_CALL=2,
    BR_INDIRECT_NORMAL=3,
    BR_INDIRECT_RET=4,
    BR_INDIRECT_CALL=5,
    BR_INDIRECT_CALL_RET=6,
    BR_DIRECT_RET=7
};

enum class OverrideReason
{
    NO_OVERRIDE,
    FALL_THRU,
    CONTROL_ADDR,
    TARGET,
    END,
    HIST_INFO
};

enum class HistoryType
{
    GLOBAL,
    GLOBALBW,
    LOCAL,
    IMLI,
    PATH
};


/**
 * @brief Branch information structure containing branch properties and targets
 * 
 * Stores essential information about a branch instruction including:
 * - PC and target address
 * - Branch type (conditional, indirect, call, return)
 * - Instruction size
 */
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
    // BranchInfo(const Addr &pc, const Addr &target_pc, bool is_cond) :
    // pc(pc), target(target_pc), isCond(is_cond), isIndirect(false), isCall(false), isReturn(false), size(0) {}
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
    int getType() const {
        if (isCond) {
            return BR_COND;
        } else if (!isIndirect) { // uncond direct
            if (isReturn) {
                fatal("jal return detected!\n");
                return BR_DIRECT_RET;
            }
            if (!isCall) {
                return BR_DIRECT_NORMAL;
            } else {
                return BR_DIRECT_CALL;
            }
        } else {  // uncond indirect
            if (!isCall) {
                if (!isReturn) {
                    return BR_INDIRECT_NORMAL; // normal indirect
                } else {
                    return BR_INDIRECT_RET; // indirect return
                }
            } else {
                if (!isReturn) { // indirect call
                    return BR_INDIRECT_CALL;
                } else { // call & return
                    return BR_INDIRECT_CALL_RET;
                }
            }
        }
    }

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


/**
 * @brief Branch Target Buffer entry extending BranchInfo with prediction metadata
 * 
 * Contains branch information plus prediction state:
 * - Valid bit
 * - Always taken bit
 * - Counter for prediction
 * - Tag for BTB lookup
 */
typedef struct BTBEntry : BranchInfo
{
    bool valid;
    bool alwaysTaken;
    int ctr;
    Addr tag;
    // Addr offset; // retrived from lowest bits of pc
    BTBEntry() : valid(false), alwaysTaken(false), ctr(0), tag(0) {}
    BTBEntry(const BranchInfo &bi) : BranchInfo(bi), valid(true), alwaysTaken(true), ctr(0) {}
    BranchInfo getBranchInfo() { return BranchInfo(*this); }

}BTBEntry;

/**
 * @brief Tage prediction info for MGSC
 */
typedef struct TageInfoForMGSC
{
    // tage info
    bool tage_pred_taken;
    bool tage_pred_conf_high;
    bool tage_pred_conf_mid;
    bool tage_pred_conf_low;
    bool tage_pred_alt_diff;

    // Addr offset; // retrived from lowest bits of pc
    TageInfoForMGSC() : tage_pred_taken(false), tage_pred_conf_high(false),
                        tage_pred_conf_mid(false), tage_pred_conf_low(false),
                        tage_pred_alt_diff(false){}
    TageInfoForMGSC(bool tage_pred_taken, bool tage_pred_conf_high, bool tage_pred_conf_mid,
                    bool tage_pred_conf_low, bool tage_pred_alt_diff) :
                    tage_pred_taken(tage_pred_taken), tage_pred_conf_high(tage_pred_conf_high),
                    tage_pred_conf_mid(tage_pred_conf_mid), tage_pred_conf_low(tage_pred_conf_low),
                    tage_pred_alt_diff(tage_pred_alt_diff) {}

}TageInfoForMGSC;

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


using FetchStreamId = uint64_t;
using FetchTargetId = uint64_t;

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
    int jumpAheadBlockNum; // number of non-predicted blocks ahead
    int conf; // confidence
    JAEntry() : jumpAheadBlockNum(0), conf(0) {}
    Addr getJumpTarget(Addr indexPC, int blockSize) {
        return indexPC + jumpAheadBlockNum * blockSize;
    }
} JAEntry;


/**
 * @brief Fetch Stream representing a sequence of instructions with prediction info
 * 
 * Key structure for decoupled frontend that contains:
 * - Stream boundaries (start PC, end PC)
 * - Prediction information (branch info, targets)
 * - Execution results for verification
 * - Loop and jump-ahead prediction state
 * - Statistics for profiling
 */
typedef struct FetchStream
{
    Addr startPC;       // start pc of the stream
    bool predTaken;     // whether the FetchStream has taken branch
    Addr predEndPC;     // predicted stream end pc (fall through pc)
    BranchInfo predBranchInfo; // predicted branch info

    bool isHit;          // whether the predicted btb entry is hit
    bool falseHit;       // not used
    std::vector<BTBEntry> predBTBEntries;   // record predicted BTB entries

    // for commit, write at redirect or fetch
    bool exeTaken;         // whether the branch is taken(resolved)
    BranchInfo exeBranchInfo; // executed branch info

    BTBEntry updateNewBTBEntry; // the possible new entry, set by L1BTB.getAndSetNewBTBEntry, used by L1BTB/L0BTB.update
    bool updateIsOldEntry; // whether the BTB entry is old, true: update the old entry, false: use updateNewBTBEntry
    bool resolved;  // whether the branch is resolved/executed

    // below two should be set before components update
    // used to decide which branches to update (don't update if not actually executed)
    Addr updateEndInstPC;   // end pc of the squash inst/taken inst
    // for components to decide which entries to update
    std::vector<BTBEntry> updateBTBEntries; // mostly like predBTBEntries

    int squashType;         // squash type
    Addr squashPC;         // pc of the squash inst
    unsigned predSource;   // source of the prediction(numStage)
    OverrideReason overrideReason; // reason of the override(for profiling)

    // prediction metas
    // FIXME: use vec
    std::array<std::shared_ptr<void>, 7> predMetas; // each component has a meta, TODO

    Tick predTick;         // tick of the prediction
    boost::dynamic_bitset<> history; // record GHR/s0History
    boost::dynamic_bitset<> phistory; // record PATH/s0History
    boost::dynamic_bitset<> bwhistory; // record BWHR/s0History
    boost::dynamic_bitset<> ihistory; // record IHR/s0History
    std::vector<boost::dynamic_bitset<>> lhistory; // record LHR/s0History
    std::queue<Addr> previousPCs; // previous PCs, used by ahead BTB

    // for profiling
    int fetchInstNum;
    int commitInstNum;

   FetchStream()
       : startPC(0),
         predTaken(false),
         predEndPC(0),
         predBranchInfo(BranchInfo()),
         isHit(false),
         falseHit(false),
         exeTaken(false),
         exeBranchInfo(BranchInfo()),
         updateNewBTBEntry(BTBEntry()),
         updateIsOldEntry(false),
         resolved(false),
         updateEndInstPC(0),
         squashType(SquashType::SQUASH_NONE),
         squashPC(0),
         predSource(0),
         predTick(0),
         history(),
         phistory(),
         bwhistory(),
         ihistory(),
         lhistory(),
         fetchInstNum(0),
         commitInstNum(0)
   {
       predMetas.fill(nullptr);
       predBTBEntries.clear();
       updateBTBEntries.clear();
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

    Addr getRealStartPC() const {
        return startPC;
    }

    std::pair<int, bool> getHistInfoDuringSquash(Addr squash_pc, bool is_cond, bool actually_taken)
    {
        int shamt = 0;
        bool cond_taken = false;
        for (auto &entry : predBTBEntries) {
            if (entry.valid && entry.pc >= startPC && entry.pc < squash_pc) {
                shamt++;
            }
        }
        if (is_cond) {
            shamt++;
            cond_taken = actually_taken;
        }
        return std::make_pair(shamt, cond_taken);
    }

    std::pair<int, bool> getBwHistInfoDuringSquash(Addr squash_pc, bool is_cond, bool actually_taken, Addr target)
    {
        int shamt = 0;
        bool cond_taken = false;
        for (auto &entry : predBTBEntries) {
            if (entry.valid && entry.pc >= startPC && entry.pc < squash_pc) {
                shamt++;
            }
        }
        if (is_cond) {
            shamt++;
            cond_taken = actually_taken && (squash_pc > target);
        }
        return std::make_pair(shamt, cond_taken);
    }
    
    // should be called before components update
    void setUpdateInstEndPC(unsigned predictWidth)
    {
        if (squashType == SQUASH_NONE) {
            if (exeTaken) { // taken inst pc
                updateEndInstPC = getControlPC();
            } else { // natural fall through, align to the next block
                // assert(halfAligned);
                updateEndInstPC = (startPC + predictWidth) & ~mask(floorLog2(predictWidth) - 1);
            }
        } else {
            updateEndInstPC = squashPC;
        }
    }

    // should be called before components update, after setUpdateInstEndPC
    void setUpdateBTBEntries()
    {
        updateBTBEntries.clear();
        for (auto &entry : predBTBEntries) {
            if (entry.valid && entry.pc >= startPC && entry.pc <= updateEndInstPC) {
                updateBTBEntries.push_back(entry);
            }
        }
    }

}FetchStream;

/**
 * @brief Full branch prediction combining predictions from all predictors
 * 
 * Aggregates predictions from:
 * - BTB entries for targets
 * - Direction predictors for conditional branches
 * - Indirect predictors for indirect branches
 * - RAS for return instructions
 */
typedef struct FullBTBPrediction
{
    Addr bbStart;
    std::vector<BTBEntry> btbEntries; // for BTB, only assigned when hit, sorted by inst order
    // for conditional branch predictors, mapped with lowest bits of branches
    std::map<Addr, bool> condTakens;

    // for indirect predictor, mapped with lowest bits of branches
    std::map<Addr, Addr> indirectTargets; // {branch pc -> target pc} maps
    Addr returnTarget; // for RAS

    std::map<Addr, TageInfoForMGSC> tageInfoForMgscs;

    unsigned predSource;
    OverrideReason overrideReason;
    Tick predTick;

    FullBTBPrediction() :
        bbStart(0),
        btbEntries(),
        condTakens(),
        indirectTargets(),
        returnTarget(0),
        predSource(0),
        predTick(0) {}

    BTBEntry getTakenEntry() {
        // IMPORTANT: assume entries are sorted
        for (auto &entry : this->btbEntries) {
            // hit
            if (entry.valid) {
                if (entry.isCond) {
                    // find corresponding direction pred in condTakens
                    // TODO: use lower-bit offset of branch instruction
                    auto it = condTakens.find(entry.pc);
                    if (it != condTakens.end()) {
                        if (it->second) {   // find and taken, return the entry
                            return entry;
                        }
                    }
                }
                if (entry.isUncond()) { // find the first uncond entry
                    return entry;
                }
            }
        }
        return BTBEntry(); // not found, return empty entry
    }

    bool isTaken() {
        return getTakenEntry().valid;   // if find a taken entry, return true
    }

    Addr getFallThrough(Addr predictWidth) {
        // max 64 byte block, 32 byte aligned
        return (bbStart + predictWidth) & ~mask(floorLog2(predictWidth) - 1);
    }

    Addr getTarget(Addr predictWidth) {
        Addr target;
        const auto &entry = getTakenEntry();
        if (entry.valid) { // found a taken entry
            target = entry.target;
            // indirect target should come from ipred or ras,
            // or btb itself when ipred miss
            if (entry.isIndirect) {
                if (!entry.isReturn) { // normal indirect, see ittage
                    auto it = indirectTargets.find(entry.pc);
                    if (it != indirectTargets.end()) { // found in ittage, use it
                        target = it->second;
                    }
                } else { // indirect return, use RAS target
                    target = returnTarget;
                }
            } // else: normal taken, use btb target
        } else {
            target = getFallThrough(predictWidth);
        }
        return target;
    }

    Addr getEnd(Addr predictWidth) {
        if (isTaken()) {
            return getTakenEntry().getEnd();
        } else {
            return getFallThrough(predictWidth);
        }
    }


    Addr controlAddr() {
        return getTakenEntry().pc;
    }

    std::pair<bool, OverrideReason> match(FullBTBPrediction &other, Addr predictWidth)
    {
        auto this_taken_entry = this->getTakenEntry();
        auto other_taken_entry = other.getTakenEntry();
        if (this_taken_entry.valid != other_taken_entry.valid) {
            return std::make_pair(false, OverrideReason::FALL_THRU);
        } else {
            // all taken or all not taken, check target and end
            if (this_taken_entry.valid && other_taken_entry.valid) {
                if (this->controlAddr() != other.controlAddr()) {
                    return std::make_pair(false, OverrideReason::CONTROL_ADDR);
                }
                else if (this->getTarget(predictWidth) != other.getTarget(predictWidth)) {
                    return std::make_pair(false, OverrideReason::TARGET);
                }
                else if (this->getEnd(predictWidth) != other.getEnd(predictWidth)) {
                    // execution will never come here
                    return std::make_pair(false, OverrideReason::END);
                }
                else if (this->getHistInfo() != other.getHistInfo()) {
                    return std::make_pair(false, OverrideReason::HIST_INFO);
                }
                else {
                    return std::make_pair(true, btb_pred::OverrideReason::NO_OVERRIDE);
                }
            } else {
                return std::make_pair(true, btb_pred::OverrideReason::NO_OVERRIDE);
            }
        }
    }

    std::vector<boost::dynamic_bitset<>> indexFoldedHist;
    std::vector<boost::dynamic_bitset<>> tagFoldedHist;

    std::pair<int, bool> getHistInfo()  //global or local
    {
        int shamt = 0; // shamt is the number of bits to shift in history update
        bool taken = false;
        for (auto &entry : btbEntries) {
            if (entry.valid) {
                if (entry.isCond) { // if found a cond branch, shamt++
                    shamt++;
                    auto it = condTakens.find(entry.pc);
                    if (it != condTakens.end()) {
                        if (it->second) { // if the cond branch is taken, taken = true
                            taken = true;
                            break;
                        }
                    }
                } else {
                    // uncond
                    break;
                }
            }
        }
        // For example, return (3, true) means 3 bits to shift in history update,
        // and the third branch is taken, new hist = xxx001
        return std::make_pair(shamt, taken);
    }

    std::pair<int, bool> getBwHistInfo() //global backward or imli
    {
        int shamt = 0;
        bool taken = false;
        for (auto &entry : btbEntries) {
            if (entry.valid) {
                if (entry.isCond) {
                    shamt++;
                    auto it = condTakens.find(entry.pc);
                    if (it != condTakens.end()) {
                        if (it->second) {
                            taken = (entry.target < entry.pc); // branch is backward if target < pc
                            break;
                        }
                    }
                } else {
                    // uncond
                    break;
                }
            }
        }
        return std::make_pair(shamt, taken);
    }

    std::pair<Addr, bool> getPHistInfo() //path
    {
        bool taken = false;
        Addr pc = 0;
        for (auto &entry : btbEntries) {
            if (entry.valid) {
                if (entry.isCond) {
                    auto it = condTakens.find(entry.pc);
                    if (it != condTakens.end()) {
                        if (it->second) {
                            taken = true;
                            pc = entry.pc; // get the pc of the cond branch
                            break;
                        }
                    }
                } else {
                    // uncond
                    break;
                }
            }
        }
        return std::make_pair(pc, taken);
    }

}FullBTBPrediction;

/**
 * @brief Fetch Target Queue entry representing a fetch block
 * 
 * Contains information needed for instruction fetch:
 * - Address range (start PC, end PC)
 * - Branch information (taken PC, target)
 * - Loop information for loop buffer
 * - Stream tracking (FSQ ID)
 */
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
};


struct TageMissTrace : public Record {
    void set(uint64_t startPC, uint64_t branchPC, uint64_t wayIdx,
        uint64_t mainFound, uint64_t mainCounter, uint64_t mainUseful, uint64_t mainTable, uint64_t mainIndex,
        uint64_t altFound, uint64_t altCounter, uint64_t altUseful, uint64_t altTable, uint64_t altIndex,
        uint64_t useAlt, uint64_t predTaken, uint64_t actualTaken, uint64_t allocSuccess)
    {
        _tick = curTick();
        _uint64_data["startPC"] = startPC;
        _uint64_data["branchPC"] = branchPC;
        _uint64_data["wayIdx"] = wayIdx;
        _uint64_data["mainFound"] = mainFound;
        _uint64_data["mainCounter"] = mainCounter;
        _uint64_data["mainUseful"] = mainUseful;
        _uint64_data["mainTable"] = mainTable;
        _uint64_data["mainIndex"] = mainIndex;
        _uint64_data["altFound"] = altFound;
        _uint64_data["altCounter"] = altCounter;
        _uint64_data["altUseful"] = altUseful;
        _uint64_data["altTable"] = altTable;
        _uint64_data["altIndex"] = altIndex;
        _uint64_data["useAlt"] = useAlt;
        _uint64_data["predTaken"] = predTaken;
        _uint64_data["actualTaken"] = actualTaken;
        _uint64_data["allocSuccess"] = allocSuccess;
    }
};

struct LoopTrace : public Record {
    void set(uint64_t pc, uint64_t target, uint64_t mispred, uint64_t training,
        uint64_t trainSpecCnt, uint64_t trainTripCnt, uint64_t trainConf,
        uint64_t inMain, uint64_t mainTripCnt, uint64_t mainConf, uint64_t predSpecCnt,
        uint64_t predTripCnt, uint64_t predConf)
    {
        _tick = curTick();
        _uint64_data["pc"] = pc;
        _uint64_data["target"] = target;
        _uint64_data["mispred"] = mispred;
        _uint64_data["predSpecCnt"] = predSpecCnt;
        _uint64_data["predTripCnt"] = predTripCnt;
        _uint64_data["predConf"] = predConf;
        // from lp
        _uint64_data["training"] = training;
        _uint64_data["trainSpecCnt"] = trainSpecCnt;
        _uint64_data["trainTripCnt"] = trainTripCnt;
        _uint64_data["trainConf"] = trainConf;
        _uint64_data["inMain"] = inMain;
        _uint64_data["mainTripCnt"] = mainTripCnt;
        _uint64_data["mainConf"] = mainConf;
    }
    void set_in_lp(uint64_t training, uint64_t trainSpecCnt, uint64_t trainTripCnt, uint64_t trainConf,
        uint64_t inMain, uint64_t mainTripCnt, uint64_t mainConf)
    {
        _uint64_data["training"] = training;
        _uint64_data["trainSpecCnt"] = trainSpecCnt;
        _uint64_data["trainTripCnt"] = trainTripCnt;
        _uint64_data["trainConf"] = trainConf;
        _uint64_data["inMain"] = inMain;
        _uint64_data["mainTripCnt"] = mainTripCnt;
        _uint64_data["mainConf"] = mainConf;
    }
    void set_outside_lp(uint64_t pc, uint64_t target, uint64_t mispred,
        uint64_t predSpecCnt, uint64_t predTripCnt, uint64_t predConf)
    {
        _tick = curTick();
        _uint64_data["pc"] = pc;
        _uint64_data["target"] = target;
        _uint64_data["mispred"] = mispred;
        _uint64_data["predSpecCnt"] = predSpecCnt;
        _uint64_data["predTripCnt"] = predTripCnt;
        _uint64_data["predConf"] = predConf;
    }
};

struct BTBTrace : public Record {
    // mode: read, write, evict
    void set(uint64_t pc, uint64_t brType, uint64_t target, uint64_t idx, uint64_t mode, uint64_t hit) {
        _tick = curTick();
        _uint64_data["pc"] = pc;
        _uint64_data["brType"] = brType;
        _uint64_data["target"] = target;
        _uint64_data["idx"] = idx;
        _uint64_data["mode"] = mode;
        _uint64_data["hit"] = hit;
    }
};

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5
#endif  // __CPU_PRED_BTB_STREAM_STRUCT_HH__
