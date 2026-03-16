# BTB-TAGE 索引与容量探索记录（2026-03-05）

## 1. 背景与目标

在 SPEC06（重点 gobmk / sjeng）中，观察到大量误预测集中在**同一基本块（32B/64B）内的多条分支**。当前 BTB-TAGE 索引长期以 `startPC` 为主，导致同块内多分支更容易落到同一组，形成明显冲突。

本次探索目标：

1. 确认 `position`/`branch` 信息进入 index 是否有效。
2. 确认在已有索引改进后，是否仍有容量收益空间。
3. 找出容量收益主要来自低表还是高表。
4. 评估“**不增加容量**”情况下的算法优化潜力。

## 2. 核心假设

1. 同块多分支冲突是当前 TAGE 误预测的重要来源之一。
2. 将分支粒度信息引入 index（`branchPC` 或 `position`）可减轻冲突。
3. 若低历史表（T0~T3）压力更大，则扩容收益应主要来自低表。

## 3. 实验设置

- 模式：本地 checkpoint，快速窗口
- 指令长度：`--maxinsts=5000000 --warmup-insts-no-switch=0`
- 统一开关：
  - `system.cpu[0].branchPred.mgsc.enabled=False`
  - `system.cpu[0].branchPred.tage.enableBankConflict=False`
- 重点切片：
  - `gobmk_nngs_18098`
  - `sjeng_84999`

## 4. 已验证改动与发现

### 4.1 branchPC-index（已实现并验证）

- 机制：索引使用 `branchPC` 替代 `startPC`。
- 结果：
  - gobmk：明显改善（mispredict 与 IPC 均提升）
  - sjeng：小幅改善

结论：方向正确，但不是最终上限。

### 4.2 扩容（ways*4 / capacity*4）

在 branchPC-index 基础上继续增大容量，收益仍明显，说明冲突问题并未被完全消除。

典型结论（5M 快速窗）：

- gobmk：`tableSizes` 4x 收益显著；`numWays` 4x 也有收益
- sjeng：`tableSizes` 4x 稳定，`numWays` 4x 收益不稳定（有时 IPC 不升反降）

### 4.3 容量收益来源定位（低表 vs 高表）

在 `useBranchPcForIndex=True`、`numWays=2` 下做分层扩容：

- base：`[2048 x 8]`
- low4x：`[8192,8192,8192,8192,2048,2048,2048,2048]`
- high4x：`[2048,2048,2048,2048,8192,8192,8192,8192]`
- all4x：`[8192 x 8]`

结果：

1. **low4x ≈ all4x**（收益接近）
2. **high4x 收益很小**
3. `updateAllocFailure` 在 low4x/all4x 大幅下降（约 80% 级别）

结论：容量瓶颈主要在**低历史表**，而非高历史表。

## 5. 新算法原型：position-mix index（本次新增）

### 5.1 设计动机

考虑 RTL 约束下难以在 S1 直接拿到 `branchPC`，先在模型中验证：

- 保持 `startPC` 作为 base
- 仅在低表索引中混入 `position` 哈希
- 可配置作用表数（`indexMixTables`）

### 5.2 新增参数

- `usePositionForIndexMix`（默认 `False`）
- `indexMixTables`（默认 `4`）

### 5.3 5M 快速结果（useBranchPcForIndex=False）

gobmk（off -> mix2/mix4/mix8）：

- IPC：`2.544804 -> 2.580220 / 2.583373 / 2.580409`
- mispred：`44249 -> 42602 / 42686 / 42887`

sjeng（off -> mix2/mix4/mix8）：

- IPC：`2.116851 -> 2.120000 / 2.129908 / 2.124130`
- mispred：`47856 -> 47515 / 47429 / 47691`

结论：

1. position-mix 确实有效。
2. `mix4` 整体最稳。
3. `mix8` 开始出现副作用（过度扰动）。

## 6. 关键认识（本轮结论）

1. `branchPC-index` 与 `position-mix` 方向一致，但并非完全相同机制。
2. 仅靠索引去冲突仍不能完全替代容量；低表容量与去别名要协同。
3. 如果追求“**不增容量**”的可行方案，`position-mix(low tables)` 是值得继续推进的候选。

## 7. 当前分支提交链（CI触发）

分支：`bigger-tage-align`

1. `dc5faa5c3c` configs: Disable MGSC in align baseline
2. `a068a5fb07` cpu,configs: Enable branch-PC index in BTB-TAGE
3. `f3327bde07` configs: Scale BTB-TAGE ways by 4x in align
4. `5bc3d42aa1` configs: Scale BTB-TAGE table capacity by 4x
5. `b44d5b169b` configs: Fix BTB-TAGE tableSizes assignment

> 注：第 5 条是配置修复，避免 `tableSizes` 赋值方式导致配置阶段异常。

## 8. 下一步建议

1. 在 CI 上优先验证「`position-mix` + 原始容量（2k, 2-way）」是否能稳定收益。
2. 把 low-table 去别名做成更 RTL 友好的版本（不依赖运行期 `position`）。
3. 为低表引入轻量冲突感知替换/分配策略，继续压 `updateAllocFailure`。
4. 最后再做长窗口（20M warmup + 20M run）确认快速窗结论是否稳定。

## 9. 风险与边界

1. 5M 快速窗口主要用于方向判断，最终需长窗口确认幅度。
2. 某些参数组合（例如非 2 幂 `tableSizes`）可能触发运行期异常，需规避。
3. sjeng 对索引扰动更敏感，参数应避免一刀切（`mix4` 比 `mix8` 更稳）。

## 10. 2026-03-10 补充：CI 归档与本地 5M 复验

### 10.1 先修正一个认识：branchPC-index 只是 upper bound

RTL 在 query index 时只有 `startPC`，`branchPC` 需要下一拍才能拿到，因此：

1. `branchPC-index` 适合作为“冲突是否主要来自同块多分支”的**上界验证**。
2. 但它**不应**直接作为 RTL 主方案继续推进。
3. 后续更值得讨论的是：在 `startPC-only` 约束下，怎样提升 2-way 的**有效容量**。

### 10.2 CI 归档补充分析：f3327 的收益确实主要来自 gobmk

比较：

- `run381 / a068a5fb07`
- `run382 / f3327bde07`

结论：

1. Int score 约 `+0.198`。
2. `gobmk` 单项约 `+0.899`，是头号贡献者。
3. gobmk 不是单个 slice 偶然变好，而是五个子 workload 都在改善。

CI 归档中的热点 branch 进一步支持“局部 branch-dense block 竞争”判断：

- `gobmk_nngs_18098`
  - `0x4f37c` / `0x4f388` -> `compute_connection_distances.lto_priv.0`
  - `0x3ec02` -> `popgo`
  - `0x3ac86` -> `remove_liberty`
  - `0x40d9e` -> `chainlinks2`
- `gobmk_trevord_4165`
  - `0x40d96` / `0x40d9e` -> `chainlinks2`
  - `0x4d040` / `0x4d04c` -> `order_moves.lto_priv.0`

这些热点集中在很少的 32B/64B block 内，例如：

1. `compute_connection_distances` 的 `0x4f37c`、`0x4f388` 位于同一紧凑循环。
2. `chainlinks2` 的 `0x40d96`、`0x40d9e` 是同一循环体里的“条件判断 + 回边”。
3. `order_moves` 的 `0x4d040`、`0x4d04c` 也是同一小循环。

中间结论：

- 这更像**结构竞争**，不是纯语义难预测。

### 10.3 本地 5M 复现：原始命令可以正常跑通

按如下命令可稳定跑通并落盘 `bp.db`：

```bash
./build/RISCV/gem5.opt -d debug/gobmk_nngs_18098 \
  ./configs/example/kmhv3.py \
  --generic-rv-cpt /nfs/home/share/jiaxiaoyu/simpoint_checkpoint_zstd_format/spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc/gobmk_nngs/18098/_18098_0.115927_.zstd \
  --enable-bp-db tage
```

为便于快速迭代，本地后续统一使用：

```bash
--maxinsts=5000000
```

### 10.4 本地 5M 对照：ways 的短窗效果是“先消压力，再慢慢转成精度”

在 `gobmk_nngs_18098` 上做局部对照：

| 配置 | IPC | branchMissPrediction | updateAllocFailure |
|---|---:|---:|---:|
| 当前默认（startPC + position-mix + 2way） | 2.583373 | 0.320990 | 6121 |
| `branchPC + 2way` | 2.582294 | 0.321232 | 6825 |
| `branchPC + 8way` | 2.592163 | 0.318747 | 0 |

解释：

1. `8way` 在前 5M 内，首先明显清掉了 allocation pressure（`updateAllocFailure -> 0`）。
2. 但短窗里 IPC / mispredict 收益还比较温和，没有 CI 全程那么大。
3. 这说明“ways 生效”的路径更像：
   - 先让热点 branch 能共存；
   - 再在更长窗口里逐渐转成更明显的精度收益。

### 10.5 一个重要发现：`numTablesToAlloc` 原先基本没有真正生效

本地尝试 `2way + branchPC + numTablesToAlloc=2` 时，发现：

1. 旧代码下结果与 `numTablesToAlloc=1` 几乎完全一致。
2. 说明该参数虽然存在，但主路径里基本没有真正发挥作用。

为验证这个方向，临时补了一个本地原型，使 `handleNewEntryAllocation()` 真正支持一次 update 连续分配多个表项。

补丁后的 5M 结果（`branchPC + 2way + numTablesToAlloc=2`）：

| 配置 | IPC | branchMissPrediction | updateAllocSuccess |
|---|---:|---:|---:|
| `branchPC + 2way` | 2.582294 | 0.321232 | 29494 |
| `branchPC + 2way + alloc2` | 2.650533 | 0.305852 | 53785 |

热点 branch 中，多条出现了实质下降：

1. `0x3ac86`: `1258 -> 1098`
2. `0x4f57a`: `553 -> 501`
3. `0x3ef4a`: `546 -> 483`
4. `0x3b67e`: `628 -> 583`
5. `0x4f37c`: `1180 -> 1127`

中间结论：

- 对这个 slice 来说，“**一次学得更多**”比“只堆 way”更像一条值得继续看的低容量方向。

### 10.6 alloc-fail victim 原型：方向有信号，但收益很敏感

为了逼近 RTL 可行性，又做了一个更保守的 update-path-only 原型：

1. 默认关闭。
2. 只在低表开启。
3. 只在同一 set 连续 alloc-fail 达阈值后，触发一次 victim replacement。
4. victim 选择偏向 `useful` 低、counter 弱的 way。

在**当前默认配置**（`startPC + position-mix + 2way`）上，做了两档参数：

| 配置 | IPC | branchMissPrediction | updateAllocVictim |
|---|---:|---:|---:|
| baseline | 2.583373 | 0.320990 | 0 |
| victim, threshold=8 | 2.586999 | 0.320779 | 90 |
| victim, threshold=4 | 2.572993 | 0.322612 | 555 |

解释：

1. `threshold=8` 只有很小的正收益，说明这条线**不是完全没用**。
2. `threshold=4` 明显变差，说明过于激进会破坏已学到的 entry。
3. 这条线目前更像“可选辅助手段”，不像主增益来源。

### 10.7 当前阶段性判断（更新版）

结合 CI 与本地 5M，当前更稳妥的中间结论是：

1. **同块/近块多分支竞争**仍然是 gobmk 提升的核心背景。
2. `branchPC-index` 证明了问题存在，但不应再作为 RTL 主方案。
3. 如果目标是“**不明显增容量**”，更值得看的方向是：
   - `startPC-only` 下的更快学习（如 limited multi-allocation）
   - 热 set 压力感知策略（但 victim 需很保守）
   - 低表去别名，而不是全表统一扰动
4. 单纯扩大 ways 在模型里有效，但 RTL 上收益不一定等价；这进一步说明问题不只是“名义容量”，而是“**有效容量/entry 流动性**”。

### 10.8 当前不建议直接下结论的点

1. 不建议根据 `branchPC-index` 直接推 RTL 收益幅度。
2. 不建议仅凭一个 aggressiveness 很高的 victim policy 判断“replacement 线无效”。
3. 5M 窗口适合看压力释放与学习速度，不足以完全替代长窗口 score 判断。

### 10.9 建议的下一步收敛方向

如果后续继续做，建议只保留两条主线，不再扩散：

1. `startPC-only + limited multi-allocation`
2. `startPC-only + 更克制的 pressure policy`

其中第 1 条目前信号更强，第 2 条应作为次优候选继续谨慎探索。

### 10.10 再补：20M warmup + 20M measured 复现实验（本地）

为回答“20M warmup 之后，2-way 是否其实已经学好了”这个问题，直接复现了更接近 CI 的 long-window 运行：

```bash
./build/RISCV/gem5.opt ./configs/example/kmhv3.py \
  --generic-rv-cpt <path> \
  --warmup-insts-no-switch=20000000 \
  --maxinsts=40000000
```

注意：

1. `stats.txt` 中会有两段 dump。
2. 第一段是 warmup 结束时的 dump+reset。
3. 第二段才是 measured window（后 20M）的结果。
4. 之前若直接 grep 或用前缀匹配，容易把 `updateAllocFailure` 和 `updateAllocFailureNoValidTable` 混淆，导致误读。

#### 10.10.1 `gobmk_nngs_18098`：2way 与 8way 的 long-window 对照

配置：

- `useBranchPcForIndex=True`
- `usePositionForIndexMix=False`
- `numWays=2 / 8`

measured window（后 20M）：

| 配置 | IPC | branchMissPrediction | predNoHitUseBim | updateNoHitUseBim | updateAllocFailure | predTableHits::mean |
|---|---:|---:|---:|---:|---:|---:|
| `2way` | 3.195621 | 0.241918 | 4443868 | 148953 | 73884 | 1.418889 |
| `8way` | 3.480643 | 0.192274 | 3691136 | 109685 | 1828 | 1.643924 |

关键结论：

1. 即使经过 20M warmup，`2way` 在 measured window 中依然有非常高的 `updateAllocFailure`。
2. `predNoHitUseBim` / `updateNoHitUseBim` 也显著更高，说明 measured window 里仍存在大量“没 provider / provider 太浅”的情况。
3. 因此不能简单认为“20M warmup 之后，2way 其实已经把高表都学好了”。

热点 branch 对照（measured window）：

| PC | `2way` mispred | `8way` mispred | `noPredMiss` 变化 |
|---|---:|---:|---:|
| `3ec02` | 9432 | 7871 | `1 -> 1` |
| `4f388` | 8879 | 8831 | `1 -> 1` |
| `4f37c` | 9696 | 9434 | `1 -> 1` |
| `3ac86` | 6587 | 5383 | `1 -> 1` |
| `40d9e` | 2807 | 2291 | `1 -> 1` |
| `4f57a` | 3766 | 3349 | `1 -> 1` |

解释：

1. 热点 branch 的收益几乎都是 `dirMiss` 下降。
2. `noPredMiss` 基本不变。
3. 所以 hottest branches 的主要收益，不像是“以前完全没有 entry，现在终于有了”，而更像“provider 更深、更稳、alias 更少”。

#### 10.10.2 `gobmk_trevord_7886`：2way 与 8way 的 long-window 对照

配置同上。

measured window（后 20M）：

| 配置 | IPC | branchMissPrediction | predNoHitUseBim | updateNoHitUseBim | updateAllocFailure | predTableHits::mean |
|---|---:|---:|---:|---:|---:|---:|
| `2way` | 1.916166 | 0.409479 | 8476178 | 293283 | 170141 | 0.950604 |
| `8way` | 2.079487 | 0.367182 | 5733766 | 95896 | 67502 | 1.166095 |

关键结论：

1. `trevord_7886` 上同样不是“warmup 后已经学完了”。
2. `2way` 在 measured window 里仍然表现出持续更高的 `no-hit-use-bim` 与 `alloc-fail`。
3. `8way` 平均命中表更深，说明收益来自更稳定的 provider，而不是单纯更大的 BTB 存活率。

热点 branch 对照（measured window）：

| PC | `2way` mispred | `8way` mispred | `noPredMiss` 变化 |
|---|---:|---:|---:|
| `1d9ac` | 13757 | 12308 | `54 -> 57` |
| `40d96` | 10638 | 10673 | `1 -> 1` |
| `3ec02` | 9803 | 9309 | `1 -> 1` |
| `40d9e` | 9518 | 9178 | `0 -> 0` |
| `4d040` | 8305 | 7803 | `0 -> 0` |
| `4d04c` | 5853 | 4223 | `0 -> 0` |

解释：

1. 和 `nngs` 一样，热点收益主要来自 `dirMiss` 下降。
2. `noPredMiss` 基本没有贡献。

#### 10.10.3 一个很关键的判断：2way -> 8way 的根因不是单一因素

结合 long-window 与 CI，可把根因拆成两层：

1. **全 slice 视角**：
   - `2way` 的确存在明显容量/分配压力。
   - measured window 里仍然有更多 `predNoHitUseBim`、`updateNoHitUseBim`、`updateAllocFailure`。
   - 这说明有一部分收益确实来自“更多 branch/history 能共存下来”。
2. **热点 branch 视角**：
   - 最大的可见收益几乎都是 `dirMiss` 下降，而不是 `noPredMiss` 下降。
   - 这说明 hottest branches 并不主要是“根本没有 entry”，而是“有 entry 但 provider 太浅、太不稳定、alias 太重”。

因此更准确的表述是：

- `8way` 的收益既有“共存能力提升”的成分，也有“provider 深度与稳定性提升”的成分。
- 对 gobmk 这两个 slice 来说，**两者都有，但对最热分支的直接可见收益，更偏向后者**。

#### 10.10.4 `numTablesToAlloc=2` 的长窗口验证：不能替代 8way

为验证“是不是只是爬升太慢”，又跑了：

- `useBranchPcForIndex=True`
- `usePositionForIndexMix=False`
- `numWays=2`
- `numTablesToAlloc=2`

在 `gobmk_nngs_18098` measured window 上结果为：

| 配置 | IPC | branchMissPrediction | predNoHitUseBim | updateNoHitUseBim | updateAllocFailure | updateAllocSuccess | predTableHits::mean |
|---|---:|---:|---:|---:|---:|---:|---:|
| `2way` | 3.195621 | 0.241918 | 4443868 | 148953 | 73884 | 85428 | 1.418889 |
| `2way + alloc2` | 3.215223 | 0.239555 | 4428262 | 138467 | 109911 | 155084 | 1.452967 |
| `8way` | 3.480643 | 0.192274 | 3691136 | 109685 | 1828 | 57793 | 1.643924 |

解释：

1. `alloc2` 在短窗（5M）里曾表现出较强正收益，但在 long-window 中只带来很小改善。
2. 它虽然略微提升了 `predTableHits::mean`，也让部分 branch 改善，但无法逼近 `8way`。
3. 更重要的是，它显著增加了 `updateAllocSuccess` 与 `updateAllocFailure`，说明“更积极地分配”在 2way 下也会带来更强 churn。

中间结论：

- `numTablesToAlloc=2` 说明“爬升速度”确实是问题的一部分。
- 但它**不能替代** ways 带来的“同时共存”能力。
- 因此 `8way` 的根因不能被简化成“只是学得慢”；更像是“**容量/共存 + 爬升/稳定性**”共同作用。

### 10.11 2way / 4way / 8way：SPEC06 CI 三点对照（2026-03-10）

新增一条 CI：

- `run402 / 68582d41be`
- archive: `/nfs/home/share/gem5_ci/performance_data/spec06-0.3c/20260310_111017_68582d41be_run402`

配置确认：

1. `useBranchPcForIndex=True`
2. `numWays=4`
3. `tableSizes=[2048 x 8]`

即：这是 `branchPC + 4way` 的整套 SPEC06 数据点。

#### 10.11.1 总分位置

| 配置 | Estimated Int score per GHz |
|---|---:|
| `2way` | 17.725340 |
| `4way` | 17.834559 |
| `8way` | 17.923055 |

定义 `2way -> 8way` 的总收益为 100%，则：

- `4way` 吃到了约 **55.24%** 的总收益

这说明：

1. `4way` 不是没用，而是明显有效。
2. 但 `4way` 仍然只走到了 `8way` 的一半左右，不是“已经基本够了”。

#### 10.11.2 benchmark 级位置

关键 benchmark：

| benchmark | `2way` | `4way` | `8way` | `4way` 吃到 `8way` 收益比例 |
|---|---:|---:|---:|---:|
| `gobmk` | 13.534 | 14.069 | 14.433 | 59.5% |
| `sjeng` | 11.671 | 11.912 | 11.996 | 74.0% |
| `astar` | 21.145 | 21.368 | 21.449 | 73.4% |

解释：

1. `sjeng` 上，`4way` 已经较接近 `8way`。
2. `gobmk` 上，`4way` 明显还没有吃满收益。
3. 这进一步说明 gobmk 对“更高共存能力”更敏感。

#### 10.11.3 `gobmk_nngs_18098`：4way 已接近 8way 的大部分可见收益

| 配置 | IPC | branchMissPrediction | predNoHitUseBim | updateAllocFailure | predTableHits::mean |
|---|---:|---:|---:|---:|---:|
| `2way` | 3.195290 | 0.243045 | 4497784 | 75773 | 1.423695 |
| `4way` | 3.422341 | 0.202649 | 3847954 | 40322 | 1.572258 |
| `8way` | 3.480643 | 0.192274 | 3691136 | 1828 | 1.643924 |

这里最有信息量的点是：

1. `4way` 已经拿到了约 **80%** 的 IPC / branchMissPrediction 改善。
2. 但只拿到了约 **48%** 的 alloc-fail 改善。

这说明：

- `4way` 已经足以把一部分关键热点 branch 稳住；
- 但更深层的共存压力还明显存在，因此 `updateAllocFailure` 还远高于 `8way`。

热点 branch 也支持这个判断：

| PC | `2way` | `4way` | `8way` |
|---|---:|---:|---:|
| `3ec02` | 9420 | 8137 | 7871 |
| `3ac86` | 6629 | 5680 | 5383 |
| `40d9e` | 2803 | 2314 | 2291 |
| `3ef4a` | 3257 | 2736 | 2646 |
| `3b67e` | 3555 | 3057 | 2932 |

但 `compute_connection_distances` 中的最顽固两条：

| PC | `2way` | `4way` | `8way` |
|---|---:|---:|---:|
| `4f388` | 8897 | 8867 | 8831 |
| `4f37c` | 9662 | 9409 | 9434 |

几乎没有明显改善，说明它们更可能带有较强语义难度，或仍然需要更强的共存/更少 alias 才能继续下降。

#### 10.11.4 `gobmk_trevord_7886`：4way 更像只解决了一半问题

| 配置 | IPC | branchMissPrediction | predNoHitUseBim | updateAllocFailure | predTableHits::mean |
|---|---:|---:|---:|---:|---:|
| `2way` | 1.905328 | 0.410177 | 8684844 | 171059 | 0.954579 |
| `4way` | 1.990286 | 0.389931 | 6686210 | 127246 | 1.018934 |
| `8way` | 2.079487 | 0.367182 | 5733766 | 67502 | 1.166095 |

这里 `4way` 只拿到大约：

1. **48.8%** 的 IPC 收益
2. **47.1%** 的 branchMissPrediction 改善

热点 branch 也体现出“只解决了一半”：

| PC | `2way` | `4way` | `8way` |
|---|---:|---:|---:|
| `1d9ac` | 13784 | 13191 | 12308 |
| `4d04c` | 5803 | 4850 | 4223 |
| `1d99c` | 6402 | 5784 | 5096 |
| `2a664` | 5863 | 5350 | 4967 |
| `3fa08` | 5413 | 4983 | 4556 |

而像 `40d96` / `40d9e` 这类 branch，`4way` 的改善就相对有限。

因此：

- `trevord_7886` 比 `nngs_18098` 更需要更高的共存能力；
- `4way` 只能部分缓解，`8way` 才开始把一整簇 dense block 内的竞争真正摊开。

#### 10.11.5 这一小节的阶段性理解

`4way` 的位置可以总结为：

1. 它足以证明问题确实和 ways / 共存能力高度相关。
2. 它对 `sjeng` 已经比较接近饱和。
3. 它对 `gobmk` 只是中间态，不足以说明“继续加 way 没意义”。
4. 但由于 `4way` 到 `8way` 仍有较大剩余空间，单纯依赖轻微改替换或轻微改分配，大概率还不能完全替代更高共存能力。
