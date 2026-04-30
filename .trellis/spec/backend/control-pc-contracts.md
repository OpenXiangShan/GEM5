# Control-PC and Split-Control Contracts

> Executable contracts for RISC-V partial decode, predictor-visible PC
> semantics, fetch owner migration, and inherited predictor params.

---

## Overview

Read this file before changing any of the following:

- `src/arch/generic/decoder.hh`
- `src/arch/riscv/decoder.hh`
- `src/arch/riscv/decoder.cc`
- `src/cpu/pred/btb/common.hh`
- `src/cpu/o3/fetch.cc`
- `src/cpu/o3/trace/TraceFetch.cc`
- `src/cpu/pred/btb/ras.cc`
- `src/cpu/pred/BranchPredictor.py`
- `src/cpu/pred/btb/microtage.cc`
- predictor-local tests under `src/cpu/pred/btb/test/`

This contract exists because the same branch can now be viewed through three
different PCs:

- `startPC`: architectural instruction-start address
- `controlPC`: predictor-visible control/tail-halfword address
- `ownerStartPC`: earliest instruction-start PC owned by a fetch target

Treating those views as interchangeable is a cross-layer bug.

---

## Scenario: Partial RISC-V Decode and Control-PC Reconstruction

### 1. Scope / Trigger

- Trigger: any change to partial instruction delivery, RISC-V decoder
  readiness, or logic that reasons about `startPC` versus `controlPC`.

### 2. Signatures

- `virtual void InstDecoder::moreBytes(const PCStateBase &pc, Addr fetchPC,
  size_t validBytes)` in `src/arch/generic/decoder.hh`
- `PartialInstResult PartialInstBuffer::pushChunk(Addr currentInstPC,
  Addr fetchPC, uint32_t chunk, size_t validBytes)` in
  `src/arch/riscv/decoder.hh`
- `Addr BranchInfo::startPC() const` in `src/cpu/pred/btb/common.hh`
- `Addr BranchInfo::controlPC() const` in `src/cpu/pred/btb/common.hh`
- `Addr BranchInfo::endPCExclusive() const` in `src/cpu/pred/btb/common.hh`
- `Addr BranchInfo::coverageEndPC(Addr naturalStreamEndPCExclusive) const`
  in `src/cpu/pred/btb/common.hh`
- `bool BranchInfo::triggerPCCoveredByFetchWindow(Addr instStartPC,
  Addr fetchEndPCExclusive) const` in `src/cpu/pred/btb/common.hh`

### 3. Contracts

- `pc.instAddr()` is the architectural instruction-start PC for the instruction
  being assembled.
- `fetchPC` is the address of the delivered chunk. The decoder accepts partial
  delivery only through the 3-argument `moreBytes(...)` overload.
- `PartialInstBuffer::pushChunk(...)` requires:
  - `validBytes > 0`
  - `validBytes <= sizeof(instBits)` (currently 4)
  - `fetchPC >= currentInstPC`
  - `fetchPC - currentInstPC + validBytes <= sizeof(instBits)`
- `BranchInfo::pc` stores the predictor-visible `controlPC`, not necessarily
  the architectural start PC.
- `BranchInfo::startPC()` returns:
  - `instStartAddr` when `hasExplicitStartAddr == true`
  - otherwise `startPCFromControlPC(pc, size)`
- For `size <= 2`, `startPC() == controlPC()`.
- For a 4-byte control instruction that starts at `0x101e`,
  `controlPC() == 0x1020` and `endPCExclusive() == 0x1022`.
- `coverageEndPC(naturalEnd)` must keep the stream alive until the entire
  instruction tail is covered:
  - return `max(endPCExclusive(), naturalEnd)`
- `triggerPCCoveredByFetchWindow(instStartPC, fetchEndPCExclusive)` is true
  only when:
  - `instStartPC == startPC()`
  - `fetchEndPCExclusive > controlPC()`
- The comparison is strictly `>` for the end-exclusive fetch boundary. When
  `fetchEndPCExclusive == controlPC()`, only the leading halfword is available
  and redirect must stay blocked.

### 4. Validation & Error Matrix

| Condition | Expected Behavior | Enforcement / Symptom |
|-----------|-------------------|------------------------|
| `validBytes == 0` or `validBytes > 4` | Reject input | `assert(...)` in `PartialInstBuffer::pushChunk(...)` |
| `fetchPC < currentInstPC` | Reject input | `assert(...)` in `PartialInstBuffer::pushChunk(...)` |
| `offset + validBytes > 4` | Reject input | `assert(...)` in `PartialInstBuffer::pushChunk(...)` |
| low 2 bytes form a compressed instruction | decoder becomes ready after 2 bytes | `PartialInstResult::ReadyCompressed` |
| only first 2 bytes of a 4-byte RVI instruction are available | stay pending | `PartialInstResult::NeedMoreBytes` |
| full 4 bytes are assembled | decoder becomes ready | `PartialInstResult::ReadyFullWidth` |
| `fetchEndPCExclusive == controlPC()` for a cross-boundary 4-byte control instruction | redirect stays blocked | `triggerPCCoveredByFetchWindow(...) == false` |

### 5. Good / Base / Bad Cases

- Good:
  - `pushChunk(0x1000, 0x1000, 0x00000001, 2)` returns
    `ReadyCompressed`.
- Base:
  - `pushChunk(0x1000, 0x1000, 0x00000013, 2)` returns
    `NeedMoreBytes`, and a second `pushChunk(0x1000, 0x1002, 0x00001234, 2)`
    returns `ReadyFullWidth`.
- Bad:
  - treating `controlPC()` as the instruction-start PC
  - treating `fetchEndPCExclusive >= controlPC()` as sufficient coverage for a
    cross-boundary 4-byte control instruction

### 6. Tests Required

- Build and run decoder helper tests:
  - `scons build/RISCV/arch/riscv/riscv_decoder.test.opt -j<N>`
  - `build/RISCV/arch/riscv/riscv_decoder.test.opt`
- Assertion points:
  - `RiscvDecoderHelper.Compressed16_ReadyAfter2B`
  - `RiscvDecoderHelper.Rvi32_WaitsForSecondHalf`
  - `RiscvDecoderHelper.Rvi32_ReadyAfterSecondHalf`
  - `RiscvDecoderHelper.ResetClearsPartialState`
- Build and run fetch coverage tests when `coverageEndPC(...)` or
  `triggerPCCoveredByFetchWindow(...)` changes:
  - `scons build/RISCV/cpu/pred/btb/test/fetch_coverage.test.opt --unit-test -j<N>`
  - `build/RISCV/cpu/pred/btb/test/fetch_coverage.test.opt`

### 7. Wrong vs Correct

#### Wrong

```cpp
if (fetchEndPCExclusive >= info.controlPC()) {
    allowRedirect = true;
}
```

#### Correct

```cpp
if (info.triggerPCCoveredByFetchWindow(instStartPC, fetchEndPCExclusive)) {
    allowRedirect = true;
}
```

---

## Scenario: Split-Control Ownership Migration Between FTQ Entries

### 1. Scope / Trigger

- Trigger: any change to `FetchTarget`, FTQ/fetch handoff, taken matching,
  trace-mode fetch, or tests that reason about stream ownership.

### 2. Signatures

- `Addr FetchTarget::ownerStartPC() const` in `src/cpu/pred/btb/common.hh`
- `void FetchTarget::setOwnerStartPC(Addr pc)` in
  `src/cpu/pred/btb/common.hh`
- `bool FetchTarget::hasSplitControlOwnership() const` in
  `src/cpu/pred/btb/common.hh`
- `bool FetchTarget::ownsInstPC(Addr inst_pc) const` in
  `src/cpu/pred/btb/common.hh`
- `bool FetchTarget::shouldTakeSplitControlOwnershipFrom(
  const FetchTarget &previous, Addr inst_pc) const` in
  `src/cpu/pred/btb/common.hh`
- `bool FetchTarget::isTakenControlAt(Addr inst_pc) const` in
  `src/cpu/pred/btb/common.hh`
- `void Fetch::maybeMigrateSplitControlOwner(ThreadID tid, Addr inst_pc)` in
  `src/cpu/o3/fetch.cc`

### 3. Contracts

- `FetchTarget::startPC` is the stream anchor of the current FTQ entry.
- `FetchTarget::ownerStartPC()` returns:
  - `startPC` by default
  - a smaller instruction-start PC only when the target owns a split 4-byte
    control instruction that began in the previous block
- `hasSplitControlOwnership()` is true only when `ownerStartPC() < startPC`.
- `ownsInstPC(inst_pc)` defines the legal fetch/decode coverage range:
  - `[ownerStartPC(), predEndPC)`
- `shouldTakeSplitControlOwnershipFrom(previous, inst_pc)` is true only when:
  - `hasSplitControlOwnership()`
  - `previous.predEndPC == startPC`
  - `previous.startPC < startPC`
  - `ownerStartPC() <= inst_pc`
  - `inst_pc < startPC`
- Fetch-side code must not re-encode the ownership predicate locally. Use the
  helper instead so fetch, tests, and future callsites stay aligned.
- Taken matching must compare the current instruction-start PC against
  `predBranchInfo.startPC()`, not `predBranchInfo.controlPC()`.
- Trace mode is an explicit boundary:
  - `Fetch::maybeMigrateSplitControlOwner(...)` must return immediately when
    `isTraceMode() == true`
  - trace wrong-path NOP sizing continues to use
    `stream.predBranchInfo.startPC()`
- Return-address logic continues to use the architectural start/fall-through
  view:
  - `BTBRAS` should keep using `takenEntry.fallThroughPC()` and `entry.startPC`
  - do not replace those with `controlPC()`

### 4. Validation & Error Matrix

| Condition | Expected Behavior | Enforcement / Symptom |
|-----------|-------------------|------------------------|
| `hasExplicitOwnerStartAddr == false` | `ownerStartPC() == startPC` | helper default |
| adjacent stream owns split control (`ownerStartPC() < startPC`) | handoff can occur before `buildInst` | `shouldTakeSplitControlOwnershipFrom(...) == true` |
| following stream is not adjacent to previous stream | no handoff | helper returns `false` |
| trace mode enabled | no owner migration | early return in `maybeMigrateSplitControlOwner(...)` |
| taken-match uses `controlPC()` instead of `startPC()` | branch fires one halfword late | wrong-path or redirect mismatch |
| RAS/trace code switched from `startPC()` or `fallThroughPC()` to `controlPC()` | return or wrong-path bookkeeping drifts | behavior regression, not necessarily an assert |

### 5. Good / Base / Bad Cases

- Good:
  - `following.startPC = 0x1020`
  - `following.ownerStartPC() = 0x101e`
  - `following.predEndPC = 0x1022`
  - instruction-start PC `0x101e` belongs to `following`
- Base:
  - ordinary stream without split ownership keeps
    `ownerStartPC() == startPC`
- Bad:
  - fetch code manually inlining five ownership checks
  - tests duplicating the handoff boolean instead of calling the helper
  - taken-match comparing `curr_pc == predBranchInfo.controlPC()`

### 6. Tests Required

- Build and run BTB ownership tests:
  - `scons build/RISCV/cpu/pred/btb/test/btb.test.opt --unit-test -j<N>`
  - `build/RISCV/cpu/pred/btb/test/btb.test.opt --gtest_filter=BTBTest.FetchTargetOwnerStartPC_DefaultsToStreamStart:BTBTest.Rvi4B_ControlPC_CrossBoundaryPredictInNextBlock:BTBTest.SplitControlOwnershipMigratesBeforeBuildInst:BTBTest.SplitControlOwnershipDoesNotMigrateToTakenTarget:BTBTest.TakenMatchUsesOwnerTargetStartPC`
- Build and run fetch coverage tests when stream-end logic changes:
  - `scons build/RISCV/cpu/pred/btb/test/fetch_coverage.test.opt --unit-test -j<N>`
  - `build/RISCV/cpu/pred/btb/test/fetch_coverage.test.opt`
- Build and run a short trace-mode sanity when trace or fetch ownership logic
  changes:
  - `scons build/RISCV/gem5.opt -j<N>`
  - run a short trace workload that exercises branch + squash
  - assert the run exits by the intended stop condition, not by a fetch/trace
    invariant failure

### 7. Wrong vs Correct

#### Wrong

```cpp
const bool in_stream = stream.startPC <= curr_pc && curr_pc < stream.predEndPC;
predict_taken = stream.predTaken && curr_pc == stream.predBranchInfo.controlPC();
```

#### Correct

```cpp
const bool in_stream = stream.ownsInstPC(curr_pc);
predict_taken = !is_microop && stream.isTakenControlAt(curr_pc);
```

---

## Scenario: Inherited TimedBaseBTBPredictor Params in Python Config

### 1. Scope / Trigger

- Trigger: any change to `src/cpu/pred/BranchPredictor.py` subclasses of
  `TimedBaseBTBPredictor`, or any runtime logic that depends on inherited
  `blockSize` / `numDelay`.

### 2. Signatures

- `TimedBaseBTBPredictor.blockSize = Param.Unsigned(...)` in
  `src/cpu/pred/BranchPredictor.py`
- `TimedBaseBTBPredictor.numDelay = Param.Unsigned(...)` in
  `src/cpu/pred/BranchPredictor.py`
- `TimedBaseBTBPredictor::TimedBaseBTBPredictor(const Params &p)` in
  `src/cpu/pred/btb/timed_base_pred.cc`
- `unsigned MicroTAGE::getBranchIndexInBlock(Addr branchPC, Addr startPC)` in
  `src/cpu/pred/btb/microtage.cc`

### 3. Contracts

- `TimedBaseBTBPredictor` owns the real SimObject params for `blockSize`,
  `predictWidth`, and `numDelay`.
- A subclass may override an inherited param value only by plain assignment.
  Example:
  - `blockSize = 32`
  - `numDelay = 0`
- Do not redeclare an inherited field as a new `Param.*` in the child class.
  That creates a shadow field in the generated params struct.
- The C++ base constructor reads `p.blockSize` and `p.numDelay` from the base
  params struct. A shadowed child param does not update those base fields.
- `MicroTAGE::getBranchIndexInBlock(...)` assumes `blockSize > 0` because it
  computes:
  - `alignedPC = startPC & ~(blockSize - 1)`
- `MicroTAGE` table-shape vectors must remain aligned with `numPredictors`:
  - `tableSizes`
  - `TTagBitSizes`
  - `TTagPcShifts`

### 4. Validation & Error Matrix

| Condition | Expected Behavior | Enforcement / Symptom |
|-----------|-------------------|------------------------|
| child uses plain assignment for inherited param | base constructor sees overridden value | normal runtime behavior |
| child redeclares `blockSize` or `numDelay` as `Param.*` | base constructor keeps stale/default base field | shadow-field bug |
| `blockSize == 0` at runtime | branch position is aligned against an invalid block size | bogus `MicroTAGE: branch ... exceeds block ...` warnings and wrong position mapping |
| vector lengths differ from `numPredictors` | constructor/setup mismatch | constructor assert / config-time failure |

### 5. Good / Base / Bad Cases

- Good:
  - `class MicroTAGE(TimedBaseBTBPredictor): blockSize = 32`
- Base:
  - subclass keeps inherited defaults and does not mention the param
- Bad:
  - `blockSize = Param.Unsigned(32, "...")`
  - `numDelay = Param.Unsigned(0, "...")`

### 6. Tests Required

- Build the runtime target:
  - `scons build/RISCV/gem5.opt -j<N>`
- Run a short workload with `MicroTAGE` instantiated.
- Assertion points:
  - no `MicroTAGE: branch ... exceeds block ... (blockSize=0, ...)` warning
  - branch position and table lookup behave normally
- When table-shape vectors or `numPredictors` change, instantiate the config
  path that constructs `MicroTAGE`; config parsing alone is not enough.

### 7. Wrong vs Correct

#### Wrong

```python
class MicroTAGE(TimedBaseBTBPredictor):
    blockSize = Param.Unsigned(32, "Block size in bytes")
    numDelay = Param.Unsigned(0, "Delay")
```

#### Correct

```python
class MicroTAGE(TimedBaseBTBPredictor):
    blockSize = 32
    numDelay = 0
```
