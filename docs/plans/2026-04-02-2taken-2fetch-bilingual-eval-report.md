# 2-Taken / 2-Fetch Evaluation Report (Bilingual)

## 中文报告

### 1. 分析范围与数据来源

- 最终版本: `out/gem5/parallel-2026-03-25-idealize-fetch-window-fill/spec_all`
  - commit: `1cae93a32bb04be2376aa5d77eb24e755751c157`
- 中间版本: `out/gem5/parallel-2026-03-23-deepdive-on-2fetch-fetch/spec_all`
  - commit: `eac43be470b912d3c3e63dbde80a846cd3377113`
- baseline: `out/gem5/parallel-2026-03-25-idealize-fetch-window-fill/ref/spec_all`
  - commit: `fc642271651f9460719b12a5fa7573cfa696587f`

本报告同时检查了 GEM5 代码实现、配置改动和 `perf-weighted*.csv` / `perf-score*.csv` 中的加权 SPEC 数据。重点是理解：

1. `2-Taken` 如何给 `2-Fetch` 提供下一条 fetch stream。
2. `2-Fetch` 在 fetch stage 中何时成功、何时失败。
3. 理想 fetch buffer / ideal fetch window fill 对结果的影响。
4. 哪些 SPEC 子项收益明显，哪些没有收益，以及原因是什么。

### 2. 代码实现结论

#### 2.1 2-Taken 的角色

- 在 `GEM5/src/cpu/pred/btb/decoupled_bpred.cc` 中，`DecoupledBPUWithBTB::tick()` 在 `enable2Taken` 打开时会在一个 tick 内尝试做两次 prediction。
- `GEM5/src/cpu/pred/btb/decoupled_bpred.hh` 暴露了 `ftqHasNext()` / `ftqNext()` / `is2FetchEnabled()` / `getMaxFetchBytesPerCycle()` 这些 fetch 侧接口。
- 从结构上看，`2-Taken` 的主要作用不是直接提升 fetch 带宽，而是更早把“下一段 fetch target”放进 FTQ，给 `2-Fetch` 提供同周期拼接的机会。

结论: 这套设计里 `2-Taken` 更像前提条件，`2-Fetch` 才是把 predictor 端 lookahead 转换成 fetch 吞吐提升的关键执行点。

#### 2.2 2-Fetch 的真实执行点

- 核心逻辑在 `GEM5/src/cpu/o3/fetch.cc` 的 `Fetch::lookupAndUpdateNextPC(...)`。
- 当当前 stream `run_out` 且当前分支 `predict_taken` 时，fetch 会检查：
  - feature 是否开启；
  - FTQ 中是否已经有 next stream；
  - 分支 target 是否与 next stream start 匹配；
  - 合并后 span 是否超过 `maxFetchBytesPerCycle`；
  - target 是否仍落在当前 fetch buffer 覆盖范围内。
- 只有这些条件同时满足时，`do_2fetch = true`，fetch 才不会在这个分支点停下来，而是继续在同一个周期把下一段 stream 也吃进去。

这说明 `2-Fetch` 的本质不是简单“多取一条 cache line”，而是“在同一 fetch 周期跨越一个已预测 taken branch，继续消费下一条已知 fetch target”。

#### 2.3 final 版本相对中间版本的关键改动

`1cae93a` 的关键点不是重新发明 `2-Fetch`，而是把 fetch window 的约束大幅理想化：

- `GEM5/configs/example/kmhv3.py`
- `GEM5/configs/example/idealkmhv3.py`
- `GEM5/src/cpu/o3/BaseO3CPU.py`
- `GEM5/src/cpu/o3/fetch.cc`

具体变化：

- `fetchBufferSize` 从默认的 `66B` 提升到 `258B`。
- 新增 `idealFetchWindowFill = True`，走 `Fetch::idealFillFetchBuffer(...)`，用 functional read 一次填满整个 fetch window。
- `maxFetchBytesPerCycle` 提升到 `256B`。

这意味着 final vs intermediate 不是“只隔离 ideal fetch buffer”这么简单，它更准确地说是“理想化/放宽 fetch window 约束”的实验：

- 更大的可覆盖窗口；
- 更理想的窗口填充方式；
- 更大的同周期可接受 span。

因此 final vs mid 的结果应该解读为“idealized fetch-window experiment”的效果，而不是纯粹只由一个 buffer-fill 机制单独带来的效果。

### 3. 新增计数器的价值

中间版本 `eac43be47` 已经加入了较完整的 `2-Fetch` 计数器，final 在其基础上又补充了更细的 failure 分类和几何分布信息。主要新增计数器位于：

- `GEM5/src/cpu/o3/fetch.hh`
- `GEM5/src/cpu/o3/fetch.cc`

最有用的计数器分三类：

#### 3.1 机会与成功率

- `twoFetchOpportunity`
- `twoFetchTaken`
- `singleFetchCycleCount`
- `doubleFetchCycleCount`

这组计数器回答三个问题：

1. 到底有多少次 run-out 场景可以评估 `2-Fetch`；
2. 成功率是多少；
3. 真正落到“同周期双 stream fetch”的 cycle 比例是多少。

#### 3.2 失败原因

- `twoFetchFailOnlyNotPredTaken`
- `twoFetchFailOnlyNoNext`
- `twoFetchFailOnlySpanTooLarge`
- `twoFetchFailOnlyTargetNotInBuffer`
- `twoFetchFailBothSpanAndTargetNotInBuffer`

这组计数器非常关键，因为它们把“失败”从一个模糊现象拆成了 predictor 约束、FTQ 可见性约束、几何 span 约束、buffer coverage 约束。

#### 3.3 几何分布

- `twoFetchSpanBytes`
- `twoFetchTargetOffsetFromBufferStart`
- `twoFetchTargetDistancePastBufferEnd`
- `twoFetchNextStartDelta`
- `twoFetchRunOutBranchOffset`

这组计数器让 `2-Fetch` 不再只是“成功/失败”的黑盒，而是可以判断瓶颈到底来自：

- next stream 太远；
- branch target 太靠后；
- 当前 stream 结束点与下一段之间存在较大跨度；
- 或者分支本身就出现在不适合拼接的位置。

### 4. 总体性能结果

#### 4.1 总分

- baseline overall/int avg: `20.6959`
- intermediate overall/int avg: `21.4439` (`+3.61%` vs baseline)
- final overall/int avg: `21.8804` (`+5.72%` vs baseline, `+2.04%` vs intermediate)

这说明：

- `2-Taken + 2-Fetch` 主体机制本身已经带来明显收益；
- final 的 idealized fetch-window 改动又在此基础上再抬高了一档；
- 但增益并不均匀，明显集中在一部分前端更敏感、控制流更碎片化的 benchmark 上。

#### 4.2 SPEC 子项增益表

| Benchmark | Ref Score | Mid Score | Final Score | Final vs Ref | Final vs Mid |
| --- | ---: | ---: | ---: | ---: | ---: |
| perlbench | 16.94 | 18.31 | 19.97 | +17.90% | +9.05% |
| xalancbmk | 33.96 | 36.91 | 38.17 | +12.40% | +3.41% |
| sjeng | 13.17 | 14.47 | 14.51 | +10.20% | +0.30% |
| gcc | 21.17 | 22.61 | 23.24 | +9.77% | +2.80% |
| gobmk | 15.61 | 16.35 | 16.59 | +6.24% | +1.47% |
| h264ref | 25.23 | 25.43 | 26.29 | +4.18% | +3.36% |
| astar | 14.33 | 14.75 | 14.89 | +3.91% | +0.99% |
| bzip2 | 11.26 | 11.41 | 11.55 | +2.52% | +1.20% |
| libquantum | 46.86 | 46.79 | 47.58 | +1.53% | +1.69% |
| mcf | 34.65 | 34.93 | 34.93 | +0.80% | -0.01% |
| omnetpp | 21.85 | 21.91 | 22.00 | +0.72% | +0.43% |
| hmmer | 17.07 | 17.07 | 17.08 | +0.09% | +0.07% |

### 5. 哪些 benchmark 收益最明显，以及证据是什么

下面把“高收益 benchmark”与关键计数器放在一起看。

| Benchmark | Score Gain vs Ref | `fetch_nisn_mean` Gain | Final `doubleFetchCycle` 占比 | Final `2-Fetch` 成功率 | `frontendBound` 降幅 | `fetchBubbles` 降幅 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| perlbench | +17.90% | +33.83% | 29.96% | 33.30% | -89.77% | -89.98% |
| xalancbmk | +12.40% | +18.55% | 46.47% | 55.25% | -83.29% | -84.70% |
| sjeng | +10.20% | +33.15% | 21.75% | 30.48% | -59.45% | -63.19% |
| gcc | +9.77% | +22.07% | 42.76% | 49.00% | -73.46% | -78.06% |
| gobmk | +6.24% | +32.25% | 24.48% | 32.15% | -70.72% | -71.42% |
| h264ref | +4.18% | +5.74% | 11.60% | 17.35% | -89.74% | -90.43% |

#### 5.1 perlbench / gcc / xalancbmk: 最像“教科书式受益者”

这几项的共同特征是：

- `fetch_nisn_mean` 明显上升；
- `doubleFetchCycleCount` 占比不低；
- `fetchBubbles` 和 `frontendBound` 大幅下降；
- score 提升幅度也在前列。

说明它们的关键瓶颈确实在前端供给，而不是后端执行。对这类 workload，`2-Taken` 提供 next stream、`2-Fetch` 把 next stream 直接拼进当前周期，能非常直接地转化成吞吐收益。

#### 5.2 sjeng / gobmk: 也受益，但几何约束仍重

这两项有不小收益，但 final 中：

- `twoFetchNotTakenTargetNotInBuffer` 仍高；
- `twoFetchNotTakenSpanTooLarge` 仍高；
- `2-Fetch` 成功率只有约 30% 左右。

这说明它们虽然存在大量可利用的分支密集场景，但 stream 几何关系仍然很差，很多机会会卡在“跨度太大”或“target 超出当前窗口覆盖范围”。

#### 5.3 h264ref: 前端问题被明显缓解，但不是最强 `2-Fetch` 受益者

`h264ref` 的 `frontendBound` / `fetchBubbles` 降幅非常大，但 `doubleFetchCycle` 占比和 `2-Fetch` 成功率并不高。这通常意味着：

- final 的理想化 fetch window 确实修复了前端供给中的一部分窗口填充问题；
- 但 `h264ref` 的增益不完全来自频繁成功的 `2-Fetch`，而是来自 fetch 供给链整体被平滑后带来的收益。

### 6. 2-Fetch 的失败原因与瓶颈

#### 6.1 final 版本总体统计

把 12 个 INT benchmark 聚合后：

- `twoFetchOpportunity`: 约 `61.09M`
- `twoFetchTaken`: 约 `22.98M`
- 总成功率: `37.61%`
- `doubleFetchCycleCount / (single + double)`: `28.70%`

失败的机会进一步按 final 新增的 fail-only 计数器拆开后：

- 仅因 `not predicted taken`: `29.69%`
- 仅因 `no next stream`: `13.83%`
- 仅因 `span too large`: `7.01%`
- 仅因 `target not in buffer`: `10.51%`
- 同时因 `span too large` 与 `target not in buffer`: `38.96%`

这里最关键的结论是：

1. 最大的瓶颈不是 FTQ 没有 next stream，而是 fetch geometry；
2. `span` 和 `buffer coverage` 经常不是单独出现，而是一起出现；
3. 也就是说，很多场景不是“只差一点点 buffer”，而是“整个下一段 stream 对当前周期来说就是太远了”。

#### 6.2 为什么说 `2-Fetch` 的主要瓶颈是几何约束

final 聚合数据中：

- `twoFetchNotTakenSpanTooLarge / opportunity = 28.68%`
- `twoFetchNotTakenTargetNotInBuffer / opportunity = 30.86%`

并且这两类失败高度重叠。说明一旦允许 fetch 跨越一个 taken branch，真正限制继续向前吃下一段 stream 的，往往是：

- 下一段结束点太远；
- 或下一段 target 已超出当前 fetch window。

这也解释了为什么 final 虽然显著进步，但仍然只有 `37.6%` 的机会真正成功变成 `2-Fetch`。

#### 6.3 各 benchmark 的失败模式差异

- `astar`: `no_next_rate` 达到 `18.62%`，同时 `span/oob` 也高，说明既有 predictor/FTQ 可见性问题，也有 stream geometry 问题。
- `sjeng`: `oob_rate` 与 `span_rate` 都接近 `38%`，几何约束很重，所以 final 相对 mid 已接近饱和。
- `xalancbmk`: 最终成功率高达 `55.25%`，几何条件比大多数 benchmark 更适合 `2-Fetch`。
- `hmmer`: `doubleFetchCycle` 仅 `11.8%`，而且总体前端本来就不重，所以几乎没有性能空间。
- `mcf`: 即使前端计数器改善不少，最终性能仍几乎不变，说明后端/内存系统仍是主导瓶颈。

### 7. intermediate vs final: ideal fetch buffer / idealized fetch window 的效果

#### 7.1 总结论

- final 相比 intermediate: overall `+2.04%`
- 最大受益者: `perlbench (+9.05%)`
- 其次: `xalancbmk (+3.41%)`, `h264ref (+3.36%)`, `gcc (+2.80%)`, `libquantum (+1.69%)`, `gobmk (+1.47%)`
- 基本不动: `sjeng`, `omnetpp`, `hmmer`, `mcf`

这说明 ideal fetch-window 实验的收益主要集中在“前端仍有残余 fetch 供给瓶颈”的 workload 上；而那些已经接近别的瓶颈的 benchmark，即便再放宽 fetch 窗口也不会继续明显上涨。

#### 7.2 一个容易误读但很关键的现象

从 intermediate 到 final，很多 benchmark 上：

- `span_rate` 明显下降；
- `doubleFetchCycle` 占比明显上升；
- `fetchBubbles` 明显下降；
- 但 `targetNotInBuffer` 的比例反而可能上升。

这并不表示 ideal fetch buffer 没起作用。更合理的解释是：

1. `maxFetchBytesPerCycle` 被放宽到 `256B` 以后，很多原本先被判成 `span too large` 的机会，被重新分类为“仍然不在 buffer 内”或者直接成功；
2. 同时更多机会真正走到了更深入的几何检查阶段；
3. 所以 `targetNotInBuffer` 的计数不是单独看绝对高低，而要结合 `span`、`doubleFetchCycleCount`、`fetchBubbles` 和最终性能一起看。

换句话说，final 相比 mid 的关键现象不是“某一个失败计数单独下降”，而是：

- 失败分类结构发生了重排；
- 成功的 `2-Fetch` 明显增加；
- 真正的 fetch supply 泡泡显著减少；
- 这些共同转化成了性能收益。

#### 7.3 典型 benchmark 观察

- `perlbench`: `doubleFetchCycle` 占比从约 `8.9%` 提到 `30.0%`，score 再涨 `9.05%`，是 final 版本最强证据。
- `gcc`: `doubleFetchCycle` 占比从约 `22.3%` 提到 `42.8%`，score 再涨 `2.80%`。
- `h264ref`: `fetchBubbles` 相对 mid 再降 `86.65%`，说明 idealized fetch window 极大缓解了前端空泡。
- `libquantum`: `fetchBubbles` 几乎清零，说明它对 fetch window 的供给连续性非常敏感。
- `sjeng`: final 只比 mid 再涨 `0.30%`，说明即便 fetch window 再理想化，它的剩余瓶颈也不主要在这里。

### 8. 对 2-Taken / 2-Fetch feature 的完整理解

#### 8.1 这套 feature 的本质

更准确地说，这不是一个单点 feature，而是一条链：

1. predictor 侧的 `2-Taken` 让 next stream 更早可见；
2. fetch 侧的 `2-Fetch` 尝试在当前周期直接拼接 next stream；
3. fetch window / fetch buffer 的覆盖能力决定这个拼接最终能否落地；
4. 真正的收益只会出现在前端供给确实是主要瓶颈的 benchmark 上。

#### 8.2 为什么 final 是“上界评估”而不是“RTL 等价评估”

从代码结构看，GEM5 final 版本在 fetch 侧使用了 `idealFillFetchBuffer(...)`，它本质上绕过了真实 cache line / 时序填充成本，把整个 fetch window 理想化。

因此 final 结果适合回答的是：

- 如果 `2-Taken + 2-Fetch` 的几何约束被大幅放宽，理论上还能吃到多少前端收益？

它不等价于：

- RTL 已经具备同样实现成本下的可达收益。

换句话说，final 是一个很有价值的上界，但不能直接视为硬件最终可实现值。

#### 8.3 当前数据里，2-Taken 的独立贡献并没有被完全隔离

虽然代码上 `2-Taken` 很重要，但在这批 `2026-03-25` 数据里，真正详尽的是 `2-Fetch` 计数器，而不是 predictor block1 的独立统计。因此：

- 我们可以很清楚地分析 `2-Fetch` 成功/失败和瓶颈；
- 但很难仅凭这批数据把“2-Taken 单独贡献”与“2-Fetch 落地贡献”完全分离。

目前最接近 `2-Taken` 独立信号的，是 `noNext` 相关计数，因为它反映了 fetch 想继续拼 next stream 时，FTQ 里有没有已经准备好的下一段。

### 9. 其他值得注意的点

#### 9.1 这批数据的收益模式非常像“控制流密集 / 前端敏感” workloads 的专项优化

收益最高的 benchmark 基本都是：

- CFG 碎片化；
- 分支较多；
- 前端供给本来就容易形成 bubble；
- fetch 带宽改善能较直接转化成 IPC。

反过来，像 `mcf` 这类更偏后端/内存系统主导的 workload，就算 `fetchBubbles` 改善，也不一定转化成最终 score。

#### 9.2 对后续硬件化最重要的不是继续加“是否成功”计数，而是继续缩小几何失败

如果后续要把这个 feature 往更真实的实现推进，最关键的问题不是“2-Fetch 有没有成功过”，而是：

- 如何缩小 `span too large`；
- 如何减少 `target not in buffer`；
- 如何让更多机会从“几何上失败”变成“几何上可行”。

也就是说，下一阶段优化重点应该是 fetch window 组织方式、buffer 覆盖策略和真实 I-cache / IFU 路径，而不是继续只盯着 predictor 端。

### 10. 中文总结

最终结论可以概括为四句：

1. `2-Taken` 的价值主要在于更早提供 next stream，真正把它变成性能的是 fetch 侧的 `2-Fetch`。
2. `2-Fetch` 的主要瓶颈不是“没有 next”，而是 `span` 和 `buffer coverage` 这两个几何约束，而且经常是同时出现。
3. `2026-03-25` 的 final 版本把整体 SPEC int 平均分从 baseline 的 `20.6959` 提升到 `21.8804`，总增益 `+5.72%`；其中 idealized fetch-window 实验相对中间版本再贡献 `+2.04%`。
4. 收益最明显的是 `perlbench / xalancbmk / sjeng / gcc / gobmk` 这类前端敏感 workload；而 `mcf / hmmer / omnetpp` 等 workload 说明该 feature 不是普适万能，而是明显偏前端供给型优化。

---

## English Report

### 1. Scope and Datasets

- Final run: `out/gem5/parallel-2026-03-25-idealize-fetch-window-fill/spec_all`
  - commit: `1cae93a32bb04be2376aa5d77eb24e755751c157`
- Intermediate run: `out/gem5/parallel-2026-03-23-deepdive-on-2fetch-fetch/spec_all`
  - commit: `eac43be470b912d3c3e63dbde80a846cd3377113`
- Baseline run: `out/gem5/parallel-2026-03-25-idealize-fetch-window-fill/ref/spec_all`
  - commit: `fc642271651f9460719b12a5fa7573cfa696587f`

This report combines code inspection and weighted SPEC analysis. The main goal is to understand:

1. how `2-Taken` feeds `2-Fetch`,
2. when `2-Fetch` succeeds or fails,
3. how much the idealized fetch-window experiment contributes,
4. which SPEC benchmarks benefit and why.

### 2. Implementation-Level Understanding

#### 2.1 What `2-Taken` really does

- In `GEM5/src/cpu/pred/btb/decoupled_bpred.cc`, `DecoupledBPUWithBTB::tick()` can issue two prediction attempts per tick when `enable2Taken` is enabled.
- In `GEM5/src/cpu/pred/btb/decoupled_bpred.hh`, fetch uses `ftqHasNext()` and `ftqNext()` to look ahead to the next stream.

So the main function of `2-Taken` is not a direct fetch-bandwidth gain by itself. Its real value is making the next fetch target visible early enough for fetch-side same-cycle stitching.

#### 2.2 What `2-Fetch` really does

- The key logic is in `GEM5/src/cpu/o3/fetch.cc`, especially `Fetch::lookupAndUpdateNextPC(...)`.
- At stream run-out, fetch attempts same-cycle continuation only if:
  - the current stream ends with a predicted taken branch,
  - `2-Fetch` is enabled,
  - the next FTQ entry is already present,
  - the predicted branch target matches the next stream start,
  - the merged span does not exceed `maxFetchBytesPerCycle`,
  - and the target is still covered by the current fetch buffer.

Therefore, `2-Fetch` is not just “fetching more bytes”. It is same-cycle continuation across a predicted-taken branch into the next already-known stream.

#### 2.3 What the final version changes

The final commit `1cae93a` mainly idealizes the fetch window:

- `fetchBufferSize` increases from the default `66B` to `258B`.
- `idealFetchWindowFill = True` enables `Fetch::idealFillFetchBuffer(...)`, which fills the whole window using a functional read.
- `maxFetchBytesPerCycle` increases to `256B`.

This matters for interpretation: final vs intermediate is not a pure “ideal fetch buffer only” experiment. It is more accurately an **idealized fetch-window** experiment combining:

- a larger coverage window,
- a more ideal fill mechanism,
- and a much looser same-cycle span constraint.

### 3. Why the New Counters Matter

The intermediate run already introduces useful `2-Fetch` counters, and the final version adds finer failure decomposition plus geometry distributions.

The most valuable groups are:

#### 3.1 Opportunity and success

- `twoFetchOpportunity`
- `twoFetchTaken`
- `singleFetchCycleCount`
- `doubleFetchCycleCount`

These tell us how often `2-Fetch` could be evaluated, how often it succeeds, and how often it really turns into same-cycle dual-stream fetch.

#### 3.2 Failure reasons

- `twoFetchFailOnlyNotPredTaken`
- `twoFetchFailOnlyNoNext`
- `twoFetchFailOnlySpanTooLarge`
- `twoFetchFailOnlyTargetNotInBuffer`
- `twoFetchFailBothSpanAndTargetNotInBuffer`

These counters are the key reason the analysis is actionable: they separate predictor-side availability, FTQ visibility, span limits, and buffer-coverage limits.

#### 3.3 Geometry diagnostics

- `twoFetchSpanBytes`
- `twoFetchTargetOffsetFromBufferStart`
- `twoFetchTargetDistancePastBufferEnd`
- `twoFetchNextStartDelta`
- `twoFetchRunOutBranchOffset`

These explain *why* a benchmark is geometry-friendly or geometry-hostile for `2-Fetch`.

### 4. Overall Performance Result

- Baseline overall/int avg: `20.6959`
- Intermediate overall/int avg: `21.4439` (`+3.61%` vs baseline)
- Final overall/int avg: `21.8804` (`+5.72%` vs baseline, `+2.04%` vs intermediate)

This indicates:

- the core `2-Taken + 2-Fetch` mechanism already produces a meaningful gain,
- and the idealized fetch-window experiment unlocks another visible step,
- but the benefit is concentrated in frontend-sensitive workloads rather than being uniformly distributed.

### 5. Per-Benchmark Result

| Benchmark | Ref Score | Mid Score | Final Score | Final vs Ref | Final vs Mid |
| --- | ---: | ---: | ---: | ---: | ---: |
| perlbench | 16.94 | 18.31 | 19.97 | +17.90% | +9.05% |
| xalancbmk | 33.96 | 36.91 | 38.17 | +12.40% | +3.41% |
| sjeng | 13.17 | 14.47 | 14.51 | +10.20% | +0.30% |
| gcc | 21.17 | 22.61 | 23.24 | +9.77% | +2.80% |
| gobmk | 15.61 | 16.35 | 16.59 | +6.24% | +1.47% |
| h264ref | 25.23 | 25.43 | 26.29 | +4.18% | +3.36% |
| astar | 14.33 | 14.75 | 14.89 | +3.91% | +0.99% |
| bzip2 | 11.26 | 11.41 | 11.55 | +2.52% | +1.20% |
| libquantum | 46.86 | 46.79 | 47.58 | +1.53% | +1.69% |
| mcf | 34.65 | 34.93 | 34.93 | +0.80% | -0.01% |
| omnetpp | 21.85 | 21.91 | 22.00 | +0.72% | +0.43% |
| hmmer | 17.07 | 17.07 | 17.08 | +0.09% | +0.07% |

### 6. Where the Gains Come From

| Benchmark | Score Gain vs Ref | `fetch_nisn_mean` Gain | Final `doubleFetchCycle` Share | Final `2-Fetch` Success Rate | `frontendBound` Drop | `fetchBubbles` Drop |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| perlbench | +17.90% | +33.83% | 29.96% | 33.30% | -89.77% | -89.98% |
| xalancbmk | +12.40% | +18.55% | 46.47% | 55.25% | -83.29% | -84.70% |
| sjeng | +10.20% | +33.15% | 21.75% | 30.48% | -59.45% | -63.19% |
| gcc | +9.77% | +22.07% | 42.76% | 49.00% | -73.46% | -78.06% |
| gobmk | +6.24% | +32.25% | 24.48% | 32.15% | -70.72% | -71.42% |
| h264ref | +4.18% | +5.74% | 11.60% | 17.35% | -89.74% | -90.43% |

The strongest beneficiaries are the benchmarks where better fetch supply directly translates into higher throughput: especially `perlbench`, `xalancbmk`, `gcc`, `sjeng`, and `gobmk`.

### 7. Why `2-Fetch` Fails

Across the 12 INT benchmarks in the final run:

- `twoFetchOpportunity`: about `61.09M`
- `twoFetchTaken`: about `22.98M`
- overall success rate: `37.61%`
- `doubleFetchCycleCount / (single + double)`: `28.70%`

Among failed opportunities, using the final fail-only counters:

- only `not predicted taken`: `29.69%`
- only `no next stream`: `13.83%`
- only `span too large`: `7.01%`
- only `target not in buffer`: `10.51%`
- both `span too large` and `target not in buffer`: `38.96%`

This is the key architectural conclusion:

1. the dominant bottleneck is **not** “no next FTQ entry”,
2. the dominant bottleneck is **fetch geometry**,
3. and span-limit and buffer-coverage failures often happen together.

In other words, many candidate cases are not merely “slightly out of range”; they are structurally too far away for same-cycle continuation.

### 8. Intermediate vs Final: Effect of the Idealized Fetch Window

The final run improves the overall score by `+2.04%` over the intermediate run.

Largest additional gains:

- `perlbench`: `+9.05%`
- `xalancbmk`: `+3.41%`
- `h264ref`: `+3.36%`
- `gcc`: `+2.80%`
- `libquantum`: `+1.69%`
- `gobmk`: `+1.47%`

Nearly flat:

- `sjeng`
- `omnetpp`
- `hmmer`
- `mcf`

An important interpretation point: from intermediate to final, `span` failure often drops, `doubleFetchCycle` rises, and `fetchBubbles` drops sharply, but `targetNotInBuffer` can still rise in rate for some benchmarks. That does **not** mean the idealized window is ineffective. It more likely means the looser `256B` span limit reclassifies many previously span-limited cases into deeper geometry checks, some of which now become either `target-not-in-buffer` or outright successes.

So the real signature of the final experiment is not one single counter going down. It is the combination of:

- more successful `2-Fetch`,
- fewer fetch bubbles,
- larger double-fetch cycle share,
- and a higher final SPEC score.

### 9. Complete Assessment of the Feature

The cleanest way to understand the feature is as a chain:

1. `2-Taken` exposes the next stream earlier.
2. `2-Fetch` tries to stitch that next stream into the current cycle.
3. fetch-window coverage decides whether that stitch is physically possible.
4. actual speedup appears only on workloads that are genuinely frontend-limited.

The final run is best interpreted as an **upper-bound study**, not an RTL-equivalent result, because `idealFillFetchBuffer(...)` idealizes the fetch-window fill path.

Also, in this dataset the `2-Fetch` side is very well instrumented, but the isolated contribution of `2-Taken` is not fully separable. The closest proxy is the `noNext` family of counters, because they indicate when fetch wanted to continue but the next FTQ stream was not ready.

### 10. English Summary

The main conclusions are:

1. `2-Taken` is mostly an enabler; `2-Fetch` is the mechanism that turns prediction lookahead into throughput.
2. The main bottleneck of `2-Fetch` is not missing-next-stream; it is fetch geometry: span and buffer coverage.
3. The final `2026-03-25` run raises the SPEC int average from `20.6959` to `21.8804`, for a total `+5.72%`, while the idealized fetch-window step alone contributes another `+2.04%` over the intermediate run.
4. The biggest winners are frontend-sensitive benchmarks such as `perlbench`, `xalancbmk`, `sjeng`, `gcc`, and `gobmk`; low-gain cases like `mcf`, `hmmer`, and `omnetpp` show that this is a frontend optimization, not a universal one.

---

## Executive Summary / 汇报摘要

### 中文 Executive Summary

#### 一页结论

- 本次 `2-Taken + 2-Fetch` 评估显示，final 版本相对 baseline 把 SPECint 加权平均分从 `20.6959` 提升到 `21.8804`，总增益 `+5.72%`。
- 相对中间版本，final 的 idealized fetch-window 实验再带来 `+2.04%`，说明 fetch-window 覆盖能力和窗口填充方式仍然是 `2-Fetch` 能否兑现收益的关键因素。
- 收益最明显的 benchmark 是 `perlbench (+17.90%)`、`xalancbmk (+12.40%)`、`sjeng (+10.20%)`、`gcc (+9.77%)`、`gobmk (+6.24%)`。
- 低收益 benchmark 包括 `mcf (+0.80%)`、`omnetpp (+0.72%)`、`hmmer (+0.09%)`，说明该 feature 更偏向前端供给优化，而不是对所有工作负载都普适有效。

#### 机制理解

- `2-Taken` 的核心价值是让 next stream 更早进入 FTQ，可视为 `2-Fetch` 的前提条件。
- `2-Fetch` 的核心价值是在当前 stream run-out 且预测 taken 时，在同一个 fetch 周期继续消费下一条 stream。
- 因此，真正转化成 IPC/score 的关键执行点在 fetch stage，而不是 predictor 单独一侧。

#### 主要证据

- 高收益 benchmark 上，`fetch_nisn_mean` 普遍显著上升，例如 `perlbench +33.83%`、`sjeng +33.15%`、`gobmk +32.25%`、`gcc +22.07%`。
- 同时，`fetchBubbles` 与 `frontendBound` 大幅下降，例如 `perlbench` 分别下降 `89.98%` / `89.77%`，`xalancbmk` 分别下降 `84.70%` / `83.29%`。
- final 聚合统计中，`twoFetchOpportunity` 约 `61.09M`，`twoFetchTaken` 约 `22.98M`，总体成功率 `37.61%`；`doubleFetchCycle` 占比约 `28.70%`。

#### 瓶颈判断

- `2-Fetch` 的最大瓶颈不是 `no next stream`，而是 fetch geometry。
- 在失败机会中：仅因 `not predicted taken` 占 `29.69%`，仅因 `no next stream` 占 `13.83%`，仅因 `span too large` 占 `7.01%`，仅因 `target not in buffer` 占 `10.51%`，同时受 `span + target-not-in-buffer` 影响的占比高达 `38.96%`。
- 这说明后续若想把该 feature 硬件化、实用化，重点应放在缩小 `span too large` 和 `target not in buffer`，而不是只继续堆 predictor 侧 lookahead。

#### 汇报建议话术

- 这套 feature 的收益链条可以概括为：`2-Taken` 提前暴露 next stream，`2-Fetch` 负责同周期拼接，fetch window 决定拼接能否真正落地。
- final 版本更适合被解读为“前端上界评估”，而不是 RTL 等价结果，因为它同时理想化了 fetch-window fill、window size 和 max span。
- 如果后续继续推进，建议把工作重点放在真实 fetch-window 组织、buffer 覆盖和 I-cache/IFU 路径上。

### English Executive Summary

#### One-page conclusion

- The final `2-Taken + 2-Fetch` configuration raises the weighted SPECint average from `20.6959` to `21.8804`, a total gain of `+5.72%` over the baseline.
- Relative to the intermediate run, the final idealized fetch-window experiment contributes another `+2.04%`, showing that fetch-window coverage and fill behavior are still key enablers for `2-Fetch`.
- The largest benchmark gains are `perlbench (+17.90%)`, `xalancbmk (+12.40%)`, `sjeng (+10.20%)`, `gcc (+9.77%)`, and `gobmk (+6.24%)`.
- Low-gain cases such as `mcf (+0.80%)`, `omnetpp (+0.72%)`, and `hmmer (+0.09%)` show that this is primarily a frontend-supply optimization rather than a universal speedup.

#### Mechanism

- `2-Taken` matters because it exposes the next stream early enough in the FTQ.
- `2-Fetch` matters because it converts that lookahead into same-cycle continuation across a predicted-taken branch.
- So the actual throughput conversion happens in fetch, not in the predictor alone.

#### Main evidence

- High-gain benchmarks also show strong `fetch_nisn_mean` growth, including `perlbench +33.83%`, `sjeng +33.15%`, `gobmk +32.25%`, and `gcc +22.07%`.
- `fetchBubbles` and `frontendBound` also drop sharply, for example `perlbench` by `89.98%` / `89.77%` and `xalancbmk` by `84.70%` / `83.29%`.
- In the final aggregate counters, `twoFetchOpportunity` is about `61.09M`, `twoFetchTaken` is about `22.98M`, overall success rate is `37.61%`, and `doubleFetchCycle` share is about `28.70%`.

#### Bottleneck assessment

- The dominant `2-Fetch` bottleneck is not missing-next-stream; it is fetch geometry.
- Among failed opportunities, `29.69%` are only due to `not predicted taken`, `13.83%` only due to `no next stream`, `7.01%` only due to `span too large`, `10.51%` only due to `target not in buffer`, and `38.96%` are constrained by both `span too large` and `target-not-in-buffer`.
- This strongly suggests that future hardware-oriented work should focus on reducing geometry failures, especially span and coverage limits, rather than only extending predictor-side lookahead.

#### Presentation-ready phrasing

- The feature can be summarized as: `2-Taken` exposes the next stream early, `2-Fetch` stitches it into the same cycle, and the fetch window determines whether the stitch is physically feasible.
- The final run should be treated as an upper-bound frontend study rather than an RTL-equivalent result, because it idealizes fetch-window fill, window size, and max span together.
- The next practical step is to improve realistic fetch-window organization, coverage, and the I-cache/IFU path.
