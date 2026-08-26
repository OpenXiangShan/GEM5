# Mockingjay Background

## Source

This note summarizes Ishan Shah, Akanksha Jain, and Calvin Lin,
"Effective Mimicry of Belady's MIN Policy," HPCA 2022,
DOI 10.1109/HPCA53966.2022.00048. The local source is
`shah2022.pdf` in the original workspace. Section and figure references below
refer to the published paper.

## Motivation

Belady's MIN evicts the cache line whose next use is furthest in the future,
but it cannot be implemented directly because the future access stream is not
known. Recent learned policies such as SHiP and Hawkeye turn the problem into a
binary classification: a line is Cache Friendly/Cache Averse, or likely
reused/not likely reused. The paper identifies two consequences of that
coarse-grained decision:

* A small predictor error flips the class rather than slightly changing an
  eviction order.
* Lines in the same class tie frequently, so the policy falls back to LRU.

Earlier reuse-distance policies KPK and IbRDP nominally predict an estimated
time of arrival (ETA), but choose a victim using `max(ETR, age)`. That lets age
replace ETA as the priority near the predicted reuse point, which is the
opposite of MIN's desired behavior. KPK also lacks a long history; IbRDP uses
global access distance rather than per-set access distance. The paper reports
43% reuse-distance prediction accuracy for IbRDP and 85% for Mockingjay. It
attributes the improvement to a long history and to measuring distance in the
access domain of the affected set.

Mockingjay therefore predicts a multi-class reuse distance for each load PC,
converts it to ETA/ETR, and compares line priorities only when a replacement
is needed. A prediction error changes a victim only when it changes the
relative ETA order of candidates, making the policy more tolerant than a
binary predictor.

## Design

The predictor is PC-based, not a joint PC/address predictor. The address is
used to locate a cache set and to identify the same block in the sampled
history. The sampled history records the PC that most recently accessed that
block, and trains the RDP for that previous PC.

### Sampled cache

The sampled cache records a long history for a small number of physical cache
sets. A record stores a block-address tag/hash, the last access timestamp, and
the last PC signature. On every access to a sampled set:

1. A sampled-cache hit trains the previous signature with the elapsed number
   of accesses to that same set, then refreshes the record with the current
   signature and timestamp.
2. A sampled-cache miss evicts the sampled-cache LRU record. Its prior
   signature is trained as a scan with `INF_RD`, because the block did not
   reappear within the history.
3. The sampled-set timestamp is then incremented. Timestamp arithmetic is
   compared only within the same sampled set; as in the paper, the 8-bit counter
   can distinguish one wrap, while an entry untouched for a full counter period
   is a rare bounded-history approximation.

For the paper's 2 MB, 16-way single-core LLC, the history is eight times the
associativity (128 set accesses), represented by 32 sampled sets and a 5-way,
512-set sampled cache. The history stores only unique blocks, not data.

### Reuse distance predictor

The Reuse Distance Predictor (RDP) is a direct-mapped table. In the paper's
single-core configuration it maps an 11-bit hash of PC plus hit/miss state to
a 7-bit reuse-distance value. The prefetch-aware form also includes a
prefetch bit. A new signature is initialized with the sampled distance; an
existing entry is updated with a temporal-difference rule that moves it toward
the sample while limiting the influence of an outlier.

`INF_RD` denotes a scan. The paper uses `INF_RD = 127` and treats predictions
near it as scans (`MAX_RD = 104` in the prose). The authors' public ChampSim
implementation uses the scalable form `INF_RD = associativity * 8 - 1` and
`MAX_RD = INF_RD - 22`; this implementation adopts that form so that an
8-way L2 slice keeps the same eight-times-associativity history semantics.

### ETR and victim selection

On an insertion or a hit promotion, the RDP prediction initializes the line's
Estimated Time Remaining (ETR). ETR is a coarse-grained representation of ETA:
all non-scan lines in a set are decremented once every eight accesses to that
set. ETR remains signed after crossing zero. A positive value means the
predicted reuse is still in the future; a negative value means the prediction
has elapsed. Scan lines are never aged.

On a miss, Mockingjay selects the valid line with maximum `abs(ETR)`. If the
absolute values tie, the negative ETR wins. This is the direct ETA ordering
that old ETA policies lost by combining ETR with age. If the incoming line's
predicted ETR is a scan, or is farther than every resident line, Mockingjay
bypasses the fill. Writebacks are not bypassed.

With a prefetcher, the paper approximates Flex-MIN: a sampled interval ending
in a prefetch (`*-P`) is trained with an inflated reuse distance. The paper
uses a 2x penalty for one core and 1.5x for multicore, saturated at `INF_RD`.

## Experimental Results in the Paper

The paper evaluates a 2 MB, 16-way LLC in ChampSim, with 33 memory-sensitive
SPEC06/SPEC17/GAP programs and 100 four-core mixes. All policies have a
32 KB replacement-state budget.

| Configuration | Mockingjay vs. LRU | Comparator |
| --- | ---: | --- |
| Single core, no prefetcher | +5.7% IPC | SHiP +3.4%, Hawkeye about +4.4% |
| Four cores, no prefetcher | +15.2% weighted speedup | SHiP +7.6%, Hawkeye +12.9% |
| Single core, prefetcher | +3.6% | Harmony +2.0% |
| Four cores, prefetcher | +13.3% weighted speedup | SHiP +6.7%, Harmony +11.1% |
| High-MPKI CVP, prefetcher | +20.1% | Harmony +13.4% |

Other reported evidence is that a per-set reuse distance changes IbRDP
prediction accuracy from 43% to 85%, and that Mockingjay falls back to an
LRU-like eviction for 7.8% of evictions versus Hawkeye's 13.8%. The paper also
reports 9.1% lower uncore energy and 9.8% lower DRAM traffic than Hawkeye for
its evaluated single-core prefetching configuration.

## L2 Transfer Caveat

The paper evaluates an LLC, not Kunminghu's sliced private L2. Its performance
figures are design evidence, not a performance target for this port. In the
target hierarchy, L1 filtering changes both PC distributions and reuse
distances, and each L2 slice observes only its routed address subset. The
implementation must therefore preserve per-set distance semantics and then
establish benefit with the requested GCC15 SPEC06 CI comparison.
