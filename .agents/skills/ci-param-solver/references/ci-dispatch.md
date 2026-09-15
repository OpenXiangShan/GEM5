# CI 触发与结果交接

仅在需要准备或执行远端求解时读取。已有授权在本次任务中持续有效。
命令从仓库根目录执行，`scripts/solver_ci_dispatch.py` 位于本 skill 根目录。

## 触发 GitHub Actions

### 触发前检查

准备远端命令时可按需做这些只读检查；实际触发前确认接口和引用：

```bash
gh --version
gh auth status
gh workflow view manual-solve.yml --repo OpenXiangShan/GEM5 --ref <branch-or-tag> --yaml
```

如果工作流未注册、认证失效、目标引用不含 spec，暂停 dispatch 并报告阻塞原因；继续可完成的本地工作。
沙箱中的认证异常应先在正常 shell 复核，不凭一次 `gh auth status` 断言 token 失效。不要因为本地
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

spec 必须存在于目标远端引用。已有提交/推送授权时完成这些步骤；否则先完成本地 spec、
预检和命令草稿，再说明发布到远端所需的动作。触发授权本身不自动扩大为任意分支推送授权。

### 使用随附脚本组装和回查

先用 `--dry-run` 打印命令并检查输入；用户已授权触发时，预检通过后直接加 `--yes` 执行，无需再次确认：

```bash
python3 .agents/skills/ci-param-solver/scripts/solver_ci_dispatch.py dispatch \
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
用户已在本次任务中明确授权触发时，去掉 `--dry-run` 并加 `--yes`；仅预览请求到此结束。随附脚本会：

1. 本地解析 `problem_ref`，校验配置、基准测试/自定义工作负载、目标、SMT、算法和正整数约束；
2. 检查 `gh auth` 与工作流注册状态；
3. 调用 `gh workflow run manual-solve.yml --repo ... --ref ... -f key=value ...`；
4. 查询最近的 `workflow_dispatch` 任务，并报告 URL、状态、结论、来源分支/SHA。

如果找不到新任务，先检查远端引用、触发时间、工作流和可见权限，报告 dispatch 与回查各自结果。
不要为找不到 run 而再次 dispatch，以免重复提交搜索；仍无法定位时说明缺口。

### 不使用随附脚本时的命令形式

```bash
gh workflow run manual-solve.yml \
  --repo OpenXiangShan/GEM5 --ref <branch> \
  -f note='<title>' \
  -f problem_ref='configs/solver_specs/foo.py:FooSearch' \
  -f configuration='kmhv3.py' \
  -f benchmark_type='spec06-rva23-novec-gcc16-0.3c' \
  -f max_parallel_trials='4' \
  -f max_parallel_workloads='4' \
  -f solver_kind='nsga2' \
  -f branch='<branch>'
```

不要把空的可选输入伪装成用户已确认的语义；可以省略它们，让工作流默认值生效，或在
命令和回复中明确写出空值。

## 结果交接

触发成功后给出：

- spec 文件和类名、最终生效的 CI 输入
- 本地验证结果及验证缺口
- `gh` 触发输出、任务 URL、当前 status/conclusion/head SHA
- 用户下一步查看产物的顺序：`summary.md` → `best_result.json` → `metadata.json` →
  `binding.json` → `parsed_problem.json` → `history.jsonl/history.csv` → `charts/`。

求解产物不使用性能 CI 的 `score.txt` 归档定位脚本；需要分析求解结果时，
直接用 `gh run view`/`gh run download` 获取上述产物。
