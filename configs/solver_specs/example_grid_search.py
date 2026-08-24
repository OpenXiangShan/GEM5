from util.solver.spec import Choice, Maximize, SolveSpec, Stop, TunableParam


class ExampleGridSearch(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "spec06-rva23-novec-gcc16-0.3c"
    specific_benchmarks = ""
    extra_args = ""
    solver_name = "grid"

    histLengths = TunableParam.VectorUnsigned(
        target="system.cpu[0].valuePred.predictors[1].histLengths",
        domain=Choice([
            [0, 0, 3, 7, 15, 31, 63, 90, 127],
            [0, 0, 4, 8, 16, 32, 64, 96, 128],
        ]),
    )

    objective = Maximize.stats("system.cpu.ipc")
    stop = Stop(max_trials=2, timeout_hours=1)
