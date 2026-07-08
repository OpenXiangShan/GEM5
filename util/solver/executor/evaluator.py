from __future__ import annotations

import os
from pathlib import Path
import subprocess


DEFAULT_PANDAS_SITE_PACKAGES = "/nfs/home/yanyue/.local/lib/python3.12/site-packages"


def run_score_evaluator(
    *,
    gem5_data_proc: str | Path,
    score_script: str,
    raw_spec_dir: str | Path,
    cluster_config: str | Path,
    repo_root: str | Path,
    score_path: str | Path,
    score_log: str | Path,
) -> int:
    gem5_data_proc = Path(gem5_data_proc)
    score_path = Path(score_path)
    score_log = Path(score_log)
    env = os.environ.copy()
    existing_pythonpath = env.get("PYTHONPATH", "")
    env["PYTHONPATH"] = ":".join(
        item
        for item in [str(gem5_data_proc), DEFAULT_PANDAS_SITE_PACKAGES, existing_pythonpath]
        if item
    )
    env["GEM5_HOME"] = str(repo_root)

    with score_path.open("w", encoding="utf-8") as out_handle, score_log.open(
        "w", encoding="utf-8"
    ) as log_handle:
        result = subprocess.run(
            [
                "bash",
                f"example-scripts/{score_script}",
                str(raw_spec_dir),
                str(cluster_config),
            ],
            check=False,
            cwd=gem5_data_proc,
            env=env,
            stdout=out_handle,
            stderr=log_handle,
        )
    return result.returncode
