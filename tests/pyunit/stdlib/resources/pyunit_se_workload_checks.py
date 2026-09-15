# Copyright (c) 2026 Institute of Computing Technology, Chinese Academy of Sciences
# All rights reserved.

import os
import tempfile
import unittest
from pathlib import Path

from gem5.resources.se_workload import (
    ResourceCatalog,
    ResourceCatalogError,
)


class SEWorkloadResourceTestSuite(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.catalog_path = Path(__file__).parent / "refs" / (
            "se-workload-catalog.json"
        )

    def setUp(self):
        self.catalog = ResourceCatalog(str(self.catalog_path))
        self.resource_dir = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.resource_dir.cleanup()

    def test_latest_and_exact_workload_versions(self):
        latest = self.catalog.get_resource("se-test-workload")
        exact = self.catalog.get_resource(
            "se-test-workload", resource_version="1.0.0"
        )
        self.assertEqual(latest["resource_version"], "2.0.0")
        self.assertEqual(exact["resource_version"], "1.0.0")

    def test_suite_iteration_and_input_groups(self):
        suite = self.catalog.get_suite("se-test-suite")
        self.assertEqual(len(suite), 1)
        self.assertEqual(suite.get_input_groups(), {"quick", "all"})
        self.assertEqual(
            suite.with_input_group("quick").workloads[0].id,
            "se-test-workload",
        )
        self.assertEqual(
            suite.get_workload("se-test-workload").resource_version,
            "2.0.0",
        )
        with self.assertRaisesRegex(ResourceCatalogError, "not in suite"):
            suite.get_workload("missing")

    def test_workload_download_and_parameters(self):
        workload = self.catalog.obtain_se_workload(
            "se-test-workload",
            resource_directory=self.resource_dir.name,
        )
        self.assertEqual(workload.arguments, ("--answer", "42"))
        self.assertEqual(workload.env_list, ("SE_TEST=works",))
        self.assertTrue(Path(workload.executable).is_file())
        self.assertTrue(Path(workload.stdin_file).is_file())
        self.assertEqual(
            Path(workload.executable).name,
            "se-test-binary-1.0.0",
        )

    def test_binary_can_be_obtained_directly(self):
        workload = self.catalog.obtain_se_workload(
            "se-test-binary",
            resource_directory=self.resource_dir.name,
        )
        self.assertEqual(workload.architecture, "RISCV")
        self.assertTrue(Path(workload.executable).is_file())

    def test_cached_file_is_reused(self):
        workload = self.catalog.obtain_se_workload(
            "se-test-workload",
            resource_directory=self.resource_dir.name,
        )
        old_mtime = os.stat(workload.executable).st_mtime_ns
        workload = self.catalog.obtain_se_workload(
            "se-test-workload",
            resource_directory=self.resource_dir.name,
        )
        self.assertEqual(
            os.stat(workload.executable).st_mtime_ns, old_mtime
        )

    def test_bad_cached_file_is_replaced(self):
        workload = self.catalog.obtain_se_workload(
            "se-test-binary",
            resource_directory=self.resource_dir.name,
        )
        Path(workload.executable).write_bytes(b"bad cache entry")
        workload = self.catalog.obtain_se_workload(
            "se-test-binary",
            resource_directory=self.resource_dir.name,
        )
        self.assertEqual(
            Path(workload.executable).stat().st_size,
            4814352,
        )

    def test_unsupported_function_is_rejected(self):
        with self.assertRaisesRegex(
            ResourceCatalogError, "unsupported function"
        ):
            self.catalog.obtain_se_workload(
                "se-unsupported-workload",
                resource_directory=self.resource_dir.name,
            )

    def test_missing_resource_is_reported(self):
        with self.assertRaisesRegex(ResourceCatalogError, "was not found"):
            self.catalog.get_resource("missing")


if __name__ == "__main__":
    unittest.main()
