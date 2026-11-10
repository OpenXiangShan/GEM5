# Prefetch trigger PC

Every hardware prefetch emitted through the classic cache prefetch framework
carries an independent trigger PC when its originating request has a PC. The
metadata is always populated, including when `tag_prefetch` is false.

Read it from a `Request` with `hasPrefetchTriggerPC()` and
`getPrefetchTriggerPC()`. Check validity before reading: an unavailable PC is
not replaced by zero, and a genuine PC of zero remains valid.

A demand-triggered prefetch records that demand instruction's PC. A prefetch
triggered by a returned prefetch inherits the originating instruction's PC.
LLDP records the producer load PC, not the predicted consumer PC. The field
identifies an origin; it does not predict which instruction will consume the
prefetched data.

`TriggerPC` snapshots this attribution when `PrefetchInfo` is constructed.
Both `PrefetchInfo` and the buffered `PrefetchInfo_old` copy it. The shared
queued prefetcher tags translation requests and final `HardPFReq` requests.
Worker/forwarder paths retain the same request, and request copies preserve
the field. Changing the ordinary request PC during SDBP demand/refill merging
does not change the trigger PC. Reusing a request with `setVirt()` clears its
trigger validity along with the other nonsticky request flags.

When multiple candidates merge, this metadata follows the representative
candidate selected by the existing queue/filter logic; it is not a list of
all contributors. Existing source and depth attribution are independent.
The generic prefetchers' older candidate constructors now explicitly use
`PF_NONE`, matching the available source taxonomy without adding new sources.

Targeted unit tests cover validity, zero PCs, buffered snapshots, chains after
merges, request copies, translation metadata, reuse, and all source IDs:

```sh
scons build/RISCV/mem/cache/prefetch/trigger_pc.test.opt -j64
build/RISCV/mem/cache/prefetch/trigger_pc.test.opt
```

The implementation adds request/snapshot metadata and debug observations;
SDBP training and prediction still use their existing demand PC semantics.
The metadata consumes host memory but does not introduce simulated resource
costs or new predictor decisions.

Validation on 2026-11-04: the optimized RISCV build and 8 unit tests passed.
Twelve checkpoint smoke runs passed; ten comparable baseline pairs had
identical simulation statistics. GDB checked 57,266 outgoing requests across
13 source categories against queue snapshots, with no missing/mismatched
trigger PCs in those samples. GDB runs also matched ordinary-run statistics.
Results and reproduction scripts are under
`analysis/prefetch-trigger-pc-20261104/RESULTS.md`.
