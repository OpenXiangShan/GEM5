# GEM5 prefetch failure analysis

This tool records timing-mode classic-cache demand and prefetch facts, correlates them offline, and produces a self-contained HTML report. It supports XS L2 wrappers and offload/forwarder paths. Tracing and analysis are optional; prefetch algorithms and their default settings stay with the existing configuration. Python 3.11+ and its standard library are sufficient; SQLite keeps the complete normalized event stream on disk.

## Framework layout

```text
L1 demand ports / cache service / prefetch generation and queues
                         |
                 system.prefetch_trace
                 IDs, ordering, buffering
                         |
                 prefetch-trace.csv
                         |
                  parse_trace.py
                         |
                  events.sqlite
                         |
             correlate.py + stages.py
                         |
        demands / prefetches / sources / stages / PCs
                         |
                  report.py -> HTML
```

| Layer | Entry points | Responsibility |
| --- | --- | --- |
| Configuration | `configs/common/Options.py`, `CacheConfig.py` | Instantiate the recorder and select its observation window |
| Recorder | `src/mem/cache/prefetch/PrefetchTrace.py`, `prefetch_trace.hh/.cc` | Allocate identities, order and buffer events, track each resident cache copy |
| Demand collection | `src/mem/cache/base.cc`, `cache.cc` | Capture first L1 arrival, lookup, MSHR merge, completion, fill, use and eviction |
| Prefetch collection | `src/mem/cache/prefetch/base.cc`, `queued.cc`, `forwarder.cc`, `worker.cc` | Capture training, candidates, translation, queues and cross-level handoffs |
| Request metadata | `src/mem/request.hh`, RISC-V TLB and O3 LSQ | Carry request identity, address-space snapshots and representative store context |
| Import | `parse_trace.py` | Validate the CSV and import normalized events into SQLite |
| Reconstruction | `correlate.py` | Reconstruct requests, aliases and cache copies; classify demand outcomes |
| Aggregation | `stages.py` | Attribute source outcomes and compute stage samples and percentiles |
| Analysis and reporting | `analyze.py`, `report.py`, `report_template.html`, `symbolize.py` | Read native statistics/configuration, compare runs, resolve functions and export reports |
| Regression examples | `test_analyzer.py`, `examples/stream.c` | Check lifecycle semantics and provide a sequential-access workload |

The simulator records facts at existing service points. The offline tools
interpret those facts, so metrics and visualizations can evolve without rerunning
a simulation whose trace already contains the required events. A new observation
needs a collection hook, a corresponding reconstruction rule and a visible result.
The tests cover the request and cache-copy relationships those rules depend on.

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

Add `--elf /path/to/matching-workload.elf` to resolve PC functions and source locations in `pcs.csv` and the searchable HTML table. The report records the ELF path and SHA256. `--addr2line` selects the resolver (default `riscv64-linux-gnu-addr2line`); this optional feature needs that binutils executable. Addresses outside the supplied ELF and absent debug information remain unknown. Paired runs must use the same workload ELF.

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

### CSV fields

The file begins with version, block-size and configured-window comments, followed
by this 26-column header. On normal exit, `trace_end_tick` and `trace_complete=1`
close the observation window. `ROI_BEGIN` records when collection actually starts.

```csv
event,order,tick,cycle,actor,level,request_id,prefetch_id,related_id,pc,vaddr,paddr,secure,context_id,requestor_id,inst_seq,op,size,result,source,reason,ready_tick,cache,command,address_space,arrival_tick
```

| Field | Meaning |
| --- | --- |
| `event` | Action at a collection point, such as `DEMAND_ACCESS` or `PF_FILL` |
| `order` | Monotonically increasing output order; breaks ties at the same tick |
| `tick` | GEM5 global event time |
| `cycle` | Integer tick divided by the actor's clock period |
| `actor` | Object performing the action, such as a cache or prefetcher |
| `level` | Cache level, with L1 represented as 1 |
| `request_id` | Current Request identity, preserved across demand retries |
| `prefetch_id` | Candidate identity carried through the prefetch lifecycle |
| `related_id` | Related prefetch identity, including alias survivor or MSHR merge target |
| `pc` | Current request PC: trigger PC at generation, consumer PC at first use |
| `vaddr` | Relevant virtual byte address; the predicted target at generation |
| `paddr` | Relevant physical byte address, when known |
| `secure` | Security domain included in physical-line matching |
| `context_id` | Thread context used with the address-space identity |
| `requestor_id` | GEM5 requestor component ID |
| `inst_seq` | Dynamic instruction sequence number, when available |
| `op` | Operation: load, store, prefetch or other |
| `size` | Current Request size in bytes; a candidate can carry its triggering load's size |
| `result` | Event result, such as hit, miss, pure or used |
| `source` | Current event's source metadata; algorithm attribution uses the candidate's source |
| `reason` | Explanation such as queue-full, translation queue or replacement |
| `ready_tick` | Time at which a filled cache copy can serve data |
| `cache` | Owning cache, distinct from the actor |
| `command` | GEM5 packet command, such as `HardPFReq` |
| `address_space` | Translation-environment snapshot for VA matching |
| `arrival_tick` | First demand arrival at the L1 port, retained across retries |

Addresses are hexadecimal. An empty field means the event has no value for that
field; a zero ID means no associated object. SQLite adds `pa_line` and `va_line`
aligned to the recorded block size. Time comparisons use `(tick, order)` and the
relevant cache copy's ready time.

### Follow a lifecycle

A candidate can start with its triggering demand Request, then create separate
translation and send Requests. First use carries the later consumer Request.
`prefetch_id` connects these stages; `request_id` follows the Request at the current
stage. `PF_ALIAS.related_id` points to the surviving prefetch, and
`DEMAND_MERGE.related_id` identifies the prefetch MSHR serving that demand.

For example, a recorded Stream prefetch with ID 37320 was triggered by Request
37302, translated with Request 37322 and sent with Request 37330. Its first user
was demand Request 40766. At first use, `source=0` described the consumer; the
candidate retained the generating Stream source. Its 8-byte candidate metadata
came from the triggering load, while the send requested a 64-byte line.

Use SQLite to inspect a prefetch chain and then its consumer. Replace these IDs
with the ones selected in your report:

```sql
SELECT event, "order", tick, request_id, prefetch_id,
       cache, paddr, result, reason, ready_tick
FROM events
WHERE prefetch_id = 37320
ORDER BY tick, "order";

SELECT event, "order", tick, pc, result, arrival_tick
FROM events
WHERE request_id = 40766 AND event LIKE 'DEMAND_%'
ORDER BY tick, "order";
```

Data can arrive before the cache permits access. `PF_FILL.tick` records arrival;
`ready_tick` records availability. Compare availability with the consumer's first
L1 arrival to distinguish timely service from a prefetch that finished during
port backpressure. Track each `(cache, physical line, secure)` copy separately
before aggregating useful and unused requests across levels.

## Validate

```bash
TMPDIR="$HOME/.tmp" python3 -m unittest discover \
  -s util/prefetch_analyzer -p 'test_*.py' -v
```

`examples/stream.c` is a sequential load/store control. Build it with the RISC-V cross compiler, then run the same GEM5 binary with tracing off/on, prefetch off, and only the offload switch changed. Require identical non-host native statistics for tracing off/on. A matched full-window trace is reconciled against demand hit/miss counters, prefetch identification/dequeue/removal counters and pure/merged fill counters in `audit.json`. ROI-boundary lifecycles are censored; a partial tick window is not compared with a full-window native counter.
