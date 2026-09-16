#!/usr/bin/env python3
"""Run same-line store-miss merge, forwarding and fence checks."""

import argparse
import json
from pathlib import Path
import subprocess


def run(command, log=None):
    if log is None:
        subprocess.run(command, check=True)
    else:
        with log.open("w") as output:
            subprocess.run(command, stdout=output, stderr=subprocess.STDOUT,
                           check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gem5", default="build/RISCV/gem5.opt")
    parser.add_argument("--outdir", required=True)
    parser.add_argument("--ref-so")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    out = Path(args.outdir).resolve()
    out.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).with_name("check.S")
    elf, binary = out / "check.elf", out / "check.bin"
    run(["riscv64-linux-gnu-gcc", "-nostdlib", "-static", "-march=rv64gc",
         "-mabi=lp64d", "-Wl,-Ttext=0x80000000", "-Wl,--build-id=none",
         "-o", str(elf), str(source)])
    run(["riscv64-linux-gnu-objcopy", "-O", "binary", str(elf), str(binary)])
    results = {}
    cases = [("merge", True, 32), ("target_pressure", True, 2),
             ("legacy_release", False, 32)]
    for name, release, targets in cases:
        dest = out / name
        dest.mkdir(exist_ok=True)
        command = [str(Path(args.gem5).resolve()), f"--outdir={dest}",
                   "--debug-flags=StoreBuffer", "--debug-file=store.trace",
                   str(root / "configs/example/kmhv3.py"), "--raw-cpt",
                   f"--generic-rv-cpt={binary}", "--mem-type=SimpleMemory",
                   "--mem-size=256MB", "--maxinsts=100000",
                   "--param=system.cpu[0].SbufferEntries=2",
                   "--param=system.cpu[0].SbufferEvictThreshold=0",
                   f"--param=system.cpu[0].sbufferReleaseOnMiss={release}",
                   f"--param=system.cpu[0].dcache.tgts_per_mshr={targets}",
                   "--param=system.mem_ctrls[0].latency='500ns'"]
        if args.ref_so:
            command += ["--enable-difftest", f"--difftest-ref-so={args.ref_so}"]
        else:
            command += ["--disable-difftest"]
        (dest / "command.json").write_text(json.dumps(command, indent=2) + "\n")
        run(command, dest / "sim.log")
        log = (dest / "sim.log").read_text()
        assert "because m5_exit instruction encountered" in log, (
            f"Self-check failed: {dest}/sim.log")
        stats = {}
        for line in (dest / "stats.txt").read_text().splitlines():
            fields = line.split()
            if len(fields) >= 2 and fields[0].startswith("system.cpu.lsq."):
                try:
                    stats[fields[0].rsplit(".", 1)[1]] = float(fields[1])
                except ValueError:
                    pass
        merged = stats["sbufferMissMerged"]
        if release:
            assert merged > 0, "Test did not exercise same-line MSHR merging"
            assert stats["sbufferMissForward"] > 0, "No miss-data forwarding"
        else:
            assert merged == 0, "Legacy release mode unexpectedly merged"
        results[name] = stats
    (out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    print(f"PASS: {out / 'results.json'}")


if __name__ == "__main__":
    main()
