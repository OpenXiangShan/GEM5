from __future__ import annotations

import os
from pathlib import Path
import subprocess


DEFAULT_PANDAS_SITE_PACKAGES = "/nfs/home/yanyue/.local/lib/python3.12/site-packages"


def _spec_version_from_score_script(score_script: str) -> str:
    return "17" if score_script.endswith("-17.sh") else "06"


def run_score_evaluator(
    *,
    gem5_data_proc: str | Path,
    score_script: str,
    raw_spec_dir: str | Path,
    cluster_config: str | Path,
    repo_root: str | Path,
    score_path: str | Path,
    score_log: str | Path,
    scratch_dir: str | Path,
) -> int:
    gem5_data_proc = Path(gem5_data_proc)
    score_path = Path(score_path)
    score_log = Path(score_log)
    scratch_dir = Path(scratch_dir)
    scratch_dir.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    existing_pythonpath = env.get("PYTHONPATH", "")
    env["PYTHONPATH"] = ":".join(
        item
        for item in [str(gem5_data_proc), DEFAULT_PANDAS_SITE_PACKAGES, existing_pythonpath]
        if item
    )
    env["GEM5_HOME"] = str(repo_root)
    results_dir = scratch_dir / "results"
    results_dir.mkdir(parents=True, exist_ok=True)
    batch_csv = results_dir / "gem5-score-example.csv"
    score_csv = results_dir / "gem5-score-example-score.csv"
    batch_py = gem5_data_proc / "batch.py"
    compute_weighted_py = gem5_data_proc / "simpoint_cpt" / "compute_weighted.py"
    spec_version = _spec_version_from_score_script(score_script)

    with score_path.open("w", encoding="utf-8") as out_handle, score_log.open(
        "w", encoding="utf-8"
    ) as log_handle:
        batch_result = subprocess.run(
            [
                "python3",
                str(batch_py),
                "-s",
                str(raw_spec_dir),
                "-o",
                str(batch_csv),
            ],
            check=False,
            cwd=gem5_data_proc,
            env=env,
            stdout=out_handle,
            stderr=log_handle,
        )
        if batch_result.returncode != 0:
            return batch_result.returncode

        result = subprocess.run(
            [
                "python3",
                str(compute_weighted_py),
                "-r",
                str(batch_csv),
                "-j",
                str(cluster_config),
                "--score",
                str(score_csv),
                "--out-dir",
                str(results_dir),
                "-v",
                spec_version,
            ],
            check=False,
            cwd=gem5_data_proc,
            env=env,
            stdout=out_handle,
            stderr=log_handle,
        )
    return result.returncode
