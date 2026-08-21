# KMHv3 BTB-TAGE 8-table T2-T4 聚焦容量 NSGA-II DSE 报告

生成时间：2026-08-21

## 结果摘要

- GitHub Actions [run 32253821540](https://github.com/OpenXiangShan/GEM5/actions/runs/32253821540)
  于 2026-08-21 成功结束，使用提交
  [`c4069f1464`](https://github.com/OpenXiangShan/GEM5/commit/c4069f1464a9ac507d20794d93fab0a50db266f4)。
  本报告基于该 run 上传的 `solver-run-32253821540` artifact，而不是本地性能运行。
- 该 run 记录了 **830/830 个有效 trial**：1 个 config-default baseline 和
  829 个唯一候选；没有 invalid trial。控制器因 `timeout_hours reached` 在
  36 小时 solver 限制到达时停止，未达到 `max_trials=4000`。
- 以配置默认值为 baseline，score 从 `27.142763139` 提升到
  **`27.336189571`**（`+0.7126%`），加权
  `system.cpu.iew.branchMispredicts` 从 `128321.721398` 降至
  **`123998.871669`**（`-3.3688%`）。对应 `trial_0748`，也是 solver
  以第一目标 score 选出的代表解。
- 最终的 solver 二目标帕累托前沿有 **3** 个点：`trial_0748`、
  `trial_0271` 和 `trial_0505`。从原始 `history.csv` 独立重算 score 最大化
  和 branch-mispredict 最小化的支配关系，得到同一集合。
- 用户提供的图是容量最小化/score 最大化的**单独二维投影**：有 18 个红色点。
  容量只是此图的 area proxy，不是本次 NSGA-II 目标；红点不能被称为 solver 的
  score/mispredict 前沿。图像已核对为当前仓库
  `tage-capacity-8table-t2t4-dse-2d-20260821.png` 的同一文件。
- 新搜索固定为 8 张 active TAGE 表，使用 35 个受 TAGE 参数影响的 0.3c slice，
  比上一次的 22 个 `astar,gobmk,sjeng` slice 多出 13 个
  `bzip2,gcc,h264ref,perlbench` slice。T2--T4 被设为至少占逻辑容量 50%，
  并获得更高的采样和局部变异优先级；这是一项搜索先验，不是 T2--T4 因果有效的
  证明。

## 容量-分数图

![8-table T2-T4 TAGE capacity and score DSE](/docs/Gem5_Docs/images/tage-capacity-8table-t2t4-dse-2d-20260821.png)

横轴为 TAGE logical capacity（KiB），纵轴为 `Estimated Int score per GHz`。
灰点是 829 个候选，蓝点是 config-default baseline，红点和连线是
capacity-minimize/score-maximize 的 18 点投影前沿，红色星形是 score 最高的
`trial_0748`。该图用于容量和性能的可视化取舍；正式 solver 前沿仍由
score 和 `system.cpu.iew.branchMispredicts` 两个目标定义。

## 目标

在保留默认 KMHv3 配置作为真实 baseline 的前提下，搜索 `BTBTAGE` 的容量分配。
所有候选固定为 8 张 active TAGE 表，分别选择每张表的 sets、tag 宽度和相联度。
搜索优化两个硬件指标：最大化所选 GCC15 SPEC06 0.3c slice 的 score，同时最小化
execute-stage 分支错误预测计数；不改编译器、二进制、OS 或运行时。

相对 [前一轮报告](tage_capacity_nsga2_dse_20260814.md)，这轮变化如下：

1. active table 数从可变空间收缩为固定 8 张表。
2. workload 从 22 个 `astar,gobmk,sjeng` slice 扩展到 35 个 slice。
3. 总逻辑容量区间改为 baseline 的 80%--130%，并将 T2--T4 合计容量限制为至少
   50%。
4. T2--T4 的 sets 可从 `2^7` 到 `2^13`，其余表为 `2^6` 到 `2^11`；变异中 65%
   的动作直接改动 T2--T4 的 sets，另有 20% 改动 T2--T4 的 tag 或 ways。

第 4 点只是将先前观察到的 T2--T4 相关性编码为 proposal prior。由于候选空间已被
这个 prior 和总容量约束共同改变，不能从本轮 trial 的表间相关性推出单表容量的因果
收益。

## 固定 CI 口径

| Item | Value |
| --- | --- |
| Branch / commit | `tage-capacity-nsga2-dse` / `c4069f1464a9ac507d20794d93fab0a50db266f4` |
| Solver spec | `TageCapacity8TableT2T4Nsga2ScoreBranchMispredictSearch` |
| Configuration | `configs/example/kmhv3.py` |
| Benchmark group | `gcc15-spec06-tage-sensitive-0.3c-260604` |
| Selected slices | 35: 5 astar, 1 bzip2, 3 gcc, 14 gobmk, 5 h264ref, 4 perlbench, 3 sjeng |
| Candidate algorithm | NSGA-II via DEAP |
| Objectives | maximize `Estimated Int score per GHz`; minimize `system.cpu.iew.branchMispredicts` |
| Objective aggregation | `mean` at benchmark aggregation level for both objectives |
| Stop condition | `max_trials=4000`, `timeout_hours=36` |
| Parallelism | 10 trials, 35 workloads per trial |
| Distribution | `distributed_servers=default`, `distributed_jobs_per_server=0` |
| 实际停止原因 | `timeout_hours reached` |
| 实际结果 | 830 valid，0 invalid，3 个 score/mispredict 帕累托点 |

第一个 trial 是不带 solver overlay 的配置默认值 baseline，并计入 `max_trials`。
artifact 对 branch-mispredicts 记录了 7 个 benchmark 级加权样本，因此该计数是
solver 使用的 benchmark 聚合结果，不是 35 个 slice 原始计数的直接相加。

## 容量 / 面积代理模型

`configs/example/kmhv3.py` 不覆盖 `src/cpu/pred/BranchPredictor.py` 中
`BTBTAGE` 的默认容量：

```text
numPredictors = 8
tableSizes    = [2048] * 8
TTagBitSizes  = [13] * 8
numWays       = [2] * 8
```

`BTBTAGE::TageEntry` 的 active lookup 和 replacement path 使用一个 valid bit、
配置的 tag 宽度、3-bit direction counter 和一个 useful bit。lookup 只检查
`valid` 与 `tag`，replacement 也不调用 LRU helper；因此以 active TAGE entry
计的逻辑 SRAM 容量为：

```text
capacity_i = tableSizes_i * numWays_i * (TTagBitSizes_i + 5) bit
```

baseline 是 `8 * 2048 * 2 * (13 + 5) = 589824 bit = 72.000 KiB`。每个
non-baseline 候选必须满足：

```text
471860 bit <= sum(capacity_i) <= 766771 bit
capacity_T2 + capacity_T3 + capacity_T4 >= 50% * sum(capacity_i)
```

容量均为 64 bit 的倍数，所以实际 829 个候选覆盖 `472064--765952 bit`；其
T2--T4 share 覆盖 `50.04%--91.24%`。这只是 active entry 的逻辑 bit 容量，未
计入 SRAM 宏、译码、比较器、PC/LRU 辅助字段、线网、功耗或时序，不能用于
`mm^2` 结论。

## 候选编码和结果审计

solver 用一个长度 24 的 vector 编码候选：

```text
tageConfig = [tableSizes(T0-T7), TTagBitSizes(T0-T7), numWays(T0-T7)]
```

`numPredictors` 在 spec 中固定为 8。解码全部 829 个 non-baseline trial 后：

- 每个 vector 都是唯一的，长度均为 24；没有 invalid trial。
- 每个 candidate 的 sets、tags 和 ways 都在 domain 内，并满足上述总容量和
  T2--T4 share 约束。
- 完整 valid 集合按“score 不低且错误预测不高，至少一项严格更好”重算支配关系，
  与 artifact 的 3 点前沿一致。

| 点 | Trial / generation | 容量 (bit / KiB) | 相对 baseline 容量 | Score / delta | 分支错误预测 / delta |
| --- | --- | ---: | ---: | ---: | ---: |
| Baseline | `trial_0001` / 0 | 589824 / 72.000 | - | 27.142763139 / - | 128321.721398 / - |
| P1，score 最高 | `trial_0748` / 74 | 672768 / 82.125 | +14.0625% | **27.336189571 / +0.7126%** | 123998.871669 / -3.3688% |
| P2，中间取舍 | `trial_0271` / 27 | 722944 / 88.250 | +22.5694% | 27.322477472 / +0.6621% | 123880.997447 / -3.4606% |
| P3，错误预测最低 | `trial_0505` / 50 | 743168 / 90.719 | +25.9983% | 27.210633668 / +0.2501% | **123642.123309 / -3.6468%** |

P1 是 artifact `best_result.json` 和 solver summary 选择的代表解，因为它的
第一目标 score 最高；这不表示多目标搜索存在唯一数学“最优解”。P2 和 P3 以更多
逻辑容量分别换取更低的错误预测；三者均在该 35-slice 口径上同时优于默认 baseline。

## 求解结果到 gem5 参数的映射

运行时 `apply_trial()` 将 vector 分别写入：

```text
system.cpu[0].branchPred.tage.numPredictors
system.cpu[0].branchPred.tage.tableSizes
system.cpu[0].branchPred.tage.TTagBitSizes
system.cpu[0].branchPred.tage.numWays
```

`kmhv3.py` 先调用 `setKmhV3Params()`，随后 `Simulation.run_vanilla()` 在
`m5.instantiate()` 前执行 `root.apply_config(options.param)`；因此 CI 的 `-P`
overlay 可以只覆盖这四个参数，不会改变其他 KMHv3 配置。所有下面 vector 的顺序
均为 T0 到 T7，且 `numPredictors=8`。

除了三个正式帕累托点，下表加入两个容量-分数投影上的候选。它们不在 solver 的
score/mispredict 前沿，却在不增大容量的约束下仍优于 baseline，适合作为 1.0c
泛化验证的低成本对照点。

| 候选 / 用途 | `numPredictors` | `tableSizes` | `TTagBitSizes` | `numWays` | 容量 / T2--T4 share |
| --- | ---: | --- | --- | --- | --- |
| P1 `trial_0748`，score 极值 | 8 | `[128, 256, 2048, 4096, 2048, 1024, 512, 1024]` | `[15, 11, 8, 11, 18, 18, 15, 17]` | `[8, 3, 4, 3, 3, 5, 1, 3]` | 82.125 KiB / 66.06% |
| P2 `trial_0271`，中间前沿点 | 8 | `[128, 256, 2048, 2048, 2048, 1024, 1024, 512]` | `[15, 11, 14, 17, 13, 18, 13, 17]` | `[8, 3, 3, 2, 8, 5, 2, 3]` | 88.250 KiB / 69.41% |
| P3 `trial_0505`，错误预测极值 | 8 | `[1024, 256, 4096, 4096, 2048, 2048, 256, 512]` | `[15, 11, 8, 11, 17, 18, 14, 13]` | `[2, 1, 4, 3, 4, 2, 1, 1]` | 90.719 KiB / 79.37% |
| C1 `trial_0584`，低于 baseline 容量的最高 score | 8 | `[128, 1024, 4096, 2048, 512, 1024, 256, 1024]` | `[15, 10, 8, 20, 13, 19, 14, 17]` | `[8, 1, 3, 2, 8, 5, 1, 3]` | 69.219 KiB / 59.23% |
| C2 `trial_0808`，更低容量且 score/mispredict 均改善 | 8 | `[256, 1024, 2048, 2048, 2048, 1024, 512, 512]` | `[15, 13, 13, 17, 18, 15, 15, 13]` | `[5, 1, 4, 2, 3, 5, 1, 2]` | 67.625 KiB / 68.39% |

相对 solver baseline，C1 的 score 为 `+0.1432%`、错误预测为 `-1.0607%`，容量
为 `-3.8628%`；C2 的对应变化为 `+0.1111%`、`-1.6314%` 和 `-6.0764%`。它们的
选择原因是容量-性能取舍，不应误写成正式二目标帕累托结论。

用于命令行或 `manual-perf.yml` 的单个候选 overlay 形式如下，其中方括号中的值由
上表对应行替换：

```bash
--param system.cpu[0].branchPred.tage.numPredictors=8 \
--param system.cpu[0].branchPred.tage.tableSizes=[T0_sets,...,T7_sets] \
--param system.cpu[0].branchPred.tage.TTagBitSizes=[T0_tag,...,T7_tag] \
--param system.cpu[0].branchPred.tage.numWays=[T0_ways,...,T7_ways]
```

`manual-perf.yml` 会把 `extra_args` 先交给 `xargs`，再由 distributed runner
解析；因此实际 dispatch 也必须使用没有 assignment 内部空格的上述形式。每条
`-P` 的参数是一个 token，不能依赖 shell 引号在这个传递链中保留空格。

## 选择建议与 GCC15 SPEC06 1.0c 验证

建议用不超过 5 个候选的完整 `gcc15-spec06-1.0c` CI 检查泛化性，且按用户要求不
创建新的 baseline run：

1. `trial_0748`：35-slice score 极值，检验最直接的性能候选。
2. `trial_0271`：正式前沿的中间错误预测取舍。
3. `trial_0505`：正式前沿的最低错误预测极值。
4. `trial_0584`：容量比默认小 3.86% 而在 solver workload 上仍同时改善的候选。
5. `trial_0808`：容量比默认小 6.08% 的更激进容量节省候选。

五个 run 必须固定 `kmhv3.py`、`gcc15-spec06-1.0c`、空 benchmark filter（全套）、
`vector_type=base`、`distributed_servers=default`、32 jobs/server 和同一
`c4069f1464a9ac507d20794d93fab0a50db266f4` implementation SHA；workflow 定义从
`tage-capacity-nsga2-dse` ref 读取，而 `branch` input 传该完整 SHA。唯一变量是
上述四个 TAGE 参数。由于不运行新的 same-SHA baseline，完成后可以验证候选是否
正常运行并比较候选之间的全套结果，但不应把与旧 run 的差异误归因于本次 parameter
overlay。

## 边界与复现

- solver 只覆盖定制 GCC15 SPEC06 0.3c 的 35 个 slice；其结果不能直接外推为
  完整 GCC15 SPEC06 1.0c。后续 CI 的角色正是验证这项泛化。
- 容量是 active TAGE entry 的逻辑 bit proxy，不是综合面积、功耗或时序指标；
  它也不是 solver 的第三个目标。
- 830 次观测相对于受约束空间仍是有限采样，且 `timeout_hours reached` 表示没有
  到达 4000-trial 上限，不能将当前前沿当作已收敛的全局最优。
- 图像可由
  [`generate_tage_capacity_8table_t2t4_dse_figures.py`](generate_tage_capacity_8table_t2t4_dse_figures.py)
  从下载的 artifact 重建。脚本会校验 830 个 valid trial、唯一 baseline、18 个
  capacity-score 前沿点和 score 最佳的 `trial_0748`。若本机没有 `matplotlib`，可
  先使用 `--verify-only` 完成不渲染的 artifact 审计：

  ```bash
  python3 docs/Gem5_Docs/frontend/generate_tage_capacity_8table_t2t4_dse_figures.py \
    /path/to/solver-run-32253821540 \
    --output docs/Gem5_Docs/images/tage-capacity-8table-t2t4-dse-2d-20260821.png
  ```

  ```bash
  python3 docs/Gem5_Docs/frontend/generate_tage_capacity_8table_t2t4_dse_figures.py \
    /path/to/solver-run-32253821540 \
    --verify-only
  ```
