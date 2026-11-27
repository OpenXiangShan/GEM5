# MainPipe Prefetch MSHR Eligibility

This follow-up to [GEM5 PR #1351](https://github.com/OpenXiangShan/GEM5/pull/1351) evaluates which L1D MSHR entries a MainPipe prefetch may allocate. It changes the entry-selection policy for MainPipe prefetches; the existing MSHR lifecycle and downstream request sender remain in use.

## Existing GEM5 behavior

Before this change, prefetch admission uses an aggregate occupancy threshold. The check accounts for allocated MSHRs, held refill credits, the queue reserve, and the configured demand reserve. After passing the threshold, `MSHRQueue::allocate()` takes `freeList.front()`. The allocator does not select an entry by request type or MSHR ID.

## RTL configuration used for comparison

The measured XiangShan RTL build has 16 DCache MissQueue entries. Its generated Constantin value is `nMaxPrefetchEntry0 = 14`, and the measured RTL command supplies no Constantin override file. The MissQueue eligibility condition therefore maps ordinary prefetch eligibility to IDs 2–15. IDs 0 and 1 are ineligible for ordinary prefetches, but IDs 2–15 are not prefetch-exclusive: demand requests can also use them.

The DCache wrapper also prints `DcacheMaxPrefetchEntry: 6`; this field is not connected to the MissQueue eligibility input in the measured build and is not the value used to derive the IDs above.

## Candidate behavior

The candidate adds an MSHR-queue lookup for a free prefetch-eligible entry and an admission check that requires both total queue capacity and an eligible free ID. For the default 16-entry queue, MainPipe prefetch allocation selects an available ID in the range 2–15. Demand and other allocations continue to use the existing first-free-entry order.

The MainPipe prefetch path no longer uses the aggregate `demand_mshr_reserve` threshold. It still accounts for total MSHR occupancy, overflow reserve, and held refill credits. Once a miss is admitted, GEM5 continues to allocate the MSHR, schedule the existing downstream sender, send the request to L2, and process the response/refill through the existing code.

## Scope and limitations

This change maps the RTL's entry-ID eligibility boundary. It does not reproduce the RTL's cycle-level `primary_ready` timing, MainPipe S0/S1 tag-query timing, secondary merging, replay/nack behavior, or full MissQueue arbitration.

If no eligible MSHR is free at the S2 allocation point, the current candidate rejects the prefetch packet after it has left the PFQ; it does not return the packet to the PFQ or retain it for retry. This is a known request-backpressure difference from the RTL handshake path and is reported in the [test report](mainpipe-mshr-eligibility-test-report.md).

This is an experimental follow-up branch based on PR #1351. Its performance is workload-dependent; the test report includes the full 38-checkpoint comparison and pressure traces.
