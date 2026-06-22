#!/usr/bin/env parallux

import os

from parallux import goal

from parallux_common import REPO_ROOT, config_path, gem5_binary, q, repo_path
from parallux_common import setup_runners


DEFAULT_WORKSPACE = repo_path("util/xs_scripts")

CASE = goal.args.get("case", "")
LOG_ROOT = goal.args.get("log_root", DEFAULT_WORKSPACE)
MAX_JOBS = int(goal.args.get("max_jobs", "1"))
RUNNERS = goal.args.get("runners", "local")
GEM5_BUILD_TYPE = goal.args.get(
    "gem5_build_type", os.environ.get("GEM5_BUILD_TYPE", "opt")
)


def arg_or_env(key: str, default: str) -> str:
    return goal.args.get(key, os.environ.get(key, default))


def selected_gem5(build_dir: str = "RISCV") -> str:
    return gem5_binary(build_dir=build_dir, build_type=GEM5_BUILD_TYPE)


SMOKE_CASES = {
    "gcbv": {
        "work_relpath": "test_v",
        "checkpoint": "/nfs/home/share/gem5_ci/checkpoints/gcbv_test.zstd",
        "env": {
            "GCBV_REF_SO": "/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-notama-so",
            "GCBV_RESTORER": "/nfs/home/share/gem5_ci/tools/gcbv-restorer.bin",
        },
        "command": lambda checkpoint: [
            selected_gem5(),
            config_path("kmhv2.py"),
            f"--generic-rv-cpt={checkpoint}",
            "--restore-rvv-cpt",
        ],
    },
    "gcb_multi_core": {
        "work_relpath": "test_multi_core",
        "checkpoint": "/nfs/home/share/gem5_ci/checkpoints/multi_core_test.gz",
        "env": {
            "GCBV_MULTI_CORE_REF_SO": "/nfs/home/share/gem5_ci/ref/multi/riscv64-nemu-interpreter-so",
            "GCB_MULTI_CORE_RESTORER": "/nfs/home/share/gem5_ci/tools/gcb-2core-restorer.bin",
        },
        "command": lambda checkpoint: [
            gem5_binary(build_dir="RISCV_CHI"),
            config_path("kmhv2.py"),
            "--ruby",
            "--num-cpus=2",
            f"--generic-rv-cpt={checkpoint}",
            "--mem-type=DDR4_2400_8x8",
        ],
    },
    "l2tlb": {
        "work_relpath": "test_l2tlb",
        "checkpoint": "/nfs/home/share/gem5_ci/checkpoints/l2tlb_test.zstd",
        "env": {
            "GCBV_REF_SO": "/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-interpreter-so",
        },
        "command": lambda checkpoint: [
            selected_gem5(),
            config_path("kmhv2.py"),
            f"--generic-rv-cpt={checkpoint}",
        ],
    },
    "gcbh": {
        "work_relpath": "test_h",
        "checkpoint": "/nfs/home/share/gem5_ci/checkpoints/gcbh_test.zstd",
        "env": {
            "GCBH_REF_SO": "/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-notama-tvalref-so",
            "GCBH_RESTORER": "/nfs/home/share/gem5_ci/tools/gcpt.bin",
        },
        "command": lambda checkpoint: [
            selected_gem5(),
            config_path("kmhv2.py"),
            f"--generic-rv-cpt={checkpoint}",
            "--restore-rvh-cpt",
        ],
    },
}

if CASE not in SMOKE_CASES:
    choices = ", ".join(sorted(SMOKE_CASES))
    raise RuntimeError(f"case must be one of: {choices}")

case = SMOKE_CASES[CASE]
setup_runners(LOG_ROOT, MAX_JOBS, RUNNERS)
goal.setEnv("GEM5_HOME", str(REPO_ROOT))
goal.setEnv("gem5_home", str(REPO_ROOT))
goal.setEnv("GEM5_BUILD_TYPE", GEM5_BUILD_TYPE)
for key, default in case["env"].items():
    goal.setEnv(key, arg_or_env(key, default))

checkpoint = goal.args.get("checkpoint", case["checkpoint"])
command = " ".join(q(item) for item in case["command"](checkpoint))
goal.schd(command, name=CASE, work_relpath=case["work_relpath"])
goal.issue().sync()
