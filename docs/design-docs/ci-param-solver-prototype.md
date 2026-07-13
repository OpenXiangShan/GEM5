# CI 参数求解框架原型实施方案

当前实现对应的用户侧使用说明见：
[ci-param-solver-user-guide.md](/nfs/home/lixin/myworkspace/simulator/perf/1212_cdp_perf/GEM5/docs/design-docs/ci-param-solver-user-guide.md)

## 1. 目标

本文档定义一个面向当前 GEM5 仓库的参数求解框架原型。该原型的目标不是一次性做完通用优化平台，而是先在 GitHub CI 上跑通一条最小可用链路：

- 用户用 Python 风格定义求解问题
- 框架解析并校验用户输入
- 使用最简单的随机搜索或穷举搜索生成 trial
- 在现有 CI 的单机环境中执行 trial，可串行也可并行
- 在 CI 手动入口中允许用户在受限的非 SMT Kunminghu config 范围内覆盖
  spec 默认 `config_path`
- 对 trial 结果做聚合、过滤、持久化
- 在 CI 中输出简明表格与基础图表

当前阶段的工作重点是用户解析层。其余层优先满足“跑通原型”，保留后续替换和扩展的接口。

## 1.1 原型跑通案例

原型以当前树中的 `VTAGE` 为最小跑通案例，搜索以下三个参数：

- `histLengths`
- `predictConfThreshold`
- `valueArrayUpgradeProb`

目标函数使用 `stats.txt` 中的：

- `system.cpu.ipc`

优化方向为最大化 `system.cpu.ipc`。

之所以选择该案例，是因为它同时覆盖了原型最关键的三类输入能力：

- `histLengths`：`VectorParam.Unsigned`
- `predictConfThreshold`：`Param.Unsigned`
- `valueArrayUpgradeProb`：`Param.Float`

这三个参数在当前仓库中的定义位于 [ValuePredictor.py](/nfs/home/lixin/myworkspace/simulator/perf/1212_cdp_perf/GEM5/src/cpu/valuepred/ValuePredictor.py:49) 附近，当前默认 `idealkmhv3.py` 中也已实例化 `VTAGE()`，见 [idealkmhv3.py](/nfs/home/lixin/myworkspace/simulator/perf/1212_cdp_perf/GEM5/configs/example/idealkmhv3.py:61)。

## 2. 非目标

原型阶段明确不做以下内容：

- 多机调度与分布式执行
- 复杂求解算法，如贝叶斯优化、遗传算法、HyperBand 等
- 通用 resume/restart 协议
- 任意 Python 脚本作为目标函数的完全自由执行模型
- 对所有 gem5 config 脚本零修改兼容
- SMT config 与 `gcc12-spec06-smt-*` benchmark_type
- 任意 repo 相对 config path；当前只允许 `configs/example/idealkmhv3.py`、
  `configs/example/kmhv3.py`、`configs/example/kmhv2.py`
- 对任意 `VectorParam` 进行复杂结构搜索

## 3. 分层架构

框架拆成五层，每层只通过明确的数据对象交互，不直接依赖相邻层的内部实现。

### 3.1 用户解析层

职责：

- 加载用户定义的 Python 求解类
- 解析搜索参数、默认值、搜索域、优化目标、停止条件
- 识别并校验与 gem5 `Param.*` / `VectorParam.*` 相关的类型约束
- 生成标准化后的 `ParsedProblem`

输入：

- `problem_ref`
  - 形式建议为 `path/to/spec.py:ClassName`
- 可选 workflow 输入
  - branch/SHA
  - note
  - benchmark override
  - configuration override

输出：

- `ParsedProblem`
- `ParsedParameter[]`
- `ObjectiveSpec`
- `StopSpec`

这一层是原型重点，需要做得最扎实。

### 3.2 求解器层

职责：

- 根据 `ParsedProblem` 生成候选 trial
- 维护 trial 历史并决定下一批评估点
- 根据停止条件决定是否结束

输入：

- `ParsedProblem`
- `TrialHistory`

输出：

- `TrialRequest[]`

原型阶段仅实现两种求解器：

- `GridSolver`
- `RandomSolver`

### 3.3 执行层

职责：

- 接收 `TrialRequest`
- 将 trial 转换为一次具体 gem5 执行请求
- 在单机 CI 环境中串行或并行执行
- 采集原始产物路径、退出码、耗时等执行信息

输入：

- `TrialRequest[]`
- `ExecutorConfig`

输出：

- `TrialExecutionResult[]`

原型阶段执行层使用现有 CI 的单机 runner，不做 workflow 递归触发，不做多机。

### 3.4 数据处理层

职责：

- 从原始执行结果中提取 `score.txt`、`stats.txt`、abort 状态等
- 根据求解目标做结果整合
- 剔除无效结果
- 维护 trial 历史、best-so-far、结构化持久化数据

输入：

- `TrialExecutionResult[]`
- `ObjectiveSpec`

输出：

- `EvaluatedTrial[]`
- `BestResult`
- `PersistentRunState`

### 3.5 可选扩展层

职责：

- 渲染 markdown summary
- 生成图表
- 导出 CSV/JSON/HTML
- 产生后续分析所需的标准数据文件

输入：

- `EvaluatedTrial[]`
- `BestResult`
- `PersistentRunState`

输出：

- `summary.md`
- `history.csv`
- `history.jsonl`
- `charts/*.png`
- 可选 `report.html`

## 4. 五层之间的标准接口

### 4.1 核心数据对象

建议使用 Python dataclass 统一各层接口。

```python
@dataclass
class ParsedProblem:
    name: str
    config_path: str
    benchmark_type: str
    specific_benchmarks: str
    extra_args: str
    parameters: list["ParsedParameter"]
    objective: "ObjectiveSpec"
    stop: "StopSpec"
    hooks: "ProblemHooks"


@dataclass
class ParsedParameter:
    name: str
    kind: str
    default: object
    domain: "DomainSpec"
    binding: "BindingSpec"
    gem5_param_desc: object | None


@dataclass
class TrialRequest:
    trial_id: str
    generation: int
    assignments: dict[str, object]


@dataclass
class TrialExecutionResult:
    trial_id: str
    status: str
    return_code: int | None
    duration_sec: float
    outdir: str
    raw_files: dict[str, str]


@dataclass
class EvaluatedTrial:
    trial_id: str
    status: str
    objective_value: float | None
    metrics: dict[str, float | int | str]
    invalid_reason: str | None
```

### 4.2 层间依赖规则

- 用户解析层不依赖具体求解算法
- 求解器层不依赖具体执行实现
- 执行层不解释优化目标，只负责执行
- 数据处理层不决定下一 trial，只输出标准化评估结果
- 扩展层只消费结构化结果，不回写求解器核心逻辑

## 5. 用户解析层设计

## 5.1 设计目标

用户输入层应：

- 保持 gem5 Python 配置风格
- 尽量复用 gem5 `Param.*` / `VectorParam.*` 类型系统
- 将“参数类型”和“搜索空间”分开表达
- 支持简单 direct binding，也支持复杂映射逻辑

## 5.2 推荐用户接口

用户通过 Python 类定义一个求解问题。

```python
from solver.spec import SolveSpec, TunableParam, InferTunable
from solver.spec import Range, Choice, Maximize, Stop


class EStrideSearch(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "gcc15-spec06-0.8c"
    specific_benchmarks = ""
    extra_args = ""

    logMaxConfidence = InferTunable(
        target="system.cpu[0].valuePred.predictors[1].logMaxConfidence",
        domain=Range(6, 14, step=1),
    )

    thresholdPercent = TunableParam.Float(
        target="system.cpu[0].valuePred.predictors[1].thresholdPercent",
        domain=Range(0.15, 0.45, step=0.01),
        default=0.35,
    )

    objective = Maximize.score_txt("Estimated Int score per GHz")
    stop = Stop(max_trials=24, no_improve_trials=8, timeout_hours=12)
```

CI / CLI 允许在运行时用 `configuration` / `--config-path` 覆盖 spec 中的
`config_path`，但仅限以下非 SMT config：

- `configs/example/idealkmhv3.py`
- `configs/example/kmhv3.py`
- `configs/example/kmhv2.py`

这个覆盖只改变 gem5 config 入口，不保证某个 spec 的 `target` 路径在所有 config
上都存在。例如 `VTAGEIPCSearch` 依赖 `idealkmhv3.py` 中的 value predictor 层级，
切到 `kmhv3.py` 或 `kmhv2.py` 时应先用 bind-only/dry-run 验证 target 绑定。

对于原型跑通案例，推荐的示例更接近下面这种形式：

```python
from solver.spec import SolveSpec, TunableParam, InferTunable
from solver.spec import Range, Choice, Maximize, Stop


class VTAGEIPCSearch(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "gcc15-spec06-0.3c"
    specific_benchmarks = ""
    extra_args = ""

    histLengths = TunableParam.VectorUnsigned(
        target="system.cpu[0].valuePred.predictors[1].histLengths",
        domain=Choice([
            [0, 0, 3, 7, 15, 31, 63, 90, 127],
            [0, 0, 4, 8, 16, 32, 64, 96, 128],
            [0, 0, 2, 6, 14, 30, 62, 94, 126],
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

## 5.3 为什么不用裸 `Param.*`

`Param.Unsigned` / `VectorParam.Unsigned` 很适合作为参数类型与值转换器，但不够表达完整搜索空间。它只能说明：

- 这是哪种 gem5 参数类型
- 默认值是什么
- 赋值时如何 `convert()`

它不能单独表达：

- 搜索区间
- 步长
- 枚举集合
- 采样分布
- 是否必须 direct bind 到某个现有参数

因此原型建议：

- gem5 `ParamDesc` 负责类型系统
- `TunableParam.*` / `InferTunable` 负责搜索空间和绑定关系

## 5.4 参数声明的两种模式

### 模式 A：自动推断

当 target 指向现有 gem5 SimObject 参数时，搜索器可在 bind 阶段读取 `_params`，识别真实参数描述。

```python
logMaxConfidence = InferTunable(
    target="system.cpu[0].valuePred.predictors[1].logMaxConfidence",
    domain=Range(6, 14, step=1),
)
```

适用场景：

- 搜索单个真实存在的 gem5 参数
- 希望搜索器自动识别 `Unsigned` / `Bool` / `Percent` / `VectorParam` 等类型

### 模式 B：显式声明

当搜索变量是抽象变量、或需要映射到多个参数时，用户显式给出参数类型。

```python
tableBudget = TunableParam.Unsigned(
    domain=Range(8, 32, step=4),
    default=16,
)
```

然后通过 hook 应用：

```python
class MySearch(SolveSpec):
    tableBudget = TunableParam.Unsigned(domain=Range(8, 32, step=4))

    @classmethod
    def apply_trial(cls, root, trial):
        root.system.cpu[0].bp.tableA = trial.tableBudget * 2
        root.system.cpu[0].bp.tableB = trial.tableBudget * 4
```

原型跑通案例中：

- `predictConfThreshold` 和 `valueArrayUpgradeProb` 适合先走自动推断模式
- `histLengths` 是 `VectorParam.Unsigned`，原型阶段建议保守处理为“整向量枚举候选集”，不要做逐元素自由搜索

## 5.5 用户解析层的两阶段处理

### 阶段 1：静态解析

输入 `problem_ref`，完成：

- import Python 模块
- 找到 `SolveSpec` 子类
- 提取类字段、参数定义、目标和停止条件
- 做基础 schema 校验

输出 `ParsedProblem` 的静态部分。

### 阶段 2：运行前绑定

在执行环境中构建一次 baseline system，然后：

- 解析每个 `target path`
- 定位目标对象和参数名
- 读取目标对象 `_params[param_name]`
- 若为 `InferTunable`，推断真实 gem5 参数类型
- 生成绑定后的 `BindingSpec`

这样可以避免把“如何解析 gem5 对象路径”和“如何 import 用户 spec”混在一起。

## 5.6 与 gem5 参数系统的关系

原型需要复用这些现成能力：

- `ParamDesc.convert()`
- `VectorParamDesc.convert()`
- `SimObject._params`

原型不应直接复用这些机制：

- `SimObject` 元类本身
- `SimObject` 的父子关系和实例化语义

也就是说，`SolveSpec` 应是轻量元类，不是新的 SimObject。

## 5.7 关于 `Percent`

解析层不能仅根据字段名推断语义。例如：

- `thresholdPercent` 在某些代码里可能是 `Float`
- gem5 自带 `Percent` 类型本身是 0..100 的整数参数

因此解析层应优先以真实 `ParamDesc` 为准，而不是以字段名或目标名中的字符串猜测类型。

## 5.8 原型案例中的 VTAGE 绑定约定

为了让原型尽快跑通，建议将 `VTAGE` 示例限定在当前 `idealkmhv3.py` 的默认 predictor 结构上，即：

- `cpu.valuePred = CompositeValuePredictor(...)`
- `predictors = [IdealConstantLVP(), VTAGE()]`

因此原型中的 `target path` 可固定写为：

- `system.cpu[0].valuePred.predictors[1].histLengths`
- `system.cpu[0].valuePred.predictors[1].predictConfThreshold`
- `system.cpu[0].valuePred.predictors[1].valueArrayUpgradeProb`

后续如果 `CompositeValuePredictor` 的子项顺序可能变化，再考虑引入“按 child name 绑定”的更稳健机制；原型阶段不做这层泛化。

## 6. 参数注入与 trial 执行方式

## 6.1 原则

不同 trial 不应通过修改 config Python 源文件文本实现。那样会：

- 依赖文本结构
- 不利于并行
- 不利于调试和复现
- 难以支持复杂映射

原型建议采用：

- 固定 config script
- 外部 trial overlay
- 构建后统一 apply

## 6.2 trial overlay

搜索器在执行每个 trial 前生成一份 overlay 文件。

```json
{
  "trial_id": "trial_0017",
  "assignments": [
    {
      "name": "logMaxConfidence",
      "target": "system.cpu[0].valuePred.predictors[1].logMaxConfidence",
      "value": 13
    },
    {
      "name": "thresholdPercent",
      "target": "system.cpu[0].valuePred.predictors[1].thresholdPercent",
      "value": 0.35
    }
  ]
}
```

然后 config script 在 baseline 参数设置完成后执行统一 apply：

```text
build system -> apply default tuning -> apply solver overlay -> run
```

## 6.3 apply 次序

建议固定为：

1. config script 解析原始命令行参数
2. 构建 baseline system
3. 应用 config script 自己的默认 tuning
4. 应用 solver overlay
5. 若 spec 定义了 `apply_trial()` hook，则执行 hook
6. instantiate / run

这样能确保 solver 具有最高优先级，同时仍允许复杂映射。

## 7. 求解器层设计

## 7.1 基础接口

```python
class BaseSolver:
    def initialize(self, problem: ParsedProblem) -> None: ...
    def propose(self, history: list[EvaluatedTrial], batch_size: int) -> list[TrialRequest]: ...
    def observe(self, results: list[EvaluatedTrial]) -> None: ...
    def should_stop(self, history: list[EvaluatedTrial]) -> bool: ...
```

## 7.2 原型实现

### `GridSolver`

适用：

- 小搜索空间
- 需要完全覆盖

特点：

- 结果容易验证
- 与用户解析层调试配合最好

### `RandomSolver`

适用：

- 搜索空间较大
- 需要快速跑通 end-to-end

特点：

- 实现简单
- 容易和后续算法共用接口

## 7.3 batch 模型

原型不做复杂异步调度。求解器按 batch 输出 trial：

- batch 内部允许 trial 间并行
- 每个 trial 内部允许 workload 并行
- batch 完成后统一进入数据处理层
- 数据处理层输出结果后再进入下一轮

## 8. 执行层设计

## 8.1 原型执行模式

原型使用单个 CI workflow run，在单台 self-hosted runner 上执行，不递归触发新的 workflow。

理由：

- 状态集中
- 调试简单
- 不依赖 workflow 嵌套和 run 轮询
- 能直接复用当前 runner 环境与 benchmark 数据

## 8.2 与现有 CI 的关系

执行层复用现有 perf CI 的 evaluator 逻辑和产物格式，但不强依赖 `workflow_call` 的递归编排方式。

建议抽取一层本地 evaluator helper，使以下逻辑能被复用：

- benchmark 选择
- gem5 build
- workload list 解析与 checkpoint 定位
- workload 并行调度
- `gem5-score-ci.sh` / `gem5-score-ci-17.sh` 计分
- archive 目录布局

## 8.3 建议执行接口

```python
class BaseExecutor:
    def prepare(self, problem: ParsedProblem) -> None: ...
    def run_trials(self, trials: list[TrialRequest]) -> list[TrialExecutionResult]: ...
    def cleanup(self) -> None: ...
```

原型执行器实现：

- `CiLocalParallelExecutor`

配置项：

- `max_parallel_trials`
- `max_parallel_workloads`
- `build_once_per_run`
- `reuse_workdir`
- `timeout_per_trial`

## 8.4 build 策略

原型阶段优先：

- 每个 solver run build 一次
- 同一个 run 中多个 trial 共享同一个 gem5 binary

前提：

- 搜索只改运行时 Python 参数，不改 C++/SConscript

若未来支持“会触发重新编译的参数”，应另行分层，不纳入原型。

## 9. 数据处理层设计

## 9.1 职责边界

数据处理层负责把执行结果变成“可用于优化决策”的结构化信息，不在此层决定下一个 trial。

## 9.2 原型需要处理的输入

- `score.txt`
- `stats.txt`
- trial 执行目录
- abort 文件数量
- return code
- duration

## 9.3 原型阶段的有效性判定

至少支持以下规则：

- 执行返回码非 0 视为无效
- 存在 abort 文件视为无效
- 目标指标缺失视为无效

无效结果仍需持久化，但不会进入有效 trial 排名。

## 9.4 原型阶段的目标类型

优先只支持：

- `score.txt` 中的命名指标
- `stats.txt` 中的单个命名指标
- 简单加权和

暂不支持任意 Python 目标函数。

对原型跑通案例，第一优先级是支持：

- 从 `stats.txt` 提取 `system.cpu.ipc`

并以此作为唯一排序依据。

## 9.5 持久化数据

原型建议在工作目录内落这些文件：

```text
solver_runs/<run_id>/
  metadata.json
  parsed_problem.json
  history.jsonl
  history.csv
  best_result.json
  trials/
    trial_0001/
      overlay.json
      raw/
      processed.json
    trial_0002/
      ...
  charts/
    best_objective.png
    trial_status.png
  summary.md
```

这些文件同时作为：

- CI artifact
- 本地调试证据
- 后续 resume 的基础素材

## 10. 可选扩展层设计

## 10.1 原型输出目标

原型至少产出：

- top-k 结果 markdown 表格
- objective 随 trial 变化折线图
- trial 状态统计图
- `history.csv` / `history.jsonl`

## 10.2 CI 展示方式

优先策略：

- 在 `GITHUB_STEP_SUMMARY` 中写入摘要表格
- 图表写成 PNG 并 upload artifact

可选增强：

- 若 summary 对本地图片渲染稳定，则尝试内联图片

## 10.3 扩展层接口

```python
class BaseReporter:
    def render_summary(self, history: list[EvaluatedTrial], best: EvaluatedTrial | None) -> str: ...
    def render_charts(self, history: list[EvaluatedTrial], outdir: str) -> list[str]: ...
```

原型实现：

- `MarkdownSummaryReporter`
- `MatplotlibChartReporter`

## 11. 原型工作流设计

## 11.1 新增 workflow

建议新增一个面向手动触发的 workflow，例如：

- `.github/workflows/manual-perf.yml`

输入建议保守：

- `problem_ref`
- `branch`
- `note`
- 可选 `max_parallel_trials` override

不要在 workflow_dispatch UI 中暴露大量搜索参数。用户解析层才是主要入口。

## 11.2 workflow 主要步骤

1. checkout 指定 branch/SHA
2. build gem5
3. 运行 solver controller 脚本
4. 产出 summary / charts / CSV / JSON
5. upload artifact

## 11.3 controller 脚本

建议提供统一 CLI，例如：

```bash
python3 util/solver/run_solver.py \
  --problem-ref configs/solver_specs/estride_search.py:EStrideSearch \
  --workdir $GITHUB_WORKSPACE/solver_runs/$RUN_ID \
  --max-parallel-trials 4
```

该 controller 串起五层：

- parse
- solve
- execute
- process
- report

## 12. 目录与模块建议

```text
util/solver/
  spec/
    base.py
    params.py
    domain.py
    objective.py
    stop.py
  parser/
    load_spec.py
    bind_targets.py
  solver/
    base.py
    grid.py
    random.py
  executor/
    base.py
    ci_local.py
    evaluator.py
  processing/
    base.py
    extract.py
    aggregate.py
    persist.py
  reporting/
    base.py
    markdown.py
    charts.py
  runtime/
    overlay.py
    apply.py
    path_resolver.py
  run_solver.py

configs/solver_specs/
  estride_search.py
  example_grid_search.py
```

## 13. MVP 范围

## 13.1 必做

- Python `SolveSpec` + `TunableParam` + `InferTunable`
- 解析层静态解析与 target bind
- `GridSolver` / `RandomSolver`
- 单机 CI 执行器
- `stats.txt` 中 `system.cpu.ipc` 目标提取
- `score.txt` 目标提取
- 无效 trial 过滤
- JSONL/CSV 持久化
- CI summary + 2 张基础图

## 13.2 可以延后

- `stats.txt` 复杂表达式
- `VectorParam` 逐元素搜索
- resume
- 跨 workflow / 多机执行
- 高级求解算法
- HTML 富报告

## 14. 风险与约束

### 风险 1：target path 解析复杂

说明：

- `system.cpu[0].valuePred.predictors[1].x` 这类路径需要稳定解析

应对：

- 原型先支持属性访问和 list 下标
- 不支持任意表达式
- 先只支持原型案例所需路径：`system.cpu[0].valuePred.predictors[1].*`

### 风险 2：config 脚本差异大

说明：

- 不同 config 脚本构建对象和默认赋值顺序不同

应对：

- 原型只先支持少量指定 config 脚本
- 通过统一的 overlay apply 点接入

### 风险 3：trial 代价高

说明：

- 即便单机并行，完整 perf 评测成本仍高

应对：

- 原型先限制 `max_trials`
- 优先支持 benchmark 子集
- 求解器先用简单算法，避免无意义 trial

### 风险 4：CI 图表展示能力有限

说明：

- GitHub summary 对本地图片展示能力有限且实现细节敏感

应对：

- 图表 artifact 化优先
- summary 中保证文字和表格足够可读

## 15. 建议实施顺序

### 阶段 1：用户解析层打底

- 定义 `SolveSpec` / `TunableParam` / `InferTunable`
- 完成 `problem_ref` 加载
- 完成静态 schema 校验
- 完成 target path bind 与 `_params` 推断

### 阶段 2：最小求解闭环

- 实现 `GridSolver` / `RandomSolver`
- 实现 overlay 生成
- 实现本地 evaluator 包装
- 跑通 VTAGE 三参数 + `system.cpu.ipc` 案例

### 阶段 3：结果处理与展示

- 提取 `score.txt`
- 实现 invalid 过滤
- 实现 history 持久化
- 生成 markdown summary 和两张基础图

### 阶段 4：CI 接线

- 新增 `manual-perf.yml`
- 跑通单个示例 spec
- 验证 artifact、summary、chart 输出

## 16. 原型完成标准

当满足以下条件时，可认为原型完成：

- 用户能新增一个 Python spec 文件来定义求解问题
- CI 能读取该 spec 并自动完成 trial 生成与执行
- 至少支持随机和穷举两种简单求解方式
- trial 结果能被结构化持久化
- CI summary 能显示 best result 与基本趋势
- 代码结构已按五层拆分，后续可替换求解器和执行器而不重写解析层
- `VTAGE(histLengths, predictConfThreshold, valueArrayUpgradeProb)` 以 `system.cpu.ipc` 为目标的示例能在 CI 中完成至少一次有效搜索 run
