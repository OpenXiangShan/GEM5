"""Exercise recovery or a distinct ICache clock while reusing se.py."""

import argparse
from pathlib import Path
import runpy
import sys

import m5
from m5.objects import DerivO3CPU, Root, SrcClockDomain


parser = argparse.ArgumentParser(add_help=False)
parser.add_argument(
    "--two-fetch-test",
    choices=("recovery", "slow_cache", "switch"),
    default="recovery",
)
options, remaining = parser.parse_known_args()
sys.argv[1:] = remaining


simulate = m5.simulate
exercised = False


def exercise_recovery(*args, **kwargs):
    global exercised
    if not exercised:
        exercised = True
        system = Root.getInstance().system
        if options.two_fetch_test == "switch":
            event = simulate(200000)
            assert event.getCause() == "simulate() limit reached"
            # Both CPUs use timing memory and this test disables prefetchers.
            m5.drain()
            system.cpu[0].switchOut()
            system.test_switch_cpu.takeOverFrom(system.cpu[0])
            print("TwoFetch test: CPU switch completed", flush=True)
            return simulate(*args, **kwargs)
        for phase in range(4):
            event = simulate(50000)
            assert event.getCause() == "simulate() limit reached"
            system.cpu[0].flushTLBs()
            print(f"TwoFetch test: TLB/frontend flush {phase}", flush=True)
        m5.drain()
        print("TwoFetch test: drain completed; resuming", flush=True)
    return simulate(*args, **kwargs)


root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "configs"))
if options.two_fetch_test in ("recovery", "switch"):
    m5.simulate = exercise_recovery
if options.two_fetch_test in ("slow_cache", "switch"):
    from common import Simulation

    run = Simulation.run

    def run_with_test_cpu(args, root, system, future_class):
        if options.two_fetch_test == "slow_cache":
            system.cpu[0].icache.clk_domain = SrcClockDomain(
                clock="1GHz", voltage_domain=system.voltage_domain
            )
        else:
            from common.FUScheduler import KMHV3Scheduler

            system.test_switch_cpu = DerivO3CPU(
                switched_out=True,
                cpu_id=0,
                system=system,
                workload=system.cpu[0].workload,
                clk_domain=system.cpu[0].clk_domain,
                isa=system.cpu[0].isa,
                enableTwoFetch=True,
                enablePredecode=True,
                enableMoveElimination=False,
                enableConstantFolding=False,
                scheduler=KMHV3Scheduler(),
            )
            system.test_switch_cpu.createThreads()
        return run(args, root, system, future_class)

    Simulation.run = run_with_test_cpu

runpy.run_path(str(root / "configs/example/se.py"), run_name="__m5_main__")
