"""Performance datasets and the exact workloads selected for a run."""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import math
from pathlib import Path
import re


@dataclass(frozen=True)
class BenchmarkProfile:
    benchmark_type: str
    root: Path
    weights: Path | None
    suite: str = "06"
    mode: str = "single"
    listing: Path | None = None
    trace_format: str = ""
    archive_subdir: str = ""

    @property
    def score_script(self):
        suffix = "" if self.suite == "06" else "-" + self.suite
        return f"gem5-score-ci{suffix}.sh"


SHARE = Path("/nfs/home/share")
CI = SHARE / "gem5_ci"


# Named variants use the simulator checkout's lock; XSCC pins a 32 GiB REF.
@dataclass(frozen=True)
class CheckpointMode:
    reference: str | Path
    ref_env: str
    extra_args: tuple[str, ...]


CHECKPOINT_MODES = {
    "single": CheckpointMode(
        "normal-dedup", "GCBV_REF_SO", ("--enable-mem-dedup",)
    ),
    "single-32g": CheckpointMode(
        CI
        / "ref/releases/af9d4ce51fc2-gem5-r4-multi16g-zfa-cbo-sstc-single32g"
        / "normal-dedup-32g/riscv64-nemu-interpreter-so",
        "GCBV_REF_SO",
        ("--enable-mem-dedup", "--mem-size=32GB"),
    ),
    "smt": CheckpointMode(
        "multi", "GCBV_MULTI_CORE_REF_SO", ("--mem-size=16GB",)
    ),
    "h": CheckpointMode(
        "h",
        "GCBH_REF_SO",
        (
            "--difftest-ref-so={ref}",
            "--maxinsts=40000000",
            "--dramsim3-ini={repo}/ext/dramsim3/xiangshan_configs/xiangshan_DDR4_8Gb_x8_3200_2ch.ini",
        ),
    ),
}


PROFILES = SHARE / "checkpoints_profiles"
SPEC06_JSON = CI / "spec06_cpts"
GCC12 = (
    SHARE
    / "jiaxiaoyu/simpoint_checkpoint_zstd_format/spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc"
)
SMT = (
    SHARE / "xuyan/spec06_gcc12.2.0_rv64gcb_base_intFppOff_for_qemu_dual_core_"
    "disable_timer_QEMU_archgroup_2024-11-11-15-46/checkpoint-0-0-0"
)
PAIRED = SPEC06_JSON / "gcc12-spec06-paired.json"
GCC16 = PROFILES / "spec06_gcc16_rva23_novec_260820"
SPEC17 = PROFILES / "spec17_rate_gcc16_rva23_novec_260904"
XSCC = PROFILES / "spec06_xscc_rv64gcb_261013"
SPEC17_SMT = PROFILES / "spec17_gcc16_rva23_novec_2harts_260911"
H = CI / "spec06_cpts/h_spec06"
RVV = SHARE / "wujiabin/checkpoint-trigger/spec06_gcc15_rva23u64_lmul8_260723"
RVV_INT = Path(
    "/nfs/home/xutongqiao/GEM5-CI/spec06_gcc15_rv64gcbv_O3_lto_base_nemu_"
    "single_core_NEMU_archgroup_2024-10-12-16-05/checkpoint06_rv64gcbv"
)
SPEC26 = PROFILES / "spec26_rate_gcc15_rv64gcb_260718/checkpoint"
SPEC26_WEIGHTS = CI / "spec26_cpts/rv64gcb_260718"
GOOGLE = CI / "gem5_trace/google_trace"
CBP = CI / "gem5_trace/cbp_traces"

CATALOG = {
    p.benchmark_type: p
    for p in (
        BenchmarkProfile(
            "gcc12-spec06-0.8c", GCC12, SPEC06_JSON / "spec06_0.8c_int.json"
        ),
        BenchmarkProfile("gcc12-spec06-1.0c", GCC12, PAIRED),
        BenchmarkProfile(
            "gcc12-spec06-smt-0.3c",
            SMT,
            SPEC06_JSON / "spec06_0.3c.json",
            mode="smt",
        ),
        BenchmarkProfile("gcc12-spec06-smt-1.0c", SMT, PAIRED, mode="smt"),
        BenchmarkProfile(
            "gcc12-spec06-smt-int-1.0c",
            SMT,
            SPEC06_JSON / "gcc12-spec06-int-paired.json",
            mode="smt",
        ),
        BenchmarkProfile(
            "spec06-rva23-novec-gcc16-0.3c",
            GCC16 / "checkpoint",
            GCC16 / "json/checkpoints_cov0.3.json",
        ),
        BenchmarkProfile(
            "spec06-rva23-novec-gcc16-1.0c",
            GCC16 / "checkpoint",
            GCC16 / "json/checkpoints_all.json",
        ),
        BenchmarkProfile(
            "xscc-spec06-1.0c",
            XSCC / "checkpoint-0-0-0",
            XSCC / "checkpoint-0-0-0/cluster-0-0.json",
            mode="single-32g",
            archive_subdir=f"custom/{XSCC.name}",
        ),
        BenchmarkProfile(
            "gcc16-spec17-smt-1.0c",
            SPEC17_SMT / "checkpoint",
            SPEC17_SMT / "json/checkpoints_all.json",
            suite="17",
            mode="smt",
            archive_subdir=f"custom/{SPEC17_SMT.name}",
        ),
        BenchmarkProfile(
            "h-spec06-0.5c",
            H / "checkpoint-0-0-0",
            H / "h_spec06_0.5c_h_profile_insts.json",
            mode="h",
        ),
        BenchmarkProfile(
            "h-spec06-1.0c",
            H / "checkpoint-0-0-0",
            H / "h_spec06_1.0c_h_profile_insts.json",
            mode="h",
        ),
        BenchmarkProfile(
            "spec17-1.0c",
            SPEC17 / "checkpoint",
            SPEC17 / "json/checkpoints_all.json",
            suite="17",
        ),
        BenchmarkProfile(
            "spec06-rvv-1.0c",
            RVV / "checkpoint",
            RVV / "json/checkpoints_all.json",
        ),
        BenchmarkProfile(
            "spec06int-rvv-0.8c", RVV_INT, RVV_INT / "cluster_0.8c_int.json"
        ),
        BenchmarkProfile(
            "gcc15-spec26-0.3c",
            SPEC26,
            SPEC26_WEIGHTS / "checkpoints_cov0.3.json",
            suite="26",
        ),
        BenchmarkProfile(
            "gcc15-spec26-1.0c",
            SPEC26,
            SPEC26_WEIGHTS / "checkpoints_all.json",
            suite="26",
        ),
        BenchmarkProfile(
            "trace-google",
            GOOGLE,
            GOOGLE / "google_trace_weights.json",
            trace_format="tracertl",
            listing=GOOGLE / "google_traces.lst",
        ),
        BenchmarkProfile(
            "trace-google-0.3c",
            GOOGLE,
            GOOGLE / "google_trace_weights.json",
            trace_format="tracertl",
            listing=GOOGLE / "google_traces_0.3c.lst",
        ),
        BenchmarkProfile(
            "trace-cbp",
            CBP,
            None,
            trace_format="cbp2025",
            listing=CBP / "cbp_traces.lst",
        ),
        BenchmarkProfile(
            "trace-cbp-0.3c",
            CBP,
            None,
            trace_format="cbp2025",
            listing=CBP / "cbp_traces_0.3c.lst",
        ),
    )
}


def resolve_custom_benchmark(
    benchmark_type: str,
    checkpoint_path: str,
    json_path: str,
) -> BenchmarkProfile:
    """Discover a custom checkpoint root and its scoring weights."""
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
    identity = hashlib.sha256(
        (str(root) + "\n" + str(cluster)).encode() + cluster.read_bytes()
    ).hexdigest()[:12]
    resolved_type = f"custom-spec{suite}-{identity}"
    profile_root = (
        root.parent
        if root.name in ("checkpoint", "checkpoint-0-0-0")
        else root
    )
    profile_name = re.sub(r"[^A-Za-z0-9_.-]", "_", profile_root.name)
    profile_name = profile_name.strip(".") or "profile"
    return BenchmarkProfile(
        resolved_type,
        root,
        cluster,
        suite=suite,
        archive_subdir=f"custom/{profile_name}",
    )


def resolve_benchmark(name, checkpoint_path="", json_path=""):
    if name == "custom" or name.startswith("custom-spec"):
        return resolve_custom_benchmark(name, checkpoint_path, json_path)
    if checkpoint_path or json_path:
        raise ValueError("checkpoint_path/json_path require a custom profile")
    return CATALOG[name]


def spec_rows(weights):
    profile = json.loads(weights.read_text())
    if not isinstance(profile, dict) or not profile:
        raise ValueError("empty checkpoint profile")
    for workload, entry in profile.items():
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", workload) or workload in (
            ".",
            "..",
        ):
            raise ValueError(f"invalid workload name: {workload}")
        insts = float(entry.get("insts", 0))
        if not math.isfinite(insts) or insts <= 0 or not entry.get("points"):
            raise ValueError(
                f"invalid instruction count or points for {workload}"
            )
        for point, weight in entry["points"].items():
            if (
                not point.isascii()
                or not point.isdigit()
                or not math.isfinite(float(weight))
                or float(weight) < 0
            ):
                raise ValueError(
                    f"invalid point or weight for {workload}/{point}"
                )
            yield f"{workload}_{point} {workload}/{point} 0 0 20 20"


@dataclass(frozen=True)
class Workload:
    name: str
    image: Path
    row: str

    def arguments(self, trace_format=""):
        if trace_format:
            _, _, _, fw, dw, sample = self.row.split()
            warmup = int(fw) + int(dw)
            return [
                "--enable-trace-mode",
                "--trace-enable-decoupled-bp",
                f"--trace-file={self.image}",
                f"--trace-format={trace_format}",
                f"--warmup-insts-no-switch={warmup}",
                f"--maxinsts={warmup + int(sample)}",
            ]
        return [f"--generic-rv-cpt={self.image}"]


def select_workloads(rows, root, filters="", trace_format=""):
    tokens = [t.strip().lower() for t in filters.split(",") if t.strip()]
    seen = set()
    for raw in rows:
        row = raw.strip()
        if (
            not row
            or row.startswith("#")
            or (tokens and not any(t in row.lower() for t in tokens))
        ):
            continue
        name, key, *_ = row.split()
        if name in seen or name in (".", "..") or Path(name).name != name:
            raise ValueError(f"duplicate or invalid output name: {name}")
        seen.add(name)
        if Path(key).is_absolute() or ".." in Path(key).parts:
            raise ValueError(f"invalid workload path: {key}")
        path = root / key
        if trace_format and path.is_file():
            yield Workload(name, path.resolve(), row)
            continue
        if trace_format:
            candidates = [
                Path(f"{path}.{s}")
                for s in ("gz", "zstd", "xz", "champsimtrace")
            ]
        else:
            candidates = [
                p for p in path.glob("*") if p.suffix in (".gz", ".zstd")
            ]
        images = [p for p in candidates if p.is_file()]
        if len(images) != 1:
            raise ValueError(
                f"expected one checkpoint image or trace at {path}, found {len(images)}"
            )
        yield Workload(name, images[0].resolve(), row)
    if not seen:
        raise ValueError(f"no workloads selected by {filters!r}")


def workloads(profile, filters=""):
    rows = (
        profile.listing.read_text().splitlines()
        if profile.listing
        else spec_rows(profile.weights)
    )
    return select_workloads(rows, profile.root, filters, profile.trace_format)
