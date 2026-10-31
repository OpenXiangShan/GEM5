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
new-path instructions. Self-including replays and nested redirects keep the
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
