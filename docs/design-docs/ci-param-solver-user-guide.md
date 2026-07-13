# CI 参数求解框架使用说明

本文档面向当前仓库里的参数求解原型实现，描述用户如何：

- 编写一个 solver spec
- 先做 dry-run 校验
- 在本地发起一次 solver run
- 通过 GitHub Actions 手动触发一次 solver run
- 理解输出目录和常见失败原因

本文档描述的是“当前代码已经实现的行为”，不是设计目标。

## 1. 当前原型能做什么

当前原型已经支持以下最小闭环：

- 用 Python 类定义一个求解问题
- 从 gem5 config 中绑定目标参数，并读取真实 `ParamDesc` / `VectorParamDesc`
- 用 `GridSolver` 或 `RandomSolver` 生成 trial
- 在单机上执行 trial，并由 solver 自己调度每个 workload
- 从 `stats.txt` 或 `score.txt` 提取目标值
- 过滤无效 trial
- 生成结构化结果和简要 summary

当前用户入口主要有两个：

- 本地 controller：
  [util/solver/run_solver.py](/nfs/home/lixin/myworkspace/simulator/perf/1212_cdp_perf/GEM5/util/solver/run_solver.py)
- GitHub Actions 手动 workflow：
  [.github/workflows/manual-perf.yml](/nfs/home/lixin/myworkspace/simulator/perf/1212_cdp_perf/GEM5/.github/workflows/manual-perf.yml)

## 2. 当前原型的使用边界

当前实现有几个重要边界，使用前应先接受它们：

- 当前只支持 Python config 脚本，不支持 shell wrapper 作为 solver runtime 接入点。
- 当前已经接好 solver runtime、并在 CI 手动入口里暴露选择的 config 是：
  - [configs/example/idealkmhv3.py](/nfs/home/lixin/myworkspace/simulator/perf/1212_cdp_perf/GEM5/configs/example/idealkmhv3.py)
  - [configs/example/kmhv3.py](/nfs/home/lixin/myworkspace/simulator/perf/1212_cdp_perf/GEM5/configs/example/kmhv3.py)
  - [configs/example/kmhv2.py](/nfs/home/lixin/myworkspace/simulator/perf/1212_cdp_perf/GEM5/configs/example/kmhv2.py)
- 当前 solver runtime / CI 手动入口暂不支持 SMT config，也不接受 `gcc12-spec06-smt-*`
  benchmark_type。
- 这里的“支持多个 config”只表示 runtime 接口已经接好；某个具体 solver spec
  能否在所选 config 上工作，仍取决于它的 `target` 路径能否在该 config 上成功绑定。
  例如当前文档中的 `VTAGEIPCSearch` 仍依赖 `idealkmhv3.py` 里的 value predictor
  结构，不能直接切到 `kmhv3.py` / `kmhv2.py`。
- `target` 路径当前只支持：
  - 属性访问
  - 列表下标
  例如 `system.cpu[0].valuePred.predictors[1].histLengths`
- 不支持任意 Python 表达式 target。
- 当前支持的参数声明类型只有：
  - `InferTunable`
  - `TunableParam.Unsigned`
  - `TunableParam.Float`
  - `TunableParam.Bool`
  - `TunableParam.VectorUnsigned`
- 当前支持的搜索域只有：
  - `Range(...)`
  - `Choice([...])`
- 当前目标函数支持以下单目标或多目标组合写法：
  - `Maximize.stats("...")`
  - `Minimize.stats("...")`
  - `Maximize.score_txt("...")`
- 当前图表输出实际是 SVG，不是设计稿里写的 PNG。
- 当前没有实现 resume / restart / 多机调度。
- 当前已经支持两级并行：
  - trial 间并行
  - 单个 trial 内 workload 并行

## 3. 一个 solver spec 应该怎么写

solver spec 是一个 `SolveSpec` 子类。当前实现入口在：
[util/solver/spec/__init__.py](/nfs/home/lixin/myworkspace/simulator/perf/1212_cdp_perf/GEM5/util/solver/spec/__init__.py)

你需要至少定义这些类属性：

- `config_path`
- `benchmark_type`
- `objective` 或 `objectives`
- `stop`

可选属性：

- `specific_benchmarks`
- `custom_bin`
- `extra_args`
- `solver_name`

其中 `custom_bin` 现在不是“只要填了就自动生效”的字段。
只有当 `benchmark_type = "custom_bin"` 时，solver 才会进入自定义 workload 模式并实际使用
`custom_bin` 中提供的 checkpoint/bin 路径；否则 `custom_bin` 会被忽略，并在 controller /
workflow 预检日志里给出提示。

当前推荐直接参考：
[configs/solver_specs/vtage_ipc_search.py](/nfs/home/lixin/myworkspace/simulator/perf/1212_cdp_perf/GEM5/configs/solver_specs/vtage_ipc_search.py)

示例：

```python
from util.solver.spec import (
    Choice,
    InferTunable,
    Maximize,
    Minimize,
    Range,
    SolveSpec,
    Stop,
    TunableParam,
)


class VTAGEIPCSearch(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "gcc15-spec06-0.3c"
    specific_benchmarks = ""
    extra_args = ""
    solver_name = "random"

    histLengths = TunableParam.VectorUnsigned(
        target="system.cpu[0].valuePred.predictors[1].histLengths",
        domain=Choice([
            [0, 0, 3, 7, 15, 31, 63, 90, 127],
            [0, 0, 4, 8, 16, 32, 64, 96, 128],
        ]),
    )

    predictConfThreshold = InferTunable(
        target="system.cpu[0].valuePred.predictors[1].predictConfThreshold",
        domain=Range(512, 1536, step=128),
    )

    valueArrayUpgradeProb = InferTunable(
        target="system.cpu[0].valuePred.predictors[1].valueArrayUpgradeProb",
        domain=Range(0.0, 1.0, step=0.125),
    )

    objective = Maximize.stats("system.cpu.ipc")
    stop = Stop(max_trials=16, timeout_hours=6)
```

## 4. 参数声明怎么选

### 4.1 `InferTunable`

适合目标是“真实存在于 gem5 对象上的参数”时使用。

例如：

```python
predictConfThreshold = InferTunable(
    target="system.cpu[0].valuePred.predictors[1].predictConfThreshold",
    domain=Range(512, 1536, step=128),
)
```

运行前 bind 阶段会读取对象的 `_params[param_name]`，自动识别它是 `Unsigned`、`Float` 还是 `Vector...`。

### 4.2 `TunableParam.*`

适合两类场景：

- 你想显式约束参数类型
- 你想先定义一个抽象搜索变量，后续再在 `apply_trial()` 里做映射

例如：

```python
tableBudget = TunableParam.Unsigned(
    domain=Range(8, 32, step=4),
    default=16,
)
```

### 4.3 当前支持的 domain

#### `Range(start, stop, step)`

当前实现是“离散枚举”，不是连续采样。

```python
Range(512, 1536, step=128)
```

会产生：

```text
512, 640, 768, ..., 1536
```

浮点 `Range` 也会离散化，例如：

```python
Range(0.0, 1.0, step=0.125)
```

#### `Choice([...])`

适合显式枚举候选值，尤其适合当前原型里的 `VectorUnsigned`。

```python
Choice([
    [0, 0, 3, 7, 15, 31, 63, 90, 127],
    [0, 0, 4, 8, 16, 32, 64, 96, 128],
])
```

## 5. 目标函数和停止条件怎么写

### 5.1 目标函数

当前每个 objective 条目只支持三种写法：

#### 从 `stats.txt` 最大化单个指标

```python
objective = Maximize.stats("system.cpu.ipc")
```

注意：

- executor 实际会优先从 `workload_dir/stats.txt` 取
- 若不存在，会回退到 `workload_dir/m5out/stats.txt`
- 如果是 `custom_bin` 模式（即 `benchmark_type = "custom_bin"`）：对该 trial 下所有 workload 的该指标做算术平均
- 如果是普通 benchmark set 模式（例如 `gcc15-spec06-0.3c`）：不会再直接对 workload 等权平均，而是调用 `gem5_data_proc` 先按 slice / input 做加权，得到每个 benchmark 的 weighted metric，再在 benchmark 间做聚合
- benchmark 间默认聚合是算术平均；如果你想改成几何平均，可以写：

```python
objective = Maximize.stats(
    "system.cpu.ipc",
    benchmark_aggregate="geomean",
)
```

#### 从 `stats.txt` 最小化单个指标

```python
objective = Minimize.stats("system.cpu.branchMispredicts")
```

同样遵循上面的聚合规则，只是 solver 会把更小的值视为更优。

#### 从 `score.txt` 最大化单个指标

```python
objective = Maximize.score_txt("Estimated Int score per GHz")
```

当前还不支持 `Minimize.score_txt(...)`。

注意：

- `score_txt` 只适用于可由 `gem5_data_proc` 算分的 benchmark set
- 如果 `benchmark_type = "custom_bin"`，controller 会直接报错，因为单独的 workload bin / checkpoint 不具备 SPEC score 语义

#### 多目标写法

如果你要做真正的双目标/多目标优化，使用 `objectives = [...]`：

```python
objectives = [
    Maximize.stats("system.cpu.ipc"),
    Minimize.stats("system.cpu.branchMispredicts"),
]
```

当前多目标语义是 Pareto 支配，不是加权和：

- 一个 trial 若在所有 objective 上都不差于另一个 trial，且至少一个 objective 更好，则支配对方
- summary 会展示 Pareto frontier
- `best_result.json` 在多目标场景下表示 controller 选出的代表 frontier 点；完整 frontier 以 `summary.md` 和 `history.jsonl` 为准
- `no_improve_trials` 在多目标场景下按 “Pareto frontier 是否继续改进” 判断

如果你希望 candidate generation 也对 Pareto frontier 敏感，而不是仅在结果分析阶段做多目标比较，可以显式指定：

```bash
--solver-kind nsga2
```

当前 `nsga2` backend 通过 repo 本地虚拟环境 `.venv-solver/` 中安装的 `deap`
提供 NSGA-II 原语。

如果你是单目标问题，但希望使用基于 surrogate model 的贝叶斯优化，
可以显式指定：

```bash
--solver-kind bayes
```

当前 `bayes` backend 复用 `scikit-optimize` 的 `skopt.Optimizer` ask/tell
接口，而不是在仓库里重新实现 surrogate model 或 acquisition logic。

注意：

- `bayes` 目前只支持单目标问题
- 多目标仍应使用 `nsga2`
- `summary.md` 会额外显示 `Bayesian Optimization Progress`

如果你是单目标问题，但希望比 `random` 更有演化式的 exploitation / exploration，
可以显式指定：

```bash
--solver-kind ga
```

当前 `ga` backend 也复用同一个 `deap` 依赖，但只支持单目标问题；多目标仍应使用
`nsga2`。

### 5.2 停止条件

当前可用字段：

- `max_trials`
- `no_improve_trials`
- `timeout_hours`

示例：

```python
stop = Stop(
    max_trials=16,
    no_improve_trials=8,
    timeout_hours=6,
)
```

## 6. `apply_trial()` 什么时候需要

如果搜索变量不能直接映射到一个真实 gem5 参数，而是需要你自己写逻辑，就实现：

```python
@classmethod
def apply_trial(cls, root, trial):
    ...
```

这里的 `trial` 当前会被包装成一个带属性访问的对象，例如 `trial.tableBudget`。

执行顺序是：

1. config 脚本构建 baseline system
2. config 脚本执行默认 tuning
3. solver overlay 对 direct target 逐项赋值
4. `apply_trial()` 再执行复杂映射
5. instantiate / run

也就是说，`apply_trial()` 的优先级高于 overlay。

## 7. 本地使用方法

### 7.1 前置条件

本地 controller 不会自动编译 gem5，它依赖你已经有可用二进制：

- `build/RISCV/gem5.fast`
- 或 `build/RISCV/gem5.opt`

当前 controller 默认使用：

```text
--gem5-build-type fast
```

如果你刚新增了 SimObject 参数，`fast` 可能还是旧二进制；这时要么重新编译，要么显式切到已经更新过的 `opt`：

```bash
--gem5-build-type opt
```

controller 会自动设置：

- `GEM5_HOME`
- `GEM5_BUILD_TYPE`
- `GCBV_REF_SO` 默认值
- `M5_OVERRIDE_PY_SOURCE=true`

其中 `M5_OVERRIDE_PY_SOURCE=true` 的作用是让 gem5 运行时优先使用当前源码里的 Python 配置，而不是依赖旧的嵌入式 Python 模块。

如果你想在本地临时覆盖 spec 默认的 `config_path`，可以额外传：

```bash
--config-path configs/example/kmhv3.py
```

当前只接受这三个 repo-root-relative 路径：

- `configs/example/idealkmhv3.py`
- `configs/example/kmhv3.py`
- `configs/example/kmhv2.py`

或对应 basename：

- `idealkmhv3.py`
- `kmhv3.py`
- `kmhv2.py`

### 7.2 先做 dry-run

第一次写 spec 时，建议先做 dry-run，只验证：

- spec 能不能被 import
- target 能不能绑定
- 参数类型能不能推断
- solver 能不能生成 preview trials

命令：

```bash
python3 util/solver/run_solver.py \
  --problem-ref configs/solver_specs/vtage_ipc_search.py:VTAGEIPCSearch \
  --workdir /tmp/solver_dry_run \
  --max-parallel-trials 4 \
  --max-parallel-workloads 1 \
  --dry-run
```

dry-run 成功后，主要看这些文件：

- `/tmp/solver_dry_run/binding.json`
- `/tmp/solver_dry_run/parsed_problem.json`
- `/tmp/solver_dry_run/preview_trials.json`

### 7.3 本地做一次短跑验证

完整 benchmark 很慢。建议先加 `--maxinsts` 做短跑。

当前一条已验证的命令是：

```bash
python3 util/solver/run_solver.py \
  --problem-ref configs/solver_specs/vtage_ipc_search.py:VTAGEIPCSearch \
  --workdir /tmp/solver_smoke_opt_short \
  --specific-benchmarks bzip2_html_7052 \
  --max-parallel-trials 1 \
  --max-parallel-workloads 1 \
  --max-trials 1 \
  --timeout-minutes 60 \
  --gem5-build-type opt \
  --extra-args=--maxinsts=1000000
```

这里：

- `--specific-benchmarks bzip2_html_7052`
  只跑单个 workload
- `--max-trials 1`
  只跑一个 trial
- `--extra-args=--maxinsts=1000000`
  让 checkpoint 在较短时间内退出

### 7.4 本地跑完整 solver run

当 dry-run 和短跑没问题后，可以去掉 `--dry-run` 和 `--maxinsts`，按真实设置跑。

例如：

```bash
python3 util/solver/run_solver.py \
  --problem-ref configs/solver_specs/vtage_ipc_search.py:VTAGEIPCSearch \
  --workdir solver_runs/vtage_local \
  --solver-kind auto \
  --max-parallel-trials 4 \
  --max-parallel-workloads 2 \
  --gem5-build-type opt
```

### 7.5 `problem_ref` 可以怎么写

当前支持三种写法：

```text
ClassName
path/to/spec.py
path/to/spec.py:ClassName
```

解析顺序是：

1. 当前工作目录
2. `GEM5_HOME`
3. 仓库根目录
4. 如果只给了 `ClassName`，会在 `configs/solver_specs/` 下搜索唯一的
   `SolveSpec` 子类
5. 如果只给了 `path/to/spec.py`，要求该文件里恰好只有一个
   `SolveSpec` 子类

所以一般推荐直接写 repo-root-relative 路径，例如：

```text
configs/solver_specs/vtage_ipc_search.py:VTAGEIPCSearch
```

如果你只是手动触发默认 spec，也可以直接写：

```text
VTAGEIPCSearch
```

## 8. CI 手动触发方法

当前 workflow 是：
[.github/workflows/manual-perf.yml](/nfs/home/lixin/myworkspace/simulator/perf/1212_cdp_perf/GEM5/.github/workflows/manual-perf.yml)

它会做这些事：

1. checkout 指定 branch / SHA
2. build gem5 fast
3. 调 `python3 util/solver/run_solver.py`
4. 上传整个 solver run 目录为 artifact

### 8.1 workflow 输入项

当前支持：

- `note`
- `problem_ref`
- `configuration`
- `max_parallel_trials`
- `max_parallel_workloads`
- `solver_kind`
- `benchmark_type`
- `specific_benchmarks`
- `custom_bin`
- `extra_args`
- `max_trials`
- `branch`

输入语义补充：

- `configuration` 当前只允许：
  - `idealkmhv3.py`
  - `kmhv3.py`
  - `kmhv2.py`
- workflow 会把 `configuration` 解析成对应的 repo-root-relative `config_path`，
  然后传给 controller 的 `--config-path`
- 当前 workflow / controller 都会拒绝 `gcc12-spec06-smt-*`，因为 solver runtime
  还没有接 SMT config
- 如果 `benchmark_type = custom_bin`，则必须显式填写 `custom_bin`
- 如果 `benchmark_type` 选择的是内建切片组，`custom_bin` 会被忽略
- `specific_benchmarks` 只对内建 `benchmark_type` 生效；在 `custom_bin` 模式下会直接报错

### 8.2 一个推荐的 CI smoke 配置

如果你只是想验证 spec 和链路，不想直接烧完整 perf：

- `problem_ref`:
  `configs/solver_specs/vtage_ipc_search.py:VTAGEIPCSearch`
- `configuration`:
  `idealkmhv3.py`
- `solver_kind`:
  `random`
- `max_parallel_trials`:
  `1`
- `max_parallel_workloads`:
  `1`
- `specific_benchmarks`:
  `bzip2_html_7052`
- `max_trials`:
  `1`
- `extra_args`:
  `--maxinsts=1000000`

### 8.3 一个更接近真实搜索的 CI 配置

- `problem_ref`:
  `configs/solver_specs/vtage_ipc_search.py:VTAGEIPCSearch`
- `configuration`:
  `idealkmhv3.py`
- `solver_kind`:
  `auto`
- `max_parallel_trials`:
  `4`
- `max_parallel_workloads`:
  `2`
- `specific_benchmarks`:
  留空
- `max_trials`:
  留空或按需求覆盖
- `extra_args`:
  留空

### 8.4 并行参数的真实语义

- `max_parallel_trials`
  控制一个 batch 里最多同时跑多少个 solver trial。
- `max_parallel_workloads`
  控制单个 trial 内最多同时跑多少个 workload。
- 普通 benchmark 模式下，solver 会直接解析 workload list 并按
  `max_parallel_workloads` 并行启动各个 workload。
- `custom_bin` 模式下，如果 `benchmark_type = custom_bin` 且 `custom_bin` 填一个路径，就表示该 trial 只有一个 workload；
  如果填多个逗号/换行分隔路径，就会把这些自定义 checkpoint 当成该 trial
  的 workload 集合，并按 `max_parallel_workloads` 并行。
- `custom_bin` 模式下不允许 `score_txt` objective；如果你需要优化自定义 workload，只能使用 `stats(...)`

## 9. `solver_kind=auto` 到底怎么选

`auto` 的行为是：

1. 如果 spec 里写了 `solver_name = "grid"`，优先用 grid
2. 如果 spec 里写了 `solver_name = "random"`，优先用 random
3. 如果 spec 里写了 `solver_name = "bayes"`，优先用 bayes
4. 如果 spec 里写了 `solver_name = "ga"`，优先用 ga
5. 如果 spec 里写了 `solver_name = "nsga2"`，优先用 nsga2
5. 否则估算搜索空间大小
6. 如果搜索空间总点数 `<= max_trials`，用 `GridSolver`
7. 否则如果是多目标问题，用 `Nsga2Solver`
8. 否则用 `GaSolver`

如果你不想依赖自动判断，就显式传：

- `--solver-kind grid`
- 或 `--solver-kind random`
- 或 `--solver-kind bayes`
- 或 `--solver-kind ga`
- 或 `--solver-kind nsga2`

所以当前 `auto` 的直觉是：

- 小空间且预算足够穷举：`grid`
- 大空间多目标：`nsga2`
- 大空间单目标：`ga`

`bayes` 当前是显式 opt-in backend，不会在 `auto` 路径下被默认选中。

如果你希望固定某个 backend，而不是依赖自动判断，仍然建议在 spec 里写
`solver_name = "..."`，或在命令行显式传 `--solver-kind ...`。

## 10. 输出目录怎么看

一个 solver run 的工作目录大致会包含：

```text
<workdir>/
  metadata.json
  binding.json
  binding.log
  parsed_problem.json
  history.jsonl
  history.csv
  best_result.json
  summary.md
  charts/
    best_objective.svg
    trial_status.svg
  trials/
    trial_0001/
      overlay.json
      executor.log
      score.txt
      score.log
      raw/
        spec_all/
          ...
```

建议按这个顺序看：

### 10.1 `binding.json`

看 target 是否绑定到了你以为的对象和参数：

- `owner_path`
- `param_name`
- `resolved_kind`
- `default`

### 10.2 `parsed_problem.json`

看 controller 视角下最终生效的 spec：

- `specific_benchmarks`
- `extra_args`
- 绑定后的 parameter 信息

### 10.3 `summary.md`

快速看：

- 总 trial 数
- valid / invalid 数量
- 单目标时的 representative best
- 多目标时的 Pareto frontier 和 representative best
- `Solver Algorithm` section 中的 backend 参数
- `NSGA-II Progress` 或 `GA Progress` 这类算法特有过程信息
- top results 表

### 10.4 `history.jsonl`

这是最适合程序读的历史格式。每行一个 trial。

多目标时，每个 trial 会带：

- `objective_value`
  - 兼容字段，对应 primary objective
- `objective_values`
  - 所有 objective 的完整数值映射

### 10.5 `trials/trial_xxxx/executor.log`

看 controller 如何调度 workload，以及哪些 workload 返回了失败或超时。

### 10.6 `trials/trial_xxxx/raw/spec_all/<workload>/log.txt`

看单个 workload 的真实 gem5 运行日志。

### 10.7 `score.txt`

如果 objective 使用了 `score_txt`，或者是 benchmark set 上的 `stats(...)`（需要 weighted csv 聚合），executor 会调用 `gem5_data_proc` 生成相应的聚合结果文件。

## 11. 一个新 config 要怎么接入 solver runtime

如果将来你想把 solver 用到别的 config 脚本上，除了当前已经支持的
`idealkmhv3.py` / `kmhv3.py` / `kmhv2.py` 之外，至少还需要做两件事。

### 11.1 给 argparse 增加三个参数

当前接入点参考：
[configs/common/xiangshan.py](/nfs/home/lixin/myworkspace/simulator/perf/1212_cdp_perf/GEM5/configs/common/xiangshan.py)

需要支持：

- `--solver-problem-ref`
- `--solver-bind-output`
- `--solver-overlay`

### 11.2 在 instantiate 前调用 runtime integration

当前参考：
[configs/example/idealkmhv3.py](/nfs/home/lixin/myworkspace/simulator/perf/1212_cdp_perf/GEM5/configs/example/idealkmhv3.py)

需要在 root 构建好、真正 `instantiate / run` 前执行：

```python
if maybe_handle_solver_runtime(root, args):
    sys.exit(0)
```

这样 bind-only 模式才能提前退出，overlay 模式才能在 instantiate 前完成参数注入。

除此之外，如果你希望新 config 也能进入当前 solver CLI / CI 白名单，还需要同步更新
`util/solver/run_solver.py` / `util/solver/types.py` 里的允许列表，以及
workflow dispatch 的 `configuration` 选项。

## 12. 常见失败与排查方法

### 12.1 `SpecLoadError: spec file does not exist`

说明 `problem_ref` 路径写错了，或者运行目录切换后相对路径失效。

建议：

- 优先写 repo-root-relative 路径
- 或直接写绝对路径

### 12.2 `binding.json` 里 `resolved_kind` 不对

说明：

- target 指到了错误对象
- 或显式声明类型和真实 gem5 参数类型不匹配

先看：

- `binding.json`
- `parsed_problem.json`

### 12.3 trial 全部 `invalid`

先分三类看：

- `return_code != 0`
  看 `executor.log` 和 workload `log.txt`
- `abort_count > 0`
  看 `raw/spec_all/.../abort`
- `missing stats metric ...`
  看目标指标名是否真存在于 `stats.txt`

### 12.4 新增了 Python SimObject 参数，但某个二进制跑不起来

如果报的是类似：

```text
m5.internal.params.<Something>Params
```

相关错误，说明你正在用的 gem5 二进制没有重新生成 param glue。

处理方式：

- 重新编译你要用的 build type
- 或切到已经更新过的 `--gem5-build-type`

### 12.5 想快速验证，不想跑完整 slice

直接用：

```text
--specific-benchmarks <single workload>
--max-trials 1
--max-parallel-trials 1
--extra-args=--maxinsts=1000000
```

这通常是最高性价比的 smoke 组合。

## 13. 当前仓库里可直接参考的例子

- 随机搜索 VTAGE 三参数：
  [configs/solver_specs/vtage_ipc_search.py](/nfs/home/lixin/myworkspace/simulator/perf/1212_cdp_perf/GEM5/configs/solver_specs/vtage_ipc_search.py)
- 最小 grid 搜索：
  [configs/solver_specs/example_grid_search.py](/nfs/home/lixin/myworkspace/simulator/perf/1212_cdp_perf/GEM5/configs/solver_specs/example_grid_search.py)

已验证过的运行方式：

- dry-run：
  `/tmp/solver_dry_run2`
- grid dry-run：
  `/tmp/solver_grid_dry_run`
- `opt + maxinsts` 有效短跑：
  `/tmp/solver_smoke_opt_short3`

其中 `/tmp/solver_smoke_opt_short3/summary.md` 已证明当前实现可以完成一次有效搜索 run，并从 `system.cpu.ipc` 提取目标值。
