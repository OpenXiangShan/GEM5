#!/usr/bin/env python3

from __future__ import annotations

import argparse
import concurrent.futures
import logging
import os
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Sequence

REPO_ROOT = Path(__file__).resolve().parents[4]
GEM5_BUILD_DIR = REPO_ROOT / "build" / "RISCV"
KMHV3_CONFIG = REPO_ROOT / "configs" / "example" / "kmhv3.py"
DEFAULT_RTL_BIN = Path("/nfs/home/yanyue/workspace/xs-env/XiangShan/build/emu")
DEFAULT_NEMU_HOME = Path("/nfs/home/yanyue/workspace/xs-env/NEMU")
DEFAULT_NOOP_HOME = Path("/nfs/home/yanyue/workspace/xs-env/XiangShan")


@dataclass
class SimConfig:
    backend: str
    label: str
    slice_name: str
    outdir: Path
    cmd: List[str]
    stdout_name: str
    stderr_name: str


def load_available_slices(logger: logging.Logger) -> Dict[str, str]:
    slices: Dict[str, str] = {
        "coremark10": "/nfs/home/share/gem5_ci/checkpoints/coremark-riscv64-xs.bin",
    }

    am_home = os.environ.get("AM_HOME")
    if not am_home:
        logger.warning("AM_HOME is not set; skip frontend test discovery")
        return slices

    base = Path(am_home) / "tests" / "frontendtest"
    build_dirs = [
        base / "build",
        base / "br_target_test" / "build",
        base / "cond_br_test" / "build",
        base / "mgsc_test" / "build",
    ]

    discovered = 0
    for build_dir in build_dirs:
        if not build_dir.exists():
            logger.warning("Frontend test directory not found: %s", build_dir)
            continue
        for binary in build_dir.glob("*-riscv64-xs.bin"):
            name = binary.stem
            suffix = "-riscv64-xs"
            if name.endswith(suffix):
                name = name[: -len(suffix)]
            if name not in slices:
                discovered += 1
            slices[name] = str(binary)

    if discovered:
        logger.info("Discovered %d frontend tests via AM_HOME", discovered)
    return slices


class BatchRunner:
    def __init__(
        self,
        max_workers: int,
        debug_dir: str,
        backend: str,
        kmhv3_params: List[str],
        skip_ref: bool,
        rtl_bin: str,
        rtl_max_instr: int,
        rtl_warmup_instr: int,
        rtl_stat_cycles: int,
        rtl_no_diff: bool,
        rtl_extra_args: List[str],
    ):
        self.max_workers = max_workers
        debug_path = Path(debug_dir)
        if not debug_path.is_absolute():
            debug_path = REPO_ROOT / debug_path
        self.debug_dir = debug_path

        self.backend = backend
        self.kmhv3_params = kmhv3_params
        self.skip_ref = skip_ref
        self.rtl_bin = Path(rtl_bin)
        self.rtl_max_instr = rtl_max_instr
        self.rtl_warmup_instr = rtl_warmup_instr
        self.rtl_stat_cycles = rtl_stat_cycles
        self.rtl_no_diff = rtl_no_diff
        self.rtl_extra_args = rtl_extra_args

        logging.basicConfig(level=logging.INFO, format="%(asctime)s - %(levelname)s - %(message)s")
        self.logger = logging.getLogger(__name__)

        self.slices = load_available_slices(self.logger)

    def _gem5_cmd(self, binary: str, checkpoint: str) -> List[str]:
        cmd: List[str] = [
            str(GEM5_BUILD_DIR / binary),
            "--outdir",
            "",
            str(KMHV3_CONFIG),
            "--generic-rv-cpt",
            str(checkpoint),
            "--raw-cpt",
        ]
        for param in self.kmhv3_params:
            cmd.extend(["-P", param])
        return cmd

    def _rtl_cmd(self, checkpoint: str) -> List[str]:
        cmd: List[str] = [
            str(self.rtl_bin),
            "--image",
            str(checkpoint),
            "--force-dump-result",
        ]
        if self.rtl_warmup_instr > 0:
            cmd.extend(["--warmup-instr", str(self.rtl_warmup_instr)])
        if self.rtl_max_instr > 0:
            cmd.extend(["--max-instr", str(self.rtl_max_instr)])
        if self.rtl_stat_cycles > 0:
            cmd.extend(["--stat-cycles", str(self.rtl_stat_cycles)])
        if self.rtl_no_diff:
            cmd.append("--no-diff")
        cmd.extend(self.rtl_extra_args)
        return cmd

    def _rtl_env(self) -> Dict[str, str]:
        env = os.environ.copy()
        if "NEMU_HOME" not in env and DEFAULT_NEMU_HOME.exists():
            env["NEMU_HOME"] = str(DEFAULT_NEMU_HOME)
        if "NOOP_HOME" not in env and DEFAULT_NOOP_HOME.exists():
            env["NOOP_HOME"] = str(DEFAULT_NOOP_HOME)
        return env

    def generate_configs(self) -> List[SimConfig]:
        configs: List[SimConfig] = []
        for slice_name, checkpoint in self.slices.items():
            if self.backend in ("gem5", "both") and not self.skip_ref:
                configs.append(
                    SimConfig(
                        backend="gem5",
                        label="gem5.opt.ref",
                        slice_name=slice_name,
                        outdir=self.debug_dir / f"{slice_name}_ref",
                        cmd=self._gem5_cmd("gem5.opt.ref", checkpoint),
                        stdout_name="gem5.stdout",
                        stderr_name="gem5.stderr",
                    )
                )
            if self.backend in ("gem5", "both"):
                configs.append(
                    SimConfig(
                        backend="gem5",
                        label="gem5.opt",
                        slice_name=slice_name,
                        outdir=self.debug_dir / f"{slice_name}_opt",
                        cmd=self._gem5_cmd("gem5.opt", checkpoint),
                        stdout_name="gem5.stdout",
                        stderr_name="gem5.stderr",
                    )
                )
            if self.backend in ("rtl", "both"):
                configs.append(
                    SimConfig(
                        backend="rtl",
                        label="rtl.emu",
                        slice_name=slice_name,
                        outdir=self.debug_dir / f"{slice_name}_rtl",
                        cmd=self._rtl_cmd(checkpoint),
                        stdout_name="rtl.stdout",
                        stderr_name="rtl.stderr",
                    )
                )
        return configs

    def run_single(self, config: SimConfig) -> bool:
        config.outdir.mkdir(parents=True, exist_ok=True)
        stdout_file = config.outdir / config.stdout_name
        stderr_file = config.outdir / config.stderr_name
        cmd = list(config.cmd)
        if config.backend == "gem5":
            cmd[2] = str(config.outdir)

        (config.outdir / "cmd.txt").write_text(" ".join(cmd) + "\n", encoding="utf-8")

        self.logger.info("Run %s with %s", config.slice_name, config.label)
        run_env = None
        if config.backend == "rtl":
            run_env = self._rtl_env()
        with stdout_file.open("w", encoding="utf-8") as out, stderr_file.open("w", encoding="utf-8") as err:
            proc = subprocess.run(cmd, stdout=out, stderr=err, text=True, cwd=config.outdir, env=run_env)

        if proc.returncode == 0:
            return True

        err_text = stderr_file.read_text(encoding="utf-8", errors="ignore").strip()
        self.logger.error("Simulation failed: %s %s: %s", config.slice_name, config.label, err_text)
        return False

    def run_all(self) -> int:
        configs = self.generate_configs()
        success = 0
        fail = 0

        with concurrent.futures.ThreadPoolExecutor(max_workers=self.max_workers) as executor:
            future_map = {executor.submit(self.run_single, cfg): cfg for cfg in configs}
            for future in concurrent.futures.as_completed(future_map):
                cfg = future_map[future]
                try:
                    if future.result():
                        success += 1
                    else:
                        fail += 1
                except Exception as exc:
                    fail += 1
                    self.logger.error("Unhandled simulation exception on %s: %s", cfg.slice_name, exc)

        self.logger.info("Simulation done. success=%d failed=%d", success, fail)
        return 0 if fail == 0 else 1


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run gem5/RTL checkpoint batch only (no analysis)")
    parser.add_argument("--max-workers", type=int, default=64)
    parser.add_argument("--debug-dir", type=str, default="debug/test1")
    parser.add_argument("--backend", choices=["gem5", "rtl", "both"], default="gem5")
    parser.add_argument("--slices", type=str, nargs="+", help="Run only selected slices")
    parser.add_argument("--skip-ref", action="store_true", help="Skip gem5.opt.ref runs")
    parser.add_argument("--param", action="append", default=[], help="Repeatable kmhv3 -P argument")
    parser.add_argument("--rtl-bin", type=str, default=str(DEFAULT_RTL_BIN))
    parser.add_argument("--rtl-max-instr", type=int, default=0)
    parser.add_argument("--rtl-warmup-instr", type=int, default=0)
    parser.add_argument("--rtl-stat-cycles", type=int, default=0)
    parser.add_argument("--rtl-no-diff", action="store_true", help="Append --no-diff to RTL emu")
    parser.add_argument("--rtl-arg", action="append", default=[], help="Repeatable extra RTL emu argument")
    return parser


def main() -> int:
    args = build_parser().parse_args()

    runner = BatchRunner(
        max_workers=args.max_workers,
        debug_dir=args.debug_dir,
        backend=args.backend,
        kmhv3_params=args.param,
        skip_ref=args.skip_ref,
        rtl_bin=args.rtl_bin,
        rtl_max_instr=args.rtl_max_instr,
        rtl_warmup_instr=args.rtl_warmup_instr,
        rtl_stat_cycles=args.rtl_stat_cycles,
        rtl_no_diff=args.rtl_no_diff,
        rtl_extra_args=args.rtl_arg,
    )

    if args.slices:
        runner.slices = {k: v for k, v in runner.slices.items() if k in args.slices}
        if not runner.slices:
            runner.logger.error("No valid slices specified")
            return 1

    return runner.run_all()


if __name__ == "__main__":
    raise SystemExit(main())
