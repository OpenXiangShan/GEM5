# Spec 编写与本地验证

从仓库根目录执行以下命令；按需参考现有 spec 和 `util/solver/spec/`、`util/solver/parser/`。

## 生成 spec

把文件放在 `configs/solver_specs/<snake_case>.py`，定义一个唯一、清晰的 `SolveSpec` 子类。
遵循这些规则：

```python
from util.solver.spec import (
    Choice,
    InferTunable,
    Maximize,
    Range,
    SolveSpec,
    Stop,
)


class ExampleSearch(SolveSpec):
    config_path = "configs/example/kmhv3.py"
    benchmark_type = "spec06-rva23-novec-gcc16-0.3c"
    specific_benchmarks = ""
    custom_bin = ""
    extra_args = ""
    solver_name = "nsga2"

    threshold = InferTunable(
        target="system.cpu[0].branchPred.someThreshold",
        domain=Range(8, 32, step=4),
    )
    mode = InferTunable(
        target="system.cpu[0].branchPred.someMode",
        domain=Choice([0, 1, 2]),
    )

    objective = Maximize.stats("system.cpu.ipc")
    stop = Stop(max_trials=32, no_improve_trials=8, timeout_hours=6)
```

具体选择：

- 真实存在于 gem5 对象且需要运行时类型推断时用 `InferTunable`；想显式指定类型或
  定义抽象变量时用 `TunableParam.Unsigned/Float/Bool/VectorUnsigned`。
- 目标超过一个时用 `objectives = [...]`，语义是 Pareto 支配，不是加权和。
- 目标依赖多个参数的确定性关系时，只暴露少量语义变量，把关系写进
  `@classmethod apply_trial(cls, root, trial)`；用 `resolve_target()` 找对象，并用
  `owner._params[param_name].convert(value)` 转换后赋值。执行顺序是配置默认调优 →
  直接覆盖 → `apply_trial()`。
- `score_txt` 只能写成 `Maximize.score_txt("精确标签")`；不要写
  `Minimize.score_txt`。统计项名称必须与实际 `stats.txt` 完全一致。
- 不要把与用户目标无关的参数、目标或工作负载混进 spec；复杂工作负载集合优先在
  `specific_benchmarks`/`custom_bin` 中表达。

生成后输出：文件路径、类名、参数/域摘要、目标和停止条件，并明确哪些字段取了默认值。

## 本地校验（触发前必做）

按风险从低到高执行：

```bash
python3 -m py_compile configs/solver_specs/<file>.py
python3 - <<'PY'
from util.solver.parser.load_spec import parse_problem
problem = parse_problem("configs/solver_specs/<file>.py:<ClassName>")
print(problem.name, problem.config_path, problem.benchmark_type)
print([obj.display_name() for obj in problem.objective_list()])
print([param.name for param in problem.parameters])
PY
```

若有可用且已更新的 `build/RISCV/gem5.fast` 或 `gem5.opt`，再运行真实绑定和预检：

```bash
python3 util/solver/run_solver.py \
  --problem-ref configs/solver_specs/<file>.py:<ClassName> \
  --workdir /tmp/solver_dry_run_<name> \
  --gem5-build-type opt \
  --max-parallel-trials 1 \
  --max-parallel-workloads 1 \
  --dry-run
```

根据用户选择补上 `--config-path`、`--benchmark-type`、`--specific-benchmarks`、
`--custom-bin`、`--extra-args` 和 `--max-trials`。检查 `binding.json`、`parsed_problem.json`
和 `preview_trials.json`；重点确认 `owner_path`、`param_name`、`resolved_kind`、默认值和
预览候选。若二进制缺失或旧导致绑定不能执行，要说明这是环境/构建缺口，不要把它包装成
spec 已通过。

需要执行验证时，可在已授权的本地验证范围内选择有界的单工作负载短跑，例如
`--max-trials 1 --max-parallel-trials 1 --extra-args=--maxinsts=1000000`。
保留正式 spec 的预算和工作负载，单独标记验证输出；短跑结果不代表完整搜索结果。
