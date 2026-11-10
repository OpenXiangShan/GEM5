# SDBP bypass with the XiangShan single-core hierarchy

Use `--cache-hierarchy-policy=xiangshan_single_core` with the same hierarchy
policy for LRU, SDBP without bypass and SDBP with bypass. Bypass defaults off;
`--sdbp-enable-bypass` enables it. Ordinary reads keep `ReadSharedReq`, while
MSHR promises allow deferred writable grants. Clean L1 victims send CleanEvict;
only dirty bypass victims return data directly to L3 through L2's write buffer.
The former bypass-specific ReadExReq ownership request is removed.

The selected baserun three-source predictor is retained. Instruction fetches
are excluded from prediction. SDBP fills invalid ways before considering demand
refill bypass; bypassed fills still update the independent sampler once.

See `docs/design-docs/cache/xiangshan-single-core-hierarchy.md` for protocol
boundaries and `tests/gem5/cache_eviction/hierarchy_bypass.py` for independent
clean/dirty guest checks with mostly exclusive L3, writable grants and routing.
The old two-core bypass regression used the superseded ReadExReq policy and is
replaced by this single-core regression. The new policy requires one CPU.
