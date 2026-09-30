#!/usr/bin/env python3
"""Run APF instruction/recovery regressions with mandatory difftest."""

import argparse
import json
from pathlib import Path
import re
import subprocess


def run(command, log, timeout):
    with log.open("w") as output:
        subprocess.run(
            command,
            stdout=output,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=True,
        )


def read_stats(path):
    result = {}
    for line in path.read_text().splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[0].startswith("system.cpu.apf."):
            result[fields[0].removeprefix("system.cpu.apf.")] = float(
                fields[1]
            )
    return result


def check_prediction_blocks(path, width):
    """Check that multiple decode cycles share one query and FTQ record."""
    queries = set()
    prefixes = {}
    for line in path.read_text().splitlines():
        query = re.search(
            r"APF query source (\S+) block (\d+) start (\S+)", line
        )
        if query:
            source, block, start = query.groups()
            key = (source, block, start)
            assert key not in queries, ("block queried twice", key)
            queries.add(key)
        prefix = re.search(
            r"APF prefix source (\S+) block (\d+) uops (\d+) start (\S+)", line
        )
        if prefix:
            source, block, uops, start = prefix.groups()
            key = (source, block, start)
            assert key in queries, ("prefix without query", key)
            previous = prefixes.get(key, 0)
            assert previous < int(uops) <= previous + width, (
                key,
                previous,
                uops,
            )
            prefixes[key] = int(uops)
    assert prefixes and max(prefixes.values()) > width, prefixes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--gem5", type=Path, default=Path("build/RISCV/gem5.opt")
    )
    parser.add_argument("--ref", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument(
        "--case",
        choices=[
            "off",
            "default",
            "ftq",
            "slow",
            "amo",
            "fence",
            "blocks",
            "fence-off",
            "fence-empty",
        ],
        action="append",
    )
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    args.out.mkdir(parents=True, exist_ok=True)
    out = args.out.resolve()
    gem5 = args.gem5.resolve()
    ref = args.ref.resolve(strict=True)
    summary = {}
    for name in args.case or [
        "off",
        "default",
        "ftq",
        "slow",
        "amo",
        "fence",
        "blocks",
        "fence-off",
        "fence-empty",
    ]:
        elf = out / f"{name}.elf"
        binary = out / f"{name}.bin"
        command = [
            "riscv64-linux-gnu-gcc",
            "-march=rv64gc",
            "-mabi=lp64d",
            "-nostdlib",
            "-nostartfiles",
            "-static",
            "-Wl,-Ttext=0x80000000",
            "-Wl,--build-id=none",
            str(root / "util/apf/smoke.S"),
            "-o",
            str(elf),
        ]
        if name == "amo":
            command.append("-DAPF_AMO")
        if name.startswith("fence"):
            command.append("-DAPF_FENCE")
        if name in ("fence-off", "fence-empty"):
            command.append("-DAPF_FENCE_DENSE")
        run(command, out / f"{name}-build.log", args.timeout)
        subprocess.run(
            ["riscv64-linux-gnu-objcopy", "-O", "binary", elf, binary],
            check=True,
        )
        enabled = name not in ("off", "fence-off")
        params = [f"system.cpu[0].enableAPF={enabled}"]
        if name in ("fence-off", "fence-empty"):
            params.append("system.cpu[0].branchPred.enable_h2p_table=False")
        if name == "ftq":
            params.append("system.cpu[0].branchPred.ftq_size=8")
        elif name == "slow":
            params += [
                "system.cpu[0].apfWidth=2",
                "system.cpu[0].apfFetchLatency=2",
            ]
        elif name == "blocks":
            params += [
                "system.cpu[0].apfWidth=2",
                "system.cpu[0].apfGenerationCycles=52",
            ]
        command = [
            str(gem5),
            f"--outdir={out / name}",
        ]
        if name == "blocks":
            command += [
                "--debug-flags=APF",
                "--debug-file=apf.trace",
                "--debug-end=30000000",
            ]
        command += [
            str(root / "configs/example/kmhv3.py"),
            "--raw-cpt",
            f"--generic-rv-cpt={binary}",
            f"--difftest-ref-so={ref}",
            "--mem-size=256MB",
            "--maxinsts=2000000",
        ]
        for param in params:
            command.append(f"--param={param}")
        log = out / f"{name}.log"
        run(command, log, args.timeout)
        if "m5_exit instruction encountered" not in log.read_text():
            raise RuntimeError(
                f"{name} did not reach the end of the test: {log}"
            )
        stats = read_stats(out / name / "stats.txt")
        if name == "fence-empty":
            assert stats["started"] == stats["replayedUops"] == 0, stats
        elif enabled:
            assert stats["recoveredCommitted"] > 0, stats
            assert 0 < stats["committedUops"] <= stats["replayedUops"], stats
            assert (
                stats["recoveredCommitted"]
                <= stats["committedConditionalMisses"]
            ), stats
            if name == "ftq":
                assert stats["ftqWaitCycles"] > 0, stats
                assert stats["replayBlockedWithRecoveryCycles"] > 0, stats
                assert (
                    stats["replayBlockedWithRecoveryCycles"]
                    <= stats["replayBlockedCycles"]
                ), stats
            if name == "blocks":
                check_prediction_blocks(out / name / "apf.trace", 2)
            if name == "amo":
                assert stats["generatedMicroops"] > 0, stats
            if name == "default":
                assert stats["crossPageReads"] > 0, stats
                assert stats["compressedUops"] > 0, stats
                assert stats["savedOccupancy::4"] > 0, stats
                assert stats["fullCycles"] > 0, stats
        else:
            assert not stats, stats
        summary[name] = stats
        print(f"{name}: PASS", flush=True)
    if "fence-off" in summary and "fence-empty" in summary:

        def architectural_stats(name):
            return {
                fields[0]: fields[1]
                for line in (out / name / "stats.txt").read_text().splitlines()
                if len(fields := line.split()) >= 2
                and fields[0]
                in (
                    "simInsts",
                    "system.cpu.numCycles",
                    "system.cpu.ipc",
                    "system.cpu.commit.squashDueToSquashAfter",
                )
            }

        assert architectural_stats("fence-off") == architectural_stats(
            "fence-empty"
        )
        print(
            "fence-off/fence-empty: identical cycles, IPC and squashes",
            flush=True,
        )
    (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()
