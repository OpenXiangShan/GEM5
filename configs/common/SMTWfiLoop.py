"""Measure T0 while T1 waits after retiring a real WFI."""

import hashlib
import json
from pathlib import Path

import m5
from m5.util import fatal


def rob_lsq_state(cpu):
    """Read actual admission limits/occupancy, without altering allocation."""
    result = []
    for tid in range(int(cpu.numThreads)):
        rows = cpu.getRobLsqState(tid)
        result.append({"active": cpu.isRobLsqActive(tid),
                       "limits": [int(row[0]) for row in rows],
                       "used": [int(row[1]) for row in rows],
                       "free": [int(row[2]) for row in rows]})
    return result


def run(options, system, maxtick):
    cpu = system.cpu[0]
    warmup = options.warmup_insts_no_switch or 0
    total = options.maxinsts
    minimum = options.smt_wfi_loop_restore_insts
    if warmup < 0 or minimum < 0 or not total or total <= max(warmup, minimum):
        fatal("WFI measurement needs 0 <= warmup/restore < --maxinsts")
    restorer = Path(str(system.gcpt_restorer_file))
    manifest_path = options.smt_wfi_loop_manifest
    if not manifest_path:
        manifest_path = restorer.with_name("manifest.json")
    manifest = json.loads(Path(manifest_path).read_text())
    if manifest.get("experiment") != "wfi-loop":
        fatal("Use the restorer manifest produced by smt_wfi/build_restorer.py")
    binary = restorer.read_bytes()
    if hashlib.sha256(binary).hexdigest() != manifest["binary_sha256"]:
        fatal("WFI restorer hash does not match its manifest")
    offset = manifest["wfi_offset"]
    if binary[offset:offset + 4] != bytes.fromhex("73005010"):
        fatal("Restorer symbol must point to a real WFI instruction")
    report = {"experiment": "wfi-wait", "measured_tid": 0, "waiting_tid": 1,
              "num_threads": int(cpu.numThreads),
              "rob_lsq_capacity": [int(cpu.numROBEntries), int(cpu.LQEntries),
                                   int(cpu.SQEntries)],
              "checkpoint": str(system.gcpt_file), "restorer": str(restorer),
              "restorer_sha256": manifest["binary_sha256"],
              "wfi_pc": manifest["wfi_pc"],
              "warmup_total_insts": warmup, "stop_total_insts": total,
              "phases": []}

    def simulate(cause, deadline):
        event = m5.simulate(max(0, min(maxtick, deadline) - m5.curTick()))
        if event.getCause() != cause or event.getCode() != 0:
            fatal("WFI expected '%s', got '%s' (code %d)",
                  cause, event.getCause(), event.getCode())
        return event

    def simulate_to(target, cause):
        remaining = target - int(cpu.getCurrentInstCount(0))
        if remaining <= 0:
            return
        cpu.scheduleInstStop(0, remaining, cause)
        return simulate(cause, maxtick)

    def snapshot(phase):
        if not cpu.isWfiWaiting(1) or cpu.numActiveThreads() != 2:
            fatal("T1 must stay in WFI with two hardware allocation identities")
        if cpu.numRunnableThreads() != 1:
            fatal("Only T0 may be runnable during WFI measurement")
        point = {"phase": phase, "tick": int(m5.curTick()),
                 "insts": [int(cpu.getCurrentInstCount(t)) for t in range(2)],
                 "pcs": [int(cpu.getThreadPC(t)) for t in range(2)],
                 "allocation_threads": int(cpu.numActiveThreads()),
                 "runnable_threads": int(cpu.numRunnableThreads()),
                 "rob_lsq": rob_lsq_state(cpu),
                 "wfi_entries": int(cpu.getWfiEntries(1)),
                 "wfi_wakeups": int(cpu.getWfiWakeups(1))}
        # Waiting preserves the hardware context, but releases all unused
        # ROB/LQ/SQ reservations. Inspect the same limits used for admission.
        resources = point["rob_lsq"]
        if (not resources[0]["active"] or resources[1]["active"] or
                resources[0]["limits"] != report["rob_lsq_capacity"] or
                resources[1]["limits"] != [0, 0, 0] or
                resources[1]["used"] != [0, 0, 0]):
            fatal("WFI did not release its ROB/LQ/SQ reservations")
        report["phases"].append(point)
        print("SMT WFI: %s tick=%d T0=%d T1=%d allocation=2 runnable=1" %
              (phase, point["tick"], *point["insts"]))
        return point

    simulate("WFI waiting", m5.curTick() + 10000000000)
    ready = snapshot("wfi_ready")
    if int(cpu.getWfiPC(1)) != manifest["wfi_pc"]:
        fatal("T1 did not retire the restorer WFI")
    simulate_to(max(warmup, minimum), "SMT WFI warmup complete")
    start = snapshot("measurement_start")
    if start["insts"][0] >= total:
        fatal("T0 warmup consumed the measurement interval")
    restorer_begin = 0x80000000
    restorer_end = restorer_begin + int(system.gcpt_restorer_size_limit)
    if restorer_begin <= start["pcs"][0] < restorer_end:
        fatal("T0 is still inside the restorer at measurement start")
    m5.stats.dump()
    m5.stats.reset()
    event = simulate_to(total, "SMT WFI measurement complete")
    end = snapshot("measurement_end")
    if any(p["insts"][1] != ready["insts"][1] for p in (start, end)):
        fatal("T1 committed instructions after entering WFI")
    report["measurement_insts"] = [
        end["insts"][t] - start["insts"][t] for t in range(2)]
    report["measurement_ticks"] = end["tick"] - start["tick"]
    if report["measurement_insts"][0] <= 0:
        fatal("T0 made no progress during WFI measurement")
    m5.stats.dump()
    path = Path(m5.options.outdir) / "smt-wfi-loop.json"
    path.write_text(json.dumps(report, indent=2) + "\n")
    return event
