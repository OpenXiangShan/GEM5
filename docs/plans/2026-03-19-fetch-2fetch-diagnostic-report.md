# 2Taken/2Fetch Fetch-Side Diagnostic Report

## English

### 1. Purpose

This report documents a focused fetch-side diagnostic experiment for the current gem5 `idealkmhv3` configuration. The goal is to explain, with direct counters, why some slices benefit from `2Taken/2Fetch` while others do not, and to answer three specific questions:

1. How often does fetch enter the main `performInstructionFetch()` while loop, and why does it fail to enter?
2. Once fetch is active, how often does a second in-cycle fetch (`2Fetch`) happen, and why does it fail when it does not happen?
3. In cycles with only one fetch versus cycles with successful `2Fetch`, how many instructions are fetched on average?

The experiment was run on two slices chosen for contrast:

- `mcf_12253`
- `gcc_200_5849`

The first is a low-IPC, backend-heavy slice where earlier analysis suggested weak `2Fetch` benefit. The second is a GCC slice where earlier analysis suggested meaningful frontend benefit from short-block stitching.

### 2. Code Changes

The diagnostic instrumentation was added in three places:

- `GEM5/src/cpu/o3/fetch.hh`
- `GEM5/src/cpu/o3/fetch.cc`
- `DataProcess/targets/branch.yaml`

#### 2.1 Added fetch-side counters

The following counters were added to gem5 fetch statistics:

**While-loop entry and non-entry diagnostics**

- `performIFCalls`
- `performIFWhileEntered`
- `performIFWhileIterations`
- `performIFWhileNotEnteredFetchWidth`
- `performIFWhileNotEnteredFetchQueueFull`
- `performIFWhileNotEnteredStopFetch`
- `performIFWhileNotEnteredFtqEmpty`
- `performIFWhileNotEnteredWaitForVsetvl`

**2Fetch opportunity and failure diagnostics**

- `twoFetchOpportunity`
- `twoFetchTaken`
- `twoFetchNotTakenNotPredTaken`
- `twoFetchNotTakenDisabled`
- `twoFetchNotTakenNoNext`
- `twoFetchNotTakenTargetMismatch`
- `twoFetchNotTakenSpanTooLarge`
- `twoFetchNotTakenTargetNotInBuffer`

**Single-fetch versus double-fetch cycle effectiveness**

- `singleFetchCycleCount`
- `singleFetchCycleInsts`
- `doubleFetchCycleCount`
- `doubleFetchCycleInsts`

#### 2.2 Added derived metrics in DataProcess

The following derived metrics were added in `DataProcess/targets/branch.yaml`:

- `singleFetchInstsPerCycle`
- `doubleFetchInstsPerCycle`
- `performIFWhileEnterRate`
- `performIFWhileItersPerEntered`
- `twoFetchTakeRate`

These metrics allow direct slice-level analysis from `perf.csv` without re-parsing raw `stats.txt` every time.

### 3. Build and Run Procedure

#### 3.1 Build

gem5 was rebuilt with the repository's preferred parallel build style:

```bash
cd GEM5
scons build/RISCV/gem5.opt --gold-linker -j60
```

The build completed successfully.

#### 3.2 Runtime details

The run command followed the information in `build-system/GEM5.just`, especially the checkpoint restorer path:

- `--gcpt-restorer /nfs/share/gem5_ci/tools/normal-gcb-restorer.bin`
- `--difftest-ref-so /nfs/home/wangrui/OpenXiangShan/XSWorkbench/NEMU/build/riscv64-nemu-interpreter-so`

The two runs were produced under:

- `out/gem5/single-2026-03-19-2taken-2fetch-diag/mcf_12253`
- `out/gem5/single-2026-03-19-2taken-2fetch-diag/gcc_200_5849`

Counters were extracted into:

- `out/gem5/single-2026-03-19-2taken-2fetch-diag/perf.csv`

### 4. Experimental Results

The extracted rows are in `out/gem5/single-2026-03-19-2taken-2fetch-diag/perf.csv:2` and `out/gem5/single-2026-03-19-2taken-2fetch-diag/perf.csv:3`.

#### 4.1 Summary table

| Slice | IPC | fetch_nisn_mean | dispatchRateMean | issueRate | while enter rate | 2Fetch take rate | singleFetchInstsPerCycle | doubleFetchInstsPerCycle |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `gcc_200_5849` | 4.124 | 4.783 | 4.454 | 4.053 | 81.3% | 61.0% | 5.132 | 12.427 |
| `mcf_12253` | 0.730 | 4.878 | 3.493 | 2.753 | 78.0% | ~0% | 6.713 | 16.0* |

`*` The `mcf_12253` double-fetch average is not representative because it is based on only 2 successful double-fetch cycles.

### 5. Question 1: How often does fetch enter the main while loop?

#### 5.1 `gcc_200_5849`

From `out/gem5/single-2026-03-19-2taken-2fetch-diag/gcc_200_5849/stats.txt:12596`:

- `performIFCalls = 4,739,813`
- `performIFWhileEntered = 3,853,184`
- `performIFWhileNotEnteredFetchQueueFull = 886,629`

This gives:

- while entered rate: `3,853,184 / 4,739,813 = 81.3%`
- not-entered-due-to-fetchQueueFull rate: `18.7%`

Interpretation:

- Fetch is active most of the time.
- The dominant reason for not entering the while loop is not lack of FTQ work, not stop-fetch state, and not vector configuration; it is `fetchQueueFull`.
- This means the fetch side is already aggressive enough that the next-stage queue often becomes the immediate gate.

#### 5.2 `mcf_12253`

From `out/gem5/single-2026-03-19-2taken-2fetch-diag/mcf_12253/stats.txt:12148`:

- `performIFCalls = 26,777,461`
- `performIFWhileEntered = 20,893,195`
- `performIFWhileNotEnteredFetchQueueFull = 5,884,266`

This gives:

- while entered rate: `78.0%`
- not-entered-due-to-fetchQueueFull rate: `22.0%`

Interpretation:

- The same primary gating pattern appears here: the main reason for not entering the loop is `fetchQueueFull`.
- Therefore, `mcf_12253` is not failing simply because fetch frequently has no FTQ target.
- Its main problem lies later: either in what happens inside fetch, or in whether a useful second fetch can actually be formed.

### 6. Question 2: How often does `2Fetch` happen, and why does it fail?

### 6.1 `gcc_200_5849`

From `out/gem5/single-2026-03-19-2taken-2fetch-diag/gcc_200_5849/stats.txt:12600`:

- `twoFetchOpportunity = 6,331,693`
- `twoFetchTaken = 3,862,767`
- `twoFetchNotTakenNotPredTaken = 137,207`
- `twoFetchNotTakenNoNext = 1,047,659`
- `twoFetchNotTakenSpanTooLarge = 640,980`
- `twoFetchNotTakenTargetNotInBuffer = 643,080`

Derived breakdown:

- success: `61.0%`
- fail because stream did not end with predicted taken branch: `2.2%`
- fail because next stream unavailable: `16.5%`
- fail because combined span too large: `10.1%`
- fail because target not covered by current fetch buffer: `10.2%`

Interpretation:

- `2Fetch` is a normal and frequent event in this slice.
- The mechanism is not marginal here; it succeeds in a majority of eligible opportunities.
- The biggest failure reason is not predictor direction quality. It is supply/geometry related:
  - next stream not ready,
  - merged span too large,
  - target not inside the current buffer window.

This strongly suggests that `gcc_200_5849` is a slice where `2Fetch` materially contributes to wider effective fetch bursts.

### 6.2 `mcf_12253`

From `out/gem5/single-2026-03-19-2taken-2fetch-diag/mcf_12253/stats.txt:12152`:

- `twoFetchOpportunity = 19,308,509`
- `twoFetchTaken = 2`
- `twoFetchNotTakenNotPredTaken = 6,069,339`
- `twoFetchNotTakenNoNext = 380,913`
- `twoFetchNotTakenSpanTooLarge = 3,543,947`
- `twoFetchNotTakenTargetNotInBuffer = 9,314,308`

Derived breakdown:

- success: effectively `0%`
- fail because stream did not end with predicted taken branch: `31.4%`
- fail because next stream unavailable: `2.0%`
- fail because combined span too large: `18.4%`
- fail because target not covered by current fetch buffer: `48.2%`

Interpretation:

- `2Fetch` almost never succeeds in this slice.
- The dominant failure mode is not `NoNext`; the next entry usually exists.
- The dominant blockers are:
  - `target not in buffer`
  - `span too large`
- This means the control-flow shape and fetch-buffer geometry prevent useful in-cycle stitching.

This result is much stronger than saying "`mcf` does not benefit much from 2Fetch." It says that, for this slice, `2Fetch` is almost structurally inapplicable.

### 7. Question 3: How many instructions are fetched in single-fetch versus 2Fetch cycles?

### 7.1 `gcc_200_5849`

From `out/gem5/single-2026-03-19-2taken-2fetch-diag/gcc_200_5849/stats.txt:12606`:

- `singleFetchCycleCount = 1,554,306`
- `singleFetchCycleInsts = 7,977,092`
- `singleFetchCycleInstMean = 5.132`
- `doubleFetchCycleCount = 1,224,635`
- `doubleFetchCycleInsts = 15,219,044`
- `doubleFetchCycleInstMean = 12.427`

Interpretation:

- A successful double-fetch cycle fetches more than twice as many instructions as a single-fetch cycle on average.
- This directly confirms that `2Fetch` is not just toggling a control path; it materially increases instantaneous frontend width.
- Since `fetch_nisn_mean` is only `4.783`, these wide cycles are being diluted by zero-fetch cycles and other constrained cycles, but the burst benefit is real and large.

### 7.2 `mcf_12253`

From `out/gem5/single-2026-03-19-2taken-2fetch-diag/mcf_12253/stats.txt:12158`:

- `singleFetchCycleCount = 19,911,683`
- `singleFetchCycleInsts = 133,665,244`
- `singleFetchCycleInstMean = 6.713`
- `doubleFetchCycleCount = 2`
- `doubleFetchCycleInsts = 32`
- `doubleFetchCycleInstMean = 16.0`

Interpretation:

- The measured `doubleFetchInstsPerCycle = 16.0` is not meaningful as a stable average because there are only 2 such cycles.
- The real insight is the opposite: nearly all useful fetch work happens in single-fetch cycles.
- `mcf_12253` is not a slice where double-fetch behavior materially shapes overall throughput.

### 8. Relationship to Dispatch and Issue Bandwidth

The new fetch counters become much more meaningful when compared with `dispatchRateMean` and `issueRate`.

#### 8.1 `gcc_200_5849`

- `fetch_nisn_mean = 4.783`
- `dispatchRateMean = 4.454`
- `issueRate = 4.053`

Interpretation:

- Frontend average supply is slightly higher than dispatch average, and dispatch average is slightly higher than issue average.
- This is the expected shape when the frontend is effective but downstream bandwidth and queueing still absorb part of the gain.
- In other words, `2Fetch` is helping, but it is not the only determinant of final IPC.

#### 8.2 `mcf_12253`

- `fetch_nisn_mean = 4.878`
- `dispatchRateMean = 3.493`
- `issueRate = 2.753`

Interpretation:

- The frontend fetch average is not extremely low.
- The larger drop happens after fetch, especially by the time instructions reach issue.
- Therefore, this slice has two simultaneous problems:
  1. `2Fetch` nearly never forms successfully.
  2. Even when fetch keeps moving, the backend consumes far less than fetch supplies.

### 9. Root-Cause Comparison

### 9.1 Why `gcc_200_5849` can benefit

`gcc_200_5849` shows a textbook "useful 2Fetch" pattern:

- the fetch loop enters frequently,
- `2Fetch` succeeds often,
- double-fetch cycles are much wider than single-fetch cycles,
- average issue remains below fetch, so not all gain converts to IPC, but the frontend mechanism is clearly doing productive work.

The main remaining losses are structural rather than conceptual:

- fetch queue full,
- next stream not yet ready,
- span and fetch-buffer coverage constraints.

### 9.2 Why `mcf_12253` does not benefit much

`mcf_12253` does **not** mainly fail because the frontend is asleep or because no next stream exists. Instead:

- the fetch loop still enters often,
- the next stream is usually available,
- but the target frequently falls outside the current fetch buffer window,
- and the merged span often exceeds the per-cycle fetch-span limit.

So the dominant explanation is geometric/structural incompatibility with the current `2Fetch` conditions, not merely insufficient predictor aggressiveness.

This is an important distinction:

- `gcc_200_5849`: "`2Fetch` works and helps, then downstream limits reduce full payoff."
- `mcf_12253`: "`2Fetch` almost never becomes legal in the first place, and downstream limitations exist on top of that."

### 10. Key Conclusions

1. In both slices, the main reason `performInstructionFetch()` does not enter its main while loop is `fetchQueueFull`, not FTQ emptiness.
2. `gcc_200_5849` is a genuine `2Fetch`-friendly slice: the `2Fetch` success rate is `61.0%`, and successful double-fetch cycles fetch `12.43` instructions on average versus `5.13` in single-fetch cycles.
3. `mcf_12253` is almost structurally hostile to `2Fetch`: out of `19.3M` opportunities, only `2` succeed.
4. The primary blockers for `mcf_12253` are `target not in buffer` and `span too large`, not `NoNext`.
5. `mcf_12253` also has much weaker backend consumption than frontend supply, so even a better frontend would not automatically produce proportional IPC gain.
6. The experiment therefore separates two classes of slices:
   - slices where `2Fetch` is valid and useful but later stages limit total gain,
   - slices where `2Fetch` is almost never valid under current fetch-buffer/span constraints.

### 11. Practical Implications

If the next step is to improve `2Taken/2Fetch`, this experiment suggests two different optimization directions:

#### 11.1 For slices like `gcc_200_5849`

Focus on converting successful frontend widening into sustained throughput:

- reduce fetch queue pressure,
- improve downstream acceptance,
- examine decode/rename/dispatch backpressure.

#### 11.2 For slices like `mcf_12253`

Focus on why legal `2Fetch` formation is so rare:

- fetch-buffer coverage,
- target-in-buffer condition,
- per-cycle merged-span restriction,
- exact cross-block geometry at run-out points.

### 12. Important Caveat

The current instrumentation only counts the specific reasons already exposed in the present implementation path. In the two completed runs:

- `twoFetchNotTakenDisabled`
- `twoFetchNotTakenTargetMismatch`

did not appear in `stats.txt`, which means they were not exercised in these two slices rather than proving they are impossible globally.

### 13. Second-Round Deep Dive: Is `target not in buffer` a forward overflow or a backward jump?

After the first diagnostic round, one ambiguity remained: the earlier `target-not-in-buffer` statistic did not distinguish whether the branch target was beyond the **right edge** of the current fetch buffer or whether it jumped **backward to an address before the current buffer start**. Likewise, `span-too-large` could reflect a real long forward span or an unsigned-underflow-style backward case.

To resolve this, a second round of counters was added and the experiment was rerun on five slices:

- `mcf_12253`
- `gcc_200_5849`
- `bzip2_program_16771`
- `xalancbmk_8082`
- `astar_biglakes_13437`

#### 13.1 Additional counters added in the second round

The second round added the following directional counters:

- `twoFetchTargetBeforeBuffer`
- `twoFetchTargetAfterBuffer`
- `twoFetchNextStreamForward`
- `twoFetchNextStreamBackward`
- `twoFetchSpanForwardOrZero`
- `twoFetchSpanBackward`
- `twoFetchTargetDistanceBeforeBufferStart`
- `twoFetchBackwardSpanDistance`
- `twoFetchBackwardNextStartDistance`

These counters let us distinguish whether a failure is dominated by:

- a forward extension that exceeds the buffer window,
- a backward jump whose target lies to the left of the current buffer,
- a truly large span,
- or a backward next-stream relationship.

The rerun results were extracted to:

- `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/perf.csv`

#### 13.2 Key comparative table

| Slice | 2Fetch take rate | target-before rate | target-after rate | next-stream-backward rate | span-backward rate |
| --- | ---: | ---: | ---: | ---: | ---: |
| `mcf_12253` | ~0% | 48.29% | 0.0013% | 48.29% | 0.054% |
| `gcc_200_5849` | 61.01% | 14.33% | 4.64% | 39.41% | 4.17% |
| `bzip2_program_16771` | 15.74% | 30.52% | 33.96% | 30.52% | 28.79% |
| `xalancbmk_8082` | 59.79% | 14.28% | 8.97% | 14.28% | 8.66% |
| `astar_biglakes_13437` | 32.83% | 30.84% | 22.53% | 30.84% | 17.20% |

All rates above are normalized by `twoFetchOpportunity`.

#### 13.3 Refined conclusion for `mcf_12253`

The second round gives a much sharper answer for `mcf_12253`.

From `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/perf.csv`:

- `twoFetchTakeRate` is effectively zero
- `twoFetchTargetBeforeBufferRate = 48.29%`
- `twoFetchTargetAfterBufferRate = 0.0013%`
- `twoFetchNextStreamBackwardRate = 48.29%`
- `twoFetchSpanBackwardRate = 0.054%`
- `twoFetchFailOnlyTargetNotInBufferRate = 48.24%`
- `twoFetchFailBothSpanAndTargetNotInBufferRate = 0.055%`

This means:

1. The dominant `target-not-in-buffer` mode is **not** a forward overflow.
2. The target is almost always **before** the current fetch buffer, not after it.
3. The next stream is very often **backward** relative to the current stream start.
4. `span-too-large` is almost never coupled with `target-not-in-buffer` in this slice.

So the refined root cause for `mcf_12253` is:

- `2Fetch` fails mainly because many opportunities correspond to **backward-taken control flow** whose next stream begins to the left of the current fetch buffer window.
- This is fundamentally different from a forward chaining problem where one only needs a larger fetch window or slightly larger max span.

In short: for `mcf_12253`, `target-not-in-buffer` should be read as **"backward target outside the current buffer window"**, not **"forward target extends beyond the right edge"**.

#### 13.4 Interpretation of the other slices

##### `gcc_200_5849`

- `twoFetchTakeRate = 61.01%`
- `target-before rate = 14.33%`
- `target-after rate = 4.64%`
- `next-stream-backward rate = 39.41%`
- `span-backward rate = 4.17%`

Interpretation:

- Backward next streams are common, but they do not dominate the way they do in `mcf_12253`.
- Most backward next-stream cases do **not** immediately turn into span-backward failures.
- This slice still supports successful `2Fetch` in a majority of opportunities, which is why the frontend mechanism remains highly productive here.

##### `bzip2_program_16771`

- `twoFetchTakeRate = 15.74%`
- `target-before rate = 30.52%`
- `target-after rate = 33.96%`
- `next-stream-backward rate = 30.52%`
- `span-backward rate = 28.79%`
- `twoFetchFailBothSpanAndTargetNotInBufferRate = 62.75%`

Interpretation:

- This slice is not dominated by a single direction.
- It has substantial backward behavior **and** substantial forward-out-of-buffer behavior.
- The strongest pattern is the high joint-failure rate: `span-too-large` and `target-not-in-buffer` frequently happen together.
- This looks more like a general geometric mismatch between stream layout and current `2Fetch` constraints.

##### `xalancbmk_8082`

- `twoFetchTakeRate = 59.79%`
- `target-before rate = 14.28%`
- `target-after rate = 8.97%`
- `next-stream-backward rate = 14.28%`
- `span-backward rate = 8.66%`

Interpretation:

- This slice is much more balanced and much more `2Fetch`-friendly.
- Neither backward-target nor forward-overflow behavior dominates enough to suppress `2Fetch` broadly.
- The final performance result is consistent with a slice where `2Fetch` remains broadly legal and useful.

##### `astar_biglakes_13437`

- `twoFetchTakeRate = 32.83%`
- `target-before rate = 30.84%`
- `target-after rate = 22.53%`
- `next-stream-backward rate = 30.84%`
- `span-backward rate = 17.20%`

Interpretation:

- `astar_biglakes_13437` sits between `mcf` and `xalancbmk`.
- Backward next-stream opportunities are common, but not as overwhelmingly dominant as in `mcf`.
- It still has a meaningful successful `2Fetch` fraction, but a large portion of opportunities are geometrically difficult.

#### 13.5 What the second round changes in our understanding

The first round said:

- `mcf_12253` fails mainly because of `target-not-in-buffer`, with some `span-too-large`

The second round refines this to:

- `mcf_12253` fails mainly because its next stream is often **backward**, and the backward target falls **before the current fetch buffer window**

This is a much stronger and more actionable conclusion.

It means that simply enlarging the right-side fetch window is unlikely to solve the main problem for `mcf_12253`. The real issue is that the current same-cycle stitching rule is naturally favorable to forward continuation, but many `mcf` opportunities want to jump backward relative to the current buffer anchor.

#### 13.6 Practical implication from the second round

There are now two clearly different classes of `2Fetch` challenges:

1. **Forward geometric pressure**
   - target goes beyond the current right edge
   - merged span exceeds the forward byte budget

2. **Backward-target pressure**
   - next stream is backward relative to current stream start
   - target lies before the current fetch buffer window

`mcf_12253` is overwhelmingly of the second type.

This suggests that any future optimization targeted at `mcf` should consider whether same-cycle stitching can support backward-taken transitions more naturally, rather than only relaxing forward span limits.

### 14. Output Artifacts

Main result files:

- `docs/plans/2026-03-19-fetch-2fetch-diagnostic-report.md`
- `out/gem5/single-2026-03-19-2taken-2fetch-diag/perf.csv`
- `out/gem5/single-2026-03-19-2taken-2fetch-diag/gcc_200_5849/stats.txt`
- `out/gem5/single-2026-03-19-2taken-2fetch-diag/mcf_12253/stats.txt`
- `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/perf.csv`
- `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/gcc_200_5849/stats.txt`
- `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/mcf_12253/stats.txt`
- `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/bzip2_program_16771/stats.txt`
- `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/xalancbmk_8082/stats.txt`
- `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/astar_biglakes_13437/stats.txt`

---

## Chinese Translation / 中文翻译

### 1. 目的

本报告记录了一次针对当前 gem5 `idealkmhv3` 配置的 fetch 侧诊断实验。目标是通过直接计数器解释：为什么有些切片能从 `2Taken/2Fetch` 中获益，而另一些不能；并回答以下三个问题：

1. fetch 在 `performInstructionFetch()` 的主 while 循环中到底进入了多少次，没进入时的原因是什么？
2. 在 fetch 已经开始工作后，第二次同周期取指（`2Fetch`）发生了多少次；若没有发生，失败原因是什么？
3. 在“只有一次 fetch 的周期”和“发生了成功 2Fetch 的周期”中，平均每周期分别取到多少条指令？

本次实验选取了两个具有对比性的切片：

- `mcf_12253`
- `gcc_200_5849`

前者是一个低 IPC、后端压力较重的切片，先前分析中已显示其 `2Fetch` 收益较弱；后者是一个 GCC 切片，先前分析中显示它可能能够从短 block 拼接中显著受益。

### 2. 代码改动

本次诊断插桩涉及三个位置：

- `GEM5/src/cpu/o3/fetch.hh`
- `GEM5/src/cpu/o3/fetch.cc`
- `DataProcess/targets/branch.yaml`

#### 2.1 新增的 fetch 侧计数器

新增的 gem5 fetch 统计包括：

**while 循环进入与未进入诊断**

- `performIFCalls`
- `performIFWhileEntered`
- `performIFWhileIterations`
- `performIFWhileNotEnteredFetchWidth`
- `performIFWhileNotEnteredFetchQueueFull`
- `performIFWhileNotEnteredStopFetch`
- `performIFWhileNotEnteredFtqEmpty`
- `performIFWhileNotEnteredWaitForVsetvl`

**2Fetch 机会与失败原因诊断**

- `twoFetchOpportunity`
- `twoFetchTaken`
- `twoFetchNotTakenNotPredTaken`
- `twoFetchNotTakenDisabled`
- `twoFetchNotTakenNoNext`
- `twoFetchNotTakenTargetMismatch`
- `twoFetchNotTakenSpanTooLarge`
- `twoFetchNotTakenTargetNotInBuffer`

**single fetch 与 double fetch 周期效果对比**

- `singleFetchCycleCount`
- `singleFetchCycleInsts`
- `doubleFetchCycleCount`
- `doubleFetchCycleInsts`

#### 2.2 在 DataProcess 中新增的派生指标

在 `DataProcess/targets/branch.yaml` 中新增了以下派生指标：

- `singleFetchInstsPerCycle`
- `doubleFetchInstsPerCycle`
- `performIFWhileEnterRate`
- `performIFWhileItersPerEntered`
- `twoFetchTakeRate`

这些派生指标使我们能够直接从 `perf.csv` 做切片级分析，而不必每次都重新读取原始 `stats.txt`。

### 3. 编译与运行流程

#### 3.1 编译

gem5 按仓库推荐方式并行编译：

```bash
cd GEM5
scons build/RISCV/gem5.opt --gold-linker -j60
```

编译成功完成。

#### 3.2 运行细节

运行命令参考了 `build-system/GEM5.just` 中的配置，尤其是 checkpoint restorer 路径：

- `--gcpt-restorer /nfs/share/gem5_ci/tools/normal-gcb-restorer.bin`
- `--difftest-ref-so /nfs/home/wangrui/OpenXiangShan/XSWorkbench/NEMU/build/riscv64-nemu-interpreter-so`

两个切片的结果目录为：

- `out/gem5/single-2026-03-19-2taken-2fetch-diag/mcf_12253`
- `out/gem5/single-2026-03-19-2taken-2fetch-diag/gcc_200_5849`

抽取得到的总 CSV 为：

- `out/gem5/single-2026-03-19-2taken-2fetch-diag/perf.csv`

### 4. 实验结果

抽取后的两行结果位于：

- `out/gem5/single-2026-03-19-2taken-2fetch-diag/perf.csv:2`
- `out/gem5/single-2026-03-19-2taken-2fetch-diag/perf.csv:3`

#### 4.1 结果总表

| Slice | IPC | fetch_nisn_mean | dispatchRateMean | issueRate | while 进入率 | 2Fetch 成功率 | singleFetchInstsPerCycle | doubleFetchInstsPerCycle |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `gcc_200_5849` | 4.124 | 4.783 | 4.454 | 4.053 | 81.3% | 61.0% | 5.132 | 12.427 |
| `mcf_12253` | 0.730 | 4.878 | 3.493 | 2.753 | 78.0% | 约 0% | 6.713 | 16.0* |

`*` `mcf_12253` 的 `doubleFetchInstsPerCycle=16.0` 并没有统计代表性，因为它只来自 2 个成功的 double-fetch 周期。

### 5. 问题一：fetch 主 while 循环进入了多少次？

#### 5.1 `gcc_200_5849`

根据 `out/gem5/single-2026-03-19-2taken-2fetch-diag/gcc_200_5849/stats.txt:12596`：

- `performIFCalls = 4,739,813`
- `performIFWhileEntered = 3,853,184`
- `performIFWhileNotEnteredFetchQueueFull = 886,629`

对应得到：

- while 进入率：`81.3%`
- 因 `fetchQueueFull` 而未进入的比例：`18.7%`

解释：

- fetch 大部分时间都能正常进入主循环。
- 没进入 while 的主要原因不是 FTQ 没工作，不是 stop-fetch 状态，也不是向量配置阻塞，而是 `fetchQueueFull`。
- 这意味着该切片上的前端其实已经相当积极，下一阶段队列更像是当前第一层门槛。

#### 5.2 `mcf_12253`

根据 `out/gem5/single-2026-03-19-2taken-2fetch-diag/mcf_12253/stats.txt:12148`：

- `performIFCalls = 26,777,461`
- `performIFWhileEntered = 20,893,195`
- `performIFWhileNotEnteredFetchQueueFull = 5,884,266`

对应得到：

- while 进入率：`78.0%`
- 因 `fetchQueueFull` 而未进入的比例：`22.0%`

解释：

- 这里出现了和 `gcc_200_5849` 相同的主导模式：没进入 while 的主要原因也是 `fetchQueueFull`。
- 因此，`mcf_12253` 的问题不能简单理解成“fetch 经常没有 FTQ target”。
- 它的主要问题要么发生在 fetch 内部后续阶段，要么发生在“第二次 fetch 能不能形成”这一层。

### 6. 问题二：`2Fetch` 发生了多少次，失败原因是什么？

#### 6.1 `gcc_200_5849`

根据 `out/gem5/single-2026-03-19-2taken-2fetch-diag/gcc_200_5849/stats.txt:12600`：

- `twoFetchOpportunity = 6,331,693`
- `twoFetchTaken = 3,862,767`
- `twoFetchNotTakenNotPredTaken = 137,207`
- `twoFetchNotTakenNoNext = 1,047,659`
- `twoFetchNotTakenSpanTooLarge = 640,980`
- `twoFetchNotTakenTargetNotInBuffer = 643,080`

换算后：

- 成功率：`61.0%`
- 因“流末尾不是 predicted taken branch”失败：`2.2%`
- 因下一 stream 不可用失败：`16.5%`
- 因 span 太大失败：`10.1%`
- 因 target 不在当前 fetch buffer 覆盖范围内失败：`10.2%`

解释：

- `2Fetch` 在这个切片上是一个正常且频繁发生的事件。
- 这里的机制不是边缘性的，而是在多数可评估机会中都能真正成功。
- 最大的失败原因并不是预测方向质量，而是供给/几何条件：
  - next stream 尚未准备好，
  - 合并 span 超过限制，
  - target 没有落在当前 buffer 窗口内。

这强烈说明 `gcc_200_5849` 是一个 `2Fetch` 的真实受益切片。

#### 6.2 `mcf_12253`

根据 `out/gem5/single-2026-03-19-2taken-2fetch-diag/mcf_12253/stats.txt:12152`：

- `twoFetchOpportunity = 19,308,509`
- `twoFetchTaken = 2`
- `twoFetchNotTakenNotPredTaken = 6,069,339`
- `twoFetchNotTakenNoNext = 380,913`
- `twoFetchNotTakenSpanTooLarge = 3,543,947`
- `twoFetchNotTakenTargetNotInBuffer = 9,314,308`

换算后：

- 成功率：几乎 `0%`
- 因“流末尾不是 predicted taken branch”失败：`31.4%`
- 因下一 stream 不可用失败：`2.0%`
- 因 span 太大失败：`18.4%`
- 因 target 不在当前 fetch buffer 覆盖范围内失败：`48.2%`

解释：

- `2Fetch` 在这个切片上几乎从不成功。
- 主导失败原因不是 `NoNext`；也就是说 next entry 大多数时候其实是存在的。
- 真正的主阻碍是：
  - `target not in buffer`
  - `span too large`

因此，这个结果比“`mcf` 对 2Fetch 收益小”更强：它说明对于这个切片，`2Fetch` 在结构上几乎不满足成立条件。

### 7. 问题三：single fetch 周期与 2Fetch 周期的平均取指数是多少？

#### 7.1 `gcc_200_5849`

根据 `out/gem5/single-2026-03-19-2taken-2fetch-diag/gcc_200_5849/stats.txt:12606`：

- `singleFetchCycleCount = 1,554,306`
- `singleFetchCycleInsts = 7,977,092`
- `singleFetchCycleInstMean = 5.132`
- `doubleFetchCycleCount = 1,224,635`
- `doubleFetchCycleInsts = 15,219,044`
- `doubleFetchCycleInstMean = 12.427`

解释：

- 成功发生 `2Fetch` 的周期，平均取指宽度超过 single-fetch 周期的两倍。
- 这直接说明 `2Fetch` 并不只是改变了一条控制路径，而是真实地显著扩大了瞬时 frontend 宽度。
- 虽然整体 `fetch_nisn_mean` 只有 `4.783`，这些宽 burst 会被 0-fetch 周期或受限周期摊薄，但它们的 burst 收益是真实且很大的。

#### 7.2 `mcf_12253`

根据 `out/gem5/single-2026-03-19-2taken-2fetch-diag/mcf_12253/stats.txt:12158`：

- `singleFetchCycleCount = 19,911,683`
- `singleFetchCycleInsts = 133,665,244`
- `singleFetchCycleInstMean = 6.713`
- `doubleFetchCycleCount = 2`
- `doubleFetchCycleInsts = 32`
- `doubleFetchCycleInstMean = 16.0`

解释：

- 这里的 `doubleFetchInstsPerCycle = 16.0` 不具有稳定统计意义，因为只有 2 个样本。
- 真正有意义的结论恰好相反：几乎所有有效 fetch 工作都发生在 single-fetch 周期中。
- `mcf_12253` 不是一个由 double-fetch 行为主导整体吞吐的切片。

### 8. 与 Dispatch / Issue 带宽的关系

把新的 fetch 计数器和 `dispatchRateMean`、`issueRate` 放在一起看，意义会更清楚。

#### 8.1 `gcc_200_5849`

- `fetch_nisn_mean = 4.783`
- `dispatchRateMean = 4.454`
- `issueRate = 4.053`

解释：

- frontend 平均供给略高于 dispatch 平均宽度，而 dispatch 又略高于 issue 平均宽度。
- 这是一个非常合理的形态：前端确实有效，但下游带宽和队列压力仍然吃掉了一部分收益。
- 换句话说，`2Fetch` 的确在帮忙，但它不是最终 IPC 的唯一决定因素。

#### 8.2 `mcf_12253`

- `fetch_nisn_mean = 4.878`
- `dispatchRateMean = 3.493`
- `issueRate = 2.753`

解释：

- 这个切片的 frontend 平均取指并不算特别低。
- 更大的跌落发生在 fetch 之后，尤其是在 issue 阶段之前。
- 因此，这个切片实际上有两个叠加问题：
  1. `2Fetch` 几乎根本形成不起来；
  2. 即使 fetch 还能继续推进，后端实际消费能力也远低于 fetch 供给能力。

### 9. 根因对比

#### 9.1 为什么 `gcc_200_5849` 能受益

`gcc_200_5849` 呈现出典型的“`2Fetch` 有效”模式：

- fetch 主循环经常进入，
- `2Fetch` 经常成功，
- double-fetch 周期明显比 single-fetch 周期更宽，
- 平均 issue 虽仍低于 average fetch，说明不是所有收益都能变成 IPC，但前端机制本身确实在产生有效工作。

它的主要剩余损失更偏结构性：

- fetch queue full，
- next stream 尚未准备好，
- span 和 fetch-buffer 覆盖限制。

#### 9.2 为什么 `mcf_12253` 受益不大

`mcf_12253` **并不是** 主要因为“前端太弱”或者“没有 next stream”才失败。相反：

- fetch 主循环仍然经常进入，
- next stream 通常也是存在的，
- 但 target 经常落在当前 fetch buffer 窗口之外，
- 同时合并后的 span 也经常超过每周期允许的 fetch-span 限制。

所以，更可信的主解释是：当前 `2Fetch` 的几何/结构条件与该切片的控制流形态不兼容，而不仅仅是预测器不够积极。

这一区别很重要：

- `gcc_200_5849`：`2Fetch` 能工作并且有收益，只是后续流水线限制了最终收益上限。
- `mcf_12253`：`2Fetch` 在现有条件下几乎从来不合法，后端问题则是在此之上进一步叠加。

### 10. 关键结论

1. 在两个切片中，`performInstructionFetch()` 不进入主 while 的首要原因都是 `fetchQueueFull`，而不是 FTQ 空。
2. `gcc_200_5849` 是一个真正的 `2Fetch` 友好切片：成功率 `61.0%`，且成功 double-fetch 周期平均可取 `12.43` 条指令，而 single-fetch 周期平均只有 `5.13`。
3. `mcf_12253` 几乎在结构上不适合 `2Fetch`：`19.3M` 次机会中只有 `2` 次成功。
4. `mcf_12253` 的主要阻碍是 `target not in buffer` 和 `span too large`，而不是 `NoNext`。
5. `mcf_12253` 还存在明显的后端消费不足，因此即使前端进一步改进，也未必会线性转化为 IPC 提升。
6. 因此，这次实验把切片清晰地区分成两类：
   - 一类是 `2Fetch` 合法且有效，但后段限制了总收益；
   - 一类是当前 fetch-buffer/span 条件下 `2Fetch` 几乎从不合法。

### 11. 工程启示

如果下一步要继续改进 `2Taken/2Fetch`，本实验提示了两条不同方向：

#### 11.1 对于 `gcc_200_5849` 这类切片

重点应放在如何把前端已经做出来的宽 burst 转化为持续吞吐：

- 降低 fetch queue 压力，
- 提高下游接收能力，
- 进一步检查 decode/rename/dispatch 背压。

#### 11.2 对于 `mcf_12253` 这类切片

重点应放在为什么合法 `2Fetch` 形成得这么少：

- fetch-buffer 覆盖范围，
- `target-in-buffer` 条件，
- 每周期 merged-span 限制，
- run-out 点上的精确跨块几何关系。

### 12. 重要说明

当前插桩只统计了现有实现路径中已经显式暴露出的失败原因。在这两个已完成的切片中：

- `twoFetchNotTakenDisabled`
- `twoFetchNotTakenTargetMismatch`

没有出现在 `stats.txt` 中。这意味着它们在这两个切片中没有被触发，而不能据此推出它们在全局范围内不可能发生。

### 14. 第二轮深挖：`target not in buffer` 到底是向前越界，还是向后回跳？

在第一轮诊断之后，还剩下一个关键歧义：之前的 `target-not-in-buffer` 统计并不能区分 branch target 是超出了当前 fetch buffer 的**右边界**，还是回跳到了当前 buffer 起点**左边**。同理，`span-too-large` 也可能是真正的前向跨度过大，也可能是 backward 情况下的无符号差值放大。

为解决这个问题，第二轮又增加了一批方向性更明确的计数器，并在五个切片上重跑：

- `mcf_12253`
- `gcc_200_5849`
- `bzip2_program_16771`
- `xalancbmk_8082`
- `astar_biglakes_13437`

#### 14.1 第二轮新增计数器

第二轮新增的方向性计数器包括：

- `twoFetchTargetBeforeBuffer`
- `twoFetchTargetAfterBuffer`
- `twoFetchNextStreamForward`
- `twoFetchNextStreamBackward`
- `twoFetchSpanForwardOrZero`
- `twoFetchSpanBackward`
- `twoFetchTargetDistanceBeforeBufferStart`
- `twoFetchBackwardSpanDistance`
- `twoFetchBackwardNextStartDistance`

这些计数器使我们能够明确区分：

- 是向前扩展时超出了 buffer 窗口，
- 还是向后跳回了 buffer 左边，
- 是真正的跨度太大，
- 还是 backward next-stream 关系导致的结构不兼容。

第二轮结果抽取到：

- `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/perf.csv`

#### 14.2 核心对比表

| Slice | 2Fetch 成功率 | target-before 比例 | target-after 比例 | next-stream-backward 比例 | span-backward 比例 |
| --- | ---: | ---: | ---: | ---: | ---: |
| `mcf_12253` | 约 0% | 48.29% | 0.0013% | 48.29% | 0.054% |
| `gcc_200_5849` | 61.01% | 14.33% | 4.64% | 39.41% | 4.17% |
| `bzip2_program_16771` | 15.74% | 30.52% | 33.96% | 30.52% | 28.79% |
| `xalancbmk_8082` | 59.79% | 14.28% | 8.97% | 14.28% | 8.66% |
| `astar_biglakes_13437` | 32.83% | 30.84% | 22.53% | 30.84% | 17.20% |

上表中的比例都相对于 `twoFetchOpportunity` 归一化。

#### 14.3 对 `mcf_12253` 的精炼结论

第二轮给出了对 `mcf_12253` 更明确的答案。

根据 `out/gem5/single-2026-03-19-2taken-2fetch-deepdive/perf.csv`：

- `twoFetchTakeRate` 几乎为零
- `twoFetchTargetBeforeBufferRate = 48.29%`
- `twoFetchTargetAfterBufferRate = 0.0013%`
- `twoFetchNextStreamBackwardRate = 48.29%`
- `twoFetchSpanBackwardRate = 0.054%`
- `twoFetchFailOnlyTargetNotInBufferRate = 48.24%`
- `twoFetchFailBothSpanAndTargetNotInBufferRate = 0.055%`

这意味着：

1. `target-not-in-buffer` 的主导模式并不是向前越界。
2. target 几乎总是落在当前 fetch buffer **左侧**，而不是右侧。
3. next stream 相对于当前 stream 起点经常是 **backward** 的。
4. `span-too-large` 在这个切片里几乎不会与 `target-not-in-buffer` 一起成为主导失败模式。

因此，`mcf_12253` 的精炼根因是：

- `2Fetch` 失败的主因，是大量机会对应的都是**向后跳转控制流**，而这些 backward target 落在当前 fetch buffer 窗口左侧。
- 这和单纯的“前向链式扩展不够长”是完全不同的问题。

换句话说：对 `mcf_12253` 来说，`target-not-in-buffer` 应理解为**“向后跳回，target 落在当前 buffer 左边”**，而不是**“向前取太宽，冲出右边界”**。

#### 14.4 其他切片的解释

##### `gcc_200_5849`

- `twoFetchTakeRate = 61.01%`
- `target-before rate = 14.33%`
- `target-after rate = 4.64%`
- `next-stream-backward rate = 39.41%`
- `span-backward rate = 4.17%`

解释：

- backward next-stream 虽然常见，但不像 `mcf_12253` 那样形成压倒性主导。
- 大多数 backward next-stream 情况并不会立刻变成 span-backward 失败。
- 这个切片仍然能在多数机会中成功进行 `2Fetch`，因此该机制在这里仍然是高产出的。

##### `bzip2_program_16771`

- `twoFetchTakeRate = 15.74%`
- `target-before rate = 30.52%`
- `target-after rate = 33.96%`
- `next-stream-backward rate = 30.52%`
- `span-backward rate = 28.79%`
- `twoFetchFailBothSpanAndTargetNotInBufferRate = 62.75%`

解释：

- 这个切片不是单一方向主导。
- 它同时存在明显的 backward 行为，也存在明显的 forward-out-of-buffer 行为。
- 最强特征是高 joint-failure：`span-too-large` 和 `target-not-in-buffer` 经常一起发生。
- 这更像是 stream 布局和当前 `2Fetch` 约束之间存在普遍几何不匹配。

##### `xalancbmk_8082`

- `twoFetchTakeRate = 59.79%`
- `target-before rate = 14.28%`
- `target-after rate = 8.97%`
- `next-stream-backward rate = 14.28%`
- `span-backward rate = 8.66%`

解释：

- 这是一个更加平衡、也更 `2Fetch`-友好的切片。
- 无论是 backward-target 还是 forward-overflow，都没有强到足以广泛压制 `2Fetch`。
- 其最终性能表现也和“`2Fetch` 仍然大体合法且有效”的判断一致。

##### `astar_biglakes_13437`

- `twoFetchTakeRate = 32.83%`
- `target-before rate = 30.84%`
- `target-after rate = 22.53%`
- `next-stream-backward rate = 30.84%`
- `span-backward rate = 17.20%`

解释：

- `astar_biglakes_13437` 处在 `mcf` 和 `xalancbmk` 之间。
- backward next-stream 很常见，但没有像 `mcf` 那样压倒性主导。
- 它仍有相当可观的成功 `2Fetch` 比例，但很多机会在几何上依然比较困难。

#### 14.5 第二轮如何改变我们的理解

第一轮的结论是：

- `mcf_12253` 主要失败在 `target-not-in-buffer`，并伴随少量 `span-too-large`

第二轮把它精炼成：

- `mcf_12253` 主要失败在 **backward next stream**，而 backward target 落在当前 fetch buffer 左侧

这比第一轮结论更强，也更可操作。

它说明：单纯增大右侧 fetch window，未必能解决 `mcf_12253` 的主问题。真正的问题在于：当前 same-cycle stitching 规则天然更适合前向延伸，而 `mcf` 的很多机会都要求相对于当前 buffer anchor 向后跳转。

#### 14.6 第二轮带来的工程启示

现在可以把 `2Fetch` 的困难更清晰地分成两类：

1. **前向几何压力**
   - target 超出当前右边界
   - merged span 超过前向字节预算

2. **向后跳转压力**
   - next stream 相对当前 stream start 是 backward 的
   - target 落在当前 fetch buffer 左侧

`mcf_12253` 明显属于第二类。

这意味着，如果后续要专门优化 `mcf` 类切片，应考虑 same-cycle stitching 是否能更自然地支持 backward-taken 转移，而不只是继续放宽前向 span 限制。

### 15. 输出产物

主要结果文件如下：

- `docs/plans/2026-03-19-fetch-2fetch-diagnostic-report.md`
- `out/gem5/single-2026-03-19-2taken-2fetch-diag/perf.csv`
- `out/gem5/single-2026-03-19-2taken-2fetch-diag/gcc_200_5849/stats.txt`
- `out/gem5/single-2026-03-19-2taken-2fetch-diag/mcf_12253/stats.txt`
