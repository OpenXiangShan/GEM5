#!/usr/bin/env python3
"""Attribute why bounded LRU RR recovery changes BOP P/C quality.

This offline-only diagnostic replays the certified native-RR P/C controller
and the existing 2K unique-address-LRU P/C counterfactual in lockstep. Raw
BOP candidates, best offsets, native delay timing, and native RR ownership
are fixed V5 trace inputs. The only policy input that changes is whether a
recorded native RR miss is recovered by the bounded mature-address LRU.

The L2 demand stream is used only after issuance to label candidates with the
existing Horizon-2048 useful/unused oracle. It is never an input to learner
or controller updates. Local filters, cache fills, residency, MSHRs,
bandwidth, and DRAM behavior are deliberately outside this model.
"""

from __future__ import annotations

import argparse
import json
import sqlite3
import time
from collections import Counter
from dataclasses import asdict, dataclass
from pathlib import Path

import bop_replay as replay
from replay_bop_pc_counterfactual import (
    HORIZON,
    _RecordedActionCursor,
    _UniqueAddressLRUEvidence,
    _configs_by_name,
    _unique_lru_validation,
)


DEFAULT_CONTROLLER_CONFIG = Path(__file__).with_name("producer_consumer_k2.json")
OUTCOMES = ("useful", "unused", "redundant", "censored")
CAUSES = (
    "direct_recovered_evidence",
    "global_bypass_amplification",
    "confidence_state_divergence",
    "residual_controller_divergence",
)


@dataclass
class _TrackedPair:
    key: tuple[str, int]
    cause: str | None
    kind: str
    issuer_pc: int
    owner_pc: int | None
    owner_relation: str
    issuer_state: str
    owner_state: str
    decision_reason: str
    prior_recovered_relation: str
    native_expected: bool
    lru_expected: bool
    native_status: str | None = None
    lru_status: str | None = None


def _empty_outcomes() -> Counter[str]:
    return Counter({status: 0 for status in OUTCOMES})


def _decision_cause(
    native: replay.ControllerDecision, lru: replay.ControllerDecision,
) -> str:
    """Classify the first observable reason an LRU-only issue exists.

    Recovered evidence has priority only when it changed this event's
    validation input. Later events with equal validation input are instead
    attributed to global-bypass or PC-confidence state divergence, avoiding
    the error of calling every later issue a direct LRU admission.
    """
    if lru.validation_source == "recovered" and not native.validation_hit:
        return "direct_recovered_evidence"
    if lru.global_bypass_at_admission and not native.global_bypass_at_admission:
        return "global_bypass_amplification"
    if (
        native.issuer_confidence != lru.issuer_confidence
        or native.issuer_state != lru.issuer_state
        or native.owner_confidence != lru.owner_confidence
        or native.owner_state != lru.owner_state
    ):
        return "confidence_state_divergence"
    return "residual_controller_divergence"


class _CausalAccumulator:
    """Bounded pending-candidate tracker plus aggregate attribution rows.

    Candidate identity is retained only until the Horizon-2048 demand oracle
    resolves it. The completed report keeps aggregate cause/context and hot
    PC counters, rather than an event-level trace.
    """

    def __init__(self, top_pcs: int):
        self.top_pcs = top_pcs
        self.pending: dict[tuple[str, int], _TrackedPair] = {}
        self.candidate_keys: dict[str, dict[int, tuple[str, int]]] = {
            "native": {},
            "unique_lru": {},
        }
        self.issue_matrix: Counter[str] = Counter()
        self.issue_matrix_by_kind: Counter[str] = Counter()
        self.lru_only: dict[
            tuple[str, str, str, str, str, str, str], Counter[str]
        ] = {}
        self.native_only: Counter[str] = _empty_outcomes()
        self.common_outcomes: Counter[str] = Counter()
        self.issuer: dict[int, Counter[str]] = {}
        self.owner: dict[int, Counter[str]] = {}
        self.max_pending = 0

    def register(
        self, *, event: replay.ReplayEvent, native_candidate: replay.Candidate | None,
        lru_candidate: replay.Candidate | None, native: replay.ControllerDecision,
        lru: replay.ControllerDecision, validation_owner_pc: int,
    ) -> None:
        native_expected = native_candidate is not None
        lru_expected = lru_candidate is not None
        if native_expected and lru_expected:
            matrix = "both_issued"
            cause = None
        elif native_expected:
            matrix = "native_only"
            cause = None
        elif lru_expected:
            matrix = "unique_lru_only"
            cause = _decision_cause(native, lru)
        else:
            self.issue_matrix["both_suppressed"] += 1
            self.issue_matrix_by_kind[f"{event.bop_kind}:both_suppressed"] += 1
            return

        self.issue_matrix[matrix] += 1
        self.issue_matrix_by_kind[f"{event.bop_kind}:{matrix}"] += 1
        owner_pc = (
            validation_owner_pc
            if lru.owner_relation in ("same_pc", "cross_pc") else None
        )
        prior_recovered_relation = "none"
        if cause == "confidence_state_divergence":
            if (
                lru.issuer_recovered_cross_pc_credit_seen
                and not native.issuer_recovered_cross_pc_credit_seen
            ):
                prior_recovered_relation = "cross_pc"
            elif (
                lru.issuer_recovered_same_pc_credit_seen
                and not native.issuer_recovered_same_pc_credit_seen
            ):
                prior_recovered_relation = "same_pc"
            elif (
                lru.owner_recovered_cross_pc_credit_seen
                and not native.owner_recovered_cross_pc_credit_seen
            ):
                prior_recovered_relation = "owner_cross_pc"
            elif (
                lru.owner_recovered_same_pc_credit_seen
                and not native.owner_recovered_same_pc_credit_seen
            ):
                prior_recovered_relation = "owner_same_pc"
            else:
                prior_recovered_relation = "no_context_recovered_marker"
        record = _TrackedPair(
            key=(event.bop_name, event.replay_order),
            cause=cause,
            kind=event.bop_kind,
            issuer_pc=event.trigger_pc,
            owner_pc=owner_pc,
            owner_relation=lru.owner_relation,
            issuer_state=lru.issuer_state or "no_pc",
            owner_state=lru.owner_state or "none",
            decision_reason=lru.reason,
            prior_recovered_relation=prior_recovered_relation,
            native_expected=native_expected,
            lru_expected=lru_expected,
        )
        if record.key in self.pending:
            raise ValueError(f"duplicate stable candidate key: {record.key}")
        self.pending[record.key] = record
        if native_candidate is not None:
            self.candidate_keys["native"][native_candidate.candidate_id] = record.key
        if lru_candidate is not None:
            self.candidate_keys["unique_lru"][lru_candidate.candidate_id] = record.key
        self.max_pending = max(self.max_pending, len(self.pending))

    def resolve(self, profile: str, candidate: replay.Candidate) -> None:
        key = self.candidate_keys[profile].pop(candidate.candidate_id, None)
        if key is None:
            return
        record = self.pending.get(key)
        if record is None:
            raise ValueError(f"missing pending candidate for {profile}: {key}")
        if candidate.status not in OUTCOMES:
            raise ValueError(f"unexpected candidate status: {candidate.status}")
        if profile == "native":
            record.native_status = candidate.status
        else:
            record.lru_status = candidate.status
        if ((not record.native_expected or record.native_status is not None)
                and (not record.lru_expected or record.lru_status is not None)):
            self._complete(record)
            self.pending.pop(key)

    def _bucket(self, record: _TrackedPair) -> Counter[str]:
        assert record.cause is not None
        key = (
            record.cause, record.kind, record.owner_relation,
            record.issuer_state, record.owner_state,
            record.prior_recovered_relation, record.decision_reason,
        )
        return self.lru_only.setdefault(key, _empty_outcomes())

    @staticmethod
    def _add(counter: Counter[str], status: str) -> None:
        counter["issued"] += 1
        counter[status] += 1

    def _complete(self, record: _TrackedPair) -> None:
        if record.native_expected and record.lru_expected:
            assert record.native_status is not None and record.lru_status is not None
            self.common_outcomes[
                f"{record.native_status}->{record.lru_status}"
            ] += 1
            return
        if record.native_expected:
            assert record.native_status is not None
            self._add(self.native_only, record.native_status)
            return
        assert record.lru_status is not None
        bucket = self._bucket(record)
        self._add(bucket, record.lru_status)
        issuer = self.issuer.setdefault(record.issuer_pc, _empty_outcomes())
        self._add(issuer, record.lru_status)
        if record.owner_pc is not None:
            owner = self.owner.setdefault(record.owner_pc, _empty_outcomes())
            self._add(owner, record.lru_status)

    @staticmethod
    def _rows(
        buckets: dict[tuple[str, str, str, str, str, str, str], Counter[str]],
    ) -> list[dict[str, object]]:
        rows = []
        for (
            cause, kind, relation, issuer_state, owner_state,
            prior_recovered_relation, decision_reason,
        ), counts in buckets.items():
            issued = counts["issued"]
            denominator = issued - counts["censored"]
            rows.append({
                "cause": cause,
                "bop_kind": kind,
                "owner_relation": relation,
                "issuer_pre_state": issuer_state,
                "owner_pre_state": owner_state,
                "prior_recovered_relation": prior_recovered_relation,
                "lru_decision_reason": decision_reason,
                "issued": issued,
                **{status: counts[status] for status in OUTCOMES},
                "accuracy": counts["useful"] / denominator if denominator else None,
            })
        return sorted(rows, key=lambda item: (-int(item["issued"]), str(item["cause"])))

    def _hot_rows(self, buckets: dict[int, Counter[str]], label: str) -> list[dict[str, object]]:
        rows = []
        for pc, counts in buckets.items():
            issued = counts["issued"]
            denominator = issued - counts["censored"]
            rows.append({
                label: f"0x{pc:x}",
                "issued": issued,
                **{status: counts[status] for status in OUTCOMES},
                "accuracy": counts["useful"] / denominator if denominator else None,
            })
        return sorted(rows, key=lambda item: -int(item["issued"]))[:self.top_pcs]

    def report(self) -> dict[str, object]:
        if self.pending or any(self.candidate_keys.values()):
            raise ValueError("candidate attribution did not drain after oracle finish")
        cause_totals: dict[str, Counter[str]] = {
            cause: _empty_outcomes() for cause in CAUSES
        }
        for (cause, _, _, _, _, _, _), counts in self.lru_only.items():
            target = cause_totals[cause]
            for key, value in counts.items():
                target[key] += value
        return {
            "issue_matrix": {key: int(value) for key, value in self.issue_matrix.items()},
            "issue_matrix_by_kind": {
                key: int(value) for key, value in self.issue_matrix_by_kind.items()
            },
            "unique_lru_only_by_cause": {
                cause: {
                    "issued": int(counts["issued"]),
                    **{status: int(counts[status]) for status in OUTCOMES},
                }
                for cause, counts in cause_totals.items()
            },
            "unique_lru_only_breakdown": self._rows(self.lru_only),
            "native_only_outcomes": {
                "issued": int(self.native_only["issued"]),
                **{status: int(self.native_only[status]) for status in OUTCOMES},
            },
            "common_candidate_outcome_transitions": {
                key: int(value) for key, value in self.common_outcomes.items()
            },
            "top_unique_lru_only_issuer_pcs": self._hot_rows(
                self.issuer, "issuer_pc"
            ),
            "top_unique_lru_only_owner_pcs": self._hot_rows(
                self.owner, "owner_pc"
            ),
            "max_pending_candidates": self.max_pending,
        }


def _validation_check(event: replay.ReplayEvent) -> bool:
    return (
        event.raw_candidate_valid
        and event.validation_enabled
        and event.validation_hit in (0, 1)
    )


def _validation_address(event: replay.ReplayEvent, config: replay.BOPConfig) -> int:
    return (
        event.trigger_addr - event.best_offset_after * config.block_size
    ) & replay.UINT64_MASK


def replay_lru_rootcause(
    connection: sqlite3.Connection, config: replay.BOPConfig,
    window: replay.EvaluationWindow, unique_lru_entries: int, top_pcs: int,
) -> dict[str, object]:
    if not config.pc_validation_producer_consumer:
        raise ValueError("root-cause attribution requires producer/consumer mode")
    if unique_lru_entries <= 0:
        raise ValueError("unique LRU entries must be positive")
    if top_pcs <= 0:
        raise ValueError("top PC count must be positive")

    replay._shared_controller_config(config)
    configs_by_name = _configs_by_name(config)
    action_cursor = _RecordedActionCursor(connection, configs_by_name)
    lru_evidence = {
        name: _UniqueAddressLRUEvidence(learner, unique_lru_entries)
        for name, learner in configs_by_name.items()
    }
    native_learners = {
        kind: replay.BOPLearner(
            config.for_kind(kind), use_recorded_delay_actions=True,
        )
        for kind in ("large", "small")
    }
    verifier = replay._OnlineVerifier()
    owner_validation_mismatches = 0
    tracker = _CausalAccumulator(top_pcs)
    native_policy = replay._StreamingPolicyReplay(config, HORIZON, window)
    lru_policy = replay._StreamingPolicyReplay(config, HORIZON, window)
    # Controller feedback remains full-trace inside each policy.  These two
    # additional bounded oracles deliberately reproduce the selected-window
    # quality contract, so a warmup candidate cannot consume a stable demand
    # label in the attribution report.
    native_labels = replay.DemandOracle(
        HORIZON, keep_candidates=False,
        on_resolve=lambda candidate: tracker.resolve("native", candidate),
    )
    lru_labels = replay.DemandOracle(
        HORIZON, keep_candidates=False,
        on_resolve=lambda candidate: tracker.resolve("unique_lru", candidate),
    )
    counters: Counter[str] = Counter()
    validation: Counter[str] = Counter()

    def on_demand(demand: replay.Demand) -> None:
        counters["demands"] += 1
        if replay._in_evaluation_window(demand, window):
            counters["window_demands"] += 1
            native_labels.observe_demand(demand)
            lru_labels.observe_demand(demand)
        native_policy.observe_demand(demand)
        lru_policy.observe_demand(demand)

    def emit(
        policy: replay._StreamingPolicyReplay, event: replay.ReplayEvent,
        validation_hit: int, owner_pc: int, owner_valid: bool,
        validation_source: str, selected: bool,
    ) -> tuple[replay.Candidate | None, replay.ControllerDecision]:
        candidate = policy.emit_values(
            bop_kind=event.bop_kind,
            best_offset=event.best_offset_after,
            best_offset_changed=event.best_offset_changed,
            raw_candidate_valid=event.raw_candidate_valid,
            raw_candidate_addr=event.raw_candidate_addr,
            pc_confidence_enabled=event.pc_confidence_enabled,
            validation_enabled=event.validation_enabled,
            validation_hit=validation_hit,
            trigger_addr=event.trigger_addr,
            trigger_pc=event.trigger_pc,
            trigger_has_pc=event.trigger_has_pc,
            access_seq=event.access_seq,
            tick=event.tick,
            phase_id=event.phase_id,
            selected=selected,
            validation_owner_pc=owner_pc,
            validation_owner_valid=owner_valid,
            validation_source=validation_source,
        )
        decision = policy.controller.last_decision
        if decision is None:
            raise ValueError("controller did not record an admission decision")
        return candidate, decision

    def on_event(event: replay.ReplayEvent) -> None:
        nonlocal owner_validation_mismatches
        counters["events"] += 1
        selected = replay._in_evaluation_window(event, window)
        if selected:
            counters["window_events"] += 1
        learner_config = configs_by_name[event.bop_name]
        before, trigger_action = action_cursor.actions_for_event(event)
        for action in before:
            lru_evidence[event.bop_name].apply_dequeue(action)
            native_learners[event.bop_kind].apply_delay_action(action)
        lru_evidence[event.bop_name].apply_trigger(event, trigger_action)
        native_output = native_learners[event.bop_kind].process(event, trigger_action)
        verifier.observe(event, native_output)
        if native_output.validation_hit != event.validation_hit:
            owner_validation_mismatches += 1

        native_owner_pc = native_output.validation_owner_pc
        native_owner_valid = native_output.validation_owner_valid
        lru_hit = event.validation_hit
        lru_source = "none"
        lru_owner_pc = native_owner_pc
        lru_owner_valid = native_owner_valid
        lru_validation_source = "native"
        if _validation_check(event):
            validation["checks"] += 1
            validation["native_hits"] += int(event.validation_hit == 1)
            lru_hit_bool, lru_source, lru_owner_pc, lru_owner_valid = (
                _unique_lru_validation(
                    event, lru_evidence[event.bop_name],
                    _validation_address(event, learner_config),
                    native_owner_pc, native_owner_valid,
                )
            )
            lru_hit = int(lru_hit_bool)
            if not event.validation_hit and lru_hit_bool:
                validation["recovered_hits"] += 1
                validation[f"recovered_source:{lru_source}"] += 1
                lru_validation_source = "recovered"
            validation["unique_lru_hits"] += int(lru_hit_bool)

        native_candidate, native_decision = emit(
            native_policy, event, event.validation_hit, native_owner_pc,
            native_owner_valid, "native", selected,
        )
        lru_candidate, lru_decision = emit(
            lru_policy, event, lru_hit, lru_owner_pc, lru_owner_valid,
            lru_validation_source, selected,
        )
        if selected:
            native_label = None
            lru_label = None
            if native_candidate is not None:
                native_label = native_labels.emit(
                    event.bop_kind, event.access_seq, event.tick,
                    event.raw_candidate_addr, event.phase_id,
                )
            if lru_candidate is not None:
                lru_label = lru_labels.emit(
                    event.bop_kind, event.access_seq, event.tick,
                    event.raw_candidate_addr, event.phase_id,
                )
            tracker.register(
                event=event, native_candidate=native_label,
                lru_candidate=lru_label, native=native_decision,
                lru=lru_decision, validation_owner_pc=lru_owner_pc,
            )

    def on_access_end() -> None:
        native_policy.commit()
        lru_policy.commit()

    replay._stream_trace_rows(connection, on_demand, on_event, on_access_end)
    owner_reconstruction = verifier.report()
    owner_reconstruction["validation_hit_mismatches"] = owner_validation_mismatches
    owner_reconstruction["pass"] = (
        owner_reconstruction["pass"] and owner_validation_mismatches == 0
    )
    if not owner_reconstruction["pass"]:
        raise ValueError(
            "native P/C owner reconstruction diverged from V5 learner trace: "
            f"{owner_reconstruction}"
        )

    native_quality = native_policy.finish()["combined"]
    lru_quality = lru_policy.finish()["combined"]
    native_labels.finish()
    lru_labels.finish()
    attribution = tracker.report()
    matrix = attribution["issue_matrix"]
    lru_only_sum = sum(
        row["issued"] for row in attribution["unique_lru_only_breakdown"]
    )
    closure = {
        "native_candidates": {
            "matrix": matrix.get("both_issued", 0) + matrix.get("native_only", 0),
            "quality": native_quality.candidates,
        },
        "unique_lru_candidates": {
            "matrix": matrix.get("both_issued", 0) + matrix.get("unique_lru_only", 0),
            "quality": lru_quality.candidates,
        },
        "unique_lru_only": {
            "matrix": matrix.get("unique_lru_only", 0),
            "breakdown": lru_only_sum,
        },
    }
    closure["pass"] = (
        closure["native_candidates"]["matrix"]
        == closure["native_candidates"]["quality"]
        and closure["unique_lru_candidates"]["matrix"]
        == closure["unique_lru_candidates"]["quality"]
        and closure["unique_lru_only"]["matrix"]
        == closure["unique_lru_only"]["breakdown"]
    )
    return {
        "model": {
            "horizon": HORIZON,
            "raw_candidate_stream": "recorded_online",
            "controller_profiles": "native_rr_pc_vs_2k_unique_lru_pc",
            "unique_lru_entries": unique_lru_entries,
            "classification_order": list(CAUSES),
            "not_modeled": "local filters, cache fills, MSHRs, residency, bandwidth",
        },
        "controller_parameters": replay._controller_parameter_report(config),
        "evaluation_window": {
            **asdict(window),
            "state_replay": "full_trace",
            "demand_oracle": "selected_window_only",
            "demands": int(counters["window_demands"]),
            "events": int(counters["window_events"]),
        },
        "owner_reconstruction": owner_reconstruction,
        "validation": {key: int(value) for key, value in validation.items()},
        "quality": {
            "native_rr_pc": asdict(native_quality),
            "unique_lru_pc": asdict(lru_quality),
        },
        "attribution": attribution,
        "closure": closure,
        "evidence_state": {
            name: evidence.stats() for name, evidence in lru_evidence.items()
        },
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("database", type=Path, help="certified V5 BOP trace DB")
    parser.add_argument(
        "--controller-config", type=Path, default=DEFAULT_CONTROLLER_CONFIG,
        help="shared P/C controller override; defaults to fixed 128-entry K=2",
    )
    parser.add_argument("--unique-lru-entries", type=int, default=2048)
    parser.add_argument("--evaluation-phase", default="stable")
    parser.add_argument("--top-pcs", type=int, default=32)
    parser.add_argument("--output", type=Path, default=None)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    start = time.perf_counter()
    with sqlite3.connect(args.database) as connection:
        config, phases = replay._streaming_metadata(connection)
        config = replay._load_controller_overrides(args.controller_config, config)
        window = replay.resolve_evaluation_window(
            phase_name=args.evaluation_phase, start_tick=None, stats_path=None,
            stats_block=None, phases=phases,
        )
        report = replay_lru_rootcause(
            connection, config, window, args.unique_lru_entries, args.top_pcs,
        )
    report["database"] = str(args.database)
    report["schema_version"] = config.schema_version
    report["replay_engine"] = {
        "engine": "streaming_pc_lru_rootcause",
        "wall_seconds": time.perf_counter() - start,
    }
    encoded = json.dumps(report, indent=2, sort_keys=True)
    if args.output is None:
        print(encoded)
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
