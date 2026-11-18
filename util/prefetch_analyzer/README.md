# GEM5 prefetch failure analysis

This tool records timing-mode classic-cache demand and prefetch facts, correlates them offline, and produces a self-contained HTML report. It supports XS L2 wrappers and offload/forwarder paths. It does not change a prefetch algorithm. Python 3.11+ and its standard library are sufficient; SQLite keeps the complete normalized event stream on disk.

## Record

Build the current checkout with the normal GEM5 build command. Add these options to an existing `kmhv3.py` or `se.py` run:

```bash
--prefetch-trace=prefetch-trace.csv --prefetch-trace-after-warmup
```

The file is written under `--outdir`. `--prefetch-trace-after-warmup` starts output at the first statistics reset after tick 0, matching `--warmup-insts-no-switch`. For a cold run omit it. `--prefetch-trace-start=TICK` and `--prefetch-trace-end=TICK` additionally bound the output to `[start,end)`. Ticks are GEM5 ticks, not CPU cycles. Existing configurations can instead instantiate `system.prefetch_trace = PrefetchTrace(...)` directly.

Tracing is disabled unless the recorder is instantiated. With a recorder configured, request/candidate identities and bounded resident-line identities survive across ROI boundaries; only events inside the ROI are written. A normal simulator exit flushes the buffered output and appends a terminal marker. An interrupted/incomplete file is rejected by the analyzer.

L1 demand collection is independent of prefetch probes and training filters. It records every L1 data load/store lookup, including hits, rejected lookups that cause a replay, AMOs and failed store conditionals. A request's ID survives retries; completion means the CPU accepted its response. `DEMAND_ARRIVE` and `arrival_tick` preserve the first port arrival even if the port rejects the request before any lookup. Latency includes that wait; a prefetch that finishes during it is a late prefetch hit, even when the eventual lookup hits. These are cache requests: an SBuffer request can represent merged stores, and speculative loads can be present. They are not a retired instruction trace. Store Buffer requests retain a trace-only representative translated store (matching the buffer's existing sequence number); its PC/VA/address space are reported without changing the actual cache or prefetch-training metadata.

## Analyze

```bash
python3 util/prefetch_analyzer/analyze.py /path/to/on/m5out \
  --baseline /path/to/off/m5out \
  --control /path/to/original/m5out \
  --output /path/to/new-analysis
```

`--baseline` is the same workload/configuration with all hardware prefetchers disabled (`--no-pf`), **with demand tracing enabled**. `--control` is an optional original prefetch-on run for an incremental optimization/offload comparison. Non-prefetch configuration differences are reported and paired metrics are withheld. Use the same checkpoint, reference, seed, warmup and retirement window. The tools read the final complete statistics block, following the existing TEA analysis workflow. `--cpu` selects the CPU statistics prefix; it defaults to `system.cpu`.

Select a timeline without losing the full event database:

```bash
python3 util/prefetch_analyzer/analyze.py /path/to/on/m5out \
  --pc 0x80001234 --line 0x123400 --output /path/to/new-pc-analysis
```

`--pc` and `--line` together form an intersection. `--line` can select a PA or VA; virtual address matching during attribution requires the exact address-space identity, ContextID and security domain. A known physical address takes precedence over an equal VA. The report displays those fields so coincident VAs can be distinguished. The HTML embeds a bounded 2,000-event preview; `timeline.csv` contains every event in the requested selection and `events.sqlite` retains every normalized input event.

## Read the results

| Artifact | Meaning |
| --- | --- |
| `report.html`, `report.md` | Global metrics, hot PCs, address deltas, failure stages, suggestions and interactive timeline |
| `audit.json` | Input hashes, ROI scope, native counter reconciliation and residual-miss conservation |
| `summary.json` | Machine-readable metrics, censoring and paired comparisons |
| `demands.csv` | One row per accepted L1 cache request; replay count, completion latency and classification |
| `prefetches.csv` | Candidate/request identities, lifecycle times and alias edges |
| `pcs.csv` | PC × cache × address space × ContextID × security-domain patterns and latency ranking |
| `sources.csv`, `source-drops.csv`, `stages.csv` | Origin-source outcomes, drop-event counts and observed phase intervals with P50/P90/P99; incomplete phases are excluded, not zero-filled |
| `pc-comparison.csv` | Paired per-PC miss and observed request-wait changes per retired instruction; compares control first, otherwise baseline |
| `performance.csv` | Cycles, retired instructions, IPC, misses, issued bytes and workload weight for existing performance workflows |
| `events.sqlite`, `timeline.csv` | Complete facts and selected timelines |

Residual misses are classified as `no_prediction`, `dropped`, `late`, `evicted_early`, `invalidated`, `lower_level_only`, or `resident_but_miss`; missing boundary history remains explicit. Facts after a demand cannot explain that earlier miss. An alias points to the surviving request; aliases do not add sends. A used and evicted prediction belongs to an earlier demand episode.

Unused is a **prefetch-request outcome**, not an additional residual-miss category. A request is unused-evicted only when it was sent in the ROI, never used at any level, every observed filled copy was evicted, and every accepted hop has an observed fill. A lower-level eviction alone does not close an upper-level copy. Other unconsumed sent requests remain right-censored. Useful, unused-evicted and right-censored requests partition the unique sends. L1 timeliness compares line-ready time with the first L1 port arrival, not the eventual accepted lookup. `late_prefetch_hit` is a hit outcome, not an additional residual-miss category. A lower-level fill is not an L1 hit. Instruction fetches and other prefetches are excluded from demand-use attribution.

Accuracy is observed useful requests / unique accepted prefetch requests and can be a lower bound when lifecycles are censored. Timeliness is timely useful / useful requests. Traffic includes unique issued request bytes and separate per-hop cache traffic; it is not automatically net extra DRAM traffic. Coverage is the net reduction of misses per retired instruction against Prefetch-Off, requires both traces to match their statistics windows, and can be negative. It does not prove a one-to-one correspondence between dynamic misses in two executions.

Pollution cannot be established by a single unused eviction. Use the off baseline and victim/cache-service evidence before making that causal claim. A timely hit with no IPC gain points to follow-up TEA/DIP analysis, not an automatic algorithm recommendation. PC latency sums overlap and are a ranking signal, not additive CPU stalls. `--weight` supplies a workload weight for that ranking; aggregate suite CPI using the existing profile-aware performance workflow.

## Event protocol

Protocol v3 includes `address_space`: RISC-V full-system translation snapshots contain SATP, VSATP, HGATP (including ASID/VMID and page-table roots), the existing invalidation generation, and effective virtualization/physical routing bits, encoded in hex. SE uses a process-ID namespace. ContextID alone is not treated as an address-space identifier; an unresolved VA without a namespace is not joined. Physical line matching uses the actual PA and security domain.

All rows carry event order, tick, actor-local cycle, actor, owning cache, level, request/candidate/related IDs, PC, VA/PA, ContextID, security domain, requestor, dynamic instruction sequence when available, operation/command, source, result and reason. Unknown values are blank; address zero remains a valid address. IDs are host-side metadata and are not serialized cache contents.

- `DEMAND_ARRIVE`, `DEMAND_ACCESS`, `DEMAND_COMPLETE`, `DEMAND_MERGE`: first port arrival, lookup, response acceptance and merging into a prefetch MSHR.
- `PF_OBSERVE`, `PF_TRAIN`, `PF_TRAIN_DROP`: probe observation, actual training and training-buffer loss.
- `PF_CANDIDATE`, `PF_IDENTIFIED`: generation before queue/page/admission filtering, and the existing `pfIdentified` accounting point.
- `PF_ENQUEUE`, `PF_DROP`, `PF_ALIAS`, `PF_TRANSLATE`, `PF_RESOLVE`, `PF_DEQUEUE`: translation/queue decisions and duplicate-to-survivor identity edges.
- `PF_OFFLOAD`, `PF_OFFLOAD_ACCEPT`, `PF_FORWARD_ACCEPT`, `PF_FORWARD_DEQUEUE`: transfer to a different prefetch/cache owner and the forwarder's physical FIFO dequeue; these are not memory-send acceptance. The real prefetcher owns native dequeue accounting.
- `PF_SEND_RETRY`, `PF_SEND_ACCEPT`, `PF_SNOOP_ACCEPT`: downstream rejection, actual acceptance, or an upstream cache committing to return the data.
- `PF_FILL`, `PF_FILL_CLASSIFY`, `PF_FIRST_USE`, `PF_EVICT`: fill arrival and ready tick, actual pure/merged counter classification after servicing targets, first data-demand use, and replacement/invalidation.

`PF_FILL` describes the MSHR state at arrival. `PF_FILL_CLASSIFY` matches `pfOnlyFill` / `pfMergedWithDemand` after extracting serviceable targets, which can differ when targets are deferred. A candidate may be deduplicated or offloaded; no one-candidate/one-downstream-packet assumption is made. Internal algorithm decisions before producing a candidate need algorithm-specific instrumentation for a deeper confidence/state diagnosis.

## Validate

```bash
TMPDIR="$HOME/.tmp" python3 -m unittest discover \
  -s util/prefetch_analyzer -p 'test_*.py' -v
```

`examples/stream.c` is a sequential load/store control. Build it with the RISC-V cross compiler, then run the same GEM5 binary with tracing off/on, prefetch off, and only the offload switch changed. Require identical non-host native statistics for tracing off/on. A matched full-window trace is reconciled against demand hit/miss counters, prefetch identification/dequeue/removal counters and pure/merged fill counters in `audit.json`. ROI-boundary lifecycles are censored; a partial tick window is not compared with a full-window native counter.
