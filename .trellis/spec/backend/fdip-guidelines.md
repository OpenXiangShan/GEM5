# FDIP Guidelines

> Executable contracts for the current FTQ-directed ICache prefetch model in
> `fdip-phase2-xsdev`.

---

## Overview

Read this file before changing any of the following:

- `src/cpu/o3/fetch.cc`
- `src/cpu/o3/fetch.hh`
- `src/cpu/o3/fdip_cleanup.hh`
- `src/cpu/pred/btb/ftq.hh`
- `src/cpu/pred/btb/decoupled_bpred.hh`
- `src/cpu/pred/btb/decoupled_bpred.cc`
- `src/cpu/pred/BranchPredictor.py`
- `configs/common/Options.py`
- `configs/common/xiangshan.py`
- `configs/example/kmhv3.py`
- `src/mem/request.hh`
- `src/mem/cache/base.hh`
- `src/mem/cache/base.cc`
- `src/mem/cache/cache.cc`
- `src/mem/cache/cache_probe_arg.hh`
- `src/mem/cache/xs_l2/SlicedCacheAccessor.*`

This contract documents the **current implemented** FDIP model, not the full
paper-faithful or RTL-complete future design.

---

## Implementation Data Flow / 实现数据流

<img src="./assets/fdip-data-flow.svg" alt="FDIP data flow" width="100%">

当前实现是 Phase 1 / 1.5 的 **FTQ-directed ICache prefetch**：它从
FTQ 的 runahead / prefetch target 读取预测窗口，尽力提前把对应 ICache
line 送入 memory/cache 路径。它不是完整 RTL 对齐或论文 faithful 的 future
design；FDIP 只影响 best-effort 性能路径，demand fetch 仍是 correctness
path。

上图是默认阅读入口：蓝色主线描述当前 FDIP best-effort prefetch path，
绿色主线保留 demand fetch 的 correctness path，紫色虚线是 direct probe /
selected-way hint，橙色虚线是 redirect / epoch cleanup，红色虚线表示 epoch
mismatch 后的 old-path FDIP refill drop。编号 1-7 对应下方关键步骤；图内只放
英文代码标识以降低 SVG 字体依赖，语义以本节文字为准。

<details>
<summary>Editable source / Mermaid fallback</summary>

### 主数据流

```mermaid
flowchart LR
  subgraph Control["control path: BPU / FTQ"]
    BPU["BPU / DecoupledBPUWithBTB"]
    FTQ["FTQ\nfetchptr + prefetchptr"]
    Target["FetchTarget\nstartPC / predEndPC / ftqId"]
    BPU --> FTQ --> Target
  end

  subgraph FetchLane["FDIP prefetch request path: Fetch"]
    Run["Fetch::runFdip(tid)\npeek prefetchptr"]
    Cover["computeFdipLineAddrs(...)\nactual fetch coverage"]
    Meta["Request::XsMetadata\nfdipEpoch / fdipFtqId / fdipStartPC"]
    Issue["startFdipTranslation\nfinishFdipTranslation\nissueFdipReadyLine"]
    Run --> Cover --> Meta --> Issue
  end

  subgraph MemoryLane["MMU + BaseCache path"]
    MMU["MMU translation"]
    Cache["BaseCache / L1I\nFDIP-scoped request"]
    Refill["FDIP refill / completion"]
    Stats["processFdipCompletion\nuseful / late / unused / drop stats"]
    MMU --> Cache --> Refill --> Stats
  end

  subgraph ProbeLane["direct probe / selected-way hint path"]
    Probe["direct probe hit\nno real miss allocation"]
    Hint["fdipSelectedWayValid\nfdipSelectedWay / tick"]
    Probe --> Hint
  end

  Target --> Run
  Issue --> MMU
  Cache -.-> Probe
  Hint -.-> Stats

  Demand["Demand fetch\ncorrectness path"] --> Cache
```

### Redirect / Epoch 清理图

```mermaid
flowchart LR
  Redirect["redirect / squash / reset"]

  subgraph FTQClean["control cleanup"]
    Squash["FTQ::squashAfter(...)"]
    PrefetchState["fetchptr / prefetchptr\nfinishPrefetchTarget state"]
  end

  subgraph FetchClean["FDIP local cleanup"]
    Partial["partial FDIP state"]
    Pending["pending requests\noutstanding accounting"]
    Hints["per-thread probe hints"]
  end

  subgraph RefillDrop["refill/drop path"]
    Epoch["fdipEpoch mismatch"]
    Drop["shouldDropFdipRefill(...)\ndrop old-path FDIP refill"]
  end

  Redirect -.-> Squash -.-> PrefetchState
  Redirect -.-> Partial -.-> Pending -.-> Hints
  Redirect -.-> Epoch -.-> Drop
```

</details>

### 关键步骤

- `BPU / FTQ` 产生 `FetchTarget`，`fetchptr` 驱动 demand fetch，
  `prefetchptr` 暴露给 FDIP runahead 使用。
- `Fetch::runFdip(tid)` 只查看 `prefetchptr` 对应的 future target，并受
  `fdip_issue_bandwidth`、`fdip_max_outstanding` 等 best-effort 约束限制。
- `computeFdipLineAddrs(...)` 按 demand fetch 相同的 actual fetch coverage
  计算 cacheline，边界 line 只有在真实 fetch 覆盖到时才纳入。
- `Request::XsMetadata` 携带 `fdipEpoch`、`fdipFtqId`、`fdipStartPC`，并在
  direct probe 命中时携带 `fdipSelectedWay*` hint。
- FDIP request 经过 `startFdipTranslation` / `finishFdipTranslation` 后进入
  MMU + `BaseCache`；cache 侧只把这些策略作用在 FDIP-scoped request 上。
- direct probe 可在命中时形成 selected-way hint 或完成 FDIP line，避免把
  旁路命中误建模成真实 miss allocation。
- redirect / squash 通过 epoch 与 per-thread cleanup 清理 partial state、
  pending request、outstanding accounting 和 probe hint；epoch mismatch 的旧路径
  FDIP refill 由 `shouldDropFdipRefill(...)` 丢弃。

### 读图约定 / Legend

- 蓝色实线：主 FDIP prefetch path，表示 FTQ-directed best-effort request 从
  `prefetchptr` 到 MMU / L1I / stats 的路径。
- 绿色实线：demand correctness path，表示 `fetchptr` 驱动的 architectural
  demand fetch；FDIP 不得改变该路径的正确性。
- 紫色虚线：direct probe / selected-way hint path，表示 FDIP-scoped bypass
  或 hint，不代表真实 miss allocation。
- 橙色虚线：redirect / epoch cleanup path，表示 squash/reset 后的 partial
  state、pending request、outstanding accounting 与 probe hint 清理。
- 红色虚线：old-path refill drop，表示 `fdipEpoch` mismatch 且 drop policy
  开启时，旧路径 FDIP refill 不安装进 L1I。
- 编号 1-7：对应“关键步骤”的维护契约边界；图中节点不是逐函数调用栈，新增实现
  应优先保持这些边界清晰。

---

## Scenario: Current FDIP Runtime Contract

### 1. Scope / Trigger

- Trigger: any change to FTQ-directed FDIP issue, predictor-side FDIP knobs,
  fetch-side FDIP state/lifecycle, request metadata, cache-side old-path refill
  drop, recent-unused suppression, or trace-mode FDIP observability.

### 2. Surfaces

Current parameter/config surface:

- `DecoupledBPUWithBTB.enable_fdip`
- `DecoupledBPUWithBTB.bpu_runahead_entries`
- `DecoupledBPUWithBTB.fdip_lookahead_entries`
- `DecoupledBPUWithBTB.fdip_issue_bandwidth`
- `DecoupledBPUWithBTB.fdip_max_outstanding`
- `DecoupledBPUWithBTB.prefetch_lines_per_ftq`
- `DecoupledBPUWithBTB.fdip_flush_partial_on_epoch_change`
- `DecoupledBPUWithBTB.fdip_drop_refill_on_epoch_mismatch`
- `DecoupledBPUWithBTB.fdip_recent_unused_cycles`

Current runtime/state surface:

- `Fetch::runFdip(ThreadID tid)`
- `Fetch::computeFdipLineAddrs(...)`
- `Fetch::resetFdipPartialState(ThreadID tid)`
- `Fetch::shouldDropFdipRefill(ContextID, const Request::XsMetadata &) const`
- `BaseCache::shouldSuppressFdipLine(Addr addr, bool is_secure, uint64_t cooldown_cycles) const`

Current request metadata surface:

- `Request::XsMetadata.fdipEpoch`
- `Request::XsMetadata.fdipFtqId`
- `Request::XsMetadata.fdipStartPC`
- `Request::XsMetadata.fdipSelectedWayValid`
- `Request::XsMetadata.fdipSelectedWay`
- `Request::XsMetadata.fdipSelectedWayTick`

### 3. Contracts

1. **FDIP-off invariance**
   - When `enable_fdip == false`, FDIP must not change architectural behavior.
   - `bpu_runahead_entries` must not throttle normal predictor behavior unless
     FDIP is enabled.

2. **Coverage contract**
   - FDIP cacheline coverage must follow the same actual fetch coverage used by
     demand fetch:
     - `startPC`
     - `predEndPC`
     - `fetchCoverageSpan(...)`
     - `fetchCoverageLastLineAddr(...)`
   - A cross-boundary 4B control-tail line must be included only when actual
     fetch coverage naturally reaches that line.

3. **Issue contract**
   - FDIP issue is best-effort only.
   - Demand fetch must remain functionally prior to FDIP.
   - Current tunable limits are:
     - `fdip_issue_bandwidth` (cachelines per cycle)
     - `fdip_max_outstanding` (cachelines)

4. **Redirect / partial-state cleanup contract**
   - Redirect/squash/reset must clear per-thread FDIP partial state:
     - thread-local state object
     - pending FDIP requests for that thread
     - outstanding-line accounting for removed in-flight requests
     - probe hints for that thread
   - The helper-level contract is implemented in:
     - `src/cpu/o3/fdip_cleanup.hh`

5. **Old-path refill contract**
   - When `fdip_drop_refill_on_epoch_mismatch == true`, old-path FDIP refills
     must not install into L1I on epoch mismatch.
   - This policy must stay scoped to FDIP traffic only.

6. **Recent-unused suppression contract**
   - Suppression must key on physical cacheline identity plus security domain:
     - `(blkAddr, isSecure)`
   - It must never suppress demand fetch.

7. **Probe / selected-way contract**
   - Direct probe hit may complete an FDIP line without allocating a real miss.
   - The resulting selected-way hint is current-model behavior and must remain
     explicitly FDIP-scoped.

### 4. Validation & Error Matrix

| Condition | Expected Behavior | Enforcement / Symptom |
|-----------|-------------------|------------------------|
| `enable_fdip == false` | FDIP path inactive; no FDIP requests issued | smoke / behavior parity |
| `prefetch_lines_per_ftq == cover_actual_fetch_range` and target spans cross-boundary control-tail | second cacheline is included | `fetch_coverage.test` witness |
| redirect/squash happens with pending FDIP partial state | partial state cleared, old hints removed | `fdip_cleanup.test` witness |
| epoch-mismatched FDIP refill with drop policy enabled | refill not installed into ICache | `fdipDroppedRefill` rises |
| recently-unused FDIP line revisited within cooldown window | FDIP line suppressed, demand unaffected | `fdipFilteredRecentUnused` rises |
| trace-mode high-I$ sanity run with FDIP enabled | `fetch.icacheStallCycles` changes and timeliness stats are non-trivial | `crypto14` / `compute_int_32` |

### 5. Good / Base / Bad Cases

- Good:
  - `crypto14` and `compute_int_32` show non-trivial:
    - `fetch.icacheStallCycles`
    - `fdipUsefulHits`
    - `fdipLate`
    - `fdipUnused`
- Base:
  - `srv67` smoke / 5M with tuned-on config stays performance-neutral to
    slightly positive while reducing repeated bad lines.
- Bad:
  - reintroducing `stallReason[0]`-based stall accounting
  - keying recent-unused tracking by raw `Addr` only
  - treating helper-level cleanup witness as a full mid-flight redirect harness

### 6. Tests Required

For current-model edits, the narrow required validation set is:

- `openspec validate add-fdip-icache-prefetch --strict`
- `scons build/RISCV/gem5.opt -j<N>`
- `scons build/RISCV/cpu/pred/btb/test/fetch_coverage.test.opt --unit-test -j<N>`
- `build/RISCV/cpu/pred/btb/test/fetch_coverage.test.opt`
- `scons build/RISCV/cpu/o3/fdip_cleanup.test.opt --unit-test -j<N>`
- `build/RISCV/cpu/o3/fdip_cleanup.test.opt`

For tuned-on sanity / regression:

- one focused `srv67` smoke
- one focused `srv67` 5M run
- two high-I$ sanity traces:
  - `crypto14`
  - `compute_int_32`

### 7. Current Limitations

- This is still a Phase 1 / 1.5 model, not full RTL alignment.
- No full tag/data split ICache interface exists yet.
- `prefetchPtr` / FTQ peek plumbing is not yet paper/RTL complete.
- `system.cpu.iew.fetchStallReason::IcacheStall` still remains zero in current
  trace-mode runs even though `system.cpu.fetch.icacheStallCycles` now moves.
- Redirect cleanup proof is helper-level, not a full fetch-stage directed
  harness.

### 8. Decision Guidance

Current recommendation:

- treat the current stack as complete for the P0/P1 stabilization cut
- stop at Phase 1 / 1.5 unless a new research question specifically requires
  deeper RTL-fidelity work
