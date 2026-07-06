#ifndef __CPU_PRED_BTB_STREAM_STRUCT_HH__
#define __CPU_PRED_BTB_STREAM_STRUCT_HH__

#include <algorithm>
#include <array>
#include <memory>
#include <queue>
#include <string>
#include <unordered_map>

#include <boost/dynamic_bitset.hpp>

// #include "arch/generic/pcstate.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/general_arch_db.hh"
#include "cpu/static_inst.hh"

namespace gem5 {

namespace branch_prediction {

namespace btb_pred {

inline uint8_t
foldAsidHash16To4(uint16_t asid)
{
    return (asid & 0xf) ^ ((asid >> 4) & 0xf) ^
           ((asid >> 8) & 0xf) ^ ((asid >> 12) & 0xf);
}

inline Addr
expandAsidHash(uint8_t asid_hash, unsigned bits)
{
    if (bits == 0) {
        return 0;
    }

    Addr expanded = 0;
    for (unsigned shift = 0; shift < bits; shift += 4) {
        expanded |= static_cast<Addr>(asid_hash) << shift;
    }
    return expanded & mask(bits);
}

inline Addr
injectAsidHashIntoTag(Addr base_tag, unsigned tag_bits, uint8_t asid_hash)
{
    if (tag_bits == 0) {
        return 0;
    }

    const unsigned hash_bits = std::min<unsigned>(4, tag_bits);
    const Addr hash_mask = mask(hash_bits);
    return (base_tag ^ (static_cast<Addr>(asid_hash) & hash_mask)) &
           mask(tag_bits);
}

inline Addr
xorAsidHashIntoIndex(Addr base_index, unsigned index_bits, uint8_t asid_hash)
{
    if (index_bits == 0) {
        return 0;
    }

    return (base_index ^ expandAsidHash(asid_hash, index_bits)) & mask(index_bits);
}

enum EndType
{
    END_CALL=0,
    END_RET,
    END_OTHER_TAKEN,
    END_NOT_TAKEN,
    END_CONT,  // to be continued
    END_NONE
};

enum SquashType
{
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
    TARGET
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
 * - Resolved bit
 * - Branch type (conditional, indirect, call, return)
 * - Instruction size
 */
struct BranchInfo
{
    Addr pc;
    Addr target;
    // An independent resolved bit to indicate whether CFI is resolved
    // or not for training, which is trained in resolve stage so
    // it's necessary to know whether the branch is resolved and skip
    // the BTB entry or not.
    bool resolved;
    bool isCond;
    bool isIndirect;
    bool isDirect;
    bool isCall;
    bool isReturn;
    uint8_t size;
    bool isUncond() const { return !this->isCond; }
    Addr getEnd() { return this->pc + this->size; }
    BranchInfo()
        : pc(0), target(0), resolved(false), isCond(false), isIndirect(false), isCall(false), isReturn(false), size(0)
    {
    }
    // BranchInfo(const Addr &pc, const Addr &target_pc, bool is_cond) :
    // pc(pc), target(target_pc), isCond(is_cond), isIndirect(false), isCall(false), isReturn(false), size(0) {}
    BranchInfo(const Addr &control_pc, const Addr &target_pc, const StaticInstPtr &static_inst, unsigned size)
        : pc(control_pc),
          target(target_pc),
          resolved(false),
          isCond(static_inst->isCondCtrl()),
          isIndirect(static_inst->isIndirectCtrl()),
          isDirect(static_inst->isDirectCtrl()),
          isCall(static_inst->isCall()),
          isReturn(static_inst->isReturn() && !static_inst->isNonSpeculative() && !static_inst->isDirectCtrl()),
          size(size)
    {
    }
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
};


/**
 * @brief Branch Target Buffer entry extending BranchInfo with prediction metadata
 *
 * Contains branch information plus prediction state:
 * - Valid bit
 * - Always taken bit
 * - Counter for prediction
 * - Tag for BTB lookup
 */
struct BTBEntry : BranchInfo
{
    bool valid;
    bool alwaysTaken;
    int ctr;
    Addr tag;
    int source;//only use for countering the source of the entry
    // Addr offset; // retrived from lowest bits of pc
    BTBEntry() : BranchInfo(), valid(false), alwaysTaken(false), ctr(0), tag(0) ,source(-1){}
    BTBEntry(const BranchInfo &bi) : BranchInfo(bi), valid(true), alwaysTaken(true), ctr(0),source(-1){}
    BranchInfo getBranchInfo() { return BranchInfo(*this); }

    int getsource() const {
        return source;
    }

    void setsource(int src) {
        source = src;
    }
};

/**
 * @brief Tage prediction info for MGSC
 */
struct TageInfoForMGSC
{
    // tage info
    bool tage_pred_taken;
    bool tage_main_taken;
    bool tage_pred_conf_high;
    bool tage_pred_conf_mid;
    bool tage_pred_conf_low;
    bool tage_pred_alt_diff;
    int tage_provider_table;
    int tage_final_provider_table;
    bool tage_final_provider_is_alt;
    bool primary_pred_taken;
    bool primary_provider_is_llbpx;
    int primary_provider_table;
    bool llbpx_provider_used;
    bool llbpx_pred_valid;
    bool llbpx_pred_taken;
    int llbpx_provider_table;

    // Addr offset; // retrived from lowest bits of pc
    TageInfoForMGSC()
        : tage_pred_taken(false),
            tage_main_taken(false),
            tage_pred_conf_high(false),
            tage_pred_conf_mid(false),
            tage_pred_conf_low(false),
            tage_pred_alt_diff(false),
            tage_provider_table(-1),
            tage_final_provider_table(-1),
            tage_final_provider_is_alt(false),
            primary_pred_taken(false),
            primary_provider_is_llbpx(false),
            primary_provider_table(-1),
            llbpx_provider_used(false),
            llbpx_pred_valid(false),
            llbpx_pred_taken(false),
            llbpx_provider_table(-1)
    {
    }
    TageInfoForMGSC(bool tage_pred_taken, bool tage_main_taken, bool tage_pred_conf_high, bool tage_pred_conf_mid,
                    bool tage_pred_conf_low, bool tage_pred_alt_diff)
        : tage_pred_taken(tage_pred_taken),
            tage_main_taken(tage_main_taken),
            tage_pred_conf_high(tage_pred_conf_high),
            tage_pred_conf_mid(tage_pred_conf_mid),
            tage_pred_conf_low(tage_pred_conf_low),
            tage_pred_alt_diff(tage_pred_alt_diff),
            tage_provider_table(-1),
            tage_final_provider_table(-1),
            tage_final_provider_is_alt(false),
            primary_pred_taken(tage_pred_taken),
            primary_provider_is_llbpx(false),
            primary_provider_table(-1),
            llbpx_provider_used(false),
            llbpx_pred_valid(false),
            llbpx_pred_taken(false),
            llbpx_provider_table(-1)
    {
    }
};

struct LFSR64
{
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
};

using FetchTargetId = uint64_t;

enum class PairPhase : uint8_t
{
    Even = 0,
    Odd = 1
};

// {branch pc -> istaken} maps
using CondTakens = std::vector<std::pair<Addr, bool>>;
// {branch pc -> target pc} maps
using IndirectTargets = std::vector<std::pair<Addr, Addr>>;

#define CondTakens_find(condTakens, branch_pc) \
    std::find_if(condTakens.begin(), condTakens.end(), \
                 [&branch_pc](const auto &p) { return p.first == branch_pc; })
#define IndirectTakens_find(indirectTargets, branch_pc) \
    std::find_if(indirectTargets.begin(), indirectTargets.end(), \
                 [&branch_pc](const auto &p) { return p.first == branch_pc; })

#define FillStageLoop(x) for (int x = getDelay(); x < stagePreds.size(); ++x)

struct DirectionHistoryUpdate
{
    int shamt = 0;
    bool taken = false;
};

struct PathHistoryUpdate
{
    static constexpr int NumShift = 2;

    int shamt = NumShift;
    bool taken = false;
    Addr pc = 0;
    Addr target = 0;
};

static constexpr size_t MaxBTBPredComponents = 16;

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
struct FetchTarget
{
    ThreadID tid;
    uint8_t asidHash;
    Addr startPC;       // start pc of the stream
    bool predTaken;     // whether the FetchTarget has taken branch
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
    // Update side channel: TAGE publishes successful allocation tables so
    // LLBP-X can allocate matching long-history patterns after TAGE updates.
    mutable std::unordered_map<Addr, unsigned> tageAllocatedTables;

    int squashType;         // squash type
    Addr squashPC;         // pc of the squash inst
    unsigned predSource;   // source of the prediction(numStage)
    OverrideReason overrideReason; // reason of the override(for profiling)
    PairPhase pairPhase;   // PairTAGE logical phase of this block start

    // prediction metas
    std::array<std::shared_ptr<void>, MaxBTBPredComponents> predMetas;

    Tick predTick;         // tick of the prediction
    boost::dynamic_bitset<> history; // record GHR/s0History
    boost::dynamic_bitset<> phistory; // record PATH/s0History
    boost::dynamic_bitset<> bwhistory; // record BWHR/s0History
    std::vector<boost::dynamic_bitset<>> lhistory; // record LHR/s0History
    std::queue<Addr> previousPCs; // previous PCs, used by ahead BTB

    // for profiling
    int fetchInstNum;
    int commitInstNum;

    int s1Source; // which stage the prediction comes from
    int s3Source; // which stage the prediction comes from

   FetchTarget()
       : tid(0),
         asidHash(0),
         startPC(0),
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
         pairPhase(PairPhase::Even),
         predTick(0),
         history(),
         phistory(),
         bwhistory(),
         lhistory(),
         fetchInstNum(0),
         commitInstNum(0),
         s1Source(-1),
         s3Source(-1)
   {
       predMetas.fill(nullptr);
       predBTBEntries.clear();
       updateBTBEntries.clear();
       tageAllocatedTables.clear();
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

    DirectionHistoryUpdate getGHistUpdateDuringSquash(
        Addr squash_pc, bool is_cond, bool actually_taken) const
    {
        DirectionHistoryUpdate update;
        for (auto &entry : predBTBEntries) {
            if (entry.valid && entry.pc >= startPC && entry.pc < squash_pc) {
                update.shamt++;
            }
        }
        if (is_cond) {
            update.shamt++;
            update.taken = actually_taken;
        }
        return update;
    }

    DirectionHistoryUpdate getBwHistUpdateDuringSquash(
        Addr squash_pc, bool is_cond, bool actually_taken, Addr target) const
    {
        DirectionHistoryUpdate update;
        for (auto &entry : predBTBEntries) {
            if (entry.valid && entry.pc >= startPC && entry.pc < squash_pc) {
                update.shamt++;
            }
        }
        if (is_cond) {
            update.shamt++;
            update.taken = actually_taken && (squash_pc > target);
        }
        return update;
    }

    PathHistoryUpdate getPHistUpdateDuringSquash(
        Addr squash_pc, bool actually_taken, Addr target) const
    {
        PathHistoryUpdate update;
        update.taken = actually_taken && getControlPC() == squash_pc;
        if (update.taken) {
            update.pc = squash_pc;
            update.target = target;
        }
        return update;
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

    // Argument resolved pc could not match any BTB entry branch pc,
    // Just ignore it in that case.
    void markBTBEntryResolved(Addr resolvedInstPC)
    {
        for (auto &entry : updateBTBEntries) {
            if (entry.valid && entry.pc == resolvedInstPC) {
                entry.resolved = true;
            }
        }
    }
};
/**
 * @brief Full branch prediction combining predictions from all predictors
 *
 * Aggregates predictions from:
 * - BTB entries for targets
 * - Direction predictors for conditional branches
 * - Indirect predictors for indirect branches
 * - RAS for return instructions
 */
struct FullBTBPrediction
{
    ThreadID tid;
    uint8_t asidHash;
    Addr bbStart;
    std::vector<BTBEntry> btbEntries; // for BTB, only assigned when hit, sorted by inst order
    // for conditional branch predictors, mapped with lowest bits of branches
    CondTakens condTakens;

    // for indirect predictor, mapped with lowest bits of branches
    IndirectTargets indirectTargets;
    Addr returnTarget; // for RAS

    std::unordered_map<Addr, TageInfoForMGSC> tageInfoForMgscs;
    unsigned predSource;
    OverrideReason overrideReason;
    Tick predTick;

    //only use for countering the source of the prediction
    int s1Source;
    int s3Source;
    FullBTBPrediction() :
        tid(0),
        asidHash(0),
        bbStart(0),
        btbEntries(),
        condTakens(),
        indirectTargets(),
        returnTarget(0),
        tageInfoForMgscs(),
        predSource(0),
        overrideReason(OverrideReason::NO_OVERRIDE),
        predTick(0),
        s1Source(-1),
        s3Source(-1) {}

    BTBEntry getTakenEntry() {
        // IMPORTANT: assume entries are sorted
        for (auto &entry : this->btbEntries) {
            // hit
            if (entry.valid) {
                if (entry.isCond) {
                    // find corresponding direction pred in condTakens
                    // TODO: use lower-bit offset of branch instruction
                    auto& pc = entry.pc;
                    auto it = CondTakens_find(condTakens, pc);
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

    Addr getEntryTarget(const BTBEntry &entry) {
        Addr target = entry.target;
        // indirect target should come from ipred or ras,
        // or btb itself when ipred miss
        if (entry.isIndirect) {
            if (!entry.isReturn) { // normal indirect, see ittage
                auto& pc = entry.pc;
                auto it = IndirectTakens_find(indirectTargets, pc);
                if (it != indirectTargets.end()) { // found in ittage, use it
                    target = it->second;
                }
            } else { // indirect return, use RAS target
                target = returnTarget;
            }
        } // else: normal taken, use btb target
        return target;
    }

    Addr getTarget(Addr predictWidth) {
        Addr target;
        const auto &entry = getTakenEntry();
        if (entry.valid) { // found a taken entry
            target = getEntryTarget(entry);
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
                else {
                    return std::make_pair(true, btb_pred::OverrideReason::NO_OVERRIDE);
                }
            } else {
                return std::make_pair(true, btb_pred::OverrideReason::NO_OVERRIDE);
            }
        }
    }

    DirectionHistoryUpdate getGHistUpdate()  //global or local
    {
        DirectionHistoryUpdate update; // shamt is the number of bits to shift in history update
        for (auto &entry : btbEntries) {
            if (entry.valid) {
                if (entry.isCond) { // if found a cond branch, shamt++
                    update.shamt++;
                    auto& pc = entry.pc;
                    auto it = CondTakens_find(condTakens, pc);
                    if (it != condTakens.end()) {
                        if (it->second) { // if the cond branch is taken, taken = true
                            update.taken = true;
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
        return update;
    }

    DirectionHistoryUpdate getBwHistUpdate() //global backward or imli
    {
        DirectionHistoryUpdate update;
        for (auto &entry : btbEntries) {
            if (entry.valid) {
                if (entry.isCond) {
                    update.shamt++;
                    auto& pc = entry.pc;
                    auto it = CondTakens_find(condTakens, pc);
                    if (it != condTakens.end()) {
                        if (it->second) {
                            update.taken = (entry.target < entry.pc); // branch is backward if target < pc
                            break;
                        }
                    }
                } else {
                    // uncond
                    break;
                }
            }
        }
        return update;
    }

    PathHistoryUpdate getPHistUpdate() //path
    {
        PathHistoryUpdate update;
        const auto &entry = getTakenEntry();
        if (entry.valid) {
            update.taken = true;
            update.pc = entry.pc;
            update.target = getEntryTarget(entry);
        }
        return update;
    }

};

struct TageMissTrace : public Record
{
    void set(uint64_t startPC, uint64_t branchPC, uint64_t wayIdx,
        uint64_t mainFound, uint64_t mainCounter, uint64_t mainUseful,
        uint64_t mainTable, uint64_t mainIndex, uint64_t mainTag,
        uint64_t altFound, uint64_t altCounter, uint64_t altUseful,
        uint64_t altTable, uint64_t altIndex, uint64_t altTag,
        uint64_t useAlt, uint64_t predTaken, uint64_t actualTaken, uint64_t allocSuccess,
        uint64_t allocTable, uint64_t allocIndex, uint64_t allocWay,
        uint64_t allocTag,
        uint64_t victimValid, uint64_t victimTag,
        uint64_t victimCounter, uint64_t victimUseful, uint64_t victimPC,
        std::string history, std::string phistory, uint64_t indexFoldedHist,
        uint64_t useAltIdx, uint64_t useAltCtr, uint64_t hitTableMask,
        uint64_t finalProviderTable, uint64_t finalProviderIsAlt,
        uint64_t historyHash, uint64_t phistoryHash,
        uint64_t indexFoldedHistHash, uint64_t tagFoldedHistHash,
        uint64_t altTagFoldedHistHash)
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
        _uint64_data["mainTag"] = mainTag;
        _uint64_data["altFound"] = altFound;
        _uint64_data["altCounter"] = altCounter;
        _uint64_data["altUseful"] = altUseful;
        _uint64_data["altTable"] = altTable;
        _uint64_data["altIndex"] = altIndex;
        _uint64_data["altTag"] = altTag;
        _uint64_data["useAlt"] = useAlt;
        _uint64_data["predTaken"] = predTaken;
        _uint64_data["actualTaken"] = actualTaken;
        _uint64_data["allocSuccess"] = allocSuccess;
        _uint64_data["allocTable"] = allocTable;
        _uint64_data["allocIndex"] = allocIndex;
        _uint64_data["allocWay"] = allocWay;
        _uint64_data["allocTag"] = allocTag;
        _uint64_data["victimValid"] = victimValid;
        _uint64_data["victimTag"] = victimTag;
        _uint64_data["victimCounter"] = victimCounter;
        _uint64_data["victimUseful"] = victimUseful;
        _uint64_data["victimPC"] = victimPC;
        _text_data["history"] = history;
        _text_data["phistory"] = phistory;
        _uint64_data["indexFoldedHist"] = indexFoldedHist;
        _uint64_data["useAltIdx"] = useAltIdx;
        _uint64_data["useAltCtr"] = useAltCtr;
        _uint64_data["hitTableMask"] = hitTableMask;
        _uint64_data["finalProviderTable"] = finalProviderTable;
        _uint64_data["finalProviderIsAlt"] = finalProviderIsAlt;
        _uint64_data["historyHash"] = historyHash;
        _uint64_data["phistoryHash"] = phistoryHash;
        _uint64_data["indexFoldedHistHash"] = indexFoldedHistHash;
        _uint64_data["tagFoldedHistHash"] = tagFoldedHistHash;
        _uint64_data["altTagFoldedHistHash"] = altTagFoldedHistHash;
    }
};

struct LlbpxTrace : public Record
{
    void set(uint64_t startPC, uint64_t branchPC, uint64_t actualTaken,
        uint64_t basePred, uint64_t llbpxPred, uint64_t providerUsed,
        uint64_t overridden, uint64_t directionChanged,
        uint64_t providerDepthEligible, uint64_t providerTimingReady,
        uint64_t providerInfoMissing, uint64_t contextHit, uint64_t patternHit,
        uint64_t patternBufferInFlight, uint64_t wi, uint64_t allocTargetWi,
        uint64_t baseProviderHistIdxBias, uint64_t hitHistIdxBias,
        uint64_t keyTable, uint64_t cid, uint64_t allocTargetCid,
        uint64_t bcid, uint64_t pcid, uint64_t pbcid, uint64_t key,
        uint64_t contextTag, uint64_t patternTag,
        uint64_t tageAllocTriggered, uint64_t tageAllocTableMask,
        uint64_t allocTableMask, uint64_t allocContextCreated,
        uint64_t allocContextRevisit, uint64_t allocPatternCreated,
        uint64_t allocPatternRevisit, uint64_t allocSkipNoKey,
        uint64_t updatePatternCalled, uint64_t updatePatternFound,
        uint64_t counterBias, uint64_t counterBeforeBiased,
        uint64_t counterAfterBiased)
    {
        _tick = curTick();
        _uint64_data["startPC"] = startPC;
        _uint64_data["branchPC"] = branchPC;
        _uint64_data["actualTaken"] = actualTaken;
        _uint64_data["basePred"] = basePred;
        _uint64_data["llbpxPred"] = llbpxPred;
        _uint64_data["providerUsed"] = providerUsed;
        _uint64_data["overridden"] = overridden;
        _uint64_data["directionChanged"] = directionChanged;
        _uint64_data["providerDepthEligible"] = providerDepthEligible;
        _uint64_data["providerTimingReady"] = providerTimingReady;
        _uint64_data["providerInfoMissing"] = providerInfoMissing;
        _uint64_data["contextHit"] = contextHit;
        _uint64_data["patternHit"] = patternHit;
        _uint64_data["patternBufferInFlight"] = patternBufferInFlight;
        _uint64_data["wi"] = wi;
        _uint64_data["allocTargetWi"] = allocTargetWi;
        _uint64_data["baseProviderHistIdxBias"] = baseProviderHistIdxBias;
        _uint64_data["hitHistIdxBias"] = hitHistIdxBias;
        _uint64_data["keyTable"] = keyTable;
        _uint64_data["cid"] = cid;
        _uint64_data["allocTargetCid"] = allocTargetCid;
        _uint64_data["bcid"] = bcid;
        _uint64_data["pcid"] = pcid;
        _uint64_data["pbcid"] = pbcid;
        _uint64_data["key"] = key;
        _uint64_data["contextTag"] = contextTag;
        _uint64_data["patternTag"] = patternTag;
        _uint64_data["tageAllocTriggered"] = tageAllocTriggered;
        _uint64_data["tageAllocTableMask"] = tageAllocTableMask;
        _uint64_data["allocTableMask"] = allocTableMask;
        _uint64_data["allocContextCreated"] = allocContextCreated;
        _uint64_data["allocContextRevisit"] = allocContextRevisit;
        _uint64_data["allocPatternCreated"] = allocPatternCreated;
        _uint64_data["allocPatternRevisit"] = allocPatternRevisit;
        _uint64_data["allocSkipNoKey"] = allocSkipNoKey;
        _uint64_data["updatePatternCalled"] = updatePatternCalled;
        _uint64_data["updatePatternFound"] = updatePatternFound;
        _uint64_data["counterBias"] = counterBias;
        _uint64_data["counterBeforeBiased"] = counterBeforeBiased;
        _uint64_data["counterAfterBiased"] = counterAfterBiased;
    }
};

struct BTBTrace : public Record
{
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

struct MgscTrace : public Record
{
    void set(uint64_t branchPC,
        uint64_t bbStart, uint64_t branchOffset,
        // TAGE prediction info
        uint64_t tagePred, uint64_t tageConfHigh, uint64_t tageConfMid, uint64_t tageConfLow,
        // Percsum for each table (signed values stored as int64)
        int64_t bwPercsum, int64_t lPercsum, int64_t iPercsum,
        int64_t gPercsum, int64_t pPercsum, int64_t biasPercsum,
        // SC decision
        int64_t totalSum, int64_t totalThres, int64_t effectiveGate, int64_t margin,
        uint64_t bwIndex0, uint64_t bwIndex1,
        uint64_t lIndex0, uint64_t lIndex1,
        uint64_t iIndex0,
        uint64_t gIndex0, uint64_t gIndex1,
        uint64_t pIndex0, uint64_t pIndex1,
        uint64_t biasIndex0,
        uint64_t useSc, uint64_t scPred, uint64_t scWrong,
        // Result
        uint64_t actualTaken)
    {
        _tick = curTick();
        _uint64_data["branchPC"] = branchPC;
        _uint64_data["bbStart"] = bbStart;
        _uint64_data["branchOffset"] = branchOffset;
        // TAGE info
        _uint64_data["tagePred"] = tagePred;
        _uint64_data["tageConfHigh"] = tageConfHigh;
        _uint64_data["tageConfMid"] = tageConfMid;
        _uint64_data["tageConfLow"] = tageConfLow;
        // Percsum values (cast signed to uint64 for storage)
        _uint64_data["bwPercsum"] = static_cast<uint64_t>(bwPercsum);
        _uint64_data["lPercsum"] = static_cast<uint64_t>(lPercsum);
        _uint64_data["iPercsum"] = static_cast<uint64_t>(iPercsum);
        _uint64_data["gPercsum"] = static_cast<uint64_t>(gPercsum);
        _uint64_data["pPercsum"] = static_cast<uint64_t>(pPercsum);
        _uint64_data["biasPercsum"] = static_cast<uint64_t>(biasPercsum);
        // SC decision
        _uint64_data["totalSum"] = static_cast<uint64_t>(totalSum);
        _uint64_data["totalThres"] = static_cast<uint64_t>(totalThres);
        _uint64_data["effectiveGate"] = static_cast<uint64_t>(effectiveGate);
        _uint64_data["margin"] = static_cast<uint64_t>(margin);
        _uint64_data["bwIndex0"] = bwIndex0;
        _uint64_data["bwIndex1"] = bwIndex1;
        _uint64_data["lIndex0"] = lIndex0;
        _uint64_data["lIndex1"] = lIndex1;
        _uint64_data["iIndex0"] = iIndex0;
        _uint64_data["gIndex0"] = gIndex0;
        _uint64_data["gIndex1"] = gIndex1;
        _uint64_data["pIndex0"] = pIndex0;
        _uint64_data["pIndex1"] = pIndex1;
        _uint64_data["biasIndex0"] = biasIndex0;
        _uint64_data["useSc"] = useSc;
        _uint64_data["scPred"] = scPred;
        _uint64_data["scWrong"] = scWrong;
        // Result
        _uint64_data["actualTaken"] = actualTaken;
    }
};

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5
#endif  // __CPU_PRED_BTB_STREAM_STRUCT_HH__
