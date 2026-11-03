# Dispatch-time instruction profiling (DIP)

DIP complements TEA's commit-side cycle stacks with dispatch-side stacks. This
implementation follows the per-slot attribution policy in Figure 3 of
*Chips Need DIP: Time-Proportional Per-Instruction Cycle Stacks at Dispatch*
(ASPLOS 2026, DOI 10.1145/3779212.3790139).

Enable it with the single-thread XiangShan configuration:

```sh
build/RISCV/gem5.opt configs/example/kmhv3.py --enable-dip \
    --generic-rv-cpt=<checkpoint.zstd>
```

The default is off. `--dip-sample-period=N` samples every N active CPU ticks;
zero and one enumerate every tick. TEA and DIP can run together. DIP also
preserves fetch cache-line miss metadata when TEA is disabled.

The ingress boundary is in-order IEW admission: directly to the scheduler/LSQ
when `enableDispatchStage=False`, or into the dispatch queues when it is true.
Out-of-order transfers from dispatch queues to issue queues are not counted as
new ingress. The number of slots is `renameWidth`. The current implementation
supports one thread; enabling it for SMT is rejected because shared dispatch
slots need a separate attribution policy.

Each accepted uop tentatively receives one Base slot. Unused slots blocked by
an instruction belong to the oldest blocked instruction, with the actual
admission gate recorded as the cause. Upstream ROB and queue reservations and
physical-register exhaustion retain the blocked rename instruction's identity.
Empty frontend slots wait for the next admitted uop, whose supplying instruction
cache/TLB events distinguish a miss from a bubble.

These targets remain tentative until retirement establishes the correct path.
An accepted squash transfers younger targets and waiting frontend slots to its
cause as Misspeculation. The last redirect remains active during pipeline
refill. Instruction squash versions distinguish late old-path admission from
new-path instructions. New-path input blocked by an observed upstream rename
gate ends refill as well: its empty slots are Stall. Recovery state advances
every tick, including ticks between sampling boundaries.
Self-including replays and nested redirects keep the
cause tentative until in-order retirement passes its sequence number.

A fused uop consumes one slot and is represented by its first architectural
member's PC; splitting a slot between fused members would change the paper's
uop attribution policy. A simulator-side redirect without an identifiable
instruction has an empty PC and remains explicitly unresolved.

`system.cpu.dip.csv` contains:

| Field | Meaning |
| --- | --- |
| `thread` | Thread ID, currently zero |
| `pc` | Static target PC, empty when no target is known |
| `state` | Base, Stall, FrontEnd, or Misspeculation |
| `cause` | Multi-hot cause mask below |
| `slot_weight` | Integer number of sampled slots |
| `cycle_weight` | `slot_weight / renameWidth` |
| `resolution` | resolved, unresolved, or pending |
| `flush_reason` | Numeric `SquashCause` of the redirect owning Misspeculation slots |

Pending rows are provisional: their target may still be squashed, or the next
frontend target has not arrived. They must not be treated as final per-PC
attribution. A stats reset drops the old window's weights and targets while
retaining live redirect state. Dumping stats does not resolve or consume them.
The CSV is a snapshot of the current stats window, as with TEA.

`system.cpu.dip.sampledSlots` equals samples times dispatch width. At every dump,
`attributedSlots + unresolvedSlots + pendingSlots` equals `sampledSlots`.
`slotsByState` and `slotsByCause` describe finalized slots; cause bits can overlap.
Use pending totals explicitly when constructing a complete cycle stack.

At period one, the sum of CSV cycle weights equals active CPU ticks in the
measurement window. At larger periods, weights are in sampled-cycle units;
multiply by the period for an estimate, retaining sampling phase/error.
DIP measures uop ingress and TEA measures architectural egress. Their Base and
Computing categories are different, particularly with fusion and ROB compression.
The two cycle stacks must be compared separately and must not be added together.

This is simulator telemetry with dynamic target maps. It does not model sampling
interrupts or claim the paper's BOOM hardware area, storage, or runtime overhead.

| Bit | Cause | Meaning |
| --- | --- | --- |
| 0 | S_ROB | ROB allocation is blocked |
| 1 | S_ISB | Issue/dispatch queue capacity or admission bandwidth is blocked |
| 2 | S_LDQ | Load queue capacity/reservation is blocked |
| 3 | S_STQ | Store queue capacity/reservation is blocked |
| 4 | S_REN | Physical register rename is blocked |
| 5 | S_SER | A serializing instruction cannot enter |
| 6 | F_BUB | Frontend bubble without an instruction cache/TLB miss event |
| 7 | F_CTM | The next uop carries an instruction cache/TLB miss event |
| 8 | M_SWE | Software-handled instruction exception |
| 9 | M_FL | Branch, memory-order, value-prediction or other hardware flush |
| 10 | M_UNKNOWN | Simulator/interrupt redirect without an instruction cause |

Stall masks describe the gates that the simulator actually evaluates. Upstream
ROB/LSQ/register causes may overlap; at direct admission, earlier failing gates
can mask later predicates. They are not an exhaustive test of all hypothetical
resource constraints, and cause counts must not be summed as disjoint cycles.


## Admission context

CSV rows additionally retain `iq`, `iq_flags`, and `frontend_reason`.
Summing over these columns recovers the original PC/state/cause/resolution
budget. Pending weights retain their context; squashed weights follow the
original Misspeculation transfer and discard the failed-path gate identity.

`flush_reason` further partitions Misspeculation using the existing
`SquashCause` enum: 0 = None/unspecified, 1 = BranchMispredict,
2 = MemOrderViolation, 3 = ValuePrediction, 4 = Trap, 5 = ThreadContext,
6 = SquashAfter, and 7 = LongLatencyFlush. Other states use zero.
It describes the redirect source, including a Trap whose instruction fault
may request re-execution. It does not change the original cause mask.
Pending slots retain the reason; an older redirect transfers younger slots
to its own reason. Summing over this column recovers the previous CSV budget.

For a surviving S_ISB stall, `iq` is the selected scheduler IQ ID. Flags
are a bit set: 1 = enqueue ports exhausted, 2 = IQ entries exhausted,
4 = replay queue above its admission limit, 8 = thread quota, and
16 = another compatible IQ is ready for this same checked opclass.
The flags are observations, not additive cycle costs. For split stores,
the store-data group is checked before the address group, matching admission.
The legacy search policy has no selected IQ (`iq=-1`) and reports the union
of gates across the searched group; it cannot have a ready peer on failure.
These queries do not allocate, reroute, or alter scheduling state.

`frontend_reason` is the numeric `StallReason` propagated through Rename
for the sampled empty lane, captured before the pipeline time buffers advance.
It is preserved until the next admitted instruction supplies the existing
F_BUB/F_CTM classification. Zero means no propagated explanation. This is
pipeline context, not a causal claim: bundle buffering can separate the lane
from the originating Fetch event, and F_CTM remains a next-uop event tag.


`decodeStrandedSlots` is auxiliary stage context: on sampled cycles with
Decode unblocked, its fixed input buffer drained, and fewer than decodeWidth
instructions decoded, it counts up to the unused lanes still backed by raw
instructions in the existing stall FIFO. It includes tentative-path entries
and does not prove those instructions would decode or retire. It is separate
from the dispatch budget and must not be added to any DIP state or cause.


`readyBranchWaits` counts selectable, non-cancelled control entries remaining
after all ports in an IQ have selected. Shared ready lists are visited once;
new entries behind an enabled enqueue boundary are excluded. Counts are per
sampled cycle and include tentative paths. This is auxiliary stage context,
not part of the dispatch budget. A selected instruction may still lose later
register-port arbitration, which this counter does not count.
