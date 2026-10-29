# Dead-first SDBP sampler with optional youngest-two protection

This experiment starts from `952aece4a7`, the L1-copy-protected SDBP
implementation measured by Actions run 40811060728. The independent sampler
uses invalid entries first, then the first predicted-dead entry in original
way order, then global LRU. The real L2 retains its existing L1-copy filter.

The pure-LRU sampler experiment improved calculix but reduced mcf's benefit.
The purpose of this matrix is to measure whether preserving the sampler's
selective dead-first residency, with a short recency guard, balances those
workloads. No performance improvement is assumed before measurement.

## Optional protection

Enable `--sdbp-sampler-protect-youngest-two` to exclude the most recently used
and second-most recently used sampler entries from the dead-first scan.
Recency positions are computed across the entire sampler set, including live
entries. This does not select the two youngest entries among only dead entries.

1. An invalid entry always wins before any protection is considered.
2. With a full set, determine the youngest two entries using existing
   `lastUse` metadata.
3. Select the first dead entry outside those positions, preserving original
   candidate order. Do not clear the skipped entries' dead bits.
4. If no dead entry qualifies, select global LRU. This fallback includes
   protected entries, which matters for one-way and two-way samplers.

The flag defaults to false and preserves the previously measured dead-first
sampler. Demand hits continue to update recency and train the previous stored
signature live. Evictions retain their existing dead-training rules. Prefetch
hits continue to leave sampler metadata and recency unchanged. The guard adds
no modeled lookup latency or storage beyond the existing recency metadata.

## Performance matrix

All cases use `idealkmhv3.py`, the full
`spec06-rva23-novec-gcc16-1.0c` suite, distributed servers `default`, and 32
jobs per server. Common gem5 arguments are:

```text
--no-pf --l2-replacement-policy=sdbp --sdbp-sampler-num=128
```

| Sampler ways | Protection disabled | Youngest-two protected |
|---|---|---|
| 6 | `--sdbp-sampler-assoc=6` | Add `--sdbp-sampler-protect-youngest-two` |
| 7 | `--sdbp-sampler-assoc=7` | Add `--sdbp-sampler-protect-youngest-two` |
| 8 | Reuse run 40811060728 | Add `--sdbp-sampler-protect-youngest-two` |

The real L2 remains 1 MiB in four 256 KiB, eight-way slices. L3 remains
32 MiB, mostly inclusive. The predictor parameters, threshold 8, bypass off,
workloads, reference model, and L1-copy protection remain unchanged.

## Validation

Targeted core tests cover both protected positions separately, both dead
positions together, original eligible-candidate order, invalid priority,
unchanged dead bits, prefetch-hit recency, and global LRU fallback in small
samplers. Existing replacement-policy tests cover the real-cache L1-copy
filter. A deterministic two-million-event stream per associativity compares
unprotected six-, seven-, and eight-way behavior against the original core.
Configuration smoke runs check that the CLI flag reaches every L2 slice.
