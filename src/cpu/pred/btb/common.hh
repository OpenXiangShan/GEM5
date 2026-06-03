#ifndef __CPU_PRED_BTB_STREAM_STRUCT_HH__
#define __CPU_PRED_BTB_STREAM_STRUCT_HH__

#include <algorithm>
#include <queue>
#include <string>

#include <boost/dynamic_bitset.hpp>

// #include "arch/generic/pcstate.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/btb/prediction_result.hh"
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
    TARGET,
    END,
    HIST_INFO
};

struct FinalPredictionMetadata
{
    unsigned firstMatchingStage = 0;
    OverrideReason overrideReason = OverrideReason::NO_OVERRIDE;
    int s1Source = -1;
    int s3Source = -1;
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
 * @brief Branch slot observed in a fetch block
 *
 * Stores essential information about a branch instruction slot including:
 * - PC and target address
 * - Resolved bit
 * - Branch type (conditional, indirect, call, return)
 * - Instruction size
 */
struct BranchSlot
{
  public:
    Addr pc;
    Addr target;
    // An independent resolved bit to indicate whether CFI is resolved
    // or not for training, which is trained in resolve stage so
    // it's necessary to know whether the branch is resolved and skip
    // the BTB entry or not.
    bool resolved;

  private:
    /**
     * Branch control-flow class and RAS action.
     *
     * This keeps branch kind and RAS behavior as one semantic attribute instead
     * of spreading legal combinations across public bools.
     */
    struct Attribute
    {
        Attribute() = default;

        static Attribute
        fromFlags(bool is_cond, bool is_indirect, bool is_direct,
                  bool is_call, bool is_return)
        {
            ControlType control_type = ControlType::None;
            if (is_cond) {
                control_type = ControlType::Conditional;
            } else if (is_indirect) {
                control_type = ControlType::Indirect;
            } else if (is_direct || is_call || is_return) {
                control_type = ControlType::Direct;
            }

            RasAction ras_action = RasAction::None;
            if (is_call && is_return) {
                ras_action = RasAction::PopAndPush;
            } else if (is_call) {
                ras_action = RasAction::Push;
            } else if (is_return) {
                ras_action = RasAction::Pop;
            }

            return Attribute(control_type, ras_action);
        }

        bool isConditional() const
        {
            return controlType == ControlType::Conditional;
        }

        bool isDirect() const { return controlType == ControlType::Direct; }

        bool isIndirect() const
        {
            return controlType == ControlType::Indirect;
        }

        bool isCall() const
        {
            return rasAction == RasAction::Push ||
                   rasAction == RasAction::PopAndPush;
        }

        bool isReturn() const
        {
            return rasAction == RasAction::Pop ||
                   rasAction == RasAction::PopAndPush;
        }

        bool needIttage() const { return isIndirect() && !isReturn(); }

      private:
        enum class ControlType : uint8_t
        {
            None,
            Conditional,
            Direct,
            Indirect
        };

        enum class RasAction : uint8_t
        {
            None,
            Push,
            Pop,
            PopAndPush
        };

        Attribute(ControlType control_type, RasAction ras_action)
            : controlType(control_type), rasAction(ras_action)
        {
        }

        ControlType controlType = ControlType::None;
        RasAction rasAction = RasAction::None;
    };

    Attribute attribute;

  public:
    uint8_t size;

    bool isCond() const { return attribute.isConditional(); }
    bool isIndirect() const { return attribute.isIndirect(); }
    bool isDirect() const { return attribute.isDirect(); }
    bool isCall() const { return attribute.isCall(); }
    bool isReturn() const { return attribute.isReturn(); }
    bool isUncond() const { return !isCond(); }
    bool needIttage() const { return attribute.needIttage(); }
    Addr getEnd() const { return this->pc + this->size; }
    BranchSlot()
        : pc(0),
          target(0),
          resolved(false),
          attribute(),
          size(0)
    {
    }
    BranchSlot(const Addr &control_pc, const Addr &target_pc,
               const StaticInstPtr &static_inst, unsigned size)
        : pc(control_pc),
          target(target_pc),
          resolved(false),
          attribute(Attribute::fromFlags(
              static_inst->isCondCtrl(),
              static_inst->isIndirectCtrl(),
              static_inst->isDirectCtrl(),
              static_inst->isCall(),
              static_inst->isReturn() && !static_inst->isNonSpeculative() &&
                  !static_inst->isDirectCtrl())),
          size(size)
    {
    }

    void
    setTypeFromFlags(bool is_cond, bool is_indirect, bool is_direct,
                     bool is_call, bool is_return)
    {
        attribute = Attribute::fromFlags(is_cond, is_indirect, is_direct,
                                         is_call, is_return);
    }

    int getType() const {
        if (isCond()) {
            return BR_COND;
        } else if (!isIndirect()) { // uncond direct
            if (isReturn()) {
                fatal("jal return detected!\n");
                return BR_DIRECT_RET;
            }
            if (!isCall()) {
                return BR_DIRECT_NORMAL;
            } else {
                return BR_DIRECT_CALL;
            }
        } else {  // uncond indirect
            if (!isCall()) {
                if (!isReturn()) {
                    return BR_INDIRECT_NORMAL; // normal indirect
                } else {
                    return BR_INDIRECT_RET; // indirect return
                }
            } else {
                if (!isReturn()) { // indirect call
                    return BR_INDIRECT_CALL;
                } else { // call & return
                    return BR_INDIRECT_CALL_RET;
                }
            }
        }
    }

    bool operator < (const BranchSlot &other) const
    {
        return this->pc < other.pc;
    }

    bool operator == (const BranchSlot &other) const
    {
        return this->pc == other.pc;
    }

    bool operator > (const BranchSlot &other) const
    {
        return this->pc > other.pc;
    }

    bool operator != (const BranchSlot &other) const
    {
        return this->pc != other.pc;
    }

};

using BranchInfo = BranchSlot;


/**
 * @brief Branch Target Buffer entry containing a branch slot plus BTB metadata
 *
 * Contains the branch slot observed in a fetch block plus prediction state:
 * - Valid bit
 * - Always taken bit
 * - Counter for prediction
 * - Tag for BTB lookup
 */
struct BTBEntry
{
    BranchSlot slot;
    bool valid;
    bool alwaysTaken;
    int ctr;
    Addr tag;
    int source;//only use for countering the source of the entry
    // Addr offset; // retrived from lowest bits of pc
    BTBEntry()
        : slot(),
          valid(false),
          alwaysTaken(false),
          ctr(0),
          tag(0),
          source(-1)
    {
    }
    BTBEntry(const BranchSlot &branch_slot)
        : slot(branch_slot),
          valid(true),
          alwaysTaken(true),
          ctr(0),
          tag(0),
          source(-1)
    {
    }

    bool operator < (const BTBEntry &other) const
    {
        return slot < other.slot;
    }

    bool operator == (const BTBEntry &other) const
    {
        return slot == other.slot;
    }

    bool operator > (const BTBEntry &other) const
    {
        return slot > other.slot;
    }

    bool operator != (const BTBEntry &other) const
    {
        return slot != other.slot;
    }

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

    // Addr offset; // retrived from lowest bits of pc
    TageInfoForMGSC()
        : tage_pred_taken(false),
            tage_main_taken(false),
            tage_pred_conf_high(false),
            tage_pred_conf_mid(false),
            tage_pred_conf_low(false),
            tage_pred_alt_diff(false)
    {
    }
    TageInfoForMGSC(bool tage_pred_taken, bool tage_main_taken, bool tage_pred_conf_high, bool tage_pred_conf_mid,
                    bool tage_pred_conf_low, bool tage_pred_alt_diff)
        : tage_pred_taken(tage_pred_taken),
            tage_main_taken(tage_main_taken),
            tage_pred_conf_high(tage_pred_conf_high),
            tage_pred_conf_mid(tage_pred_conf_mid),
            tage_pred_conf_low(tage_pred_conf_low),
            tage_pred_alt_diff(tage_pred_alt_diff)
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

// {branch pc -> istaken} maps
using CondTakens = std::vector<std::pair<Addr, bool>>;
// {branch pc -> target pc} maps
using IndirectTargets = std::vector<std::pair<Addr, Addr>>;

struct FetchPredictionSnapshot
{
    bool taken = false;        // whether the final prediction is taken
    Addr fallThrough = 0;      // predicted stream end pc
    BranchSlot branchSlot;     // predicted taken branch slot
    bool btbHit = false;       // whether any BTB entry was predicted
    bool falseHit = false;     // not used
    std::vector<BTBEntry> btbEntries; // predicted BTB entries
};

struct FetchResolveResult
{
    bool valid = false;        // whether execution resolved this stream
    bool taken = false;        // actual taken result
    BranchSlot branchSlot;     // executed branch slot
    SquashType squashType = SquashType::SQUASH_NONE;
    Addr squashPC = 0;         // pc of the squash inst
};

struct FetchUpdatePayload
{
    BTBEntry newBTBEntry;      // possible new entry from L1 BTB update prep
    bool isOldEntry = false;   // true: update old entry; false: use newBTBEntry
    Addr endInstPC = 0;        // end pc of the squash inst/taken inst
    std::vector<BTBEntry> btbEntries; // entries that were actually executed
};

#define CondTakens_find(condTakens, branch_pc) \
    std::find_if(condTakens.begin(), condTakens.end(), \
                 [&branch_pc](const auto &p) { return p.first == branch_pc; })
#define IndirectTakens_find(indirectTargets, branch_pc) \
    std::find_if(indirectTargets.begin(), indirectTargets.end(), \
                 [&branch_pc](const auto &p) { return p.first == branch_pc; })

#define FillStageLoop(x) for (int x = getDelay(); x < stagePreds.size(); ++x)

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
    FetchPredictionSnapshot prediction; // final prediction snapshot

    FetchResolveResult resolve; // execution result written by squash/commit
    FetchUpdatePayload update;  // payload prepared before predictor update
    FinalPredictionMetadata finalPredMetadata; // final prediction attribution

    // prediction metas
    // FIXME: use vec
    std::array<std::shared_ptr<void>, 8> predMetas; // each component has a meta, TODO

    Tick predTick;         // tick of the prediction
    boost::dynamic_bitset<> history; // record GHR/s0History
    boost::dynamic_bitset<> phistory; // record PATH/s0History
    boost::dynamic_bitset<> bwhistory; // record BWHR/s0History
    std::vector<boost::dynamic_bitset<>> lhistory; // record LHR/s0History
    std::queue<Addr> previousPCs; // previous PCs, used by ahead BTB

    // for profiling
    int fetchInstNum;
    int commitInstNum;

   FetchTarget()
       : tid(0),
         asidHash(0),
         startPC(0),
         prediction(),
         resolve(),
         update(),
         finalPredMetadata(),
         predTick(0),
         history(),
         phistory(),
         bwhistory(),
         lhistory(),
         fetchInstNum(0),
         commitInstNum(0)
   {
       predMetas.fill(nullptr);
   }

    // the default exe result should be consistent with prediction
    void setDefaultResolve() {
        resolve.valid = false;
        resolve.branchSlot = prediction.branchSlot;
        resolve.taken = prediction.taken;
        resolve.squashType = SQUASH_NONE;
        resolve.squashPC = 0;
    }

    // bool getEnded() const { return resolved ? exeEnded : predEnded; }
    BranchSlot getBranchSlot() const
    {
        return resolve.valid ? resolve.branchSlot : prediction.branchSlot;
    }
    BranchSlot getBranchInfo() const { return getBranchSlot(); }
    Addr getControlPC() const { return getBranchSlot().pc; }
    // FIXME: should be end of squash inst when non-control squash of trap squash.
    Addr getEndPC() const { return getBranchSlot().getEnd(); }
    Addr getTaken() const { return resolve.valid ? resolve.taken : prediction.taken; }
    Addr getTakenTarget() const { return getBranchSlot().target; }

    Addr getRealStartPC() const {
        return startPC;
    }

    DirectionHistoryUpdate getGHistUpdateDuringSquash(
        Addr squash_pc, bool is_cond, bool actually_taken) const
    {
        DirectionHistoryUpdate update;
        for (auto &entry : prediction.btbEntries) {
            if (entry.valid && entry.slot.pc >= startPC &&
                entry.slot.pc < squash_pc) {
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
        for (auto &entry : prediction.btbEntries) {
            if (entry.valid && entry.slot.pc >= startPC &&
                entry.slot.pc < squash_pc) {
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
        if (resolve.squashType == SQUASH_NONE) {
            if (resolve.taken) { // taken inst pc
                update.endInstPC = getControlPC();
            } else { // natural fall through, align to the next block
                // assert(halfAligned);
                update.endInstPC =
                    (startPC + predictWidth) & ~mask(floorLog2(predictWidth) - 1);
            }
        } else {
            update.endInstPC = resolve.squashPC;
        }
    }

    // should be called before components update, after setUpdateInstEndPC
    void setUpdateBTBEntries()
    {
        update.btbEntries.clear();
        for (auto &entry : prediction.btbEntries) {
            if (entry.valid && entry.slot.pc >= startPC &&
                entry.slot.pc <= update.endInstPC) {
                update.btbEntries.push_back(entry);
            }
        }
    }

    // Argument resolved pc could not match any BTB entry branch pc,
    // Just ignore it in that case.
    void markBTBEntryResolved(Addr resolvedInstPC)
    {
        for (auto &entry : update.btbEntries) {
            if (entry.valid && entry.slot.pc == resolvedInstPC) {
                entry.slot.resolved = true;
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
    using PredictionResult =
        PredictionBlockResultView<std::vector<BTBEntry>, CondTakens,
                                  IndirectTargets>;
    using TakenSlotResult = PredictionResult::TakenSlotResult;

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

    Tick predTick;

    FullBTBPrediction() :
        tid(0),
        asidHash(0),
        bbStart(0),
        btbEntries(),
        condTakens(),
        indirectTargets(),
        returnTarget(0),
        tageInfoForMgscs(),
        predTick(0) {}

    PredictionResult
    resultView() const
    {
        return PredictionResult(bbStart, btbEntries, condTakens,
                                indirectTargets, returnTarget);
    }

    BTBEntry getTakenEntry() const {
        return resultView().getTakenEntry();
    }

    bool isTaken() const {
        return resultView().isTaken();
    }

    Addr getFallThrough(Addr predictWidth) const {
        return resultView().getFallThrough(predictWidth);
    }

    Addr getEntryTarget(const BTBEntry &entry) const {
        return resultView().getEntryTarget(entry);
    }

    TakenSlotResult getTakenSlotResult(Addr predictWidth) const
    {
        return resultView().getTakenSlotResult(predictWidth);
    }

    Addr getTarget(Addr predictWidth) const {
        return resultView().getTarget(predictWidth);
    }

    Addr getEnd(Addr predictWidth) const {
        return resultView().getEnd(predictWidth);
    }

    Addr controlAddr() const {
        return resultView().controlAddr();
    }

    std::pair<bool, OverrideReason> match(const FullBTBPrediction &other,
                                          Addr predictWidth) const
    {
        const auto match =
            resultView().match(other.resultView(), predictWidth);
        return std::make_pair(match.matches,
                              toOverrideReason(match.reason));
    }

    DirectionHistoryUpdate getGHistUpdate() const  //global or local
    {
        return resultView().getGHistUpdate();
    }

    DirectionHistoryUpdate getBwHistUpdate() const //global backward or imli
    {
        return resultView().getBwHistUpdate();
    }

    PathHistoryUpdate getPHistUpdate() const //path
    {
        return resultView().getPHistUpdate();
    }

  private:
    static OverrideReason
    toOverrideReason(PredictionResultMismatchReason reason)
    {
        switch (reason) {
          case PredictionResultMismatchReason::NoOverride:
            return OverrideReason::NO_OVERRIDE;
          case PredictionResultMismatchReason::FallThrough:
            return OverrideReason::FALL_THRU;
          case PredictionResultMismatchReason::ControlAddr:
            return OverrideReason::CONTROL_ADDR;
          case PredictionResultMismatchReason::Target:
            return OverrideReason::TARGET;
        }

        return OverrideReason::NO_OVERRIDE;
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
        uint64_t bwIndexSig, uint64_t lIndexSig, uint64_t iIndexSig,
        uint64_t gIndexSig, uint64_t pIndexSig, uint64_t biasIndexSig,
        uint64_t useSc, uint64_t scPred,
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
        _uint64_data["bwIndexSig"] = bwIndexSig;
        _uint64_data["lIndexSig"] = lIndexSig;
        _uint64_data["iIndexSig"] = iIndexSig;
        _uint64_data["gIndexSig"] = gIndexSig;
        _uint64_data["pIndexSig"] = pIndexSig;
        _uint64_data["biasIndexSig"] = biasIndexSig;
        _uint64_data["useSc"] = useSc;
        _uint64_data["scPred"] = scPred;
        // Result
        _uint64_data["actualTaken"] = actualTaken;
    }
};

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5
#endif  // __CPU_PRED_BTB_STREAM_STRUCT_HH__
