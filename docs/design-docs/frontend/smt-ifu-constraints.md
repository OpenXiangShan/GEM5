# SMT2 IFU constraint probes

Three independent opt-in probes for `configs/example/smt_idealkmhv3.py`.
The defaults keep the existing shared 64KB/4-way ICache and dual-hart fetch.
Use the default as the baseline and enable one flag at a time for the first
performance comparison; no combination sweep is needed.

| Flag | Effect |
| --- | --- |
| `--smt-ifu-64b` | Admit at most 64 effective FetchBlock bytes per cycle across both harts. |
| `--smt-icache-partition` | Restrict hart 0 to ways 0/1 and hart 1 to ways 2/3 in the existing ICache. |
| `--smt-icache-banks` | Delay conflicting hit data reads in eight 8B banks per way. |

The 64B probe approximates the low-conflict envelope of RTL 2fetch. It charges
the predicted block range (through a predicted taken branch, including its
known 2B/4B size), not the 66B decoder buffer. An over-budget second block is
deferred intact and receives the next first selection when still eligible.
It does not force each hart to 32B or reduce the number of fetch target threads.

The partition probe retains 256 sets, four physical ways, the original shared
MSHR pool, tag ports, and downstream interface. Lookup and replacement use
the requesting context's two ways. Cross-hart merged physical-line misses
allocate a copy for each requesting hart. Snoop invalidation and downgrade
reach both copies; evicting one copy does not evict the L1's snoop-filter entry
while another copy remains. This probe supports a single SMT2 CPU with context
IDs 0/1, BaseSetAssoc tags, and no ICache hardware prefetcher or clean writebacks.
As in classic cache, a resident line is pinned while a same-physical-line
MSHR is outstanding. This also covers a miss in the other hart's way group:
an eviction must not clear the snoop-filter holder between the downstream
response and the L1 fill. An extra hart-owned copy is skipped if its way group
has no replaceable entry; it never borrows the primary fill's temporary block.

The bank probe uses the hit block's resident way and physical set after tag
lookup. Overlapping banks in the same way at different sets cannot read in
one cycle; same-set broadcasts and different-way accesses can proceed together.
Future reservations account for a read already delayed into a later cycle.
Only the predicted valid fragment of each line request reserves data banks.
A miss is not assigned a fictitious hit way and does not hold the other hart's
hit until the miss completes. The existing cache miss and response paths remain
in use. The fixed 66B request buffer and its cache accesses remain unchanged.

These are throughput probes, not a complete RTL frontend model. In particular,
the bank probe does not model WayLookup timing, virtual-set differences,
refill-write contention, miss-forward data reads, or IFU predecode width.

Relevant counters:

- `fetch.smtFetchByteBudgetStalls`: whole-block deferrals from the 64B budget.
- `icache.icacheBankDelayedReads` and `icache.icacheBankDelayCycles`: hit reads
  delayed by data banks and the sum of their added cycles.
- `icache.icachePartitionExtraFills`: extra copies for merged cross-hart fills.
- Existing `fetchTargetsStartedByThread`, ICache misses/replacements, and
  committed per-thread progress provide the remaining first-pass checks.

Build with `scons build/RISCV/gem5.opt --gold-linker -j<N>`.
Run the bank regression with
`scons build/RISCV/mem/cache/icache_banks.test.opt -j<N>` followed by
`build/RISCV/mem/cache/icache_banks.test.opt`.
Use identical checkpoint, warmup, measurement, memory, and multi-context REF
settings for the baseline and the three one-flag comparisons. Short smoke
checkpoints establish operation only; SPEC06 conclusions need the full matched
SMT 0.3c set.
