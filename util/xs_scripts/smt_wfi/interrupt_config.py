"""gem5 configuration used by run_smoke.py to exercise WFI interrupts."""

import json
import os
from pathlib import Path
import runpy
import sys

import m5

repo = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(repo / "configs"), str(repo / "configs/example")]
from common import SMTWfiLoop


def run(options, system, maxtick):
    cpu = system.cpu[0]
    enabled = os.environ["SMT_WFI_SMOKE_CASE"] == "wake"
    points = []

    def simulate(ticks, expected):
        event = m5.simulate(ticks)
        if event.getCause() != expected or event.getCode() != 0:
            raise RuntimeError(f"expected {expected}, got {event.getCause()}")
        return event

    def snapshot(label):
        point = {
            "phase": label,
            "tick": int(m5.curTick()),
            "insts": [int(cpu.getCurrentInstCount(t)) for t in range(2)],
            "pc": int(cpu.getThreadPC(1)),
            "waiting": cpu.isWfiWaiting(1),
            "entries": int(cpu.getWfiEntries(1)),
            "wakeups": int(cpu.getWfiWakeups(1)),
            "allocated": int(cpu.numActiveThreads()),
            "runnable": int(cpu.numRunnableThreads()),
            "rob_lsq": SMTWfiLoop.rob_lsq_state(cpu),
            "prediction": SMTWfiLoop.prediction_state(cpu),
        }
        capacity = [int(cpu.numROBEntries), int(cpu.LQEntries),
                    int(cpu.SQEntries)]
        resources = point["rob_lsq"]
        for index, total in enumerate(capacity):
            used = sum(r["used"][index] for r in resources)
            if used > total:
                raise RuntimeError("ROB/LQ/SQ physical capacity exceeded")
            for resource in resources:
                expected_free = min(
                    max(0, resource["limits"][index] - resource["used"][index]),
                    total - used)
                if resource["free"][index] != expected_free:
                    raise RuntimeError("incorrect resource credit after wake")
        if point["waiting"]:
            if (resources[0]["limits"] != capacity or
                    resources[1]["active"] or
                    resources[1]["limits"] != [0, 0, 0]):
                raise RuntimeError("Waiting did not relinquish its quotas")
        elif not all(r["active"] for r in resources):
            raise RuntimeError("wake/cleanup must retain both resource identities")
        predicted = point["prediction"]
        total = int(cpu.branchPred.ftq_size)
        used = sum(state[1] for state in predicted)
        if used > total:
            raise RuntimeError("FTQ physical capacity exceeded")
        active = sum(resource["active"] for resource in resources)
        for resource, state in zip(resources, predicted):
            limit = total // active if resource["active"] else 0
            if (state[0] != limit or
                    state[2] != min(max(0, limit - state[1]), total - used)):
                raise RuntimeError("incorrect FTQ quota/credits after wake")
            for name, banks in zip(("ubtb", "abtb", "microtage", "mbtb",
                                    "tage", "ittage", "mgsc"), state[3:]):
                component = getattr(cpu.branchPred, name)
                expected = 0
                if resource["active"] and bool(component.enabled):
                    expected = 1 if (active == 2 and
                                     bool(component.smtTidPartitioned)) else 2
                if banks != expected:
                    raise RuntimeError("incorrect predictor capacity: " + name)
        points.append(point)
        print("WFI interrupt test", json.dumps(point), flush=True)
        if point["allocated"] != 2:
            raise RuntimeError("WFI changed resource allocation identity")
        return point

    event = simulate(10_000_000_000, "WFI waiting")
    first = snapshot("first_wait")
    if not first["waiting"] or first["entries"] != 1:
        raise RuntimeError("T1 did not enter its first WFI")
    cpu.postInterrupt(1, 3, 0)  # MSIP
    if enabled:
        simulate(1000, "simulate() limit reached")
        awake = snapshot("interrupt_wake")
        if awake["waiting"] or awake["wakeups"] != 1:
            raise RuntimeError("locally enabled interrupt did not wake T1")
        if awake["pc"] != first["pc"]:
            raise RuntimeError("WFI did not resume at its following instruction")
        cpu.clearInterrupt(1, 3, 0)
        # Stop at the second WFI's retirement, before its squash has drained.
        cpu.scheduleInstStop(1, 2, "second WFI retired")
        simulate(100_000_000, "second WFI retired")
        cleaning = snapshot("second_retired")
        if cleaning["entries"] != 2 or cleaning["waiting"]:
            raise RuntimeError("expected second WFI cleanup in progress")
        # A short interrupt during cleanup must be latched until it is safe
        # to restart. Deassert it before the next CPU tick.
        cpu.postInterrupt(1, 3, 0)
        cpu.clearInterrupt(1, 3, 0)
        event = simulate(100_000_000, "WFI waiting")
        last = snapshot("third_wait")
        if (not last["waiting"] or last["entries"] != 3 or
                last["wakeups"] != 2 or
                last["insts"][1] != first["insts"][1] + 4):
            raise RuntimeError("lost wake during WFI cleanup or bad resume PC")
        # After the third wait, SPEC may occupy more than its restored quota.
        # Wake once more and inspect every credit using the saturating formula
        # above; existing work must remain intact and T1 must still progress.
        cpu.postInterrupt(1, 3, 0)
        simulate(1000, "simulate() limit reached")
        snapshot("quota_reclaimed")
        cpu.clearInterrupt(1, 3, 0)
        event = simulate(100_000_000, "WFI waiting")
        last = snapshot("fourth_wait")
        if (last["entries"] != 4 or last["wakeups"] != 3 or
                last["insts"][1] != first["insts"][1] + 6):
            raise RuntimeError("wake stalled while reclaiming shared resources")
    else:
        event = simulate(30_000_000, "simulate() limit reached")
        last = snapshot("masked_still_waiting")
        if (not last["waiting"] or last["wakeups"] != 0 or
                last["insts"][1] != first["insts"][1] or
                last["insts"][0] <= first["insts"][0]):
            raise RuntimeError("masked interrupt ended WFI or T0 lost progress")
        cpu.clearInterrupt(1, 3, 0)
    if last["runnable"] != 1:
        raise RuntimeError("T1 did not remain in WFI")
    (Path(m5.options.outdir) / "interrupt-result.json").write_text(
        json.dumps({"passed": True, "enabled": enabled, "phases": points},
                   indent=2) + "\n")
    return event


SMTWfiLoop.run = run
runpy.run_path(str(repo / "configs/example/smt_idealkmhv3.py"),
               run_name="__m5_main__")
