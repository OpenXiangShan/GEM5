# CDP Prefetch Status for PC 0xae88c

## Context

This note records the current CDP prefetching status for the OMNeT++ 14042
slice, focusing on the load PC `0xae88c`.

The current analysis is based on the worktree and result directories below:

```text
worktree:
/nfs/home/lijiangtao/temp/worktrees/GEM5/kmhv3-Turbo-0518-cdptest

result root:
/nfs/home/lijiangtao/temp/gem5_cdp_analysis/omnetpp_14042
```

Relevant runs:

```text
cdp_off_norestorer
cdp_p0_relax_low6_norestorer
cdp_ae88c_target_chain_norestorer
```

The target checkpoint is:

```text
/nfs/home/share/checkpoints_profiles/spec06_gcc15_rv64gcb_base_260122/checkpoint-0-0-0/omnetpp/14042/_14042_0.376785_.zstd
```

The current target-chain run completed successfully:

```text
/nfs/home/lijiangtao/temp/gem5_cdp_analysis/omnetpp_14042/cdp_ae88c_target_chain_norestorer
```

The run exited normally:

```text
Exiting @ tick 9988956045 because a thread reached the max instruction count
```

## Instruction Pattern

The load PC `0xae88c` belongs to the red-black-tree traversal code around
`std::_Rb_tree_increment`. The relevant local instruction pattern is:

```text
0xae886: ld a5,24(a0)
0xae88c: ld a5,16(a5)
0xae894: ld a4,8(a0)
```

The important property is that `0xae88c` consumes a pointer loaded by an earlier
load. For this kind of pointer-chasing access, the useful prefetch point is
usually when the producer load value becomes available. CDP is therefore a
reasonable mechanism to try first, because it can use loaded data as a pointer
source and generate dependent prefetches.

## Current CDP Configuration

The current best experiment for this PC is the targeted-chain run:

```text
enable_cdp = True
relax_low_accuracy_align = True
relaxed_align_bits = 5
enable_targeted_chain = True
target_chain_pcs = [0xae886, 0xae88c, 0xae894]
target_chain_max_depth = 3
target_chain_scan_offsets = [1, 2, 3]
```

The targeted-chain change is intentionally limited to the three red-black-tree
PCs above. Non-target PCs keep the original CDP scanning path. This avoids
turning the experiment into a global recursive CDP policy and keeps the traffic
increase attributable.

One important implementation fix in the current worktree is that CDP-generated
`AddrPriority` entries preserve `trigger_info`. Without this, prefetches that go
through the prefetch buffer can lose the original trigger PC, which makes PC
level CDPTrace attribution unreliable.

## Load Replay Result

The current `0xae88c` result is:

| Run | TotalBlockCycles | L3Miss |
| --- | ---: | ---: |
| CDP off | 2,031,334 | 29.671% |
| relaxed low6 | 1,367,098 | 13.412% |
| targeted chain | 1,088,727 | 9.595% |

Compared with CDP off, the current targeted-chain CDP version greatly reduces
the L3 miss rate and total block cycles for `0xae88c`.

Compared with relaxed low6, targeted-chain still improves `0xae88c`:

```text
TotalBlockCycles: 1,367,098 -> 1,088,727
delta: -20.36%

L3Miss: 13.412% -> 9.595%
```

For the six P0 PCs that were previously selected as the highest priority group:

```text
CDP off P0 TotalBlockCycles:          6,969,949
relaxed low6 P0 TotalBlockCycles:     3,941,617
targeted-chain P0 TotalBlockCycles:   3,664,166

targeted-chain vs relaxed low6: -7.04%
```

The global ROI-level numbers also improved in this run:

| Metric | relaxed low6 | targeted chain |
| --- | ---: | ---: |
| IPC | 1.360280 | 1.377042 |
| cycles | 14,702,851 | 14,523,879 |
| CDP pfGenerated | 874,329 | 914,095 |
| CDP inserted | 632,169 | 651,045 |
| CDP pfHitCDP | 52,181 | 53,673 |

This means the current targeted-chain policy improves `0xae88c` without showing
a global IPC regression in this OMNeT++ 14042 slice.

## CDPTrace Observation

The targeted-chain run produced CDPTrace rows for the target PCs:

```text
0xae886: Site0  1,282 rows, Site1 14,690 rows
0xae88c: Site0  7,334 rows, Site1 53,932 rows
0xae894: Site0    366 rows, Site1  6,618 rows
```

Compared with the relaxed-low6 run, the targeted-chain version generates
`PFDepth=1/2/3` for the target PCs. This confirms that chained CDP prefetching
is actually active for the intended instruction group.

Targeted-chain specific counters:

```text
targetChainScan:       165,411
targetChainDepthExit:   28,107
targetChainOffsetSkip: 275,685
```

The interpretation is:

- CDP is generating useful dependent prefetch traffic for the target PCs.
- The new chain path is exercised frequently enough to affect `0xae88c`.
- The depth limit is also being hit, so the mechanism is not simply disabled or
  too narrowly gated.

## Address Repetition

For demand accesses from `0xae88c`, the dynamic count is:

```text
dynamic load count: 22,688
```

Exact address and cache-line repetition:

```text
unique exact address / line: 12,437
unique / total: 54.82%
repeat instances: 10,251
repeat instance rate: 45.18%
max count per address / line: 15
```

Frequency distribution among unique exact addresses:

```text
count = 1:      6,105 unique addresses
count = 2:      5,592
count = 3-4:      273
count = 5-8:      280
count = 9-16:     187
```

Short-window locality is weak:

```text
window 64:    avg unique 64.0,    p50 64,    max 64
window 128:   avg unique 124.4,   p50 128,   max 128
window 256:   avg unique 238.3,   p50 256,   max 256
window 512:   avg unique 465.0,   p50 512,   max 512
window 1024:  avg unique 911.0,   p50 1024,  max 1024
```

Reuse distance for exact address / line:

```text
first-time accesses: 12,437
repeat instances:    10,251
immediate repeats:        0

occurrence gap:
min: 50
p10: 93
p25: 281
p50: 13,559
p75: 13,653
p90: 13,653
p99: 13,765
max: 19,452
```

Miss rate by first/repeated access:

```text
first access:   12,437 count, 1,371 miss, 11.024%
repeat access:  10,251 count,   806 miss,  7.863%
```

Miss rate by repeat gap:

```text
first:             12,437 count, 1,371 miss, 11.024%
repeat gap <100:    2,334 count,     0 miss,  0.000%
repeat 100-1k:        753 count,     6 miss,  0.797%
repeat 1k-5k:       1,298 count,    43 miss,  3.313%
repeat 5k-10k:        466 count,    34 miss,  7.296%
repeat >=10k:       5,400 count,   723 miss, 13.389%
```

This PC has moderate global exact-address repetition, but weak short-term
cache-line locality. The accesses behave more like a large working-set tree
traversal than a tight loop over a small set of cache lines.

Page-level locality is much stronger:

```text
unique pages: 623
unique pages / total accesses: 2.75%
page repeated access rate: 99.78%
max accesses per page: 243
```

This is favorable for CDP's VPN-level organization, but it does not mean that
L1/L2/L3 can naturally retain the exact lines until reuse. The remaining misses
are mainly from first-time accesses and long-gap repeated accesses.

## Producer-Consumer Window

For `0xae88c`, producer-consumer matching used:

```text
consumer base pointer = consumer.VAddress - 16
producer PC in {0xae886, 0xae88c}
producer.Result == consumer base pointer
latest producer.ID < consumer.ID
```

The corrected producer-consumer timing uses:

```text
consumer.ReqCreateTick - producer.AtBypassVal
```

or `AtWriteVal`.

It should not use `producer.DataReadyTick` for this analysis, because
`DataReadyTick` is often zero. In the current gem5 path it is only set for a
subset of normal load paths:

```text
request && request->isNormalLd() && !inst->fullForward()
```

Corrected matching result:

```text
consumers: 22,688
matched producers: 22,688
EffL3Miss consumers: 2,177
```

Producer source split for `0xae88c` effective L3 misses:

```text
producer 0xae886: 881
producer 0xae88c: 1,296
```

Correct producer-consumer window distribution for the effective L3 misses:

```text
0-1k ticks:      1,567
1k-5k ticks:        17
5k-20k ticks:      593
>20k ticks:          0

window <5k:   72.76%
window <20k: 100.00%
```

This is the most important difficulty for `0xae88c`: after the true producer
value becomes available, the consumer often needs the dependent address very
soon. CDP has to wait for the producer load data before it can derive the next
pointer, so a large fraction of the remaining misses are close to the hard
timeliness limit of data-dependent prefetching.

## Current Difficulties

### Remaining L3 Miss Rate Is Still High

The targeted-chain run reduces `0xae88c` L3Miss to `9.595%`, but this is still
high for a PC that dominates block cycles. The remaining misses are not simply
because CDP is completely failing to generate prefetches. CDP is active and does
help, but it cannot always get enough lead time.

### CDP Lead Time Is Limited by the Producer Data

For pointer chasing, the next address is only known after the previous load's
data returns. If the consumer appears shortly after the producer data is
bypassed or written back, CDP can only issue the prefetch very late.

For the remaining `0xae88c` effective L3 misses, all matched windows are below
20k ticks, and about 72.76% are below 5k ticks. These are difficult to fix by
ordinary CDP chaining, because the algorithm has no legal address to prefetch
before the producer value is available.

### Short-Term Address Reuse Is Weak

The exact cache-line stream has weak short-window locality. In a window of 64
dynamic `0xae88c` loads, the average number of unique lines is 64.0. This means
the PC is usually moving through different lines rather than repeatedly touching
the same few lines.

Short-gap repeats are already easy to hit:

```text
repeat gap <100 miss rate: 0.000%
repeat 100-1k miss rate:  0.797%
```

The misses are concentrated in first-time accesses and long-gap repeats:

```text
first access miss rate:      11.024%
repeat >=10k miss rate:      13.389%
```

This makes conventional cache reuse less useful. CDP has to bring in future
nodes, not just rely on the fact that an address was seen before.

### Chaining Depth and Offset Have a Pollution Tradeoff

Targeted chain currently scans offsets `[1, 2, 3]` with max depth 3. This helps
`0xae88c`, but increasing offsets or depth is risky:

- More offsets may scan unrelated fields or stale pointers.
- More depth may generate prefetches along paths that the demand stream never
  follows.
- Extra prefetch traffic can increase MSHR, queue, bandwidth, or cache
  replacement pressure.

The current run does not show a global IPC regression, but this remains a
central risk if the policy is generalized beyond this target PC group.

### Original CDP Depth Semantics Limited Recursive Benefit

Before the targeted-chain experiment, CDP prefetch fills from the normal path
were not very useful for recursive dependent prefetching. Depth-0 generated CDP
prefetches could become depth-4 on the refill path and then hit the depth
threshold exit. The targeted-chain path was added to test whether limited,
PC-gated recursive scanning can create more useful lead time.

The result is positive for `0xae88c`, but the remaining miss rate shows that
this alone is not enough.

### Source-Level Attribution Is Not Precise

The slice is built with O3 optimization and loses much of the direct debug
mapping. For this PC, the practical attribution path is:

1. Use the ELF to map PC to function and nearby assembly.
2. Identify the function-level source in:

```text
/nfs/home/lijiangtao/spec/cpu2006v99/
```

3. Reason from optimized assembly and STL/container source, rather than relying
   on exact source line information.

This makes source-level explanation possible, but not as direct as normal
debug-line based analysis.

## Current Interpretation

`0xae88c` is a good CDP target because:

- It is a high-impact load PC by TotalBlockCycles.
- Its address depends on previous load values.
- CDP has already reduced its L3 miss rate significantly.
- Page-level locality is strong enough that VPN-based tracking is meaningful.

At the same time, `0xae88c` is hard to optimize further because:

- The true producer-consumer window is usually short.
- Exact cache-line reuse is not strong in short windows.
- Many remaining misses are first-time or long-gap accesses.
- More aggressive chained prefetching risks stale or unrelated prefetch traffic.

The current result should therefore be viewed as:

```text
CDP is effective for 0xae88c, but remaining misses are dominated by timeliness
and large-working-set traversal effects rather than lack of any CDP activity.
```

## Suggested Next Debug Steps

The next useful debug direction is to split the remaining `0xae88c` effective
L3 misses into more precise categories:

1. No CDP prefetch was generated for the demand line.
2. CDP generated the correct line, but it was issued too late.
3. CDP generated the correct line early enough, but the line was evicted before
   demand.
4. CDP generated nearby or stale-chain addresses, but not the demanded line.

For the PC-specific analysis, the most useful additional trace fields are:

```text
trigger PC
prefetch address
prefetch issue tick
prefetch depth
prefetch site
demand PC
demand address
demand request tick
demand L3 hit/miss
```

The current compact CDPTrace already covers most of this, so the next step does
not require a much larger DB by default. It mainly requires PC-specific joining
between CDPTrace and `LoadLifeTimeCommitTrace`.
