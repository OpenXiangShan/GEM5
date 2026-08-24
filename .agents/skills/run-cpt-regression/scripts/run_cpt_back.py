#!/usr/bin/env python3

from __future__ import annotations

import argparse
import concurrent.futures
import logging
import os
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List

REPO_ROOT = Path(__file__).resolve().parents[4]
GEM5_BUILD_DIR = REPO_ROOT / "build" / "RISCV"
KMHV3_CONFIG = REPO_ROOT / "configs" / "example" / "kmhv3.py"


@dataclass
class SimConfig:
    binary: str
    slice_name: str
    checkpoint: str
    outdir: Path
    args: List[str]


class GEM5Runner:
    def __init__(self, max_workers: int, debug_dir: str, kmhv3_params: List[str], skip_ref: bool):
        self.max_workers = max_workers
        debug_path = Path(debug_dir)
        if not debug_path.is_absolute():
            debug_path = REPO_ROOT / debug_path
        self.debug_dir = debug_path

        self.kmhv3_params = kmhv3_params
        self.skip_ref = skip_ref

        logging.basicConfig(level=logging.INFO, format="%(asctime)s - %(levelname)s - %(message)s")
        self.logger = logging.getLogger(__name__)

        self.slices: Dict[str, str] = {
            "coremark10": "/nfs/home/share/gem5_ci/checkpoints/coremark-riscv64-xs.bin",
        }
        self.load_frontend_tests()

    def load_frontend_tests(self) -> None:
        am_home = os.environ.get("AM_HOME")
        if not am_home:
            self.logger.warning("AM_HOME is not set; skip frontend test discovery")
            return

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
                self.logger.warning("Frontend test directory not found: %s", build_dir)
                continue
            for binary in build_dir.glob("*-riscv64-xs.bin"):
                name = binary.stem
                suffix = "-riscv64-xs"
                if name.endswith(suffix):
                    name = name[: -len(suffix)]
                if name not in self.slices:
                    discovered += 1
                self.slices[name] = str(binary)

        if discovered:
            self.logger.info("Discovered %d frontend tests via AM_HOME", discovered)

    def generate_configs(self) -> List[SimConfig]:
        configs: List[SimConfig] = []
        for slice_name, checkpoint in self.slices.items():
            if not self.skip_ref:
                configs.append(
                    SimConfig(
                        binary="gem5.opt.ref",
                        slice_name=slice_name,
                        checkpoint=checkpoint,
                        outdir=self.debug_dir / f"{slice_name}_ref",
                        args=[""],
                    )
                )
            configs.append(
                SimConfig(
                    binary="gem5.opt",
                    slice_name=slice_name,
                    checkpoint=checkpoint,
                    outdir=self.debug_dir / f"{slice_name}_opt",
                    args=[""],
                )
            )
        return configs

    def run_single(self, config: SimConfig) -> bool:
        config.outdir.mkdir(parents=True, exist_ok=True)
        stdout_file = config.outdir / "gem5.stdout"
        stderr_file = config.outdir / "gem5.stderr"

        cmd: List[str] = [
            str(GEM5_BUILD_DIR / config.binary),
            "--outdir",
            str(config.outdir),
            str(KMHV3_CONFIG),
            "--generic-rv-cpt",
            str(config.checkpoint),
            "--raw-cpt",
            *config.args,
        ]
        for param in self.kmhv3_params:
            cmd.extend(["-P", param])

        self.logger.info("Run %s with %s", config.slice_name, config.binary)
        with stdout_file.open("w", encoding="utf-8") as out, stderr_file.open("w", encoding="utf-8") as err:
            proc = subprocess.run(cmd, stdout=out, stderr=err, text=True)

        if proc.returncode == 0:
            return True

        err_text = stderr_file.read_text(encoding="utf-8", errors="ignore").strip()
        self.logger.error("Simulation failed: %s %s: %s", config.slice_name, config.binary, err_text)
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
    parser = argparse.ArgumentParser(description="Run gem5 checkpoint batch only (no analysis)")
    parser.add_argument("--max-workers", type=int, default=64)
    parser.add_argument("--debug-dir", type=str, default="debug/test1")
    parser.add_argument("--slices", type=str, nargs="+", help="Run only selected slices")
    parser.add_argument("--skip-ref", action="store_true", help="Skip gem5.opt.ref runs")
    parser.add_argument("--param", action="append", default=[], help="Repeatable kmhv3 -P argument")
    return parser


def main() -> int:
    args = build_parser().parse_args()

    runner = GEM5Runner(
        max_workers=args.max_workers,
        debug_dir=args.debug_dir,
        kmhv3_params=args.param,
        skip_ref=args.skip_ref,
    )

    if args.slices:
        runner.slices = {k: v for k, v in runner.slices.items() if k in args.slices}
        if not runner.slices:
            runner.logger.error("No valid slices specified")
            return 1

    return runner.run_all()


if __name__ == "__main__":
    raise SystemExit(main())
