from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import subprocess


@dataclass(frozen=True)
class BenchmarkConfig:
    benchmark_type: str
    checkpoint_list: str
    checkpoint_root: str
    cluster_config: str
    score_script: str


_BENCHMARKS = {
    "gcc12-spec06-0.3c": BenchmarkConfig(
        benchmark_type="gcc12-spec06-0.3c",
        checkpoint_list="/nfs/home/share/gem5_ci/spec06_cpts/spec06_0.3c_int.lst",
        checkpoint_root=(
            "/nfs/home/share/jiaxiaoyu/simpoint_checkpoint_zstd_format/"
            "spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc"
        ),
        cluster_config="/nfs/home/share/gem5_ci/spec06_cpts/cluster-0-0.json",
        score_script="gem5-score-ci.sh",
    ),
    "gcc12-spec06-smt-0.3c": BenchmarkConfig(
        benchmark_type="gcc12-spec06-smt-0.3c",
        checkpoint_list="/nfs/home/share/gem5_ci/spec06_cpts/spec06_0.3c.lst",
        checkpoint_root=(
            "/nfs/home/share/xuyan/"
            "spec06_gcc12.2.0_rv64gcb_base_intFppOff_for_qemu_dual_core_"
            "disable_timer_QEMU_archgroup_2024-11-11-15-46/checkpoint-0-0-0"
        ),
        cluster_config="/nfs/home/share/gem5_ci/spec06_cpts/spec06_0.3c.json",
        score_script="gem5-score-ci.sh",
    ),
    "gcc12-spec06-smt-1.0c": BenchmarkConfig(
        benchmark_type="gcc12-spec06-smt-1.0c",
        checkpoint_list="/nfs/home/share/gem5_ci/spec06_cpts/checkpoint-0-0-0.lst",
        checkpoint_root=(
            "/nfs/home/share/xuyan/"
            "spec06_gcc12.2.0_rv64gcb_base_intFppOff_for_qemu_dual_core_"
            "disable_timer_QEMU_archgroup_2024-11-11-15-46/checkpoint-0-0-0"
        ),
        cluster_config="/nfs/home/share/gem5_ci/spec06_cpts/cluster-0-0.json",
        score_script="gem5-score-ci.sh",
    ),
    "gcc12-spec06-0.8c": BenchmarkConfig(
        benchmark_type="gcc12-spec06-0.8c",
        checkpoint_list="/nfs/home/share/gem5_ci/spec06_cpts/spec_0.8c_int.lst",
        checkpoint_root=(
            "/nfs/home/share/jiaxiaoyu/simpoint_checkpoint_zstd_format/"
            "spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc"
        ),
        cluster_config="/nfs/home/share/gem5_ci/spec06_cpts/cluster-0-0.json",
        score_script="gem5-score-ci.sh",
    ),
    "gcc12-spec06-1.0c": BenchmarkConfig(
        benchmark_type="gcc12-spec06-1.0c",
        checkpoint_list="/nfs/home/share/gem5_ci/spec06_cpts/checkpoint-0-0-0.lst",
        checkpoint_root=(
            "/nfs/home/share/jiaxiaoyu/simpoint_checkpoint_zstd_format/"
            "spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc"
        ),
        cluster_config="/nfs/home/share/gem5_ci/spec06_cpts/cluster-0-0.json",
        score_script="gem5-score-ci.sh",
    ),
    "gcc15-spec06-0.3c": BenchmarkConfig(
        benchmark_type="gcc15-spec06-0.3c",
        checkpoint_list=(
            "/nfs/home/share/gem5_ci/spec06_cpts/"
            "gcc15_260604/spec06_0.3c.lst"
        ),
        checkpoint_root=(
            "/nfs/home/share/checkpoints_profiles/"
            "spec06_gcc15_rv64gcb_base_260604/checkpoint"
        ),
        cluster_config=(
            "/nfs/home/share/checkpoints_profiles/"
            "spec06_gcc15_rv64gcb_base_260604/json/checkpoints_cov0.3.json"
        ),
        score_script="gem5-score-ci.sh",
    ),
    "gcc15-spec06-0.8c": BenchmarkConfig(
        benchmark_type="gcc15-spec06-0.8c",
        checkpoint_list="/nfs/home/share/gem5_ci/spec06_cpts/gcc15/spec06_0.8c.lst",
        checkpoint_root="/nfs/home/share/checkpoints_profiles/spec06_gcc15_rv64gcb_base_260122/checkpoint-0-0-0",
        cluster_config="/nfs/home/share/gem5_ci/spec06_cpts/gcc15/gcc15-spec06-0.8.json",
        score_script="gem5-score-ci.sh",
    ),
    "gcc15-spec06-1.0c": BenchmarkConfig(
        benchmark_type="gcc15-spec06-1.0c",
        checkpoint_list=(
            "/nfs/home/share/checkpoints_profiles/"
            "spec06_gcc15_rv64gcb_base_260604/checkpoint/checkpoint.lst"
        ),
        checkpoint_root=(
            "/nfs/home/share/checkpoints_profiles/"
            "spec06_gcc15_rv64gcb_base_260604/checkpoint"
        ),
        cluster_config=(
            "/nfs/home/share/checkpoints_profiles/"
            "spec06_gcc15_rv64gcb_base_260604/json/checkpoints_all.json"
        ),
        score_script="gem5-score-ci.sh",
    ),
    "spec06-rva23-novec-gcc16-0.3c": BenchmarkConfig(
        benchmark_type="spec06-rva23-novec-gcc16-0.3c",
        checkpoint_list=(
            "/nfs/home/share/gem5_ci/spec06_cpts/"
            "gcc16_rva23_novec/spec06_0.3c.lst"
        ),
        checkpoint_root=(
            "/nfs/home/share/checkpoints_profiles/"
            "spec06_gcc16_rva23_novec_260820/checkpoint"
        ),
        cluster_config=(
            "/nfs/home/share/checkpoints_profiles/"
            "spec06_gcc16_rva23_novec_260820/json/checkpoints_cov0.3.json"
        ),
        score_script="gem5-score-ci.sh",
    ),
    "spec06-rva23-novec-gcc16-1.0c": BenchmarkConfig(
        benchmark_type="spec06-rva23-novec-gcc16-1.0c",
        checkpoint_list=(
            "/nfs/home/share/checkpoints_profiles/"
            "spec06_gcc16_rva23_novec_260820/checkpoint/checkpoint.lst"
        ),
        checkpoint_root=(
            "/nfs/home/share/checkpoints_profiles/"
            "spec06_gcc16_rva23_novec_260820/checkpoint"
        ),
        cluster_config=(
            "/nfs/home/share/checkpoints_profiles/"
            "spec06_gcc16_rva23_novec_260820/json/checkpoints_all.json"
        ),
        score_script="gem5-score-ci.sh",
    ),
    "spec17-1.0c": BenchmarkConfig(
        benchmark_type="spec17-1.0c",
        checkpoint_list="/nfs/home/yanyue/spec17_cpts/checkpoint-0-0-0/checkpoint.lst",
        checkpoint_root="/nfs/home/yanyue/spec17_cpts/checkpoint-0-0-0",
        cluster_config="/nfs/home/yanyue/spec17_cpts/checkpoint-0-0-0/cluster-0-0.json",
        score_script="gem5-score-ci-17.sh",
    ),
    "spec06-rvv-1.0c": BenchmarkConfig(
        benchmark_type="spec06-rvv-1.0c",
        checkpoint_list=(
            "/nfs/home/xutongqiao/GEM5-CI/"
            "spec06_gcc15_rv64gcbv_O3_lto_base_nemu_single_core_"
            "NEMU_archgroup_2024-10-12-16-05/checkpoint06_rv64gcbv/"
            "checkpoint.lst"
        ),
        checkpoint_root=(
            "/nfs/home/xutongqiao/GEM5-CI/"
            "spec06_gcc15_rv64gcbv_O3_lto_base_nemu_single_core_"
            "NEMU_archgroup_2024-10-12-16-05/checkpoint06_rv64gcbv"
        ),
        cluster_config=(
            "/nfs/home/xutongqiao/GEM5-CI/"
            "spec06_gcc15_rv64gcbv_O3_lto_base_nemu_single_core_"
            "NEMU_archgroup_2024-10-12-16-05/checkpoint06_rv64gcbv/"
            "cluster-0-0.json"
        ),
        score_script="gem5-score-ci.sh",
    ),
    "spec06int-rvv-0.8c": BenchmarkConfig(
        benchmark_type="spec06int-rvv-0.8c",
        checkpoint_list="/nfs/home/share/zhenhao/runnable_tests.lst",
        checkpoint_root=(
            "/nfs/home/xutongqiao/GEM5-CI/"
            "spec06_gcc15_rv64gcbv_O3_lto_base_nemu_single_core_"
            "NEMU_archgroup_2024-10-12-16-05/checkpoint06_rv64gcbv"
        ),
        cluster_config="/nfs/home/share/zhenhao/runnable_tests.json",
        score_script="gem5-score-ci.sh",
    ),
}


def resolve_benchmark(benchmark_type: str) -> BenchmarkConfig:
    if benchmark_type not in _BENCHMARKS:
        raise KeyError(f"unsupported benchmark_type {benchmark_type!r}")
    return _BENCHMARKS[benchmark_type]


def iter_workload_entries(checkpoint_list: str, filters: str = ""):
    filter_tokens = [
        token.strip().lower() for token in filters.split(",") if token.strip()
    ]
    with open(checkpoint_list, "r", encoding="utf-8") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            if not line:
                continue
            if filter_tokens and not any(
                token in line.lower() for token in filter_tokens
            ):
                continue
            fields = line.split()
            if len(fields) < 2:
                continue
            yield fields


def locate_checkpoint(checkpoint_root: str, checkpoint_fragment: str) -> str:
    for suffix in ("gz", "zstd"):
        result = subprocess.run(
            [
                "find",
                "-L",
                checkpoint_root,
                "-wholename",
                f"*{checkpoint_fragment}*.{suffix}",
            ],
            capture_output=True,
            check=False,
            text=True,
        )
        for line in result.stdout.splitlines():
            candidate = line.strip()
            if candidate:
                return candidate
    raise FileNotFoundError(
        f"failed to locate checkpoint for fragment {checkpoint_fragment!r} under {checkpoint_root}"
    )


def select_representative_checkpoint(config: BenchmarkConfig, filters: str = "") -> str:
    for fields in iter_workload_entries(config.checkpoint_list, filters):
        return locate_checkpoint(config.checkpoint_root, fields[1])
    raise FileNotFoundError(
        f"no workload matched filters {filters!r} in {config.checkpoint_list}"
    )
