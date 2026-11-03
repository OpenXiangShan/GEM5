# L2 request-origin counters

These counters distinguish real memory-instruction requests from prefetch
traffic at L2, regardless of replacement policy. They are active on caches with
`cache_level=2`, including each inner cache of the sliced L2.

## Origin definitions

Every vector below has the following subnames:

| Subname | Origin at the L2 observation point |
|---|---|
| `l1Demand` | Cacheable requests from a CPU's data requestor, without a prefetch flag: load/store traffic, read-for-ownership, upgrades, and whole-line-store invalidates |
| `l1Prefetch` | Upstream requests carrying `Request::PREFETCH` or `PF_EXCLUSIVE`, including L1 prefetch misses forwarded as `ReadExReq` |
| `l2Prefetch` | Packets dequeued from the L2's local prefetch issue queue, including the sliced L2 forwarder queue |
| `instruction` | Non-prefetch instruction-fetch requests |
| `other` | Remaining requests, including page-table walkers and software-prefetch commands |

CPU data requestor IDs come from the system's thread contexts at startup; PC
presence is not used to infer demand origin. An absent PC does not exclude a
real demand request, and a valid PC of zero is accepted. Page-table walks are
excluded from `l1Demand`, including walker requestors that do not set `PT_WALK`.

These are cache-line requests that actually reach L2. They are not counts of
unique or retired memory instructions, and they do not include demand hits
satisfied entirely in L1. A prefetch-initiated L1 MSHR can later merge a demand;
the downstream request is classified by its metadata at the L2 lookup, before
refill processing can rewrite that metadata.

Writebacks, clean evictions, `WriteClean`, cache-maintenance operations,
uncacheable accesses, and store-prefetch training notifications are excluded.
They are not silently assigned to one of the three requested traffic classes.

## Counters

For sliced L2, prepend
`system.l2_wrappers.slicesN.inner_cache.` and sum all slices/cores.

| Counter | Meaning |
|---|---|
| `l2RequestLookups` | Lookup attempts, including retries rejected by MSHR arbitration |
| `l2RequestAccesses` | Accepted lookups, excluding those arbitration retries |
| `l2RequestTagHits` | Accepted lookups finding a resident tag, even without the needed coherence permissions |
| `l2RequestHits` | Requests served directly by L2; for local prefetches, resident-tag matches that drop the redundant prefetch |
| `l2RequestMisses` | Accepted lookups not counted as service hits, including permission misses |
| `l2RequestMshrHits` | Misses successfully merged into an existing MSHR; local prefetches are instead dropped on an MSHR match |
| `l2RequestMshrMisses` | Requests allocating a new MSHR, after successful arbitration |
| `l2RequestWriteBufferHits` | Local prefetches dropped on a write-buffer match |
| `l2RequestArbRetries` | Lookup attempts rejected by MSHR arbitration |
| `l2RequestNoPc` | Accepted requests without PC metadata |
| `l2RequestHitsOnPrefetchedBlocks` | Hits whose resident block still had its prefetched flag before hit probes/notifications; this is not a prefetch-accuracy metric |

`l2PrefetchReceived` is a separate scalar counting `HardPFReq` packets delivered
to L2's prefetch receiver. In sliced L2, reception and issue-queue dequeue are
different boundaries. Queue residency, admission filtering, and a statistics
reset with packets in flight can make it differ from
`l2RequestAccesses::l2Prefetch`. The main hit-rate denominator is the number of
lookups actually checked, not packets merely received or candidate addresses
generated inside a prefetcher. Directly attached classic-L2 prefetchers can
issue local packets without going through the receiver.

In particular, an L2 prefetch does not use the normal `access()` path. Counting
only `MemCmd::HardPFReq.hits` would miss its resident-tag hits. The counters
observe the existing cache/MSHR/write-buffer tests in `getNextQueueEntry()`
without adding a new replacement-policy touch, lookup event, or queue action.

## Consistency checks

For each of the three traffic classes:

```text
lookups  = accesses + arb_retries
accesses = hits + misses
misses   = mshr_hits + mshr_misses + write_buffer_hits
```

For the current sliced-L2 configuration, aggregate local-prefetch counts also
match the wrapper's existing counters:

```text
l2RequestAccesses::l2Prefetch        = prefetcher.pfIssued
l2RequestHits::l2Prefetch            = prefetcher.pfHitInCache
l2RequestMshrHits::l2Prefetch        = prefetcher.pfHitInMSHR
l2RequestWriteBufferHits::l2Prefetch = prefetcher.pfHitInWB
```

These equations describe lookup/queue actions. A new MSHR need not immediately
send to L3, and an upward snoop can later suppress or satisfy a local prefetch.
Neither MSHR hits nor prefetch tag drops are genuine demand cache hits.

## Summarizing results

```bash
python3 util/sdbp_reference/summarize_l2_requests.py \
    /path/to/ci/archive --out-dir /tmp/lru-request-origins
```

The input can also be a single `stats.txt` or its containing output directory.
The script selects the last completed statistics interval and sums all L2
slices/cores. It checks the identities above and, for the standard single-core
sliced L2, cross-checks existing wrapper prefetch counters.

Outputs:

- `points.csv`: per-point counts and hit rates for all origins.
- `raw-totals.csv`: sums across the measured slices; these are not full-program
  totals and are not weighted benchmark estimates.
- `weighted.csv`: when a CI archive with `cluster.json` is supplied, normalized
  SimPoint and workload-instruction-weighted counts per benchmark.
- `summary.json`: counts, source paths, and the legacy-counter cross-checks.

The main hit rate is `hits / accesses`. `tag_hits / accesses` is also reported
so permission misses can be distinguished. Ratios are computed after weighting
counts, rather than averaging individual hit percentages. Every point for each
included workload must be present before benchmark weighting proceeds.

## Local validation on 2026-11-03

- `gem5.opt` builds successfully.
- LRU/prefetch-on checkpoint smoke runs for `mcf_12886`, `milc_9643`, and
  `sphinx3_74526` complete with difftest, using 10K warmup and 200K total
  instructions. All counter identities and prefetch-counter cross-checks pass.
- Their cycles, instructions, simulation ticks, and key L2 access/miss/MSHR
  statistics exactly match the pre-instrumentation CI binary from commit
  `912c1db8b1`, using the same configuration and checkpoints.
- A prefetch-off `sphinx3_74526` smoke run has zero L1/L2 prefetch accesses.
- Modified-region gem5 style checks, Black 22.6.0 for the Python script, and
  `git diff --check` pass.

These smoke counts are configuration/semantic checks; the full performance
rerun uses the unchanged `baserun.md` LRU/prefetch-on settings and normal
20M warmup plus 20M measurement intervals.
