# BOP Replay Oracle

`bop_replay.py` consumes the opt-in `--dump-bop-replay-trace` SQLite trace.
It labels pre-filter BOP candidates using later L2 demand reads and reports
Large, Small, and combined accuracy/coverage at multiple demand-count
horizons.  It intentionally does not model local filters, prefetch queues,
MSHRs, fills, evictions, or cache residency.

Schema V5 traces additionally contain the exact Large/Small learner metadata,
the BOP clock period used by delay-queue callbacks, and the native online
candidate result after the policy and local filter. They also contain
`BOPReplayPhase`, with `trace_start` and the gem5 `stable` phase created at
the warmup stats-reset callback; `L2DemandTrace` and `BOPReplayEvent` carry
the corresponding `PhaseId`. These fields are the golden reference for
offline learner replay and phase-aware reporting. V5 additionally records each
BOP learner's native delay-queue `enqueue`, `drop_full`, and `dequeue_to_rr`
actions in `BOPReplayDelayAction`. This captures the real event-queue order
between callbacks and triggers without modeling cache/filter behavior.

V3 traces remain compatible. For them, select the stable window explicitly
from the first gem5 stats block or its final tick:

```bash
python3 util/bop_replay/bop_replay.py trace.db \
  --mode learner-replay --candidate-stage raw --horizons 512 \
  --evaluation-start-stats /path/to/stats.txt \
  --evaluation-stats-block 1 --output stable.json
```

For V5 traces, prefer the native phase marker:

```bash
python3 util/bop_replay/bop_replay.py trace.db \
  --mode learner-replay --candidate-stage raw --horizons 512 \
  --evaluation-phase stable --output stable.json
```

`--evaluation-start-tick T` is the equivalent explicit boundary for either
schema. The learner and shared PC/global controller always replay the full
trace first; only candidates issued in the selected window are labeled, and
only selected-window L2 demands contribute to `useful`, `unused`, `accuracy`,
and `coverage`. This preserves warmup training state without counting
warmup-issued candidates in stable quality.

Run the unit tests:

```bash
python3 util/bop_replay/test_bop_replay.py
```

Run an emitted trace:

```bash
python3 util/bop_replay/bop_replay.py /nfs/home/lijiangtao/temp/bop-replay/workload/trace.db
```

The default `replay-controller` mode replays the shared PC-confidence table
and global bypass feedback using fixed BOP raw-candidate and RR-validation
signals from the trace. `recorded` evaluates the online policy candidates;
`raw` evaluates the same BOP candidates before PC/global suppression.

## Direct-Quality Gate Trace

When `--dump-bop-direct-quality-trace` and the direct-quality gate are both
enabled, ArchDB writes a separate causal feedback ledger. Schema V3 preserves
the established table names `BOPDirectQualityIssue`,
`BOPDirectQualityDemand`, and `BOPDirectQualityOutcome`; despite its
historical name, an `Issue` row means a **selected raw BOP candidate** at the
gate admission point. `BOPDirectQualityCandidate` records every raw gate
input and pre-update decision, while `Issue` records only a selected sample
whose feedback entry was actually inserted. They are recorded before address
translation, PFQ admission, packet coalescing, cache lookup, and memory
bandwidth effects.

`IssueDemandSequence` is therefore the L2-read-demand sequence at raw
candidate selection, not a physical packet-issue time. A candidate resolves
as `UsefulDemand` when a later L2 read demand reaches the same line within the
configured horizon. It resolves as `UnusedExpiry` only after that demand
horizon. `UnknownFeedbackReplacement` and `UnknownOwnerReplaced` are dropped
evidence and must not be counted as unused. This trace is intended to certify
the bounded gate against the offline raw-policy controller; it is deliberately
separate from physical `pfIssued`, `pfUseful`, `pfUnused`, and IPC metrics.

When `pc_validation_producer_consumer` is enabled in trace metadata or through
`--controller-config`, `replay-controller` reconstructs the RR producer owner
from the learner and native delay-action stream, then runs the policy stage.
This applies to both streaming and explicit materialized replay engines; an
event-only controller replay cannot recover the owner.

## Per-Issuer-PC Quality Attribution

`analyze_bop_pc_quality.py` compares raw BOP with a fixed producer/consumer
controller in one V5 streaming pass. A candidate's quality belongs to
`BOPReplayEvent.TriggerPC`, the PC that emits that candidate. The RR owner is
reported only as a producer/consumer training diagnostic and never replaces
the candidate issuer as the useful/unused owner.

```bash
python3 util/bop_replay/analyze_bop_pc_quality.py trace.db \
  --controller-config producer_consumer_k2.json \
  --evaluation-phase stable --top 20 \
  --verify-raw-report raw-baseline.json \
  --verify-current-report producer-consumer-current.json \
  --output pc-quality.json
```

The tool requires P/C mode. It reports every `issuer PC x BOP kind` row and a
hot-PC union selected by raw traffic, current traffic, raw useful loss, and raw
unused reduction. `combined` labels let all issuer rows sum to the complete
cross-BOP coverage. `isolated_kind` labels use separate Large/Small oracles so
their sums certify against standard per-BOP quality reports. It also reports
the raw-only/current-only/both-covered L2-demand sets, so a per-PC useful delta
is not misreported as a coverage-set delta.

To regenerate candidates offline from the learner state machine and check the
baseline implementation against online GEM5:

```bash
python3 util/bop_replay/bop_replay.py trace.db \
  --mode learner-replay --verify-online --horizons 128,512,2048 \
  --output learner-baseline.json
```

`--verify-online` is a hard prerequisite for parameter sweeps. It compares
`BestOffsetBefore/After`, score/round state, issue enable, and raw candidate
validity/address for every Large/Small trigger event. The command exits with
status `2` on a mismatch. Strict certification requires V5 because its native
delay-action stream resolves same-tick callback ordering. V3/V4 traces remain
usable for non-certifying exploratory learner replay and for
recorded/raw/controller quality modes.

After baseline verification passes, provide learner-only overrides as JSON:

```json
{
  "large": {"score_max": 24, "round_max": 40},
  "small": {"score_max": 16}
}
```

The regenerated raw candidates are evaluated against future L2 demand reads.
Online `OnlineGenerated`, `OnlineBuffered`, `OnlineFiltered`, and
`OnlineFilterPassed` are retained for comparison, but local filtering is not
modeled by the offline learner or its quality oracle.

Use `--candidate-stage raw` to evaluate the learner's native pre-filter
candidate stream. `--candidate-stage policy` additionally recomputes strict
or PC-confidence issue validation from the regenerated RR state. In policy
mode the global BOP bypass is updated from demand-oracle useful/unused labels;
this makes it an offline optimization model rather than an online-golden
comparison, because GEM5's online global feedback observes real cache
outcomes. `--verify-online` deliberately certifies the upstream learner/raw
candidate stage; it does not compare policy validation against online cache
outcomes.

## Batch SPEC06 Trace And Controller Sweep

`spec06_bop_policy_manifest.json` contains the selected workload slices for
coverage-sensitive BOP-controller tuning. Export all V5 traces with at most
eight independent GEM5 processes:

```bash
python3 util/bop_replay/run_spec06_bop_trace_batch.py --workers 8 --resume
```

Each case runs 20M warmup instructions and 20M stable instructions, writes its
own `trace.db`, then requires `--verify-online` to pass for the complete raw
Large/Small learner stream. Results live under
`~/temp/bop-replay/spec06-bop-policy-sweep-20260813/` by default. Use repeated
`--case <name>` to run or resume a subset.

`spec06_bop_controller_oat_v1.json` defines the first one-at-a-time controller
matrix. It sweeps only the shared PC-validation/global-controller state; it
does not change learner parameters or model local filtering/cache behavior.
After certified traces are available, run:

```bash
python3 util/bop_replay/run_spec06_bop_policy_sweep.py --workers 8 --resume
```

The sweep records per-case policy JSON files and creates
`policy-sweep-summary.json`. It reports raw-relative deltas and aggregate
accuracy/coverage Pareto fronts at each horizon. Controller overrides are
always applied to both Large and Small BOP metadata because online GEM5 shares
their PC-validation table. A standalone policy point can use the same API:

```bash
python3 util/bop_replay/bop_replay.py trace.db \
  --mode learner-replay --candidate-stage policy \
  --controller-config controller.json --evaluation-phase stable
```

For an existing V5 trace captured before producer/consumer metadata was added,
enable the policy with a shared-controller override:

```json
{
  "pc_validation_producer_consumer": true,
  "pc_validation_entries": 128,
  "pc_validation_offset_context_slots": 2
}
```

## No-Conflict Confidence Experiment

`sweep_bop_pc_confidence.py` evaluates directed P/C confidence-update profiles
without changing online GEM5. It replays the full V5 learner and P/C owner
stream once, then gives each profile independent controller/global-bypass
state. The shared counterfactual evidence retains every matured RR line but
only recovers a recorded validation miss when the line's latest native RR
maturity is within the configured age limit.

The checked-in `bop_pc_confidence_no_conflict_age2048_v1.json` fixes the
experiment at no-conflict age 2,048, quality Horizon 2,048, 128 PC entries,
K=2 offset contexts, and global bypass. It varies only `hit_increment`,
`miss_decay_period`, and the medium-to-low hysteresis. `raw accuracy >= 10%`
is an offline PC cohort label used for reporting; it is never passed into the
controller.

```bash
python3 util/bop_replay/sweep_bop_pc_confidence.py trace.db \
  --experiment util/bop_replay/bop_pc_confidence_no_conflict_age2048_v1.json \
  --evaluation-phase stable \
  --verify-raw-report raw-baseline.json \
  --verify-baseline-report producer-consumer-age.json \
  --output confidence-no-conflict.json
```

The result contains one combined/Large/Small `accuracy` and `coverage` report
per profile, full per-issuer-PC useful/unused data, coverage transitions, and
the raw-accuracy-at-least-10-percent cohort's candidate/useful/unused
retention. The `baseline` profile is required to reproduce the existing
`no_conflict_age_2048` counterfactual point before alternative profiles are
interpreted.

## Bounded Unique-Address LRU Evidence

`replay_bop_pc_counterfactual.py` also provides a hardware-bounded alternative
to the unbounded `no_conflict` idealization. `unique_lru` keeps one exact
unique-address LRU per BOP, populated only when the native delayed RR training
line matures. It does not replace the native RR: a recorded native validation
hit remains authoritative, and the LRU is queried only after a recorded miss.
Each entry retains its most recent valid producer PC; repeated mature lines
refresh LRU recency, while a no-PC demand cannot erase a previously valid
owner. The model reports recovered hits, duplicate refreshes, and capacity
evictions separately.

```bash
python3 util/bop_replay/replay_bop_pc_counterfactual.py trace.db \
  --controller-config producer_consumer_k2.json \
  --points current unique_lru --unique-lru-entries 2048 \
  --evaluation-phase stable --output unique-lru.json
```

Run every certified trace with separate processes and require the replayed
`current` point to match the previously certified P/C-K2 controller before a
unique-LRU result is aggregated:

```bash
python3 util/bop_replay/run_spec06_bop_unique_lru.py \
  --workers 31 --unique-lru-entries 2048 --resume
```

The batch writes `summary.json` and `summary.csv` under
`~/temp/bop-replay/pc-validation-unique-lru-20260817/`. It reports raw BOP,
native-RR P/C-K2, and bounded unique-LRU P/C-K2 quality at Horizon 2,048 in
the stable phase. The LRU changes controller evidence only; it does not model
local filters, fills, MSHRs, cache residency, bandwidth, or future-demand
quality as an online input.

## Offset-Consistent Recovered Evidence

`unique_lru` intentionally treats every exact historical predecessor address
as a normal validation hit. It is useful for measuring the coverage exposed
by RR replacement, but it is not a selective producer-quality signal when
BOP's best offset changes over time. The offset-consistent points retain the
same bounded 2,048-entry per-BOP LRU and preserve all recorded native RR
hits, but store the producer's `BestOffsetAfter` in every mature LRU entry.
A recovered miss is positive evidence only when the stored producer offset
equals the current validation offset. An exact-address offset mismatch uses
the unchanged PC/global controller miss path and gives no producer credit.

The three offline-only points are fixed algorithm variants, not a threshold
sweep:

- `unique_lru_offset_match_r1`: compatible recovered owner credit is `+1`.
- `unique_lru_offset_match_r1_gate`: additionally gates a compatible
  recovered same-PC issue with pre-update confidence.
- `unique_lru_offset_match_r1_gate_probation`: additionally requires two
  compatible recovered observations per existing PC-kind-offset context
  before a `+1` owner credit is committed.

Native RR hits keep their established behavior and strong credit. The
recovered compatibility fields exist only in the Python replay model; they do
not alter online GEM5 configuration or use future L2 demand labels as an
admission input.

```bash
python3 util/bop_replay/replay_bop_pc_counterfactual.py trace.db \
  --controller-config util/bop_replay/producer_consumer_k2.json \
  --points current unique_lru unique_lru_offset_match_r1 \
           unique_lru_offset_match_r1_gate \
           unique_lru_offset_match_r1_gate_probation \
  --unique-lru-entries 2048 --evaluation-phase stable \
  --output offset-consistent.json
```

## Recovered Evidence Factors

The causal attribution experiment separates immediate LRU-hit admission from
the later producer-confidence amplification. The following offline-only
points all use the same offset-unrestricted, 2,048-entry unique-address LRU;
they neither alter the online GEM5 controller nor use L2 demand outcomes as
controller input:

- `lru_full`: source-tagged full recovered behavior. Its quality must match
  the legacy `unique_lru` point exactly.
- `direct_only`: a recovered hit may retain its direct admission effect, but
  writes no recovered producer credit.
- `credit_only`: a recovered hit may write producer credit, but cannot bypass
  the current consumer's normal confidence/global-bypass admission.
- `cross_pc_credit_off` and `same_pc_credit_only`: retain recovered admission
  and same-PC credit, while suppressing cross-PC recovered producer credit.
- `cross_pc_credit_only`: retain recovered admission and cross-PC credit,
  while suppressing same-PC recovered producer credit.

The controller report records the four factor booleans for every point. Native
RR hits remain unchanged. When recovered admission is disabled, invalid-owner
and no-trigger-PC evidence follows the normal miss/global-bypass path rather
than issuing unconditionally.

```bash
python3 util/bop_replay/replay_bop_pc_counterfactual.py trace.db \
  --controller-config util/bop_replay/producer_consumer_k2.json \
  --points current unique_lru lru_full direct_only credit_only \
           cross_pc_credit_off same_pc_credit_only cross_pc_credit_only \
  --unique-lru-entries 2048 --evaluation-phase stable \
  --output recovered-evidence-factors.json
```

## Native-vs-LRU Causal Attribution

`analyze_bop_pc_lru_rootcause.py` replays the certified native-RR P/C and bounded unique-address LRU P/C controllers in lockstep. It classifies each stable-phase LRU-only issuance by the first divergence: direct recovered evidence, global-bypass amplification, confidence-state divergence, or residual controller divergence. Candidate labels come from the existing Horizon-2,048 L2-demand oracle only after issue.

```bash
python3 util/bop_replay/analyze_bop_pc_lru_rootcause.py trace.db \
  --controller-config util/bop_replay/producer_consumer_k2.json \
  --unique-lru-entries 2048 --evaluation-phase stable \
  --output rootcause.json
```

The report requires native learner/owner reconstruction and candidate-count closure. It keeps only bounded pending candidate state, and does not model local filters, fills, MSHRs, cache residency, bandwidth, or DRAM behavior.

## Same-PC Admission Experiment

`bop_pc_same_pc_gate_no_conflict_age2048_v1.json` keeps the certified
`medium_hysteresis` P/C configuration fixed and isolates the same-PC RR-hit
path. The gated profiles make the issue decision from the pre-update
PC-kind-offset confidence state, but always retain the positive producer
update after the decision. `same_pc_hit_gated_reward1` changes only that
same-PC positive update from the normal `+4` to `+1`; cross-PC credit and
RR-miss decay are unchanged. This is offline-only: it adds no GEM5 state or
online behavior.

```bash
python3 util/bop_replay/sweep_bop_pc_confidence.py trace.db \
  --experiment util/bop_replay/bop_pc_same_pc_gate_no_conflict_age2048_v1.json \
  --evaluation-phase stable \
  --verify-raw-report raw-baseline.json \
  --verify-baseline-report confidence-no-conflict.json \
  --verify-baseline-point medium_hysteresis \
  --output same-pc-gate.json
```

## PC-Quality Oracle Thresholds

`analyze_bop_pc_oracle_threshold.py` measures the offline upper bound obtained
by closing raw BOP traffic from issuer PCs whose **stable-phase raw combined
accuracy** is below a selected threshold. It keeps no-PC candidates, uses the
same 2,048-demand quality oracle as the controller reports, and compares each
threshold against raw BOP and a fixed P/C-K2/global-bypass replay. The label is
non-causal and is never an online controller input.

```bash
python3 util/bop_replay/run_spec06_bop_pc_oracle_threshold.py \
  --thresholds 5,10,15,20 --workers 8
```

Each report is evaluated only in phase `stable` after replaying the full trace
for controller state. The batch writes per-case `oracle-threshold.json` plus
aggregate `summary.json` and `summary.csv` under
`~/temp/bop-replay/pc-quality-oracle-threshold-20260817/`.
`--workers` is the number of independent worker processes. This is intentional:
the learner/controller replay and bounded demand-oracle accounting are Python
CPU work, so a thread pool would remain constrained by the interpreter GIL.
