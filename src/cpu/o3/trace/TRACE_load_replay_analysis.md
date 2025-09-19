# Trace Load Replay Hang Analysis (2025-09-17)

## Observation
- Running `configs/example/xiangshan.py --enable-trace-mode --trace-file=/nfs/home/share/glr/champsim_traces/cvp1_public/compute_fp_1.gz --trace-format=champsim --trace-max-insts=100000` aborts with commit watchdog: `Commit stage is stucked for more than 40,000 cycles!`
- `m5out/trace_debug_verbose.log` shows head ROB entry `[sn:1] lw gp,0(a3)` never reaches `isExecuted()`; commit loops on `Can't commit, Instruction [sn:1] ... is head of ROB and not ready`.
- `m5out/trace_debug_load.log` indicates `sn:1` enters `loadDoRecvData`, immediately triggers `setCacheMissReplay`, and replays endlessly. No `writebackReg()` occurs.

## Memory Hierarchy Behavior
- `trace_debug_cache.log` demonstrates a full miss/refill path: L1D miss → L2 miss → L3 miss → DRAM `ReadCleanReq` → `ReadResp` returned and inserted into each cache.
- L1D pushes data to LSQ via `SingleDataRequest::CustomResp` / `addToBus`, then issues `Bus_Clear`, clearing the bus entry before the replayed load consumes it.

## Root Cause
1. **Replay Discards Request Early**: `loadDoRecvData` calls `loadSetReplay(inst, request, false)` on first miss, resetting `savedRequest` state while the original `SingleDataRequest` stays in the cache hierarchy.
2. **No Wake-Up Path**: After replay, `loadDoSendRequest` never sets `inst->wakeUpEarly()` for `sn:1`; the next `loadDoRecvData` therefore hits the fallback branch (`!fullForward() && !cacheHit()`), re-issuing miss replay even though data already arrived.
3. **Bus Clearance**: Because no load consumes the data, L1 issues `CustomBusClear`, removing the bus entry for `[sn:1]`. Subsequent replays still lack data and loop forever.
4. **Normal vs Trace Flow Difference**: In non-trace runs, replays keep the request active, a later pipeline wake-up sets `wakeUpEarly()`, and data forwarded from bus triggers `forwardFromBus()` → `writebackReg()`. Trace mode injects instructions without a fetch pipeline; the load relies purely on trace metadata but still uses the actual memory subsystem, violating assumptions (request persistence, timely wake-up).

## Recommendations
1. **Retain or Recreate Requests on Replay**: Ensure `loadSetReplay` either keeps the `savedRequest` alive or reinitializes it before the next pipeline issue so the miss response can still be matched.
2. **Force Wake-Up / Mark Cache Hit**: On trace-mode memory responses, explicitly set `inst->setCacheHit()` and/or `inst->setFullForward()` to bypass the miss replay branch; alternatively guarantee `setWakeUpEarly()` before the first replay, allowing `forwardFromBus()` to fire.
3. **Gate Bus_Clear During Trace Replays**: Delay L1's `CustomBusClear` until the matching load has been reissued in trace mode, or keep the bus entry alive longer when the pipeline is trace-driven.
4. **Trace-Mode Fast-Path**: Consider short-circuiting load completion using trace metadata—e.g., synthesize memory data directly via `writebackReg()` and skip the full memory replay when `cpu->isTraceMode()`.
5. **Diagnostic Checks**: Add assertions/logging around `loadSetReplay` in trace mode to verify `savedRequest` lifecycle and whether `wakeUpEarly` gets set on replays.

Implementing (1)+(2) should unblock forward progress; (3) and (4) are robustness improvements specific to trace-mode semantics.

