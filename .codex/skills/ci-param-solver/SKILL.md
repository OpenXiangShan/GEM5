---
name: ci-param-solver
description: 用于在 OpenXiangShan/GEM5 中把设计空间探索和参数搜索需求整理成 `configs/solver_specs/` 下的 Python `SolveSpec`，做本地预检，并在用户明确授权且输入完备后通过 `gh` 触发 `.github/workflows/manual-solve.yml`。当用户要求生成、修改、校验或提交求解 spec，或要求发起 CI 参数求解任务时使用。
---

# CI 参数求解

这个技能覆盖一条可追溯的闭环：收集需求 → 生成 spec → 本地校验 → 确认 CI 输入 →
触发求解工作流 → 回查任务。它不替用户猜测目标、工作负载、分支或搜索预算；缺少会
改变实验含义的信息时，先提问并暂停写文件/触发 CI。

## 0. 先确认当前仓库事实

以当前源码和工作流为准，文档只是解释。开始时检查：

```bash
sed -n '1,260p' docs/tools/others/ci-param-solver-user-guide.md
sed -n '1,260p' .github/workflows/manual-solve.yml
```

需要时再读：

- `util/solver/spec/`：`SolveSpec`、搜索域、目标函数和停止条件的描述语法
- `util/solver/parser/`：`problem_ref`、目标绑定和运行时覆盖
- `configs/solver_specs/`：现有 spec 的命名和 `apply_trial()` 写法
- `references/solver-rules.md`：本技能的约束速查

如果本地工作流与远端注册状态不一致，以 `gh workflow view/list` 的结果为准；本地有
`.yml` 不代表远端已经可以 `workflow_dispatch`。

## 1. 信息完备性闸门

先把用户需求归纳成下面四组。每组缺少会影响实验语义的字段都必须追问；不要用邻近
基准测试、默认参数或“看起来合理”的统计项名称代替。

### A. 求解问题（生成 spec 必需）

必须明确：

- **要改什么**：每个可调参数的名称、真实 gem5 目标路径、类型/表示方式，以及离散候选。
  `Range(start, stop, step)` 是离散枚举，`Choice([...])` 是显式枚举。
- **约束**：参数之间的公式、硬性预算、非法组合和默认值。需要耦合映射时，说明应在
  `apply_trial()` 中如何从抽象变量推导真实参数。
- **目标**：每个目标是最大化还是最小化、来自哪个精确的 `stats.txt` 指标或
  `score.txt` 标签；多目标必须说明优先目标（它会影响代表性最佳结果的展示）。
- **停止条件**：至少给出 `max_trials`；如果要早停，再给 `no_improve_trials` 和
  `timeout_hours`。

可由 spec 或 CI 输入继承、因此不必重复追问的字段：`config_path`、`benchmark_type`、
`specific_benchmarks`、`custom_bin`、`extra_args`、`solver_name`。但如果二者冲突，必须让
用户选择，而不是静默覆盖。

### B. 工作负载语义

必须选择其中一种：

- 内建检查点组：给出准确的 `benchmark_type`，可选给出逗号分隔的
  `specific_benchmarks`。
- `custom_bin`：给出一个或多个可访问的绝对路径（逗号或换行分隔），并确认目标只能使用
  `stats(...)`，不能使用 `score_txt(...)`。

`custom_bin` 下不能同时给 `specific_benchmarks`；内建检查点组下的 `custom_bin` 会被忽略。
SMT 基准测试（`gcc12-spec06-smt-*`）当前求解运行时不支持，必须换成非 SMT 选择。

### C. CI 执行设置

至少确认：

- `configuration`：`idealkmhv3.py`、`kmhv3.py` 或 `kmhv2.py`，并且目标路径能在该配置
  上绑定；配置只支持属性访问和列表下标形式的目标路径。
- `solver_kind`：`auto`、`grid`、`random`、`bayes`、`ga` 或 `nsga2`。多目标优先
  `nsga2`；`bayes`/`ga` 只用于单目标；小空间且预算覆盖全空间时可用 `grid`。
- `max_parallel_trials` 和 `max_parallel_workloads`。说明总并发约为二者乘积，并根据
  工作负载数量平衡二者，不要把候选试验和工作负载混为一个概念。
- `distributed_servers`：留空表示当前运行节点；`default` 或显式列表/范围表示分布式。
  若使用分布式，再确认 `distributed_jobs_per_server`（`0` 让求解器推导）。
- `extra_args`：例如短跑时的 `--maxinsts=1000000`；不要为完整搜索擅自加入短跑限制。
- `branch`/标签/SHA：CI 检出的真实来源。spec 必须已经存在于该远端引用；仅在本地未
  提交的文件无法被 CI 使用。

没有明确指定时可以采用并在回复中列出的非语义默认值：`solver_kind=auto`、并行度
`4/4`、分布式留空、`extra_args` 为空、工作流的默认 `note`。`configuration`、工作负载、
目标、预算、远端引用不可擅自猜测。

### D. 动作授权

区分两种请求：

1. “只生成/校验 spec”：写文件并做本地验证，不触发远端。
2. “触发 CI”：只有用户明确要求触发，并且 spec 已在目标远端引用后才执行触发。

如果信息不全，先用一条简短的定向问题收齐缺口，例如：

```text
为了生成并触发这个求解任务，还缺 4 项：
1. 要绑定的配置和每个参数的精确目标路径/候选域是什么？
2. 工作负载使用哪个 benchmark_type（或哪些 custom_bin）？
3. 目标指标的精确 stats 名称/score.txt 标签、方向和停止预算是什么？
4. CI 要跑哪个已推送的分支/标签/SHA，以及是否现在触发？
```

只列仍缺失的项；用户已经明确的内容不要重复询问。收到答案后，先回显一份“最终 spec
语义 + CI 输入”摘要，再写文件或触发任务。

## 2. 生成 spec

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
    benchmark_type = "gcc15-spec06-0.3c"
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

## 3. 本地校验（触发前必做）

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

完整工作负载很慢时，用户同意后才做一次短跑验证：单个工作负载、`--max-trials 1`、
`--max-parallel-trials 1`、`--extra-args=--maxinsts=1000000`。不要用短跑结果替代完整搜索。

## 4. 触发 GitHub Actions

### 4.1 触发前检查

仅当用户明确要求触发时执行：

```bash
gh --version
gh auth status
gh workflow view manual-solve.yml --repo OpenXiangShan/GEM5 --ref <branch-or-tag> --yaml
```

如果工作流未注册、认证失效、目标引用不含 spec，先停止并报告阻塞原因。不要因为本地
存在 `.github/workflows/manual-solve.yml` 就声称远端可触发。当前仓库的实际入口是
`manual-solve.yml`；不要把求解任务误发到普通 `manual-perf.yml`。

触发前再次检查 CI 输入与 spec 的一致性：

- `problem_ref` 用 `ClassName`、`path.py` 或 `path.py:ClassName`；推荐使用相对于仓库根目录
  的 `path.py:ClassName`。
- CI `configuration`、`benchmark_type`、`specific_benchmarks`、`custom_bin`、
  `extra_args`、`max_trials` 会覆盖/补充 spec 中相应值；把最终生效值展示给用户。
- `custom_bin` 和 `score_txt`、SMT、`custom_bin` 与特定基准测试筛选条件的冲突必须在触发
  前报错。
- `--ref` 和工作流的 `branch` 输入通常填同一个远端分支/标签/SHA；必须确保该引用
  已推送，且包含 spec 文件和工作流。

如果 spec 只存在于本地有未提交修改的工作区，先告诉用户 CI 看不到它，并请用户提供已
推送的引用，或明确授权后再讨论提交/推送；不要在没有授权时自动推送。

### 4.2 使用随附脚本组装和回查

先用 `--dry-run` 只打印命令并检查输入，再在用户授权后加 `--yes` 实际触发：

```bash
python3 .codex/skills/ci-param-solver/scripts/solver_ci_dispatch.py dispatch \
  --repo OpenXiangShan/GEM5 \
  --workflow manual-solve.yml \
  --branch <branch-or-sha> \
  --problem-ref configs/solver_specs/<file>.py:<ClassName> \
  --configuration <idealkmhv3.py|kmhv3.py|kmhv2.py> \
  --benchmark-type <group-or-custom_bin> \
  --solver-kind <auto|grid|random|bayes|ga|nsga2> \
  --max-parallel-trials <N> \
  --max-parallel-workloads <N> \
  [--distributed-servers <list-or-default>] \
  [--distributed-jobs-per-server <N>] \
  [--specific-benchmarks <filters>] \
  [--custom-bin <absolute-paths>] \
  [--extra-args '<gem5 args>'] \
  [--max-trials <N>] \
  [--note '<title>'] \
  --dry-run
```

`--dry-run` 输出完整 `gh workflow run` 命令但不联网触发。展示该命令和最终参数；
得到用户对“现在触发”的明确授权后，去掉 `--dry-run` 并加 `--yes`。随附脚本会：

1. 本地解析 `problem_ref`，校验配置、基准测试/自定义工作负载、目标、SMT、算法和正整数约束；
2. 检查 `gh auth` 与工作流注册状态；
3. 调用 `gh workflow run manual-solve.yml --repo ... --ref ... -f key=value ...`；
4. 查询最近的 `workflow_dispatch` 任务，并报告 URL、状态、结论、来源分支/SHA。

如果找不到新任务，不要只说“已触发”；报告触发输出和回查结果，并让用户检查远端引用、
工作流注册及令牌的 `workflow` 权限。

### 4.3 不使用随附脚本时的命令形式

```bash
gh workflow run manual-solve.yml \
  --repo OpenXiangShan/GEM5 --ref <branch> \
  -f note='<title>' \
  -f problem_ref='configs/solver_specs/foo.py:FooSearch' \
  -f configuration='kmhv3.py' \
  -f benchmark_type='gcc15-spec06-0.3c' \
  -f max_parallel_trials='4' \
  -f max_parallel_workloads='4' \
  -f solver_kind='nsga2' \
  -f branch='<branch>'
```

不要把空的可选输入伪装成用户已确认的语义；可以省略它们，让工作流默认值生效，或在
命令和回复中明确写出空值。

## 5. 结果交接

触发成功后给出：

- spec 文件和类名、最终生效的 CI 输入
- 本地验证结果及验证缺口
- `gh` 触发输出、任务 URL、当前 status/conclusion/head SHA
- 用户下一步查看产物的顺序：`summary.md` → `best_result.json` → `metadata.json` →
  `binding.json` → `parsed_problem.json` → `history.jsonl/history.csv` → `charts/`。

求解产物不使用性能 CI 的 `score.txt` 归档定位脚本；需要分析求解结果时，
直接用 `gh run view`/`gh run download` 获取上述产物。

## 资源

- `scripts/solver_ci_dispatch.py`：输入校验、dry-run 命令预览、工作流触发和新任务回查。
- `scripts/self_test.py`：基于现有 BOP specs 的离线合法/非法输入回归测试，不触发 CI。
- `references/solver-rules.md`：从当前源码和用户指南整理的 DSL、目标、工作负载、并行
  语义速查。
- `references/test-scenarios.md`：信息不足、规则冲突和信息完整的对话级验收提示词与预期行为。
