# CROB RTL Alignment Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Align the gem5 Hybrid CROB model with the RTL entry/slot semantics for planning, physical-entry commit width, latter-slot downgrade, and slot-aware squash.

**Architecture:** Keep the existing flat `DynInst` list for execution and architectural commit side effects, but add persistent Hybrid entry metadata alongside it. Each Hybrid entry records former/latter member counts and type; each member carries an entry id and slot bit. Hybrid commit consumes one physical-entry quota after all members of the current entry retire, while Hybrid squash removes members according to the redirect slot and downgrades a surviving former entry to `NORMAL`.

**Tech Stack:** C++ gem5 O3 CPU, existing gtest planner test, Python configuration/stat checker.

**Spec:** RTL CROB PR #6539 at commit `c4fd3e302d97a306560b7056dc8c7bd7c5510c71`; scope is limited to former/latter entry planning, physical-entry commit width, latter-to-NORMAL downgrade, and slot-aware squash.

## Global Constraints

- Hybrid remains single-threaded as enforced by the current constructor checks.
- Non-Hybrid ROB policies retain their default group-window behavior; the optional DynInst quota is removed as authorized by the user.
- Do not add RAB, value-prediction, vector-state, flag-tracker, or new pipeline-stage modeling in this change.
- Do not reinterpret a simple run as one DynInst; its members remain individually committed for architectural side effects.
- A surviving former entry is never re-paired with a later allocation after a latter squash.
- `commitWidth` is the Hybrid physical-entry width; no separate DynInst quota remains.


## Review Focus (source audit, 2026-09-20)

- Planner member conservation: `SSSCSS` contains only one C and produces `[SSS | C] [SS]`; test lengths and reconstructed class order in Task 1.
- Late faults and strictly ordered operations: preserve partial architectural progress and entry allocation until retirement or squash releases it; test in Task 2 and the exception workload.
- Squash boundaries: publish the same retained prefix to ROB, Rename, IQ, and LSQ, including sequence-number gaps; test in Task 4.
- Full flush and stale targets: empty ROB, already retired squash-after instruction, and repeated older redirects must not dereference invalid entry state; test in Task 4.
- Multi-cycle latter clearing: downgrade once only when former survives; whole-entry deletion must not count as a surviving downgrade; test in Task 3.

---

### Task 1: Define a persistent former/latter entry plan

**Files:**
- Modify: `src/cpu/o3/rob.hh:94-158, 405-430`
- Modify: `src/cpu/o3/rob.cc:134-226, 597-677`
- Modify: `src/cpu/o3/dyn_inst.hh` near the existing ROB status accessors
- Modify: `src/cpu/o3/commit.cc:2355-2470`
- Test: `src/cpu/o3/rob_hybrid.test.cc`

**Interfaces:**
- Replace the planner's single `HybridGroup::length` contract with an entry plan containing `type`, `formerLength`, and `latterLength`; add `memberCount()` and `hasLatter()` helpers.
- Add `HybridEntryState { id, type, formerRemaining, latterRemaining }` to ROB Hybrid state.
- Add `DynInst` fields/accessors `hybridEntryId` and `hybridSlotIsFormer` (default invalid/true for non-Hybrid instructions).
- `planHybridBatch()` returns one plan item per physical entry; `insertHybridBatch()` consumes that exact plan without re-planning.

- [x] **Step 1: Update planner tests with explicit slot lengths.**

  Change expected cases so `SSSSSSSS` is `{NORMAL, 8, 0}`, `CSSSSSSS` is `{CS, 1, 7}`, `SSSCSS` is `{SC, 3, 1}, {NORMAL, 2, 0}`, and `CCCCCCCC` is four `{CC, 1, 1}` entries. Keep the exhaustive legality test and validate `formerLength + latterLength <= limit`.

- [x] **Step 2: Implement the new plan representation and planner.**

  Preserve the current bounded, streaming S/C/N grouping rules in `appendHybridClass()`, and track which slot each accepted member extends. An S appended to a normal S entry extends former; a C paired after it starts latter. A C entry paired with S starts a latter simple run; subsequent S members extend that latter run. CC and SC close the entry. N remains an unpaired singleton. Preserve `group_limit` and independent Rename-window boundaries. Runtime NORMAL semantics must not erase the S/C/N classification needed while planning or the meaning of allocation statistics.

- [x] **Step 3: Add persistent entry state and member metadata.**

  On Hybrid insertion, allocate one `HybridEntryState` per plan item, assign one monotonically increasing entry id, tag each member with that id and its former/latter slot, and retain the existing flat `instList` ordering. Use the new entry deque as the single source of Hybrid entry counts. Retain `threadGroups` for non-Hybrid policies and route shared count/readiness accessors by policy; do not independently mutate two Hybrid queues containing the same counts.

- [x] **Step 4: Update admission and capacity accounting.**

  Keep `hybrid_plan.size()` as the number of physical entries required by a Rename window. Add `countHybridEntries()` for physical-entry accounting. Update `getThreadEntries`, `totalEntries`, `canAllocate`, `numFreeEntries`, `isFull`, `isEmpty`, `readHeadInst`, group-member count helpers, and `resetState`; audit borrowing-accounting readers of `threadGroups` too. Ensure each successful batch consumes exactly `plan.size()` ROB entries regardless of the number of member DynInsts.

- [x] **Step 5: Run planner tests.**

  Run: `scons build/RISCV/cpu/o3/rob_hybrid.test.opt --gold-linker -j64 && build/RISCV/cpu/o3/rob_hybrid.test.opt`

  Expected: all existing and updated planner tests pass, including exhaustive S/C/N legality.

### Task 2: Change Hybrid commit quota from DynInsts to physical entries

**Files:**
- Modify: `src/cpu/o3/commit.cc:1471-1697, 1932-1939`
- Modify: `src/cpu/o3/commit.hh:526-531, 639-675`
- Modify: `configs/example/kmhv3.py:114-128`
- Modify: `src/cpu/o3/BaseO3CPU.py:168-171` (parameter semantics; remove the obsolete instruction quota)
- Modify: `src/cpu/o3/hybrid_trace_check.py`
- Modify: `src/cpu/o3/README.hybrid.md`

**Interfaces:**
- Add ROB helpers `isHeadHybridEntryReady()`, `headHybridEntryId()`, `headHybridEntrySize()`, and `countHybridEntries()`.
- Keep `commitHead()` as the per-DynInst architectural side-effect function.
- Hybrid commit loop tracks `committedEntries` and `committedInsts`; only `committedEntries` limits the loop. Non-Hybrid code retains the expanded group window without a separate instruction quota.

- [x] **Step 1: Remove the independent DynInst quota.**

  Delete the instruction-quota parameter, C++ member, initialization, limit check, assertion, full-cycle statistic, and test configuration reference. Remove README/checker assumptions about a 16-instruction quota. Preserve the default behavior of other ROB policies.

- [x] **Step 2: Add an entry-scoped commit loop.**

  For Hybrid, obtain the head entry's member count before committing it. Allow the loop to process all remaining members of that same entry even after the first member is processed. Increment the physical-entry counter only when the entry becomes empty. Stop before starting a new entry when `committedEntries == commitWidth`. Keep the existing per-DynInst `commitHead()` and commit epilogue. If a fault, strictly ordered operation, interrupt, or squash-after stops progress, retain the unretired members and stop this cycle without freeing the entry. Readiness must preserve the existing fault/non-speculative escape paths. Bound physical entries visited even when draining squashed heads; distinguish drain bandwidth from successful retirement statistics.

- [x] **Step 3: Preserve architectural instruction accounting.**

  Keep `numCommittedDist` and `committedInstType` based on DynInsts. Size the Hybrid distribution using `commitWidth * CROB_instPerGroup`, replacing the fixed factor 8. Add a separate `committedEntries` statistic or debug count, and use physical-entry accounting for Hybrid `commitEligibleSamples`. Remove the instruction-quota assertion and full-cycle statistic from all policies.

- [x] **Step 4: Verify commit-window semantics.**

  Add trace-check assertions that one cycle can retire at most `commitWidth` Hybrid entries, while a single entry may retire multiple DynInsts. Verify that an entry is not partially released merely because the physical-entry width was reached.

### Task 3: Implement latter removal and downgrade to `NORMAL`

**Files:**
- Modify: `src/cpu/o3/rob.hh:405-480`
- Modify: `src/cpu/o3/rob.cc:597-930`
- Modify: `src/cpu/o3/commit.cc:1150-1250, 1400-1450`
- Test: `src/cpu/o3/rob_hybrid.test.cc`

**Interfaces:**
- Add `downgradeHybridEntryToNormal(entryId)` and `removeHybridMember(entryId, slot)` helpers.
- Add `HybridSquashTarget { entryId, slotIsFormer, flushItself }` as ROB squash state.

- [x] **Step 1: Add direct state-transition tests.**

  Test `CS` and `SC` plans after latter removal: type becomes `NORMAL`, latter count becomes zero, former count is preserved, and the entry remains in the entry deque. Test `CC` similarly and ensure no new pairing is attempted.

- [x] **Step 2: Implement downgrade bookkeeping.**

  When the last latter member selected for removal is removed, and the squash target retains a nonempty former slot, set type to `NORMAL`, clear `latterRemaining`, retain the former members and former entry id, and preserve the former DynInsts in program order. If the whole entry is selected, continue deleting it without counting a surviving-entry downgrade. If no former members remain, remove the empty entry instead. Never re-label surviving latter members as former after partial architectural retirement. Do not alter non-Hybrid group state.

- [x] **Step 3: Keep commit readiness correct after downgrade.**

  A downgraded entry is ready when all surviving former members are ready. Its former members are committed as one physical entry under Task 2's quota.

- [x] **Step 4: Validate downgrade tests.**

  Run the targeted gtest and the existing Hybrid exception workload. Expected: the surviving former commits normally after the latter is squashed, with no stale latter member in the ROB trace.

### Task 4: Make squash selection slot-aware

**Files:**
- Modify: `src/cpu/o3/rob.hh:348-366, 459-480`
- Modify: `src/cpu/o3/rob.cc:820-930, 1011-1053, 1055-1123`
- Modify: `src/cpu/o3/commit.cc:801-940, 1204-1245, 1419-1445`
- Modify: `src/cpu/o3/hybrid_trace_check.py`
- Test: `src/cpu/o3/rob_hybrid.test.cc`

**Interfaces:**
- Keep the existing sequence-number squash API for non-Hybrid policies.
- Add a Hybrid API taking the redirect DynInst (or its entry id/slot) and `flushItself`; the API must distinguish `former` and `latter` within one entry.
- `doSquash()` uses a predicate based on `{targetEntryId, targetSlot, flushItself}` rather than only `inst->seqNum > squashedSeqNum`.

- [x] **Step 1: Define the four required squash cases.**

  Implement and test this truth table:

  ```text
  former + flushItself  -> remove former, latter, and all younger entries
  former + flushAfter   -> retain former, remove latter and all younger entries
  latter + flushItself  -> retain former, remove latter and all younger entries
  latter + flushAfter   -> retain both slots, remove only younger entries
  ```

- [x] **Step 2: Pass slot information from Commit.**

  At each Hybrid redirect call site, resolve the original redirect DynInst before the existing `includeSquashInst` sequence-number decrement, read its `hybridEntryId` and `hybridSlotIsFormer`, and pass the original inclusion flag. Derive the last surviving instruction boundary from the same slot decision and publish it through `youngestSeqNum`, `doneSeqNum`, `doneMemSeqNum`, and the retained-instruction recovery anchor. Rename, IQ, and LSQ already consume this sequence boundary, so their queue algorithms need not learn entry/slot semantics. Preserve restart-PC consistency for replay. Keep the legacy sequence-only call for non-Hybrid. Handle `squashAll()` explicitly for trap, TC, empty-ROB, and already-retired squash-after targets; never require a live ROB lookup for those cases or roll back already committed instructions.

- [x] **Step 3: Update the squash walker.**

  Preserve the existing per-cycle squash width and iterator mechanics, but replace the sequence-only removal condition with a slot-aware `shouldSquash(inst)` predicate. When the last latter member is removed in a former-preserving case, call `downgradeHybridEntryToNormal()` exactly once if former members remain.

- [x] **Step 4: Update squash width accounting.**

  Count only members selected by `shouldSquash(inst)` when computing rollback/replay/constant-cycle widths. This keeps recovery latency accounting consistent with the actual members removed, including a former-only surviving entry.

- [x] **Step 5: Add slot-aware tests.**

  Add cases for `[C | S,S]`, `[S,S | C]`, and `[C | C]` covering all four table rows. Assert surviving member sequence numbers, entry type, entry id, and the next tail entry. Run the gtest plus the existing `hybrid_exception.S` difftest.

### Task 5: Final regression and documentation

**Files:**
- Modify: `src/cpu/o3/README.hybrid.md`
- Modify: `src/cpu/o3/hybrid_trace_check.py`
- Modify: `docs/design-docs/frontend/Hybrid_ROB代码修改说明.md`

- [x] **Step 1: Update documentation.**

  Document that Hybrid `commitWidth` counts physical entries, a simple run can retire multiple DynInsts under one entry, and latter squash downgrades the entry to `NORMAL`. Remove claims that Hybrid has a 16-DynInst successful-retirement quota.

- [x] **Step 2: Run focused validation.**

  Run the planner/slot gtest, the Hybrid CoreMark trace checker, and the exception workload. Compare non-Hybrid policy stats against the pre-change baseline.

- [x] **Step 3: Run style/build checks.**

  Run the repository's modified-region style checks and an optimized gem5 build. Report any environment-dependent test gaps separately.



## Implementation record — 2026-09-20

Implemented in the existing HybridCROB checkout at the user's request.

- Kept planner NORMAL-S/C/N types for allocation classification/statistics;
  introduced separate runtime NORMAL/CC/CS/SC types.
- Reused policy-aware `getThreadEntries()`, `headGroupSize()`, and
  `isHeadGroupReady()` instead of duplicating Hybrid-only accessors.
- `HybridEntryState::removeMember()` handles remaining counts and reports a
  surviving-former downgrade; no linear id lookup is needed for head/tail
  removal. Squash setup uses one bounded ROB scan to normalize the boundary.
- Removed the optional DynInst quota globally after the user approved its
  deletion. Hybrid physical quota includes squashed-head draining; legacy
  policies keep their default expanded group window.
- Preserved per-DynInst fault/strictly-ordered/squash-after behavior. Full
  trap/TC flush uses the architectural boundary, not a stale slot lookup.
- Added `hybrid_replay.S` to exercise actual former/latter flushItself paths
  under atResolve and atCommit recovery; retained the exception workload.

Validation artifacts: `/tmp/crob-rtl-20260920/`. Commands and results are
summarized in `src/cpu/o3/README.hybrid.md`. Ten gtests and seven Python checker
tests passed; optimized build, CoreMark/exception/replay difftest and trace
validation passed. Independent code review found no confirmed runtime bug;
both checker findings were reproduced with failing tests and fixed.

Validation limits: full unit suite stopped at six pre-existing SocketTest
failures due to sandbox socket permissions. Legacy policy comparisons matched
all compared old non-host stats except the known uninitialized
`decodeEfficiency` on policy none; cycle/retirement counts matched.
SPEC campaigns, interrupt-specific stress and strictly ordered load stress
were not run. No commit or PR was created.


Follow-up cleanup: the user approved complete removal of the obsolete DynInst
quota, including non-Hybrid compatibility. Optimized build, 10 C++ tests,
7 Python tests and style checks passed. Hybrid CoreMark/exception results
matched the preceding model; four non-Hybrid CoreMark regressions also passed
(with the existing decodeEfficiency initialization variation noted above).
Artifacts: `/tmp/crob-remove-inst-quota-20260920/`.
