"""Bounded diagnostic entry points over observed PerfCCT evidence."""


import math
from types import SimpleNamespace


def final_stats(path):
    """Return the final completed dump, rejecting unfinished trailing dumps."""
    current, last = None, None
    with open(path, encoding="utf-8") as stream:
        for line in stream:
            if "Begin Simulation Statistics" in line:
                current = {}
            elif "End Simulation Statistics" in line:
                if current is not None:
                    last = current
                current = None
            elif current is not None:
                fields = line.split()
                if len(fields) >= 2:
                    current[fields[0]] = fields[1]
    if current is not None:
        raise ValueError(
            "trailing stats section is incomplete; refusing an earlier ROI"
        )
    if last is None or not {"finalTick", "simTicks"} <= last.keys():
        raise ValueError(
            "last complete stats section needs finalTick and simTicks"
        )
    return last


def stats_roi(path):
    """Use the final completed stats dump as a half-open tick window."""
    last = final_stats(path)
    try:
        end, ticks = int(last["finalTick"]), int(last["simTicks"])
    except ValueError as error:
        raise ValueError("finalTick and simTicks must be integers") from error
    if ticks <= 0 or end < ticks:
        raise ValueError("stats ROI needs 0 < simTicks <= finalTick")
    return end - ticks, end


def stats_context(path, cpu):
    if not path:
        return {
            "status": "unavailable_without_stats",
            "scope": "Topdown cannot be assigned to an explicit tick window without --stats",
        }
    section = final_stats(path)
    names = {
        "level1": (
            "baseRetiring",
            "frontendBound",
            "badSpecBound",
            "backendBound",
        ),
        "frontend_level2": ("frontendLatencyBound", "frontendBandwidthBound"),
        "fetch_counters": (
            "fetch.fetchBubbles",
            "fetch.fetchBubbles_max",
            "iew.fetchStallReason::FetchFragStall",
        ),
    }
    values = {}
    missing = []
    for group, suffixes in names.items():
        values[group] = {}
        for suffix in suffixes:
            key = f"{cpu}.{suffix}"
            try:
                value = float(section[key])
                if not math.isfinite(value):
                    raise ValueError("nonfinite")
            except (KeyError, ValueError):
                value = None
                missing.append(key)
            values[group][suffix] = value
    histogram_prefix = f"{cpu}.fetch.instsSentToDecodePerCycle::"
    bins = [
        int(key[len(histogram_prefix) :])
        for key in section
        if key.startswith(histogram_prefix)
        and key[len(histogram_prefix) :].isdigit()
    ]
    # The distribution is initialized with the configured transfer width;
    # max_value instead describes observed samples and can be smaller.
    values["decode_width_from_histogram_bins"] = max(bins) if bins else None
    return {
        "status": "available" if not missing else "partial",
        "source": "last_complete_stats_section",
        **values,
        "missing_or_nonfinite": missing,
        "scope_hint": "Topdown Level1 values are raw background, not an automatic bottleneck ranking",
        "counter_note": (
            "IEW FetchFragStall is a propagated slot count; "
            "not a cause or identical to Fetch transfer samples"
        ),
    }


def dispatch_stall_context(path, cpu, top=5):
    if not path:
        return {"status": "unavailable_without_stats"}
    section = final_stats(path)
    prefix = f"{cpu}.iew.dispatchStallReason::"
    counts = {}
    for key, raw in section.items():
        if not key.startswith(prefix):
            continue
        try:
            value = float(raw)
            if not math.isfinite(value) or value < 0 or not value.is_integer():
                raise ValueError("invalid dispatch slot count")
            counts[key[len(prefix) :]] = int(value)
        except ValueError:
            return {"status": "invalid_counter", "key": key}
    total = counts.pop("total", None)
    if total is None or total <= 0:
        return {"status": "unavailable_missing_total"}
    ranked = sorted(
        (
            (name, count)
            for name, count in counts.items()
            if name != "NoStall" and count > 0
        ),
        key=lambda item: (-item[1], item[0]),
    )
    return {
        "status": "available",
        "source": "last_complete_stats_section",
        "unit": "dispatch slot samples at IEW; not exclusive recoverable cycles",
        "total_slot_samples": total,
        "named_bin_sum": sum(counts.values()),
        "bins_match_total": sum(counts.values()) == total,
        "no_stall": {
            "count": counts.get("NoStall"),
            "fraction": (
                counts["NoStall"] / total if "NoStall" in counts else None
            ),
        },
        "top_non_nostall": [
            {"reason": name, "count": count, "fraction": count / total}
            for name, count in ranked[:top]
        ],
        "omitted_nonzero_bins": max(0, len(ranked) - top),
        "scope": (
            "IEW samples propagated lane reasons after dispatch each tick; "
            "these are a different observation point from Fetch partial transfers"
        ),
    }


def investigation_hints(dispatch, blockers, fetch, symptoms=()):
    """Map observed dispatch labels to places worth inspecting next."""
    routes = []
    valid_dispatch = (
        dispatch["status"] == "available" and dispatch["bins_match_total"]
    )
    if valid_dispatch:
        for bin_ in dispatch["top_non_nostall"]:
            reason = bin_["reason"]
            if reason in (
                "ControlRecovery",
                "MemVioRecovery",
                "VPRecovery",
                "TrapRecovery",
                "CommitSquash",
            ):
                entry = "control_recovery"
            elif reason.startswith("Fetch") or reason in ("InstMisPred",):
                entry = "fetch_ftq_supply"
            elif reason.startswith(("Load", "Store")) or reason in (
                "ScalarLongExecute",
                "VectorLongExecute",
                "InstNotReady",
                "ROBFull",
                "RegFull",
            ):
                entry = "backend_resource_or_dependency"
            else:
                entry = "inspect_dispatch_label"
            routes.append({**bin_, "next_entry": entry})
    candidate = blockers[0] if blockers else None
    fetch_rank = next(iter(fetch.get("rankings", [])), None)
    no_stall = dispatch["no_stall"]["fraction"] if valid_dispatch else None
    if candidate and no_stall is not None and no_stall >= 0.9:
        start = {
            "next_entry": "commit_blocker",
            "basis": "IEW NoStall covers at least 90% of sampled slots",
        }
    elif routes:
        start = {
            "next_entry": routes[0]["next_entry"],
            "basis": "largest observed non-NoStall IEW slot bin",
        }
    elif candidate:
        start = {
            "next_entry": "commit_blocker",
            "basis": "observed zero-commit blocker ranking",
        }
    elif fetch_rank:
        start = {
            "next_entry": "fetch_ftq_supply",
            "basis": "observed partial Fetch transfer anchor",
        }
    elif symptoms:
        start = {
            "next_entry": "backend_symptom",
            "basis": "observed committed instruction stage ranking",
        }
    else:
        start = {
            "next_entry": "insufficient_observations",
            "basis": "no dispatch or commit candidate",
        }
    return {
        "dispatch_status": dispatch["status"],
        "dispatch_routes": routes,
        "no_stall_fraction": no_stall,
        "suggested_start": start,
        "heuristic_scope": (
            "Suggested start orders investigations only; the 90% NoStall "
            "rule is a heuristic, not a bottleneck or performance-gain threshold. "
            "Other dispatch, commit and Fetch entries remain candidates"
        ),
        "commit_blocker_candidate": (
            {
                "PC": candidate["PC"],
                "observed_ticks": candidate["observed_ticks"],
            }
            if candidate
            else None
        ),
        "fetch_anchor_status": fetch["status"],
        "fetch_anchor_candidate": (
            {
                "AnchorPC": fetch_rank["AnchorPC"],
                "eligible_empty_slots": fetch_rank["eligible_empty_slots"],
            }
            if fetch_rank
            else None
        ),
        "evidence_ladder": {
            "pc_candidate": (
                "observed_rankings"
                if candidate or routes or symptoms or fetch.get("rankings")
                else "none_observed"
            ),
            "instance_mechanism": (
                "review_bounded_events_and_wait_overlap"
                if candidate
                else "not_assessed"
            ),
            "intervention_support": "not_measured_by_diagnose",
        },
        "scope": (
            "Dispatch routes are ordered only within IEW slot samples; "
            "compare them with commit and Fetch evidence without adding scores"
        ),
    }


def resolve_window(args):
    if args.stats:
        if args.start is not None or args.end is not None:
            raise ValueError("--stats cannot be combined with --start/--end")
        return stats_roi(args.stats)
    if args.start is None or args.end is None or args.start >= args.end:
        raise ValueError("diagnose requires --stats or both --start < --end")
    return args.start, args.end


STAGES = {
    "iq_to_fu": ("AtIssueQue", "AtFU"),
    "execution_or_memory": ("AtFU", "AtWriteVal"),
    "rob_drain": ("AtWriteVal", "AtCommit"),
}
FAILURES = "('group_not_ready','commit_head_blocked')"
WAIT_WAKE_SOURCES = {
    "stlf": {"stlf"},
    "translation": {"translation_observed_complete"},
    "cache_admission": {"cache_retry"},
    "cache_refill": {"cache_hint", "cache_response"},
}


def rows(db, sql, params):
    return [dict(row) for row in db.execute(sql, params)]


def selection_audit(population, rankings, value_key, unit, requested_top):
    """Audit output truncation within one ranking's own observation unit."""
    total = population["total_observed_units"]
    returned = sum(rank[value_key] for rank in rankings)
    return {
        "status": "available",
        "requested_top": requested_top,
        "population_ranked_pcs": population["ranked_pcs"],
        "returned_ranked_pcs": len(rankings),
        "omitted_ranked_pcs": population["ranked_pcs"] - len(rankings),
        "total_observed_units": total,
        "returned_observed_units": returned,
        "omitted_observed_units": total - returned,
        "returned_fraction_of_ranked_units": returned / total
        if total
        else None,
        "unit": unit,
        "scope": (
            "All qualifying ROI rows are aggregated before Top N output selection; "
            "coverage of this ranking is not root-cause confidence or recoverable time"
        ),
    }


def select_rankings(
    db, cte, grouped_sql, params, value_key, unit, pc_key="PC"
):
    """Calculate population totals before LIMIT without another ROI scan."""
    prefix = cte + "," if cte else "WITH"
    selected = rows(
        db,
        prefix
        + f""" selection_groups AS ({grouped_sql})
        SELECT *,count(*) OVER () AS _ranked_pcs,
            sum({value_key}) OVER () AS _total_units FROM selection_groups
        ORDER BY {value_key} DESC,{pc_key} LIMIT :top""",
        params,
    )
    population = {
        "ranked_pcs": selected[0]["_ranked_pcs"] if selected else 0,
        "total_observed_units": selected[0]["_total_units"] if selected else 0,
    }
    for rank in selected:
        del rank["_ranked_pcs"], rank["_total_units"]
    return selected, selection_audit(
        population, selected, value_key, unit, params["top"]
    )


def identity(row):
    return {key: row[key] for key in ("Cpu", "TID", "SeqNum")}


def lifetime_cte(columns):
    times = ["AtIssueQue", "AtFU", "AtWriteVal", "AtCommit"]
    fields = ",".join(
        f"l.{name}" if name in columns else f"NULL AS {name}" for name in times
    )
    nonmonotonic = " OR ".join(
        f"({a}>0 AND {b}>0 AND {a}>{b})"
        for i, a in enumerate(times)
        for b in times[i + 1 :]
    )
    components = []
    for name, (start, end) in STAGES.items():
        components.append(
            f"CASE WHEN nonmonotonic=0 AND {start}>0 AND {end}>0 "
            f"THEN max(0,min({end},:end)-max({start},:start)) "
            f"ELSE 0 END AS {name}"
        )
    return f"""WITH raw AS (
        SELECT i.*,l.ID AS lifetime_id,{fields}
        FROM PerfCCTInst i LEFT JOIN LifeTimeCommitTrace l ON l.ID=i.CommitID
        WHERE i.Cpu=:cpu AND i.TID=:tid AND i.EndKind='commit'
          AND i.EndTick>=:start AND i.EndTick<:end
    ), checked AS (
        SELECT *,CASE WHEN {nonmonotonic} THEN 1 ELSE 0 END AS nonmonotonic
        FROM raw
    ), scored AS (
        SELECT *,{','.join(components)} FROM checked
    ), symptoms AS (
        SELECT *,{' + '.join(STAGES)} AS score FROM scored
    ) """


def blocker_cte(kind="zero"):
    # Running maximum handles nested overlapping intervals, unlike lag(EndTick).
    predicate = (
        f"s.Committed=0 AND s.Reason IN {FAILURES}"
        if kind == "zero"
        else "s.Committed>0 AND s.Reason='group_not_ready'"
    )
    return f"""WITH spans AS (
        SELECT s.*,i.PC,max(s.StartTick,:start) AS lo,
            min(s.EndTick,:end) AS hi
        FROM PerfCCTCommitSpan s LEFT JOIN PerfCCTInst i
          ON i.Cpu=s.Cpu AND i.TID=s.TID AND i.SeqNum=s.BlockerSeq
        WHERE s.Cpu=:cpu AND s.TID=:tid AND {predicate} AND s.StartTick<:end
          AND s.EndTick>:start AND s.EndTick>s.StartTick
    ), previous AS (
        SELECT *,max(hi) OVER (PARTITION BY PC ORDER BY lo,hi,ID
            ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING) AS prev_hi
        FROM spans
    ), contributions AS (
        SELECT *,max(0,hi-max(lo,coalesce(prev_hi,lo))) AS union_ticks,
            row_number() OVER (PARTITION BY PC ORDER BY hi-lo DESC,ID) AS sample_rank
        FROM previous
    ) """


def blocker_duration_samples(db, params, rankings, kind="zero"):
    """Pick typical and tail spans for the already ranked blocker PCs."""
    if not rankings:
        return {}
    sample_params = dict(params)
    pc_terms = []
    for index, rank in enumerate(rankings):
        name = f"sample_pc_{index}"
        sample_params[name] = rank["PC"]
        pc_terms.append(f"PC IS :{name}")
    sql = (
        blocker_cte(kind)
        + f""", ordered AS (
        SELECT PC,ID,hi-lo AS duration_ticks,
            row_number() OVER (PARTITION BY PC ORDER BY hi-lo,ID) AS rn,
            count(*) OVER (PARTITION BY PC) AS n
        FROM spans WHERE {' OR '.join(pc_terms)}
    ), labeled AS (
        SELECT *,
            max(CASE WHEN rn=(n+1)/2 THEN duration_ticks END)
                OVER (PARTITION BY PC) AS p50_ticks,
            max(CASE WHEN rn=(95*n+99)/100 THEN duration_ticks END)
                OVER (PARTITION BY PC) AS p95_ticks
        FROM ordered
    ) SELECT PC,
        max(CASE WHEN rn=(n+1)/2 THEN ID END) AS p50_span_id,
        max(p50_ticks) AS p50_ticks,
        max(CASE WHEN rn=(95*n+99)/100 THEN ID END) AS p95_span_id,
        max(CASE WHEN rn=n THEN ID END) AS max_span_id,
        max(p95_ticks) AS p95_ticks,
        max(duration_ticks) AS max_ticks,
        count(*) AS total_spans,
        sum(duration_ticks) AS total_span_ticks,
        sum(duration_ticks<=p50_ticks) AS short_spans,
        sum(CASE WHEN duration_ticks<=p50_ticks THEN duration_ticks ELSE 0 END)
            AS short_ticks,
        sum(duration_ticks>p50_ticks AND duration_ticks<=p95_ticks)
            AS middle_spans,
        sum(CASE WHEN duration_ticks>p50_ticks AND duration_ticks<=p95_ticks
            THEN duration_ticks ELSE 0 END) AS middle_ticks,
        sum(duration_ticks>p95_ticks) AS long_spans,
        sum(CASE WHEN duration_ticks>p95_ticks THEN duration_ticks ELSE 0 END)
            AS long_ticks
    FROM labeled GROUP BY PC"""
    )
    return {row["PC"]: row for row in rows(db, sql, sample_params)}


def adjacent_commit_chain(db, params, span_id):
    """Show contiguous commit decisions and observed dependencies near a span."""
    select = """SELECT s.ID,s.StartTick,s.EndTick,s.HeadSeq,s.BlockerSeq,
        s.Reason,s.Committed,i.PC,i.Disasm
        FROM PerfCCTCommitSpan s LEFT JOIN PerfCCTInst i
          ON i.Cpu=s.Cpu AND i.TID=s.TID AND i.SeqNum=s.BlockerSeq
        WHERE s.Cpu=:cpu AND s.TID=:tid
          AND s.ID BETWEEN :id-3 AND :id+3 ORDER BY s.ID"""
    query_params = dict(params, id=span_id)
    context = rows(db, select, query_params)
    index = next(i for i, span in enumerate(context) if span["ID"] == span_id)
    lo, hi = index, index
    while lo > 0 and context[lo - 1]["EndTick"] == context[lo]["StartTick"]:
        lo -= 1
    while (
        hi + 1 < len(context)
        and context[hi]["EndTick"] == context[hi + 1]["StartTick"]
    ):
        hi += 1
    selected = context[lo : hi + 1]
    for offset, span in enumerate(selected, lo - index):
        span["relative_position"] = offset
        span["intersects_roi"] = (
            span["StartTick"] < params["end"]
            and span["EndTick"] is not None
            and span["EndTick"] > params["start"]
        )
    seqs = {span["BlockerSeq"] for span in selected if span["BlockerSeq"]}
    edges = []
    for seq in sorted(seqs):
        dependencies = rows(
            db,
            """SELECT ID,Tick,SeqNum,RelatedSeq,Detail
            FROM PerfCCTEvent WHERE Cpu=:cpu AND TID=:tid AND SeqNum=:seq
            AND Event='dependency' ORDER BY Tick,ID""",
            dict(params, seq=seq),
        )
        edges.extend(
            {
                "event_id": event["ID"],
                "tick": event["Tick"],
                "producer_seq": event["RelatedSeq"],
                "consumer_seq": event["SeqNum"],
                "detail": event["Detail"],
            }
            for event in dependencies
            if event["RelatedSeq"] in seqs
        )
    return {
        "spans": selected,
        "dependency_edges": edges,
        "scope": "Contiguous decisions within span ID +/-3; edges are observed dependencies",
    }


def paired_wait_overlap(db, key, lo, hi):
    """Intersect observed wait_begin/wake pairs with one blocker span."""
    events = rows(
        db,
        """SELECT ID,Tick,Attempt,Event,Detail,RelatedSeq
        FROM PerfCCTEvent WHERE Cpu=:Cpu AND TID=:TID AND SeqNum=:SeqNum
        AND Event IN ('wait_begin','wake') ORDER BY Tick,ID""",
        key,
    )
    opened, intervals = {}, []
    unmatched_wakes = begins = superseded = 0
    for event in events:
        attempt = event["Attempt"]
        if event["Event"] == "wait_begin":
            token = (attempt, event["Detail"])
            begins += 1
            superseded += token in opened
            # Repeated observations do not establish distinct wait lifetimes.
            # Retain the latest anchor; superseded begins remain unmatched.
            opened[token] = event
            continue
        token = next(
            (
                token
                for token, begin in opened.items()
                if token[0] == attempt
                and event["Detail"]
                in WAIT_WAKE_SOURCES.get(begin["Detail"], set())
                and (
                    begin["Detail"] != "stlf"
                    or (
                        begin["RelatedSeq"]
                        and begin["RelatedSeq"] == event["RelatedSeq"]
                    )
                )
            ),
            None,
        )
        if token is None:
            unmatched_wakes += 1
            continue
        begin = opened.pop(token)
        intervals.append(
            {
                "begin_tick": begin["Tick"],
                "end_tick": event["Tick"],
                "begin_id": begin["ID"],
                "wake_id": event["ID"],
                "reason": begin["Detail"],
                "related_seq": begin["RelatedSeq"],
                "wake_source": event["Detail"],
                "overlap_ticks": max(
                    0, min(event["Tick"], hi) - max(begin["Tick"], lo)
                ),
            }
        )
    clipped = sorted(
        (max(lo, span["begin_tick"]), min(hi, span["end_tick"]))
        for span in intervals
        if span["overlap_ticks"] > 0
    )
    merged = []
    for begin, end in clipped:
        if merged and begin <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(end, merged[-1][1]))
        else:
            merged.append((begin, end))
    overlap = sum(end - begin for begin, end in merged)
    relevant = [span for span in intervals if span["overlap_ticks"] > 0]
    return {
        "paired_intervals": len(intervals),
        "unmatched_begins": begins - len(intervals),
        "superseded_begins": superseded,
        "open_begins": len(opened),
        "unmatched_wakes": unmatched_wakes,
        "overlapping_intervals": len(relevant),
        "overlap_ticks_union": overlap,
        "ticks_outside_observed_paired_wait": hi - lo - overlap,
        "examples": relevant[:4],
        "examples_omitted": max(0, len(relevant) - 4),
        "pairing_semantics": (
            "Same attempt and compatible wake source; STLF requires the same known store. "
            "Repeated same-reason begins retain only the latest anchor; earlier observations "
            "remain unmatched. This does not establish the original wait start or continuity"
        ),
        "scope": "Observed wait intervals only; overlap is not recoverable time",
    }


def blocker_evidence_sample(db, params, pc, population, limit=64):
    """Time-spread screen of replay timing and paired waits for one blocker PC."""
    sql = (
        blocker_cte()
        + """, ordered AS (
        SELECT ID,BlockerSeq,lo,hi,
            row_number() OVER (ORDER BY lo,ID) AS rn,
            count(*) OVER () AS n
        FROM spans WHERE PC IS :pc
    ) SELECT ID,BlockerSeq,lo,hi FROM ordered
      WHERE ((rn-1)*:sample_limit)/n != (rn*:sample_limit)/n
      ORDER BY rn"""
    )
    sampled = rows(db, sql, dict(params, pc=pc, sample_limit=limit))
    reasons = {}
    wait_spans = no_timed_evidence = span_ticks = wait_ticks = 0
    for span in sampled:
        key = {
            "Cpu": params["cpu"],
            "TID": params["tid"],
            "SeqNum": span["BlockerSeq"],
        }
        replay = rows(
            db,
            """SELECT Detail,count(*) AS lifetime_count,
            sum(Tick>=:lo AND Tick<:hi) AS within_span_count
            FROM PerfCCTEvent WHERE Cpu=:Cpu AND TID=:TID AND SeqNum=:SeqNum
            AND Event='replay' GROUP BY Detail""",
            dict(key, lo=span["lo"], hi=span["hi"]),
        )
        aligned_replay = False
        for row in replay:
            counts = reasons.setdefault(
                row["Detail"], {"lifetime_spans": 0, "within_span_spans": 0}
            )
            counts["lifetime_spans"] += 1
            if row["within_span_count"]:
                counts["within_span_spans"] += 1
                aligned_replay = True
        wait = paired_wait_overlap(db, key, span["lo"], span["hi"])
        overlap = wait["overlap_ticks_union"]
        wait_spans += overlap > 0
        no_timed_evidence += not aligned_replay and overlap == 0
        span_ticks += span["hi"] - span["lo"]
        wait_ticks += overlap
    return {
        "population_spans": population,
        "sampled_spans": len(sampled),
        "unsampled_spans": population - len(sampled),
        "selection": f"Up to {limit} evenly spaced spans by start tick; not random",
        "replay_reasons": reasons,
        "spans_with_paired_wait_overlap": wait_spans,
        "spans_without_time_aligned_replay_or_paired_wait": no_timed_evidence,
        "sampled_summed_span_ticks": span_ticks,
        "paired_wait_overlap_ticks_summed": wait_ticks,
        "ticks_outside_observed_paired_wait": span_ticks - wait_ticks,
        "scope": (
            "Sample counts are not population estimates; paired wait overlap "
            "does not establish causality or recoverable time"
        ),
    }


def fetch_transfer_context(db, params):
    available = (
        db.execute(
            "SELECT 1 FROM sqlite_master WHERE type='table' "
            "AND name='PerfCCTFetchTransfer'"
        ).fetchone()
        is not None
    )
    if not available:
        return {
            "status": "unavailable_old_db",
            "rankings": [],
            "scope": "single-thread partial Decode transfers only",
        }
    where = "Cpu=:cpu AND TID=:tid AND Tick>=:start AND Tick<:end"
    coverage = rows(
        db,
        f"""SELECT count(*) AS all_samples,
        coalesce(sum(EmptySlots),0) AS all_empty_slots,
        coalesce(sum(TopdownEligible),0) AS eligible_samples,
        coalesce(sum(CASE WHEN TopdownEligible THEN EmptySlots ELSE 0 END),0)
            AS eligible_empty_slots
        FROM PerfCCTFetchTransfer WHERE {where}""",
        params,
    )[0]
    boundary = rows(
        db,
        """SELECT Tick,coalesce(sum(EmptySlots),0) AS slots
        FROM PerfCCTFetchTransfer WHERE Cpu=:cpu AND TID=:tid
        AND Tick IN (:start,:end) AND TopdownEligible=1
        GROUP BY Tick""",
        params,
    )
    boundary_slots = {row["Tick"]: row["slots"] for row in boundary}
    rankings, audit = select_rankings(
        db,
        "",
        f"""SELECT AnchorPC,
        count(*) AS samples, sum(EmptySlots) AS empty_slots,
        sum(TopdownEligible) AS eligible_samples,
        sum(CASE WHEN TopdownEligible THEN EmptySlots ELSE 0 END)
            AS eligible_empty_slots
        FROM PerfCCTFetchTransfer WHERE {where}
        GROUP BY AnchorPC HAVING eligible_empty_slots>0""",
        params,
        "eligible_empty_slots",
        "Fetch Topdown-eligible partial-transfer empty slots",
        "AnchorPC",
    )
    for rank in rankings:
        rank["sample"] = rows(
            db,
            f"""SELECT Tick,AnchorSeq,FTQID,EmptySlots
            FROM PerfCCTFetchTransfer WHERE {where}
            AND AnchorPC=:pc AND TopdownEligible=1
            ORDER BY EmptySlots DESC,Tick,AnchorSeq LIMIT 1""",
            dict(params, pc=rank["AnchorPC"]),
        )[0]
    return {
        "status": (
            "available"
            if coverage["all_samples"]
            else "no_samples_or_smt_unsupported"
        ),
        "coverage": coverage,
        "rankings": rankings,
        "selection_audit": audit,
        "left_boundary_eligible_slots": boundary_slots.get(params["start"], 0),
        "right_boundary_eligible_slots": boundary_slots.get(params["end"], 0),
        "scope": (
            "single-thread partial Decode transfers; last-sent PC is an "
            "anchor, not an empty-slot cause"
        ),
        "ranking_unit": "Topdown-eligible empty slots grouped by last-sent instruction PC",
        "smt_scope": "not recorded when CPU has multiple threads",
    }


def fetch_counter_check(transfer, topdown):
    if (
        transfer["status"] != "available"
        or topdown["status"] == "unavailable_without_stats"
    ):
        return {"status": "unavailable"}
    counters = topdown["fetch_counters"]
    bubbles = counters["fetch.fetchBubbles"]
    empty_cycles = counters["fetch.fetchBubbles_max"]
    width = topdown["decode_width_from_histogram_bins"]
    if None in (bubbles, empty_cycles, width):
        return {"status": "missing_stats_counter"}
    expected = bubbles - width * empty_cycles
    actual = transfer["coverage"]["eligible_empty_slots"]
    left_boundary = transfer["left_boundary_eligible_slots"]
    right_boundary = transfer["right_boundary_eligible_slots"]
    status = (
        "exact"
        if expected == actual
        else "consistent_with_stats_boundary_shift"
        if expected - actual == right_boundary - left_boundary
        else "mismatch"
    )
    return {
        "status": status,
        "expected_partial_eligible_slots": expected,
        "observed_partial_eligible_slots": actual,
        "difference": actual - expected,
        "left_boundary_eligible_slots": left_boundary,
        "right_boundary_eligible_slots": right_boundary,
        "scope": (
            "same Fetch-side partial-slot condition; matching the "
            "boundary samples does not prove all reset or pipeline "
            "boundary behavior"
        ),
    }


def local_evidence(db, key, args):
    params = dict(key, start=args.start, end=args.end, limit=args.limit + 1)
    match = "Cpu=:Cpu AND TID=:TID AND SeqNum=:SeqNum"
    inst = rows(db, "SELECT * FROM PerfCCTInst WHERE " + match, params)
    result = {
        "identity": key,
        "instruction": inst[0] if inst else None,
        "seed_window": {"start_tick": args.start, "end_tick": args.end},
    }
    prefix = "SELECT * FROM PerfCCTEvent WHERE " + match
    # Reserve only a small context budget; old replay traffic must never
    # consume the window's event budget. All four groups share one limit.
    anchor_budget = min(2, args.limit // 6)
    dependency_budget = min(4, args.limit // 6)
    specs = {
        "before_anchor": (
            "Tick<:start AND Event!='dependency'",
            "Tick DESC,ID DESC",
            anchor_budget,
        ),
        "after_anchor": (
            "Tick>=:end AND Event!='dependency'",
            "Tick,ID",
            anchor_budget,
        ),
        "dependency_context": (
            "Tick<:end AND Event='dependency'",
            "Tick,ID",
            dependency_budget,
        ),
    }
    groups, omitted = {}, {}
    for name, (condition, order, budget) in specs.items():
        found = rows(
            db,
            prefix
            + " AND "
            + condition
            + " ORDER BY "
            + order
            + " LIMIT :limit",
            dict(params, limit=budget + 1),
        )
        groups[name] = found[:budget]
        omitted[name] = len(found) > budget
    context = {
        event["ID"]: event for events in groups.values() for event in events
    }
    available = args.limit - len(context)
    found = rows(
        db,
        prefix
        + " AND Tick>=:start AND Tick<:end ORDER BY Tick,ID LIMIT :limit",
        params,
    )
    groups["window"] = found[:available]
    omitted["window"] = len(found) > available
    selected = dict(context)
    selected.update((event["ID"], event) for event in groups["window"])
    result.update(
        events=sorted(
            selected.values(), key=lambda event: (event["Tick"], event["ID"])
        ),
        events_truncated=any(omitted.values()),
        omitted=omitted,
        event_groups={
            name: [event["ID"] for event in events]
            for name, events in groups.items()
        },
        context_scope="bounded adjacent anchors and independent rename dependencies; not full history",
    )
    result["window_query_argv"] = [
        "python3",
        "util/perfcct_query.py",
        args.db,
        "inst",
        str(key["SeqNum"]),
        "--cpu",
        key["Cpu"],
        "--tid",
        str(key["TID"]),
        "--start",
        str(args.start),
        "--end",
        str(args.end),
        "--limit",
        str(args.limit),
    ]
    if omitted["window"]:
        last = groups["window"][-1]
        result["next_window_page"] = {
            "sql": prefix + " AND Tick>=:start AND Tick<:end "
            "AND (Tick>:after_tick OR (Tick=:after_tick AND ID>:after_id)) "
            "ORDER BY Tick,ID LIMIT :limit",
            "parameters": dict(
                params,
                limit=args.limit,
                after_tick=last["Tick"],
                after_id=last["ID"],
            ),
        }
    else:
        result["next_window_page"] = None
    return result


def diagnose(db, args, result, cache_reader, request_mapper):
    start, end = resolve_window(args)
    args = SimpleNamespace(**dict(vars(args), start=start, end=end))
    result["query_window"] = {"start_tick": start, "end_tick": end}
    result["stats_source"] = args.stats
    result["topdown"] = stats_context(args.stats, args.cpu)
    result["dispatch_stall_reasons"] = dispatch_stall_context(
        args.stats, args.cpu, args.top
    )
    params = {
        "cpu": args.cpu,
        "tid": args.tid,
        "start": args.start,
        "end": args.end,
        "top": args.top,
        "limit": args.limit + 1,
    }
    result["fetch_partial_transfers"] = fetch_transfer_context(db, params)
    result["fetch_partial_transfers"]["counter_check"] = fetch_counter_check(
        result["fetch_partial_transfers"], result["topdown"]
    )
    meta = result["metadata"]
    has_load_table = (
        db.execute(
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name='LoadLifeTimeCommitTrace'"
        ).fetchone()
        is not None
    )
    result["scope"] = {
        "Cpu": args.cpu,
        "TID": args.tid,
        "symptom_population": "instructions committed in [start,end)",
        "symptom_unit": "summed instruction_ticks; not runtime contribution",
        "blocker_unit": "union of observed zero-commit failure ticks per PC",
        "critical_path": "not_computed",
    }
    bounds = {}
    for bound in ("start", "end"):
        value = meta.get(f"cpu.{args.cpu}.{bound}_tick")
        try:
            bounds[bound + "_tick"] = int(value)
        except (ValueError, TypeError):
            bounds[bound + "_tick"] = None
    result["trace_bounds"] = bounds
    result["roi_outside_known_trace"] = (
        bounds["start_tick"] is not None and args.start < bounds["start_tick"]
    ) or (bounds["end_tick"] is not None and args.end > bounds["end_tick"])
    columns = {
        row["name"]
        for row in rows(db, "PRAGMA table_info(LifeTimeCommitTrace)", {})
    }
    result.update(
        symptom_rankings=[],
        commit_blocker_rankings=[],
        partial_commit_rankings=[],
        examples=[],
    )
    result["selection_audits"] = {}
    cte = None
    if "ID" in columns:
        cte = lifetime_cte(columns)
        missing = ",".join(
            f"coalesce(sum(CASE WHEN {a} IS NULL OR {a}<=0 OR {b} IS NULL OR {b}<=0 "
            f"THEN 1 ELSE 0 END),0) AS {name}_missing"
            for name, (a, b) in STAGES.items()
        )
        result["lifetime_coverage"] = rows(
            db,
            cte
            + f"""SELECT
            count(*) AS committed_instances,
            coalesce(sum(lifetime_id IS NULL),0) AS missing_lifetimes,
            coalesce(sum(nonmonotonic),0) AS nonmonotonic_instances,
            {missing} FROM symptoms""",
            params,
        )[0]
        result["lifetime_coverage"]["missing_columns"] = sorted(
            {value for pair in STAGES.values() for value in pair} - columns
        )
        component_sums = ",".join(f"sum({name}) AS {name}" for name in STAGES)
        ranked_cte = (
            cte
            + """, ranked AS (
            SELECT PC,SeqNum,score,iq_to_fu,execution_or_memory,rob_drain,
                row_number() OVER (PARTITION BY PC ORDER BY score DESC,SeqNum) AS rn
            FROM symptoms
        )"""
        )
        result["symptom_rankings"], audit = select_rankings(
            db,
            ranked_cte,
            f"""SELECT PC,
            count(*) AS committed_instances,sum(score) AS instruction_ticks,
            max(CASE WHEN rn=1 THEN SeqNum END) AS representative_seq,
            {component_sums} FROM ranked GROUP BY PC HAVING sum(score)>0""",
            params,
            "instruction_ticks",
            "summed committed instruction_ticks",
        )
        result["selection_audits"]["symptom"] = audit
    else:
        result["lifetime_coverage"] = {"status": "lifetime_table_unavailable"}
        result["selection_audits"]["symptom"] = {
            "status": "lifetime_table_unavailable"
        }
    bcte = blocker_cte()
    result["commit_blocker_rankings"], audit = select_rankings(
        db,
        bcte,
        """SELECT PC,
        count(*) AS spans,count(DISTINCT nullif(BlockerSeq,0)) AS dynamic_blockers,
        sum(union_ticks) AS observed_ticks,
        max(CASE WHEN sample_rank=1 THEN ID END) AS representative_span_id,
        sum(EndKind!='closed') AS incomplete_spans
        FROM contributions GROUP BY PC""",
        params,
        "observed_ticks",
        "summed_per_pc zero-commit interval-union ticks",
    )
    # Keep the all-PC union distinct from the sum of per-PC unions.
    union = rows(
        db,
        bcte
        + """, global_previous AS (
        SELECT lo,hi,max(hi) OVER (ORDER BY lo,hi,ID
            ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING) AS prev_hi
        FROM spans
    ) SELECT coalesce(sum(max(0,hi-max(lo,coalesce(prev_hi,lo)))),0)
        AS ticks FROM global_previous""",
        params,
    )[0]["ticks"]
    audit["all_pc_interval_union_ticks"] = union
    audit["all_pc_interval_union_fraction_of_roi"] = union / (
        args.end - args.start
    )
    audit["unit_note"] = (
        "Summed per-PC unions may overlap across PCs; the separate all-PC union "
        "measures observed zero-commit failures, not recoverable execution time"
    )
    result["selection_audits"]["zero_commit"] = audit
    duration_samples = blocker_duration_samples(
        db, params, result["commit_blocker_rankings"]
    )
    for rank in result["commit_blocker_rankings"]:
        sample = duration_samples[rank["PC"]]
        rank["duration_samples"] = {
            label: {
                "ticks": sample[f"{label}_ticks"],
                "span_id": (
                    rank["representative_span_id"]
                    if label == "max"
                    else sample[f"{label}_span_id"]
                ),
            }
            for label in ("p50", "p95", "max")
        }
        rank["duration_cohorts"] = {
            "population_spans": sample["total_spans"],
            "summed_span_ticks": sample["total_span_ticks"],
            "bins": {
                label: {
                    "spans": sample[f"{label}_spans"],
                    "span_ticks": sample[f"{label}_ticks"],
                    "fraction_of_summed_span_ticks": sample[f"{label}_ticks"]
                    / sample["total_span_ticks"],
                }
                for label in ("short", "middle", "long")
            },
            "cutoffs": {
                "short_at_most_ticks": sample["p50_ticks"],
                "middle_at_most_ticks": sample["p95_ticks"],
            },
            "scope": "Clipped span durations; overlapping spans are summed, not unioned",
        }
    result["top_blocker_evidence_sample"] = (
        blocker_evidence_sample(
            db,
            params,
            result["commit_blocker_rankings"][0]["PC"],
            result["commit_blocker_rankings"][0]["spans"],
        )
        if result["commit_blocker_rankings"]
        else None
    )
    pcte = blocker_cte("partial")
    result["partial_commit_rankings"], audit = select_rankings(
        db,
        pcte,
        """SELECT PC,
        count(*) AS spans,
        count(DISTINCT nullif(BlockerSeq,0)) AS dynamic_blockers,
        sum(hi-lo) AS observed_ticks
        FROM spans GROUP BY PC""",
        params,
        "observed_ticks",
        "summed partial-commit group_not_ready span ticks",
    )
    result["selection_audits"]["partial_commit"] = audit
    result["selection_audits"]["fetch_partial"] = result[
        "fetch_partial_transfers"
    ].get(
        "selection_audit",
        {"status": result["fetch_partial_transfers"]["status"]},
    )
    partial_samples = blocker_duration_samples(
        db, params, result["partial_commit_rankings"][:1], "partial"
    )
    for rank in result["partial_commit_rankings"][:1]:
        sample = partial_samples[rank["PC"]]
        rank["duration_samples"] = {
            label: {
                "ticks": sample[f"{label}_ticks"],
                "span_id": sample[f"{label}_span_id"],
            }
            for label in ("p50", "p95", "max")
        }
    result["partial_commit_scope"] = (
        "group_not_ready after some instructions committed in the same cycle; "
        "observed_ticks identify candidates, not lost or recoverable cycles"
    )
    result["commit_coverage"] = rows(
        db,
        f"""SELECT count(*) AS selected_spans,
        coalesce(sum(EndTick IS NULL),0) AS unknown_end_spans,
        coalesce(sum(EndTick<=StartTick),0) AS nonpositive_spans
        FROM PerfCCTCommitSpan WHERE Cpu=:cpu AND TID=:tid AND Committed=0
        AND Reason IN {FAILURES} AND StartTick<:end
        AND (EndTick>=:start OR EndTick IS NULL)""",
        params,
    )[0]
    result["investigation_hints"] = investigation_hints(
        result["dispatch_stall_reasons"],
        result["commit_blocker_rankings"],
        result["fetch_partial_transfers"],
        result["symptom_rankings"],
    )

    # Representative identities plus typical/tail Top1 samples; no full event-table scan.
    cached = {}

    def evidence(key, start, end):
        token = (key["Cpu"], key["TID"], key["SeqNum"], start, end)
        if token not in cached:
            window_args = SimpleNamespace(
                **dict(vars(args), start=start, end=end)
            )
            cached[token] = local_evidence(db, key, window_args)
        return cached[token]

    def route(key, start, end, rob_first=False):
        target = evidence(key, start, end)
        route_result = {"identity": key, "critical_predecessor": "unknown"}
        if rob_first and end > start:
            overlap_params = dict(params, start=start, end=end)
            choices = rows(
                db,
                bcte
                + """SELECT BlockerSeq,HeadSeq,Reason,ID,
                lo,hi FROM spans ORDER BY hi-lo DESC,ID LIMIT 1""",
                overlap_params,
            )
            if choices:
                chosen = choices[0]
                next_key = dict(key, SeqNum=chosen["BlockerSeq"])
                route_result.update(
                    entry="commit_blocker",
                    observed_span=chosen,
                    blocker_identity=next_key,
                )
                start, end = chosen["lo"], chosen["hi"]
                target = evidence(next_key, start, end)
            else:
                route_result.update(
                    entry="commit_wait", unresolved="no_matching_failure_span"
                )
        observed = target["events"]
        kinds = {event["Event"] for event in observed}
        known_load = (
            has_load_table
            and target["instruction"]
            and db.execute(
                "SELECT 1 FROM LoadLifeTimeCommitTrace WHERE ID=? LIMIT 1",
                (target["instruction"]["CommitID"],),
            ).fetchone()
            is not None
        )
        store_observed = any(
            event["Event"] == "store_data_ready"
            or (
                event["Event"] == "attempt_begin"
                and event["Detail"] == "store_address"
            )
            for event in observed
        )
        store_role = any(
            event["Event"] == "dependency"
            and "role=store_address" in event["Detail"].split(";")
            for event in observed
        )
        load_attempt = any(
            event["Event"] == "attempt_begin"
            and event["Detail"]
            in ("issue_queue", "fast_replay", "replay_queue")
            for event in observed
        )
        is_load = bool(
            known_load or (load_attempt and not (store_observed or store_role))
        )
        if is_load:
            next_entry = "load_replay_cache"
        elif store_observed or store_role:
            next_entry = "store_translation_or_completion"
        elif kinds & {"attempt_begin", "replay", "wait_begin", "response"}:
            next_entry = "memory_wait_observed"
        else:
            next_entry = (
                "iq_schedule_dependencies"
                if any(k.startswith("iq_") for k in kinds)
                else "insufficient_observations"
            )
        route_result["next_entry"] = next_entry
        route_result["evidence"] = target
        # Existing cache reader uses exact requestor/context mapping and LIMIT.
        if is_load and target["instruction"]:
            cache_args = SimpleNamespace(
                **dict(
                    vars(args),
                    start=start,
                    end=end,
                    limit=max(1, args.limit // 2),
                )
            )
            cache = cache_reader(db, target["instruction"], cache_args, meta)
            route_result["cache"] = cache
            remaining = args.limit - len(cache["events"])
            rejects = {
                e["ID"] for e in cache["events"] if e["Event"] == "reject"
            }
            cache["rejection_owners"] = []
            for reject_id in sorted(rejects):
                owners = rows(
                    db,
                    "SELECT * FROM PerfCCTCacheEvent WHERE ParentID=:id "
                    "ORDER BY Tick,ID LIMIT :limit",
                    {"id": reject_id, "limit": remaining + 1},
                )
                selected = owners[:remaining]
                cache["rejection_owners"].append(
                    {
                        "reject_id": reject_id,
                        "events": [
                            dict(
                                e, request_identity=request_mapper(db, e, meta)
                            )
                            for e in selected
                        ],
                        "events_truncated": len(owners) > remaining,
                    }
                )
                remaining -= len(selected)
            cache[
                "context_scope"
            ] = "Seed-window request events and ParentID-bound owner snapshots only"
        producer_events = [
            e
            for e in observed
            if e["RelatedSeq"]
            and (
                e["Event"] == "dependency"
                or (e["Event"] == "wait_begin" and e["Detail"] == "stlf")
            )
        ]
        route_result["predecessor_candidates"] = []
        seen = set()
        for event in producer_events:
            seq = event["RelatedSeq"]
            if seq in seen:
                continue
            seen.add(seq)
            if len(route_result["predecessor_candidates"]) == 2:
                break
            pred = dict(target["identity"], SeqNum=seq)
            route_result["predecessor_candidates"].append(
                {
                    "relation_event": event,
                    "identity": pred,
                    "instruction": rows(
                        db,
                        "SELECT * FROM PerfCCTInst WHERE Cpu=:Cpu "
                        "AND TID=:TID AND SeqNum=:SeqNum",
                        pred,
                    ),
                }
            )
        candidate_count = len({e["RelatedSeq"] for e in producer_events})
        route_result["candidate_count_in_returned_events"] = candidate_count
        route_result["candidates_truncated"] = candidate_count > 2
        route_result["candidate_coverage"] = "bounded_observed_relations_only"
        return route_result

    for rank in result["symptom_rankings"]:
        sample = rows(
            db,
            cte + "SELECT * FROM symptoms WHERE SeqNum=:seq",
            dict(params, seq=rank["representative_seq"]),
        )[0]
        key = identity(sample)
        largest = max(STAGES, key=lambda name: sample[name])
        start, end = STAGES[largest]
        rank["representative"] = key
        result["examples"].append(
            {
                "source": "symptom",
                "PC": rank["PC"],
                "largest_fragment": largest,
                "instruction_ticks": sample["score"],
                "stage_times": {
                    name: sample[name]
                    for pair in STAGES.values()
                    for name in pair
                },
                "route": route(
                    key,
                    max(sample[start], args.start),
                    min(sample[end], args.end),
                    largest == "rob_drain",
                ),
            }
        )

    def add_blocker_example(span_id, pc, selection, source="commit_blocker"):
        sample = rows(
            db,
            "SELECT *,max(StartTick,:start) AS lo,"
            "min(EndTick,:end) AS hi "
            "FROM PerfCCTCommitSpan WHERE ID=:id",
            dict(params, id=span_id),
        )[0]
        key = {
            "Cpu": args.cpu,
            "TID": args.tid,
            "SeqNum": sample["BlockerSeq"],
        }
        event_timing = rows(
            db,
            """SELECT Event,Detail,
            count(*) AS lifetime_count,
            sum(Tick>=:lo AND Tick<:hi) AS within_span_count
            FROM PerfCCTEvent WHERE Cpu=:cpu AND TID=:tid AND SeqNum=:seq
            AND Event IN ('replay','wait_begin','wake','response')
            GROUP BY Event,Detail ORDER BY Event,Detail""",
            dict(params, seq=key["SeqNum"], lo=sample["lo"], hi=sample["hi"]),
        )
        wait = paired_wait_overlap(db, key, sample["lo"], sample["hi"])
        if wait["overlap_ticks_union"]:
            instance_level = "paired_wait_overlaps_blocker"
        elif any(
            event["Event"] == "replay" and event["within_span_count"]
            for event in event_timing
        ):
            instance_level = "replay_timestamp_in_blocker"
        else:
            instance_level = "no_aligned_replay_or_paired_wait"
        result["examples"].append(
            {
                "source": source,
                "PC": pc,
                "selection": selection,
                "observed_span": sample,
                "instance_evidence_level": instance_level,
                "event_timestamp_counts": event_timing,
                "paired_wait_overlap": wait,
                "adjacent_commit_chain": adjacent_commit_chain(
                    db, params, span_id
                ),
                "route": route(key, sample["lo"], sample["hi"]),
            }
        )
        return key

    for index, rank in enumerate(result["commit_blocker_rankings"]):
        selection = "max_duration" if index == 0 else "p95_duration"
        if index:
            rank["representative_span_id"] = rank["duration_samples"]["p95"][
                "span_id"
            ]
        rank["representative_selection"] = selection
        rank["representative"] = add_blocker_example(
            rank["representative_span_id"], rank["PC"], selection
        )
        rank["requested_roi_fraction"] = rank["observed_ticks"] / (
            args.end - args.start
        )
        if index == 0:
            used_ids = {rank["representative_span_id"]}
            for label in ("p50", "p95"):
                span_id = rank["duration_samples"][label]["span_id"]
                if span_id in used_ids:
                    continue
                used_ids.add(span_id)
                add_blocker_example(span_id, rank["PC"], f"{label}_duration")
    for rank in result["partial_commit_rankings"][:1]:
        used_ids = set()
        for label in ("p50", "p95", "max"):
            span_id = rank["duration_samples"][label]["span_id"]
            if span_id in used_ids:
                continue
            used_ids.add(span_id)
            add_blocker_example(
                span_id,
                rank["PC"],
                f"{label}_duration",
                "partial_commit_blocker",
            )
    candidate = next(iter(result["commit_blocker_rankings"]), None)
    screen = result["top_blocker_evidence_sample"]
    result["investigation_hints"]["zero_commit_candidate_check"] = (
        {
            "spans": candidate["spans"],
            "observed_roi_fraction": candidate["requested_roi_fraction"],
            "repeatability": (
                "single_observed_span"
                if candidate["spans"] == 1
                else "multiple_observed_spans"
            ),
            "sampled_spans": screen["sampled_spans"],
            "sampled_spans_without_time_aligned_replay_or_paired_wait": screen[
                "spans_without_time_aligned_replay_or_paired_wait"
            ],
            "root_cause_status": "not_established",
            "evidence_gaps": [
                "Time-aligned evidence is local; unobserved or unsampled causes remain possible",
                "Intervention and performance-gain validation are not measured by this query",
            ],
            "scope": (
                "Observed coverage and repetition describe this candidate; "
                "a small fraction or a single span alone does not explain the global bottleneck"
            ),
        }
        if candidate
        else {"status": "no_observed_zero_commit_candidate"}
    )
    result["notes"] = [
        "Symptom score sums valid final-stage fragments across instructions; "
        "overlapping victims are not runtime costs.",
        "Final timestamps may overwrite earlier attempts; execution_or_memory is not cache-miss latency.",
        "Known stage order violations exclude the entire instance; missing endpoints exclude "
        "only that fragment.",
        "Zero timestamps are treated as missing, including genuinely occurring tick-zero transitions.",
        "Blocker ticks are a per-PC interval union; different-PC overlaps must not be summed "
        "as exclusive attribution.",
        "Blocker duration p50/p95 are unweighted span quantiles after ROI clipping; "
        "the max span is not necessarily a typical mechanism. The top-ranked zero-commit "
        "and partial-commit PCs get p50/p95/max routed examples; other zero-commit PCs use p95.",
        "Event timestamp counts describe occurrences inside each selected blocker span, "
        "not wait-interval overlap or causal delay.",
        "The top blocker evidence sample is time-spread and bounded; counts are not "
        "population estimates. Paired wait overlap does not imply recoverable time.",
        "Investigation hints prioritize queries using IEW slot labels and NoStall; "
        "they do not compare dispatch samples with blocker ticks.",
        "Partial-commit blocker time has a different population from zero-commit blocker time; "
        "the two rankings are not additive performance costs.",
        "A group blocker is the first observed failing member, not the exclusive critical instruction.",
        "ROB-drain routing selects a representative overlapping failure, not all causes across the wait.",
        "Events prioritize the seed window, plus bounded adjacent anchors and independent dependency context.",
        "Cache queries use the seed window; owners are snapshots, not proven culprits or complete lifecycle history.",
        "Half-open commit population excludes finalTick itself and need not equal stats committedOps/simInsts.",
        "Missing/truncated events do not establish readiness or absence of waiting; "
        "predecessors are candidates only.",
    ]
    return result
