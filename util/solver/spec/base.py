from __future__ import annotations

from collections import OrderedDict

from util.solver.spec.params import TunableField


class SolveSpecMeta(type):
    def __new__(mcls, name, bases, namespace):
        tunables = OrderedDict()
        for base in bases:
            for field_name, field in getattr(base, "__tunable_fields__", {}).items():
                tunables[field_name] = field
        for field_name, value in namespace.items():
            if isinstance(value, TunableField):
                tunables[field_name] = value

        cls = super().__new__(mcls, name, bases, namespace)
        cls.__tunable_fields__ = tunables
        return cls


class SolveSpec(metaclass=SolveSpecMeta):
    config_path = ""
    benchmark_type = ""
    specific_benchmarks = ""
    custom_bin = ""
    extra_args = ""
    solver_name = None

    @classmethod
    def iter_tunables(cls):
        return list(cls.__tunable_fields__.items())

    @classmethod
    def apply_trial(cls, root, trial) -> None:
        return None
