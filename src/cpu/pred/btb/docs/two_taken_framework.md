# 2-Taken Framework

## Overview

The 2-Taken framework extends `DecoupledBPUWithBTB` so one prediction opportunity can produce:

- `block0`: the normal finalized prediction for the current fetch block
- `block1`: an optional next fetch block derived from the speculative post-state of `block0`

The fetch side is intentionally unchanged at the interface level. It still consumes ordinary single-block `FetchTarget` entries one by one. The framework only allows the predictor side to enqueue two consecutive targets earlier.

## Design Goal

This framework is designed for experiments, not for a fully generalized N-block predictor. The first implementation focuses on:

- keeping `FetchTarget` as the external unit of fetch, squash, and recovery
- generating at most two blocks per prediction
- allowing each predictor to decide independently whether it actively participates in `block1`
- allowing unsupported `block1` branch types to be filtered conservatively

## High-Level Architecture

### 1. External Model Stays Single-Block

The following structures remain single-block externally:

- `FetchTarget`
- `FetchTargetQueue`
- fetch-side consumption in `src/cpu/o3/fetch.cc`

The framework does not make fetch consume two targets in one cycle. Instead, the predictor may enqueue two targets in order.

### 2. Internal Bundle Model

`DecoupledBPUWithBTB` now has an internal bundle path:

- `SpecState`
  - `pc`
  - `history`
  - `phistory`
  - `bwhistory`
  - `lhistory`
- `PredictionBundle`
  - `pred0`
  - optional `pred1`
  - `stateAfter0`
  - `stateAfterFinal`
  - `pred1DropReason`

`pred1` is generated from `stateAfter0`, not from the original thread state.

### 3. Prediction Flow

The flow is:

1. Generate the normal final prediction for `block0`
2. Compute speculative next state after `block0`
3. Check top-level `block1` gates
4. If allowed, run predictor `block1` hooks using `stateAfter0`
5. Finalize `pred1`
6. Enqueue `target0`
7. If valid, enqueue `target1`
8. Commit thread speculative state once using `stateAfterFinal`

## Predictor Participation Model

Each `TimedBaseBTBPredictor` now has a `block1Participate` control.

Two modes are supported:

- `active`
  - the predictor runs its own `putPCHistoryForBlock1(...)`
- `pass-through`
  - the predictor does not actively predict `block1`
  - instead, it can preserve lower-stage information from `lowerPred`

### Current Implemented Behavior

- `UBTB`
  - active `block1` prediction path is implemented
- `TAGE`
  - active mode uses normal prediction path
  - passive mode copies `condTakens` and `tageInfoForMgscs` from `lowerPred`
- `ITTAGE`
  - passive mode copies `indirectTargets` from `lowerPred`
- `RAS`
  - passive mode copies `returnTarget` from `lowerPred`
- `MBTB`
  - passive mode copies `btbEntries` from `lowerPred`

## Top-Level Block1 Gating

Before `block1` is accepted, the bundle logic checks:

- `enableTwoTaken`
- `dropBlock1OnBlock0Override`
- `dropBlock1WhenFTQHasOnlyOneSlot`
- valid next PC after `block0`
- whether `block0` had an initial uBTB hit

Then the finalized `pred1` is filtered for unsupported branch classes:

- conditional branch without direction support
- indirect branch without indirect-target support
- return without RAS support

Support can come from either:

- active predictor participation, or
- copied support preserved from `lowerPred`

## Configuration Parameters

The framework adds the following top-level controls in `DecoupledBPUWithBTB`:

- `enableTwoTaken`
  - enable the two-block bundle path
- `dropBlock1OnBlock0Override`
  - drop `block1` when `block0` is overridden by a later predictor stage
- `dropBlock1WhenFTQHasOnlyOneSlot`
  - require at least two free FTQ slots before creating `block1`
- `dropBlock1OnCondWithoutTage`
  - reject `block1` conditional branches when no valid direction support exists
- `dropBlock1OnIndirectWithoutIttage`
  - reject `block1` indirect branches when no valid indirect-target support exists
- `dropBlock1OnReturnWithoutRas`
  - reject `block1` returns when no valid return target exists

Each predictor also inherits:

- `block1Participate`
  - whether that predictor actively predicts `block1`

## Files Touched by the Framework

Core logic:

- `src/cpu/pred/btb/decoupled_bpred.hh`
- `src/cpu/pred/btb/decoupled_bpred.cc`
- `src/cpu/pred/btb/common.hh`
- `src/cpu/pred/btb/timed_base_pred.hh`
- `src/cpu/pred/btb/timed_base_pred.cc`

Configuration surface:

- `src/cpu/pred/BranchPredictor.py`

Predictor hooks:

- `src/cpu/pred/btb/btb_ubtb.hh`
- `src/cpu/pred/btb/btb_ubtb.cc`
- `src/cpu/pred/btb/btb_tage.hh`
- `src/cpu/pred/btb/btb_ittage.hh`
- `src/cpu/pred/btb/ras.hh`
- `src/cpu/pred/btb/mbtb.hh`

Queue behavior:

- `src/cpu/pred/btb/ftq.hh`
- `src/cpu/pred/btb/ftq.cc`

## Statistics

The framework adds first-stage block1 statistics in `DBPBTBStats`:

- `block1Attempted`
- `block1Accepted`
- `block1DroppedByBlock0Override`
- `block1DroppedByCond`
- `block1DroppedByIndirect`
- `block1DroppedByReturn`
- `block1DroppedByFTQFull`
- `block1DroppedOther`

These counters are intended to support ablation studies and help explain performance changes.

## Tests Added

Current focused test coverage includes:

- `src/cpu/pred/btb/test/btb.test.cc`
  - block1 drop-reason helpers
  - copied-support acceptance cases
  - `UBTB` block1 prediction behavior
  - `MBTB` block1 pass-through behavior
- `src/cpu/pred/btb/test/btb_tage.test.cc`
  - `TAGE` block1 active path
  - `TAGE` block1 passive copy path
- `src/cpu/pred/btb/test/fetch_target_queue.test.cc`
  - two-target squash behavior
  - FTQ free-slot accounting for two-target admission

## Current Scope and Limitations

This implementation intentionally does not yet do the following:

- fetch consuming two targets in one cycle
- N-block generalization beyond two blocks
- full block1 specialization for every predictor component
- broad end-to-end recovery validation for every history source

The framework is intended to be usable for current 2-Taken experiments while leaving room for later refinement.

## Suggested Experimental Starting Points

Useful first configurations include:

1. `UBTB` active, all other predictors passive
2. `UBTB + TAGE` active, `ITTAGE/RAS` passive
3. `UBTB + TAGE + ITTAGE + RAS` active
4. `dropBlock1OnBlock0Override = true` vs `false`
5. conservative filtering on vs off for conditional / indirect / return cases

These settings should give a good first view of the latency/accuracy tradeoff introduced by the two-block framework.

---

## 中文版本

### 概述

2-Taken 框架对 `DecoupledBPUWithBTB` 做了扩展，使一次预测机会可以产出：

- `block0`：当前取指块的正常最终预测结果
- `block1`：基于 `block0` 推测后状态导出的一个可选后继取指块

取指侧接口保持不变。fetch 仍然逐个消费普通的单块 `FetchTarget`。这个框架做的事情，是让预测侧能够更早地顺序入队两个连续的 target。

### 设计目标

这个框架主要面向实验，而不是一个完全泛化的 N-block 预测框架。当前实现重点是：

- 保持 `FetchTarget` 仍然是 fetch、squash 和 recovery 的外部基本单位
- 一次预测最多只生成两个块
- 允许每个 predictor 独立决定自己是否主动参与 `block1`
- 当 `block1` 遇到不受支持的分支类型时，可以保守地将其过滤掉

### 整体架构

#### 1. 对外仍然保持单块模型

下面这些结构对外仍然是单块语义：

- `FetchTarget`
- `FetchTargetQueue`
- `src/cpu/o3/fetch.cc` 中的 fetch 消费逻辑

本框架并没有让 fetch 在一个周期里同时消费两个 target，只是允许预测侧按顺序更早地把两个 target 放入队列。

#### 2. 内部使用 bundle 模型

`DecoupledBPUWithBTB` 内部新增了 bundle 路径：

- `SpecState`
  - `pc`
  - `history`
  - `phistory`
  - `bwhistory`
  - `lhistory`
- `PredictionBundle`
  - `pred0`
  - 可选 `pred1`
  - `stateAfter0`
  - `stateAfterFinal`
  - `pred1DropReason`

其中 `pred1` 的生成输入来自 `stateAfter0`，而不是原始线程状态。

#### 3. 预测流程

整体流程如下：

1. 生成 `block0` 的正常最终预测
2. 计算 `block0` 之后的推测状态
3. 检查 `block1` 的顶层 gate
4. 如果允许，则基于 `stateAfter0` 运行各 predictor 的 `block1` hook
5. 形成最终 `pred1`
6. 入队 `target0`
7. 如果 `pred1` 有效，则再入队 `target1`
8. 用 `stateAfterFinal` 一次性提交线程推测状态

### Predictor 参与模型

每个 `TimedBaseBTBPredictor` 都新增了 `block1Participate` 控制位。

支持两种模式：

- `active`
  - predictor 主动执行自己的 `putPCHistoryForBlock1(...)`
- `pass-through`
  - predictor 不主动预测 `block1`
  - 而是从 `lowerPred` 中保留低层已经得到的信息

#### 当前已经实现的行为

- `UBTB`
  - 已实现真正的 `block1` active 预测路径
- `TAGE`
  - active 模式走正常预测路径
  - passive 模式复制 `lowerPred` 中的 `condTakens` 和 `tageInfoForMgscs`
- `ITTAGE`
  - passive 模式复制 `lowerPred` 中的 `indirectTargets`
- `RAS`
  - passive 模式复制 `lowerPred` 中的 `returnTarget`
- `MBTB`
  - passive 模式复制 `lowerPred` 中的 `btbEntries`

### 顶层 Block1 Gate

在接受 `block1` 之前，bundle 逻辑会检查：

- `enableTwoTaken`
- `dropBlock1OnBlock0Override`
- `dropBlock1WhenFTQHasOnlyOneSlot`
- `block0` 之后的 next PC 是否有效
- `block0` 是否存在初始 uBTB hit

之后还会根据 `pred1` 的最终内容，过滤不受支持的分支类型：

- 没有方向支持的条件分支
- 没有间接目标支持的 indirect 分支
- 没有返回地址支持的 return

这里的“支持”既可以来自：

- predictor 主动参与 `block1`
- 也可以来自从 `lowerPred` 中复制保留下来的 support

### 配置项

`DecoupledBPUWithBTB` 顶层新增了如下控制项：

- `enableTwoTaken`
  - 是否启用 two-block bundle 路径
- `dropBlock1OnBlock0Override`
  - 如果 `block0` 被更高阶段 override，是否丢弃 `block1`
- `dropBlock1WhenFTQHasOnlyOneSlot`
  - 当 FTQ 剩余空间不足两个条目时，是否丢弃 `block1`
- `dropBlock1OnCondWithoutTage`
  - 当 `block1` 中存在没有有效方向支持的条件分支时，是否丢弃
- `dropBlock1OnIndirectWithoutIttage`
  - 当 `block1` 中存在没有有效间接目标支持的 indirect 分支时，是否丢弃
- `dropBlock1OnReturnWithoutRas`
  - 当 `block1` 中存在没有有效 return target 的 return 时，是否丢弃

每个 predictor 还继承了：

- `block1Participate`
  - 决定该 predictor 是否主动参与 `block1` 预测

### 涉及的主要文件

核心逻辑：

- `src/cpu/pred/btb/decoupled_bpred.hh`
- `src/cpu/pred/btb/decoupled_bpred.cc`
- `src/cpu/pred/btb/common.hh`
- `src/cpu/pred/btb/timed_base_pred.hh`
- `src/cpu/pred/btb/timed_base_pred.cc`

配置定义：

- `src/cpu/pred/BranchPredictor.py`

Predictor hook：

- `src/cpu/pred/btb/btb_ubtb.hh`
- `src/cpu/pred/btb/btb_ubtb.cc`
- `src/cpu/pred/btb/btb_tage.hh`
- `src/cpu/pred/btb/btb_ittage.hh`
- `src/cpu/pred/btb/ras.hh`
- `src/cpu/pred/btb/mbtb.hh`

队列相关：

- `src/cpu/pred/btb/ftq.hh`
- `src/cpu/pred/btb/ftq.cc`

### 统计项

`DBPBTBStats` 中新增了第一版 block1 统计项：

- `block1Attempted`
- `block1Accepted`
- `block1DroppedByBlock0Override`
- `block1DroppedByCond`
- `block1DroppedByIndirect`
- `block1DroppedByReturn`
- `block1DroppedByFTQFull`
- `block1DroppedOther`

这些统计项主要用于实验归因，帮助分析性能变化来自哪里。

### 已有测试覆盖

当前已有的聚焦测试包括：

- `src/cpu/pred/btb/test/btb.test.cc`
  - block1 drop-reason helper
  - copied-support 接受路径
  - `UBTB` 的 block1 预测行为
  - `MBTB` 的 block1 pass-through 行为
- `src/cpu/pred/btb/test/btb_tage.test.cc`
  - `TAGE` 的 block1 active 路径
  - `TAGE` 的 block1 passive copy 路径
- `src/cpu/pred/btb/test/fetch_target_queue.test.cc`
  - 双 target squash 语义
  - two-target 准入时 FTQ 剩余空间的计数

### 当前范围与限制

当前实现有意没有进一步扩展到：

- fetch 在一个周期里同时消费两个 target
- 超过两个块的 N-block 泛化
- 每个 predictor 的完整 block1 特化实现
- 所有历史源的端到端恢复验证

因此，这个框架的定位是：已经可以支撑当前 2-Taken 实验，但仍然保留后续细化空间。

### 建议的实验起点

建议优先尝试以下配置组合：

1. 仅 `UBTB` active，其余 predictor passive
2. `UBTB + TAGE` active，`ITTAGE/RAS` passive
3. `UBTB + TAGE + ITTAGE + RAS` active
4. 对比 `dropBlock1OnBlock0Override = true` 与 `false`
5. 对比条件分支 / indirect / return 的保守过滤开关开闭

这些配置有助于比较 two-block 框架带来的延迟收益和准确率损失之间的权衡。
