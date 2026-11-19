# Fast-fetch mask routing

`enableFastFetchMaskRouting` selects single-consumption routing instead of the
legacy early fast offer followed by a later normal copy. It defaults to false
in `BaseO3CPU` and is enabled with fast fetch in the KMHV3 experiment.
`enableFastFetch` independently controls early acceptance: mask routing remains
active with fast disabled so both sides of an experiment use the same normal
pipeline.

Fetch still constructs at most `fetchWidth` instructions per cycle and sends at
most `decodeWidth` instructions. KMHV3 uses 32 and 8 respectively. Mask routing
does not widen this link, the Decode output, or the ICache request.

## Block limits and continuation

After an eligible backend recovery, Fetch offers an ordered prefix from the
oldest FTQ block. The per-block limits are `decodeWidth` instructions and
`fastFetchBytes` bytes measured from the block start, not from each packet.
Incomplete instructions and instructions outside either limit remain normal.
Already available prefixes can be sent before the entire block is constructed.

While fast is possible, a packet ends at the FTQ boundary even when fewer than
eight instructions are available. A completely accepted block permits the next
block to use fast. Once any instruction is routed normal, that recovery epoch
stays normal, including already in-flight fast offers. A twelve-instruction
block therefore sends eight fast instructions and four normal instructions on
separate cycles; it does not drain the remaining four through fast.

`enableFastFetchOnEmpty` optionally permits a new opportunity without a redirect.
It defaults to false in `BaseO3CPU` and is enabled in KMHV3. Fetch can rearm before
sending its next packet when both Decode buffers are empty, Decode is unblocked,
no commit squash or ROB squash is active, and no older normal packet remains in
the forward link. Unrouted packets conservatively count as normal. A routed fast
packet with no normal lanes does not occupy that link for this check. The current
normal input has already been consumed by Decode before Fetch performs the check.
Pending redirects, trap/idle states and CPU drain prevent empty-buffer rearming.

An empty-buffer opportunity receives a new monotonically increasing epoch, so
stale stop feedback cannot close it. It preserves the Fetch-side instruction
budget and control cutoff for the current FTQ block: emptying the buffer cannot
make the ninth instruction of that block fast. Byte offsets still refer to the
original block start. Normal instructions retain the original delivery delay,
buffer capacity and scan accounting. Set `enableFastFetchOnEmpty=False` to
reproduce recovery-only continuation.

Predicted-not-taken conditional branches do not truncate the prefix. Existing
fault, vector, predicted-taken, block-boundary and control restrictions remain.
Packet-local fusion boundaries can shorten the fast prefix. No cross-packet
fusion waiting is added.

## Single routing point

Decode decides the route at `fetchToDecodeDelay - 1`, after the original normal
input processing and squash handling. It uses the state before the early packet
is inserted. Fast and normal masks are disjoint and cover all instruction slots.
Invalid slots cannot enter fast, but remain in normal for the baseline's buffer
and Decode scan accounting. The early decision does not mark normal slots
squashed ahead of their original pipeline stage.

The fast prefix enters the existing output buffer. The normal mask is latched
in the same time-buffer packet, whose single instruction array remains owned by
the original forward pipeline. For a packet sent at T with delay 3, routing
occurs at T+2. The normal portion enters the original stall buffer at T+3,
through the original input wire and queue advancement rules. That wire consumes
only lanes assigned normal; it does not re-decide routing, construct a second
payload or consult a late dynamic-instruction dedup flag. Thus normal enqueue,
grouping, scan slots, squash and backpressure retain their original timing.

The four normal groups and 32 instruction slots in the default configuration
remain unchanged. Each normal input packet adds at most one group. There is no
early normal occupancy or new normal FIFO. The existing feedback reserve and
group-count backpressure are unchanged.
An empty output can still accept the first fast packet under backend blocking
when `fastFetchAllowBlockedDecode` is true; further fast packets are disabled.

## Recovery and observation

Each eligible recovery receives a monotonically increasing 64-bit epoch.
Packets and stop feedback are checked against that epoch. Decode invalidates
old opportunities on squash; only Fetch's later eligible backend recovery can
arm a new one, unless the optional empty-buffer checks above permit rearming.
Rejected old packets cannot disable a newer epoch.

Normal instructions retain the original squash processing and Decode scan
bandwidth, including instructions already marked invalid in flight. An invalid
slot is not compacted away at the early routing point. Fast survivors in the
output buffer retain their version handling. Drain and activity checks include
the normal buffer. Predecode still owns final frontend consistency
checking and redirects. The dynamic `fastFetchAccepted` marker remains available
for that checking and statistics, rather than normal-copy deduplication.

`FastFetch` traces contain packet masks, routing decisions, per-instruction
ownership, ready cycles and recovery epochs. Check them with:

```sh
python3 util/xs_scripts/check_fast_fetch_routing.py m5out/frontend.trace --bytes 64
```

Counters include `fastFetchCompleteBlocks`, `fastFetchNextBlocks`,
`fastFetchNormalInsts`, `fastFetchInvalidInsts`, and `fastFetchStopReasons`.
The invalid counter counts invalid slots retained for normal accounting;
it does not count an early discard. `normal-enqueue` trace events verify the
original input cycle, and `normal-ready` events verify output timing.
The checker requires exactly one on-time enqueue for every normal lane whose
ready cycle has been reached. A final routed packet may still be in flight when
the trace ends; its pending lanes are reported as `normal_inflight`. A later CPU
tick makes those enqueues mandatory, even if no later packet is routed.
The legacy normal-dedup counter stays zero in this mode. Performance comparisons
must keep fetch/decode widths, byte budgets, predecode delays, fusion settings
and workloads equal; compare legacy continuous, mask routing and fast disabled.
As a negative control, `fastFetchBytes=1` permits no RISC-V instruction into
fast. It must reproduce the original normal CPU counters, as must mask routing
with `enableFastFetch=False`. An unavailable first candidate does not split a
normal packet at an FTQ boundary.

`fastFetchEmptyRearms`, `fastFetchEmptyAcceptedBundles`, and
`fastFetchEmptyAcceptedInsts` separate empty-buffer opportunities from backend
recovery opportunities. `rearm-empty` trace events record the new epoch and its
inherited per-block budget; the routing checker verifies that this budget is not
reset by rearming.

Whole-CPU drain remains limited by the existing empty `Fetch::drainStall()` and
`Fetch::drainResume()` implementations. A mid-execution drain probe does not
finish in either legacy or mask-routing mode. Decode now accounts for its
unready normal groups, but this does not make the existing Fetch drain protocol
functional.
