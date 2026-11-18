#!/usr/bin/env python3
"""Correlate facts by identity and address, keeping demand and PF denominators separate."""

import collections

from parse_trace import require


def ratio(numerator, denominator):
    return numerator / denominator if denominator else None


def reconstruct(db):
    """Candidate aliases retain their own identities; they do not create extra sends."""
    duplicate = db.execute("""SELECT prefetch_id FROM events WHERE event='PF_CANDIDATE'
        GROUP BY prefetch_id HAVING count(*)>1 OR prefetch_id=0 LIMIT 1""").fetchone()
    require(duplicate is None, "Duplicate or missing candidate identity")
    db.executescript("""
        CREATE TABLE prefetches AS
        SELECT prefetch_id AS id,
            min(CASE WHEN event='PF_CANDIDATE' THEN tick END) AS candidate_tick,
            min(CASE WHEN event='PF_CANDIDATE' THEN "order" END) AS candidate_order,
            min(CASE WHEN event='PF_ENQUEUE' THEN tick END) AS enqueue_tick,
            min(CASE WHEN event IN ('PF_DEQUEUE','PF_FORWARD_DEQUEUE') THEN tick END) AS dequeue_tick,
            min(CASE WHEN event IN ('PF_SEND_ACCEPT','PF_SNOOP_ACCEPT') THEN tick END) AS send_tick,
            min(CASE WHEN event='PF_FILL' THEN tick END) AS fill_tick,
            min(CASE WHEN event IN ('PF_FILL','PF_FIRST_USE') THEN ready_tick END) AS ready_tick,
            min(CASE WHEN event='PF_FIRST_USE' THEN tick END) AS use_tick,
            min(CASE WHEN event='PF_EVICT' THEN tick END) AS evict_tick,
            max(CASE WHEN event='PF_CANDIDATE' THEN pc END) AS pc,
            max(CASE WHEN event='PF_CANDIDATE' THEN cache END) AS origin_cache,
            max(CASE WHEN event='PF_CANDIDATE' THEN level END) AS origin_level,
            max(CASE WHEN event='PF_CANDIDATE' THEN requestor_id END) AS requestor_id,
            max(CASE WHEN event='PF_CANDIDATE' THEN context_id END) AS context_id,
            max(CASE WHEN event='PF_CANDIDATE' THEN address_space END) AS address_space,
            max(CASE WHEN event='PF_CANDIDATE' THEN secure END) AS secure,
            max(CASE WHEN event='PF_CANDIDATE' THEN source END) AS source,
            max(CASE WHEN event='PF_CANDIDATE' THEN request_id END) AS trigger_id,
            max(CASE WHEN event='PF_CANDIDATE' THEN va_line END) AS va_line,
            NULL AS pa_line, NULL AS alias_id
        FROM events WHERE prefetch_id>0 GROUP BY prefetch_id;
        CREATE UNIQUE INDEX pf_id ON prefetches(id);
        UPDATE prefetches SET pa_line=(SELECT pa_line FROM events
            WHERE prefetch_id=prefetches.id AND pa_line IS NOT NULL
            ORDER BY tick,"order" LIMIT 1);
        UPDATE prefetches SET alias_id=(SELECT related_id FROM events
            WHERE prefetch_id=prefetches.id AND event='PF_ALIAS'
            ORDER BY tick,"order" LIMIT 1);
        CREATE INDEX pf_pa ON prefetches(pa_line,secure,requestor_id,candidate_tick);
        CREATE INDEX pf_va ON prefetches(va_line,secure,context_id,address_space,requestor_id,candidate_tick);
        CREATE TABLE demands AS
        SELECT a.*, (SELECT min(c.tick) FROM events c WHERE c.event='DEMAND_COMPLETE'
            AND c.cache=a.cache AND c.request_id=a.request_id AND c.tick>=a.tick) AS complete_tick,
            coalesce(a.arrival_tick, (SELECT min(tick) FROM events r
             WHERE r.event='DEMAND_ACCESS' AND r.cache=a.cache
             AND r.request_id=a.request_id)) AS first_tick,
            coalesce((SELECT min("order") FROM events r WHERE r.event='DEMAND_ARRIVE'
             AND r.cache=a.cache AND r.request_id=a.request_id),a."order") AS first_order,
            (SELECT count(*) FROM events r WHERE r.event='DEMAND_ACCESS'
             AND r.cache=a.cache AND r.request_id=a.request_id) AS attempts,
            (EXISTS(SELECT 1 FROM events t WHERE t.event='PF_TRAIN' AND t.level=1
             AND t.cache=a.cache AND t.request_id=a.request_id) OR
             (a.inst_seq IS NOT NULL AND a.address_space<>'' AND
              EXISTS(SELECT 1 FROM events t WHERE t.event='PF_TRAIN' AND t.level=1
               AND t.cache=a.cache AND t.inst_seq=a.inst_seq AND
               t.context_id IS a.context_id AND t.address_space=a.address_space
               AND t.pa_line=a.pa_line))) AS trained,
            NULL AS classification, NULL AS cause_stage, NULL AS matched_pf
        FROM events a WHERE a.event='DEMAND_ACCESS' AND a.result IN ('hit','miss');
        CREATE UNIQUE INDEX demand_id ON demands(cache,request_id);
        CREATE INDEX demand_pc ON demands(cache,context_id,address_space,secure,pc,tick);
        CREATE INDEX demand_line ON demands(cache,pa_line,secure,first_tick,first_order);
    """)
    # Same ID may travel through several cache levels, but an accepted L1
    # request has only one result. The index above rejects accidental double counting.
    db.commit()


def resolve_alias(db, pf):
    visited = set()
    while pf and pf["alias_id"]:
        require(pf["id"] not in visited, "Cycle in candidate aliases")
        visited.add(pf["id"])
        pf = db.execute("SELECT * FROM prefetches WHERE id=?", (pf["alias_id"],)).fetchone()
    return pf


def classify_miss(db, demand, roi_begin):
    """Use only facts preceding this demand; subsequent fills explain lateness."""
    tick, order = demand["first_tick"], demand["first_order"]
    if tick < roi_begin:
        return "left_censored", "arrival_before_roi", None
    merge = db.execute(
        """SELECT related_id FROM events WHERE event='DEMAND_MERGE'
        AND cache=? AND request_id=? AND tick<=? ORDER BY tick,"order" LIMIT 1""",
        (demand["cache"], demand["request_id"], demand["tick"]),
    ).fetchone()
    if merge and merge[0]:
        prediction = db.execute(
            "SELECT candidate_tick,candidate_order FROM prefetches WHERE id=?", (merge[0],)
        ).fetchone()
        if (
            prediction
            and prediction[0] is not None
            and (prediction[0], prediction[1]) >= (tick, order)
        ):
            return "no_prediction", "prediction_after_arrival", merge[0]
        return "late", "inflight_mshr", merge[0]
    candidates = db.execute(
        """SELECT * FROM prefetches WHERE
        candidate_tick IS NOT NULL AND (candidate_tick<? OR
        (candidate_tick=? AND candidate_order<?)) AND secure=?
        AND (origin_cache=? OR origin_level>1) AND
        (pa_line=? OR (pa_line IS NULL AND va_line=? AND context_id=?
                       AND address_space=? AND address_space<>''))
        ORDER BY candidate_tick DESC,candidate_order DESC""",
        (
            tick,
            tick,
            order,
            demand["secure"],
            demand["cache"],
            demand["pa_line"],
            demand["va_line"],
            demand["context_id"],
            demand["address_space"],
        ),
    ).fetchall()
    if not candidates:
        return "no_prediction", "no_candidate_in_observed_roi", None
    prior = db.execute(
        """SELECT first_tick AS tick,first_order AS event_order
        FROM demands WHERE cache=? AND pa_line=? AND secure=? AND
        (first_tick<? OR (first_tick=? AND first_order<?))
        ORDER BY first_tick DESC,first_order DESC LIMIT 1""",
        (demand["cache"], demand["pa_line"], demand["secure"], tick, tick, order),
    ).fetchone()
    evidence = []
    for candidate in candidates:
        pf = resolve_alias(db, candidate)
        if pf is None:
            evidence.append(("left_censored", "alias_outside_roi", candidate["id"]))
            continue
        pid = pf["id"]
        if candidate["alias_id"] and pf["pa_line"] and pf["pa_line"] != demand["pa_line"]:
            evidence.append(("dropped", "alias_different_physical_line", candidate["id"]))
            continue
        # Do not use an eviction/drop after the demand to explain the miss.
        evict = db.execute(
            """SELECT result,reason,tick,"order" AS event_order FROM events WHERE event='PF_EVICT'
            AND prefetch_id=? AND (tick<? OR (tick=? AND "order"<?))
            ORDER BY tick DESC,"order" DESC LIMIT 1""",
            (pid, tick, tick, order),
        ).fetchone()
        if evict:
            if prior and (evict["tick"], evict["event_order"]) <= (
                prior["tick"],
                prior["event_order"],
            ):
                continue
            if evict["result"] == "unused":
                category = "evicted_early" if evict["reason"] == "replacement" else "invalidated"
                evidence.append((category, evict["reason"], pid))
            # A used, evicted request belongs to a previous demand episode.
            continue
        drop = db.execute(
            """SELECT reason,tick,"order" AS event_order FROM events WHERE event='PF_DROP'
            AND prefetch_id=? AND (tick<? OR (tick=? AND "order"<?))
            ORDER BY tick DESC,"order" DESC LIMIT 1""",
            (pid, tick, tick, order),
        ).fetchone()
        if drop and pf["send_tick"] is None:
            if prior and (drop["tick"], drop["event_order"]) <= (
                prior["tick"],
                prior["event_order"],
            ):
                continue
            evidence.append(("dropped", drop["reason"], pid))
            continue
        if pf["ready_tick"] is not None and pf["ready_tick"] <= tick:
            fill = db.execute(
                """SELECT cache FROM events WHERE event='PF_FILL'
                AND prefetch_id=? ORDER BY tick,"order" LIMIT 1""",
                (pid,),
            ).fetchone()
            if fill[0] == demand["cache"]:
                return "resident_but_miss", "permissions_or_cache_state", pid
            return "lower_level_only", "filled_below_l1", pid
        if pf["send_tick"] is not None and pf["send_tick"] <= tick:
            return "late", "memory_or_fill", pid
        if pf["dequeue_tick"] is not None and pf["dequeue_tick"] <= tick:
            return "late", "send_backpressure_or_arbitration", pid
        if pf["enqueue_tick"] is not None and pf["enqueue_tick"] <= tick:
            return "late", "queue_or_translation", pid
        evidence.append(("late", "prediction_buffer", pid))
    return evidence[0] if evidence else ("no_prediction", "previous_prediction_already_used", None)


def analyze_demands(db, roi_begin):
    classifications, stages = collections.Counter(), collections.Counter()
    for demand in db.execute('SELECT * FROM demands ORDER BY tick,"order"'):
        if demand["result"] == "miss":
            category, stage, pid = classify_miss(db, demand, roi_begin)
            classifications[category] += 1
            stages[stage] += 1
        else:
            useful = db.execute(
                """SELECT prefetch_id,ready_tick FROM events WHERE event='PF_FIRST_USE'
                AND cache=? AND request_id=? AND result='hit' LIMIT 1""",
                (demand["cache"], demand["request_id"]),
            ).fetchone()
            category, stage, pid = "hit", "", None
            if useful:
                pid = useful["prefetch_id"]
                ready = useful["ready_tick"]
                if ready is None:
                    ready = db.execute(
                        "SELECT ready_tick FROM prefetches WHERE id=?", (pid,)
                    ).fetchone()[0]
                if ready is not None and ready > demand["first_tick"]:
                    category, stage = "late_prefetch_hit", "ready_after_arrival"
                else:
                    category, stage = "prefetch_hit", (
                        "timely" if ready is not None else "unknown_ready_time"
                    )
        db.execute(
            """UPDATE demands SET classification=?,cause_stage=?,matched_pf=?
            WHERE cache=? AND request_id=?""",
            (category, stage, pid, demand["cache"], demand["request_id"]),
        )
    db.commit()
    return dict(classifications), dict(stages)


def prefetch_metrics(db, block_size):
    counts = dict(db.execute("SELECT event,count(*) FROM events GROUP BY event"))
    scalar = lambda sql: db.execute(sql).fetchone()[0]
    sent = scalar("SELECT count(*) FROM prefetches WHERE send_tick IS NOT NULL")
    useful = scalar(
        "SELECT count(*) FROM prefetches WHERE send_tick IS NOT NULL AND use_tick IS NOT NULL"
    )
    timely = scalar("""SELECT count(*) FROM prefetches p WHERE send_tick IS NOT NULL
        AND EXISTS(SELECT 1 FROM events e WHERE e.prefetch_id=p.id
        AND e.event='PF_FIRST_USE' AND e.result='hit'
        AND coalesce(e.ready_tick,p.ready_tick)<=coalesce(e.arrival_tick,e.tick))""")
    unused = scalar("""SELECT count(DISTINCT prefetch_id) FROM events WHERE
        event='PF_EVICT' AND result='unused'""")
    censored = scalar("""SELECT count(*) FROM prefetches WHERE send_tick IS NOT NULL
        AND use_tick IS NULL AND evict_tick IS NULL""")
    return {
        "event_counts": counts,
        "candidates": counts.get("PF_CANDIDATE", 0),
        "sent_requests": sent,
        "useful_requests": useful,
        "timely_useful_requests": timely,
        "unused_evicted_requests": unused,
        "right_censored_sent_requests": censored,
        "left_censored_requests": scalar(
            "SELECT count(*) FROM prefetches WHERE candidate_tick IS NULL"
        ),
        "accuracy_observed": ratio(useful, sent),
        "timeliness": ratio(timely, useful),
        "prefetch_request_bytes": sent * block_size,
        "hop_send_bytes": counts.get("PF_SEND_ACCEPT", 0) * block_size,
        "traffic_by_cache": [
            dict(r)
            for r in db.execute(
                """SELECT cache,level,
            count(*) AS sends,count(*)*? AS bytes FROM events
            WHERE event='PF_SEND_ACCEPT' GROUP BY cache,level""",
                (block_size,),
            )
        ],
    }


def pc_analysis(db, weight):
    pcs = []
    query = """SELECT cache,context_id,address_space,secure,pc,count(*) AS accesses,
        sum(result='miss') AS misses,sum(trained) AS trained,
        sum(complete_tick IS NOT NULL) AS completed,
        avg(complete_tick-first_tick) AS mean_latency_ticks,
        sum(CASE WHEN result='miss' THEN complete_tick-first_tick ELSE 0 END) AS miss_latency_ticks
        FROM demands GROUP BY cache,context_id,address_space,secure,pc"""
    for group in db.execute(query):
        pc = dict(group)
        args = (
            group["cache"],
            group["context_id"],
            group["address_space"],
            group["secure"],
            group["pc"],
        )
        clause = "cache=? AND context_id IS ? AND address_space IS ? AND secure=? AND pc IS ?"
        sequence = db.execute(
            f"""SELECT vaddr,paddr FROM demands WHERE {clause}
            ORDER BY first_tick,first_order""",
            args,
        )
        deltas, previous = collections.Counter(), None
        for access in sequence:
            value = access["vaddr"] or access["paddr"]
            domain = "va" if access["vaddr"] else "pa"
            if value is not None:
                current = (domain, int(value, 0))
                if previous and previous[0] == domain:
                    deltas[current[1] - previous[1]] += 1
                previous = current
        pc["deltas"] = deltas.most_common(8)
        total = sum(deltas.values())
        dominant, frequency = deltas.most_common(1)[0] if deltas else (None, 0)
        pc["dominant_stride_bytes"] = dominant
        pc["stride_fraction"] = ratio(frequency, total)
        pc["pattern"] = (
            "stride"
            if dominant and frequency >= 0.75 * total
            else (
                "repeated_line"
                if dominant == 0 and frequency >= 0.75 * total
                else "irregular" if total else "insufficient_data"
            )
        )
        pc["failures"] = dict(
            db.execute(
                f"""SELECT classification,count(*) FROM demands
            WHERE {clause} AND result='miss' GROUP BY classification""",
                args,
            )
        )
        pc["stages"] = dict(
            db.execute(
                f"""SELECT cause_stage,count(*) FROM demands
            WHERE {clause} AND result='miss' GROUP BY cause_stage""",
                args,
            )
        )
        pc["trained_fraction"] = ratio(pc["trained"], pc["accesses"])
        pc["training_drops"] = dict(
            db.execute(
                f"SELECT reason,count(*) FROM events WHERE {clause} "
                "AND event='PF_TRAIN_DROP' AND level=1 GROUP BY reason",
                args,
            )
        )
        pc["weighted_miss_latency_ticks"] = (pc["miss_latency_ticks"] or 0) * weight
        main = max(pc["failures"], key=pc["failures"].get, default="")
        pc["suggestion"] = {
            "no_prediction": (
                "Check L1 training admission and stride confidence/detection."
                if pc["pattern"] == "stride"
                else "Check training admission and inspect the irregular address sequence."
            ),
            "dropped": "Inspect recorded drop reasons and queue/admission pressure.",
            "late": "Use the timeline to distinguish prediction, queue/send and memory delays.",
            "evicted_early": "Reduce prefetch distance or limit cache pressure; verify with an off baseline.",
            "invalidated": "Inspect coherence invalidations before changing prefetch distance.",
            "lower_level_only": "Prediction filled below L1; compare demand service latency and offload control.",
            "resident_but_miss": "Inspect permissions/cache state; a resident line did not satisfy this access.",
            "left_censored": "Extend the trace to include this prediction's generation.",
        }.get(main, "No residual miss diagnosis for this PC.")
        pcs.append(pc)
    return sorted(pcs, key=lambda p: (p["weighted_miss_latency_ticks"], p["misses"]), reverse=True)


def summarize(db, metadata, weight=1.0):
    reconstruct(db)
    roi_begin = max(
        int(metadata["start_tick"]),
        db.execute("SELECT coalesce(max(tick),0) FROM events WHERE event='ROI_BEGIN'").fetchone()[
            0
        ],
    )
    failures, stages = analyze_demands(db, roi_begin)
    counts = db.execute("""SELECT count(*),sum(result='miss'),sum(complete_tick IS NOT NULL),
        avg(complete_tick-first_tick),sum(attempts-1) FROM demands""").fetchone()
    requests, misses, completed, latency, retries = counts
    require(sum(failures.values()) == (misses or 0), "Residual miss categories do not conserve")
    summary = {
        "trace": metadata,
        "demand": {
            "requests": requests,
            "misses": misses or 0,
            "completed": completed or 0,
            "right_censored": requests - (completed or 0),
            "mean_latency_ticks": latency,
            "replayed_lookups": retries or 0,
            "left_censored_completions": db.execute(
                "SELECT count(*) FROM events e WHERE event='DEMAND_COMPLETE' "
                "AND NOT EXISTS(SELECT 1 FROM demands d WHERE d.cache=e.cache "
                "AND d.request_id=e.request_id)"
            ).fetchone()[0],
            "arrival_only_requests": db.execute("""SELECT count(*) FROM events e
                       WHERE event='DEMAND_ARRIVE' AND NOT EXISTS(SELECT 1 FROM demands d
                       WHERE d.cache=e.cache AND d.request_id=e.request_id)""").fetchone()[0],
            "late_prefetch_hits": db.execute(
                "SELECT count(*) FROM demands WHERE classification='late_prefetch_hit'"
            ).fetchone()[0],
            "lookup_attempts": db.execute(
                "SELECT count(*) FROM events WHERE event='DEMAND_ACCESS'"
            ).fetchone()[0],
            "retry_only_requests": db.execute(
                """SELECT count(*) FROM
                       (SELECT cache,request_id FROM events WHERE event='DEMAND_ACCESS'
                        GROUP BY cache,request_id HAVING sum(result IN ('hit','miss'))=0)"""
            ).fetchone()[0],
            "failures": failures,
            "failure_stages": stages,
        },
        "prefetch": prefetch_metrics(db, int(metadata["block_size"])),
    }
    return summary, pc_analysis(db, weight)


def compare_pcs(before, after, before_retired, after_retired, weight):
    """Rank observed overlapping request-wait changes, never additive CPU stalls."""
    keys = ("cache", "context_id", "address_space", "secure", "pc")
    old = {tuple(pc[k] for k in keys): pc for pc in before}
    new = {tuple(pc[k] for k in keys): pc for pc in after}
    rows = []
    for key in old.keys() | new.keys():
        a, b = old.get(key, {}), new.get(key, {})
        wait_a = (a.get("mean_latency_ticks") or 0) * a.get("completed", 0)
        wait_b = (b.get("mean_latency_ticks") or 0) * b.get("completed", 0)
        rows.append(
            {
                **dict(zip(keys, key)),
                "accesses_before": a.get("accesses", 0),
                "accesses_after": b.get("accesses", 0),
                "misses_before": a.get("misses", 0),
                "misses_after": b.get("misses", 0),
                "pending_before": a.get("accesses", 0) - a.get("completed", 0),
                "pending_after": b.get("accesses", 0) - b.get("completed", 0),
                "weighted_wait_delta_ticks_per_inst": weight
                * (wait_b / after_retired - wait_a / before_retired),
            }
        )
    return sorted(
        rows,
        key=lambda r: (
            -abs(r["weighted_wait_delta_ticks_per_inst"]),
            str(tuple(r[k] for k in keys)),
        ),
    )
