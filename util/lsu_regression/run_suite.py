#!/usr/bin/env python3
"""Run current-version LSU regression cases with explicit provenance and bounded parallelism."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import support as h

HERE = Path(__file__).resolve().parent
DEFAULT_CASES = [f"fs{i}" for i in range(11)] + [
    "store_cold",
    "store_warm",
    "negative",
    "se0",
    "se3",
    "se4",
    "vector",
    "vector_mask",
    "vector_pressure",
    "se_smt",
]


def check_exit(code, exits, kind, log, expect_success=True):
    if code != 0 or not exits:
        return False
    if not expect_success:
        return "max instruction count" in exits[-1][1]
    if kind == "se":
        checks = re.findall(
            r"CASE=\d+ checksum=\d+ expected=\d+ failures=\d+", log
        )
        return bool(
            checks
            and all("failures=0" in v for v in checks)
            and "last active thread context" in exits[-1][1]
        )
    return "m5_exit instruction" in exits[-1][1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ("repo", "gem5", "output", "ref"):
        p.add_argument("--" + name, type=Path, required=True)
    p.add_argument(
        "--group", choices=["smoke", "positive", "full"], default="full"
    )
    p.add_argument("--cases", nargs="+", choices=DEFAULT_CASES + ["fs10_deep"])
    p.add_argument(
        "--fixed-workloads",
        type=Path,
        help="Prebuilt immutable ELF/image directory shared by both sides; prevents SE argv stack differences",
    )
    p.add_argument(
        "--source-manifest",
        type=Path,
        help="Build source provenance for immutable binary; actual working tree is also recorded",
    )
    p.add_argument("--jobs", type=int, choices=range(1, 5), default=3)
    a = p.parse_args()
    if a.cases is None:
        a.cases = (
            ["fs0", "store_cold", "negative", "vector_pressure"]
            if a.group == "smoke"
            else [
                n
                for n in DEFAULT_CASES
                if a.group == "full" or n != "negative"
            ]
        )
    repo, binary, out, ref = [
        getattr(a, k).resolve() for k in ("repo", "gem5", "output", "ref")
    ]
    if out == repo or out.is_relative_to(repo / "src"):
        p.error("--output must be a separate results directory")
    for path in (binary, ref):
        if not path.is_file():
            p.error(f"Missing {path}")
    for tool in (
        "riscv64-linux-gnu-gcc",
        "riscv64-linux-gnu-objcopy",
        "riscv64-linux-gnu-objdump",
    ):
        if not shutil.which(tool):
            p.error(f"Missing {tool}")
    out.mkdir(parents=True, exist_ok=False)
    source_paths = sorted((repo / "src/cpu/o3").glob("*.cc")) + sorted(
        (repo / "src/cpu/o3").glob("*.hh")
    )
    manifest = {
        "head": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=repo, text=True
        ).strip(),
        "time_utc": datetime.now(timezone.utc).isoformat(),
        "binary": str(binary),
        "binary_sha256": h.sha(binary),
        "reference": str(ref),
        "reference_sha256": h.sha(ref),
        "source_sha256": {
            str(v.relative_to(repo)): h.sha(v) for v in source_paths
        },
        "test_sha256": {
            v.name: h.sha(v)
            for v in HERE.iterdir()
            if v.is_file() and v.suffix in (".py", ".S", ".ld", ".c")
        },
        "compiler": subprocess.check_output(
            ["riscv64-linux-gnu-gcc", "--version"], text=True
        ).splitlines()[0],
        "cases": a.cases,
        "jobs": a.jobs,
    }
    if a.source_manifest:
        build_source = json.loads(a.source_manifest.read_text())
        manifest["binary_source_provenance"] = {
            "manifest": str(a.source_manifest.resolve()),
            "sha256": h.sha(a.source_manifest),
            "build": build_source,
        }
        manifest["working_tree_is_binary_source"] = False
    else:
        manifest["working_tree_is_binary_source"] = True
    if a.fixed_workloads:
        manifest["fixed_workloads"] = {
            "path": str(a.fixed_workloads.resolve()),
            "manifest_sha256": h.sha(a.fixed_workloads / "manifest.json"),
        }
    h.write_json(out / "manifest.json", manifest)
    (out / "source.diff").write_text(
        subprocess.check_output(
            ["git", "diff", "--", "src", "configs"], cwd=repo, text=True
        )
    )
    (out / "worktree-status.txt").write_text(
        subprocess.check_output(
            ["git", "status", "--short"], cwd=repo, text=True
        )
    )

    def run(name):
        d = out / name
        d.mkdir()
        elf = d / "test.elf"
        image = d / "test.bin"
        cmd = ["riscv64-linux-gnu-gcc", "-static", "-Wl,--build-id=none"]
        kind = "se" if name.startswith("se") else "fs"
        if kind == "se":
            if name == "se_smt":
                cmd += [
                    "-nostdlib",
                    "-march=rv64gc",
                    "-mabi=lp64d",
                    "-Wl,-Ttext=0x10000",
                    str(HERE / "se_smt.S"),
                ]
            else:
                cmd += [
                    "-O2",
                    "-march=rv64gc",
                    "-mabi=lp64d",
                    f"-DCASE={name[2:]}",
                    str(HERE / "memory_cases.c"),
                ]
        else:
            cmd += [
                "-nostdlib",
                "-mabi=lp64d",
                "-mno-relax",
                "-fno-pic",
                "-fno-pie",
                "-Wl,--no-relax",
            ]
            if name.startswith("vector"):
                cmd += [
                    "-march=rv64gcv",
                    "-Wl,-Ttext=0x80000000",
                    str(HERE / "vector_cross.S"),
                ]
                if name == "vector_mask":
                    cmd += ["-DTEST_MASKED"]
            elif name.startswith("store"):
                cmd += [
                    "-march=rv64gc",
                    f'-DWARM={int(name=="store_warm")}',
                    "-DEXPECT_BAD=0",
                    f'-Wl,-T,{HERE/"ordinary_store.ld"}',
                    str(HERE / "ordinary_store.S"),
                ]
            else:
                cmd += [
                    "-march=rv64gc",
                    f'-DCASE={10 if name == "fs10_deep" else name[2:] if name.startswith("fs") else 0}',
                    f'-DEXPECT_BAD={int(name=="negative")}',
                    f'-Wl,-T,{HERE/"ordinary_store.ld"}',
                    str(HERE / "paths.S"),
                ]
        cmd += ["-o", str(elf)]
        compiled = h.run_record(cmd, repo, d / "compile.log")
        if compiled["returncode"]:
            return {
                "case": name,
                "check_passed": False,
                "compile_failed": True,
            }
        subprocess.run(
            [
                "riscv64-linux-gnu-objcopy",
                "-O",
                "binary",
                str(elf),
                str(image),
            ],
            check=True,
        )
        (d / "disassembly.txt").write_text(
            subprocess.check_output(
                ["riscv64-linux-gnu-objdump", "-d", str(elf)], text=True
            )
        )
        (d / "symbols.txt").write_text(
            subprocess.check_output(
                ["riscv64-linux-gnu-nm", "-n", str(elf)], text=True
            )
        )
        workload_elf, workload_image = elf, image
        if a.fixed_workloads:
            workload_elf = a.fixed_workloads.resolve() / name / "test.elf"
            workload_image = a.fixed_workloads.resolve() / name / "test.bin"
            if (
                h.sha(workload_image) != h.sha(image)
                or kind == "se"
                and name != "se_smt"
                and h.sha(workload_elf) != h.sha(elf)
            ):
                raise RuntimeError(
                    "Fixed workload payload differs from freshly compiled test"
                )
        flags = (
            "LSQUnit"
            if kind == "se" or name.startswith("vector") or name == "negative"
            else "LSQUnit,StorePipeline,StoreBuffer,LoadPipeline,Commit,IQ"
        )
        if name == "vector_pressure":
            flags = "LSQUnit,StoreBuffer"
        launch_binary_sha = h.sha(binary)
        if launch_binary_sha != manifest["binary_sha256"]:
            raise RuntimeError(
                "Binary changed before case launch; refuse mixed baseline/candidate evidence"
            )
        cmd = [
            str(binary),
            f'--outdir={d/"m5out"}',
            f"--debug-flags={flags}",
            "--debug-file=trace.debug",
        ]
        if kind == "se":
            cmd += [
                str(repo / "configs/example/se.py"),
                f'--cmd={str(workload_elf) + chr(59) + str(workload_elf) if name == "se_smt" else workload_elf}',
                "-n",
                "1",
                "--caches",
                "--mem-type=SimpleMemory",
                "-I",
                "2000000",
            ]
            if name == "se_smt":
                cmd += [
                    "--smt",
                    "--cpu-type=DerivO3CPU",
                    "--param",
                    "system.cpu[0].StoreQueueMultiple=1",
                ]
            if name == "se4":
                for param in [
                    "RARQEntries=4",
                    "RAWQEntries=4",
                    "SbufferEntries=4",
                    "SbufferEvictThreshold=2",
                    "EnableLdMissReplay=True",
                    "BankConflictCheck=True",
                    "dcache.mshrs=2",
                ]:
                    cmd += ["--param", f"system.cpu[0].{param}"]
        else:
            cmd += [
                str(repo / "configs/example/kmhv3.py"),
                "--raw-cpt",
                f"--generic-rv-cpt={workload_image}",
                "--enable-difftest",
                f"--difftest-ref-so={ref}",
                "--mem-type=SimpleMemory",
                "-I",
                "2000" if name == "negative" else "50000",
            ]
            if name == "fs10_deep":
                cmd += ["--param", "system.cpu[0].LdPipeStages=8"]
            if name == "vector_pressure":
                for param in ["SbufferEntries=1", "SbufferEvictThreshold=1"]:
                    cmd += ["--param", f"system.cpu[0].{param}"]
            if name in ("fs5", "fs6"):
                for param in [
                    "SQEntries=4",
                    "SbufferEntries=4",
                    "SbufferEvictThreshold=2",
                ]:
                    cmd += ["--param", f"system.cpu[0].{param}"]
        launch_time_utc = datetime.now(timezone.utc).isoformat()
        rec = h.run_record(cmd, repo, d / "run.log", timeout=240)
        finish_time_utc = datetime.now(timezone.utc).isoformat()
        finish_binary_sha = h.sha(binary)
        log = (d / "run.log").read_text(errors="replace")
        exits = re.findall(r"Exiting @ tick (\d+) because ([^\n]+)", log)
        success = check_exit(rec["returncode"], exits, kind, log)
        if name == "se_smt":
            success = bool(
                rec["returncode"] == 0
                and exits
                and "last active thread context" in exits[-1][1]
                and log.count("SMT_PASS") == 2
            )
        result = {
            "case": name,
            "kind": kind,
            "returncode": rec["returncode"],
            "exit": exits[-1] if exits else None,
            "success": success,
            "expected_success": name != "negative",
            "check_passed": success
            if name != "negative"
            else check_exit(rec["returncode"], exits, kind, log, False),
            "difftest_required": kind != "se",
            "difftest_enabled": "Difftest is enabled with ref so" in log,
            "image_sha256": h.sha(
                workload_elf if kind == "se" else workload_image
            ),
            "workload_elf": str(workload_elf),
            "workload_image": str(workload_image),
            "launch_binary_sha256": launch_binary_sha,
            "finish_binary_sha256": finish_binary_sha,
            "binary_unchanged": launch_binary_sha == finish_binary_sha,
            "launch_time_utc": launch_time_utc,
            "finish_time_utc": finish_time_utc,
            "negative_rejected_as_positive": name != "negative" or not success,
        }
        result["check_passed"] &= (
            not result["difftest_required"] or result["difftest_enabled"]
        )
        result["check_passed"] &= result["binary_unchanged"]
        h.write_json(d / "result.json", result)
        print(json.dumps(result), flush=True)
        return result

    def guarded_run(name):
        try:
            return run(name)
        except Exception as e:
            import traceback

            d = out / name
            d.mkdir(exist_ok=True)
            failure = {
                "case": name,
                "check_passed": False,
                "stage": "harness_exception",
                "exception": type(e).__name__,
                "message": str(e),
                "traceback": traceback.format_exc(),
            }
            h.write_json(d / "failure.json", failure)
            print(
                json.dumps(
                    {"case": name, "check_passed": False, "exception": str(e)}
                ),
                flush=True,
            )
            return failure

    with ThreadPoolExecutor(max_workers=a.jobs) as pool:
        results = list(pool.map(guarded_run, a.cases))
    h.write_json(out / "summary.json", results)
    return 0 if all(v["check_passed"] for v in results) else 1


if __name__ == "__main__":
    sys.exit(main())
