# 2Taken/2Fetch Analysis for Three Additional Slices

## English

### 1. Purpose

This report extends the previous `2Taken/2Fetch` diagnostic study to three additional slices:

- `bzip2_program_16771`
- `xalancbmk_8082`
- `astar_biglakes_13437`

The goal is to analyze them in the same style as the earlier two-slice report: for each slice, explain

1. how often fetch enters the main `performInstructionFetch()` loop,
2. how often `2Fetch` succeeds and why it fails when it does not,
3. how much wider successful double-fetch cycles are than single-fetch cycles,
4. how frontend widening relates to dispatch and issue bandwidth,
5. what kind of geometric pattern dominates the slice.

The analysis uses the refined deep-dive counters extracted into:

- `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/perf.csv`

and the corresponding `stats.txt` files under the same directory.

### 2. Input Data and Context

The runs were produced with the same setup as the previous report:

- gem5 built with `scons build/RISCV/gem5.opt --gold-linker -j60`
- runtime arguments consistent with `build-system/GEM5.just`
- no BP DB dump enabled, to save runtime

The relevant result directories are:

- `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/bzip2_program_16771`
- `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/xalancbmk_8082`
- `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/astar_biglakes_13437`

### 3. Summary Table

The three slices already show three different `2Fetch` personalities.

| Slice | IPC | fetch_nisn_mean | dispatchRateMean | issueRate | while enter rate | 2Fetch take rate | singleFetchInstsPerCycle | doubleFetchInstsPerCycle |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `bzip2_program_16771` | 2.672 | 7.743 | 5.253 | 4.523 | 94.6% | 15.7% | 8.695 | 11.668 |
| `xalancbmk_8082` | 6.173 | 6.220 | 6.201 | 6.349 | 89.7% | 59.8% | 6.283 | 11.898 |
| `astar_biglakes_13437` | 1.051 | 6.631 | 4.027 | 2.828 | 87.4% | 32.8% | 6.297 | 19.308 |

These numbers already suggest:

- `xalancbmk_8082` is `2Fetch`-friendly and balanced
- `bzip2_program_16771` has a high fetch rate but weak conversion into successful `2Fetch`
- `astar_biglakes_13437` has very wide successful double-fetch bursts, but poor downstream conversion

### 4. Slice 1: `bzip2_program_16771`

#### 4.1 While-loop entry behavior

From `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/bzip2_program_16771/stats.txt:11606`:

- `performIFCalls = 7,137,133`
- `performIFWhileEntered = 6,751,939`
- `performIFWhileNotEnteredFetchQueueFull = 385,194`

Derived:

- while entered rate: `94.6%`
- fetchQueueFull non-entry rate: `5.4%`

Interpretation:

- Fetch enters its main loop extremely often.
- This slice is not frontend-starved at the point of entering `performInstructionFetch()`.
- The main while-loop gate is relatively mild compared with the previous slices.

#### 4.2 `2Fetch` success and failure pattern

From `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/bzip2_program_16771/stats.txt:11610`:

- `twoFetchOpportunity = 6,102,449`
- `twoFetchTaken = 960,399`
- `twoFetchNotTakenNotPredTaken = 953,955`
- `twoFetchNotTakenNoNext = 54,778`
- `twoFetchNotTakenSpanTooLarge = 4,027,628`
- `twoFetchNotTakenTargetNotInBuffer = 3,934,918`

Derived rates from `perf.csv`:

- success: `15.74%`
- fail only because stream end is not predicted taken: `15.63%`
- fail only because next stream unavailable: `0.90%`
- fail only because span too large: `3.25%`
- fail only because target not in buffer: `1.73%`
- fail because both span-too-large and target-not-in-buffer: `62.75%`

Interpretation:

- `2Fetch` succeeds in only a small minority of opportunities.
- The dominant issue is **joint geometric failure**, not a single isolated cause.
- This slice is not mainly failing because the predictor cannot provide the next stream.
- It is also not failing mainly because of one-sided backward geometry like `mcf_12253`.
- Instead, it looks like a case where current stream layout often violates both:
  - the per-cycle fetch span budget,
  - and the current buffer coverage requirement.

#### 4.3 Single-fetch versus double-fetch effectiveness

From `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/bzip2_program_16771/stats.txt:11955`:

- `singleFetchCycleCount = 5,802,724`
- `singleFetchCycleInstMean = 8.695`
- `doubleFetchCycleCount = 642,863`
- `doubleFetchCycleInstMean = 11.668`

Interpretation:

- A successful double-fetch cycle is wider than a single-fetch cycle, but the gap is only moderate compared with GCC or ASTAR.
- This slice already fetches quite a lot even in single-fetch cycles.
- Therefore the marginal gain from double-fetch is smaller than in short-block slices.

#### 4.4 Relationship to dispatch and issue

From `perf.csv`:

- `fetch_nisn_mean = 7.743`
- `dispatchRateMean = 5.253`
- `issueRate = 4.523`

Interpretation:

- The frontend is already very strong on average.
- The bigger drop happens after fetch.
- This means `bzip2_program_16771` is not a pure frontend-limited case. It already has high fetch supply, but downstream conversion is significantly lower.

#### 4.5 Geometric interpretation

From the refined counters in `perf.csv`:

- `twoFetchTargetBeforeBufferRate = 30.52%`
- `twoFetchTargetAfterBufferRate = 33.96%`
- `twoFetchNextStreamBackwardRate = 30.52%`
- `twoFetchSpanBackwardRate = 28.79%`

Interpretation:

- This slice has both backward-target and forward-overflow behavior.
- Neither direction dominates completely.
- The high joint-failure rate suggests that this slice is broadly geometry-unfriendly to the current `2Fetch` rule.

#### 4.6 Slice conclusion

`bzip2_program_16771` is not a slice where `2Fetch` is absent. Instead, it is a slice where:

- fetch already runs strongly,
- single-fetch cycles are already wide,
- successful `2Fetch` is somewhat wider,
- but most `2Fetch` opportunities fail because stream geometry violates both span and buffer constraints.

So its limited `2Fetch` gain comes from **geometric incompatibility plus limited marginal frontend benefit**, not from lack of fetch activity.

### 5. Slice 2: `xalancbmk_8082`

#### 5.1 While-loop entry behavior

From `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/xalancbmk_8082/stats.txt:12346`:

- `performIFCalls = 2,975,590`
- `performIFWhileEntered = 2,669,216`
- `performIFWhileNotEnteredFetchQueueFull = 306,374`

Derived:

- while entered rate: `89.7%`
- fetchQueueFull non-entry rate: `10.3%`

Interpretation:

- Fetch is active for most cycles.
- The frontend is not blocked at entry very often.
- Queue pressure exists, but it is not overwhelming.

#### 5.2 `2Fetch` success and failure pattern

From `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/xalancbmk_8082/stats.txt:12350`:

- `twoFetchOpportunity = 3,749,604`
- `twoFetchTaken = 2,241,779`
- `twoFetchNotTakenNotPredTaken = 114,397`
- `twoFetchNotTakenNoNext = 311,836`
- `twoFetchNotTakenSpanTooLarge = 870,673`
- `twoFetchNotTakenTargetNotInBuffer = 871,920`

Derived rates:

- success: `59.79%`
- fail only because stream end is not predicted taken: `3.05%`
- fail only because next stream unavailable: `8.32%`
- fail only because span too large: `5.59%`
- fail only because target not in buffer: `5.63%`
- fail because both span-too-large and target-not-in-buffer: `17.63%`

Interpretation:

- `2Fetch` succeeds in a clear majority of opportunities.
- This is one of the most `2Fetch`-friendly slices in the current deep-dive set.
- Geometry still matters, but it does not dominate the slice the way it does for `bzip2` or `mcf`.

#### 5.3 Single-fetch versus double-fetch effectiveness

From `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/xalancbmk_8082/stats.txt:12695`:

- `singleFetchCycleCount = 1,403,007`
- `singleFetchCycleInstMean = 6.283`
- `doubleFetchCycleCount = 952,862`
- `doubleFetchCycleInstMean = 11.898`

Interpretation:

- Successful double-fetch cycles are about 1.9x as wide as single-fetch cycles.
- This is a strong widening effect.
- The widening is not as extreme as ASTAR, but it is much more frequent and much more consistently useful.

#### 5.4 Relationship to dispatch and issue

- `fetch_nisn_mean = 6.220`
- `dispatchRateMean = 6.201`
- `issueRate = 6.349`

Interpretation:

- This slice is unusually well balanced.
- Fetch, dispatch, and issue are all near the same level.
- This is a strong sign that successful `2Fetch` widening is actually getting converted into useful machine throughput.

#### 5.5 Geometric interpretation

- `twoFetchTargetBeforeBufferRate = 14.28%`
- `twoFetchTargetAfterBufferRate = 8.97%`
- `twoFetchNextStreamBackwardRate = 14.28%`
- `twoFetchSpanBackwardRate = 8.66%`

Interpretation:

- Backward geometry exists, but it is modest.
- Forward overflow also exists, but is not dominant.
- The overall pattern is balanced enough that `2Fetch` remains broadly legal and beneficial.

#### 5.6 Slice conclusion

`xalancbmk_8082` is a highly successful `2Fetch` slice because:

- the fetch loop is active most of the time,
- `2Fetch` succeeds often,
- double-fetch cycles are much wider than single-fetch cycles,
- and frontend widening is well reflected in dispatch and issue bandwidth.

This is close to an ideal “`2Fetch` works and the machine can use it” case.

### 6. Slice 3: `astar_biglakes_13437`

#### 6.1 While-loop entry behavior

From `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/astar_biglakes_13437/stats.txt:13302`:

- `performIFCalls = 17,765,172`
- `performIFWhileEntered = 15,529,719`
- `performIFWhileNotEnteredFetchQueueFull = 2,235,453`

Derived:

- while entered rate: `87.4%`
- fetchQueueFull non-entry rate: `12.6%`

Interpretation:

- Fetch is again active most of the time.
- The slice is not failing because the frontend cannot start work.

#### 6.2 `2Fetch` success and failure pattern

From `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/astar_biglakes_13437/stats.txt:13306`:

- `twoFetchOpportunity = 17,719,736`
- `twoFetchTaken = 5,817,294`
- `twoFetchNotTakenNotPredTaken = 885,080`
- `twoFetchNotTakenNoNext = 1,558,779`
- `twoFetchNotTakenSpanTooLarge = 7,041,872`
- `twoFetchNotTakenTargetNotInBuffer = 9,455,307`

Derived rates:

- success: `32.83%`
- fail only because stream end is not predicted taken: `4.99%`
- fail only because next stream unavailable: `8.80%`
- fail only because span too large: `0.018%`
- fail only because target not in buffer: `13.64%`
- fail because both span-too-large and target-not-in-buffer: `39.72%`

Interpretation:

- `2Fetch` succeeds in about one-third of all opportunities.
- The dominant problem is again geometry, especially the overlap between span-too-large and target-not-in-buffer.
- Unlike `bzip2`, however, the slice still retains a substantial successful `2Fetch` fraction.

#### 6.3 Single-fetch versus double-fetch effectiveness

From `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/astar_biglakes_13437/stats.txt:13651`:

- `singleFetchCycleCount = 10,013,965`
- `singleFetchCycleInstMean = 6.297`
- `doubleFetchCycleCount = 3,266,883`
- `doubleFetchCycleInstMean = 19.308`

Interpretation:

- This is the most dramatic widening among the three new slices.
- Successful double-fetch cycles are more than 3x as wide as single-fetch cycles.
- So when `2Fetch` works here, it works very strongly.

#### 6.4 Relationship to dispatch and issue

- `fetch_nisn_mean = 6.631`
- `dispatchRateMean = 4.027`
- `issueRate = 2.828`

Interpretation:

- This slice shows a large frontend-to-backend drop.
- The frontend can create very wide bursts, but the machine cannot sustain them downstream.
- This is the clearest case among the three new slices where `2Fetch` can help frontend burst width, but later stages absorb a large fraction of the benefit.

#### 6.5 Geometric interpretation

- `twoFetchTargetBeforeBufferRate = 30.84%`
- `twoFetchTargetAfterBufferRate = 22.53%`
- `twoFetchNextStreamBackwardRate = 30.84%`
- `twoFetchSpanBackwardRate = 17.20%`

Interpretation:

- This slice has both backward and forward geometric pressure.
- It is not as one-sided as `mcf_12253`.
- It is also not as balanced and forgiving as `xalancbmk_8082`.

#### 6.6 Slice conclusion

`astar_biglakes_13437` is a mixed case:

- `2Fetch` succeeds often enough to matter,
- successful double-fetch cycles are extremely wide,
- but geometry blocks many opportunities,
- and backend consumption is far weaker than frontend burst width.

So its bottleneck is not only `2Fetch` legality; it is also the machine’s ability to absorb the widened fetch stream.

### 7. Cross-Slice Comparison

These three slices form a useful contrast set.

#### 7.1 `bzip2_program_16771`: frontend already strong, geometry hostile

- Very high `fetch_nisn_mean`
- low `2Fetch` success
- high joint-failure rate
- moderate widening when `2Fetch` succeeds
- noticeable drop from fetch to issue

This is a slice where the frontend is already powerful, but current `2Fetch` geometry is not a good match.

#### 7.2 `xalancbmk_8082`: best-balanced `2Fetch` case

- high `2Fetch` success
- strong widening
- low-to-moderate geometric pressure
- dispatch and issue remain close to fetch

This is the cleanest positive example among the three.

#### 7.3 `astar_biglakes_13437`: very wide bursts, weak downstream conversion

- medium `2Fetch` success
- extremely large widening when successful
- significant geometric difficulty
- large drop from fetch to issue

This is a slice where `2Fetch` can create big frontend bursts, but backend throughput is the real limit.

### 8. Main Conclusions

1. All three slices enter `performInstructionFetch()` frequently; none is primarily blocked at loop entry.
2. `bzip2_program_16771` is limited mainly by joint geometric failure, not by lack of fetch activity.
3. `xalancbmk_8082` is a strong positive `2Fetch` case: successful, balanced, and well-converted into issue bandwidth.
4. `astar_biglakes_13437` gets large frontend burst widening from `2Fetch`, but much of that gain is lost before issue.
5. Across the three slices, `2Fetch` effectiveness depends on two things together:
   - whether the fetch geometry allows same-cycle stitching,
   - whether downstream stages can absorb the resulting wider fetch stream.

### 9. Output Artifacts

- `docs/plans/2026-03-19-fetch-2fetch-three-slice-analysis.md`
- `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/perf.csv`
- `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/bzip2_program_16771/stats.txt`
- `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/xalancbmk_8082/stats.txt`
- `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/astar_biglakes_13437/stats.txt`

---

## Chinese Translation / 中文翻译

### 1. 目的

本报告将之前的 `2Taken/2Fetch` 诊断扩展到另外三个切片：

- `bzip2_program_16771`
- `xalancbmk_8082`
- `astar_biglakes_13437`

目标是延续上一份报告的分析方式，对每个切片分别回答：

1. fetch 进入 `performInstructionFetch()` 主循环的频率如何；
2. `2Fetch` 成功发生的频率如何，失败时的主要原因是什么；
3. single-fetch 周期与成功 `2Fetch` 周期分别平均取多少条指令；
4. 前端变宽和 dispatch / issue 带宽之间是什么关系；
5. 哪种几何模式在该切片中占主导。

分析使用的核心输入为：

- `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/perf.csv`

及对应目录下的 `stats.txt`。

### 2. 输入数据与实验背景

这些运行与上一份报告保持一致：

- gem5 用 `scons build/RISCV/gem5.opt --gold-linker -j60` 编译
- 运行参数参考 `build-system/GEM5.just`
- 为节省时间，不启用 BP DB dump

对应的结果目录为：

- `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/bzip2_program_16771`
- `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/xalancbmk_8082`
- `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/astar_biglakes_13437`

### 3. 总览表

这三个切片分别代表了三种不同的 `2Fetch` 形态。

| Slice | IPC | fetch_nisn_mean | dispatchRateMean | issueRate | while 进入率 | 2Fetch 成功率 | singleFetchInstsPerCycle | doubleFetchInstsPerCycle |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `bzip2_program_16771` | 2.672 | 7.743 | 5.253 | 4.523 | 94.6% | 15.7% | 8.695 | 11.668 |
| `xalancbmk_8082` | 6.173 | 6.220 | 6.201 | 6.349 | 89.7% | 59.8% | 6.283 | 11.898 |
| `astar_biglakes_13437` | 1.051 | 6.631 | 4.027 | 2.828 | 87.4% | 32.8% | 6.297 | 19.308 |

从总表可以初步看出：

- `xalancbmk_8082` 是一个 `2Fetch` 友好且较平衡的切片；
- `bzip2_program_16771` 的平均 fetch 很高，但成功 `2Fetch` 转化较弱；
- `astar_biglakes_13437` 的成功 `2Fetch` 周期很宽，但下游消化能力明显不足。

### 4. 切片一：`bzip2_program_16771`

#### 4.1 while 循环进入情况

根据 `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/bzip2_program_16771/stats.txt:11606`：

- `performIFCalls = 7,137,133`
- `performIFWhileEntered = 6,751,939`
- `performIFWhileNotEnteredFetchQueueFull = 385,194`

可得：

- while 进入率：`94.6%`
- 因 `fetchQueueFull` 未进入的比例：`5.4%`

解释：

- fetch 几乎总能进入主循环。
- 这个切片并不是在 `performInstructionFetch()` 入口就被卡住的。
- while 入口处的前端活动已经非常积极。

#### 4.2 `2Fetch` 成功与失败模式

根据 `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/bzip2_program_16771/stats.txt:11610`：

- `twoFetchOpportunity = 6,102,449`
- `twoFetchTaken = 960,399`
- `twoFetchNotTakenNotPredTaken = 953,955`
- `twoFetchNotTakenNoNext = 54,778`
- `twoFetchNotTakenSpanTooLarge = 4,027,628`
- `twoFetchNotTakenTargetNotInBuffer = 3,934,918`

由 `perf.csv` 换算可得：

- 成功率：`15.74%`
- 仅因“流末尾不是 predicted taken”而失败：`15.63%`
- 仅因 next stream 不可用而失败：`0.90%`
- 仅因 span 太大而失败：`3.25%`
- 仅因 target 不在 buffer 中而失败：`1.73%`
- 因 `span-too-large` 与 `target-not-in-buffer` 同时出现而失败：`62.75%`

解释：

- `2Fetch` 虽然存在，但成功机会只占少数。
- 最主要的问题不是单一失败项，而是**联合几何失败**。
- 这个切片既不是因为 next stream 不存在，也不是因为单向 backward 几何像 `mcf_12253` 那样压倒性主导。
- 它更像是：stream 布局经常同时违反
  - 每周期 fetch span 限制，
  - 以及当前 fetch buffer 覆盖条件。

#### 4.3 single-fetch 与 double-fetch 周期效果

根据 `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/bzip2_program_16771/stats.txt:11955`：

- `singleFetchCycleCount = 5,802,724`
- `singleFetchCycleInstMean = 8.695`
- `doubleFetchCycleCount = 642,863`
- `doubleFetchCycleInstMean = 11.668`

解释：

- 成功 `2Fetch` 周期确实更宽，但相对提升幅度没有 GCC 或 ASTAR 那么大。
- 这是因为该切片在 single-fetch 周期里本来就已经能取得很多指令。
- 因此，`2Fetch` 的边际收益相对较小。

#### 4.4 与 dispatch / issue 的关系

- `fetch_nisn_mean = 7.743`
- `dispatchRateMean = 5.253`
- `issueRate = 4.523`

解释：

- 该切片的前端平均供给已经很强。
- 更大的跌落发生在 fetch 之后。
- 这说明它并不是一个纯前端受限切片；即便 fetch 很强，下游的转化效率仍然有限。

#### 4.5 几何解释

- `twoFetchTargetBeforeBufferRate = 30.52%`
- `twoFetchTargetAfterBufferRate = 33.96%`
- `twoFetchNextStreamBackwardRate = 30.52%`
- `twoFetchSpanBackwardRate = 28.79%`

解释：

- 这个切片既有明显的 backward-target，也有明显的 forward-overflow。
- 没有哪一边完全主导。
- 高 joint-failure 比例说明：当前 `2Fetch` 的几何条件与该切片的 stream 形态整体匹配度不高。

#### 4.6 小结

`bzip2_program_16771` 不是没有 `2Fetch` 的切片，而是一个：

- fetch 本身已经很强，
- single-fetch 周期已经比较宽，
- double-fetch 周期能更宽一些，
- 但大多数 `2Fetch` 机会会被 span 和 buffer 两类几何约束同时挡住。

因此，它的 `2Fetch` 收益偏小，主要来自**几何不匹配 + 前端边际收益有限**，而不是 fetch 活跃度不足。

### 5. 切片二：`xalancbmk_8082`

#### 5.1 while 循环进入情况

根据 `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/xalancbmk_8082/stats.txt:12346`：

- `performIFCalls = 2,975,590`
- `performIFWhileEntered = 2,669,216`
- `performIFWhileNotEnteredFetchQueueFull = 306,374`

可得：

- while 进入率：`89.7%`
- 因 `fetchQueueFull` 未进入的比例：`10.3%`

解释：

- fetch 在大多数周期都能进入主循环。
- 入口处并没有明显的前端饥饿现象。
- 队列压力存在，但不算严重。

#### 5.2 `2Fetch` 成功与失败模式

根据 `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/xalancbmk_8082/stats.txt:12350`：

- `twoFetchOpportunity = 3,749,604`
- `twoFetchTaken = 2,241,779`
- `twoFetchNotTakenNotPredTaken = 114,397`
- `twoFetchNotTakenNoNext = 311,836`
- `twoFetchNotTakenSpanTooLarge = 870,673`
- `twoFetchNotTakenTargetNotInBuffer = 871,920`

对应比率：

- 成功率：`59.79%`
- 仅因“末尾不是 predicted taken”失败：`3.05%`
- 仅因 next stream 不可用失败：`8.32%`
- 仅因 span 太大失败：`5.59%`
- 仅因 target 不在 buffer 中失败：`5.63%`
- 因 span 与 target 两者同时失败：`17.63%`

解释：

- `2Fetch` 在这个切片上能在大多数机会中成功。
- 这是目前 deep-dive 样本里最 `2Fetch` 友好的切片之一。
- 几何条件仍有影响，但远没有达到压倒性主导。

#### 5.3 single-fetch 与 double-fetch 周期效果

根据 `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/xalancbmk_8082/stats.txt:12695`：

- `singleFetchCycleCount = 1,403,007`
- `singleFetchCycleInstMean = 6.283`
- `doubleFetchCycleCount = 952,862`
- `doubleFetchCycleInstMean = 11.898`

解释：

- 成功 `2Fetch` 周期大约是 single-fetch 周期的 `1.9x`。
- 这是一个很明显的前端扩宽效果。
- 它不像 ASTAR 那样“单次特别极端”，但更频繁、更稳定、更能转化成整体收益。

#### 5.4 与 dispatch / issue 的关系

- `fetch_nisn_mean = 6.220`
- `dispatchRateMean = 6.201`
- `issueRate = 6.349`

解释：

- 这是一个非常平衡的切片。
- fetch、dispatch、issue 三者都在同一量级。
- 这表明成功的 `2Fetch` 扩宽在这里确实被后续流水线有效利用了。

#### 5.5 几何解释

- `twoFetchTargetBeforeBufferRate = 14.28%`
- `twoFetchTargetAfterBufferRate = 8.97%`
- `twoFetchNextStreamBackwardRate = 14.28%`
- `twoFetchSpanBackwardRate = 8.66%`

解释：

- backward 几何存在，但不重。
- forward overflow 也存在，但不重。
- 整体上它的几何分布足够温和，因此 `2Fetch` 保持了高可用性。

#### 5.6 小结

`xalancbmk_8082` 是一个非常成功的 `2Fetch` 切片，因为：

- fetch 主循环活跃，
- `2Fetch` 成功率高，
- 成功 double-fetch 周期明显更宽，
- 且这种扩宽基本能顺利传递到 dispatch / issue。

这可以视为一个接近理想的“`2Fetch` 有效且机器能吃下去”的案例。

### 6. 切片三：`astar_biglakes_13437`

#### 6.1 while 循环进入情况

根据 `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/astar_biglakes_13437/stats.txt:13302`：

- `performIFCalls = 17,765,172`
- `performIFWhileEntered = 15,529,719`
- `performIFWhileNotEnteredFetchQueueFull = 2,235,453`

可得：

- while 进入率：`87.4%`
- 因 `fetchQueueFull` 未进入的比例：`12.6%`

解释：

- fetch 大多数时候都能进入主循环。
- 它的瓶颈不在于前端“启动不起来”。

#### 6.2 `2Fetch` 成功与失败模式

根据 `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/astar_biglakes_13437/stats.txt:13306`：

- `twoFetchOpportunity = 17,719,736`
- `twoFetchTaken = 5,817,294`
- `twoFetchNotTakenNotPredTaken = 885,080`
- `twoFetchNotTakenNoNext = 1,558,779`
- `twoFetchNotTakenSpanTooLarge = 7,041,872`
- `twoFetchNotTakenTargetNotInBuffer = 9,455,307`

对应比率：

- 成功率：`32.83%`
- 仅因“末尾不是 predicted taken”失败：`4.99%`
- 仅因 next stream 不可用失败：`8.80%`
- 仅因 span 太大失败：`0.018%`
- 仅因 target 不在 buffer 中失败：`13.64%`
- 因 span 与 target 两者同时失败：`39.72%`

解释：

- `2Fetch` 在这个切片上能成功约三分之一机会。
- 最主要的问题依然是几何条件，尤其是 `span-too-large` 和 `target-not-in-buffer` 的重叠。
- 但与 `bzip2` 不同的是，它仍保留了相当可观的成功 `2Fetch` 比例。

#### 6.3 single-fetch 与 double-fetch 周期效果

根据 `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/astar_biglakes_13437/stats.txt:13651`：

- `singleFetchCycleCount = 10,013,965`
- `singleFetchCycleInstMean = 6.297`
- `doubleFetchCycleCount = 3,266,883`
- `doubleFetchCycleInstMean = 19.308`

解释：

- 这是三个新增切片里最显著的扩宽效果。
- 成功 `2Fetch` 周期平均宽度超过 single-fetch 周期的三倍。
- 说明只要 `2Fetch` 成功，它在这个切片上就非常有力。

#### 6.4 与 dispatch / issue 的关系

- `fetch_nisn_mean = 6.631`
- `dispatchRateMean = 4.027`
- `issueRate = 2.828`

解释：

- 这个切片存在明显的前端到后端跌落。
- frontend 能做出很宽的 burst，但后续流水线无法持续消化。
- 这是三个新增切片里最典型的“前端做出来了，但后段吃不下去”的案例。

#### 6.5 几何解释

- `twoFetchTargetBeforeBufferRate = 30.84%`
- `twoFetchTargetAfterBufferRate = 22.53%`
- `twoFetchNextStreamBackwardRate = 30.84%`
- `twoFetchSpanBackwardRate = 17.20%`

解释：

- backward 与 forward 的几何压力都比较明显。
- 它不像 `mcf_12253` 那样完全单边，也不像 `xalancbmk_8082` 那样足够温和。

#### 6.6 小结

`astar_biglakes_13437` 是一个混合型切片：

- `2Fetch` 成功率足以产生作用，
- 成功 double-fetch 周期非常宽，
- 但几何条件拦掉了很多机会，
- 且后端消费能力明显弱于前端 burst 宽度。

因此，它的问题不仅仅是 `2Fetch` 合法性，还包括后续流水线对 widened fetch stream 的承接能力。

### 7. 跨切片对比

这三个切片构成了一组很有代表性的对照。

#### 7.1 `bzip2_program_16771`：前端已强，但几何不友好

- `fetch_nisn_mean` 很高
- `2Fetch` 成功率低
- joint-failure 很高
- 成功时扩宽幅度有限
- fetch 到 issue 有明显跌落

这是一个前端本来就很强，但当前 `2Fetch` 几何条件并不匹配的切片。

#### 7.2 `xalancbmk_8082`：最平衡的 `2Fetch` 成功案例

- `2Fetch` 成功率高
- 扩宽明显
- 几何压力较轻
- dispatch / issue 与 fetch 保持接近

这是三个新增切片中最干净的正面样例。

#### 7.3 `astar_biglakes_13437`：burst 很宽，但后段转化弱

- `2Fetch` 成功率中等
- 成功时 burst 极宽
- 几何困难明显
- fetch 到 issue 的跌落很大

这是一个 `2Fetch` 可以制造巨大前端 burst，但真正限制在后段吞吐的切片。

### 8. 主要结论

1. 三个切片都能频繁进入 `performInstructionFetch()`，没有哪个主要卡在 while 入口。
2. `bzip2_program_16771` 的主要限制是联合几何失败，而不是 fetch 活跃度不足。
3. `xalancbmk_8082` 是一个很强的正面 `2Fetch` 样例：成功率高、平衡好、能有效转化为 issue 带宽。
4. `astar_biglakes_13437` 能从 `2Fetch` 得到非常宽的 frontend burst，但大量收益在 issue 之前就损失掉了。
5. 这三个切片共同说明：`2Fetch` 的有效性取决于两个因素同时满足：
   - fetch 几何上是否允许 same-cycle stitching；
   - 下游流水线是否能消化更宽的 fetch stream。

### 9. 输出产物

- `docs/plans/2026-03-19-fetch-2fetch-three-slice-analysis.md`
- `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/perf.csv`
- `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/bzip2_program_16771/stats.txt`
- `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/xalancbmk_8082/stats.txt`
- `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/astar_biglakes_13437/stats.txt`
