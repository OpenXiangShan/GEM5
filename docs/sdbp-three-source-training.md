# SDBP training by request source

The L2 sampler is shared by three request classes: L1 demand, L1-targeted
prefetch traffic forwarded by L1, and L2-targeted prefetch traffic issued at
L2. A prefetch ahead/hint issued by the L2 worker belongs to the L2 class,
even when L1 originally generated its address. Memory instructions use the
demand class, including coherence upgrades and atomic operations whose
commands are outside `MemCmd::isDemand()`. The classification follows the existing L2 request counters
and survives coherence-command changes, request copies, and refills.
Instruction fetches, page-table walks, writebacks, maintenance operations,
and software prefetches do not train this sampler.

Each sampler entry stores its PC signature and source. Every accepted hit
trains the old signature live in the old source's predictor bank. Every
replacement trains the valid victim signature dead in the victim source's
bank. Both paths then install the incoming signature/source, refresh sampler
recency, and compute the new dead prediction from the incoming source's bank.
Invalid entries never train. Requests without a valid PC cannot allocate,
replace, train, or refresh sampler entries. A valid PC of zero is accepted.
The explicit `sampler_demand_pc_only` option remains available as a sampler
filter; the default admits all three classes with a PC.

Demand events use the memory instruction PC. Prefetch events use the
independent trigger PC, with the ordinary request PC as a compatibility
fallback. A demand merged into a prefetch MSHR takes the demand class and PC
for refill sampling. If that demand has no PC, its validity is cleared;
the retained prefetch trigger does not become a demand signature.

There are three independent predictor banks. Each has the existing
`predictor_tables` skewed tables, `predictor_entries` entries per table,
`counter_bits` counters, hash/seed configuration, dead threshold, and CRC
live-update rule. Defaults are three tables x 4096 entries x two bits per
bank: nine tables in total, 9 KiB of logical counters per SDBP instance
(three times the previous predictor budget). Hash functions and dimensions
are identical between banks; counters are not shared. The sampler geometry,
dead-first victim order, youngest protection, and L1-copy protection retain
their configured organization.

Real-cache demand and prefetch fills/hits query their respective bank.
The local prefetch cache-presence checks bypass `accessBlock()`, so they
explicitly notify the sampler. Those checks retain the existing real-cache
recency behavior. Prefetches dropped because of an existing MSHR or write
buffer do not fabricate a refill event.

Six source vectors accompany the scalar counters:

- `samplerAccessesBySource`, `samplerHitsBySource`: incoming source.
- `liveTrainingBySource`, `deadTrainingBySource`: old trained signature source.
- `predictionQueriesBySource`, `deadPredictionsBySource`: incoming source used
  by the real-cache predictor.

Each vector has `l1Demand`, `l1Prefetch`, and `l2Prefetch` buckets. The sum of
its buckets equals the corresponding scalar. Dead-training/eviction counters
include sampler replacements triggered by real-cache hits as well as fills.

Validation covers all nine old-to-new source combinations on hits and
replacements, independent banks for identical PCs, source-specific new-entry
predictions, missing/zero PCs, prefetch-hit recency, metadata copies/reuse,
and merged demand PC selection. Checkpoint validation exercises both aligned
and classic L2 paths with difftest. Performance comparisons use the same
commit/configuration/checkpoints/REF for SDBP and LRU with prefetch enabled.
