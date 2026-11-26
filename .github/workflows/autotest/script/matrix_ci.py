#!/usr/bin/env python3
"""Check Matrix operator results and record cycles for each shape."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import re
import resource
import subprocess
import sys
import time


REPO = Path(__file__).resolve().parents[4]
IMAGE_DIR = Path("/nfs/share/gem5_ci/matrix")
REFERENCE = Path(
    "/nfs/share/gem5_ci/ref/matrix/af9d4ce51fc2/riscv64-nemu-interpreter-so"
)
REQUIRED_CAPABILITIES = 3  # xsai-blas RVV and AME
# Run larger cases first when fewer workers are available.
CASES = (
    "gemm_i8_f32-m512-k256-n512",
    "flash_attention-q256-kv512-d128",
    "gemm_i8_f32-m256-k256-n256",
    "flash_attention-q32-kv256-d64",
    "gemm_i8_f32-m128-k64-n128",
    "flash_attention-q8-kv130-d70",
)


def save(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + "\n")


def select_images(directory, cases=None):
    selected = set(cases or CASES)
    operators = {name.partition("-")[0] for name in CASES}
    unknown = selected.difference(CASES, operators)
    if unknown:
        raise ValueError("Unknown CI cases: " + ", ".join(sorted(unknown)))
    images = [
        {"id": name, "image": directory / f"{name}-ci.bin"}
        for name in CASES
        if name in selected or name.partition("-")[0] in selected
    ]
    missing = [
        str(item["image"]) for item in images if not item["image"].is_file()
    ]
    if missing:
        raise ValueError("Missing CI images: " + ", ".join(missing))
    return images


def evaluate(log, case, returncode, timed_out):
    failures = []
    if timed_out:
        failures.append("wall-clock timeout")
    if returncode != 0:
        failures.append(f"simulator exit {returncode}")
    numerical = re.search(r"\[matrix-ci\] result=(PASS|FAIL) .*", log)
    if not numerical or numerical[1] != "PASS":
        failures.append("numerical correctness verdict missing or failed")
    if not re.search(r"--- 1 passed, 0 failed, 0 skipped ---", log):
        failures.append("single-case completion summary missing")
    exit_reason = r"Exiting @ tick \d+ because m5_exit instruction encountered"
    guest_completed = bool(re.search(exit_reason, log))
    if not guest_completed:
        failures.append("normal guest completion missing")
    if "Simulated exit code not 0" in log:
        failures.append("guest returned nonzero")
    diff_initialized = "NEMU register state: 1544 bytes" in log
    if not diff_initialized:
        failures.append("compatible NEMU difftest initialization missing")
    diff_failed = bool(
        re.search(r"Difftest failed|Matrix difftest ended with", log)
    )
    if diff_failed or re.search(r"panic:|fatal:|BAD TRAP", log):
        failures.append("simulator/difftest failure reported")
    dispatch = re.findall(
        r"\[matrix-ci-dispatch\] reference=0 "
        r"capabilities=(\d+) .*?status=(\d+)",
        log,
    )
    if not dispatch or any(
        int(caps) & REQUIRED_CAPABILITIES != REQUIRED_CAPABILITIES
        or int(status) != 0
        for caps, status in dispatch
    ):
        failures.append("required implementation was not selected")
    samples = re.findall(
        r"^\[matrix-ci-perf\] case=(\S+) cycles=(\d+) instret=(\d+)\r?$",
        log,
        re.MULTILINE,
    )
    performance = None
    if len(samples) == 1 and log.count("[matrix-ci-perf]") == 1:
        name, cycles, instret = samples[0]
        if name == case and int(cycles) > 0 and int(instret) > 0:
            performance = {"cycles": int(cycles), "instret": int(instret)}
    if performance is None:
        failures.append(f"missing or invalid performance sample for {case}")
    summaries = []
    for line in log.splitlines():
        if "MatrixDiff committed=" in line:
            summaries.append(
                {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", line)}
            )
    pending = ("pendingChecks", "pendingOwners", "pendingMemory")
    required_counts = {
        "committed",
        "submitted",
        "completed",
        "checked",
        "memorySent",
        "memoryCompleted",
        "mma",
        *pending,
    }
    checks_complete = bool(summaries) and all(
        required_counts <= s.keys()
        and s["submitted"] == s["completed"] == s["checked"]
        and s["memorySent"] == s["memoryCompleted"]
        and all(s[k] == 0 for k in pending)
        for s in summaries
    )
    if not summaries:
        failures.append("Matrix completion/checking summary missing")
    elif not checks_complete:
        failures.append("Matrix work or memory checks incomplete")
    if not any(0 < s.get("mma", 0) <= s.get("checked", 0) for s in summaries):
        failures.append("no Matrix MMA was checked")
    if not any(s.get("memorySent", 0) > 0 for s in summaries):
        failures.append("no Matrix memory request was sent")
    divergence = re.search(r".*NEMU PC:.*GEM5 PC:.*inst:.*", log)
    difftest = (
        "failed"
        if diff_failed
        else "passed"
        if diff_initialized and guest_completed and checks_complete
        else "incomplete"
    )
    return {
        "passed": not failures,
        "failures": failures,
        "application_correctness": numerical[1] == "PASS"
        if numerical
        else None,
        "instruction_difftest": difftest,
        "guest_completed": guest_completed,
        "first_divergence": divergence[0] if divergence else None,
        "numerical_verdict": numerical[0] if numerical else None,
        "matrix_difftest": summaries,
        "performance": performance,
    }


def run(args):
    # Abort diagnostics belong in logs; avoid large guest core dumps.
    _, core_limit = resource.getrlimit(resource.RLIMIT_CORE)
    resource.setrlimit(resource.RLIMIT_CORE, (0, core_limit))
    images = select_images(args.image_dir.resolve(), args.case)
    binary = args.gem5.resolve()
    reference = args.reference.resolve()
    for path in (binary, reference):
        if not path.is_file():
            raise FileNotFoundError(path)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)

    def run_one(item):
        case_dir = output / "runs" / item["id"]
        case_dir.mkdir(parents=True, exist_ok=True)
        invocation = [
            str(binary),
            "--listener-mode=off",
            "-d",
            str(case_dir / "m5out"),
            str(REPO / "configs/example/kmhv2.py"),
            "--matrix",
            "--classic-l2",
            "--enable-difftest",
            "--raw-cpt",
            "--maxinsts=0",
            "--warmup-insts-no-switch=0",
            f"--generic-rv-cpt={item['image']}",
            f"--difftest-ref-so={reference}",
        ]
        save(case_dir / "command.json", invocation)
        started = time.monotonic()
        timed_out = False
        with (case_dir / "sim.log").open("w") as stream:
            process = subprocess.Popen(
                invocation, cwd=REPO, stdout=stream, stderr=subprocess.STDOUT
            )
            try:
                returncode = process.wait(timeout=args.timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                returncode = process.returncode
        log = (case_dir / "sim.log").read_text(errors="replace")
        result = dict(
            evaluate(log, item["id"], returncode, timed_out),
            id=item["id"],
            image=str(item["image"]),
            returncode=returncode,
            wall_seconds=time.monotonic() - started,
            log=str(case_dir / "sim.log"),
        )
        save(case_dir / "result.json", result)
        if result["passed"]:
            perf = result["performance"]
            mma = sum(s["mma"] for s in result["matrix_difftest"])
            print(
                f"{item['id']}: PASS cycles={perf['cycles']} "
                f"instret={perf['instret']} mma={mma}",
                flush=True,
            )
        else:
            print(f"{item['id']}: FAIL {result['failures']}", flush=True)
        return result

    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        results = list(executor.map(run_one, images))
    save(output / "validation-results.json", results)
    return 0 if all(r["passed"] for r in results) else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="action", required=True)
    runner = subparsers.add_parser("run")
    runner.add_argument("--output", type=Path, required=True)
    runner.add_argument(
        "--gem5", type=Path, default=REPO / "build/RISCV_MATRIX/gem5.opt"
    )
    runner.add_argument("--reference", type=Path, default=REFERENCE)
    runner.add_argument("--image-dir", type=Path, default=IMAGE_DIR)
    runner.add_argument("--jobs", type=int, default=2)
    runner.add_argument("--timeout", type=int, default=1800)
    runner.add_argument(
        "--case",
        action="append",
        help="operator or full shape ID (repeatable)",
    )
    args = parser.parse_args()
    if args.jobs < 1 or args.timeout < 1:
        parser.error("--jobs and --timeout must be positive")
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
