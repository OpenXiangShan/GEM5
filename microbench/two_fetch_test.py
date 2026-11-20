#!/usr/bin/env python3
"""Build deterministic paired-fetch layouts and run the three model settings."""

import argparse
import configparser
import json
import re
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gem5", type=Path)
    parser.add_argument("--compiler", default="riscv64-linux-gnu-gcc")
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--layouts", nargs="+", help="Run only these layouts")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    gem5 = (args.gem5 or root / "build/RISCV/gem5.opt").resolve()
    out = root / "microbench/output/two_fetch"
    binaries = root / "microbench/build/two_fetch"
    binaries.mkdir(parents=True, exist_ok=True)
    rows = []
    for layout, offset, first in [
        ("near", 16, 0),
        ("disjoint", 136, 0),
        ("conflict", 128, 0),
        ("four_lines", 128, 60),
        ("hot_disjoint", 136, 0),
        ("hot_conflict", 128, 0),
    ]:
        if args.layouts and layout not in args.layouts:
            continue
        binary = binaries / layout
        bare = layout.startswith("hot_")
        subprocess.run(
            [
                args.compiler,
                "-O2",
                "-g",
                "-static",
                f"-DBLOCK_OFFSET={offset}",
                f"-DFIRST_OFFSET={first}",
                "-o",
                str(binary),
                str(root / "microbench/two_fetch.c"),
            ]
            + (
                ["-DFREESTANDING", "-nostdlib", "-Wl,-e,_start"]
                if bare
                else []
            ),
            check=True,
        )
        modes = [
            ("off", False, 0),
            ("pair", True, 0),
            ("bank", True, 8),
        ]
        if bare:
            modes = modes[1:]
        if layout == "hot_disjoint":
            modes += [("switch", True, 0)]
        if layout == "four_lines":
            modes += [
                ("tag1", True, 0),
                ("mshr1", True, 0),
                ("slow_cache", True, 0),
                ("recovery", True, 0),
            ]
        for mode, enabled, bank_bytes in modes:
            run = out / f"{layout}_{mode}"
            run.mkdir(parents=True, exist_ok=True)
            cmd = [
                str(gem5),
                "-d",
                str(run),
                "configs/example/se.py",
                "--classic-l2",
                "--no-l3cache",
                "--no-pf",
                "--l1i_assoc=4",
                "--mem-size=512MB",
                "-c",
                str(binary),
                "-P",
                f"system.cpu[0].enableTwoFetch={enabled}",
                "-P",
                "system.cpu[0].enablePredecode=True",
                "-P",
                f"system.cpu[0].icache.icache_data_bank_bytes={bank_bytes}",
            ]
            if bare:
                cmd[1:1] = [
                    "--debug-flags=DecoupleBP",
                    "--debug-end=1000000",
                    "--debug-file=two_fetch.trace",
                ]
                cmd += ["-P", "system.cpu[0].icache.tag_latency=4"]
            config_index = cmd.index("configs/example/se.py")
            if mode in ("tag1", "mshr1"):
                cmd += ["-P", "system.cpu[0].icache.tag_load_read_ports=1"]
            if mode == "mshr1":
                cmd += [
                    "-P",
                    "system.cpu[0].icache.mshrs=1",
                    "-P",
                    "system.cpu[0].icache.demand_mshr_reserve=0",
                ]
            if mode == "slow_cache":
                cmd[config_index] = "microbench/two_fetch_recovery.py"
                cmd += [
                    "-P",
                    "system.cpu[0].icache.tag_load_read_ports=1",
                    "--two-fetch-test=slow_cache",
                ]
            if mode == "recovery":
                cmd[config_index] = "microbench/two_fetch_recovery.py"
            if mode == "switch":
                cmd[config_index] = "microbench/two_fetch_recovery.py"
                cmd += ["--two-fetch-test=switch"]
            (run / "command.json").write_text(json.dumps(cmd, indent=2) + "\n")
            with (run / "console.log").open("w") as log:
                subprocess.run(
                    cmd,
                    cwd=root,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    timeout=args.timeout,
                    check=True,
                )
            config = configparser.ConfigParser(
                interpolation=None, strict=False
            )
            config.read(run / "config.ini")
            assert (
                config["system.cpu"]["enableTwoFetch"] == str(enabled).lower()
            )
            assert (
                config["system.cpu"]["icache_port"]
                == "system.cpu.icache.cpu_side"
            )
            assert config["system.cpu.icache"]["assoc"] == "4"
            assert config["system.cpu.icache"][
                "icache_data_bank_bytes"
            ] == str(bank_bytes)
            stats = {}
            for line in (run / "stats.txt").read_text().splitlines():
                words = line.split()
                if len(words) >= 2 and words[0].startswith(
                    ("system.cpu.", "system.test_switch_cpu.")
                ):
                    try:
                        key = words[0].split(".", 2)[2]
                        value = float(words[1])
                        if "twoFetch" in key or key in (
                            "committedInsts",
                            "numCycles",
                        ):
                            stats[key] = stats.get(key, 0) + value
                        else:
                            stats[key] = value
                    except ValueError:
                        pass
            log = (run / "console.log").read_text()
            assert "Simulated exit code not 0" not in log, log[-2000:]
            assert "exiting with last active thread context" in log, log[
                -2000:
            ]
            if enabled and not (layout == "hot_conflict" and mode == "bank"):
                assert stats.get("fetch.twoFetchIssued", 0) > 0, stats
                assert stats.get("fetch.twoFetchSuccesses", 0) > 0, stats
            if mode in ("tag1", "mshr1", "slow_cache"):
                assert stats["fetch.twoFetchTagWaits"] > 0, stats
            if mode == "mshr1":
                assert stats["fetch.twoFetchCacheRetries"] > 0, stats
            if layout == "conflict" and mode == "bank":
                assert stats["fetch.twoFetchRejected::bank"] > 0, stats
            if bare:
                if layout == "hot_conflict" and mode == "bank":
                    assert stats["fetch.twoFetchRejected::bank"] > 100, stats
                else:
                    assert stats["fetch.twoFetchSuccesses"] > 100, stats
                    symbols = subprocess.check_output(
                        ["riscv64-linux-gnu-nm", "-n", str(binary)], text=True
                    )
                    starts = {
                        name: int(addr, 16)
                        for addr, _, name in (
                            line.split()
                            for line in symbols.splitlines()
                            if len(line.split()) == 3
                        )
                        if name in ("paired_b0", "paired_b1")
                    }
                    trace = (run / "two_fetch.trace").read_text()
                    groups = re.findall(
                        r"blocks=2 FTQ=\d+/(\d+) ranges=\[(0x[0-9a-f]+),.*?/\[(0x[0-9a-f]+),",
                        trace,
                    )
                    consumed = set(
                        re.findall(r"2Fetch: continue with FTQ (\d+)", trace)
                    )
                    assert any(
                        ftq in consumed
                        and {int(a, 16), int(b, 16)} == set(starts.values())
                        for ftq, a, b in groups
                    ), "Hot loop did not consume an independent pair"
            if mode == "recovery":
                assert "drain completed; resuming" in log
            if mode == "switch":
                assert "CPU switch completed" in log
                assert (
                    config["system.test_switch_cpu"]["enableTwoFetch"]
                    == "true"
                )
            if mode == "slow_cache":
                assert (
                    config["system.cpu.icache.clk_domain"]["clock"] == "1000"
                )
                assert config["system.cpu_clk_domain"]["clock"] == "333"
            stats["ipc"] = stats["committedInsts"] / stats["numCycles"]
            rows.append(
                {
                    "layout": layout,
                    "mode": mode,
                    "ipc": stats.get("ipc"),
                    **{k: v for k, v in stats.items() if "twoFetch" in k},
                }
            )
            print(f"PASS {layout}/{mode}: IPC={stats.get('ipc')}", flush=True)
        if layout == "four_lines":
            pair = next(
                row
                for row in rows
                if row["layout"] == layout and row["mode"] == "pair"
            )
            assert pair["fetch.twoFetchTagWaits"] > 0, pair
    summary = (
        "summary_" + "_".join(args.layouts) if args.layouts else "summary"
    )
    (out / f"{summary}.json").write_text(json.dumps(rows, indent=2) + "\n")


if __name__ == "__main__":
    main()
