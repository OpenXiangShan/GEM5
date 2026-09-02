"""Shared benchmark catalog owned by the gem5 performance CI workflow.

The performance workflow consumes this module through its CLI, while the
solver imports the same resolver. Consumer-specific capability checks stay in
their respective workflows or runtimes.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
from pathlib import Path


_SPEC06_CI_ROOT = "/nfs/home/share/gem5_ci/spec06_cpts"
_GCC12_SPEC06_ROOT = (
    "/nfs/home/share/jiaxiaoyu/simpoint_checkpoint_zstd_format/"
    "spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc"
)
_GCC12_SPEC06_SMT_ROOT = (
    "/nfs/home/share/xuyan/"
    "spec06_gcc12.2.0_rv64gcb_base_intFppOff_for_qemu_dual_core_"
    "disable_timer_QEMU_archgroup_2024-11-11-15-46/checkpoint-0-0-0"
)
_GCC16_SPEC06_PROFILE = (
    "/nfs/home/share/checkpoints_profiles/"
    "spec06_gcc16_rva23_novec_260820"
)
_GCC15_SPEC06_PROFILE = (
    "/nfs/home/share/checkpoints_profiles/"
    "spec06_gcc15_rv64gcb_base_260604"
)
_H_SPEC06_ROOT = f"{_SPEC06_CI_ROOT}/h_spec06"
_SPEC17_ROOT = "/nfs/home/yanyue/spec17_cpts/checkpoint-0-0-0"
_RVV_SPEC06_ROOT = (
    "/nfs/home/xutongqiao/GEM5-CI/"
    "spec06_gcc15_rv64gcbv_O3_lto_base_nemu_single_core_"
    "NEMU_archgroup_2024-10-12-16-05/checkpoint06_rv64gcbv"
)
_SPEC26_CI_ROOT = "/nfs/home/share/gem5_ci/spec26_cpts/rv64gcb_260718"
_GCC15_SPEC26_ROOT = (
    "/nfs/home/share/checkpoints_profiles/"
    "spec26_rate_gcc15_rv64gcb_260718/checkpoint"
)


@dataclass(frozen=True)
class BenchmarkConfig:
    benchmark_type: str
    checkpoint_list: str
    checkpoint_root: str
    cluster_config: str
    comment: str
    score_script: str = "gem5-score-ci.sh"
    artifact_name_override: str = ""

    @property
    def artifact_name(self) -> str:
        if self.artifact_name_override:
            return self.artifact_name_override
        return f"performance-score-{self.benchmark_type}"

    def as_dict(self) -> dict[str, str]:
        return {
            "benchmark_type": self.benchmark_type,
            "checkpoint_list": self.checkpoint_list,
            "checkpoint_root": self.checkpoint_root,
            "cluster_config": self.cluster_config,
            "score_script": self.score_script,
            "artifact_name": self.artifact_name,
            "comment": self.comment,
        }

    def github_outputs(self) -> dict[str, str]:
        return {
            "checkpoint_list": self.checkpoint_list,
            "checkpoint_root_node": self.checkpoint_root,
            "score_script": self.score_script,
            "cluster_config": self.cluster_config,
            "artifact_name": self.artifact_name,
            "comment": self.comment,
        }


def _index_benchmarks(
    configs: tuple[BenchmarkConfig, ...]
) -> dict[str, BenchmarkConfig]:
    result = {}
    for config in configs:
        if config.benchmark_type in result:
            raise ValueError(
                f"duplicate benchmark_type {config.benchmark_type!r}"
            )
        result[config.benchmark_type] = config
    return result


_BENCHMARKS = _index_benchmarks(
    (
        BenchmarkConfig(
            benchmark_type="gcc12-spec06-0.3c",
            checkpoint_list=f"{_SPEC06_CI_ROOT}/spec06_0.3c_int.lst",
            checkpoint_root=_GCC12_SPEC06_ROOT,
            cluster_config=f"{_SPEC06_CI_ROOT}/cluster-0-0.json",
            comment="run 30% coverage spec06 checkpoints, 148 checkpoints",
        ),
        BenchmarkConfig(
            benchmark_type="gcc12-spec06-smt-0.3c",
            checkpoint_list=f"{_SPEC06_CI_ROOT}/spec06_0.3c.lst",
            checkpoint_root=_GCC12_SPEC06_SMT_ROOT,
            cluster_config=f"{_SPEC06_CI_ROOT}/spec06_0.3c.json",
            comment=(
                "run 30% coverage dual-context SMT spec06 checkpoints, "
                "148 checkpoints"
            ),
        ),
        BenchmarkConfig(
            benchmark_type="gcc12-spec06-smt-1.0c",
            checkpoint_list=f"{_SPEC06_CI_ROOT}/checkpoint-0-0-0.lst",
            checkpoint_root=_GCC12_SPEC06_SMT_ROOT,
            cluster_config=f"{_SPEC06_CI_ROOT}/cluster-0-0.json",
            comment="run 100% coverage dual-context SMT spec06 checkpoints",
        ),
        BenchmarkConfig(
            benchmark_type="gcc12-spec06-smt-int-1.0c",
            checkpoint_list=f"{_SPEC06_CI_ROOT}/spec_1c_int.lst",
            checkpoint_root=_GCC12_SPEC06_SMT_ROOT,
            cluster_config=f"{_SPEC06_CI_ROOT}/spec06_1c_int.json",
            comment=(
                "run 100% coverage dual-context SMT SPEC06 int checkpoints"
            ),
        ),
        BenchmarkConfig(
            benchmark_type="gcc12-spec06-0.8c",
            checkpoint_list=f"{_SPEC06_CI_ROOT}/spec_0.8c_int.lst",
            checkpoint_root=_GCC12_SPEC06_ROOT,
            cluster_config=f"{_SPEC06_CI_ROOT}/cluster-0-0.json",
            comment="run 80% coverage spec06 checkpoints, 500+ checkpoints",
        ),
        BenchmarkConfig(
            benchmark_type="gcc12-spec06-1.0c",
            checkpoint_list=f"{_SPEC06_CI_ROOT}/checkpoint-0-0-0.lst",
            checkpoint_root=_GCC12_SPEC06_ROOT,
            cluster_config=f"{_SPEC06_CI_ROOT}/cluster-0-0.json",
            comment="run 100% coverage spec06 checkpoints",
        ),
        BenchmarkConfig(
            benchmark_type="spec06-rva23-novec-gcc16-0.3c",
            checkpoint_list=(
                f"{_SPEC06_CI_ROOT}/gcc16_rva23_novec/spec06_0.3c.lst"
            ),
            checkpoint_root=f"{_GCC16_SPEC06_PROFILE}/checkpoint",
            cluster_config=(
                f"{_GCC16_SPEC06_PROFILE}/json/checkpoints_cov0.3.json"
            ),
            comment=(
                "run 30% coverage gcc16 rva23-novec SPEC06 checkpoints"
            ),
        ),
        BenchmarkConfig(
            benchmark_type="spec06-rva23-novec-gcc16-1.0c",
            checkpoint_list=(
                f"{_GCC16_SPEC06_PROFILE}/checkpoint/checkpoint.lst"
            ),
            checkpoint_root=f"{_GCC16_SPEC06_PROFILE}/checkpoint",
            cluster_config=(
                f"{_GCC16_SPEC06_PROFILE}/json/checkpoints_all.json"
            ),
            comment=(
                "run 100% coverage gcc16 rva23-novec SPEC06 checkpoints"
            ),
        ),
        BenchmarkConfig(
            benchmark_type="gcc15-spec06-0.3c",
            checkpoint_list=(
                f"{_SPEC06_CI_ROOT}/gcc15_260604/spec06_0.3c.lst"
            ),
            checkpoint_root=f"{_GCC15_SPEC06_PROFILE}/checkpoint",
            cluster_config=(
                f"{_GCC15_SPEC06_PROFILE}/json/checkpoints_cov0.3.json"
            ),
            comment=(
                "run legacy 30% coverage gcc15 SPEC06 checkpoints from "
                "spec06_gcc15_rv64gcb_base_260604"
            ),
        ),
        BenchmarkConfig(
            benchmark_type="gcc15-spec06-1.0c",
            checkpoint_list=(
                f"{_GCC15_SPEC06_PROFILE}/checkpoint/checkpoint.lst"
            ),
            checkpoint_root=f"{_GCC15_SPEC06_PROFILE}/checkpoint",
            cluster_config=(
                f"{_GCC15_SPEC06_PROFILE}/json/checkpoints_all.json"
            ),
            comment=(
                "run legacy 100% coverage gcc15 SPEC06 checkpoints from "
                "spec06_gcc15_rv64gcb_base_260604"
            ),
        ),
        BenchmarkConfig(
            benchmark_type="h-spec06-0.5c",
            checkpoint_list=f"{_H_SPEC06_ROOT}/h_spec06_0.5c.lst",
            checkpoint_root=f"{_H_SPEC06_ROOT}/checkpoint-0-0-0",
            cluster_config=(
                f"{_H_SPEC06_ROOT}/h_spec06_0.5c_h_profile_insts.json"
            ),
            comment=(
                "run 50% coverage H SPEC06 checkpoints with FS0 NEMU, "
                "GCPT restore, DRAMsim3 and 40M maxinsts"
            ),
        ),
        BenchmarkConfig(
            benchmark_type="h-spec06-1.0c",
            checkpoint_list=f"{_H_SPEC06_ROOT}/h_spec06_1.0c.lst",
            checkpoint_root=f"{_H_SPEC06_ROOT}/checkpoint-0-0-0",
            cluster_config=(
                f"{_H_SPEC06_ROOT}/h_spec06_1.0c_h_profile_insts.json"
            ),
            comment=(
                "run 100% coverage H SPEC06 checkpoints with FS0 NEMU, "
                "GCPT restore, DRAMsim3 and 40M maxinsts"
            ),
        ),
        BenchmarkConfig(
            benchmark_type="spec17-1.0c",
            checkpoint_list=f"{_SPEC17_ROOT}/checkpoint.lst",
            checkpoint_root=f"{_SPEC17_ROOT}/",
            cluster_config=f"{_SPEC17_ROOT}/cluster-0-0.json",
            score_script="gem5-score-ci-17.sh",
            comment="run 100% coverage spec17 checkpoints",
        ),
        BenchmarkConfig(
            benchmark_type="spec06-rvv-1.0c",
            checkpoint_list=f"{_RVV_SPEC06_ROOT}/checkpoint.lst",
            checkpoint_root=f"{_RVV_SPEC06_ROOT}/",
            cluster_config=f"{_RVV_SPEC06_ROOT}/cluster-0-0.json",
            artifact_name_override=(
                "performance-score-spec06-1.0c-with-rvv-extension"
            ),
            comment="run 100% coverage spec06 rvv checkpoints",
        ),
        BenchmarkConfig(
            benchmark_type="spec06int-rvv-0.8c",
            checkpoint_list=f"{_RVV_SPEC06_ROOT}/checkpoint_0.8c_int.lst",
            checkpoint_root=f"{_RVV_SPEC06_ROOT}/",
            cluster_config=f"{_RVV_SPEC06_ROOT}/cluster_0.8c_int.json",
            artifact_name_override=(
                "performance-score-spec06int-0.8c-with-rvv-extension"
            ),
            comment="run 80% coverage spec06 int rvv checkpoints",
        ),
        BenchmarkConfig(
            benchmark_type="gcc15-spec26-0.3c",
            checkpoint_list=f"{_SPEC26_CI_ROOT}/spec26_0.3c.lst",
            checkpoint_root=_GCC15_SPEC26_ROOT,
            cluster_config=f"{_SPEC26_CI_ROOT}/checkpoints_cov0.3.json",
            score_script="gem5-score-ci-26.sh",
            comment=(
                "run 30% coverage SPEC CPU2026 checkpoints plus "
                "722.palm_r/201821 regression"
            ),
        ),
        BenchmarkConfig(
            benchmark_type="gcc15-spec26-1.0c",
            checkpoint_list=f"{_SPEC26_CI_ROOT}/spec26_1.0c.lst",
            checkpoint_root=_GCC15_SPEC26_ROOT,
            cluster_config=f"{_SPEC26_CI_ROOT}/checkpoints_all.json",
            score_script="gem5-score-ci-26.sh",
            comment="run 100% coverage SPEC CPU2026 checkpoints",
        ),
    )
)


def benchmark_types() -> tuple[str, ...]:
    return tuple(_BENCHMARKS)


def resolve_benchmark(benchmark_type: str) -> BenchmarkConfig:
    try:
        return _BENCHMARKS[benchmark_type]
    except KeyError:
        supported = ", ".join(benchmark_types())
        raise KeyError(
            f"unsupported benchmark_type {benchmark_type!r}; "
            f"supported values: {supported}"
        ) from None


def write_github_outputs(
    config: BenchmarkConfig, output_path: str | Path
) -> None:
    with Path(output_path).open("a", encoding="utf-8") as output:
        for name, value in config.github_outputs().items():
            if "\n" in value or "\r" in value:
                raise ValueError(
                    f"GitHub output {name!r} must be a single line"
                )
            print(f"{name}={value}", file=output)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Resolve the benchmark configuration used by gem5 perf CI."
    )
    parser.add_argument("benchmark_type")
    parser.add_argument(
        "--github-output",
        help="Append step outputs to this GitHub Actions output file.",
    )
    args = parser.parse_args()

    try:
        config = resolve_benchmark(args.benchmark_type)
    except KeyError as error:
        parser.error(error.args[0])

    if args.github_output:
        write_github_outputs(config, args.github_output)
    else:
        print(json.dumps(config.as_dict(), indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
