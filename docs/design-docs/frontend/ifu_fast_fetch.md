# Continuous IFU fast fetch

With `enableFastFetch`, an eligible backend recovery starts a fast-fetch
opportunity. The first candidate uses the existing recovery path. Its remaining
instructions and subsequent FTQ blocks can then use the same earlier wire while
Decode's output and stall buffers are empty and Fetch and Decode are unblocked.
This extends the RTL's single recovery-block fast attempt.

Each block keeps its own `fastFetchBytes` window and instruction restrictions.
Candidate collection stops at control instructions, faults, vector instructions,
or the window boundary. A bundle contains a prefix of one block, limited to
`decodeWidth`; remaining candidates can be offered in following cycles. A later
block must already have a complete candidate prefix available in the fetch queue.

The normal channel continues in parallel. Decode accepts a continuation only
after the preceding fast bundle was accepted and while its buffers remain empty.
Older normal instructions therefore prevent a younger fast bundle from passing
them. Accepted instructions retain `fastFetchAccepted`, so their normal copies
are filtered once. Fusion-boundary trimming remains in effect.

A blocked or occupied buffer, an exhausted block prefix, or a rejected bundle
ends the opportunity until another eligible recovery. The first recovery bundle
can still be buffered while Decode is blocked. Squashes and thread resets clear
continuation state; a stale rejection cannot cancel a newer recovery version.
`fastFetchContinuedBundles` counts accepted bundles after the first bundle, and
the `FastFetch` trace labels offers and acceptances with `continuation`.
