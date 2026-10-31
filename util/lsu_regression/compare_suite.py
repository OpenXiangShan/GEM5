#!/usr/bin/env python3
"""Strictly compare all simulated statistics sections, config, exits, guest output and trace."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

HOST_STATS = {
    "hostSeconds",
    "hostTickRate",
    "hostInstRate",
    "hostOpRate",
    "hostMemory",
    "hostMemoryUsage",
}


def sha_text(t):
    return hashlib.sha256(t.encode()).hexdigest()


def stats(path):
    sections = []
    section_open = False
    for line in path.read_text().splitlines():
        if "Begin Simulation Statistics" in line:
            sections.append({})
            section_open = True
        elif "End Simulation Statistics" in line:
            section_open = False
        elif section_open and line and not line.startswith(("#", "-")):
            fields = line.split("#", 1)[0].split()
            if len(fields) > 1 and fields[0] not in HOST_STATS:
                if fields[0] in sections[-1]:
                    raise ValueError(f"Duplicate stats {path} {fields[0]}")
                sections[-1][fields[0]] = " ".join(fields[1:])
    return [] if section_open else sections


# These exact logs sample memData before SQ/SB forward packets are assembled.
# Only their trailing unassembled-data token is non-deterministic.  Tick,
# thread, sequence numbers, physical address and all real writeback data stay.
UNASSEMBLED_FORWARD_FIELDS = [
    r"(?m)^(\s*\d+: [^\n]+: (?:\[c: \d+\] )?Forwarding from store "
    r"\[sn:\d+\] to load \[sn:\d+\] addr 0x[0-9a-f]+, data: )0x[0-9a-f]+$",
    r"(?m)^(\s*\d+: [^\n]+: (?:\[c: \d+\] )?Load \[sn:\d+\] forward from sbuffer, data: )[0-9a-f]+$",
]


def normalize_trace(raw):
    counts = []
    for pattern in UNASSEMBLED_FORWARD_FIELDS:
        raw, n = re.subn(
            pattern, lambda m: m[1] + "<UNASSEMBLED_FORWARD_DATA>", raw
        )
        counts.append(n)
    return raw, counts


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--base", type=Path, required=True)
    p.add_argument("--candidate", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument(
        "--base-source-root",
        type=Path,
        help="Normalize absolute source paths from the base gem5 tree",
    )
    p.add_argument(
        "--candidate-source-root",
        type=Path,
        help="Normalize absolute source paths from the candidate gem5 tree",
    )
    p.add_argument(
        "--base-output-prefix",
        help="Normalize absolute output paths from all base suite variants",
    )
    p.add_argument(
        "--candidate-output-prefix",
        help="Normalize absolute output paths from all candidate suite variants",
    )
    a = p.parse_args()
    base, cand = a.base.resolve(), a.candidate.resolve()
    run_family = base.parent
    source_roots = [
        a.base_source_root.resolve() if a.base_source_root else None,
        a.candidate_source_root.resolve() if a.candidate_source_root else None,
    ]
    summaries = [
        json.loads((v / "summary.json").read_text()) for v in (base, cand)
    ]
    names = [[v["case"] for v in s] for s in summaries]
    if names[0] != names[1]:
        raise SystemExit("Case lists differ")
    results = []
    for name in names[0]:
        ds = [v / name for v in (base, cand)]
        required = [
            "result.json",
            "run.log",
            "m5out/config.ini",
            "m5out/stats.txt",
            "m5out/trace.debug",
        ]
        missing = [
            str(d / rel)
            for d in ds
            for rel in required
            if not (d / rel).is_file()
        ]
        if missing:
            results.append(
                {
                    "case": name,
                    "passed": False,
                    "functional_success": False,
                    "image_equal": False,
                    "exit_equal": False,
                    "outputs_equal": False,
                    "config_equal": False,
                    "stats_sections": [],
                    "stat_values": 0,
                    "stats_diff_count": None,
                    "stats_diffs": [],
                    "trace_equal": False,
                    "config_error": "missing required artifacts",
                    "missing_artifacts": missing,
                }
            )
            continue
        records = [json.loads((d / "result.json").read_text()) for d in ds]
        values = [stats(d / "m5out/stats.txt") for d in ds]
        diffs = []
        for i in range(max(map(len, values))):
            x, y = [v[i] if i < len(v) else {} for v in values]
            for key in sorted(x.keys() | y.keys()):
                if x.get(key) != y.get(key):
                    diffs.append(
                        {
                            "section": i,
                            "key": key,
                            "base": x.get(key),
                            "candidate": y.get(key),
                        }
                    )
        configs = []
        output_prefixes = [
            a.base_output_prefix,
            a.candidate_output_prefix,
        ]
        for d, run_root, source_root, output_prefix in zip(
            ds, (base, cand), source_roots, output_prefixes
        ):
            config = (d / "m5out/config.ini").read_text().replace(
                str(run_root), "<RUN>"
            )
            if source_root is not None:
                config = config.replace(str(source_root), "<GEM5>")
            config = re.sub(
                re.escape(str(run_family))
                + r"/(?:official|candidate)-[^/]+/",
                "<RUN>/",
                config,
            )
            if output_prefix is not None:
                config = config.replace(output_prefix, "<RUN>/")
            configs.append(config)
        raw_traces = [(d / "m5out/trace.debug").read_text() for d in ds]
        normalized = [normalize_trace(t) for t in raw_traces]
        traces = [v[0] for v in normalized]
        logs = [(d / "run.log").read_text(errors="replace") for d in ds]
        outputs = [
            re.findall(r"CASE=\d+ checksum=\d+ expected=\d+ failures=\d+", v)
            for v in logs
        ]
        r = {
            "case": name,
            "functional_success": all(v["check_passed"] for v in records),
            "image_equal": records[0]["image_sha256"]
            == records[1]["image_sha256"],
            "exit_equal": records[0]["exit"] == records[1]["exit"],
            "outputs_equal": outputs[0] == outputs[1],
            "config_equal": configs[0] == configs[1],
            "stats_sections": list(map(len, values)),
            "stat_values": sum(map(len, values[0])),
            "stats_diff_count": len(diffs),
            "stats_diffs": diffs,
            "trace_equal": traces[0] == traces[1],
            "trace_sha256": list(map(sha_text, traces)),
            "raw_trace_equal": raw_traces[0] == raw_traces[1],
            "raw_trace_sha256": list(map(sha_text, raw_traces)),
            "trace_normalized_field_counts": [v[1] for v in normalized],
            "trace_normalization": (
                "Only two exact unassembled SQ/SB forwarding-data fields; "
                "all event lines/tick/seq/addresses/real writeback data retained; "
                "see docs/Gem5_Docs/lsu/lsu_architecture/05-rtl-and-validation.md"
            ),
            "ignored_host_stats": sorted(HOST_STATS),
        }
        r["passed"] = (
            all(
                r[k]
                for k in (
                    "functional_success",
                    "image_equal",
                    "exit_equal",
                    "outputs_equal",
                    "config_equal",
                    "trace_equal",
                )
            )
            and not diffs
            and bool(values[0])
            and len(values[0]) == len(values[1])
        )
        results.append(r)
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text(json.dumps(results, indent=2) + "\n")
    print(
        json.dumps(
            [
                {
                    "case": r["case"],
                    "passed": r["passed"],
                    "stats_diff_count": r["stats_diff_count"],
                    "trace_equal": r["trace_equal"],
                    "config_equal": r["config_equal"],
                }
                for r in results
            ],
            indent=2,
        )
    )
    return 0 if all(r["passed"] for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
