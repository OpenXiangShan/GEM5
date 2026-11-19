# RTL DefaultConfig data-prefetch algorithm profile

Use `--rtl-default-data-prefetchers` with `kmhv3.py` or `idealkmhv3.py` to
select the data-prefetch generator set enabled in the local XiangShan RTL
`DefaultConfig`. This switch affects prefetch configuration, not cache
capacity, associativity, replacement, or the CPU's general `--kmh-align`
settings. The historical prefetch defaults remain selected without it.

The source reference is XiangShan commit
`e7dccb7a4d9b829c719361174141c421fec33374` and XSCache commit
`d2c3a99341b44b486b39758dc8fd8982feaf87b4`. The audited working XSCache has
local changes; the source audit and file hashes are recorded at
`~/.tmp/rtl-default-prefetch-audit-20261120/source-provenance.json`.

## Generator set and destinations

| Generator | gem5 switch | Request destinations | Source label |
| --- | --- | --- | --- |
| Independent Stream | `enable_xsstream` | L1D, L2 | `SStream` |
| Independent Stride | `enable_sstride` | L1D, L2 | `SStride` |
| SMS AGT | `enable_activepage`, `rtl_sms_agt` | L2 | `SMS_AGT` |
| SMS PHT | `enable_pht` | L2 | `SPht` |
| Virtual large BOP | L2 `enable_bop`, `bop_large` | L2 | `HWP_BOP` |
| Physical small BOP | L2 `enable_bop`, `bop_small` | L2 | `HWP_BOP` |

The profile disables Berti, CMC/temporal, CDP, LLDP, SPP, CPLX, OPT and
DespacitoStream, including `idealkmhv3.py`'s otherwise forced LLDP defaults.
Stream's L3 destination is disabled. L1D/L2 cache pressure offloading is
also disabled: an occupied MSHR/blocked cache must not silently move these
candidates into L3's prefetch queue. Ordinary misses still access lower
memory levels. Stride uses its existing L1/L2 RTL
lookahead mode and single table. The BOP components use the existing
`XSVirtualLargeBOP`/`XSPhysicalSmallBOP` parameter presets, with adaptive
offset-list modification disabled. VBOP explicitly uses VA; embedded PBOP
trains its recent-request/offset learner on PA. PBOP candidates that leave
the physical page are dropped; same-page PA candidates are mapped back to
the trigger VA page before entering the parent's VA translation queue.
The profile requires L1-to-L2 hint
forwarding and rejects incompatible top-level prefetcher types.

The new switch has final precedence over optional legacy component-enable
flags during configuration. Explicit later `--param` overrides still work,
so experimental scripts must verify the resolved `config.ini`.

## SMS AGT coexistence

The legacy `activepage` implementation was an alternative stream generator:
it excluded independent XSStream and sent a full region to L1, optionally
also ahead to L2/L3. The RTL has both a StreamStride module and an SMS module.
The new `rtl_sms_agt` mode therefore permits coexistence, leaves the legacy
path unchanged, and sends AGT requests only to L2.

AGT uses its active-region count, a threshold of 12 cache lines, and a
30-cache-line lookahead. It marks from the target offset through the region
boundary in the forward direction, or from the region start through the
target offset in the backward direction. Wrapped addresses are discarded.
AGT takes precedence over PHT generation for a training event; independent
Stream/Stride do not disappear when AGT is enabled. AGT and PHT share the
SMS filter, with per-bit source/trigger-PC retention.

`SMS_AGT` is appended to the enum, preserving all historical source IDs.
The independent stream and AGT can now use separate observer and active
SDBP banks. BOP remains a combined statistical source; both BOP generators
are separately instantiated. Old 16-entry admission-control vectors remain
accepted; the new AGT source inherits the global action unless overridden.
The split SDBP layout becomes 35 banks/slice (105 KiB of logical counters),
compared with the previous 33 banks (99 KiB). Even a run without the profile
uses the expanded enum layout; record this when comparing storage budgets.

## Scope and validation

This is alignment of the enabled algorithms and request destinations.
It does not establish equivalence of training streams, finite-table state,
AGT/PHT update algorithms, dynamic monitors, arbitration, queue occupancy,
TLB timing, or RTL cycle timing. In particular the reused gem5 SMS/Stream
models retain their existing training and filtering abstractions. FDIP and
translation prefetch are outside this switch's scope.

Checks include AGT forward/backward region masks and address bounds;
configuration precedence, valid component types and source-ID compatibility;
SDBP isolation between same-PC AGT and stream requests; resolved full-system
configuration and checkpoint smoke runs with disabled-source traffic checks.
