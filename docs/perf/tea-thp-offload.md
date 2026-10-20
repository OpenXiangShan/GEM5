# TEA comparison for THP stream offload

This branch adds the TEA implementation from `35634e841f` (including the fetch
translation override fix in `fdbb505712`) to the frozen stream-offload revision
`0cf962d0630d65b11c81af2e408aee61fb2012a6`. The older cache interface reports
completion depth through `pkt->req->getAccessDepth()`; a nonzero depth marks
`ST_L1`. The remaining sampling and attribution semantics are unchanged.

Both comparison arms use this branch and `configs/example/kmhv3.py`:

```text
baseline: --enable-tea --tea-sample-period=1 -P system.l2_wrappers[:].prefetcher.offload_low_accuracy=False
offload:  --enable-tea --tea-sample-period=1 -P system.l2_wrappers[:].prefetcher.offload_low_accuracy=True
```

The baseline bypasses the offload policy. The candidate uses the existing
stream-only policy with CDP ratio threshold 0.2 and source accuracy threshold
0.8. All other parameters, checkpoints, reference, warmup and measurement
windows must match. TEA is disabled by default.

For a full paired CI run, use `manual-perf.yml`, pin both `branch` inputs to the
same source SHA, select `kmhv3.py`, `custom-spec06`, and checkpoint profile
`/nfs/home/share/cirunner/checkpoint-trigger/spec06_gcc16_rva23_novec_thp_260907_seed1`.
Leave benchmark filtering empty. The normal CI defaults provide 20M warmup and
20M measurement instructions and the pinned difftest reference with memdedup.

After both runs finish, compare their complete archive directories:

```bash
python3 util/tea/compare_offload.py /absolute/baseline/archive \
  /absolute/offload/archive --output /absolute/comparison
```

The comparator checks completion, matching profiles and configurations, TEA
sampling, CSV/statistics conservation, and selects the final measurement dump.
It writes `points.csv`, `benchmarks.csv`, `pc-psv.csv` and `comparison.json`.
SimPoint weights are normalized within each input. Inputs are combined within
a benchmark using their full instruction counts. PC/PSV deltas remain separated
by input so identical PCs in different programs are not merged.

`baseline_cpi - offload_cpi` is positive when cycles per instruction decrease.
Four state deltas and exact PSV partitions are additive. Event bits overlap:
`ST_L1` and `ST_TLB` must not be added; `data_memory_union` counts their union.
Negative PSV rows preserve cancelled, pending and unknown attribution. A PC's
weight also includes computing cycles, and an event/PC delta alone does not
establish an independently removable latency or a causal performance gain.
