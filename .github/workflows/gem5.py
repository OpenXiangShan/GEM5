#!/usr/bin/env parallux

from parallux import goal

from checkpoint_lists import GEM5_CHECKPOINTS
from parallux_common import config_path, gem5_binary, q, schedule_workloads
from parallux_common import setup_runners


LOG_ROOT = goal.args.get("log_root", "./log_root")
MAX_PROCESS = int(goal.args.get("max_process", "48"))
RUNNERS = goal.args.get("runners", "local")

GEM5_EXE = gem5_binary()
FS_PATH = config_path("kmhv3.py")
GCPT_PATH = "/nfs/home/share/gem5_shared_tools/normal-gcb-restorer.bin"
REF_SO_PATH = "/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-notama-so"


setup_runners(LOG_ROOT, MAX_PROCESS, RUNNERS)
goal.setEnv("NEMU_HOME", REF_SO_PATH)


def gem5_command(checkpoint: str) -> str:
    return " ".join(
        [
            q(GEM5_EXE),
            '--outdir="$PARALLUX_WORK_DIR"',
            q(FS_PATH),
            "--enable-difftest",
            f"--difftest-ref-so={q(REF_SO_PATH)}",
            f"--generic-rv-cpt={q(checkpoint)}",
            f"--gcpt-restorer={q(GCPT_PATH)}",
            "--warmup-insts=800",
            "--warmup-insts-no-switch=50000010",
            "--maxinsts=100000010",
        ]
    )


print("will start running GEM5")
schedule_workloads(
    GEM5_CHECKPOINTS,
    command=gem5_command,
    levels=4,
    work_prefix="rungem5",
)
print("run finish")
