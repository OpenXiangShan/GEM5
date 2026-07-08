import unittest
import tempfile
from pathlib import Path

from util.solver.runtime.path_resolver import resolve_object, resolve_target
from util.solver.processing.extract import collect_workload_stats


class FakeParamDesc:
    def convert(self, value):
        return f"converted:{value}"


class FakePredictor:
    def __init__(self):
        self.histLengths = [1, 2, 3]
        self._params = {"histLengths": FakeParamDesc()}


class FakeValuePred:
    def __init__(self):
        self.predictors = [object(), FakePredictor()]


class FakeCpu:
    def __init__(self):
        self.valuePred = FakeValuePred()


class FakeSystem:
    def __init__(self):
        self.cpu = [FakeCpu()]


class FakeRoot:
    def __init__(self):
        self.system = FakeSystem()


class PathResolverTestCase(unittest.TestCase):
    def test_resolve_object_with_indices(self):
        root = FakeRoot()
        predictor = resolve_object(root, "system.cpu[0].valuePred.predictors[1]")
        self.assertIsInstance(predictor, FakePredictor)

    def test_resolve_target(self):
        root = FakeRoot()
        owner, owner_path, param_name = resolve_target(
            root,
            "system.cpu[0].valuePred.predictors[1].histLengths",
        )
        self.assertEqual(owner_path, "system.cpu[0].valuePred.predictors[1]")
        self.assertEqual(param_name, "histLengths")
        self.assertEqual(owner._params[param_name].convert([4, 5]), "converted:[4, 5]")

    def test_collect_workload_stats_from_m5out(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            stats_dir = Path(tmpdir) / "spec_all" / "demo" / "m5out"
            stats_dir.mkdir(parents=True)
            (stats_dir / "stats.txt").write_text(
                "system.cpu.ipc 3.25 # IPC\\n",
                encoding="utf-8",
            )
            metrics = collect_workload_stats(Path(tmpdir) / "spec_all", "system.cpu.ipc")
            self.assertEqual(metrics, {"demo": 3.25})


if __name__ == "__main__":
    unittest.main()
