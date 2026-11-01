# Fast-fetch mask routing

`enableFastFetchMaskRouting` selects single-consumption routing instead of the
legacy early fast offer followed by a later normal copy. It defaults to false
in `BaseO3CPU` and is enabled with fast fetch in the KMHV3 experiment. Disabling
`enableFastFetch` keeps the original normal delivery path.

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

Predicted-not-taken conditional branches do not truncate the prefix. Existing
fault, vector, predicted-taken, block-boundary and control restrictions remain.
Packet-local fusion boundaries can shorten the fast prefix. No cross-packet
fusion waiting is added.

## Single routing point

Decode reads the packet at `fetchToDecodeDelay - 1`. After squash processing and
advancing older ready work, it decides the actual fast prefix using the state
before this packet is inserted. Fast and normal masks are disjoint and cover
the valid instructions. Invalid instructions are discarded separately.

The fast prefix enters the existing output buffer. The normal suffix enters
the existing stall buffer with the original absolute normal delivery cycle.
For a packet sent at T with delay 3, routing occurs at T+2, but the normal
portion cannot reach Decode before T+3. Both attempts to advance a normal group
check its ready cycle. The older normal wire is retained for legacy operation
and stall metadata, but does not insert another copy in mask-routing mode.

The four normal groups and 32 instruction slots in the default configuration
remain unchanged. Unready groups occupy capacity. Each input packet adds at
most one group. Fetch observes the shared `blockFetch` signal after Decode
ticks; at most `early_delay - 1` previously sent packets can remain after the
current input. Reserving at least `early_delay` groups, together with the
existing feedback reserve, bounds occupancy even when every offer is rejected.
An empty output can still accept the first fast packet under backend blocking
when `fastFetchAllowBlockedDecode` is true; further fast packets are disabled.

## Recovery and observation

Each eligible recovery receives a monotonically increasing 64-bit epoch.
Packets and stop feedback are checked against that epoch. Decode invalidates
old opportunities on squash; only Fetch's later eligible backend recovery can
arm a new one. Rejected old packets cannot disable a newer epoch.

Squash filtering preserves older valid instructions and their normal ready
cycles, updates survivor versions, and rebuilds group counts. Drain and activity
checks include unready groups. Predecode still owns final frontend consistency
checking and redirects. The dynamic `fastFetchAccepted` marker remains available
for that checking and statistics, rather than normal-copy deduplication.

`FastFetch` traces contain packet masks, routing decisions, per-instruction
ownership, ready cycles and recovery epochs. Check them with:

```sh
python3 util/xs_scripts/check_fast_fetch_routing.py m5out/frontend.trace --bytes 64
```

Counters include `fastFetchCompleteBlocks`, `fastFetchNextBlocks`,
`fastFetchNormalInsts`, `fastFetchInvalidInsts`, and `fastFetchStopReasons`.
The legacy normal-dedup counter stays zero in this mode. Performance comparisons
must keep fetch/decode widths, byte budgets, predecode delays, fusion settings
and workloads equal; compare legacy continuous, mask routing and fast disabled.

Whole-CPU drain remains limited by the existing empty `Fetch::drainStall()` and
`Fetch::drainResume()` implementations. A mid-execution drain probe does not
finish in either legacy or mask-routing mode. Decode now accounts for its
unready normal groups, but this does not make the existing Fetch drain protocol
functional.
