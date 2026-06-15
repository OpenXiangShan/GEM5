#ifndef __CPU_PRED_BTB_PREDICTION_RESULT_HH__
#define __CPU_PRED_BTB_PREDICTION_RESULT_HH__

#include "base/bitfield.hh"
#include "base/intmath.hh"
#include "base/types.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

enum class PredictionResultMismatchReason
{
    NoOverride,
    FallThrough,
    ControlAddr,
    Target
};

struct PredictionResultMatch
{
    bool matches;
    PredictionResultMismatchReason reason;
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
 * Resolves a fetch block prediction into the first taken branch and its target.
 *
 * The view keeps the stage-prediction storage outside this module, but owns the
 * rules that combine BTB entries, direction predictions, indirect targets, and
 * RAS targets into the final fetch-block result.
 */
template <typename Entries, typename CondTakenMap, typename IndirectTargetMap>
class PredictionBlockResultView
{
  public:
    using Entry = typename Entries::value_type;

    struct TakenSlotResult
    {
        Entry entry;
        Addr target = 0;
        Addr fallThrough = 0;

        bool taken() const { return entry.valid; }
        Addr controlPC() const { return entry.slot.pc; }
        Addr endPC() const
        {
            return taken() ? entry.slot.getEnd() : fallThrough;
        }

        auto
        resolvedSlot() const
        {
            auto resolved_slot = entry.slot;
            resolved_slot.target = target;
            return resolved_slot;
        }
    };

    PredictionBlockResultView(Addr block_start, const Entries &entries,
                              const CondTakenMap &cond_taken_map,
                              const IndirectTargetMap &indirect_target_map,
                              Addr ras_return_target)
        : blockStart(block_start),
          btbEntries(entries),
          condTakens(cond_taken_map),
          indirectTargets(indirect_target_map),
          returnTarget(ras_return_target)
    {
    }

    Entry
    getTakenEntry() const
    {
        // IMPORTANT: callers provide entries sorted by instruction order.
        for (const auto &entry : btbEntries) {
            if (!entry.valid) {
                continue;
            }

            if (entry.slot.isCond()) {
                if (condTaken(entry.slot.pc)) {
                    return entry;
                }
                continue;
            }

            if (entry.slot.isUncond()) {
                return entry;
            }
        }

        return Entry();
    }

    bool isTaken() const { return getTakenEntry().valid; }

    Addr
    getFallThrough(Addr predict_width) const
    {
        // max 64 byte block, 32 byte aligned
        return (blockStart + predict_width) &
               ~mask(floorLog2(predict_width) - 1);
    }

    Addr
    getEntryTarget(const Entry &entry) const
    {
        Addr target = entry.slot.target;

        if (entry.slot.isIndirect()) {
            if (!entry.slot.isReturn()) {
                Addr indirect_target = 0;
                if (findIndirectTarget(entry.slot.pc, indirect_target)) {
                    target = indirect_target;
                }
            } else {
                target = returnTarget;
            }
        }

        return target;
    }

    TakenSlotResult
    getTakenSlotResult(Addr predict_width) const
    {
        TakenSlotResult result;
        result.fallThrough = getFallThrough(predict_width);
        result.entry = getTakenEntry();
        result.target = result.entry.valid ? getEntryTarget(result.entry) :
                                             result.fallThrough;
        return result;
    }

    Addr getTarget(Addr predict_width) const
    {
        return getTakenSlotResult(predict_width).target;
    }

    Addr getEnd(Addr predict_width) const
    {
        return getTakenSlotResult(predict_width).endPC();
    }

    Addr
    controlAddr() const
    {
        const auto entry = getTakenEntry();
        return entry.valid ? entry.slot.pc : 0;
    }

    PredictionResultMatch
    match(const PredictionBlockResultView &other, Addr predict_width) const
    {
        const auto this_taken_entry = getTakenEntry();
        const auto other_taken_entry = other.getTakenEntry();

        if (this_taken_entry.valid != other_taken_entry.valid) {
            return {false, PredictionResultMismatchReason::FallThrough};
        }

        if (!this_taken_entry.valid) {
            return {true, PredictionResultMismatchReason::NoOverride};
        }

        if (controlAddr() != other.controlAddr()) {
            return {false, PredictionResultMismatchReason::ControlAddr};
        }

        if (getTarget(predict_width) != other.getTarget(predict_width)) {
            return {false, PredictionResultMismatchReason::Target};
        }

        return {true, PredictionResultMismatchReason::NoOverride};
    }

    DirectionHistoryUpdate
    getGHistUpdate() const
    {
        DirectionHistoryUpdate update;

        for (const auto &entry : btbEntries) {
            if (!entry.valid) {
                continue;
            }

            if (entry.slot.isCond()) {
                update.shamt++;
                if (condTaken(entry.slot.pc)) {
                    update.taken = true;
                    break;
                }
            } else {
                break;
            }
        }

        return update;
    }

    DirectionHistoryUpdate
    getBwHistUpdate() const
    {
        DirectionHistoryUpdate update;

        for (const auto &entry : btbEntries) {
            if (!entry.valid) {
                continue;
            }

            if (entry.slot.isCond()) {
                update.shamt++;
                if (condTaken(entry.slot.pc)) {
                    update.taken = entry.slot.target < entry.slot.pc;
                    break;
                }
            } else {
                break;
            }
        }

        return update;
    }

    PathHistoryUpdate
    getPHistUpdate() const
    {
        PathHistoryUpdate update;
        const auto entry = getTakenEntry();
        if (entry.valid) {
            update.taken = true;
            update.pc = entry.slot.pc;
            update.target = getEntryTarget(entry);
        }

        return update;
    }

  private:
    bool
    condTaken(Addr branch_pc) const
    {
        for (const auto &prediction : condTakens) {
            if (prediction.first == branch_pc) {
                return prediction.second;
            }
        }

        return false;
    }

    bool
    findIndirectTarget(Addr branch_pc, Addr &target) const
    {
        for (const auto &prediction : indirectTargets) {
            if (prediction.first == branch_pc) {
                target = prediction.second;
                return true;
            }
        }

        return false;
    }

    Addr blockStart;
    const Entries &btbEntries;
    const CondTakenMap &condTakens;
    const IndirectTargetMap &indirectTargets;
    Addr returnTarget;
};

} // namespace btb_pred

} // namespace branch_prediction

} // namespace gem5

#endif // __CPU_PRED_BTB_PREDICTION_RESULT_HH__
