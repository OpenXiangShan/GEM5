# KMHv3 BTB-TAGE 容量 NSGA-II DSE 报告

生成时间：2026-08-17

## 结果摘要

- GitHub Actions [run 31789052477](https://github.com/OpenXiangShan/GEM5/actions/runs/31789052477)
  在 2026-08-15 16:25 UTC 成功结束，使用提交
  [`83e7471ce9`](https://github.com/OpenXiangShan/GEM5/commit/83e7471ce954294d12c38420ad7c2db5d39dc78c)。本报告基于该 run 上传的
  `solver-run-31789052477` artifact，而不是本地性能运行。
- 运行记录了 **672/672 个有效 trial**：1 个 config-default baseline 和
  671 个唯一候选；没有 invalid trial。控制器因 `timeout_hours reached` 在
  30 小时限制到达时停止，未达到 `max_trials=4000`。
- 以配置默认值为 baseline，分数从 `16.594126194` 提升到
  **`16.662289224`**（`+0.4108%`），同时加权
  `system.cpu.iew.branchMispredicts` 从 `181619.942658` 降至
  **`177099.013330`**（`-2.4892%`）。对应配置是
  `trial_0205`，也是 solver 按第一目标选出的代表解。
- 最终二目标帕累托前沿有 **3** 个点。独立以原始 `history.csv` 重算每个
  候选容量和二目标支配关系，得到同一集合：`trial_0205`、`trial_0671`、
  `trial_0351`。
- 图中的横轴是 TAGE 逻辑存储容量，作为 area proxy，不是综合得到的
  `mm^2`。面积没有作为本次 NSGA-II 目标；红色帕累托点严格表示实际优化的
  “分数最大化 + 分支错误预测最小化”二维非支配集。

## 三维结果图

![3D TAGE capacity, score, and branch-mispredict DSE](/docs/Gem5_Docs/images/tage-capacity-nsga2-dse-3d-20260815.png)

三维图的横轴为逻辑容量（KiB），纵轴为 `Estimated Int score per GHz`，
竖轴为加权 `system.cpu.iew.branchMispredicts`。蓝点为 baseline，灰点为
671 个候选，红点和连线为二目标帕累托前沿，红色星形为第一目标分数最高的
代表解 `trial_0205`。为便于精确比较分数和面积，保留二维投影：

![2D TAGE capacity and score DSE](/docs/Gem5_Docs/images/tage-capacity-nsga2-dse-2d-20260815.png)

## 目标

在保留默认 KMHv3 配置作为真实 baseline 的前提下，搜索 `BTBTAGE` 的容量
分配。每个候选独立选择 6、7 或 8 张 active TAGE 表，以及每张表的 sets、tag
宽度和相联度。搜索实际优化两个硬件指标：最大化选定 SPEC06 整数分数，同时
最小化 execute-stage 分支错误预测计数；不改编译器、二进制、OS 或运行时。

## 固定 CI 口径

| Item | Value |
| --- | --- |
| Branch / commit | `tage-capacity-nsga2-dse` / `83e7471ce9` |
| Solver spec | `TageCapacityNsga2ScoreBranchMispredictSearch` |
| Configuration | `configs/example/kmhv3.py` |
| Benchmark group | `gcc15-spec06-0.3c` |
| Filter | `sjeng,gobmk,astar` |
| Selected slices | 22: 14 gobmk, 4 sjeng, 4 astar |
| Candidate algorithm | NSGA-II |
| Objectives | maximize `Estimated Int score per GHz`; minimize `system.cpu.iew.branchMispredicts` |
| Stop condition | `max_trials=4000`, `timeout_hours=30` |
| Parallelism | 16 trials, 22 workloads per trial |
| Distribution | `distributed_servers=default`, `distributed_jobs_per_server=0` |
| 实际停止原因 | `timeout_hours reached` |
| 实际结果 | 672 valid，0 invalid，3 个二目标帕累托点 |

第一个 trial 是不带 solver overlay 的配置默认值 baseline，并计入
`max_trials`。`score` 和 `branchMispredicts` 的 `benchmark_aggregate` 都是
`mean`。该 artifact 对后者记录了 3 个 benchmark 级加权样本（`astar`、
`gobmk`、`sjeng`），所以图中的 z 轴是 solver 使用的聚合计数，不是把 22 个
slice 的原始计数直接相加。

## 容量 / 面积代理模型

`configs/example/kmhv3.py` 没有覆盖
`src/cpu/pred/BranchPredictor.py` 中的 `BTBTAGE` 默认容量：

```text
numPredictors = 8
tableSizes    = [2048] * 8
TTagBitSizes  = [13] * 8
numWays       = [2] * 8
```

active lookup 和 replacement datapath 使用一个 valid bit、配置的 tag 宽度、
3-bit direction counter 与一个 useful bit。存储的 `pc` 不参与 tag matching，
而 LRU helper 也不在 active replacement path 上调用。因此第 `i` 张表的建模
逻辑 SRAM 容量为：

```text
capacity_i = tableSizes_i * numWays_i * (TTagBitSizes_i + 5) bit
```

baseline 为 `8 * 2048 * 2 * (13 + 5) = 589824 bit`。每个非 baseline 候选
必须满足：

```text
294912 bit <= sum(capacity_i) <= 707788 bit
```

上界是 baseline 120% 的整数形式。每张候选表容量都是 64 bit 的倍数，因此这个
阈值下可表示的最大候选容量为 `707776 bit`。

## 候选编码和输入验证

spec 将候选表示为：

```text
[numPredictors, tableSizes..., TTagBitSizes..., numWays...]
```

三个向量都具有 `numPredictors` 个元素。domain 只会产生满足容量区间和以下范围的
配置：

- sets: `2^6` through `2^13`
- ways: 1 through 8
- tags: 8 through 20 bit
- active tables: 6, 7, or 8

精确的受约束设计空间包含 `40072411400567206589151` 个 assignment。使用自定义
sample/mutation/crossover domain，保证 NSGA-II 不会向 gem5 提交超预算或畸形的
向量。NSGA-II backend 扩展为调用可选的 domain-specific operator，使这些合法
结构在初始随机种群之后仍能保留遗传关系。

CI dispatch 前，以下检查均通过：

1. Python 编译和 spec 解析。
2. 2048 个 sample 以及 512 对 mutation/crossover 的容量检查。
3. 使用最终 16/22 并行度的 live KMHv3 binding 与 16-trial NSGA-II dry-run。
4. 同时检查 8-table 和 6-table 候选的 overlay config。生成的 `config.ini`
   精确携带推导出的 `numPredictors`、`tableSizes`、`TTagBitSizes` 与
   `numWays`。

短本地运行无法产生性能数据，因为 baseline 和 candidate 都在不相关的本地
`gem5.opt` PHAST 默认参数断言处停止。因此，本报告不把它作为性能证据。

## 结果审计

从 artifact 的 `history.csv` 读取 672 条记录后，重新解码每个候选的
`tageConfig`，按本文的 `tagBits + 5` 公式计算容量。671 个候选的容量范围是
`295808–707712 bit`，全都落在指定的 `294912–707788 bit` 区间，并且 671 个
编码均不重复。随后在完整 valid 集合上按“分数不低且错误预测不高，至少一项严格
更好”重算支配关系，得到 3 个点，与 artifact 的 `summary.md` 一致。

| 点 | Trial / 代数 | Active 表数 | 容量 (bit / KiB) | 相对 baseline 容量 | Score / delta | 分支错误预测 / delta |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Baseline | `trial_0001` / 0 | 8 | 589824 / 72.000 | - | 16.594126194 / - | 181619.942658 / - |
| P1, 分数最高，图中红星 | `trial_0205` / 12 | 7 | 702464 / 85.750 | +19.0972% | **16.662289224 / +0.4108%** | 177099.013330 / -2.4892% |
| P2, 折中点 | `trial_0671` / 41 | 7 | 669184 / 81.688 | +13.4549% | 16.649437699 / +0.3333% | 176575.013863 / -2.7777% |
| P3, 最低错误预测 | `trial_0351` / 21 | 6 | 681984 / 83.250 | +15.6250% | 16.586782995 / -0.0443% | **176389.173110 / -2.8801%** |

P1 是 `best_result.json` 和 solver summary 选择的代表解，因为它的第一目标
`Estimated Int score per GHz` 最高；这不表示多目标搜索存在唯一数学“最优
解”。P2 的容量比 P1 少 `4.062 KiB`，分数只少 `0.0771%`，错误预测再少
`0.296%`，因而是更稳妥的折中候选。

## 帕累托配置

向量按 `tageConfig = [numPredictors, tableSizes..., TTagBitSizes..., numWays...]`
编码；下面各列顺序都是 T0 到最后一张 active 表。所有位宽和容量均从 artifact
重新解码，未依赖图表中的坐标。

| 点 | Trial | `tableSizes` | `TTagBitSizes` | `numWays` |
| --- | --- | --- | --- | --- |
| P1 | `trial_0205` | `[256, 64, 2048, 2048, 2048, 1024, 256]` | `[16, 20, 19, 8, 18, 17, 15]` | `[2, 8, 7, 2, 3, 6, 1]` |
| P2 | `trial_0671` | `[2048, 2048, 2048, 2048, 512, 1024, 256]` | `[18, 11, 10, 8, 16, 20, 19]` | `[2, 4, 2, 8, 7, 2, 7]` |
| P3 | `trial_0351` | `[2048, 2048, 2048, 2048, 512, 1024]` | `[18, 11, 9, 9, 11, 20]` | `[2, 4, 7, 6, 4, 2]` |

## 选择建议

- 性能优先：选择 P1 / `trial_0205`。这是本次搜索中分数最高且仍优于 baseline
  错误预测的点，但逻辑容量比 baseline 高 19.1%。
- 分支错误预测优先：选择 P3 / `trial_0351`。它的错误预测最低，不过分数比
  baseline 低 0.0443%，容量也高 15.6%。
- 建议的后续实现起点：P2 / `trial_0671`。它同时高于 baseline 分数、低于
  baseline 错误预测，在这三个前沿点中容量最低，并接近 P1 的分数。P2 在最后
  一代才被发现，说明当前 timeout 结果不能被视为已穷尽或完全收敛。

## 边界与复现

- 实验只覆盖 GCC15 SPEC06 0.3c 中 `sjeng`、`gobmk`、`astar` 的 22 个
  slice；结果不能外推成完整 SPEC06 或其他 workload 的性能结论。
- “面积”仅是 active TAGE entry 的逻辑 bit 容量，未计入 SRAM 宏、译码、比较器、
  连线、功耗或时序。因此它支持相对硬件容量比较，不支持芯片 `mm^2` 结论。
- 672 次观测相对于 `40072411400567206589151` 个合法配置空间仍是极小采样；
  `timeout_hours reached` 也意味着没有达到 4000 次设计的 trial 上限。
- 图表由
  [`generate_tage_capacity_nsga2_dse_figures.py`](generate_tage_capacity_nsga2_dse_figures.py)
  从 artifact 重新计算。复现命令如下：

  ```bash
  python3 docs/Gem5_Docs/frontend/generate_tage_capacity_nsga2_dse_figures.py \
    /path/to/solver-run-31789052477
  ```

  脚本会验证容量约束和预期的三点二目标前沿，然后生成本文引用的 3D 和 2D PNG。
