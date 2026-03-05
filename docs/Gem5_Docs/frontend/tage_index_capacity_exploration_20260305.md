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
