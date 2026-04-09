# 对齐 gem5 与 RTL 的 PHR target 更新语义

## 背景与目标

当前在评审 PR #814（`Fix path history info target`）时，发现 gem5 旧实现中
`FullBTBPrediction::getTarget()` 与 `getPHistInfo()` 对 indirect/return target 的处理不一致。
PR 将两者统一为同一套 target 解析逻辑。

但这个修复是否“符合 RTL”仍需单独验证。这里的核心问题不是
“PHR 是否必须使用真实 target”，而是：

- XiangShan RTL 中 path history/PHR 更新时，实际使用的是哪一个 target
- 这个 target 是 BTB entry 原始 target，还是经过 override 后的最终预测 target
- gem5 当前 PR 的行为是否在语义上与 RTL 一致

本任务的目标是给出基于代码证据的结论，而不是仅凭经验判断。

## 当前已知信息

- gem5 预测后更新 path history 的入口在
  `src/cpu/pred/btb/decoupled_bpred.cc`，通过 `finalPred.getPHistInfo()` 取得
  `(pc, target, taken)` 后调用 `pHistShiftIn(...)`。
- PR #814 之前，`getPHistInfo()` 直接使用 `entry.target`；
  `getTarget()` 则会对 indirect target 和 return target 做 override。
- 因此 gem5 旧实现存在“最终预测 target”和“PHR 使用 target”不一致的可能。
- 已确认 XiangShan RTL 的 PHR 更新显式依赖 `(cfiPc, target)` 的 path hash，而不是仅依赖 PC。

## 假设与待验证问题

该部分已完成验证。初始假设中的两个分支里，最终结论如下：

1. RTL 不要求 PHR 在推测更新时必须等于 backend 最终真实执行 target。
2. 但 RTL 也不是“随便使用一个近似 target”即可；它要求 PHR 使用当前真正驱动 fetch 前进的 target。
3. 如果后续出现更晚阶段的 override 或 backend redirect，RTL 会基于新的 target 对 PHR 做修正。

## 计划步骤

1. 阅读 XiangShan `frontend/bpu/tage` 相关 Scala 实现，确认 `tage` 仅消费 folded PHR，不直接维护 PHR。
2. 追踪 `frontend/bpu/history/phr` 中的 PHR 更新逻辑，确认更新使用 `pathHash(cfiPc, target)`。
3. 回到 `frontend/bpu/Bpu.scala`，确认 `s1_prediction.target`、`s3_prediction.target` 与 `redirect.bits.target` 都是“当前生效的 fetch target”，包含 RAS / ITTAGE / override 路径。
4. 对照 gem5 的 `getPHistInfo()`、`getTarget()` 路径，判断 PR #814 是否与 RTL 一致。
5. 补充 PR 的 `gcc12-spec06-0.8c` 性能数据分析，确认收益方向是否与该类修复的预期一致。

## 验证方式

- 在 RTL 中找到 path history 更新代码及其输入来源。
- 能明确回答“PHR 更新使用的 target 是什么”。
- 能把该结论映射回 gem5 PR #814 的具体代码修改点。
- 如有现成 CI 数据，验证性能变化是否主要体现在 conditional-path 学习质量相关指标上。

## 结论

### RTL 语义

- XiangShan `tage` 自身只读取 folded PHR，入口位于
  `frontend/bpu/tage/Tage.scala` 的 `io.fromPhr.foldedPathHist` /
  `foldedPathHistForTrain`。
- 真正维护 PHR 的模块是
  `frontend/bpu/history/phr/Phr.scala`。
- RTL 在 `Phr.scala` 中用 `pathHash(updateCfiPc, updateTarget)` 更新 PHR；
  因此 PHR 的更新输入明确包含 target。
- `updateTarget` 的来源优先级为：
  `redirect > s3_override > s1_valid`。
- 这些 target 并不是某个静态 BTB entry target：
  - `s1_prediction.target` 会在 return 场景下被 uRAS override；
  - `s3_prediction.target` 会在 return 场景下被 RAS override，在其他 indirect 场景下可被 ITTAGE override；
  - `redirect.bits.target` 则来自 backend/redirect 的修正结果。

### 对 gem5 PR #814 的判断

- gem5 旧实现的问题，不是“PHR 没有使用真实 target”，而是“PHR 没有使用当前最终生效的预测 target”。
- 旧代码中 `getTarget()` 已经会对 indirect / return target 做 override，
  但 `getPHistInfo()` 仍直接使用 `entry.target`。
- 这会导致 fetch 实际沿着 override 后的 target 前进，而 PHR 却按未 override 的 target 更新。
- XiangShan RTL 的行为明显要求 PHR 跟随当前生效的 fetch target，而不是允许它与 fetch path 脱节。
- 因此，PR #814 将 `getPHistInfo()` 与 `getTarget()` 统一到同一套 target 解析逻辑，是与 RTL 语义一致的修复。

### 性能结果

- PR 已合入。评审过程中使用 `gcc12-spec06-0.8c` 的 Ideal BTB 性能数据做了对比分析。
- 使用 `python3 run.py <archive> --slice gcc12` 对 PR run 与主线 Ideal BTB baseline 做对比后，结果为：
  - Int score / GHz：`20.6907 -> 20.8375`，`+0.71%`
  - Total branch wrong MPKI：`4.9768 -> 4.8501`
  - Conditional branch MPKI：`4.8504 -> 4.7279`
- 主要收益集中在 `gobmk`、`sjeng`、`gcc`，且更明显地体现在 conditional-path 相关错误下降上。
- indirect / return 自身的聚合 MPKI 变化很小，这与“PHR/path history 一致性修复主要改善后续条件分支学习质量”的预期一致。

## 进度

- [x] 2026-04-09 12:10 确认该问题属于 gem5/RTL 行为对齐，需要单独记录执行计划。
- [x] 2026-04-09 12:12 复核 gem5 中 `getTarget()` 与 `getPHistInfo()` 的旧差异。
- [x] 2026-04-09 12:30 阅读 XiangShan RTL 中 `tage`、`history/phr` 与 `Bpu.scala` 路径，确认 PHR 的 target 来源与 update 优先级。
- [x] 2026-04-09 12:40 给出 gem5 PR #814 与 RTL 的一致性结论。
- [x] 2026-04-09 14:10 使用 `gem5_data_proc/run.py --slice gcc12` 分析 PR 的 `gcc12-spec06-0.8c` 数据，并将结论评论到 PR。
- [x] 2026-04-09 14:20 PR 已合入，执行计划转移到 `completed/`。

## 发现与意外情况

- 当前用户提出了一个关键反问：PHR 使用的 target 不一定必须等于真实 target。
  这说明评审不能只看 gem5 内部“是否自洽”，还必须核对 RTL 的实际设计语义。
- GitHub 上部分 `Manual Performance Test` run 的显示元信息会混入 `xs-dev` 头信息，但实际 perf job checkout 的 commit 可能不同。
  这次分析中最终以 archive 目录内的 `metadata.txt` 为准，避免错误选取 baseline。
