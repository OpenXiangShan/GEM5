#!/usr/bin/env python3
"""Read PerfCCT causal MVP tables and emit JSON evidence without attribution."""

import argparse
import json
import sqlite3
import sys
from pathlib import Path


TABLES = (
    "PerfCCTMeta",
    "PerfCCTInst",
    "PerfCCTEvent",
    "PerfCCTCommitSpan",
    "PerfCCTCacheEvent",
    "PerfCCTFetchTransfer",
)


def connect(path):
    db = sqlite3.connect(Path(path).resolve().as_uri() + "?mode=ro", uri=True)
    db.row_factory = sqlite3.Row
    return db


def rows(db, sql, params=()):
    return [dict(row) for row in db.execute(sql, params)]


def metadata(db):
    return dict(db.execute("SELECT Key, Value FROM PerfCCTMeta"))


def has_table(db, name):
    return (
        db.execute(
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?",
            (name,),
        ).fetchone()
        is not None
    )


def instruction_key(row):
    return row["Cpu"], row["TID"], row["SeqNum"]


def identity_dict(key):
    return dict(zip(("Cpu", "TID", "SeqNum"), key))


def request_identity(db, event, meta):
    """Resolve only a unique, complete requestor/context mapping."""
    if not event["SeqNum"] or event["Context"] < 0:
        return {"status": "unknown", "candidates": []}
    candidates = []
    for key, value in meta.items():
        if not key.startswith("cpu.") or not key.endswith(".data_requestor"):
            continue
        cpu = key[4 : -len(".data_requestor")]
        if str(event["Requestor"]) != value:
            continue
        prefix = f"cpu.{cpu}.context."
        for context_key, context in meta.items():
            if context_key.startswith(prefix) and context == str(
                event["Context"]
            ):
                try:
                    tid = int(context_key[len(prefix) :])
                except ValueError:
                    continue
                candidates.append((cpu, tid, event["SeqNum"]))
    if not event["SeqNum"] or len(candidates) != 1:
        return {
            "status": "unknown",
            "candidates": [identity_dict(k) for k in candidates],
        }
    key = candidates[0]
    inst = rows(
        db, "SELECT * FROM PerfCCTInst WHERE Cpu=? AND TID=? AND SeqNum=?", key
    )
    return {
        "status": "resolved" if inst else "missing_instruction",
        "identity": identity_dict(key),
        "instruction": inst[0] if inst else None,
    }


def instruction_cache_events(db, inst, args, meta):
    result = {
        "identity": identity_dict(instruction_key(inst)),
        "events": [],
        "events_truncated": False,
    }
    if not has_table(db, "PerfCCTCacheEvent"):
        result["status"] = "cache_events_unavailable"
        return result
    prefix = "cpu." + inst["Cpu"]
    try:
        requestor = int(meta[prefix + ".data_requestor"])
        context = int(meta[prefix + ".context." + str(inst["TID"])])
    except (KeyError, ValueError):
        result["status"] = "missing_request_mapping"
        return result
    mapping = request_identity(
        db,
        {"Requestor": requestor, "Context": context, "SeqNum": inst["SeqNum"]},
        meta,
    )
    if mapping["status"] != "resolved":
        result["status"] = "ambiguous_or_unknown_request_mapping"
        return result
    clauses, params = ["Requestor=?", "Context=?", "SeqNum=?"], [
        requestor,
        context,
        inst["SeqNum"],
    ]
    for op, value in ((">=", args.start), ("<", args.end)):
        if value is not None:
            clauses.append(f"Tick {op} ?")
            params.append(value)
    events = rows(
        db,
        "SELECT * FROM PerfCCTCacheEvent WHERE "
        + " AND ".join(clauses)
        + " ORDER BY Tick,ID LIMIT ?",
        [*params, args.limit + 1],
    )
    result.update(
        status="resolved",
        Requestor=requestor,
        Context=context,
        events=events[: args.limit],
        events_truncated=len(events) > args.limit,
    )
    return result


def scheduling_evidence(identities, events, args):
    """Expose scheduler observations without reconstructing readiness intervals."""
    grouped = {instruction_key(inst): [] for inst in identities}
    for event in events:
        if event["Event"].startswith("iq_"):
            grouped.setdefault(instruction_key(event), []).append(event)
    results = []
    for key, observed in sorted(grouped.items()):
        observed.sort(key=lambda event: (event["Tick"], event["ID"]))
        selected = observed[: args.limit]
        counts = {}
        for event in selected:
            counts[event["Event"]] = counts.get(event["Event"], 0) + 1
        results.append(
            {
                "identity": identity_dict(key),
                "status": "observed_events_only"
                if observed
                else "no_observed_events",
                "events": selected,
                "event_counts": counts,
                "events_truncated": len(observed) > args.limit,
                "window_filtered": args.start is not None
                or args.end is not None,
                "readiness_coverage": "unknown",
                "notes": [
                    "Repeated candidate, wake, and cancel observations remain distinct.",
                    "Counts describe returned events only, not whole-run scheduling costs.",
                    "A ready candidate need not satisfy memory dependencies or all issue conditions.",
                    "Speculative operand wakes may be canceled; RelatedSeq records the observed producer.",
                    "Detail booleans are observation snapshots, not persistent readiness state.",
                    "Window/limit boundaries can omit earlier readiness or later cancellation/issue.",
                    "Missing events do not establish absence of waiting or complete readiness coverage.",
                    "Observed spacing is not recoverable delay; no readiness interval is inferred.",
                ],
            }
        )
    return results


def target_history(db, history, meta, at, truncated, available):
    """Replay explicit target transitions; service/release do not remove one."""
    result = {
        "available": available,
        "at_tick": at,
        "targets": [],
        "active_targets": [],
        "status": "unavailable",
    }
    if not available:
        return result
    selected = [e for e in history if at is None or e["Tick"] <= at]
    states, issues = {}, []
    for event in selected:
        kind, target = event["Event"], event["TargetID"]
        if kind not in (
            "target_add",
            "target_remove",
            "target_replace",
            "target_service",
            "open_target",
        ):
            continue
        if not target:
            issues.append(
                {"event_id": event["ID"], "reason": "unknown_target_id"}
            )
            continue
        state = states.get(target)
        if kind == "target_add":
            if state is not None:
                issues.append(
                    {"event_id": event["ID"], "reason": "duplicate_target_add"}
                )
            state = {
                "TargetID": target,
                "RequestID": event["RequestID"],
                "add_tick": event["Tick"],
                "remove_tick": None,
                "active": True,
                "end_kind": "unobserved",
                "event_ids": [],
            }
            states[target] = state
        elif state is None:
            issues.append(
                {"event_id": event["ID"], "reason": "missing_target_add"}
            )
            state = {
                "TargetID": target,
                "RequestID": event["RequestID"],
                "add_tick": None,
                "remove_tick": None,
                "active": None,
                "end_kind": "unknown",
                "event_ids": [],
            }
            states[target] = state
        state["event_ids"].append(event["ID"])
        # Extraction may remove the target before the response is serviced.
        # Service observes completion and must not resurrect or invalidate it.
        if (
            kind not in ("target_add", "target_service")
            and state["active"] is False
        ):
            issues.append(
                {
                    "event_id": event["ID"],
                    "reason": "event_after_target_remove",
                }
            )
        if (
            kind not in ("target_add", "target_replace")
            and state["RequestID"]
            and event["RequestID"]
            and state["RequestID"] != event["RequestID"]
        ):
            issues.append(
                {"event_id": event["ID"], "reason": "target_request_mismatch"}
            )
        if kind == "target_replace":
            if not event["RelatedRequestID"]:
                issues.append(
                    {
                        "event_id": event["ID"],
                        "reason": "unknown_replaced_request_id",
                    }
                )
            if (
                state["RequestID"]
                and event["RelatedRequestID"]
                and state["RequestID"] != event["RelatedRequestID"]
            ):
                issues.append(
                    {
                        "event_id": event["ID"],
                        "reason": "replacement_request_mismatch",
                    }
                )
            state["RequestID"] = event["RequestID"]
        elif kind == "target_remove":
            state.update(
                active=False, remove_tick=event["Tick"], end_kind="removed"
            )
        elif kind == "open_target":
            state.update(active=True, end_kind="trace_end_open")
            if at is not None and at > event["Tick"]:
                issues.append(
                    {
                        "event_id": event["ID"],
                        "reason": "query_after_trace_end",
                    }
                )
        state["request_identity"] = request_identity(db, event, meta)
        if not event["RequestID"]:
            issues.append(
                {"event_id": event["ID"], "reason": "unknown_request_id"}
            )
    allocated = any(e["Event"] == "allocate" for e in selected)
    complete = not truncated and allocated and not issues
    result.update(
        targets=list(states.values()),
        active_targets=[s for s in states.values() if s["active"] is True],
        status="complete_observation"
        if complete
        else "incomplete_observation",
        events_truncated=truncated,
        issues=issues,
        at_tick=at
        if at is not None
        else (selected[-1]["Tick"] if selected else None),
    )
    return result


def resource_query(db, args, result):
    result.update(
        available=has_table(db, "PerfCCTCacheEvent"), events=[], lifecycles=[]
    )
    if not result["available"]:
        result["notes"] = [
            "This database has no cache resource events; absence is unknown."
        ]
        return result
    columns = {
        row["name"]
        for row in rows(db, 'PRAGMA table_info("PerfCCTCacheEvent")')
    }
    target_tracking = {"RequestID", "TargetID", "RelatedRequestID"} <= columns
    result["target_tracking_available"] = target_tracking
    if (args.request_id is not None and "RequestID" not in columns) or (
        args.target_id is not None and "TargetID" not in columns
    ):
        result.update(
            available=False,
            notes=[
                "Requested identity filter is unavailable in this database schema."
            ],
        )
        return result
    clauses, params = [], []
    for column, value in (
        ("Cache", args.cache),
        ("MSHR", args.mshr),
        ("RequestID", args.request_id),
        ("TargetID", args.target_id),
    ):
        if value is not None:
            clauses.append(f"{column}=?")
            params.append(value)
    if args.reject is not None:
        clauses.append("(ID=? OR ParentID=?)")
        params.extend((args.reject, args.reject))
    for op, value in ((">=", args.start), ("<", args.end)):
        if value is not None:
            clauses.append(f"Tick {op} ?")
            params.append(value)
    where = " WHERE " + " AND ".join(clauses) if clauses else ""
    events = rows(
        db,
        "SELECT * FROM PerfCCTCacheEvent"
        + where
        + " ORDER BY Tick,ID LIMIT ?",
        [*params, args.limit + 1],
    )
    result["events_truncated"] = len(events) > args.limit
    events = events[: args.limit]
    meta = result["metadata"]
    for event in events:
        event["request_identity"] = request_identity(db, event, meta)
    result["events"] = events
    generations = sorted(
        {(e["Cache"], e["MSHR"]) for e in events if e["MSHR"]}
    )
    # A specific generation can be inspected even if the selected window
    # falls between its events. Never treat generation zero as an identity.
    if args.cache is not None and args.mshr:
        generations = [(args.cache, args.mshr)]
    context_remaining = args.limit
    for cache, generation in generations:
        history = rows(
            db,
            "SELECT * FROM PerfCCTCacheEvent WHERE Cache=? AND MSHR=?"
            + " ORDER BY Tick,ID LIMIT ?",
            (cache, generation, context_remaining + 1),
        )
        truncated = len(history) > context_remaining
        history = history[:context_remaining]
        context_remaining -= len(history)
        members = {}
        for event in history:
            if event["Event"] in ("allocate", "merge"):
                key = event["Requestor"], event["Context"], event["SeqNum"]
                if "RequestID" in columns:
                    key += (event["RequestID"],)
                members[key] = {
                    "Requestor": key[0],
                    "Context": key[1],
                    "SeqNum": key[2],
                    "mapping": request_identity(db, event, meta),
                }
                if "RequestID" in columns:
                    members[key]["RequestID"] = event["RequestID"]
        result["lifecycles"].append(
            {
                "Cache": cache,
                "MSHR": generation,
                "events": history,
                "events_truncated": truncated,
                "members": list(members.values()),
                "historical_members": list(members.values()),
                "target_state": target_history(
                    db, history, meta, args.at, truncated, target_tracking
                ),
                "allocation_observed": any(
                    e["Event"] == "allocate" for e in history
                ),
                "release_observed": any(
                    e["Event"] == "release" for e in history
                ),
            }
        )
    rejects = {e["ID"] for e in events if e["Event"] == "reject"}
    rejects.update(e["ParentID"] for e in events if e["ParentID"])
    result["rejections"] = []
    for parent in sorted(rejects):
        rejection = rows(
            db,
            "SELECT * FROM PerfCCTCacheEvent WHERE ID=? AND Event='reject'",
            (parent,),
        )
        owners = rows(
            db,
            "SELECT * FROM PerfCCTCacheEvent WHERE ParentID=? ORDER BY ID LIMIT ?",
            (parent, context_remaining + 1),
        )
        owner_count = min(len(owners), context_remaining)
        result["rejections"].append(
            {
                "reject": rejection[0] if rejection else None,
                "ParentID": parent,
                "owners": owners[:context_remaining],
                "owners_truncated": len(owners) > context_remaining,
                "target_owners": [
                    dict(e, request_identity=request_identity(db, e, meta))
                    for e in owners[:context_remaining]
                    if e["Event"] == "target_owner"
                ],
                "target_owners_status": (
                    "unavailable"
                    if not target_tracking
                    else "truncated"
                    if len(owners) > context_remaining
                    else "snapshot_observation"
                    if rejection
                    else "missing_reject"
                ),
            }
        )
        context_remaining -= owner_count
    result["notes"] = [
        "Window filters select events; lifecycle and rejection context may lie outside it.",
        "--limit bounds selected events and separately the total lifecycle/owner context events.",
        "MSHR is a cache-local generation; zero is unknown, not a shared resource.",
        "Request mapping requires a unique Requestor/Context mapping and exact SeqNum.",
        "Only allocate/merge events establish lifecycle members; snapshots are observations.",
        "members/historical_members are historical requests, not current target holders.",
        "target_owners use only ParentID-bound target_owner snapshots at rejection, including deferred targets.",
        "Target state applies events in Tick/ID order through --at inclusively; "
        "removals are effective after their observation.",
        "TargetID is cache-local and RequestID identifies a Request object, not an instruction attempt.",
        "target_service/release do not end targets; target_replace keeps TargetID and changes RequestID.",
        "An incomplete target state is not a complete owner set; open_target is an exit snapshot, not removal.",
        "owner/owner_credit are the observed owner set, not individually proven culprits.",
        "Allocated is observed after allocation and before release, not a uniform post-event count.",
        "BlockedMask is the pre-transition state for blocked/unblocked; Detail is the cause enum.",
        "Blocked causes: 0=NoMSHRs, 1=NoWBBuffers, 2=NoTargets.",
        "HeldCredits is post-change for credit_hold/credit_release.",
        "release does not imply held credits ended; inspect credit_hold/credit_release.",
        "Resource observations are not a proof of exclusive causality or recoverable cycles.",
    ]
    return result


def chain_query(db, args, result):
    clauses, params = filters(args)
    clauses.append("SeqNum=?")
    roots = rows(
        db,
        "SELECT * FROM PerfCCTInst WHERE "
        + " AND ".join(clauses)
        + " ORDER BY Cpu,TID",
        [*params, args.seq],
    )
    pending = [(instruction_key(inst), 0) for inst in roots]
    visited, nodes, edges = set(), [], []
    while pending and len(nodes) < args.nodes:
        key, depth = pending.pop(0)
        if key in visited:
            continue
        visited.add(key)
        identity = rows(
            db,
            "SELECT * FROM PerfCCTInst WHERE Cpu=? AND TID=? AND SeqNum=?",
            key,
        )
        node = {
            "identity": identity_dict(key),
            "depth": depth,
            "instruction": identity[0] if identity else None,
            "status": "resolved" if identity else "missing_instruction",
        }
        nodes.append(node)
        clauses = ["Cpu=?", "TID=?", "SeqNum=?"]
        wait_clauses = ["Event='wait_begin'", "Detail='stlf'"]
        params = list(key)
        for op, value in ((">=", args.start), ("<", args.end)):
            if value is not None:
                wait_clauses.append(f"Tick {op} ?")
                params.append(value)
        clauses.append(
            "(Event='dependency' OR (" + " AND ".join(wait_clauses) + "))"
        )
        events = rows(
            db,
            "SELECT * FROM PerfCCTEvent WHERE "
            + " AND ".join(clauses)
            + " ORDER BY Tick,ID",
            params,
        )
        for event in events:
            target = (key[0], key[1], event["RelatedSeq"])
            edge = {
                "from": identity_dict(key),
                "event": event,
                "to": identity_dict(target) if event["RelatedSeq"] else None,
            }
            edges.append(edge)
            if not event["RelatedSeq"]:
                edge["status"] = "unknown_related_instruction"
            elif target in visited:
                edge["status"] = "already_visited"
            elif depth >= args.depth:
                edge["status"] = "depth_limit"
            else:
                edge["status"] = "queued"
                pending.append((target, depth + 1))
    for edge in edges:
        if edge.get("status") == "queued":
            edge["status"] = (
                "visited"
                if instruction_key(edge["to"]) in visited
                else "node_limit"
            )
    result.update(
        roots=[identity_dict(instruction_key(i)) for i in roots],
        root_status="resolved" if roots else "missing_instruction",
        nodes=nodes,
        edges=edges,
        truncated=any(
            e["status"] in ("depth_limit", "node_limit") for e in edges
        )
        or any(k not in visited for k, _ in pending),
    )
    result["notes"] = [
        "Edges follow observed RelatedSeq within the same Cpu/TID, never a guessed producer.",
        "Only dependency and wait_begin/stlf events are traversed; other causes remain unknown.",
        "STLF waits use the query window; dependencies use full lifecycle, including pre-window rename.",
        "Repeated nodes are not expanded twice; relations do not prove exclusive causality.",
        "An empty chain does not prove independence; older traces may lack dependency events.",
    ]
    return result


def filters(args, tick_column=None):
    clauses, params = [], []
    for column, value in (("Cpu", args.cpu), ("TID", args.tid)):
        if value is not None:
            clauses.append(f"{column} = ?")
            params.append(value)
    if tick_column:
        for op, value in ((">=", args.start), ("<", args.end)):
            if value is not None:
                clauses.append(f"{tick_column} {op} ?")
                params.append(value)
    return clauses, params


def clip_span(row, start, end):
    row = dict(row)
    left = (
        max(row["StartTick"], start) if start is not None else row["StartTick"]
    )
    # An absent end is unknown, even if the query has a finite upper bound.
    right = row["EndTick"]
    if right is not None and end is not None:
        right = min(right, end)
    row["covered_start_tick"] = left
    row["covered_end_tick"] = right
    row["covered_ticks"] = None if right is None else max(0, right - left)
    row["boundary_incomplete"] = row["EndKind"] != "closed" or right is None
    return row


def span_rows(db, args, extra=(), values=(), limit=None):
    clauses, params = filters(args)
    clauses.extend(extra)
    params.extend(values)
    if args.start is not None:
        clauses.append("(EndTick > ? OR EndTick IS NULL)")
        params.append(args.start)
    if args.end is not None:
        clauses.append("StartTick < ?")
        params.append(args.end)
    where = " WHERE " + " AND ".join(clauses) if clauses else ""
    order = " ORDER BY StartTick, ID"
    if limit is not None:
        right = "EndTick" if args.end is None else "min(EndTick, ?)"
        left = "StartTick" if args.start is None else "max(StartTick, ?)"
        if args.end is not None:
            params.append(args.end)
        if args.start is not None:
            params.append(args.start)
        order = (
            f" ORDER BY (EndTick IS NULL), ({right} - {left}) DESC,"
            " StartTick, ID LIMIT ?"
        )
        params.append(limit)
    return [
        clip_span(row, args.start, args.end)
        for row in rows(
            db, "SELECT * FROM PerfCCTCommitSpan" + where + order, params
        )
    ]


def query(db, args):
    if args.command == "schema":
        return {
            "tables": {
                name: rows(db, f'PRAGMA table_info("{name}")')
                for name in TABLES
            },
            "metadata": metadata(db),
        }
    result = {
        "metadata": metadata(db),
        "query_window": {"start_tick": args.start, "end_tick": args.end},
        "time_unit": "tick",
    }
    if args.command == "diagnose":
        from perfcct_diagnose import diagnose

        return diagnose(
            db, args, result, instruction_cache_events, request_identity
        )
    if args.command == "resources":
        return resource_query(db, args, result)
    if args.command == "chain":
        return chain_query(db, args, result)
    if args.command == "stalls":
        clause = {
            "all": (),
            "zero": ("Committed = 0",),
            "partial": ("Committed > 0",),
        }
        extra = list(clause[args.kind])
        values = []
        if args.reason:
            placeholders = ",".join("?" for _ in args.reason)
            extra.append(f"Reason IN ({placeholders})")
            values.extend(args.reason)
        spans = span_rows(db, args, extra, values, limit=args.top)
        result.update(returned_spans=len(spans), spans=spans)
        result["notes"] = [
            "Ranking is by observed covered ticks, not recoverable cycles.",
            "SampleCycles counts original decisions; ROI clipping does not prorate it.",
            "Unknown ends rank last; trace_end/gap spans are incomplete observations.",
            "Zero commits includes empty/squash/status states; inspect Reason.",
            "Partial means Committed > 0; a full commit window need not imply a stall.",
        ]
        return result
    clauses, params = filters(args)
    clauses.append("SeqNum = ?")
    params.append(args.seq)
    identities = rows(
        db,
        "SELECT * FROM PerfCCTInst WHERE "
        + " AND ".join(clauses)
        + " ORDER BY Cpu, TID",
        params,
    )
    clauses, params = filters(args, "Tick")
    clauses.append("SeqNum = ?")
    params.append(args.seq)
    events = rows(
        db,
        "SELECT * FROM PerfCCTEvent WHERE "
        + " AND ".join(clauses)
        + " ORDER BY Tick, ID",
        params,
    )
    spans = span_rows(
        db, args, ("(HeadSeq = ? OR BlockerSeq = ?)",), (args.seq, args.seq)
    )
    spans.sort(key=lambda row: (row["StartTick"], row["ID"]))
    lifetimes = []
    has_legacy = db.execute(
        "SELECT 1 FROM sqlite_master WHERE type='table' "
        "AND name='LifeTimeCommitTrace'"
    ).fetchone()
    if has_legacy:
        for inst in identities:
            if inst["CommitID"] is None:
                continue
            for lifetime in rows(
                db,
                "SELECT * FROM LifeTimeCommitTrace WHERE ID = ?",
                (inst["CommitID"],),
            ):
                lifetimes.append(
                    {
                        "Cpu": inst["Cpu"],
                        "TID": inst["TID"],
                        "SeqNum": inst["SeqNum"],
                        "CommitID": inst["CommitID"],
                        "lifetime": lifetime,
                    }
                )
    result.update(
        instructions=identities,
        events=events,
        commit_spans=spans,
        lifetimes=lifetimes,
        schedule_evidence=scheduling_evidence(identities, events, args),
        cache_events=[
            instruction_cache_events(db, inst, args, result["metadata"])
            for inst in identities
        ],
    )
    result["notes"] = [
        "Identity is (Cpu, TID, SeqNum); unqualified seq may match multiple identities.",
        "RelatedSeq is an observed relation, not proof of exclusive causality.",
        "Only events/spans use the query window; instruction metadata is unfiltered.",
        "Missing events or unresolved EndKind do not imply absence of waiting.",
        "Cache events require exact Requestor/Context/SeqNum mapping; use their Cache/MSHR or reject ID in resources.",
    ]
    return result


def parser():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("db", help="SQLite trace database (opened read-only)")
    commands = cli.add_subparsers(dest="command", required=True)
    commands.add_parser("schema", help="Show table columns and trace metadata")
    diagnose = commands.add_parser(
        "diagnose", help="Rank PC symptoms and select evidence entry points"
    )
    diagnose.add_argument("--cpu", required=True)
    diagnose.add_argument("--tid", type=int, required=True)
    diagnose.add_argument("--start", type=int)
    diagnose.add_argument("--end", type=int)
    diagnose.add_argument(
        "--stats", help="Infer ROI from final complete stats section"
    )
    diagnose.add_argument("--top", type=int, default=5)
    diagnose.add_argument("--limit", type=int, default=128)
    stalls = commands.add_parser(
        "stalls", help="Rank observed commit-blocked spans"
    )
    stalls.add_argument(
        "--kind", choices=("all", "zero", "partial"), default="all"
    )
    stalls.add_argument("--top", type=int, default=20)
    stalls.add_argument(
        "--reason",
        action="append",
        default=[],
        help="Include this Reason; repeat to include multiple reasons",
    )
    inst = commands.add_parser(
        "inst", help="Inspect a dynamic instruction and its events"
    )
    inst.add_argument("seq", type=int)
    inst.add_argument(
        "--limit",
        type=int,
        default=2000,
        help="Maximum cache/schedule evidence events per matched instruction",
    )
    chain = commands.add_parser(
        "chain", help="Follow observed dependency and STLF relations"
    )
    chain.add_argument("seq", type=int)
    chain.add_argument("--depth", type=int, default=4)
    chain.add_argument("--nodes", type=int, default=100)
    resources = commands.add_parser(
        "resources", help="Inspect cache MSHR generations and owners"
    )
    resources.add_argument("--cache")
    resources.add_argument("--mshr", type=int)
    resources.add_argument(
        "--request-id", type=int, help="Exact Request object ID"
    )
    resources.add_argument(
        "--target-id", type=int, help="Exact cache-local Target ID"
    )
    resources.add_argument(
        "--reject", type=int, help="Reject event ID and its owner snapshots"
    )
    resources.add_argument(
        "--at",
        type=int,
        help="Reconstruct target state after events at this tick",
    )
    resources.add_argument("--limit", type=int, default=2000)
    for command in (stalls, inst, chain):
        command.add_argument("--cpu")
        command.add_argument("--tid", type=int)
    for command in (stalls, inst, chain, resources):
        command.add_argument("--start", type=int, help="Inclusive start tick")
        command.add_argument("--end", type=int, help="Exclusive end tick")
    return cli


def main(argv=None):
    cli = parser()
    args = cli.parse_args(argv)
    if args.command != "schema":
        if (
            args.start is not None
            and args.end is not None
            and args.start >= args.end
        ):
            cli.error("--start must be less than --end")
        if args.command in ("stalls", "diagnose") and args.top < 1:
            cli.error("--top must be positive")
        if args.command == "chain" and (args.depth < 0 or args.nodes < 1):
            cli.error(
                "--depth must be nonnegative and --nodes must be positive"
            )
        if (
            args.command in ("resources", "inst", "diagnose")
            and args.limit < 1
        ):
            cli.error("--limit must be positive")
    try:
        db = connect(args.db)
        try:
            print(json.dumps(query(db, args), indent=2, ensure_ascii=False))
        finally:
            db.close()
    except (sqlite3.Error, OSError, ValueError) as error:
        print(
            json.dumps(
                {
                    "error": str(error),
                    "hint": "Use an MVP-enabled PerfCCT database.",
                }
            ),
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
