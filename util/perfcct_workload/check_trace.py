#!/usr/bin/env python3
"""Check causal trace identities and resource lifetimes without attribution."""

import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import sqlite3


def check_connection(db):
    db.row_factory = sqlite3.Row
    errors, warnings = [], []
    active, credits, seen = (
        defaultdict(set),
        defaultdict(set),
        defaultdict(set),
    )
    opened, open_credits = defaultdict(set), defaultdict(set)
    endings, rejects, counts = {}, {}, Counter()
    columns = {
        row["name"]
        for row in db.execute("PRAGMA table_info(PerfCCTCacheEvent)")
    }
    has_targets = {"TargetID", "RequestID", "RelatedRequestID"}.issubset(
        columns
    )
    targets, target_history = defaultdict(dict), defaultdict(dict)
    opened_targets, target_rejects = defaultdict(set), {}

    def same_request(left, right):
        # Zero is explicitly unknown, never an identity equality claim.
        return not left or not right or left == right

    def require(ok, message, row):
        if not ok:
            errors.append({"id": row["ID"], "message": message})

    for row in db.execute("SELECT * FROM PerfCCTCacheEvent ORDER BY ID"):
        cache, event, resource = row["Cache"], row["Event"], row["MSHR"]
        counts[event] += 1
        if cache in endings:
            require(
                event in ("open_mshr", "open_credit", "open_target")
                and row["Tick"] == endings[cache],
                "event after trace_end",
                row,
            )
        if event == "allocate":
            require(
                resource != 0 and resource not in seen[cache],
                "allocation generation reused or zero",
                row,
            )
            seen[cache].add(resource)
            active[cache].add(resource)
        if event in (
            "merge",
            "send",
            "retry",
            "response",
            "release",
            "owner",
            "open_mshr",
        ):
            require(
                resource in active[cache],
                "MSHR event without active allocation",
                row,
            )
        if event == "credit_hold":
            require(
                resource in active[cache] and resource not in credits[cache],
                "credit hold without active MSHR or duplicate hold",
                row,
            )
            credits[cache].add(resource)
        if event == "credit_release":
            require(
                resource in credits[cache], "credit release without hold", row
            )
            credits[cache].discard(resource)
        if event in ("owner_credit", "open_credit"):
            require(
                resource in credits[cache],
                "credit event without held credit",
                row,
            )
        require(
            row["Allocated"] == len(active[cache]),
            "Allocated snapshot mismatch",
            row,
        )
        require(
            row["HeldCredits"] == len(credits[cache]),
            "HeldCredits snapshot mismatch",
            row,
        )
        if has_targets:
            target, request = row["TargetID"], row["RequestID"]
            if event.startswith("target_") or event == "open_target":
                require(
                    resource in active[cache],
                    "target references inactive MSHR",
                    row,
                )
                require(target != 0, "zero target identity", row)
                if event == "target_add":
                    require(
                        target not in target_history[cache],
                        "target identity reused",
                        row,
                    )
                    targets[cache][target] = (resource, request)
                    target_history[cache][target] = (resource, request)
                elif event == "target_service":
                    prior = target_history[cache].get(target)
                    require(
                        prior is not None
                        and prior[0] == resource
                        and same_request(prior[1], request),
                        "service references unknown target/MSHR/request",
                        row,
                    )
                else:
                    prior = targets[cache].get(target)
                    require(
                        prior is not None and prior[0] == resource,
                        "target absent or belongs to another MSHR",
                        row,
                    )
                    if prior:
                        if event == "target_replace":
                            require(
                                same_request(
                                    prior[1], row["RelatedRequestID"]
                                ),
                                "replacement old request mismatch",
                                row,
                            )
                            require(
                                not request
                                or not row["RelatedRequestID"]
                                or request != row["RelatedRequestID"],
                                "Request copy retained old identity",
                                row,
                            )
                            targets[cache][target] = (resource, request)
                            target_history[cache][target] = (resource, request)
                        else:
                            require(
                                same_request(prior[1], request),
                                "target request mismatch",
                                row,
                            )
                if "Targets" in columns:
                    require(
                        row["Targets"]
                        == sum(
                            value[0] == resource
                            for value in targets[cache].values()
                        ),
                        "target occupancy snapshot mismatch",
                        row,
                    )
            if event == "target_owner":
                snapshot = target_rejects.get(row["ParentID"])
                require(
                    snapshot is not None
                    and snapshot[:2] == (cache, row["Tick"]),
                    "target owner lacks same-cache/tick rejection",
                    row,
                )
                if snapshot:
                    prior = snapshot[2].get(target)
                    require(
                        prior is not None
                        and prior[0] == resource
                        and same_request(prior[1], request),
                        "target owner absent/request mismatch at rejection",
                        row,
                    )
            if event == "target_remove":
                targets[cache].pop(target, None)
            if event == "open_target":
                require(cache in endings, "open target without trace_end", row)
                require(
                    target not in opened_targets[cache],
                    "duplicate open target",
                    row,
                )
                opened_targets[cache].add(target)
            if event == "release":
                require(
                    not any(
                        value[0] == resource
                        for value in targets[cache].values()
                    ),
                    "MSHR released with active targets",
                    row,
                )
        if event == "reject":
            rejects[row["ID"]] = (
                cache,
                row["Tick"],
                set(active[cache]),
                set(credits[cache]),
            )
            if has_targets:
                target_rejects[row["ID"]] = (
                    cache,
                    row["Tick"],
                    dict(targets[cache]),
                )
        if event in ("owner", "owner_credit"):
            parent = rejects.get(row["ParentID"])
            require(
                parent is not None and parent[:2] == (cache, row["Tick"]),
                "owner ParentID is not a same-cache/tick rejection",
                row,
            )
            if parent:
                index = 2 if event == "owner" else 3
                require(
                    resource in parent[index],
                    "owner absent at parent rejection",
                    row,
                )
        if event == "release":
            active[cache].discard(resource)
        if event == "trace_end":
            require(cache not in endings, "duplicate trace_end", row)
            endings[cache] = row["Tick"]
        if event in ("open_mshr", "open_credit"):
            collection = opened if event == "open_mshr" else open_credits
            require(cache in endings, "open resource without trace_end", row)
            require(
                resource not in collection[cache],
                "duplicate open resource",
                row,
            )
            collection[cache].add(resource)
    for cache in set(active) | set(credits):
        if cache not in endings:
            errors.append({"cache": cache, "message": "missing trace_end"})
        if (
            active[cache] != opened[cache]
            or credits[cache] != open_credits[cache]
        ):
            errors.append(
                {
                    "cache": cache,
                    "message": "open tail does not match remaining resources",
                }
            )

    if has_targets:
        for cache in set(targets) | set(opened_targets):
            if set(targets[cache]) != opened_targets[cache]:
                errors.append(
                    {"cache": cache, "message": "open target tail mismatch"}
                )

    identities = {
        (row["Cpu"], row["TID"], row["SeqNum"]): row["EndKind"]
        for row in db.execute("SELECT Cpu,TID,SeqNum,EndKind FROM PerfCCTInst")
    }
    fused = {
        (row["Cpu"], row["TID"], row["SeqNum"]): row["RelatedSeq"]
        for row in db.execute(
            "SELECT * FROM PerfCCTEvent WHERE Event='fused_into'"
        )
    }
    dependencies, unknown, followed_fusions = 0, 0, 0
    for row in db.execute(
        "SELECT * FROM PerfCCTEvent WHERE Event='dependency'"
    ):
        dependencies += 1
        cpu, tid, seq, producer = (
            row["Cpu"],
            row["TID"],
            row["SeqNum"],
            row["RelatedSeq"],
        )
        consumer = identities.get((cpu, tid, seq))
        require(consumer is not None, "dependency consumer missing", row)
        if producer == 0:
            unknown += 1
            continue
        require(producer < seq, "producer is not older than consumer", row)
        key = (cpu, tid, producer)
        require(key in identities, "producer identity missing", row)
        visited = set()
        while identities.get(key) == "fused_into":
            if key in visited or key not in fused:
                require(False, "unresolved or cyclic fused producer", row)
                break
            visited.add(key)
            followed_fusions += 1
            key = (cpu, tid, fused[key])
            require(
                key in identities and key[2] < seq,
                "fused producer missing or not older",
                row,
            )
        if consumer == "commit":
            require(
                identities.get(key) != "squash",
                "committed consumer has squashed producer",
                row,
            )
    return {
        "ok": not errors,
        "errors": errors,
        "warnings": warnings,
        "cache_event_counts": dict(counts),
        "dependencies": dependencies,
        "target_checks_enabled": has_targets,
        "open_targets": {
            cache: sorted(values) for cache, values in targets.items()
        },
        "zero_producer_edges": unknown,
        "followed_fusions": followed_fusions,
        "open_mshrs": {
            cache: sorted(values) for cache, values in active.items()
        },
        "open_credits": {
            cache: sorted(values) for cache, values in credits.items()
        },
        "notes": [
            "Allocated is sampled after allocate and before release; HeldCredits after hold/release.",
            "MSHR generation reuse is forbidden; physical slot reuse with a new generation is valid.",
            "Zero producer is constant/initial/unsupported, not an integrity failure.",
            "RequestID zero is unknown; replacements validate RelatedRequestID when both are known.",
            "Target service need not remove a target; extracted targets may service after removal.",
            "Open resources are valid only when explicitly recorded at trace end.",
            "These checks validate recorded consistency, not complete capture or performance causality.",
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("db")
    args = parser.parse_args()
    try:
        with sqlite3.connect(
            Path(args.db).resolve().as_uri() + "?mode=ro", uri=True
        ) as db:
            result = check_connection(db)
    except sqlite3.Error as error:
        result = {"ok": False, "errors": [{"message": str(error)}]}
    print(json.dumps(result, indent=2))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
