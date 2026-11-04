# SDBP real-cache recency protection

`--sdbp-cache-protect-youngest=N` skips the N most recently used entries
when scanning real L2 dead candidates. The default is zero, preserving the
existing dead-first policy. This is independent of sampler recency protection.

The rank counts all entries in the candidate set, including live entries.
After the rank check, the existing L1-copy check still applies. The first
eligible dead entry in candidate order wins; if none qualifies, the original
global LRU candidate is used. Invalid entries and bypass retain priority.
Writebacks continue to preserve the dead bit while updating recency.

The purpose is to delay dead priority after a recent access or writeback.
This may improve downstream write locality but can also delay useful dead
reclamation. It does not change predictor training, source banks, prefetch
PC attribution, or sampler associativity. On an 8-way cache, N=6 still permits
a non-LRU dead victim; N>=7 reduces the victim choice to global LRU.

Validation covers retained dead state across writeback, non-LRU dead selection
at N=2/4/6, live entries in rank calculation, L1 protection, LRU fallback, and
invalid priority. Overall performance requires a common-build comparison of
LRU and SDBP at N=0, N=4, and N=6 with the full weighted workload set.
