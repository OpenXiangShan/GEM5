#!/usr/bin/env python3
"""Integrate exact cache state changes over the observed ROI."""

import collections
import json

from parse_trace import require

COUNTS = ("valid_lines", "prefetched_lines", "unused_prefetched_lines")


def cache_occupancy(db, metadata, begin, end, bins=128):
    if metadata.get("cache_occupancy_version") != "1":
        return {"status": "not_recorded", "caches": [], "timeline": []}
    require(end > begin, "Cache occupancy requires a nonempty ROI")
    width = end - begin
    bins = min(bins, width)
    boundaries = [begin + width * i // bins for i in range(bins + 1)]
    caches, timeline = [], []
    owners = db.execute(
        "SELECT DISTINCT cache FROM events WHERE event='CACHE_OCCUPANCY' ORDER BY cache"
    ).fetchall()
    require(owners, "Cache occupancy metadata has no state snapshots")
    for (cache,) in owners:
        events = db.execute(
            "SELECT tick,result,level FROM events WHERE event='CACHE_OCCUPANCY' "
            'AND cache=? ORDER BY tick,"order"',
            (cache,),
        )
        integrals = collections.Counter()
        peaks = collections.Counter()
        areas = [collections.Counter() for _ in range(bins)]
        previous, tick, capacity, level = None, begin, None, None
        index = 0
        for event in events:
            next_tick = min(event["tick"], end)
            require(
                event["tick"] >= begin,
                "Cache state precedes the occupancy ROI",
            )
            require(
                previous is not None or next_tick == begin,
                f"{cache}: missing ROI snapshot",
            )
            while tick < next_tick:
                stop = min(next_tick, boundaries[index + 1])
                for key in COUNTS:
                    area = previous[key] * (stop - tick)
                    integrals[key] += area
                    areas[index][key] += area
                tick = stop
                if index + 1 < bins and tick == boundaries[index + 1]:
                    index += 1
            if event["tick"] >= end:
                break
            state = json.loads(event["result"])
            current_capacity = state["capacity_lines"]
            require(
                0
                <= state["unused_prefetched_lines"]
                <= state["prefetched_lines"]
                <= state["valid_lines"]
                <= current_capacity,
                f"{cache}: invalid occupancy state {state}",
            )
            require(
                capacity is None or capacity == current_capacity,
                "Cache capacity changed",
            )
            capacity, level, previous = current_capacity, event["level"], state
            for key in COUNTS:
                peaks[key] = max(peaks[key], state[key])
        # The last observed state persists to the exclusive ROI end.
        while tick < end:
            stop = boundaries[index + 1]
            for key in COUNTS:
                area = previous[key] * (stop - tick)
                integrals[key] += area
                areas[index][key] += area
            tick = stop
            index += 1
        record = {"cache": cache, "level": level, "capacity_lines": capacity}
        for key in COUNTS:
            record[key + "_ticks"] = integrals[key]
            record["mean_" + key] = integrals[key] / width
            record["peak_" + key] = peaks[key]
            record[key + "_capacity_fraction"] = integrals[key] / (
                width * capacity
            )
        caches.append(record)
        for i, area in enumerate(areas):
            timeline.append(
                {
                    "cache": cache,
                    "begin_tick": boundaries[i],
                    "end_tick": boundaries[i + 1],
                    "capacity_lines": capacity,
                    **{
                        key: area[key] / (boundaries[i + 1] - boundaries[i])
                        for key in COUNTS
                    },
                }
            )
    return {"status": "recorded", "caches": caches, "timeline": timeline}
