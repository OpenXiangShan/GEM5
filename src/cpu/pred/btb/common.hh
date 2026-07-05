#ifndef __CPU_PRED_BTB_STREAM_STRUCT_HH__
#define __CPU_PRED_BTB_STREAM_STRUCT_HH__

#include <algorithm>
#include <cassert>
#include <queue>
#include <string>
#include <vector>

#include <boost/dynamic_bitset.hpp>

// #include "arch/generic/pcstate.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/btb/branch_record.hh"
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

inline int
getBranchType(bool is_cond, bool is_indirect, bool is_call, bool is_return)
{
    if (is_cond) {
        return BR_COND;
    } else if (!is_indirect) { // uncond direct
        if (is_return) {
            fatal("jal return detected!\n");
            return BR_DIRECT_RET;
        }
        if (!is_call) {
            return BR_DIRECT_NORMAL;
        } else {
            return BR_DIRECT_CALL;
        }
    } else {  // uncond indirect
        if (!is_call) {
            if (!is_return) {
                return BR_INDIRECT_NORMAL; // normal indirect
            } else {
                return BR_INDIRECT_RET; // indirect return
            }
        } else {
            if (!is_return) { // indirect call
                return BR_INDIRECT_CALL;
            } else { // call & return
                return BR_INDIRECT_CALL_RET;
            }
        }
    }
}

inline int
getBranchType(const ResolvedBranch &branch)
{
    return getBranchType(
        branch.isCond, branch.isIndirect, branch.isCall, branch.isReturn);
}

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
struct BranchInfo
{
    Addr pc;
    Addr target;
    bool isCond;
    bool isIndirect;
    bool isDirect;
    bool isCall;
    bool isReturn;
    uint8_t size;
    bool isUncond() const { return !this->isCond; }
    Addr getEnd() const { return this->pc + this->size; }
    BranchInfo()
        : pc(0), target(0), isCond(false), isIndirect(false),
          isDirect(false), isCall(false), isReturn(false), size(0)
    {
    }
    // BranchInfo(const Addr &pc, const Addr &target_pc, bool is_cond) :
    // pc(pc), target(target_pc), isCond(is_cond), isIndirect(false), isCall(false), isReturn(false), size(0) {}
    BranchInfo(const Addr &control_pc, const Addr &target_pc, const StaticInstPtr &static_inst, unsigned size)
        : pc(control_pc),
          target(target_pc),
          isCond(static_inst->isCondCtrl()),
          isIndirect(static_inst->isIndirectCtrl()),
          isDirect(static_inst->isDirectCtrl()),
          isCall(static_inst->isCall()),
          isReturn(static_inst->isReturn() && !static_inst->isNonSpeculative() && !static_inst->isDirectCtrl()),
          size(size)
    {
    }
    int getType() const {
        return getBranchType(isCond, isIndirect, isCall, isReturn);
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
 * - Counter for prediction
 * - Tag for BTB lookup
 */
struct BTBEntry : BranchInfo
{
    bool valid;
    int ctr;
    Addr tag;
    int source;//only use for countering the source of the entry
    // Addr offset; // retrived from lowest bits of pc
    BTBEntry() : BranchInfo(), valid(false), ctr(0), tag(0), source(-1) {}

    int getsource() const {
        return source;
    }

    void setsource(int src) {
        source = src;
    }
};

inline BTBEntry
makeBTBEntryFromResolvedBranch(const ResolvedBranch &branch)
{
    BTBEntry entry;
    entry.pc = branch.pc;
    entry.target = branch.target;
    entry.isCond = branch.isCond;
    entry.isIndirect = branch.isIndirect;
    entry.isDirect = branch.isDirect;
    entry.isCall = branch.isCall;
    entry.isReturn = branch.isReturn;
    entry.size = branch.size;
    entry.valid = true;
    return entry;
}

struct DirectionUpdateEntry
{
    // Actual direction facts come from the resolved branch record. baseTaken
    // and isNewEntry describe the prediction-time update base.
    Addr pc = 0;
    bool actualTaken = false;
    bool mispred = false;
    bool baseTaken = false;
    bool isNewEntry = false;
};

inline DirectionUpdateEntry
makeDirectionUpdateEntry(const ResolvedBranch &actual_branch,
                         bool base_taken,
                         bool is_new_entry)
{
    DirectionUpdateEntry entry;
    entry.pc = actual_branch.pc;
    entry.actualTaken = actual_branch.taken;
    entry.mispred = actual_branch.mispred;
    entry.baseTaken = base_taken;
    entry.isNewEntry = is_new_entry;
    return entry;
}

struct BranchUpdateContext
{
    ThreadID tid = 0;
    Addr startPC = 0;
    uint8_t asidHash = 0;
    Tick predTick = 0;
};

struct TargetUpdateEntry
{
    // Actual branch facts come from the resolved branch record. The base
    // fields are only the prediction-time/table-write state needed when no
    // newer matching table entry exists during update.
    ResolvedBranch actualBranch;
    Addr baseTarget = 0;
    int baseCtr = 0;
    int baseSource = -1;
    // This is not a target-table miss bit.
    bool synthesizedFromActual = false;
};

inline const DirectionUpdateEntry *
findFirstTakenDirectionUpdateEntry(
    const std::vector<DirectionUpdateEntry> &entries)
{
    const DirectionUpdateEntry *first_taken = nullptr;
    for (const auto &entry : entries) {
        if (!entry.actualTaken) {
            continue;
        }
        // Branches in one fetch block are ordered by PC.
        if (!first_taken || entry.pc < first_taken->pc) {
            first_taken = &entry;
        }
    }
    return first_taken;
}

inline const ResolvedBranch *
findFirstTakenActualUpdateBranch(
    const std::vector<ResolvedBranch> &branches)
{
    const ResolvedBranch *first_taken = nullptr;
    for (const auto &branch : branches) {
        if (!branch.taken) {
            continue;
        }
        if (!first_taken || branch.pc < first_taken->pc) {
            first_taken = &branch;
        }
    }
    return first_taken;
}

inline const ResolvedBranch *
findActualUpdateSummaryBranch(
    const std::vector<ResolvedBranch> &branches)
{
    if (branches.empty()) {
        return nullptr;
    }
    if (const auto *taken_branch =
            findFirstTakenActualUpdateBranch(branches)) {
        return taken_branch;
    }
    return &branches.back();
}

inline const ResolvedBranch *
findMispredictedActualUpdateBranch(
    const std::vector<ResolvedBranch> &branches)
{
    auto it = std::find_if(
        branches.begin(), branches.end(),
        [](const auto &branch) { return branch.mispred; });
    return it == branches.end() ? nullptr : &*it;
}

inline const ResolvedBranch *
findUpdateBoundaryActualBranch(
    const std::vector<ResolvedBranch> &branches)
{
    const ResolvedBranch *boundary = nullptr;
    for (const auto &branch : branches) {
        if (!endsUpdateBranchPrefix(branch)) {
            continue;
        }
        if (!boundary || branch.pc < boundary->pc) {
            boundary = &branch;
        }
    }
    return boundary;
}

inline const TargetUpdateEntry *
findTakenTargetUpdateEntry(const std::vector<TargetUpdateEntry> &entries)
{
    auto it = std::find_if(
        entries.begin(), entries.end(),
        [](const auto &entry) { return entry.actualBranch.taken; });
    return it == entries.end() ? nullptr : &*it;
}

inline bool
targetUpdateHitPrediction(
    const TargetUpdateEntry &taken_entry,
    const std::vector<BTBEntry> &predicted_entries)
{
    return std::any_of(
        predicted_entries.begin(), predicted_entries.end(),
        [&taken_entry](const auto &predicted_entry) {
            return taken_entry.actualBranch.pc == predicted_entry.pc;
        });
}

inline void
updateTargetEntryCounter(int &ctr, bool taken)
{
    if (taken && ctr < 1) {
        ctr++;
    }
    if (!taken && ctr > -2) {
        ctr--;
    }
}

inline void
applyActualBranchIdentity(BTBEntry &entry, const ResolvedBranch &branch)
{
    entry.pc = branch.pc;
    entry.isCond = branch.isCond;
    entry.isIndirect = branch.isIndirect;
    entry.isDirect = branch.isDirect;
    entry.isCall = branch.isCall;
    entry.isReturn = branch.isReturn;
    entry.size = branch.size;
    entry.valid = true;
}

inline BTBEntry
makeTargetEntryWriteBase(const TargetUpdateEntry &update_entry)
{
    BTBEntry entry;
    entry.target = update_entry.baseTarget;
    entry.ctr = update_entry.baseCtr;
    entry.source = update_entry.baseSource;
    return entry;
}

inline TargetUpdateEntry
makeTargetUpdateEntry(const ResolvedBranch &actual_branch,
                      Addr base_target,
                      int base_ctr,
                      int base_source,
                      bool synthesized_from_actual)
{
    TargetUpdateEntry entry;
    entry.actualBranch = actual_branch;
    entry.baseTarget = base_target;
    entry.baseCtr = base_ctr;
    entry.baseSource = base_source;
    entry.synthesizedFromActual = synthesized_from_actual;
    return entry;
}

inline TargetUpdateEntry
makeTargetUpdateEntryFromBase(const BTBEntry &base_entry,
                              const ResolvedBranch &actual_branch,
                              bool synthesized_from_actual)
{
    return makeTargetUpdateEntry(
        actual_branch, base_entry.target, base_entry.ctr,
        base_entry.source, synthesized_from_actual);
}

inline BTBEntry
buildUpdatedTargetEntry(const TargetUpdateEntry &update_entry,
                        const BTBEntry *existing_entry,
                        Addr tag)
{
    const auto &actual_branch = update_entry.actualBranch;
    BTBEntry entry_to_write =
        (actual_branch.isCond && existing_entry) ?
            BTBEntry(*existing_entry) :
            makeTargetEntryWriteBase(update_entry);

    applyActualBranchIdentity(entry_to_write, actual_branch);
    entry_to_write.tag = tag;

    if (actual_branch.isCond) {
        updateTargetEntryCounter(
            entry_to_write.ctr, actual_branch.taken);
    }

    if (actual_branch.taken) {
        entry_to_write.target = actual_branch.target;
    }

    return entry_to_write;
}

enum class TargetUpdateEntryFilter
{
    Any,
    IndirectNonReturn,
    TakenControl
};

enum class PredictorUpdateProtocol
{
    None,
    DirectionEntries,
    TargetEntries,
    BranchContext,
    AheadPipelineState
};

inline const ResolvedBranch *
findActualUpdateBranch(
    const std::vector<ResolvedBranch> &actual_update_branches, Addr pc)
{
    auto it = std::find_if(
        actual_update_branches.begin(), actual_update_branches.end(),
        [pc](const auto &branch) { return branch.pc == pc; });
    return it == actual_update_branches.end() ? nullptr : &*it;
}

inline std::vector<DirectionUpdateEntry>
buildDirectionUpdateEntries(
    const std::vector<BTBEntry> &pred_update_entries,
    const std::vector<ResolvedBranch> &actual_update_branches)
{
    std::vector<DirectionUpdateEntry> entries;
    entries.reserve(pred_update_entries.size() +
                    actual_update_branches.size());

    const auto has_pred_entry_pc = [&](Addr pc) {
        return std::any_of(
            pred_update_entries.begin(), pred_update_entries.end(),
            [pc](const auto &entry) { return entry.pc == pc; });
    };

    auto add_entry = [&](const ResolvedBranch &branch, bool base_taken,
                         bool is_new_entry) {
        entries.push_back(makeDirectionUpdateEntry(
            branch, base_taken, is_new_entry));
    };

    for (const auto &entry : pred_update_entries) {
        const auto *actual_branch =
            findActualUpdateBranch(actual_update_branches, entry.pc);
        if (!entry.isCond || !actual_branch) {
            continue;
        }
        add_entry(*actual_branch, entry.ctr >= 0, false);
    }
    for (const auto &branch : actual_update_branches) {
        if (!branch.isCond || has_pred_entry_pc(branch.pc)) {
            continue;
        }
        // Missing predicted entries use the neutral ctr=0 base direction.
        add_entry(branch, true, true);
    }

    return entries;
}

inline std::vector<TargetUpdateEntry>
buildTargetUpdateEntries(
    const std::vector<BTBEntry> &pred_update_entries,
    const std::vector<ResolvedBranch> &actual_update_branches,
    TargetUpdateEntryFilter filter)
{
    std::vector<TargetUpdateEntry> entries;
    entries.reserve(pred_update_entries.size() +
                    actual_update_branches.size());

    auto add_entry = [&](const ResolvedBranch &actual_branch,
                         Addr base_target, int base_ctr, int base_source,
                         bool synthesized_from_actual) {
        bool keep = false;
        switch (filter) {
          case TargetUpdateEntryFilter::Any:
            keep = true;
            break;
          case TargetUpdateEntryFilter::IndirectNonReturn:
            keep = actual_branch.isIndirect && !actual_branch.isReturn;
            break;
          case TargetUpdateEntryFilter::TakenControl:
            keep = actual_branch.taken;
            break;
        }
        if (!keep) {
            return;
        }

        entries.push_back(makeTargetUpdateEntry(
            actual_branch, base_target, base_ctr, base_source,
            synthesized_from_actual));
    };
    auto has_entry_pc = [&](Addr pc) {
        return std::any_of(
            entries.begin(), entries.end(),
            [pc](const auto &entry) {
                return entry.actualBranch.pc == pc;
            });
    };
    auto add_new_target_branch = [&](const ResolvedBranch &branch) {
        if (has_entry_pc(branch.pc)) {
            return;
        }
        add_entry(branch, branch.target, 0, -1, true);
    };

    for (const auto &entry : pred_update_entries) {
        if (!entry.valid) {
            continue;
        }
        const auto *actual_branch =
            findActualUpdateBranch(actual_update_branches, entry.pc);
        if (!actual_branch) {
            continue;
        }
        add_entry(
            *actual_branch, entry.target, entry.ctr, entry.source, false);
    }
    for (const auto &branch : actual_update_branches) {
        if (branch.taken) {
            add_new_target_branch(branch);
        }
    }

    return entries;
}

inline Addr
buildUpdateEndInstPC(
    Addr start_pc,
    const std::vector<ResolvedBranch> &actual_update_branches,
    unsigned predict_width)
{
    if (const auto *boundary_branch =
            findUpdateBoundaryActualBranch(actual_update_branches)) {
        return boundary_branch->pc;
    }
    return (start_pc + predict_width) & ~mask(floorLog2(predict_width) - 1);
}

inline std::vector<BTBEntry>
selectPredictedBTBEntriesForUpdate(
    const std::vector<BTBEntry> &pred_btb_entries,
    Addr start_pc,
    Addr update_end_inst_pc)
{
    std::vector<BTBEntry> entries;
    for (const auto &entry : pred_btb_entries) {
        if (entry.valid && entry.pc >= start_pc &&
            entry.pc <= update_end_inst_pc) {
            entries.push_back(entry);
        }
    }
    return entries;
}

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
    std::vector<BTBEntry> predBTBEntries;   // record predicted BTB entries

    bool resolved;  // whether the branch is resolved/executed

    std::vector<ResolvedBranch> resolvedBranches; // actual CFIs from resolve

    int squashType;         // squash type
    Addr squashPC;         // pc of the squash inst
    unsigned predSource;   // source of the prediction(numStage)
    OverrideReason overrideReason; // reason of the override(for profiling)

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
         resolved(false),
         squashType(SquashType::SQUASH_NONE),
         squashPC(0),
         predSource(0),
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
       resolvedBranches.clear();
   }

    DirectionHistoryUpdate getGHistUpdateDuringSquash(
        Addr squash_pc, const ResolvedBranch *actual_branch) const
    {
        DirectionHistoryUpdate update;
        for (auto &entry : predBTBEntries) {
            if (entry.valid && entry.pc >= startPC && entry.pc < squash_pc) {
                update.shamt++;
            }
        }
        if (actual_branch && actual_branch->isCond) {
            update.shamt++;
            update.taken = actual_branch->taken;
        }
        return update;
    }

    DirectionHistoryUpdate getBwHistUpdateDuringSquash(
        Addr squash_pc, const ResolvedBranch *actual_branch) const
    {
        DirectionHistoryUpdate update;
        for (auto &entry : predBTBEntries) {
            if (entry.valid && entry.pc >= startPC && entry.pc < squash_pc) {
                update.shamt++;
            }
        }
        if (actual_branch && actual_branch->isCond) {
            update.shamt++;
            update.taken =
                actual_branch->taken && (squash_pc > actual_branch->target);
        }
        return update;
    }

    PathHistoryUpdate getPHistUpdateDuringSquash(
        Addr squash_pc, const ResolvedBranch *actual_branch) const
    {
        PathHistoryUpdate update;
        update.taken = actual_branch &&
            actual_branch->taken && actual_branch->pc == squash_pc;
        if (update.taken) {
            update.pc = squash_pc;
            update.target = actual_branch->target;
        }
        return update;
    }

    bool addResolvedBranch(const ResolvedBranch &branch)
    {
        return insertResolvedBranchByPC(resolvedBranches, branch);
    }

    size_t addResolvedBranches(const std::vector<ResolvedBranch> &branches)
    {
        return insertResolvedBranchesByPC(resolvedBranches, branches);
    }

};

inline BranchUpdateContext
makeBaseBranchUpdateContext(const FetchTarget &target)
{
    BranchUpdateContext ctx;
    ctx.tid = target.tid;
    ctx.startPC = target.startPC;
    ctx.asidHash = target.asidHash;
    ctx.predTick = target.predTick;
    return ctx;
}

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
