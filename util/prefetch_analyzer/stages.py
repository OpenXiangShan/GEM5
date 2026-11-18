"""Source-specific lifecycle outcomes and observed phase intervals."""

import math


def source_analysis(db):
    """Attribute sends to their generating source; aliases never add sends."""
    sources = [
        dict(row)
        for row in db.execute("""SELECT origin_cache AS cache,source,count(*) AS candidates,
        sum(alias_id IS NOT NULL) AS aliases,
        sum(send_tick IS NOT NULL) AS sent,
        sum(send_tick IS NOT NULL AND use_tick IS NOT NULL) AS useful,
        sum(unused_evicted) AS unused_evicted,
        sum(send_tick IS NOT NULL AND use_tick IS NULL AND unused_evicted=0) AS censored
        FROM prefetches WHERE candidate_tick IS NOT NULL GROUP BY origin_cache,source
        ORDER BY origin_cache,source""")
    ]
    for row in sources:
        row["accuracy_observed"] = row["useful"] / row["sent"] if row["sent"] else None
    drops = [
        dict(row)
        for row in db.execute(
            """SELECT p.origin_cache AS cache,p.source,e.reason,count(*) AS events
        FROM events e JOIN prefetches p ON e.prefetch_id=p.id
        WHERE e.event='PF_DROP' AND p.candidate_tick IS NOT NULL
        GROUP BY p.origin_cache,p.source,e.reason ORDER BY events DESC"""
        )
    ]
    phases = []
    for stage, begin, end in (
        ("prediction_to_enqueue", "candidate_tick", "enqueue_tick"),
        ("enqueue_to_dequeue", "enqueue_tick", "dequeue_tick"),
        ("dequeue_to_accept", "dequeue_tick", "send_tick"),
        ("accept_to_first_ready", "send_tick", "ready_tick"),
    ):
        query = f"""SELECT origin_cache,source,{end}-{begin} AS elapsed FROM prefetches
            WHERE candidate_tick IS NOT NULL AND alias_id IS NULL
            AND {begin} IS NOT NULL AND {end}>={begin}
            ORDER BY origin_cache,source,elapsed"""
        groups = {}
        for row in db.execute(query):
            groups.setdefault((row["origin_cache"], row["source"]), []).append(row["elapsed"])
        for (cache, source), values in groups.items():

            def quantile(fraction):
                return values[max(0, math.ceil(fraction * len(values)) - 1)]

            phases.append(
                dict(
                    cache=cache,
                    source=source,
                    stage=stage,
                    samples=len(values),
                    mean_ticks=sum(values) / len(values),
                    p50_ticks=quantile(0.5),
                    p90_ticks=quantile(0.9),
                    p99_ticks=quantile(0.99),
                )
            )
    return dict(sources=sources, drop_events=drops, phases=phases)
