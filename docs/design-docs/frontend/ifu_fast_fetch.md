# Configurable IFU fast fetch

This document describes the legacy offer-and-dedup path. KMHV3 defaults to
[mask routing](ifu-fast-fetch-mask-routing.md), which assigns each instruction
to one path and optionally rearms fast fetch when IBuffer drains. To select the
legacy path for all comparisons below, explicitly set both
`enableFastFetchMaskRouting=False` and `enableFastFetchOnEmpty=False`.

With `enableFastFetch`, an eligible backend recovery starts a fast-fetch
opportunity. The first candidate uses the existing recovery path. With
`enableFastFetchContinuation`, its remaining instructions and subsequent FTQ
blocks can then use the same earlier wire while Decode's output and stall
buffers are empty and Fetch and Decode are unblocked. This extends the RTL's
single recovery-block fast attempt.

Each block keeps its own `fastFetchBytes` window and instruction restrictions.
Candidate collection continues past conditional branches predicted not taken.
It stops at unconditional control instructions, predicted-taken control
instructions, faults, vector instructions, or the window boundary.
Predicted-taken four-byte instructions remain excluded from the candidate.
A bundle contains a prefix of one block, limited to
`decodeWidth`; remaining candidates can be offered in following cycles. A later
block must already have a complete candidate prefix available in the fetch queue.

The normal channel continues in parallel. Decode accepts a continuation only
after the preceding fast bundle was accepted and while its buffers remain empty.
Older normal instructions therefore prevent a younger fast bundle from passing
them. Accepted instructions retain `fastFetchAccepted`, so their normal copies
are filtered once. Fusion-boundary trimming remains in effect.

A blocked or occupied buffer, an exhausted block prefix, or a rejected bundle
ends the opportunity until another eligible recovery. With
`fastFetchAllowBlockedDecode`, the first recovery bundle can still be buffered
while Decode is blocked. Squashes and thread resets clear
continuation state; a stale rejection cannot cancel a newer recovery version.
`fastFetchContinuedBundles` counts accepted bundles after the first bundle, and
the `FastFetch` trace labels offers and acceptances with `continuation`.

## Recovery modes

The two mode parameters default to false in `BaseO3CPU`. The continuous KMHV3
configuration enables both; `enableFastFetch` remains the master switch.
With mask routing and empty-buffer rearming disabled, all modes use the same
candidate restrictions, normal channel, and dedup logic. The two parameters in
the table alone do not select the legacy path.

| Mode | `fastFetchAllowBlockedDecode` | `enableFastFetchContinuation` |
| --- | --- | --- |
| Original single bundle | false | false |
| Buffer the first bundle while blocked | true | false |
| Continuous fast fetch | true | true |

Disabling continuation ends each fast attempt after its first publication,
including any unoffered remainder of the candidate block. It does not change
the normal fallback. Blocking Decode always stops continuation even when first
bundle buffering is enabled. Decode buffers must remain empty for fast
admission in every mode; this parameter does not merge normal and fast packets.

For a controlled legacy comparison, use these common overrides:

```sh
--param system.cpu[0].enableFastFetch=True \
--param system.cpu[0].enableFastFetchMaskRouting=False \
--param system.cpu[0].enableFastFetchOnEmpty=False \
--param system.cpu[0].fastFetchBytes=64
```

Then select the two table parameters with `--param` as appropriate.
`enableFastFetch` remains the master switch; set it to false for the normal-only
legacy baseline while keeping the other common overrides unchanged.
