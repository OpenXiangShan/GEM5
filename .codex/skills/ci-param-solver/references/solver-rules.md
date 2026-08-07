# 求解规则与输入矩阵

本文是 `ci-param-solver` 工作流的简明参考。如果工作流或解析器发生变化，应重新检查
仓库源码。

## 求解 spec 约定

`configs/solver_specs/*.py` 中应包含一个 `SolveSpec` 子类，并定义：

- 必填项：`config_path`、`benchmark_type`、`objective` 或 `objectives`、`stop`
- 可选项：`specific_benchmarks`、`custom_bin`、`extra_args`、`solver_name`、
  `summary_top_n`

支持的配置路径为 `configs/example/idealkmhv3.py`、
`configs/example/kmhv3.py` 和 `configs/example/kmhv2.py`；工作流也接受这三个文件的
基本名称。目标路径只能使用属性访问和列表下标，不能写任意 Python 表达式。

参数声明方式：

| 声明 | 用途 |
| --- | --- |
| `InferTunable` | 绑定时推断真实 gem5 `ParamDesc` 类型 |
| `TunableParam.Unsigned` / `Float` / `Bool` / `VectorUnsigned` | 显式指定类型，或定义由 `apply_trial()` 映射的抽象变量 |

搜索域是离散的：`Range(start, stop, step)` 和 `Choice([...])`。

## 目标函数约定

```python
Maximize.stats("metric", benchmark_aggregate="mean")
Minimize.stats("metric", benchmark_aggregate="mean")
Maximize.score_txt("Estimated Int score per GHz")
```

不支持 `Minimize.score_txt`。当 `benchmark_type = "custom_bin"` 时，不能使用
`score_txt` 目标。对于内建基准测试组，统计项会先经过仓库中的 `gem5_data_proc` 路径
按权重聚合，再做基准测试间聚合；自定义工作负载则对各个工作负载结果做算术聚合。

目标超过一个时使用 `objectives = [...]`。求解器使用 Pareto 支配关系比较候选试验。
`nsga2` 是明确的多目标后端；`bayes` 和 `ga` 只支持单目标。如果有限搜索空间不超过
试验预算，`auto` 会选择 `grid`；否则多目标选择 NSGA-II，单目标选择 GA，除非
`solver_name` 或命令行参数进行了覆盖。

每次运行的第一个真实试验都是配置默认值基线。该试验不包含参数赋值，并占用一个
`max_trials` 名额。

## 工作流输入约定（`manual-solve.yml`）

| 输入项 | 含义 | 默认值或约束 |
| --- | --- | --- |
| `note` | 任务标题前缀 | `Manual Solver Run` |
| `problem_ref` | 类名、文件路径或文件路径加类名 | 必填；默认 `VTAGEIPCSearch` |
| `configuration` | 运行时配置文件基本名称 | `kmhv2.py`、`kmhv3.py`、`idealkmhv3.py` |
| `benchmark_type` | 内建检查点组或 `custom_bin` | 使用工作流中列出的非 SMT 选项 |
| `max_parallel_trials` | 并行候选试验数 | 默认 `4`，必须为正数 |
| `max_parallel_workloads` | 单个试验内的并行工作负载数 | 默认 `4`，必须为正数 |
| `distributed_servers` | 留空、`default` 或显式服务器列表 | 留空表示当前运行节点 |
| `distributed_jobs_per_server` | 每台服务器的工作负载并发上限 | `0` 表示自动推导 |
| `solver_kind` | 候选生成算法 | `auto`、`grid`、`random`、`bayes`、`ga`、`nsga2` |
| `specific_benchmarks` | 内建检查点组的筛选条件 | 自定义工作负载模式下不得使用 |
| `custom_bin` | `.bin`、`.gz` 或 `.zstd` 绝对路径 | 自定义工作负载模式下必填 |
| `extra_args` | 附加 gem5 命令行参数 | 默认留空 |
| `max_trials` | 运行时停止条件覆盖值 | 留空时使用 spec 中的值 |
| `branch` | 检出的远端引用 | 留空时使用工作流 SHA |

工作流拒绝 `gcc12-spec06-smt-*`。在自定义工作负载模式下，
`specific_benchmarks` 必须留空，并且不能使用 `score_txt` 目标。在内建检查点组模式下，
`custom_bin` 会被忽略，基准测试筛选条件可以留空。

## 执行语义

一个候选试验由一组参数赋值以及所有选定工作负载组成。所有工作负载完成后，再聚合该
候选试验的目标值。`max_parallel_trials` 限制一个批次中的并行候选试验数，
`max_parallel_workloads` 限制单个候选试验内部的并行工作负载数。启用分布式服务器后，
全局工作负载并发预算仍为二者的乘积，`distributed_jobs_per_server` 负责限制每台服务器。

## 产物查看顺序

工作流上传 `summary.md`、`best_result.json`、`metadata.json`、
`parsed_problem.json`、`binding.json`、`history.jsonl`、`history.csv` 和 `charts/`。
被取消的任务可能没有 `summary.md`，此时应优先检查 JSON 产物。
