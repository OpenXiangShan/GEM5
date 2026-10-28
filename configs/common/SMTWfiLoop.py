"""Measure T0 while T1 runs a software WFI loop on the unmodified CPU."""

import hashlib
import json
from pathlib import Path

import m5
from m5.util import fatal


def run(options, system, maxtick):
    cpu = system.cpu[0]
    warmup = options.warmup_insts_no_switch or 0
    total = options.maxinsts
    minimum = options.smt_wfi_loop_restore_insts
    if warmup < 0 or minimum < 0 or not total or total <= max(warmup, minimum):
        fatal("WFI loop needs 0 <= warmup/restore < --maxinsts (T0 totals)")
    restorer = Path(str(system.gcpt_restorer_file))
    manifest_path = options.smt_wfi_loop_manifest
    if not manifest_path:
        manifest_path = restorer.with_name("manifest.json")
    manifest = json.loads(Path(manifest_path).read_text())
    if manifest.get("experiment") != "wfi-loop":
        fatal("Use the manifest produced by smt_wfi/build_restorer.py")
    binary = restorer.read_bytes()
    if hashlib.sha256(binary).hexdigest() != manifest["binary_sha256"]:
        fatal("WFI loop restorer hash does not match its manifest")
    offset = manifest["wfi_offset"]
    if binary[offset:offset + 8] != bytes.fromhex("730050106ff0dfff"):
        fatal("WFI loop must contain an uncompressed WFI and backward JAL")
    report = {"experiment": "wfi-loop", "measured_tid": 0, "loop_tid": 1,
              "num_threads": int(cpu.numThreads),
              "checkpoint": str(system.gcpt_file), "restorer": str(restorer),
              "restorer_sha256": manifest["binary_sha256"],
              "wfi_pc": manifest["wfi_pc"],
              "warmup_total_insts": warmup, "stop_total_insts": total,
              "phases": []}

    def simulate_to(target, cause):
        remaining = target - int(cpu.getCurrentInstCount(0))
        if remaining <= 0:
            return
        cpu.scheduleInstStop(0, remaining, cause)
        event = m5.simulate(max(0, maxtick - m5.curTick()))
        if event.getCause() != cause or event.getCode() != 0:
            fatal("WFI loop expected '%s', got '%s' (code %d)",
                  cause, event.getCause(), event.getCode())
        return event

    def snapshot(phase):
        point = {"phase": phase, "tick": int(m5.curTick()),
                 "insts": [int(cpu.getCurrentInstCount(t)) for t in range(2)]}
        report["phases"].append(point)
        print("SMT WFI loop: %s tick=%d T0=%d T1=%d" %
              (phase, point["tick"], *point["insts"]))
        return point

    simulate_to(max(warmup, minimum), "SMT WFI loop warmup complete")
    start = snapshot("measurement_start")
    if start["insts"][1] == 0:
        fatal("T1 made no progress before WFI loop measurement")
    if start["insts"][0] >= total:
        fatal("T0 warmup consumed the measurement interval")
    m5.stats.dump()
    m5.stats.reset()
    event = simulate_to(total, "SMT WFI loop measurement complete")
    end = snapshot("measurement_end")
    report["measurement_insts"] = [
        end["insts"][t] - start["insts"][t] for t in range(2)]
    report["measurement_ticks"] = end["tick"] - start["tick"]
    if min(report["measurement_insts"]) <= 0:
        fatal("Both T0 and the software WFI loop must keep committing")
    m5.stats.dump()
    path = Path(m5.options.outdir) / "smt-wfi-loop.json"
    path.write_text(json.dumps(report, indent=2) + "\n")
    return event
