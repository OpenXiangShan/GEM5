# Paired fetch groups and ICache Data SRAM constraints

The optional `enableTwoFetch` path consumes up to two consecutive FTQ targets
from independent virtual addresses. The ordinary KMHV3 preset keeps this option
disabled; ideal KMHV3 enables it for a single non-trace thread. The classic L1
ICache is resolved from the connected instruction port at startup, without
adding a SimObject construction dependency from the CPU to its cache.

## Block boundaries and bandwidth

The existing FTQ prediction window remains unchanged. A taken block's effective
end is its predicted control PC plus predicted instruction size; a fall-through
block ends at the prediction-window end. Invalid boundaries use the original
single-block path. The combined effective sizes must fit `twoFetchMaxBytes`
(default 64), and both starts must share a 4KiB virtual page. Address gaps do
not consume this budget; overlapping execution ranges are charged separately.

Ranges crossing a page fall back to the original path. In particular, this
version does not implement RTL's page-clipped fall-through predictor. Instruction
FTQ ownership still follows the instruction's starting PC, including a 32-bit
instruction straddling a sequential block boundary. These are explicit remaining
differences from RTL `kunminghu-v3` commit `1ac5c4d2f`.

## Requests, recovery, and bank conflicts

Each group captures two prediction snapshots, up to four initial line fragments,
and bounded instruction-tail supplements. Shared line reads may supply both
blocks without changing either block's size accounting. Responses and delayed
translations carry a generation identity. Squash, TLB flush, reset, and drain
discard the current group without reclaiming packets owned by the memory system.

ICache performs a side-effect-free way lookup after translation and before
timing sends. `icache_data_bank_bytes=8` selects eight Data banks per 64-byte
line; zero disables the pair constraint. A cross-block line pair conflicts if
its bank masks overlap, its way masks are equal, and its virtual sets differ.
Misses have zero way masks, matching the selected RTL predicate. A conflict
retains only the first block. Cache contents, replacement, and normal hit/miss
handling remain in the existing classic Cache implementation.

Pure Tag-port rejection is distinguished from Cache protocol retry. Tag waits
use a next-ICache-cycle event; blocked/MSHR waits use `recvReqRetry`. Accepted
fragments are never resent. When paired fetch is enabled, legacy single-block
fallbacks also get a Tag retry event: their first packet can be rejected before
any response exists to drive the old sibling-response retry path. Tail bytes
use normal translated timing reads;
skipped predicted control PCs recover without waiting for the old window end.
If a straddling instruction consumes a halfword containing the next block's
predicted control, the next block recovers before translating or reading bytes.

Inactive CPUs resolve the cache only after taking over connected ports. Cache
request-interval storage is initialized in `BaseCache::init`, after all CPUs
have registered their requestor IDs, including switched-out CPUs.

The global Tag-port budget and normal MSHRs still constrain request acceptance.
Consequently, a logical pair may take multiple cycles to issue. Meta banks,
WayLookup readiness/update timing, refill read/write arbitration, and complete
VIPT storage organization are not modeled here.

## Validation and counters

`twoFetchIssued` counts logical paired groups at first successful packet send.
`twoFetchSameCycleAccepted` additionally requires all initial fragments to be
accepted in one ICache cycle. `twoFetchSuccesses` counts actual boundary
consumption. None alone is equivalent to RTL's request-level `2fetch` counter.
Rejection causes, Tag waits, Cache retries, supplements, and cancellation have
separate counters.

Build and run the decision tests with:

```sh
scons build/RISCV/cpu/o3/two_fetch.test.opt --unit-test -j8
./build/RISCV/cpu/o3/two_fetch.test.opt
python3 microbench/two_fetch_test.py
# Focused reproductions:
python3 microbench/two_fetch_test.py --layouts four_lines
python3 microbench/two_fetch_test.py --layouts hot_disjoint hot_conflict
```

The microbenchmark runner explicitly enables the new path, fixes L1I at 4 ways,
checks the generated configuration, and compares disabled/pair/banked modes on
near, far/disjoint-bank, overlapping-bank, and four-line layouts. It checks the
guest checksum and aggregates counter intervals across stats resets. Additional
cases cover a single Tag port, a single MSHR, a 1GHz ICache with a 3GHz CPU,
frontend/TLB flush, drain/resume, and a short CPU takeover. Bare hot-loop cases
raise Tag latency to four cycles so the FTQ has pair candidates available;
the trace must show actual consumption of the two loop blocks in either order.
SE results validate the mechanism; they are not full-system performance calibration.

Validation against base commit `244990a233`:

| Check | Observed result |
| --- | --- |
| Decision/bank helper unit tests | 6 passed |
| SE mechanism and recovery cases | 16 passed |
| Bare hot-loop and CPU takeover cases | 5 passed |
| Disjoint banks, pair/bank modes | 2,001 consumed pairs in both modes |
| Overlapping banks, pair mode | 2,001 consumed pairs |
| Overlapping banks, bank mode | 0 consumed pairs; 3,999 bank rejections |
| Single-MSHR case | 291 Cache retries; guest exited successfully |
| Slow ICache with one Tag port | 68,067 Tag waits; guest exited successfully |
| Two-fetch disabled, 4 SE layouts | 5,776–5,779 shared simulated-stat keys per layout matched the base exactly |
| Two-fetch disabled, FS `2fetch` | 6,038 shared simulated-stat keys matched the base exactly |
| Two-fetch disabled, short SMT loop | 5,512 shared simulated-stat keys matched the base exactly |
| FS `2fetch`, Tag latency 4 | NEMU difftest passed; 10,002 consumed pairs |

The FS test uses the existing nexus-am `2fetch-riscv64-xs.bin` with the normal
KMHV3 configuration, default L2, and `--no-pf`. The default-latency FS run also
passes difftest, but its FTQ producer leaves no pair candidates available;
the higher-latency run is needed to exercise paired consumption.

Results and commands are under the ignored `microbench/output/two_fetch/`.
The long libc-based SMT and CPU-switch probes also exposed the existing Commit
watchdog checking inactive threads/CPUs; short bare tests avoid that unrelated
limitation. Large checkpoint/SPEC performance calibration remains future work.
