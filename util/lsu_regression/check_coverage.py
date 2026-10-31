#!/usr/bin/env python3
"""Check functional success plus branch evidence on current XS-GEM5 traces."""
import argparse
from collections import defaultdict
import json
from pathlib import Path
import re
import sys
from compare_suite import stats


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--run", type=Path, required=True)
    p.add_argument("--partial", action="store_true")
    a = p.parse_args()
    root = a.run.resolve()
    cases = json.loads(
        (
            root / ("partial-summary.json" if a.partial else "summary.json")
        ).read_text()
    )
    result = []
    for case in cases:
        name = case["case"]
        d = root / name
        trace_path = d / "m5out/trace.debug"
        stats_path = d / "m5out/stats.txt"
        ss = stats(stats_path) if stats_path.is_file() else []
        if not trace_path.is_file() or not ss:
            result.append(
                {
                    "case": name,
                    "checks": {"artifacts_present": False},
                    "passed": False,
                    "detail": {"functional_result": case.get("check_passed", False)},
                }
            )
            continue
        trace = trace_path.read_text()
        v = ss[-1]

        def value(k):
            return float(v.get(k, "0"))

        checks = {"functional_result": case.get("check_passed", False)}
        detail = {}
        if name == "negative":
            checks["positive_checker_rejects_bad_expected"] = (
                not case.get("success", False)
                and case.get("negative_rejected_as_positive", False)
            )
        elif name == "fs0":
            checks["byte_merge"] = value("system.cpu.lsq0.sbufferMerge") >= 3
        elif name == "fs1":
            checks["partial_reschedule"] = (
                value("system.cpu.lsq0.loadReplayEvents::RescheduleReplay") > 0
            )
        elif name in ("fs2", "fs7"):
            checks["two_architectural_faults"] = (
                trace.count("Committing instruction with fault") == 2
            )
        elif name == "fs3":
            st = re.findall(r"Store Conditional success: (\d+)", trace)
            checks["SC_success_and_forced_failure"] = (
                "1" in st and bool(st) and st[-1] == "0"
            )
            detail["SC_internal_statuses"] = st
        elif name == "fs4":
            checks["wrong_path_store_issued"] = "issueToStorePipe:" in trace
            checks["wrong_path_fault_detected"] = (
                "not executed from fault" in trace
            )
            checks["wrong_path_no_architectural_trap"] = (
                "Committing instruction with fault" not in trace
            )
            checks["squash"] = trace.count("Squashing until") > 1
        elif name == "fs5":
            checks["physical_SQ_backpressure"] = (
                value("system.cpu.lsq0.storeReplayPhysicalSQFull") > 0
            )
            checks["vice"] = value("system.cpu.lsq0.sbufferCreateVice") > 0
            checks["new_same_line_MSHR_merge"] = (
                value("system.cpu.lsq.sbufferMissMerged") > 0
            )
        elif name == "fs6":
            checks["physical_SQ_backpressure"] = (
                value("system.cpu.lsq0.storeReplayPhysicalSQFull") > 0
            )
            checks["SBuffer_full"] = value("system.cpu.lsq0.sbufferFull") > 0
            checks["new_same_line_MSHR_merge"] = (
                value("system.cpu.lsq.sbufferMissMerged") > 0
            )
        elif name == "fs8":
            checks["one_real_interrupt"] = (
                trace.count("Interrupt detected.") == 1
            )
        elif name.startswith("vector"):
            checks["split_store_observed"] = "Spilt store idx" in trace
            if name == "vector_pressure":
                lines = trace.splitlines()
                by = defaultdict(list)
                for i, line in enumerate(lines):
                    m = re.search(
                        r"^\s*(\d+):.*Spilt store idx (\d+) \[sn:(\d+)\]", line
                    )
                    if m:
                        by[int(m[3])].append((int(m[1]), int(m[2]), i))
                witnesses = []
                for seq, ev in by.items():
                    ix = [e[1] for e in ev]
                    if ix.count(0) != 1 or ix.count(1) <= 1:
                        continue
                    first = ev[0]
                    last = ev[-1]
                    first_success = any(
                        "successed" in l
                        for l in lines[first[2] + 1 : ev[1][2]]
                    )
                    second_fail = any(
                        "failed due to sbuffer full" in l
                        for l in lines[ev[1][2] + 1 : ev[2][2]]
                    )
                    tail = lines[last[2] + 1 : last[2] + 6]
                    final_success = any("successed" in l for l in tail)
                    final_completion = any(
                        f"Completing store [sn:{seq}]" in l for l in tail
                    )
                    early_completion = any(
                        f"Completing store [sn:{seq}]" in l
                        for l in lines[first[2] : last[2]]
                    )
                    if (
                        first_success
                        and second_fail
                        and final_success
                        and final_completion
                        and not early_completion
                    ):
                        witnesses.append(
                            {
                                "seq": seq,
                                "first_fragment_tick": first[0],
                                "second_fragment_finish_tick": last[0],
                                "fragment1_attempts": ix.count(1),
                                "fragment0_attempts": 1,
                            }
                        )
                checks["partial_handoff_second_fragment_blocked"] = bool(
                    witnesses
                )
                detail["partial_handoff_witness_count"] = len(witnesses)
                detail["partial_handoff_witnesses"] = witnesses[:4]
        elif name == "se_smt":
            log = (d / "run.log").read_text()
            checks["two_guest_self_checks"] = log.count("SMT_PASS") == 2
            checks["both_LSQ_thread_identities"] = (
                "lsq.thread0:" in trace and "lsq.thread1:" in trace
            )
        elif name in ("fs9", "fs10"):
            registers = re.findall(r"MDP addr replay register", trace)
            producer = re.findall(r"MDP addr replay ready \(store\[sn:", trace)
            pipe = re.findall(r"MDP addr replay ready \(pipeDone\)", trace)
            checks["MDP_registered"] = len(registers) > 0
            checks["MDP_woken"] = len(producer) + len(pipe) > 0
            detail["MDP_producer_second"] = len(producer)
            detail["MDP_pipeDone_second"] = len(pipe)
        detail["selected_counters"] = {
            k: v[k]
            for k in v
            if any(
                x in k
                for x in (
                    "sbufferMissMerged",
                    "sbufferCreateVice",
                    "sbufferFull",
                    "storeReplayPhysicalSQFull",
                    "MdpAddrReplay",
                )
            )
        }
        result.append(
            {
                "case": name,
                "checks": checks,
                "passed": all(checks.values()),
                "detail": detail,
            }
        )
    (root / "coverage_checks.json").write_text(
        json.dumps(result, indent=2) + "\n"
    )
    print(
        json.dumps(
            [
                {
                    "case": v["case"],
                    "passed": v["passed"],
                    "failed": [k for k, x in v["checks"].items() if not x],
                }
                for v in result
            ],
            indent=2,
        )
    )
    return 0 if all(v["passed"] for v in result) else 1


if __name__ == "__main__":
    sys.exit(main())
