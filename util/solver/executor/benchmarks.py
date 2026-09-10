from __future__ import annotations

import subprocess

from util.xs_scripts.perf_benchmarks import (
    BenchmarkConfig,
    resolve_benchmark,
)


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
        "failed to locate checkpoint for fragment "
        f"{checkpoint_fragment!r} under {checkpoint_root}"
    )


def select_representative_checkpoint(
    config: BenchmarkConfig, filters: str = ""
) -> str:
    for fields in iter_workload_entries(config.checkpoint_list, filters):
        return locate_checkpoint(config.checkpoint_root, fields[1])
    raise FileNotFoundError(
        f"no workload matched filters {filters!r} in {config.checkpoint_list}"
    )
