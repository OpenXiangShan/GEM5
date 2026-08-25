# Kunminghu BTB-TAGE 设计说明

## 1. 文档范围

本文档说明 Kunminghu v3 中 `BTBTAGE` 的核心设计思路，重点回答：

- 为什么传统单分支 TAGE 的组织方式在多分支 fetch block 下不再够用
- 为什么当前实现中 `index` 不带 `branchPC/position`
- 为什么 `tag` 要带 `position`
- 为什么当前设计采用 `2-way`
- 为什么当前的 allocation / `useful` 管理围绕“同一 fetch block 内多 branch 共存”展开

本文档不覆盖：

- `MicroTAGE`
- `BTBTAGEUpperBound`
- `ITTAGE`
- 顶层 target selection / override 逻辑

这些应由单独文档说明。

## 2. 问题背景

传统 TAGE 的默认问题设定，是“每次输入一条 branch，输出这条 branch 的方向预测结果”。

无论是经典论文、CBP 框架还是 ChampSim 一类模拟器，基本都在这个语境下讨论 TAGE：

- 预测粒度是单条 branch
- 索引和 tag 都天然围绕这条 branch 的 `branchPC` 展开
- 每拍只需要解决“这条 branch 去哪个 entry 查”这个问题

Kunminghu v3 的 `BTBTAGE` 不是这个问题。

在 Kunminghu v3 中，TAGE 需要服务的是一个 fetch block，而不是单条 branch。一个 fetch block 中最多可以容纳多条 branch。这样一来，问题就变成了：

- 同一拍中，一个 fetch block 内多条 branch 共享同一个 `startPC`
- 它们也共享同一份 `PHR` folded history
- 但预测器仍然必须尽量区分这些 branch，而不是把它们当成同一条 branch

这才是 `BTBTAGE` 的核心设计难点。

### 2.1 TAGE 原理速记

为了便于不熟悉 TAGE 的读者建立最基本的直觉，这里只保留最必要的一层原理说明。

TAGE 的核心思想是：

- 不同历史长度的表分别捕捉不同长度的历史模式
- 较短历史表负责较常见、较局部的模式
- 较长历史表负责依赖更长上下文的模式
- 预测时，从高表往低表找命中的 entry
- 最终通常由“最长历史的命中项”作为 main provider，再配一个较低历史的 alt provider 作为后备

所以，传统单分支 TAGE 的关键问题是：

- 这条 branch 在不同历史长度下分别会不会命中
- 命中后由哪张表提供最终预测

Kunminghu v3 的难点在于，它不是对“一条 branch”重复这个过程，而是要在一拍内对同一个 fetch block 里的多条 branch 同时做这件事。

### 2.2 当前 BTB-TAGE v3 草图

下面这张图可以帮助理解当前 BTB-TAGE v3 中 `startPC`、`PHR folded history`、`mBTB` 和最终 `position/tag` 之间的大致时序关系：

![BTB-TAGE v3 草图](../images/BPU-TAGE.jpeg)

这张图最值得关注的不是每个框的名字，而是两个时序事实：

- 在较早阶段，TAGE 只能稳定拿到 `startPC + PHR folded history`
- 到 `mBTB` 结果出来后，才真正知道 block 内有哪些 branch，以及它们的 `position`

这正是后面 “index 不带 branchPC，而 tag 带 position” 的根本背景。

## 3. 为什么传统 FTB 时代的问题更简单

Kunminghu v2 的 FTB 架构下，一个 fetch block 内最多 `2` 条 branch。这个问题虽然也不是纯单分支，但仍然相对简单。

原因是：

- block 内 branch 数量很小
- 可以比较自然地用“两份 SRAM / 类似两路”的方式去覆盖这两条 branch
- block 内 branch 之间的竞争还没有严重到需要重新思考整个 TAGE 的组织方式

而 Kunminghu v3 的目标是：

- 更大的 fetch block
- 更高的 block 内 branch 容纳能力

一旦一个 fetch block 内最多可能有 `8` 条 branch，问题就不再能靠“把原来两条分支的结构线性扩成八条”来解决了。

这并不是简单把 TAGE 做成 `8-way` 就完事，因为：

- 时序会更难
- 存储和访问结构会更重
- 更关键的是，当前设计中 branch 的很多关键信息并不是在 index 计算那一拍就已经可用

## 4. 当前实现里 index 为什么不带 branchPC

这是当前设计里最关键的一点。

在 Kunminghu v3 的流水里，TAGE 的 index 计算必须尽量早做，以满足时序要求。参考当前 BTB-TAGE v3 草图和实现，可以把这个过程简化理解为：

- 在较早阶段，已经知道 fetch block 的 `startPC`
- 同时已经有 `PHR` folded history
- 但 `mBTB` 结果尚未完全出来，因此具体是哪几条 branch、它们在 block 内的 `position` 是多少，还没有最终确定

所以在这个时点上，TAGE 只能稳定使用：

- `startPC`
- folded history

来计算 index。

这就是为什么当前实现里：

- `getTageIndex()` 只使用 `startPC + indexFoldedHist`
- 不使用具体 `branchPC`

从实现上也能直接看到这一点：`generateSinglePrediction()` 中计算 index 时，传入的是 `startPC`，而不是 `btb_entry.pc`。

这不是一个“更优雅”的选择，而是时序约束下的结果。

## 5. 如果 index 不带 branchPC，会出现什么问题

一旦 index 不带 `branchPC`，问题就立刻出现了。

对同一个 fetch block 来说：

- 所有 branch 共享同一个 `startPC`
- 所有 branch 共享同一个 folded history 快照

那么在某一张给定历史长度的表上，它们天然会落到同一个 index。

也就是说，Kunminghu v3 的 `BTBTAGE` 面对的不是普通的“branch aliasing”，而是一个更具体的问题：

- 同一个 fetch block 内多条 branch，会天然共享同一个 index

如果没有额外机制，这些 branch 就几乎不可能在同一张表中共存。

## 6. 为什么 tag 要带 position

虽然 index 无法带 `branchPC`，但 tag 计算发生得更晚，可以在 `mBTB` 给出 branch 信息后再补入 branch 的块内位置。

因此当前实现采用的方式是：

- index 只由 `startPC + folded history` 决定
- tag 在 folded history 基础上，再 XOR `position`

这里的 `position` 本质上就是 branch 在当前 fetch block 内的相对位置。

从代码看：

- `position = getBranchIndexInBlock(btb_entry.pc, startPC)`
- `getTageTag(..., position)` 最终把 `position` XOR 进 tag

这样做有两个直接效果：

1. 同一 fetch block 内的不同 branch，即便落到同一个 index，也不会天然拥有相同 tag
2. 不需要额外存储一份独立的 `position` 字段再做二次比较

所以当前实现中，并不是“branchPC 完全没有参与 TAGE 识别”，而是它没有参与 index，只通过块内 `position` 的形式进入了 tag。

## 7. 为什么当前设计采用 2-way

仅仅让 tag 带 `position` 还不够。

如果同一个 fetch block 内的多条 branch 共享同一个 index，而一个 set 里只有一路，那么不同 branch 即便 tag 不同，也无法共存。

这就是当前 `BTBTAGE` 采用 `2-way` 的根本原因。

这里的 `2-way` 不应理解成一个泛泛的“提高 associativity、提升容量”的普通 cache 优化。对 `BTBTAGE` 来说，它更具体的职责是：

- 在同一个 index 下，为同一个 fetch block 内的不同 branch 提供最基本的共存空间

也就是说，当前 `2-way` 的意义首先不是“让任意两个不相关 branch 更少冲突”，而是：

- 让共享同一 `startPC`
- 共享同一 folded history
- 但 `position` 不同的 branch

至少有可能在同一张表中同时存在。

这也是为什么当前设计没有试图简单地把它做成“天然 8-way”：

- 真正的问题不是普通单分支 TAGE 的容量不够
- 而是多分支 fetch block 下，同 index 多 branch 的组织和时序都变得更难

## 8. 当前设计真正依赖的分化机制

当前 `BTBTAGE` 并不是指望“在一张表里把一个 fetch block 的所有 branch 都塞开”。它更现实的依赖三种分化机制：

### 8.1 position 进入 tag

这是最直接的一层区分，让不同 branch 不至于完全不可分。

### 8.2 2-way 提供同 index 下的有限共存空间

这是最基本的物理承载空间。

### 8.3 不同历史长度的表让不同 branch 自然分化

这是最关键的一层设计直觉。

当前设计希望看到的情况不是：

- 一个 fetch block 内所有 branch 都在同一层表里竞争

而更接近：

- 较容易预测的 branch 停留在较低历史长度表中
- 较难预测、需要更长上下文的 branch 往更高历史长度表走

这样，同一个 fetch block 内不同 branch 的压力可以沿“历史长度维度”被部分分散，而不只是都堆到同一层表、同一个 index、同一个 set 的两个 ways 上竞争。

这也是为什么当前的 allocation / `useful` 管理会显得特别强调“不要轻易替换已经证明自己有价值的 provider”。

## 9. 为什么 allocation / useful 设计成现在这样

如果把 `BTBTAGE` 当成普通单分支 TAGE，会觉得当前很多策略有点奇怪。

但在“同一个 fetch block 内多条 branch 共享同一个 index”的背景下，这些策略就比较自然了。

### 9.1 Allocation 不是简单追求“有 miss 就分配”

当前 allocation 的优先级是：

1. `invalid way`
2. `weak && !useful`
3. 任意 `!useful`

这表示当前设计并不愿意为了新 branch 轻易破坏已经稳定工作的 entry。因为一旦误伤已有 provider，就可能让原本已经分化好的不同 branch 重新发生冲突。

当低表 `ti` 存在候选 way 时，当前实现按距离依次比较 `ti + 1` 和 `ti + 2`。第一个候选优先级严格高于 `ti` 的高表会被选中，因此当两张高表都优于 `ti` 时优先在更近的 `ti + 1` allocation；只有 `ti + 1` 不优于 `ti` 时才可能选择 `ti + 2`。高表候选与 `ti` 并列或更低时保留低表。当前表没有候选时，继续原有的低到高扫描和 failure/reset 记账。

### 9.2 最高表 provider 不再继续向上分配

如果 provider 已经来自最高历史长度表，再向更高表分配已经没有意义。当前实现直接抑制这种分配。

### 9.3 provider 弱但方向正确时，也不急着分配

如果 provider 虽然弱，但方向已经对了，当前实现更倾向于认为：

- 这个 entry 需要的是继续训练
- 而不是立刻把它推到更长历史表

这在多分支 fetch block 语境下尤其重要，因为盲目分配会让有限 ways 和表项更快被新的 branch 占满，反而加重 ping-pong。

### 9.4 useful 的职责是保护已经证明自己的 provider

当前 `useful` 不做局部 decrement，只在 provider 相对 alt 证明自己正确时置 1，并在全局阈值触发时统一 reset。

这背后的设计重点不是“做一套精细的 per-entry aging”，而是尽量简洁地回答一个问题：

- 这个 entry 是否已经在当前多分支竞争环境里证明自己值得被保留

一旦 `useful` 被置起，allocation 就更难把它替换掉。

## 10. 当前方案的限制

当前 `BTBTAGE` 不是一个完美解。

它本质上是在严格时序约束下，对多分支 fetch block TAGE 做的一组工程折中。至少有以下限制：

- index 不带 `branchPC`，同一 fetch block 内多 branch 共享 index 的问题不会消失
- `2-way` 只能提供有限的共存空间，不可能天然承载一个 fetch block 内全部分支
- 不同 branch 向不同历史表自然分化，更接近一种设计目标和设计直觉，而不是可以在所有 workload 上严格保证的性质
- 当前 tag 只通过 XOR `position` 区分 branch，这是一种节省存储的折中，而不是信息最完备的表示

换句话说，当前实现不是“把传统单分支 TAGE 原封不动搬到 fetch block 上”，而是在承认这个迁移本身没有标准答案的前提下，给出一个能跑、能训、时序可接受的组织方式。

## 11. 为什么 16B / 更高 associativity 不是当前主线

理论上，当然还可以考虑更多替代方向，例如：

- 让 block 组织更细
- 引入更高 associativity
- 在 index 中引入更多 branch-specific 信息

但这些方向当前都不是主线。原因不是它们一定错误，而是：

- 时序成本不明确
- 存储成本可能明显上升
- 需要重新平衡“早期 index 计算”和“后期 branch position 可用”的关系

所以当前 `BTBTAGE` 的重点不是追求某个理论上最干净的结构，而是在现有流水约束下把多分支 fetch block 预测这件事做成。

## 12. 设计总结

`BTBTAGE` 当前设计可以概括成五句话：

1. 传统 TAGE 默认面向单条 branch 预测，而 Kunminghu v3 要解决的是单个 fetch block 内多条 branch 的同拍预测。
2. 在当前流水里，index 计算时只有 `startPC + folded history` 可用，`branchPC/position` 还不可用，因此 index 不带 branch-specific 信息。
3. 为了区分同一 fetch block 内不同 branch，当前实现把 `position` XOR 到 tag 中。
4. 为了让这些共享同一个 index 的 branch 至少有有限共存空间，当前设计采用 `2-way`。
5. allocation / `useful` / provider 选择等策略，本质上都在努力让不同 branch 尽量沿不同历史表或同表不同 way 分化，而不是反复互相挤掉。

所以 `BTBTAGE` 的难点，不在于“把传统 TAGE 再实现一遍”，而在于：

- 把“单分支 predictor”
- 迁移到“多分支 fetch block predictor”
- 同时还要满足时序约束

## 13. 实现锚点

当前最相关的实现文件有：

- `src/cpu/pred/btb/btb_tage.hh`
- `src/cpu/pred/btb/btb_tage.cc`
- `src/cpu/pred/BranchPredictor.py`
- `src/cpu/pred/btb/test/btb_tage.test.cc`

## 14. 参考资料

- `docs/design-docs/frontend/bpu_top_level.md`
- `docs/design-docs/frontend/mbtb_design.md`
- `docs/design-docs/images/BPU-TAGE.jpeg`
