"""Execute the workflow guard against successful and failed guest logs."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

import yaml


ROOT = Path(__file__).resolve().parents[3]
WORKFLOW = yaml.safe_load(
    (ROOT / ".github/workflows/gem5-perf-template.yml").read_text()
)
SCRIPT = next(
    step["run"]
    for job in WORKFLOW["jobs"].values()
    for step in job.get("steps", [])
    if step.get("name") == "Check guest kernel logs"
)


class GuestLogsTest(unittest.TestCase):
    def run_guard(self, root):
        return subprocess.run(
            ["bash", "-e", "-c", SCRIPT],
            env=dict(os.environ, PERF_ARCHIVE_DIR=str(root),
                     GITHUB_STEP_SUMMARY=str(root / "summary")),
            capture_output=True, text=True,
        )

    def test_kernel_panic_overrides_success(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            job = root / "spec_all/failed"
            job.mkdir(parents=True)
            (job / "completed").touch()
            (job / "log.txt").write_bytes(
                b"[ 42.0] Kernel panic - not syncing: Fatal exception\r\r\n"
                b"Exiting because a thread reached the max instruction count\n"
                b"exit_status: 0\n"
            )
            self.assertEqual(self.run_guard(root).returncode, 1)
            self.assertFalse((job / "completed").exists())
            self.assertTrue((job / "abort").exists())
            self.assertIn("refusing to score", (root / "summary").read_text())
            self.assertEqual(self.run_guard(root).returncode, 1)

    def test_normal_output_and_existing_failures_are_preserved(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name, marker in [("passed", "completed"), ("failed", "abort")]:
                job = root / "spec_all" / name
                job.mkdir(parents=True)
                (job / marker).touch()
                (job / "log.txt").write_bytes(
                    b"Kernel command line: panic=-1\n"
                    b"Warning: optional device unavailable\n"
                )
            self.assertEqual(self.run_guard(root).returncode, 0)
            self.assertTrue((root / "spec_all/passed/completed").exists())
            self.assertFalse((root / "spec_all/passed/abort").exists())
            self.assertTrue((root / "spec_all/failed/abort").exists())

    def test_missing_stats_directory_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertNotEqual(self.run_guard(Path(tmp)).returncode, 0)


if __name__ == "__main__":
    unittest.main()
