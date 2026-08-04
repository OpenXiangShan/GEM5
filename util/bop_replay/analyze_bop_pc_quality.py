#!/usr/bin/env python3
"""Attribute raw and P/C-current BOP quality to candidate issuer PCs.

The primary candidate owner is ``BOPReplayEvent.TriggerPC``: it is the PC
which emitted the current BOP candidate.  In producer/consumer mode the RR
owner is a training diagnostic only; credit for an old RR line must not move
the useful/unused label of the current candidate away from its issuer.

The analyzer replays the complete V5 trace to preserve learner and shared
controller state, but labels only candidates and L2 demand reads in the
selected evaluation window.  It intentionally remains filter-free: local
filters, fills, cache residency, MSHRs, and bandwidth are not modeled.
"""

from __future__ import annotations

import argparse
import json
import math
import sqlite3
import time
from collections import Counter, defaultdict
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable, Mapping

import bop_replay as replay


HORIZON = 2048
STATUSES = ("useful", "unused", "redundant", "censored")
METRIC_FIELDS = (
    "candidates",
    "useful",
    "unused",
    "redundant",
    "censored",
    "eligible_demands",
    "covered_demands",
)


@dataclass(frozen=True)
class _IssuerKey:
    """The issuer of a candidate, distinct from a P/C RR owner."""

    has_pc: bool
    pc: int
    bop_kind: str


@dataclass
class _Counts:
    sent: int = 0
    useful: int = 0
    unused: int = 0
    redundant: int = 0
    censored: int = 0

    def resolve(self, status: str) -> None:
        if status not in STATUSES:
            raise ValueError(f"unexpected candidate status {status!r}")
        setattr(self, status, getattr(self, status) + 1)

    def add(self, other: _Counts) -> None:
        self.sent += other.sent
        self.useful += other.useful
        self.unused += other.unused
        self.redundant += other.redundant
        self.censored += other.censored

    def report(self, eligible_demands: int) -> dict[str, int | float | None]:
        denominator = self.useful + self.unused + self.redundant
        return {
            "candidates": self.sent,
            "useful": self.useful,
            "unused": self.unused,
            "redundant": self.redundant,
            "censored": self.censored,
            "eligible_demands": eligible_demands,
            "covered_demands": self.useful,
            "accuracy": self.useful / denominator if denominator else None,
            "coverage_contribution": (
                self.useful / eligible_demands if eligible_demands else None
            ),
        }


def _key_payload(key: _IssuerKey) -> dict[str, object]:
    return {
        "issuer_trigger_pc": hex(key.pc) if key.has_pc else None,
        "issuer_has_pc": key.has_pc,
        "bop_kind": key.bop_kind,
    }


def _pc_payload(has_pc: bool, pc: int) -> dict[str, object]:
    return {
        "issuer_trigger_pc": hex(pc) if has_pc else None,
        "issuer_has_pc": has_pc,
    }


def _key_sort(key: _IssuerKey) -> tuple[str, int, int]:
    return (key.bop_kind, 0 if key.has_pc else 1, key.pc)


class _PCQualityView:
    """One demand-oracle view with bounded pending candidate attribution."""

    def __init__(self, horizon: int):
        self.eligible_demands = 0
        self.by_key: dict[_IssuerKey, _Counts] = defaultdict(_Counts)
        self.pending_issuer: dict[int, _IssuerKey] = {}
        self.demand_owners: dict[int, _IssuerKey] = {}
        self.oracle = replay.DemandOracle(
            horizon, keep_candidates=False, on_resolve=self._resolve,
        )

    def observe_demand(self, demand: replay.Demand) -> None:
        self.eligible_demands += 1
        self.oracle.observe_demand(demand)

    def emit(self, key: _IssuerKey, access_seq: int, tick: int, addr: int,
             phase_id: int) -> None:
        self.by_key[key].sent += 1
        candidate = self.oracle.emit(
            key.bop_kind, access_seq, tick, addr, phase_id,
        )
        self.pending_issuer[candidate.candidate_id] = key

    def _resolve(self, candidate: replay.Candidate) -> None:
        key = self.pending_issuer.pop(candidate.candidate_id)
        self.by_key[key].resolve(candidate.status)
        if candidate.status != "useful":
            return
        if candidate.matched_demand_seq is None:
            raise AssertionError("useful candidate is missing its demand sequence")
        previous = self.demand_owners.setdefault(candidate.matched_demand_seq, key)
        if previous != key:
            raise AssertionError("one demand has two useful candidate owners")

    def finish(self) -> None:
        self.oracle.finish()
        if self.pending_issuer:
            raise AssertionError("unresolved candidate issuer attribution")

    def aggregate(self) -> _Counts:
        result = _Counts()
        for counts in self.by_key.values():
            result.add(counts)
        return result


class _PCQualityTracker:
    """Combined and isolated-BOP candidate labels keyed by issuer PC."""

    def __init__(self, horizon: int):
        self.views = {
            "combined": _PCQualityView(horizon),
            "large": _PCQualityView(horizon),
            "small": _PCQualityView(horizon),
        }

    def observe_demand(self, demand: replay.Demand) -> None:
        for view in self.views.values():
            view.observe_demand(demand)

    def emit(self, key: _IssuerKey, access_seq: int, tick: int, addr: int,
             phase_id: int) -> None:
        self.views["combined"].emit(key, access_seq, tick, addr, phase_id)
        self.views[key.bop_kind].emit(key, access_seq, tick, addr, phase_id)

    def finish(self) -> None:
        for view in self.views.values():
            view.finish()

    def report_for_key(self, key: _IssuerKey) -> dict[str, object]:
        combined = self.views["combined"]
        isolated = self.views[key.bop_kind]
        return {
            "combined": combined.by_key.get(key, _Counts()).report(
                combined.eligible_demands
            ),
            "isolated_kind": isolated.by_key.get(key, _Counts()).report(
                isolated.eligible_demands
            ),
        }

    def aggregate_report(self) -> dict[str, dict[str, int | float | None]]:
        return {
            view_name: view.aggregate().report(view.eligible_demands)
            for view_name, view in self.views.items()
        }

    def keys(self) -> set[_IssuerKey]:
        return set(self.views["combined"].by_key)


@dataclass(frozen=True)
class _PolicyDecision:
    issued: bool
    admission_reason: str
    validation: str
    rr_owner_relation: str


class _TracingPCValidationController(replay.PCValidationController):
    """Exact controller with a non-functional decision explanation sideband."""

    def __init__(self, config: replay.BOPConfig):
        super().__init__(config)
        self.last_decision = _PolicyDecision(False, "not_run", "not_checked", "none")
        self._last_admission_reason: str | None = None

    def _admit_by_confidence(
        self, lookup, pc: int, kind: str, trigger_line: int, best_offset: int,
        *, note_pc_validation_miss: bool,
    ) -> bool:
        issued = super()._admit_by_confidence(
            lookup, pc, kind, trigger_line, best_offset,
            note_pc_validation_miss=note_pc_validation_miss,
        )
        prefix = "" if note_pc_validation_miss else "same_key_"
        if self.global_bypass:
            self._last_admission_reason = f"{prefix}global_bypass"
        elif lookup.state == "high":
            self._last_admission_reason = f"{prefix}high"
        elif lookup.state == "medium":
            self._last_admission_reason = (
                f"{prefix}medium_sampled"
                if issued else f"{prefix}medium_suppressed"
            )
        else:
            self._last_admission_reason = f"{prefix}low_suppressed"
        return issued

    def policy_candidate_values(
        self, *, bop_kind: str, best_offset: int, best_offset_changed: bool,
        raw_candidate_valid: bool, pc_confidence_enabled: bool,
        validation_enabled: bool, validation_hit: int, trigger_addr: int,
        trigger_pc: int, trigger_has_pc: bool,
        validation_owner_pc: int = 0, validation_owner_valid: bool = False,
    ) -> bool:
        self._last_admission_reason = None
        issued = super().policy_candidate_values(
            bop_kind=bop_kind,
            best_offset=best_offset,
            best_offset_changed=best_offset_changed,
            raw_candidate_valid=raw_candidate_valid,
            pc_confidence_enabled=pc_confidence_enabled,
            validation_enabled=validation_enabled,
            validation_hit=validation_hit,
            trigger_addr=trigger_addr,
            trigger_pc=trigger_pc,
            trigger_has_pc=trigger_has_pc,
            validation_owner_pc=validation_owner_pc,
            validation_owner_valid=validation_owner_valid,
        )
        if not validation_enabled:
            validation = "disabled"
        elif validation_hit == 1:
            validation = "hit"
        elif validation_hit == 0:
            validation = "miss"
        else:
            validation = "not_checked"

        owner_relation = "not_applicable"
        if (self.config.pc_validation_producer_consumer and validation == "hit"
                and pc_confidence_enabled):
            if not validation_owner_valid:
                owner_relation = "invalid"
            elif not trigger_has_pc:
                owner_relation = "owner_only"
            elif self._pc_key(validation_owner_pc, bop_kind) == self._pc_key(
                    trigger_pc, bop_kind):
                owner_relation = "same_key"
            else:
                owner_relation = "cross_key"

        if not raw_candidate_valid:
            admission_reason = "raw_invalid"
        elif not pc_confidence_enabled:
            admission_reason = "confidence_disabled"
        elif validation == "hit":
            if owner_relation == "cross_key":
                admission_reason = self._last_admission_reason or "cross_key_unknown"
            elif owner_relation == "same_key" and self._last_admission_reason:
                admission_reason = self._last_admission_reason
            elif owner_relation == "not_applicable":
                admission_reason = "rr_hit"
            else:
                admission_reason = f"rr_hit_{owner_relation}"
        elif trigger_has_pc:
            admission_reason = self._last_admission_reason or "unknown"
        else:
            admission_reason = (
                "no_pc_global_bypass" if issued else "no_pc_suppressed"
            )
        self.last_decision = _PolicyDecision(
            issued, admission_reason, validation, owner_relation,
        )
        return issued


class _DecisionTracker:
    """Selected-window controller causes for raw issuer candidates."""

    def __init__(self):
        self.by_key: dict[_IssuerKey, Counter[str]] = defaultdict(Counter)

    def observe(self, key: _IssuerKey, decision: _PolicyDecision) -> None:
        counts = self.by_key[key]
        counts["raw_candidates"] += 1
        counts["issued" if decision.issued else "suppressed"] += 1
        counts[f"admission_{decision.admission_reason}"] += 1
        counts[f"validation_{decision.validation}"] += 1
        counts[f"rr_owner_{decision.rr_owner_relation}"] += 1

    def report_for_key(self, key: _IssuerKey) -> dict[str, object]:
        counts = self.by_key.get(key, Counter())
        return {
            "raw_candidates": int(counts["raw_candidates"]),
            "issued": int(counts["issued"]),
            "suppressed": int(counts["suppressed"]),
            "admission_reasons": {
                name.removeprefix("admission_"): int(value)
                for name, value in sorted(counts.items())
                if name.startswith("admission_")
            },
            "validation": {
                name.removeprefix("validation_"): int(value)
                for name, value in sorted(counts.items())
                if name.startswith("validation_")
            },
            "rr_owner_relation": {
                name.removeprefix("rr_owner_"): int(value)
                for name, value in sorted(counts.items())
                if name.startswith("rr_owner_")
            },
        }


def _delta(
    raw: Mapping[str, int | float | None], current: Mapping[str, int | float | None],
) -> dict[str, int | float | None]:
    raw_accuracy = raw["accuracy"]
    current_accuracy = current["accuracy"]
    return {
        field: int(current[field]) - int(raw[field])
        for field in ("candidates", "useful", "unused", "redundant", "censored")
    } | {
        "accuracy_points": (
            (float(current_accuracy) - float(raw_accuracy)) * 100
            if current_accuracy is not None and raw_accuracy is not None else None
        ),
        "coverage_points": (
            (float(current["coverage_contribution"])
             - float(raw["coverage_contribution"])) * 100
            if current["coverage_contribution"] is not None
            and raw["coverage_contribution"] is not None else None
        ),
    }


def _merge_counts(items: Iterable[_Counts]) -> _Counts:
    result = _Counts()
    for item in items:
        result.add(item)
    return result


def _row_for_key(
    key: _IssuerKey, raw: _PCQualityTracker, current: _PCQualityTracker,
    decisions: _DecisionTracker,
) -> dict[str, object]:
    raw_report = raw.report_for_key(key)
    current_report = current.report_for_key(key)
    return {
        **_key_payload(key),
        "raw": raw_report,
        "current": current_report,
        "combined_delta": _delta(
            raw_report["combined"], current_report["combined"],
        ),
        "current_decisions": decisions.report_for_key(key),
    }


def _pc_rows(
    raw: _PCQualityTracker, current: _PCQualityTracker,
    decisions: _DecisionTracker,
) -> list[dict[str, object]]:
    groups: dict[tuple[bool, int], list[_IssuerKey]] = defaultdict(list)
    for key in raw.keys() | current.keys() | set(decisions.by_key):
        groups[(key.has_pc, key.pc)].append(key)

    rows = []
    for (has_pc, pc), keys in groups.items():
        raw_counts = _merge_counts(
            raw.views["combined"].by_key.get(key, _Counts()) for key in keys
        )
        current_counts = _merge_counts(
            current.views["combined"].by_key.get(key, _Counts()) for key in keys
        )
        decision = Counter()
        for key in keys:
            decision.update(decisions.by_key.get(key, Counter()))
        eligible = raw.views["combined"].eligible_demands
        raw_report = raw_counts.report(eligible)
        current_report = current_counts.report(eligible)
        rows.append({
            **_pc_payload(has_pc, pc),
            "raw": raw_report,
            "current": current_report,
            "combined_delta": _delta(raw_report, current_report),
            "current_decisions": {
                "raw_candidates": int(decision["raw_candidates"]),
                "issued": int(decision["issued"]),
                "suppressed": int(decision["suppressed"]),
            },
        })
    return sorted(
        rows,
        key=lambda row: (
            -max(
                int(row["raw"]["candidates"]),
                int(row["current"]["candidates"]),
            ),
            0 if row["issuer_has_pc"] else 1,
            row["issuer_trigger_pc"] or "",
        ),
    )


def _hot_keys(
    raw: _PCQualityTracker, current: _PCQualityTracker, top: int,
) -> set[_IssuerKey]:
    selected: set[_IssuerKey] = set()
    keys = raw.keys() | current.keys()
    for kind in ("large", "small"):
        kind_keys = [key for key in keys if key.bop_kind == kind]
        metrics = (
            lambda key: raw.views["combined"].by_key.get(key, _Counts()).sent,
            lambda key: current.views["combined"].by_key.get(key, _Counts()).sent,
            lambda key: (
                raw.views["combined"].by_key.get(key, _Counts()).useful
                - current.views["combined"].by_key.get(key, _Counts()).useful
            ),
            lambda key: (
                raw.views["combined"].by_key.get(key, _Counts()).unused
                - current.views["combined"].by_key.get(key, _Counts()).unused
            ),
        )
        for metric in metrics:
            selected.update(sorted(
                kind_keys, key=lambda key: (metric(key), _key_sort(key)), reverse=True,
            )[:top])
    return selected


def _coverage_transition(
    raw: _PCQualityTracker, current: _PCQualityTracker,
) -> dict[str, object]:
    raw_owners = raw.views["combined"].demand_owners
    current_owners = current.views["combined"].demand_owners
    raw_only = Counter()
    current_only = Counter()
    owner_changes = Counter()
    both = 0
    unchanged_owner = 0

    for demand_seq, raw_owner in raw_owners.items():
        current_owner = current_owners.get(demand_seq)
        if current_owner is None:
            raw_only[raw_owner] += 1
        else:
            both += 1
            if raw_owner == current_owner:
                unchanged_owner += 1
            else:
                owner_changes[(raw_owner, current_owner)] += 1
    for demand_seq, current_owner in current_owners.items():
        if demand_seq not in raw_owners:
            current_only[current_owner] += 1

    def owner_rows(counter: Counter[_IssuerKey]) -> list[dict[str, object]]:
        return [
            {**_key_payload(key), "demands": int(count)}
            for key, count in sorted(
                counter.items(), key=lambda item: (-item[1], _key_sort(item[0])),
            )
        ]

    return {
        "raw_covered_demands": len(raw_owners),
        "current_covered_demands": len(current_owners),
        "raw_only_demands": sum(raw_only.values()),
        "current_only_demands": sum(current_only.values()),
        "both_covered_demands": both,
        "unchanged_owner_demands": unchanged_owner,
        "ownership_changed_demands": sum(owner_changes.values()),
        "raw_only_by_issuer_pc_kind": owner_rows(raw_only),
        "current_only_by_issuer_pc_kind": owner_rows(current_only),
        "ownership_changes": [
            {
                "raw": _key_payload(raw_owner),
                "current": _key_payload(current_owner),
                "demands": int(count),
            }
            for (raw_owner, current_owner), count in sorted(
                owner_changes.items(),
                key=lambda item: (-item[1], _key_sort(item[0][0]), _key_sort(item[0][1])),
            )
        ],
    }


def _invariant_report(
    tracker: _PCQualityTracker,
) -> dict[str, dict[str, object]]:
    result = {}
    for view_name, view in tracker.views.items():
        aggregate = view.aggregate().report(view.eligible_demands)
        summed = _merge_counts(view.by_key.values()).report(view.eligible_demands)
        result[view_name] = {
            "pass": aggregate == summed,
            "aggregate": aggregate,
            "per_pc_kind_sum": summed,
        }
    return result


def _expected_quality(path: Path) -> Mapping[str, Mapping[str, object]]:
    payload = json.loads(path.read_text())
    if "points" in payload:
        return payload["points"]["current"]["quality"]
    return payload["horizons"][str(HORIZON)]


def _certify_external(
    actual: Mapping[str, Mapping[str, int | float | None]], path: Path,
) -> dict[str, object]:
    expected = _expected_quality(path)
    mismatches = []
    for kind in ("combined", "large", "small"):
        for field in METRIC_FIELDS:
            if actual[kind][field] != expected[kind][field]:
                mismatches.append({
                    "kind": kind,
                    "field": field,
                    "actual": actual[kind][field],
                    "expected": expected[kind][field],
                })
        if not math.isclose(
            float(actual[kind]["accuracy"] or 0.0),
            float(expected[kind]["accuracy"] or 0.0),
            rel_tol=0.0, abs_tol=1e-15,
        ):
            mismatches.append({
                "kind": kind,
                "field": "accuracy",
                "actual": actual[kind]["accuracy"],
                "expected": expected[kind]["accuracy"],
            })
    return {
        "path": str(path),
        "pass": not mismatches,
        "mismatches": mismatches,
    }


def analyze_pc_quality(
    connection: sqlite3.Connection, config: replay.BOPConfig,
    window: replay.EvaluationWindow, top: int,
) -> dict[str, object]:
    if not config.pc_validation_producer_consumer:
        raise ValueError("PC quality analysis requires producer/consumer mode")
    if top <= 0:
        raise ValueError("top must be positive")
    shared_config = replay._shared_controller_config(config)
    learner_configs = {
        kind: config.for_kind(kind) for kind in ("large", "small")
    }
    recorded_actions = {
        learner_config.bop_name
        for learner_config in learner_configs.values()
        if replay._recorded_delay_actions_compatible(learner_config)
    }
    delay_actions = replay._DelayActionCursors(connection, recorded_actions)
    learners: dict[str, replay.BOPLearner] = {}
    verifier = replay._OnlineVerifier()
    controller = _TracingPCValidationController(shared_config)
    controller_oracle = replay.DemandOracle(
        HORIZON, controller.note_outcome, keep_candidates=False,
    )
    raw = _PCQualityTracker(HORIZON)
    current = _PCQualityTracker(HORIZON)
    decisions = _DecisionTracker()
    counters: Counter[str] = Counter()
    validation_mismatches = 0

    def on_demand(demand: replay.Demand) -> None:
        controller_oracle.observe_demand(demand)
        counters["demands"] += 1
        if replay._in_evaluation_window(demand, window):
            counters["window_demands"] += 1
            raw.observe_demand(demand)
            current.observe_demand(demand)

    def on_event(event: replay.ReplayEvent) -> None:
        nonlocal validation_mismatches
        learner = learners.get(event.bop_kind)
        if learner is None:
            learner_config = learner_configs[event.bop_kind]
            learner = replay.BOPLearner(
                learner_config,
                use_recorded_delay_actions=(
                    learner_config.bop_name in recorded_actions
                ),
            )
            learners[event.bop_kind] = learner
        trigger_action = (
            delay_actions.trigger_action(event, learner)
            if learner.use_recorded_delay_actions else None
        )
        output = learner.process(event, trigger_action)
        verifier.observe(event, output)
        if output.validation_hit != event.validation_hit:
            validation_mismatches += 1

        selected = replay._in_evaluation_window(event, window)
        counters["events"] += 1
        if selected:
            counters["window_events"] += 1

        learner_config = learner_configs[event.bop_kind]
        issued = controller.policy_candidate_values(
            bop_kind=output.kind,
            best_offset=output.best_offset_after,
            best_offset_changed=output.best_offset_changed,
            raw_candidate_valid=output.raw_candidate_valid,
            pc_confidence_enabled=learner_config.pc_validation_confidence,
            validation_enabled=output.validation_enabled,
            validation_hit=output.validation_hit,
            trigger_addr=event.trigger_addr,
            trigger_pc=event.trigger_pc,
            trigger_has_pc=event.trigger_has_pc,
            validation_owner_pc=output.validation_owner_pc,
            validation_owner_valid=output.validation_owner_valid,
        )
        key = _IssuerKey(event.trigger_has_pc, event.trigger_pc, output.kind)
        if selected and output.raw_candidate_valid:
            raw.emit(
                key, event.access_seq, event.tick, output.raw_candidate_addr,
                event.phase_id,
            )
            decisions.observe(key, controller.last_decision)
        if issued:
            controller_oracle.emit(
                output.kind, event.access_seq, event.tick,
                output.raw_candidate_addr, event.phase_id,
            )
            controller.note_issued()
            if selected:
                current.emit(
                    key, event.access_seq, event.tick, output.raw_candidate_addr,
                    event.phase_id,
                )

    def on_access_end() -> None:
        controller.commit()

    replay._stream_trace_rows(connection, on_demand, on_event, on_access_end)
    controller_oracle.finish()
    raw.finish()
    current.finish()
    verification = verifier.report()
    verification["validation_hit_mismatches"] = validation_mismatches
    verification["pass"] = verification["pass"] and validation_mismatches == 0
    if not verification["pass"]:
        raise ValueError(f"native learner replay diverged: {verification}")

    keys = raw.keys() | current.keys() | set(decisions.by_key)
    rows_by_key = {
        key: _row_for_key(key, raw, current, decisions) for key in keys
    }
    all_pc_kind = [rows_by_key[key] for key in sorted(keys, key=_key_sort)]
    all_pc_kind.sort(
        key=lambda row: (
            -max(
                int(row["raw"]["combined"]["candidates"]),
                int(row["current"]["combined"]["candidates"]),
            ),
            row["bop_kind"], row["issuer_trigger_pc"] or "",
        ),
    )
    hot_keys = _hot_keys(raw, current, top)
    hot_pc_kind = [rows_by_key[key] for key in hot_keys]
    hot_pc_kind.sort(
        key=lambda row: (
            -max(
                int(row["raw"]["combined"]["candidates"]),
                int(row["current"]["combined"]["candidates"]),
            ),
            row["bop_kind"], row["issuer_trigger_pc"] or "",
        ),
    )
    return {
        "model": {
            "horizon": HORIZON,
            "candidate_owner": "BOPReplayEvent.TriggerPC",
            "rr_owner": "producer_consumer_training_diagnostic_only",
            "raw_point": "all recorded-online raw BOP candidates",
            "current_point": "replayed producer_consumer controller",
            "quality_window": "selected L2 demands and candidates only",
            "state_replay": "full trace",
            "not_modeled": "local filters, cache fills, MSHRs, residency, bandwidth",
            "quality_views": {
                "combined": "cross-BOP competing candidates; PC rows sum to global coverage",
                "isolated_kind": "per-BOP oracle; rows sum to standard Large/Small metrics",
            },
        },
        "controller_parameters": replay._controller_parameter_report(config),
        "evaluation_window": {
            **asdict(window),
            "demands": int(counters["window_demands"]),
            "events": int(counters["window_events"]),
        },
        "events": int(counters["events"]),
        "demands": int(counters["demands"]),
        "owner_reconstruction": verification,
        "aggregate_quality": {
            "raw": raw.aggregate_report(),
            "current": current.aggregate_report(),
        },
        "per_pc_invariants": {
            "raw": _invariant_report(raw),
            "current": _invariant_report(current),
        },
        "coverage_transition": _coverage_transition(raw, current),
        "hot_selection": {
            "top": top,
            "selection": (
                "per BOP-kind union of top raw candidates, current candidates, "
                "raw-useful loss, and raw-unused reduction"
            ),
        },
        "hot_pc_kind": hot_pc_kind,
        "all_pc_kind": all_pc_kind,
        "all_pc": _pc_rows(raw, current, decisions),
        "controller_stats": controller.stats(),
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("database", type=Path, help="certified V5 BOP trace DB")
    parser.add_argument(
        "--controller-config", type=Path, required=True,
        help="P/C shared controller JSON override",
    )
    parser.add_argument(
        "--evaluation-phase", default="stable",
        help="V5 phase used for reporting; full state is always replayed",
    )
    parser.add_argument(
        "--top", type=int, default=20,
        help="per-BOP hot-PC rank depth for each selection criterion",
    )
    parser.add_argument(
        "--verify-raw-report", type=Path, default=None,
        help="existing raw report whose quality must match exactly",
    )
    parser.add_argument(
        "--verify-current-report", type=Path, default=None,
        help="existing P/C-current report whose quality must match exactly",
    )
    parser.add_argument("--output", type=Path, default=None)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    start = time.perf_counter()
    with sqlite3.connect(args.database) as connection:
        config, phases = replay._streaming_metadata(connection)
        config = replay._load_controller_overrides(args.controller_config, config)
        window = replay.resolve_evaluation_window(
            phase_name=args.evaluation_phase,
            start_tick=None,
            stats_path=None,
            stats_block=None,
            phases=phases,
        )
        report = analyze_pc_quality(connection, config, window, args.top)
    checks = {}
    if args.verify_raw_report is not None:
        checks["raw"] = _certify_external(
            report["aggregate_quality"]["raw"], args.verify_raw_report,
        )
    if args.verify_current_report is not None:
        checks["current"] = _certify_external(
            report["aggregate_quality"]["current"], args.verify_current_report,
        )
    if any(not check["pass"] for check in checks.values()):
        raise ValueError(f"aggregate certification failed: {checks}")
    report["aggregate_certification"] = checks
    report["database"] = str(args.database)
    report["replay_engine"] = {
        "engine": "streaming_raw_pc_current_quality",
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
