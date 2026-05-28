#!/usr/bin/env parallux

from parallux import goal

from checkpoint_lists import VECTOR_TESTS
from parallux_common import config_path, gem5_binary, q, schedule_workloads
from parallux_common import setup_runners


LOG_ROOT = goal.args.get("log_root", "./log_root_v")
MAX_PROCESS = int(goal.args.get("max_process", "48"))
RUNNERS = goal.args.get("runners", "local")

GEM5_EXE = gem5_binary()
FS_PATH = config_path("kmhv2.py")
REF_SO_PATH = "/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-notama-so"


setup_runners(LOG_ROOT, MAX_PROCESS, RUNNERS)
goal.setEnv("GCBV_REF_SO", REF_SO_PATH)


def gem5_command(binary: str) -> str:
    return " ".join(
        [
            q(GEM5_EXE),
            '--outdir="$PARALLUX_WORK_DIR"',
            q(FS_PATH),
            "--enable-riscv-vector",
            "--restore-rvv-cpt",
            "--raw-cpt",
            f"--generic-rv-cpt={q(binary)}",
        ]
    )


print("will start running GEM5")
schedule_workloads(
    VECTOR_TESTS,
    command=gem5_command,
    levels=1,
    work_prefix="vector-test",
)
print("run finish")
