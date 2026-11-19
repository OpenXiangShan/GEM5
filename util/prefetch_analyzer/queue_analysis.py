"""Queue state samples and matched residence intervals from trace facts."""

import json
import math


def queue_analysis(db):
    samples = []
    for event in db.execute(
        """SELECT actor,source,prefetch_id,event,tick,result
            FROM events WHERE event IN
            ('PF_BUFFER_ENQUEUE','PF_BUFFER_DEQUEUE','PF_ENQUEUE','PF_DEQUEUE')
            ORDER BY tick,[order]"""
    ):
        if not event["result"].startswith("{"):
            continue
        state = json.loads(event["result"])
        samples.append(
            {
                **{
                    key: event[key]
                    for key in (
                        "actor",
                        "source",
                        "prefetch_id",
                        "event",
                        "tick",
                    )
                },
                "priority": state["priority"],
                "prediction_buffer_entries": state.get(
                    "prediction_buffer_entries"
                ),
                "ready_queue_entries": state.get("ready_queue_entries"),
                "translation_queue_entries": state.get(
                    "translation_queue_entries"
                ),
            }
        )

    intervals = []
    for stage, begin, end, condition in (
        ("prediction_buffer", "PF_BUFFER_ENQUEUE", "PF_BUFFER_DEQUEUE", "1"),
        ("ready_queue", "PF_ENQUEUE", "PF_DEQUEUE", "reason='ready_queue'"),
    ):
        query = f"""SELECT actor,source,prefetch_id,
            min(CASE WHEN event=? AND {condition} THEN tick END) AS begin_tick,
            min(CASE WHEN event=? THEN tick END) AS end_tick,
            max(CASE WHEN event=? THEN result END) AS state
            FROM events WHERE event IN (?,?)
            GROUP BY actor,source,prefetch_id
            HAVING begin_tick IS NOT NULL AND end_tick>=begin_tick"""
        for row in db.execute(query, (begin, end, end, begin, end)):
            if not row["state"].startswith("{"):
                continue
            intervals.append(
                {
                    "actor": row["actor"],
                    "source": row["source"],
                    "prefetch_id": row["prefetch_id"],
                    "priority": json.loads(row["state"])["priority"],
                    "stage": stage,
                    "begin_tick": row["begin_tick"],
                    "end_tick": row["end_tick"],
                    "elapsed_ticks": row["end_tick"] - row["begin_tick"],
                }
            )

    groups = {}
    for row in intervals:
        key = row["actor"], row["source"], row["priority"], row["stage"]
        groups.setdefault(key, []).append(row["elapsed_ticks"])
    stages = []
    for (actor, source, priority, stage), values in sorted(groups.items()):
        values.sort()
        stages.append(
            {
                "actor": actor,
                "source": source,
                "priority": priority,
                "stage": stage,
                "samples": len(values),
                "mean_ticks": sum(values) / len(values),
                "p90_ticks": values[math.ceil(0.9 * len(values)) - 1],
                "p99_ticks": values[math.ceil(0.99 * len(values)) - 1],
            }
        )
    outcomes = {}
    seen = set()
    for row in db.execute("""SELECT e.actor,e.source,e.prefetch_id,e.result,
            p.send_tick,p.use_tick,p.unused_evicted FROM events e
            JOIN prefetches p ON e.prefetch_id=p.id
            WHERE e.event='PF_DEQUEUE' ORDER BY e.tick,e.[order]"""):
        identity = row["actor"], row["prefetch_id"]
        if identity in seen or not row["result"].startswith("{"):
            continue
        seen.add(identity)
        priority = json.loads(row["result"])["priority"]
        key = row["actor"], row["source"], priority
        counts = outcomes.setdefault(
            key,
            {
                "actor": row["actor"],
                "source": row["source"],
                "priority": priority,
                "dequeued_requests": 0,
                "accepted_requests": 0,
                "useful_requests": 0,
                "unused_evicted_requests": 0,
                "censored_requests": 0,
            },
        )
        counts["dequeued_requests"] += 1
        if row["send_tick"] is not None:
            counts["accepted_requests"] += 1
            if row["use_tick"] is not None:
                counts["useful_requests"] += 1
            elif row["unused_evicted"]:
                counts["unused_evicted_requests"] += 1
            else:
                counts["censored_requests"] += 1
    outcome_rows = []
    for key in sorted(outcomes):
        row = outcomes[key]
        row["accuracy_observed"] = (
            row["useful_requests"] / row["accepted_requests"]
            if row["accepted_requests"]
            else None
        )
        outcome_rows.append(row)
    return {
        "stages": stages,
        "samples": samples,
        "intervals": intervals,
        "outcomes": outcome_rows,
    }
