"""Shared benchmark catalog owned by the gem5 performance CI workflow.

The performance workflow consumes this module through its CLI, while the
solver imports the same resolver. Consumer-specific capability checks stay in
their respective workflows or runtimes.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import math
import re
from pathlib import Path


@dataclass(frozen=True)
class BenchmarkConfig:
    benchmark_type: str
    checkpoint_list: str
    checkpoint_root: str
    cluster_config: str
    comment: str
    score_script: str = "gem5-score-ci.sh"

    def as_dict(self) -> dict[str, str]:
        return {
            "benchmark_type": self.benchmark_type,
            "checkpoint_list": self.checkpoint_list,
            "checkpoint_root": self.checkpoint_root,
            "cluster_config": self.cluster_config,
            "score_script": self.score_script,
            "comment": self.comment,
        }

    def github_outputs(self) -> dict[str, str]:
        return {
            "benchmark_type": self.benchmark_type,
            "checkpoint_list": self.checkpoint_list,
            "checkpoint_root_node": self.checkpoint_root,
            "score_script": self.score_script,
            "cluster_config": self.cluster_config,
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
            checkpoint_list="/nfs/home/share/gem5_ci/spec06_cpts/spec06_0.3c_int.lst",
            checkpoint_root="/nfs/home/share/jiaxiaoyu/simpoint_checkpoint_zstd_format/spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc",
            cluster_config="/nfs/home/share/gem5_ci/spec06_cpts/cluster-0-0.json",
            comment="run 30% coverage spec06 checkpoints, 148 checkpoints",
        ),
        BenchmarkConfig(
            benchmark_type="gcc12-spec06-smt-0.3c",
            checkpoint_list="/nfs/home/share/gem5_ci/spec06_cpts/spec06_0.3c.lst",
            checkpoint_root="/nfs/home/share/xuyan/spec06_gcc12.2.0_rv64gcb_base_intFppOff_for_qemu_dual_core_disable_timer_QEMU_archgroup_2024-11-11-15-46/checkpoint-0-0-0",
            cluster_config="/nfs/home/share/gem5_ci/spec06_cpts/spec06_0.3c.json",
            comment=(
                "run 30% coverage dual-context SMT spec06 checkpoints, "
                "148 checkpoints"
            ),
        ),
        BenchmarkConfig(
            benchmark_type="gcc12-spec06-smt-1.0c",
            checkpoint_list="/nfs/home/share/gem5_ci/spec06_cpts/checkpoint-0-0-0.lst",
            checkpoint_root="/nfs/home/share/xuyan/spec06_gcc12.2.0_rv64gcb_base_intFppOff_for_qemu_dual_core_disable_timer_QEMU_archgroup_2024-11-11-15-46/checkpoint-0-0-0",
            cluster_config="/nfs/home/share/gem5_ci/spec06_cpts/cluster-0-0.json",
            comment="run 100% coverage dual-context SMT spec06 checkpoints",
        ),
        BenchmarkConfig(
            benchmark_type="gcc12-spec06-smt-int-1.0c",
            checkpoint_list="/nfs/home/share/gem5_ci/spec06_cpts/spec_1c_int.lst",
            checkpoint_root="/nfs/home/share/xuyan/spec06_gcc12.2.0_rv64gcb_base_intFppOff_for_qemu_dual_core_disable_timer_QEMU_archgroup_2024-11-11-15-46/checkpoint-0-0-0",
            cluster_config="/nfs/home/share/gem5_ci/spec06_cpts/spec06_1c_int.json",
            comment=(
                "run 100% coverage dual-context SMT SPEC06 int checkpoints"
            ),
        ),
        BenchmarkConfig(
            benchmark_type="gcc12-spec06-0.8c",
            checkpoint_list="/nfs/home/share/gem5_ci/spec06_cpts/spec_0.8c_int.lst",
            checkpoint_root="/nfs/home/share/jiaxiaoyu/simpoint_checkpoint_zstd_format/spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc",
            cluster_config="/nfs/home/share/gem5_ci/spec06_cpts/cluster-0-0.json",
            comment="run 80% coverage spec06 checkpoints, 500+ checkpoints",
        ),
        BenchmarkConfig(
            benchmark_type="gcc12-spec06-1.0c",
            checkpoint_list="/nfs/home/share/gem5_ci/spec06_cpts/checkpoint-0-0-0.lst",
            checkpoint_root="/nfs/home/share/jiaxiaoyu/simpoint_checkpoint_zstd_format/spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc",
            cluster_config="/nfs/home/share/gem5_ci/spec06_cpts/cluster-0-0.json",
            comment="run 100% coverage spec06 checkpoints",
        ),
        BenchmarkConfig(
            benchmark_type="spec06-rva23-novec-gcc16-0.3c",
            checkpoint_list="/nfs/home/share/gem5_ci/spec06_cpts/gcc16_rva23_novec/spec06_0.3c.lst",
            checkpoint_root="/nfs/home/share/checkpoints_profiles/spec06_gcc16_rva23_novec_260820/checkpoint",
            cluster_config="/nfs/home/share/checkpoints_profiles/spec06_gcc16_rva23_novec_260820/json/checkpoints_cov0.3.json",
            comment=("run 30% coverage gcc16 rva23-novec SPEC06 checkpoints"),
        ),
        BenchmarkConfig(
            benchmark_type="spec06-rva23-novec-gcc16-1.0c",
            checkpoint_list="/nfs/home/share/checkpoints_profiles/spec06_gcc16_rva23_novec_260820/checkpoint/checkpoint.lst",
            checkpoint_root="/nfs/home/share/checkpoints_profiles/spec06_gcc16_rva23_novec_260820/checkpoint",
            cluster_config="/nfs/home/share/checkpoints_profiles/spec06_gcc16_rva23_novec_260820/json/checkpoints_all.json",
            comment=("run 100% coverage gcc16 rva23-novec SPEC06 checkpoints"),
        ),
        BenchmarkConfig(
            benchmark_type="h-spec06-0.5c",
            checkpoint_list="/nfs/home/share/gem5_ci/spec06_cpts/h_spec06/h_spec06_0.5c.lst",
            checkpoint_root="/nfs/home/share/gem5_ci/spec06_cpts/h_spec06/checkpoint-0-0-0",
            cluster_config="/nfs/home/share/gem5_ci/spec06_cpts/h_spec06/h_spec06_0.5c_h_profile_insts.json",
            comment=(
                "run 50% coverage H SPEC06 checkpoints with FS0 NEMU, "
                "GCPT restore, DRAMsim3 and 40M maxinsts"
            ),
        ),
        BenchmarkConfig(
            benchmark_type="h-spec06-1.0c",
            checkpoint_list="/nfs/home/share/gem5_ci/spec06_cpts/h_spec06/h_spec06_1.0c.lst",
            checkpoint_root="/nfs/home/share/gem5_ci/spec06_cpts/h_spec06/checkpoint-0-0-0",
            cluster_config="/nfs/home/share/gem5_ci/spec06_cpts/h_spec06/h_spec06_1.0c_h_profile_insts.json",
            comment=(
                "run 100% coverage H SPEC06 checkpoints with FS0 NEMU, "
                "GCPT restore, DRAMsim3 and 40M maxinsts"
            ),
        ),
        BenchmarkConfig(
            benchmark_type="spec17-1.0c",
            checkpoint_list=(
                "/nfs/home/share/checkpoints_profiles/"
                "spec17_rate_gcc16_rva23_novec_260904/checkpoint/checkpoint.lst"
            ),
            checkpoint_root=(
                "/nfs/home/share/checkpoints_profiles/"
                "spec17_rate_gcc16_rva23_novec_260904/checkpoint"
            ),
            cluster_config=(
                "/nfs/home/share/checkpoints_profiles/"
                "spec17_rate_gcc16_rva23_novec_260904/json/checkpoints_all.json"
            ),
            score_script="gem5-score-ci-17.sh",
            comment="run 100% coverage GCC16 RVA23 no-vector SPEC17 rate checkpoints",
        ),
        BenchmarkConfig(
            benchmark_type="spec06-rvv-1.0c",
            checkpoint_list="/nfs/home/xutongqiao/GEM5-CI/spec06_gcc15_rv64gcbv_O3_lto_base_nemu_single_core_NEMU_archgroup_2024-10-12-16-05/checkpoint06_rv64gcbv/checkpoint.lst",
            checkpoint_root="/nfs/home/xutongqiao/GEM5-CI/spec06_gcc15_rv64gcbv_O3_lto_base_nemu_single_core_NEMU_archgroup_2024-10-12-16-05/checkpoint06_rv64gcbv/",
            cluster_config="/nfs/home/xutongqiao/GEM5-CI/spec06_gcc15_rv64gcbv_O3_lto_base_nemu_single_core_NEMU_archgroup_2024-10-12-16-05/checkpoint06_rv64gcbv/cluster-0-0.json",
            comment="run 100% coverage spec06 rvv checkpoints",
        ),
        BenchmarkConfig(
            benchmark_type="spec06int-rvv-0.8c",
            checkpoint_list="/nfs/home/xutongqiao/GEM5-CI/spec06_gcc15_rv64gcbv_O3_lto_base_nemu_single_core_NEMU_archgroup_2024-10-12-16-05/checkpoint06_rv64gcbv/checkpoint_0.8c_int.lst",
            checkpoint_root="/nfs/home/xutongqiao/GEM5-CI/spec06_gcc15_rv64gcbv_O3_lto_base_nemu_single_core_NEMU_archgroup_2024-10-12-16-05/checkpoint06_rv64gcbv/",
            cluster_config="/nfs/home/xutongqiao/GEM5-CI/spec06_gcc15_rv64gcbv_O3_lto_base_nemu_single_core_NEMU_archgroup_2024-10-12-16-05/checkpoint06_rv64gcbv/cluster_0.8c_int.json",
            comment="run 80% coverage spec06 int rvv checkpoints",
        ),
        BenchmarkConfig(
            benchmark_type="gcc15-spec26-0.3c",
            checkpoint_list="/nfs/home/share/gem5_ci/spec26_cpts/rv64gcb_260718/spec26_0.3c.lst",
            checkpoint_root="/nfs/home/share/checkpoints_profiles/spec26_rate_gcc15_rv64gcb_260718/checkpoint",
            cluster_config="/nfs/home/share/gem5_ci/spec26_cpts/rv64gcb_260718/checkpoints_cov0.3.json",
            score_script="gem5-score-ci-26.sh",
            comment=(
                "run 30% coverage SPEC CPU2026 checkpoints plus "
                "722.palm_r/201821 regression"
            ),
        ),
        BenchmarkConfig(
            benchmark_type="gcc15-spec26-1.0c",
            checkpoint_list="/nfs/home/share/gem5_ci/spec26_cpts/rv64gcb_260718/spec26_1.0c.lst",
            checkpoint_root="/nfs/home/share/checkpoints_profiles/spec26_rate_gcc15_rv64gcb_260718/checkpoint",
            cluster_config="/nfs/home/share/gem5_ci/spec26_cpts/rv64gcb_260718/checkpoints_all.json",
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


def resolve_custom_benchmark(
    benchmark_type: str,
    checkpoint_path: str,
    json_path: str,
    output_dir: str | Path,
) -> BenchmarkConfig:
    """Resolve a profile and materialize its complete point list for both runners."""
    if not checkpoint_path or not Path(checkpoint_path).is_absolute():
        raise ValueError(
            "custom checkpoint_path must be an absolute directory"
        )
    supplied_root = Path(checkpoint_path).resolve()
    if not supplied_root.is_dir():
        raise ValueError(
            f"checkpoint directory does not exist: {supplied_root}"
        )
    roots = [
        supplied_root / name for name in ("checkpoint", "checkpoint-0-0-0")
    ]
    roots = [root for root in roots if root.is_dir()]
    if len(roots) > 1:
        raise ValueError(
            "multiple checkpoint directories; specify one explicitly"
        )
    root = roots[0] if roots else supplied_root
    if benchmark_type == "custom":
        suites = set(
            re.findall(r"spec(06|17|26)(?=[^0-9]|$)", checkpoint_path.lower())
        )
        if len(suites) != 1:
            raise ValueError(
                "cannot infer SPEC suite; select custom-spec06/17/26"
            )
        suite = suites.pop()
    else:
        suite = benchmark_type.removeprefix("custom-spec")
        if suite not in ("06", "17", "26"):
            raise ValueError(
                f"unsupported custom benchmark type: {benchmark_type}"
            )
    if json_path:
        cluster = Path(json_path)
        if not cluster.is_absolute():
            raise ValueError("json_path must be absolute")
    else:
        candidates = [
            root / "checkpoints_all.json",
            root / "cluster-0-0.json",
            root.parent / "json" / "checkpoints_all.json",
            root.parent / "cluster-0-0.json",
        ]
        candidates = list(
            dict.fromkeys(p.resolve() for p in candidates if p.is_file())
        )
        if len(candidates) != 1:
            raise ValueError(
                "expected one full-coverage JSON; specify json_path explicitly"
            )
        cluster = candidates[0]
    cluster = cluster.resolve()
    with cluster.open(encoding="utf-8") as source:
        profile = json.load(source)
    if not isinstance(profile, dict) or not profile:
        raise ValueError("checkpoint JSON must be a nonempty workload mapping")
    rows = []
    for workload, entry in sorted(profile.items()):
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", workload) or workload in (
            ".",
            "..",
        ):
            raise ValueError(f"invalid workload name: {workload!r}")
        if (
            not isinstance(entry, dict)
            or not isinstance(entry.get("points"), dict)
            or not entry["points"]
        ):
            raise ValueError(f"missing points for {workload}")
        insts = float(entry.get("insts", 0))
        if not math.isfinite(insts) or insts <= 0:
            raise ValueError(f"invalid instruction count for {workload}")
        for point, weight in sorted(entry["points"].items()):
            if not re.fullmatch(r"[0-9]+", point):
                raise ValueError(f"invalid checkpoint point: {point!r}")
            weight = float(weight)
            if not math.isfinite(weight) or weight < 0:
                raise ValueError(f"invalid weight for {workload}/{point}")
            directory = root / workload / point
            images = [
                p
                for p in directory.glob("*")
                if p.is_file() and p.suffix in (".gz", ".zstd")
            ]
            if len(images) != 1:
                raise ValueError(
                    f"expected one checkpoint image in {directory}, got {len(images)}"
                )
            rows.append(f"{workload}_{point} {workload}/{point} 0 0 20 20\n")
    identity = hashlib.sha256(
        (str(root) + "\n" + str(cluster)).encode() + cluster.read_bytes()
    ).hexdigest()[:12]
    resolved_type = f"custom-spec{suite}-{identity}"
    destination = Path(output_dir).resolve()
    destination.mkdir(parents=True, exist_ok=True)
    checkpoint_list = destination / f"{resolved_type}.lst"
    checkpoint_list.write_text("".join(rows), encoding="utf-8")
    return BenchmarkConfig(
        benchmark_type=resolved_type,
        checkpoint_list=str(checkpoint_list),
        checkpoint_root=str(root),
        cluster_config=str(cluster),
        score_script=(
            "gem5-score-ci.sh"
            if suite == "06"
            else f"gem5-score-ci-{suite}.sh"
        ),
        comment=f"custom SPEC{suite}: all {len(rows)} points from {cluster}",
    )


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
    parser.add_argument("--checkpoint-path", default="")
    parser.add_argument("--json-path", default="")
    parser.add_argument("--output-dir", default=".")
    args = parser.parse_args()

    try:
        if args.benchmark_type == "custom" or args.benchmark_type.startswith(
            "custom-spec"
        ):
            config = resolve_custom_benchmark(
                args.benchmark_type,
                args.checkpoint_path,
                args.json_path,
                args.output_dir,
            )
        else:
            if args.checkpoint_path or args.json_path:
                raise ValueError(
                    "checkpoint_path/json_path require a custom benchmark type"
                )
            config = resolve_benchmark(args.benchmark_type)
    except (KeyError, ValueError, OSError, TypeError) as error:
        parser.error(
            error.args[0] if isinstance(error, KeyError) else str(error)
        )

    if args.github_output:
        write_github_outputs(config, args.github_output)
    else:
        print(json.dumps(config.as_dict(), indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
