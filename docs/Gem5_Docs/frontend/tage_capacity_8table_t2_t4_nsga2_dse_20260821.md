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
- 新搜索固定为 8 张 active TAGE 表，使用 35 个受 TAGE 参数影响的 0.3c slice。
  相比上一轮的 22 个 slice，数量增加 13；`gobmk` 仍为 14 个，但 `astar/sjeng`
  的构成从 `4/4` 重选为 `5/3`，所以这不是旧 22 个 slice 的严格追加。在此基础上
  引入了 13 个 `bzip2,gcc,h264ref,perlbench` slice。T2--T4 被设为至少占逻辑容量 50%，
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
2. workload 由 22 个 `astar,gobmk,sjeng` slice 重选并扩展为 35 个 slice；
   `astar/sjeng` 构成也由 `4/4` 变为 `5/3`。
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

除了三个正式帕累托点，下表加入四个容量-分数投影上的候选。C1/C2 用于较小容量的
对照；C3/C4 则在 70--75 KiB 内按 solver score 排名第 1/2，分别测试该区间的性能
上界和最接近默认容量的选择。它们不在 solver 的 score/mispredict 前沿，不能被称为
正式帕累托点。

| 候选 / 用途 | `numPredictors` | `tableSizes` | `TTagBitSizes` | `numWays` | 容量 / T2--T4 share |
| --- | ---: | --- | --- | --- | --- |
| P1 `trial_0748`，score 极值 | 8 | `[128, 256, 2048, 4096, 2048, 1024, 512, 1024]` | `[15, 11, 8, 11, 18, 18, 15, 17]` | `[8, 3, 4, 3, 3, 5, 1, 3]` | 82.125 KiB / 66.06% |
| P2 `trial_0271`，中间前沿点 | 8 | `[128, 256, 2048, 2048, 2048, 1024, 1024, 512]` | `[15, 11, 14, 17, 13, 18, 13, 17]` | `[8, 3, 3, 2, 8, 5, 2, 3]` | 88.250 KiB / 69.41% |
| P3 `trial_0505`，错误预测极值 | 8 | `[1024, 256, 4096, 4096, 2048, 2048, 256, 512]` | `[15, 11, 8, 11, 17, 18, 14, 13]` | `[2, 1, 4, 3, 4, 2, 1, 1]` | 90.719 KiB / 79.37% |
| C1 `trial_0584`，低于 baseline 容量的最高 score | 8 | `[128, 1024, 4096, 2048, 512, 1024, 256, 1024]` | `[15, 10, 8, 20, 13, 19, 14, 17]` | `[8, 1, 3, 2, 8, 5, 1, 3]` | 69.219 KiB / 59.23% |
| C2 `trial_0808`，更低容量且 score/mispredict 均改善 | 8 | `[256, 1024, 2048, 2048, 2048, 1024, 512, 512]` | `[15, 13, 13, 17, 18, 15, 15, 13]` | `[5, 1, 4, 2, 3, 5, 1, 2]` | 67.625 KiB / 68.39% |
| C3 `trial_0335`，70--75 KiB score #1 | 8 | `[128, 1024, 2048, 2048, 1024, 1024, 512, 1024]` | `[15, 13, 19, 8, 13, 18, 15, 17]` | `[8, 1, 4, 1, 8, 5, 1, 3]` | 73.875 KiB / 61.25% |
| C4 `trial_0166`，最接近默认、同区间 score #2 | 8 | `[256, 256, 2048, 4096, 2048, 2048, 1024, 2048]` | `[15, 18, 16, 11, 18, 11, 13, 13]` | `[2, 3, 2, 3, 3, 2, 2, 1]` | 72.156 KiB / 71.72% |

相对 solver baseline，C1 的 score 为 `+0.1432%`、错误预测为 `-1.0607%`，容量
为 `-3.8628%`；C2 的对应变化为 `+0.1111%`、`-1.6314%` 和 `-6.0764%`。C3/C4 的
score 分别为 `+0.3027%` 和 `+0.2878%`，错误预测分别为 `-1.5309%` 和 `-1.4715%`；
它们的选择原因是容量-性能取舍，不应误写成正式二目标帕累托结论。

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

### 选点图和标签

下图沿用 solver artifact 的完整 829 个候选、默认 `trial_0001` 以及 18 点
capacity-minimize/score-maximize 投影。横轴和纵轴仍是 solver 的 GCC15 SPEC06
0.3c 口径，**不是** 1.0c 跑分；彩色 P/C 标记只说明哪些点被选择进入或准备进入
1.0c 回归，不构成新的 Pareto 定义。

![Selected TAGE capacity DSE regression points](/docs/Gem5_Docs/images/tage-capacity-8table-t2t4-selected-spec06-1c-20260821.png)

- `P1`--`P3`：正式 solver 目标（最大 score、最小
  `system.cpu.iew.branchMispredicts`）的三点 Pareto 集。
- `C1`--`C4`：容量/验证选择。它们可处于面积-分数投影上，但不等同于正式两目标
  Pareto 点。
- `C3` 和 `C4` 是新增的 70--75 KiB 近默认容量候选；图上标出的是 solver 证据，
  其 1.0c SPECint 结果在新的 CI 完成前未知。

| 标签 | Trial | 逻辑容量 | 相对 72 KiB 默认 | Solver score / delta | Solver branch-mispredict / delta | 选择理由与 1.0c 状态 |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| P1 | `trial_0748` | 82.125 KiB | +14.0625% | 27.336190 / +0.7126% | 123998.872 / -3.3688% | score 极值；已完成 |
| P2 | `trial_0271` | 88.250 KiB | +22.5694% | 27.322477 / +0.6621% | 123880.997 / -3.4606% | 正式前沿中间取舍；已完成 |
| P3 | `trial_0505` | 90.719 KiB | +25.9983% | 27.210634 / +0.2501% | 123642.123 / -3.6468% | 正式前沿的 branch 极值；已完成 |
| C1 | `trial_0584` | 69.219 KiB | -3.8628% | 27.181631 / +0.1432% | 126960.641 / -1.0607% | 小容量对照；已完成 |
| C2 | `trial_0808` | 67.625 KiB | -6.0764% | 27.172910 / +0.1111% | 126228.257 / -1.6314% | 更激进小容量对照；已完成 |
| C3 | `trial_0335` | 73.875 KiB | +2.6042% | 27.224924 / +0.3027% | 126357.292 / -1.5309% | 70--75 KiB 的 score #1；待跑 SPECint |
| C4 | `trial_0166` | 72.156 KiB | +0.2170% | 27.220893 / +0.2878% | 126433.430 / -1.4715% | 最接近默认、同区间 score #2；待跑 SPECint |

`C3` 与 `C4` 均被 P1/P2 在正式 solver 两目标上支配，因此不能升级为 P4/P5；
新增它们的目的，是区分接近默认容量时的性能泛化，而不是寻找第二条前沿。

### 已完成的 1.0c 结果

用户指定 [run 31990034028](https://github.com/OpenXiangShan/GEM5/actions/runs/31990034028)
作为 baseline。其归档为 `20260817_111028_c599c3370_kmhv3_run870`，使用
`kmhv3.py`、`base`、默认节点池、32 jobs/server、无 TAGE overlay，且只包含
`perlbench,bzip2,gcc,mcf,gobmk,hmmer,sjeng,libquantum,h264ref,omnetpp,astar,xalancbmk`
12 个 SPECint benchmark。该 archive 的硬门槛为 697 `stats.txt`、697
`completed`、零 `running`/`abort` 和非空 `score.txt`。

五个原始候选均以完整 `gcc15-spec06-1.0c`（空 filter）运行，实际模型 checkout
均由 `metadata.txt` 复核为 `c4069f1464a9ac507d20794d93fab0a50db266f4`。分析时只
从其完整归档提取 baseline 所覆盖的 12 个 benchmark。每个候选的 35 workload / 697
point 子集都与从
`spec06_gcc15_rv64gcb_base_260604/json/checkpoints_all.json` 导出的 whitelist
完全匹配；每个 workload 内先归一化 SimPoint 权重，再按 profile 的 instruction
count 聚合多个输入。

对五个已完成候选，各 archive 的 `astar_biglakes_8418/m5out/config.ini` 还逐一
核对了 `numPredictors`、`tableSizes`、`TTagBitSizes` 与 `numWays` 四个生效字段，
均与上表对应行一致；这比仅从 `metadata.txt` 读取请求的 `extra_args` 更强。

| 标签 | Trial | Actions run | Archive | 归档/Actions 状态 |
| --- | --- | --- | --- | --- |
| P1 | `trial_0748` | [32451374323](https://github.com/OpenXiangShan/GEM5/actions/runs/32451374323) | `20260821_134457_c4069f14_kmhv3_run937` | 1112/1112/0/0/score；Actions cleanup 后 NFS stale-handle failure |
| P2 | `trial_0271` | [32451374106](https://github.com/OpenXiangShan/GEM5/actions/runs/32451374106) | `20260821_134429_c4069f146_kmhv3_run936` | 1112/1112/0/0/score；Actions cleanup 后 NFS stale-handle failure |
| P3 | `trial_0505` | [32451374465](https://github.com/OpenXiangShan/GEM5/actions/runs/32451374465) | `20260821_134430_c4069f14_kmhv3_run940` | 1112/1112/0/0/score；Actions cleanup 后 NFS stale-handle failure |
| C1 | `trial_0584` | [32451374328](https://github.com/OpenXiangShan/GEM5/actions/runs/32451374328) | `20260821_134521_c4069f1464_kmhv3_run938` | 1112/1112/0/0/score；Actions cleanup 后 NFS stale-handle failure |
| C2 | `trial_0808` | [32451374438](https://github.com/OpenXiangShan/GEM5/actions/runs/32451374438) | `20260821_135048_c4069f1464_kmhv3_run939` | 1112/1112/0/0/score；Actions success |

表中的 `1112/1112/0/0/score` 依次为 `stats.txt`、`completed`、`running`、`abort`
和非空 `score.txt` 的验收结果。前四条 Actions 的 failure 发生在模拟和 score 生成后
的 NFS 清理；完整 archive 仍满足数据门槛，不能把它误写成性能失败。C2 的 Actions
所有 simulation、data processing、archive 和 upload 步骤均成功。Actions 页的
`headSha=d67bde...` 只是 workflow dispatch revision；实际模型版本以 archive metadata
的 `c4069f...` 为准。

下表仅报告 12 benchmark 的 SPECint 几何均值。`branchMispredicts` 从每个
`stats.txt` 的最后一个 ROI block 读取，先按 SimPoint/输入权重聚合；表中跨 benchmark
求和与 MPKI 因而是诊断量，不是 SPEC 官方 aggregate。

| 标签 | SPECint score / GHz | 相对 baseline | Final-ROI branchMispredicts | 诊断 branch MPKI | MPKI 相对 baseline |
| --- | ---: | ---: | ---: | ---: | ---: |
| Baseline `run870` | 18.689004 | +0.000000% | 1736380.363 | 7.234918 | +0.000000% |
| P1 `trial_0748` | **18.797498** | **+0.580524%** | 1671706.072 | 6.965442 | -3.724661% |
| P2 `trial_0271` | 18.758323 | +0.370909% | 1703895.553 | 7.099565 | -1.870833% |
| P3 `trial_0505` | 18.777698 | +0.474580% | **1670709.998** | **6.961292** | **-3.782027%** |
| C1 `trial_0584` | 18.687556 | -0.007749% | 1728281.519 | 7.201173 | -0.466421% |
| C2 `trial_0808` | 18.712569 | +0.126090% | 1720013.144 | 7.166721 | -0.942606% |

| Benchmark score delta vs baseline | P1 | P2 | P3 | C1 | C2 |
| --- | ---: | ---: | ---: | ---: | ---: |
| perlbench | -0.563637% | -0.582761% | -1.757445% | -0.579445% | -0.988988% |
| bzip2 | +0.227919% | +0.290104% | +0.575293% | -0.716433% | +0.186383% |
| gcc | -0.044013% | -0.173122% | -0.647416% | -0.704764% | -0.370017% |
| mcf | +0.180304% | **+1.671596%** | +0.219687% | +0.463387% | +0.956537% |
| gobmk | +2.399588% | +1.201293% | **+3.862373%** | +0.523240% | +0.686673% |
| hmmer | +0.005806% | -0.007894% | -0.010321% | -0.006611% | +0.002709% |
| sjeng | +1.092785% | +0.747830% | **+1.363768%** | +0.422215% | +0.497727% |
| libquantum | -0.056072% | -0.045629% | -0.126056% | -0.048094% | -0.007526% |
| h264ref | +0.042751% | +0.025601% | -0.077234% | -0.071419% | -0.016000% |
| omnetpp | +0.697463% | +0.208798% | -0.153970% | +0.489420% | +0.276692% |
| astar | **+2.977682%** | +1.090307% | +2.757067% | +0.224656% | +0.401254% |
| xalancbmk | +0.067800% | +0.048730% | -0.184973% | -0.077548% | -0.098172% |

结论应按这套 SPECint 数据而不是 solver 0.3c score 作出：

1. P1 是当前首选性能点：它给出最高的 SPECint `+0.580524%`，同时 branch MPKI
   降 `3.724661%`。P1 在 `astar`、`gobmk` 和 `omnetpp` 的收益抵消了较温和的
   `perlbench` 回退。
2. P3 的 branch MPKI 比 P1 再低 `0.004150`，但总 score 低于 P1，且对
   `perlbench`（`-1.757445%`）和 `gcc`（`-0.647416%`）的回退更明显；它仅适合
   branch-mispredict 优先的取舍。P2 在 `mcf` 上的收益最大，但总体仍落后 P1。
3. C2 以 `-6.0764%` 容量换取 `+0.126090%` SPECint score，适合作为面积敏感备选；
   C1 虽少 `3.8628%` 容量，但 score 为 `-0.007749%`，不应作为性能推荐点。
4. C3/C4 尚未有 1.0c 数据。C3 测试该容量带内的 score 极值，C4 将默认容量只增加
   `0.2170%`；二者的价值是补齐近默认容量区间，不预设其会重复 solver 的收益。

本段 baseline 的模型 SHA 是 `c599c33709229410ba37088aa766a1a8f2e7f5ac`，而五个
候选为 `c4069f1464a9ac507d20794d93fab0a50db266f4`，因此上述百分比是跨 SHA 对比，
不能宣称严格的 TAGE-only A/B。已检查这两个 SHA 的 `src/`、
`configs/example/kmhv3.py` 与 `.github/workflows/manual-perf.yml` 无差异；差异在
solver/workflow、solver checkpoint 选择和文档，仍需在解释中保留这一边界。

### C3/C4 的 SPECint CI 合同

新 CI 保持 `kmhv3.py`、`gcc15-spec06-1.0c`、`base`、默认节点池、32 jobs/server
和模型 SHA `c4069f1464a9ac507d20794d93fab0a50db266f4`；只把
`specific_benchmarks` 固定为 baseline 的 12 个 SPECint benchmark，并替换四个 TAGE
overlay。`--ref` 选择已推送的 workflow 定义，`branch` input 固定实际模型 checkout。
两个 dispatch 需在明确确认后执行：

```bash
gh workflow run manual-perf.yml \
  --repo OpenXiangShan/GEM5 \
  --ref tage-capacity-nsga2-dse \
  -f note=TAGE-C3-specint-1.0c \
  -f configuration=kmhv3.py \
  -f benchmark_type=gcc15-spec06-1.0c \
  -f specific_benchmarks=perlbench,bzip2,gcc,mcf,gobmk,hmmer,sjeng,libquantum,h264ref,omnetpp,astar,xalancbmk \
  -f vector_type=base \
  -f branch=c4069f1464a9ac507d20794d93fab0a50db266f4 \
  -f distributed_servers=default \
  -f distributed_jobs_per_server=32 \
  -f extra_args='-P system.cpu[0].branchPred.tage.numPredictors=8 -P system.cpu[0].branchPred.tage.tableSizes=[128,1024,2048,2048,1024,1024,512,1024] -P system.cpu[0].branchPred.tage.TTagBitSizes=[15,13,19,8,13,18,15,17] -P system.cpu[0].branchPred.tage.numWays=[8,1,4,1,8,5,1,3]'
```

```bash
gh workflow run manual-perf.yml \
  --repo OpenXiangShan/GEM5 \
  --ref tage-capacity-nsga2-dse \
  -f note=TAGE-C4-specint-1.0c \
  -f configuration=kmhv3.py \
  -f benchmark_type=gcc15-spec06-1.0c \
  -f specific_benchmarks=perlbench,bzip2,gcc,mcf,gobmk,hmmer,sjeng,libquantum,h264ref,omnetpp,astar,xalancbmk \
  -f vector_type=base \
  -f branch=c4069f1464a9ac507d20794d93fab0a50db266f4 \
  -f distributed_servers=default \
  -f distributed_jobs_per_server=32 \
  -f extra_args='-P system.cpu[0].branchPred.tage.numPredictors=8 -P system.cpu[0].branchPred.tage.tableSizes=[256,256,2048,4096,2048,2048,1024,2048] -P system.cpu[0].branchPred.tage.TTagBitSizes=[15,18,16,11,18,11,13,13] -P system.cpu[0].branchPred.tage.numWays=[2,3,2,3,3,2,2,1]'
```

两条 run 完成后，仍按 baseline 对应的 35 workload / 697 point whitelist 重聚合，并
要求运行 archive 对 12 个实际 benchmark 有 697 `stats.txt` 和 `completed`、零
`running`/`abort`、非空 `score.txt`。在加入实测表前，还必须从每条 run 的一个完成
切片 `m5out/config.ini` 核对四个 TAGE 字段与“求解结果到 gem5 参数的映射”中 C3/C4
对应行完全一致；仅有 `metadata.txt` 的请求参数不足以证明最终 SimObject 配置。

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
- 本节的 P1--P3/C1--C4 覆盖图由
  [`generate_tage_capacity_8table_t2t4_selection_figure.py`](generate_tage_capacity_8table_t2t4_selection_figure.py)
  生成。它复用上述 artifact 审计，并额外验证 P1--P3 恰为正式 score/branch
  Pareto 集、C1--C4 位于容量-分数投影、C3/C4 是 70--75 KiB 中的 score #1/#2；无
  `matplotlib` 时可运行：

  ```bash
  python3 docs/Gem5_Docs/frontend/generate_tage_capacity_8table_t2t4_selection_figure.py \
    /path/to/solver-run-32253821540 \
    --verify-only
  ```
