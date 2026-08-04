#!/usr/bin/env python3
"""Compare bounded P/C confidence-update profiles on one V5 BOP trace.

The replay keeps the online Large/Small learner stream intact and changes
only the shared PC-validation controller.  It uses no-conflict RR evidence
with an optional native-maturity age cap, so a recorded online RR hit remains
a hit while a recovered line must be sufficiently recent.  Each profile gets
independent P/C confidence and global-bypass state, but all profiles share one
SQLite scan, one learner reconstruction, and one counterfactual evidence map.

Candidate quality remains filter-free and is labeled only by subsequent L2
demand reads.  The raw per-PC accuracy threshold in the output is an offline
analysis cohort, never an input to the controller.
"""

from __future__ import annotations

import argparse
import json
import math
import sqlite3
import time
from collections import Counter
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable, Mapping

import analyze_bop_pc_quality as pc_quality
import bop_replay as replay
from replay_bop_pc_counterfactual import (
    _PersistentRREvidence,
    _RecordedActionCursor,
    _configs_by_name,
    _no_conflict_validation,
)


HORIZON = 2048
UPDATE_FIELDS = frozenset({
    "pc_validation_hit_increment",
    "pc_validation_miss_decay_period",
    "pc_validation_low_entry_miss_streak_threshold",
})
POLICY_FIELDS = frozenset({
    "pc_validation_same_pc_hit_gate",
    "pc_validation_same_pc_hit_increment",
})


@dataclass(frozen=True)
class Profile:
    name: str
    overrides: Mapping[str, object]
    policy_overrides: Mapping[str, object]
    config: replay.BOPConfig


@dataclass(frozen=True)
class Experiment:
    name: str
    no_conflict_max_insert_age: int
    raw_pc_accuracy_threshold: float
    profiles: tuple[Profile, ...]


def _require_mapping(value: object, field: str) -> Mapping[str, object]:
    if not isinstance(value, Mapping):
        raise ValueError(f"{field} must be a JSON object")
    return value


def load_experiment_payload(
    payload: Mapping[str, object], trace_config: replay.BOPConfig,
) -> Experiment:
    """Apply the fixed base profile and validate update-only alternatives."""
    if int(payload.get("horizon", HORIZON)) != HORIZON:
        raise ValueError(f"confidence experiment must use Horizon {HORIZON}")
    age = int(payload.get("no_conflict_max_insert_age", -1))
    if age < 0:
        raise ValueError("no_conflict_max_insert_age must be non-negative")
    threshold = float(payload.get("raw_pc_accuracy_threshold", -1.0))
    if not 0.0 <= threshold <= 1.0:
        raise ValueError("raw_pc_accuracy_threshold must be in [0, 1]")

    base_overrides = _require_mapping(
        payload.get("base_controller_overrides", {}),
        "base_controller_overrides",
    )
    base_config = replay._apply_controller_overrides(base_overrides, trace_config)
    base_policy = sorted(set(base_overrides) & POLICY_FIELDS)
    if base_policy:
        raise ValueError(
            "same-PC policy fields must be profile-level overrides: "
            + ", ".join(base_policy)
        )
    shared = replay._shared_controller_config(base_config)
    if not shared.pc_validation_producer_consumer:
        raise ValueError("confidence experiment requires producer/consumer mode")

    profile_payload = payload.get("profiles")
    if not isinstance(profile_payload, list) or not profile_payload:
        raise ValueError("profiles must be a non-empty JSON array")
    profiles: list[Profile] = []
    names: set[str] = set()
    for item in profile_payload:
        record = _require_mapping(item, "profile")
        name = str(record.get("name", ""))
        if not name:
            raise ValueError("profile name must be non-empty")
        if name in names:
            raise ValueError(f"duplicate profile name: {name}")
        names.add(name)
        overrides = _require_mapping(record.get("overrides", {}), "profile overrides")
        unexpected = sorted(set(overrides) - UPDATE_FIELDS)
        if unexpected:
            raise ValueError(
                f"{name}: only confidence-update fields may vary: "
                + ", ".join(unexpected)
            )
        if set(overrides) != UPDATE_FIELDS:
            missing = sorted(UPDATE_FIELDS - set(overrides))
            raise ValueError(
                f"{name}: missing explicit confidence-update field(s): "
                + ", ".join(missing)
            )
        policy_overrides = _require_mapping(
            record.get("policy_overrides", {}), "profile policy_overrides",
        )
        unexpected_policy = sorted(set(policy_overrides) - POLICY_FIELDS)
        if unexpected_policy:
            raise ValueError(
                f"{name}: only same-PC policy fields may vary: "
                + ", ".join(unexpected_policy)
            )
        if policy_overrides and set(policy_overrides) != POLICY_FIELDS:
            missing_policy = sorted(POLICY_FIELDS - set(policy_overrides))
            raise ValueError(
                f"{name}: missing explicit same-PC policy field(s): "
                + ", ".join(missing_policy)
            )
        profile_config = replay._apply_controller_overrides(
            {**overrides, **policy_overrides}, base_config,
        )
        profile_shared = replay._shared_controller_config(profile_config)
        if not profile_shared.pc_validation_producer_consumer:
            raise ValueError(f"{name}: producer/consumer mode must remain enabled")
        profiles.append(Profile(
            name, dict(overrides), dict(policy_overrides), profile_config,
        ))

    return Experiment(
        name=str(payload.get("name", "p_c_confidence_experiment")),
        no_conflict_max_insert_age=age,
        raw_pc_accuracy_threshold=threshold,
        profiles=tuple(profiles),
    )


def load_experiment(path: Path, trace_config: replay.BOPConfig) -> Experiment:
    payload = _require_mapping(json.loads(path.read_text()), "experiment")
    return load_experiment_payload(payload, trace_config)


def _is_validation_check(event: replay.ReplayEvent) -> bool:
    return (
        event.raw_candidate_valid
        and event.validation_enabled
        and event.validation_hit in (0, 1)
    )


class _ProfileState:
    """Independent controller and selected-window attribution for one point."""

    def __init__(self, profile: Profile, window: replay.EvaluationWindow):
        self.profile = profile
        shared = replay._shared_controller_config(profile.config)
        self.controller = pc_quality._TracingPCValidationController(shared)
        self.outcome_oracle = replay.DemandOracle(
            HORIZON, self.controller.note_outcome, keep_candidates=False,
        )
        self.current = pc_quality._PCQualityTracker(HORIZON)
        self.decisions = pc_quality._DecisionTracker()

    def observe_demand(self, demand: replay.Demand, selected: bool) -> None:
        self.outcome_oracle.observe_demand(demand)
        if selected:
            self.current.observe_demand(demand)

    def emit(
        self, event: replay.ReplayEvent, output: replay.LearnerOutput,
        validation_hit: int, validation_owner_pc: int,
        validation_owner_valid: bool, selected: bool,
    ) -> None:
        learner_config = self.profile.config.for_kind(output.kind)
        issued = self.controller.policy_candidate_values(
            bop_kind=output.kind,
            best_offset=output.best_offset_after,
            best_offset_changed=output.best_offset_changed,
            raw_candidate_valid=output.raw_candidate_valid,
            pc_confidence_enabled=learner_config.pc_validation_confidence,
            validation_enabled=output.validation_enabled,
            validation_hit=validation_hit,
            trigger_addr=event.trigger_addr,
            trigger_pc=event.trigger_pc,
            trigger_has_pc=event.trigger_has_pc,
            validation_owner_pc=validation_owner_pc,
            validation_owner_valid=validation_owner_valid,
        )
        key = pc_quality._IssuerKey(
            event.trigger_has_pc, event.trigger_pc, output.kind,
        )
        if selected and output.raw_candidate_valid:
            self.decisions.observe(key, self.controller.last_decision)
        if not issued:
            return
        self.outcome_oracle.emit(
            output.kind, event.access_seq, event.tick,
            output.raw_candidate_addr, event.phase_id,
        )
        self.controller.note_issued()
        if selected:
            self.current.emit(
                key, event.access_seq, event.tick,
                output.raw_candidate_addr, event.phase_id,
            )

    def commit(self) -> None:
        self.controller.commit()

    def finish(self) -> None:
        self.outcome_oracle.finish()
        self.current.finish()


def _counts_from_report(report: Mapping[str, object]) -> pc_quality._Counts:
    return pc_quality._Counts(
        sent=int(report["candidates"]),
        useful=int(report["useful"]),
        unused=int(report["unused"]),
        redundant=int(report["redundant"]),
        censored=int(report["censored"]),
    )


def _quality_with_coverage(
    quality: Mapping[str, Mapping[str, object]],
) -> dict[str, dict[str, object]]:
    """Expose the standard aggregate ``coverage`` name beside PC attribution."""
    return {
        kind: {
            **metrics,
            "coverage": metrics["coverage_contribution"],
        }
        for kind, metrics in quality.items()
    }


def _cohort_report(
    rows: Iterable[Mapping[str, object]], eligible_demands: int,
    raw_accuracy_threshold: float,
) -> dict[str, object]:
    raw_total = pc_quality._Counts()
    current_total = pc_quality._Counts()
    selected_rows = []
    for row in rows:
        if not bool(row["issuer_has_pc"]):
            continue
        raw = _require_mapping(row["raw"], "raw PC report")
        raw_accuracy = raw["accuracy"]
        if raw_accuracy is None or float(raw_accuracy) < raw_accuracy_threshold:
            continue
        current = _require_mapping(row["current"], "current PC report")
        raw_total.add(_counts_from_report(raw))
        current_total.add(_counts_from_report(current))
        selected_rows.append(row)

    raw_report = raw_total.report(eligible_demands)
    current_report = current_total.report(eligible_demands)
    return {
        "qualification": (
            "issuer_has_pc and raw combined accuracy >= "
            f"{raw_accuracy_threshold:.6f}; raw stream is independent of profile"
        ),
        "raw_accuracy_threshold": raw_accuracy_threshold,
        "qualified_pc_count": len(selected_rows),
        "raw": raw_report,
        "current": current_report,
        "candidate_retention": (
            current_total.sent / raw_total.sent if raw_total.sent else None
        ),
        "useful_retention": (
            current_total.useful / raw_total.useful if raw_total.useful else None
        ),
        "unused_retention": (
            current_total.unused / raw_total.unused if raw_total.unused else None
        ),
        "qualified_pcs": sorted(
            selected_rows,
            key=lambda row: (
                -int(_require_mapping(row["raw"], "raw PC report")["useful"]),
                str(row["issuer_trigger_pc"]),
            ),
        ),
    }


def _quality_certification(
    actual: Mapping[str, Mapping[str, object]], expected_path: Path,
    point: str | None,
) -> dict[str, object]:
    payload = json.loads(expected_path.read_text())
    if point is not None:
        if "points" in payload:
            point_payload = _require_mapping(
                _require_mapping(payload.get("points"), "expected points").get(point),
                f"expected point {point}",
            )
            expected = point_payload["quality"]
        else:
            point_payload = _require_mapping(
                _require_mapping(payload.get("profiles"), "expected profiles").get(point),
                f"expected profile {point}",
            )
            expected = point_payload["aggregate_quality"]
    elif "horizons" in payload:
        expected = _require_mapping(payload["horizons"], "expected horizons")[str(HORIZON)]
    else:
        expected = payload
    expected = _require_mapping(expected, "expected quality")
    mismatches = []
    for kind in ("combined", "large", "small"):
        actual_kind = _require_mapping(actual[kind], f"actual {kind}")
        expected_kind = _require_mapping(expected[kind], f"expected {kind}")
        for field in pc_quality.METRIC_FIELDS:
            if actual_kind[field] != expected_kind[field]:
                mismatches.append({
                    "kind": kind,
                    "field": field,
                    "actual": actual_kind[field],
                    "expected": expected_kind[field],
                })
        if not math.isclose(
            float(actual_kind["accuracy"] or 0.0),
            float(expected_kind["accuracy"] or 0.0),
            rel_tol=0.0,
            abs_tol=1e-15,
        ):
            mismatches.append({
                "kind": kind,
                "field": "accuracy",
                "actual": actual_kind["accuracy"],
                "expected": expected_kind["accuracy"],
            })
    return {
        "path": str(expected_path),
        "point": point,
        "pass": not mismatches,
        "mismatches": mismatches,
    }


def replay_confidence_experiment(
    connection: sqlite3.Connection, base_config: replay.BOPConfig,
    experiment: Experiment, window: replay.EvaluationWindow,
) -> dict[str, object]:
    """Run every profile in one full trace pass with shared RR evidence."""
    learner_configs = _configs_by_name(base_config)
    action_cursor = _RecordedActionCursor(connection, learner_configs)
    evidence = {
        name: _PersistentRREvidence(learner_config)
        for name, learner_config in learner_configs.items()
    }
    learners = {
        kind: replay.BOPLearner(
            base_config.for_kind(kind), use_recorded_delay_actions=True,
        )
        for kind in ("large", "small")
    }
    states = {
        profile.name: _ProfileState(profile, window)
        for profile in experiment.profiles
    }
    raw = pc_quality._PCQualityTracker(HORIZON)
    counters: Counter[str] = Counter()
    validation: Counter[str] = Counter()
    verifier = replay._OnlineVerifier()
    validation_mismatches = 0

    def on_demand(demand: replay.Demand) -> None:
        counters["demands"] += 1
        selected = replay._in_evaluation_window(demand, window)
        if selected:
            counters["window_demands"] += 1
            raw.observe_demand(demand)
        for state in states.values():
            state.observe_demand(demand, selected)

    def on_event(event: replay.ReplayEvent) -> None:
        nonlocal validation_mismatches
        counters["events"] += 1
        selected = replay._in_evaluation_window(event, window)
        if selected:
            counters["window_events"] += 1

        learner_config = learner_configs[event.bop_name]
        learner = learners[event.bop_kind]
        before, trigger_action = action_cursor.actions_for_event(event)
        for action in before:
            evidence[event.bop_name].apply_dequeue(action)
            learner.apply_delay_action(action)
        evidence[event.bop_name].apply_trigger(event, trigger_action)
        output = learner.process(event, trigger_action)
        verifier.observe(event, output)
        if output.validation_hit != event.validation_hit:
            validation_mismatches += 1

        validation_hit = output.validation_hit
        owner_pc = output.validation_owner_pc
        owner_valid = output.validation_owner_valid
        if _is_validation_check(event):
            validation["checks"] += 1
            if event.validation_hit:
                validation["recorded_hits"] += 1
            else:
                validation["recorded_misses"] += 1
            address = (
                event.trigger_addr
                - event.best_offset_after * learner_config.block_size
            ) & replay.UINT64_MASK
            recovered_hit, source, owner_pc, owner_valid = _no_conflict_validation(
                event, evidence[event.bop_name], address,
                experiment.no_conflict_max_insert_age,
                output.validation_owner_pc, output.validation_owner_valid,
            )
            validation_hit = int(recovered_hit)
            if recovered_hit:
                validation["counterfactual_hits"] += 1
            else:
                validation["counterfactual_misses"] += 1
                if source == "stale_matured":
                    validation["stale_age_misses"] += 1
            if not event.validation_hit and recovered_hit:
                validation["recovered_hits"] += 1
                if source == "matured_recorded":
                    validation["recovered_conflict_hits"] += 1
                elif source == "delay_drop":
                    validation["recovered_delay_drop_hits"] += 1
                else:
                    validation["recovered_other_hits"] += 1
        if selected and output.raw_candidate_valid:
            raw.emit(
                pc_quality._IssuerKey(
                    event.trigger_has_pc, event.trigger_pc, output.kind,
                ),
                event.access_seq, event.tick, output.raw_candidate_addr,
                event.phase_id,
            )
        for state in states.values():
            state.emit(
                event, output, validation_hit, owner_pc, owner_valid, selected,
            )

    def on_access_end() -> None:
        for state in states.values():
            state.commit()

    replay._stream_trace_rows(connection, on_demand, on_event, on_access_end)
    for state in states.values():
        state.finish()
    raw.finish()

    verification = verifier.report()
    verification["validation_hit_mismatches"] = validation_mismatches
    verification["pass"] = (
        verification["pass"] and validation_mismatches == 0
    )
    if not verification["pass"]:
        raise ValueError(f"native learner replay diverged: {verification}")

    raw_aggregate = _quality_with_coverage(raw.aggregate_report())
    raw_invariants = pc_quality._invariant_report(raw)
    profiles = {}
    qualified_sets: dict[str, tuple[str | None, ...]] = {}
    for profile in experiment.profiles:
        state = states[profile.name]
        rows = pc_quality._pc_rows(raw, state.current, state.decisions)
        cohort = _cohort_report(
            rows, raw.views["combined"].eligible_demands,
            experiment.raw_pc_accuracy_threshold,
        )
        qualified_sets[profile.name] = tuple(
            row["issuer_trigger_pc"] for row in cohort["qualified_pcs"]
        )
        profiles[profile.name] = {
            "overrides": dict(profile.overrides),
            "policy_overrides": dict(profile.policy_overrides),
            "controller_parameters": replay._controller_parameter_report(profile.config),
            "aggregate_quality": _quality_with_coverage(
                state.current.aggregate_report()
            ),
            "per_pc_invariants": pc_quality._invariant_report(state.current),
            "controller_stats": state.controller.stats(),
            "raw_accuracy_target_cohort": cohort,
            "all_pc": rows,
            "coverage_transition": pc_quality._coverage_transition(raw, state.current),
        }
    if len(set(qualified_sets.values())) != 1:
        raise AssertionError("raw accuracy target cohort changed across profiles")

    checks = validation["checks"]
    validation_report = {
        name: int(validation[name])
        for name in (
            "checks",
            "recorded_hits",
            "recorded_misses",
            "counterfactual_hits",
            "counterfactual_misses",
            "recovered_hits",
            "recovered_conflict_hits",
            "recovered_delay_drop_hits",
            "recovered_other_hits",
            "stale_age_misses",
        )
    }
    validation_report["recorded_hit_rate"] = (
        validation_report["recorded_hits"] / checks if checks else 0.0
    )
    validation_report["counterfactual_hit_rate"] = (
        validation_report["counterfactual_hits"] / checks if checks else 0.0
    )
    return {
        "model": {
            "horizon": HORIZON,
            "raw_candidate_stream": "recorded_online",
            "best_offset_stream": "recorded_online",
            "controller": "independent_shared_pc_global_replay_per_profile",
            "no_conflict": "native delay actions; matured RR lines persist",
            "no_conflict_max_insert_age": experiment.no_conflict_max_insert_age,
            "candidate_owner": "BOPReplayEvent.TriggerPC",
            "raw_accuracy_target": "offline cohort only; never controller input",
            "state_replay": "full trace",
            "not_modeled": "local filters, cache fills, MSHRs, residency, bandwidth",
        },
        "experiment": {
            "name": experiment.name,
            "raw_pc_accuracy_threshold": experiment.raw_pc_accuracy_threshold,
            "profile_count": len(experiment.profiles),
        },
        "evaluation_window": {
            **asdict(window),
            "demands": int(counters["window_demands"]),
            "events": int(counters["window_events"]),
        },
        "demands": int(counters["demands"]),
        "events": int(counters["events"]),
        "owner_reconstruction": verification,
        "validation": validation_report,
        "evidence_state": {
            name: item.stats() for name, item in evidence.items()
        },
        "raw": {
            "aggregate_quality": raw_aggregate,
            "per_pc_invariants": raw_invariants,
        },
        "profiles": profiles,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("database", type=Path, help="certified V5 BOP trace DB")
    parser.add_argument(
        "--experiment", type=Path,
        default=Path(__file__).with_name(
            "bop_pc_confidence_no_conflict_age2048_v1.json"
        ),
        help="JSON experiment definition",
    )
    parser.add_argument(
        "--evaluation-phase", default="stable",
        help="V5 phase used for reporting; full state is always replayed",
    )
    parser.add_argument(
        "--verify-raw-report", type=Path, default=None,
        help="raw baseline report that must certify this replay's raw stream",
    )
    parser.add_argument(
        "--verify-baseline-report", type=Path, default=None,
        help="counterfactual or confidence report that certifies one profile",
    )
    parser.add_argument(
        "--verify-baseline-point", default=None,
        help=(
            "profile/report point to certify; defaults to profile baseline and "
            "counterfactual no_conflict_age_<age>"
        ),
    )
    parser.add_argument("--output", type=Path, default=None)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    start = time.perf_counter()
    with sqlite3.connect(args.database) as connection:
        trace_config, phases = replay._streaming_metadata(connection)
        experiment = load_experiment(args.experiment, trace_config)
        base_config = experiment.profiles[0].config
        window = replay.resolve_evaluation_window(
            phase_name=args.evaluation_phase,
            start_tick=None,
            stats_path=None,
            stats_block=None,
            phases=phases,
        )
        report = replay_confidence_experiment(
            connection, base_config, experiment, window,
        )
    certifications = {}
    if args.verify_raw_report is not None:
        certifications["raw"] = _quality_certification(
            report["raw"]["aggregate_quality"], args.verify_raw_report, None,
        )
    if args.verify_baseline_report is not None:
        baseline_profile = args.verify_baseline_point or "baseline"
        expected_point = (
            args.verify_baseline_point
            or f"no_conflict_age_{experiment.no_conflict_max_insert_age}"
        )
        try:
            baseline_quality = report["profiles"][baseline_profile][
                "aggregate_quality"
            ]
        except KeyError as error:
            raise ValueError(
                f"missing profile selected for baseline certification: "
                f"{baseline_profile}"
            ) from error
        certifications["baseline"] = _quality_certification(
            baseline_quality,
            args.verify_baseline_report,
            expected_point,
        )
    if any(not item["pass"] for item in certifications.values()):
        raise ValueError(f"aggregate certification failed: {certifications}")
    report["aggregate_certification"] = certifications
    report["database"] = str(args.database)
    report["schema_version"] = base_config.schema_version
    report["replay_engine"] = {
        "engine": "streaming_multi_profile_pc_confidence_counterfactual",
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
