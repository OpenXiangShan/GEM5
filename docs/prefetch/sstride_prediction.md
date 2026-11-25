# KMH SStride prediction state

`XSStridePrefetcher` models the KMH stride predictor directly. The separate
`sstride_require_first_issue_tlb_hit` training gate selects eligible loads;
the predictor applies the following state transitions to those loads without
a prediction-policy switch.

The prediction state follows `StrideMetaBundle.update` in XiangShan
`8b15f06520bc7fc264095b11ece1d0e268153de7`:

- Save the low 25 virtual-address bits before computing the next signed step.
  Crossing the history boundary can change the observed direction.
- An absolute step below 128 bytes is invalid. Always update the previous
  address, including invalid and zero steps.
- Use a 3-bit saturating confidence counter. A valid exact match increments
  it; a valid mismatch decrements it. Replace the stride on a mismatch only
  when the **old** confidence is at most one.
- Generate candidates only for a valid exact match with **old** confidence
  at least three. A mismatch never generates candidates using the old stride.
- Keep the existing L1/L2 lookahead distances of four and 32 strides and the
  existing downstream filters, translation, queues and issue arbitration.

`stride_history_bits` defaults to 25. Wider histories are available for
isolated experiments; they are not the KMH hardware configuration. Values
outside 1--63 are rejected. The predictor requires 64-byte cache lines.
The legacy prediction path and its fuzzy matching, short-stride suppression,
adaptive depth, redundant table and non-stride filter settings are removed.

Table capacity/replacement, PC hashing, training-filter scheduling, and
downstream filtering remain separate model choices. This implementation does
not claim cycle-for-cycle equivalence for the whole prefetch pipeline.

The state transition takes constant time and fixed state per entry. Enable
the `XSStridePrefetcher` debug flag to inspect address history, stride,
confidence and candidate decisions. Existing performance counters are reused.

Table capacity remains unchanged. To test the RTL's 16-entry table separately:

```sh
--param 'system.cpu[0].dcache.prefetcher.sstride.stride_entries="16"'
```

The quotes around `16` are required because the existing parameter is a
MemorySize. Capacity, PC hashing, replacement and training scheduling need
their own validation; the prediction state update does not alter them.

Validation covers the sixth-access trigger, negative strides, saturation and
mismatch recovery, invalid-step history updates, history wrap and high-address
aliasing. Local state tests and same-image microbenchmark traces are retained
with the experiment artifacts outside the source commit.
