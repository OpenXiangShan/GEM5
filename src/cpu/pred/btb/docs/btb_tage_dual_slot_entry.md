# BTBTAGE Multi-Slot Entry Design

## Background

`BTBTAGE` used to follow a "one entry stores one branch" model. That model is awkward for block-based prediction because branches from the same fetch block share the same block context but still compete as if they were unrelated entries.

The current design changes the storage granularity from "branch entry" to "block/tag entry with fixed branch slots". The goal is to make lookup, update, allocation, eviction, trace, and MGSC-facing state all agree on the same semantics.

## Entry Semantics

The key invariant is:

- `index` selects a set for the fetch block
- `tag` selects the unique block container inside that set
- `slot.position` selects the concrete branch inside the block

That means one `TageEntry` now represents one `(table, index, tag)` block-level container, and the `TageSlot`s inside it represent multiple conditional branches from that block (currently fixed to 4 slots in the capacity experiment).

```cpp
struct TageSlot {
    bool valid;
    unsigned position;
    short counter;
    bool useful;
};

struct TageEntry {
    bool valid;
    Addr tag;
    unsigned lruCounter;
    std::array<TageSlot, 4> slots;
};
```

Compatibility mirrors such as `entry.counter`, `entry.useful`, and `entry.pc` still exist during the transition, but they are only a temporary view of `slots[0]`. New logic should not rely on them for semantic decisions.

## Lookup Rules

`getTageIndex()` remains block-based. `getTageTag()` is also block-based now and no longer includes branch position.

Lookup is two-level:

1. Find a matching `(index, tag)` entry in the set.
2. Inside that entry, find a matching `slot.position`.

Only a slot hit counts as a provider hit. A `tag` hit with `position` miss means:

- at least one block-level container with the same `(index, tag)` exists
- the current branch does not have a slot in any of those containers yet
- later update may first try to absorb into an existing same-tag entry and then spill into another way if none can absorb it

## Allocation and Replacement

There are two mutually exclusive allocation paths.

### Same-Tag Path

If `(index, tag)` already exists:

- `position hit`: do not allocate; normal training updates the existing slot
- `position miss + empty slot`: insert into the first existing same-tag entry with an empty slot
- `position miss + all same-tag entries full`: prefer spilling into another way first (invalid way or whole-entry victim) to borrow associativity earlier
- `position miss + spill cannot find invalid/evictable way + replaceable slot exists`: replace the first `!useful && weakish` slot
- `position miss + spill cannot find invalid/evictable way + no replaceable slot`: weaken one `!useful && !weakish` slot in set order by one step toward zero

After insertion or replacement, slots are sorted by `position`, so slot numbering is a storage detail rather than a semantic identity. The invariant is that one `(table, index, tag, position)` may map to only one live slot, even if the set holds multiple entries with the same tag.

### Different-Tag Path

If no same-tag entry exists, allocation may use an invalid way or evict a whole entry. Whole-entry eviction is allowed only when all slots are unprotected.

The current rule is:

```text
unprotected = slot invalid || (!slot.useful && weakish(counter))
```

If any valid slot is protected, whole-entry eviction must fail for that candidate. If no candidate is evictable, the allocator only performs one counter weakening step and does not force insertion.

## Update Semantics

Updates are recomputed from prediction-time folded-history snapshots instead of trusting any old slot id captured earlier. This matters because:

- slot order may change after insertion or sorting
- same-tag reuse or spill can create a slot that did not exist at prediction time
- the live provider may move to a different same-tag entry than the one remembered at prediction time

Provider reconstruction therefore follows the prediction-time snapshot and then re-finds the live `(way, slot)` by `(table, index, tag, position)` when mutating state.

Main and alternate provider updates operate on `slot.counter` and `slot.useful`, not on entry-level mirrors.

## Trace and MGSC View

Trace and MGSC need slot-aware outputs so that debugging and downstream consumers observe the same semantics as the predictor core.

Current alignment:

- `TageInfoForMGSC` derives confidence and `tage_main_taken` from `mainInfo.slotInfo`
- `TAGEMISSTRACE` records `mainWay/mainSlot`, `altWay/altSlot`, and `allocWay/allocSlot`

This avoids conflating:

- entry hit vs. slot hit
- block container allocation vs. branch-slot allocation

## BTBTAGEUpperBound Compatibility

`BTBTAGEUpperBound` is not rewritten into a true dual-slot predictor in this stage. Instead, it provides a slot-aware shim:

- it exposes a single synthetic slot view
- confidence and taken/not-taken use that slot view
- its external interface matches the slot-aware `BTBTAGE` contract

This keeps shared interfaces aligned without over-expanding the scope of the upper-bound model.

## Testing Focus

The most important tests for the dual-slot design are:

- two branches from one block can share one entry and hit different slots
- same-tag `position` miss can populate an empty slot in an existing entry
- same-tag `position` miss can spill into another way when no existing same-tag entry can absorb it
- slot order remains sorted by `position`
- same-tag full-entry replacement only targets `!useful && weakish` slots
- different-tag whole-entry eviction requires every slot to be unprotected
- one `position` never appears twice across duplicate same-tag entries

When adding new tests, prefer asserting on `slots[i].counter/useful/position` instead of transitional entry-level mirrors.

## Recommended Regression Set

The following unit tests provide a compact regression set for the current dual-slot behavior:

- `BTBTAGETest.UsefulBitMechanism`
- `BTBTAGETest.CounterUpdateMechanism`
- `BTBTAGETest.SlotAwareSharedEntryLookup`
- `BTBTAGETest.DifferentTagWholeEntryEvictionRequiresAllSlotsUnprotected`
- `BTBTAGETest.SameTagPositionMissFillsEmptySlotAndSortsByPosition`
- `BTBTAGETest.SameTagFullEntryReplacesWeakishNonUsefulSlot`
- `BTBTAGETest.SameTagFullEntryPrefersSpillOverReplaceWhenWayAvailable`
- `BTBTAGETest.SameTagFullEntryWithoutReplaceableSlotSpillsToInvalidWay`
- `BTBTAGETest.SameTagFullEntryWithoutReplaceableSlotSpillsByWholeEntryEviction`
- `BTBTAGETest.SameTagSpillFailureWeakensSetAndCountsAllocationFailure`
- `BTBTAGETest.NewConditionalEntryWithoutPredictionMetaStillTrains`

Suggested command:

```bash
./build/RISCV/cpu/pred/btb/test/tage.test.debug --gtest_filter='BTBTAGETest.UsefulBitMechanism:BTBTAGETest.CounterUpdateMechanism:BTBTAGETest.SlotAwareSharedEntryLookup:BTBTAGETest.DifferentTagWholeEntryEvictionRequiresAllSlotsUnprotected:BTBTAGETest.SameTagPositionMissFillsEmptySlotAndSortsByPosition:BTBTAGETest.SameTagFullEntryReplacesWeakishNonUsefulSlot:BTBTAGETest.SameTagFullEntryPrefersSpillOverReplaceWhenWayAvailable:BTBTAGETest.SameTagFullEntryWithoutReplaceableSlotSpillsToInvalidWay:BTBTAGETest.SameTagFullEntryWithoutReplaceableSlotSpillsByWholeEntryEviction:BTBTAGETest.SameTagSpillFailureWeakensSetAndCountsAllocationFailure:BTBTAGETest.NewConditionalEntryWithoutPredictionMetaStillTrains'
```

## Recommended BTBTAGEUpperBound Regression Set

For the slot-aware shim in `BTBTAGEUpperBound`, the following tests provide a compact compatibility regression set:

- `BTBTAGEUpperBoundTest.ExactContextLookup`
- `BTBTAGEUpperBoundTest.ProviderAltSelection`
- `BTBTAGEUpperBoundTest.AllocationUsesPredictionTimeHistory`
- `BTBTAGEUpperBoundTest.NewConditionalEntryWithoutPredictionMetaStillTrains`
- `BTBTAGEUpperBoundPathHashTest.PredictionUsesPathHashHistorySnapshot`

Suggested command:

```bash
./build/RISCV/cpu/pred/btb/test/tage.test.debug --gtest_filter='BTBTAGEUpperBoundTest.ExactContextLookup:BTBTAGEUpperBoundTest.ProviderAltSelection:BTBTAGEUpperBoundTest.AllocationUsesPredictionTimeHistory:BTBTAGEUpperBoundTest.NewConditionalEntryWithoutPredictionMetaStillTrains:BTBTAGEUpperBoundPathHashTest.PredictionUsesPathHashHistorySnapshot'
```

There are also broader path-hash override tests:

- `BTBTAGEUpperBoundPathHashTest.PredictionUsesIndirectOverridePathHashSnapshot`
- `BTBTAGEUpperBoundPathHashTest.PredictionUsesReturnOverridePathHashSnapshot`

They are useful when touching indirect/return override plumbing, but they are not required for the smallest shim regression set.
