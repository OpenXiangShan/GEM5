#!/usr/bin/env parallux

from parallux import goal

from checkpoint_lists import GEM5_H_CHECKPOINTS
from parallux_common import config_path, gem5_binary, q, schedule_workloads
from parallux_common import setup_runners


LOG_ROOT = goal.args.get("log_root", "./log_root_h")
MAX_PROCESS = int(goal.args.get("max_process", "48"))
RUNNERS = goal.args.get("runners", "local")

GEM5_EXE = gem5_binary()
FS_PATH = config_path("kmhv2.py")
REF_SO_PATH = "/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-notama-tvalref-so"


setup_runners(LOG_ROOT, MAX_PROCESS, RUNNERS)
goal.setEnv("GCBH_REF_SO", REF_SO_PATH)
goal.setEnv("GCBV_REF_SO", REF_SO_PATH)


def gem5_command(checkpoint: str) -> str:
    return " ".join(
        [
            q(GEM5_EXE),
            '--outdir="$PARALLUX_WORK_DIR"',
            q(FS_PATH),
            "--enable-h-gcpt",
            f"--generic-rv-cpt={q(checkpoint)}",
        ]
    )


print("will start running GEM5")
schedule_workloads(
    GEM5_H_CHECKPOINTS,
    command=gem5_command,
    levels=4,
    work_prefix="rungem5",
)
print("run finish")
