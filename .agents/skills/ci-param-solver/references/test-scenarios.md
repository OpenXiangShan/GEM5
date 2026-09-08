# 手工测试场景

技能可以被发现后，在新的 Codex 会话中使用以下提示词。除非提示词明确授权触发 CI，
否则所有测试都应离线完成。

## 目录

- 场景 1：信息不足
- 场景 2：信息互相矛盾
- 场景 3：完整的内建 BOP 请求
- 场景 4：完整的自定义工作负载 BOP 请求
- 场景 5：正确描述 CDP 预取器参数求解
- 离线自动检查

## 场景 1：信息不足

提示词：

```text
$ci-param-solver 帮我做一个 L2 BOP 参数求解并跑 CI。
```

预期行为：

- 不创建 spec，也不调用 `gh workflow run`。
- 只询问仍缺少的语义信息：配置、目标路径、搜索域、参数约束、工作负载模式、目标指标、
  停止预算、CI 并行度、远端引用，以及当前是否授权触发。
- 不自行编造检查点组、指标名称、参数范围或分支。

## 场景 2：信息互相矛盾

提示词：

```text
$ci-param-solver 使用
configs/solver_specs/l2_vbop_bop_large_nsga2_score_search.py:
L2VbopBopLargeNsga2ScoreSearch，但把 benchmark_type 改成 custom_bin，
custom_bin=/tmp/example.zstd，同时保留最大化 SPEC 分数，
specific_benchmarks=mcf，solver_kind=bayes，max_parallel_trials=0；现在触发 CI。
```

预期行为：

- 拒绝 `custom_bin + score_txt`。
- 拒绝 `custom_bin + specific_benchmarks`。
- 拒绝让这个七目标 spec 使用 `bayes`，并建议改用 `nsga2`。
- 拒绝将候选试验并行度设置为零。
- 在用户解决所有冲突前，不创建文件，也不触发任务。

## 场景 3：完整的内建 BOP 请求

提示词：

```text
$ci-param-solver 使用现有
configs/solver_specs/l2_vbop_bop_large_nsga2_score_search.py:
L2VbopBopLargeNsga2ScoreSearch。
configuration=kmhv3.py；benchmark_type=spec06-rva23-novec-gcc16-0.3c；
specific_benchmarks=mcf,omnetpp,xalancbmk；solver_kind=nsga2；
max_trials=4000；max_parallel_trials=16；max_parallel_workloads=10；
distributed_servers=default；distributed_jobs_per_server=0；
branch=solver-bop-demo。只校验并展示 CI 命令，不实际触发。
```

预期行为：

- 复用现有 spec，不创建重复文件。
- 报告八个可调参数和七个目标函数。
- 确认参数赋值空间包含 705024 个点，此外还有一个配置默认值基线。
- 确认 `score_max = round_max * score_ratio // 100`。
- 在本地环境允许的范围内执行解析和预检。
- 打印完整的 `gh workflow run manual-solve.yml ...` 命令，但不触发任务。

## 场景 4：完整的自定义工作负载 BOP 请求

提示词：

```text
$ci-param-solver 使用现有
configs/solver_specs/l2_vbop_bop_large_nsga2_bin_search.py:
L2VbopBopLargeNsga2BinSearch。
configuration=kmhv3.py；benchmark_type=custom_bin；
custom_bin=/tmp/example.zstd；solver_kind=nsga2；
max_parallel_trials=32；max_parallel_workloads=1；
branch=solver-bop-custom-demo。只校验，不触发 CI。
```

预期行为：

- 接受自定义工作负载模式下只包含统计项的目标函数。
- 要求显式提供非空的自定义工作负载路径。
- 保持 `specific_benchmarks` 为空。
- 报告八个可调参数和六个目标函数。

## 场景 5：正确描述 CDP 预取器参数求解

这个案例用于测试用户是否能一次性提供足够完整、且符合当前 gem5 配置结构的需求。
`kmhv3.py` 默认使用对齐 L2，CDP 对象的目标路径从
`system.l2_wrappers[0].prefetcher.cdp` 开始。下面的搜索范围是一个可运行的示例，实际
实验可以根据已有基线和敏感性分析调整。

提示词：

```text
$ci-param-solver 请生成一个 CDP 预取器参数求解 spec，只生成并预检，不触发 CI。

一、配置和运行模式
- configuration 使用 configs/example/kmhv3.py。
- 使用默认对齐 L2：不加 --classic-l2，不加 --no-pf，保持
  l2_wrapper_hwp_type=L2CompositeWithWorkerPrefetcher。
- CDP 必须保持启用，固定
  system.l2_wrappers[0].prefetcher.enable_cdp=True。

二、要搜索的参数
所有直接参数都位于 system.l2_wrappers[0].prefetcher.cdp 下：
- vpn_reset_period：Range(32, 512, step=32)，整数，表示 VPN 表重置周期。
- throttle_aggressiveness：Range(0.5, 4.0, step=0.25)，浮点数。
- filter_entry_granularity：Choice([64, 128, 256, 512, 1024, 2048, 4096, 8192])，
  单位是字节，只选择不小于 64 的 2 的幂。
- filter_entry_region_blks：Choice([16, 32, 64, 128])，必须保持偶数。
- filter_table_assoc：Choice([64, 128, 256])。
- filter_table_sets：Choice([1, 2, 4, 8])，这是一个抽象搜索变量，不是 gem5 直接参数。

三、表结构约束
- VPN 表结构固定为 vpn_sub_entries=4、vpn_assoc=4、vpn_entries=16，
  只搜索 vpn_reset_period。
- filter_table_entries 是 MemorySize 参数，不直接声明为 TunableParam.Unsigned；
  在 apply_trial() 中根据 filter_table_assoc * filter_table_sets 推导，保证总表项数
  能被组相联度整除，并写入
  system.l2_wrappers[0].prefetcher.cdp.filter_table_entries。
- filter_table_indexing_policy 和 filter_table_replacement_policy 固定使用配置默认值，
  不把策略对象作为本次搜索变量。
- filter_entry_granularity 和 filter_entry_region_blks 的约束必须在 spec 中保留，
  不能让求解器生成违反 CDP 构造函数断言的候选。

四、目标函数
- 最大化 system.cpu.ipc。
- 最大化 system.l2_wrappers.prefetcher.accuracy。
- 最大化 system.l2_wrappers.prefetcher.coverage。
- 将以下 CDP 计数器作为结果检查项，只有确认它们在 stats.txt 中的完整名称后，
  才决定是否加入 objectives：
  system.l2_wrappers.prefetcher.cdp.cdpStats.pfHitCDP、
  system.l2_wrappers.prefetcher.cdp.cdpStats.actualFilted、
  system.l2_wrappers.prefetcher.cdp.cdpStats.inserted。

五、工作负载和停止条件
- benchmark_type=spec06-rva23-novec-gcc16-0.3c。
- specific_benchmarks=mcf,omnetpp,xalancbmk。
- 使用 nsga2；最多 1000 个 trial，连续 20 个 trial 无提升时停止，最长 20 小时。

六、CI 预览参数
- max_parallel_trials=8，max_parallel_workloads=10。
- distributed_servers=default，distributed_jobs_per_server=0。
- branch=cdp-solver-demo。
- extra_args 留空；只做预检，不触发 CI。
```

预期行为：

- 识别这是 `kmhv3.py` 对齐 L2 下的 CDP，而不是 `system.l2_caches[*].prefetcher` 路径。
- 将 `filter_table_sets` 作为抽象变量，在 `apply_trial()` 中推导
  `filter_table_entries`，而不是把两个相关参数独立搜索。
- 注意 `filter_table_entries` 的真实类型是 `MemorySize`，绑定后应检查 `binding.json` 中的
  `resolved_kind`，不能强行声明为 `TunableParam.Unsigned`。
- 检查 `filter_entry_granularity >= 64` 且为 2 的幂、`filter_entry_region_blks` 为偶数，
  并拒绝违反这些约束的候选。
- 对 `accuracy`、`coverage` 和 CDP 计数器先检查实际 `stats.txt` 名称；不能只根据 C++
  成员名猜测统计路径。
- 生成 spec 后执行 `py_compile`、`parse_problem` 和可用范围内的 dry-run，并展示最终 CI
  参数；由于提示词明确“不触发”，不得调用 `gh workflow run`。

## 离线自动检查

```bash
python3 .agents/skills/ci-param-solver/scripts/self_test.py
```

如当前 agent 提供 Agent Skills validator，再用该 validator 检查
`.agents/skills/ci-param-solver`；不要依赖某个用户 home 下的 validator 路径。
