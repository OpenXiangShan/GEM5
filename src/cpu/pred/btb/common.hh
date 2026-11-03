#ifndef __CPU_PRED_BTB_STREAM_STRUCT_HH__
#define __CPU_PRED_BTB_STREAM_STRUCT_HH__

#include <algorithm>
#include <cstdlib>
#include <queue>
#include <string>

#include <boost/dynamic_bitset.hpp>

// #include "arch/generic/pcstate.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/btb/branch_outcome.hh"
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
    BTBEntry(const BranchInfo &bi)
        : BranchInfo(bi), valid(true), ctr(0), tag(0), source(-1) {}
    BranchInfo getBranchInfo() { return BranchInfo(*this); }

    int getsource() const {
        return source;
    }

    void setsource(int src) {
        source = src;
    }
};

struct DirectionPrediction
{
    bool valid = false;
    bool taken = false;
};

struct TargetPrediction
{
    bool valid = false;
    Addr target = 0;
};

/** A branch candidate, independent of BTB storage and replacement state. */
struct BranchPrediction : BranchInfo
{
    bool valid{false};
    bool taken{false};
    Addr predictedTarget{0}; // Selected target; inherited target is the BTB input.
    bool baseTaken{false};
    bool baseStrong{false};
    int source{-1};

    BranchPrediction() = default;
    BranchPrediction(const BranchInfo &branch)
        : BranchInfo(branch), valid(true), predictedTarget(branch.target)
    {}
    BranchPrediction(const BTBEntry &entry)
        : BranchInfo(entry), valid(entry.valid),
          taken(!entry.isCond || entry.ctr >= 0),
          predictedTarget(entry.target),
          baseTaken(entry.ctr >= 0),
          baseStrong(std::abs(2 * entry.ctr + 1) == 5),
          source(entry.source)
    {}

    BranchInfo getBranchInfo() const { return *this; }
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

enum class PairPhase : uint8_t
{
    Even = 0,
    Odd = 1
};

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
 * - Recovery and profiling bookkeeping
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
    std::vector<Addr> predictedBranchPCs;

    unsigned predSource;   // source of the prediction(numStage)
    OverrideReason overrideReason; // reason of the override(for profiling)
    PairPhase pairPhase;   // PairTAGE logical phase of this block start

    // prediction metas
    // FIXME: use vec
    std::array<std::shared_ptr<void>, 9> predMetas; // each component has a meta, TODO

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
   }

    template<class Entry = BranchPrediction>
    void setPredictedBranches(const std::vector<Entry> &entries)
    {
        predictedBranchPCs.clear();
        predictedBranchPCs.reserve(entries.size());
        for (const auto &entry : entries) {
            if (entry.valid) {
                predictedBranchPCs.push_back(entry.pc);
            }
        }
    }

    Addr getRealStartPC() const {
        return startPC;
    }

    DirectionHistoryUpdate getGHistUpdateDuringSquash(
        Addr squash_pc, bool is_cond, bool actually_taken) const
    {
        DirectionHistoryUpdate update;
        for (const auto pc : predictedBranchPCs) {
            if (pc >= startPC && pc < squash_pc) {
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
        for (const auto pc : predictedBranchPCs) {
            if (pc >= startPC && pc < squash_pc) {
                update.shamt++;
            }
        }
        if (is_cond) {
            update.shamt++;
            update.taken = actually_taken && (squash_pc > target);
        }
        return update;
    }

};

/** Prediction result exposed to the Fetch stage for one FTQ entry. */
struct FetchBlockPrediction
{
    FetchTargetId ftqId = 0;
    Addr startPC = 0;
    Addr endPC = 0;
    bool taken = false;
    Addr controlPC = 0;
    Addr target = 0;
};

/**
 * Short-lived view of FTQ-owned prediction checkpoints required to recover
 * speculative history. Components must not retain this context.
 */
struct HistoryRecoveryContext
{
    ThreadID tid;
    uint8_t asidHash;
    Addr startPC;
    PairPhase pairPhase;

    const std::array<std::shared_ptr<void>, 9> &predMetas;
    const boost::dynamic_bitset<> &history;
    const boost::dynamic_bitset<> &phistory;
    const boost::dynamic_bitset<> &bwhistory;
    const std::vector<boost::dynamic_bitset<>> &lhistory;

    explicit HistoryRecoveryContext(const FetchTarget &target)
        : tid(target.tid),
          asidHash(target.asidHash),
          startPC(target.startPC),
          pairPhase(target.pairPhase),
          predMetas(target.predMetas),
          history(target.history),
          phistory(target.phistory),
          bwhistory(target.bwhistory),
          lhistory(target.lhistory)
    {}
};

/**
 * Read-only prediction state required to build and apply predictor updates.
 *
 * This is deliberately a view into an FTQ-owned FetchTarget.  It narrows the
 * component API without creating a second owner or extending the lifetime of
 * prediction metadata beyond the FTQ entry.
 */
struct PredictionUpdateContext
{
    ThreadID tid;
    uint8_t asidHash;
    Addr startPC;
    Tick predTick;

    const std::array<std::shared_ptr<void>, 9> &predMetas;
    const boost::dynamic_bitset<> &phistory;
    const std::queue<Addr> &previousPCs;

    explicit PredictionUpdateContext(const FetchTarget &target)
        : tid(target.tid),
          asidHash(target.asidHash),
          startPC(target.startPC),
          predTick(target.predTick),
          predMetas(target.predMetas),
          phistory(target.phistory),
          previousPCs(target.previousPCs)
    {}

    Addr getRealStartPC() const { return startPC; }
};

/**
 * Representative control-flow result for training and statistics.
 * A resolve packet may contain only part of a FetchBlock.
 */
struct ControlFlowOutcome
{
    BranchInfo branch;
    bool taken = false;
    bool controlMispred = false;
    bool valid = false;
};

struct PreparedUpdate
{
    std::vector<BranchOutcome> branches;
    ControlFlowOutcome outcome;

    /**
     * Build a packet from actual outcomes. An empty vector is a valid
     * complete branchless block. Each dynamic control instruction appears
     * once; a repeated PC within one FTQ entry would violate that boundary.
     */
    explicit PreparedUpdate(const std::vector<BranchOutcome> &outcomeEvents)
        : branches(outcomeEvents)
    {
        std::stable_sort(
            branches.begin(), branches.end(),
            [](const BranchOutcome &lhs, const BranchOutcome &rhs) {
                return lhs.seqNum < rhs.seqNum;
            });
        outcome = makeControlFlowOutcome(branches);
    }

  private:
    static ControlFlowOutcome makeControlFlowOutcome(
        const std::vector<BranchOutcome> &sortedBranches)
    {
        if (sortedBranches.empty()) {
            return {};
        }

        // Select the first actual transfer or misprediction in program order.
        // If all branches correctly fall through, describe the last one for
        // statistics; taken remains false for training consumers.
        const auto selected = std::find_if(
            sortedBranches.begin(), sortedBranches.end(),
            [](const BranchOutcome &branch) {
                return branch.taken || branch.mispredicted;
            });
        const auto &primary = selected != sortedBranches.end() ?
            *selected : sortedBranches.back();
        return ControlFlowOutcome{
            primary, primary.taken, primary.mispredicted, true
        };
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
    bool hit{false}; // A hit may describe a branchless fallthrough.
    std::vector<BranchPrediction> btbEntries; // for BTB, only assigned when hit, sorted by inst order
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
        tageInfoForMgscs(),
        predSource(0),
        overrideReason(OverrideReason::NO_OVERRIDE),
        predTick(0),
        s1Source(-1),
        s3Source(-1) {}

    BranchPrediction getTakenEntry() const
    {
        // Keep the not-taken prefix for speculative history updates.
        for (const auto &entry : btbEntries) {
            if (entry.valid && (!entry.isCond || entry.taken)) {
                return entry;
            }
        }
        return {};
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
            target = entry.predictedTarget;
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

    DirectionHistoryUpdate getGHistUpdate() const
    {
        DirectionHistoryUpdate update;
        for (const auto &entry : btbEntries) {
            if (!entry.valid) {
                continue;
            }
            if (!entry.isCond) {
                break;
            }
            ++update.shamt;
            if (entry.taken) {
                update.taken = true;
                break;
            }
        }
        return update;
    }

    DirectionHistoryUpdate getBwHistUpdate() const
    {
        auto update = getGHistUpdate();
        if (update.taken) {
            const auto entry = getTakenEntry();
            update.taken = entry.target < entry.pc;
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
            update.target = entry.predictedTarget;
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
