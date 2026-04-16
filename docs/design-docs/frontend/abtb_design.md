# Kunminghu AheadBTB 设计说明

## 1. 文档范围

本文档说明 Kunminghu v3 中 `AheadBTB` 的设计定位和几个核心取舍，重点回答：

- 为什么需要 `AheadBTB`
- 为什么它采用 ahead pipeline
- 为什么它虽然名字叫 BTB，但存储语义更接近 FTB 风格
- 为什么它仍然可能出现 alias 和 way 不足问题

本文档不展开：

- `MicroTAGE`
- `uBTB`
- `mBTB`
- 顶层 override / target selection 细节

这些应由单独文档说明。

## 2. 基本定位

如果只保留最粗的一层理解，可以把 `AheadBTB` 看成：

- 位于 `S1` 的较大容量 target predictor
- 目标是在 `uBTB` 之上，补一层“更大但仍然足够快”的早期预测来源

它和 `uBTB`、`mBTB` 的关系可以粗略理解成：

- `uBTB`：最早、最快，但容量很小
- `AheadBTB`：仍然很早，但容量明显更大
- `mBTB`：更大、更完整，但结果更晚

所以 `AheadBTB` 的角色不是替代 `mBTB`，而是尽量在 `S1` 阶段提供比 `uBTB` 更强的 target 覆盖能力，从而减少后续 override bubble。

## 3. 为什么需要 AheadBTB

Kunminghu v3 中，最终较重的预测逻辑已经后移到更晚阶段。这样做虽然解决了更大 fetch block 下的时序问题，但也让早期预测与最终预测之间的距离被拉长。

这意味着：

- 如果早期预测太弱
- 后续阶段就会更频繁地 override 它
- 前端 bubble 代价会明显上升

`AheadBTB` 的存在，就是为了在 `uBTB` 和后级 `mBTB` 之间再补一层更有容量的早期 target 预测。

因此，`AheadBTB` 的第一职责是：

- 尽量在更早阶段提供更多 branch 候选和 target 信息
- 减少完全依赖后级 `mBTB` 才能修正的情况

当前经验收益大致在 `0.2 分/GHz` 量级。

## 4. 为什么要用 ahead pipeline

`AheadBTB` 和 `uBTB` 的关键区别之一，是容量更大。

当前配置大致是：

- `1K` entries
- `8-way`

这比 `uBTB` 更大，但也带来直接代价：它已经难以像 `uBTB` 那样在同一拍内完成普通的索引访问和匹配。

因此，`AheadBTB` 采用了 ahead pipeline。

### 4.1 核心机制

它的基本思想是把“读 set”和“tag compare”拆开到相邻两拍：

- 上一拍：用上一拍的 `startPC` 生成 index，并把对应 set 读出来
- 这一拍：用这一拍的 `startPC` 生成 tag，与上一拍读出的 set 做匹配
- 这一拍产出的结果，用于预测“下一拍的 fetch block”

从代码实现上看，这件事体现在：

- `lookupSingleBlock()` 先把当前 `block_pc` 对应的 set 压进 `aheadReadBtbEntries`
- 队列填满后，再取出之前那拍缓存下来的 set
- 用当前拍 `block_pc` 的 tag 去匹配之前那拍的 set

所以，它不是普通的“当前拍 index、当前拍 tag、当前拍命中”，而是明确跨拍分工。

### 4.2 这意味着什么

这意味着 `AheadBTB` 并不是一个普通 BTB 的简单缩小版，而是一个建立在前后两拍关联之上的预测结构。

它的快，不是来自结构极小，而是来自：

- 先把 SRAM 访问超前铺开
- 再在当前拍只做 tag compare 和结果拼装

## 5. 为什么 AheadBTB 的存储语义更接近 FTB

虽然名字叫 `AheadBTB`，而且代码也是从 `MBTB` 复制演化而来，但它的存储语义其实并不真正像 `mBTB`。

原因很简单：

- `mBTB` 以对齐块为基本组织单位，更像“按地址空间切块存储”
- `AheadBTB` 的索引不是基于 `32B aligned PC`

从实现上看：

- `AheadBTB` 的 `idxShiftAmt = 1`
- `getIndex(pc)` 直接对 `startPC >> 1` 取 index
- 查找时也直接用 `startPC` 参与索引和 tag，不做 `32B aligned` 的 half-aligned 查询

因此，`AheadBTB` 更接近 Kunminghu v2 的 FTB 语义：

- 一个 fetch block 以它自己的 `startPC` 为核心被组织和存储
- 同一个 branch 如果出现在不同 `startPC` 对应的 fetch block 里，就可能被存到不同 entry 中

这就是它相对严格 BTB 架构的一个重要区别，也是它会产生冗余存储的根源。

## 6. 一个 fetch block 内多 branch 如何组织

`AheadBTB` 并不是把一个 fetch block 里的所有 branch 压成一个“大 entry”。

更准确地说：

- 同一个 fetch block 会映射到同一个 `index / set`
- block 内的不同 branch 存放在这个 set 的不同 way 中

所以它的组织方式应理解为：

- 同一个 fetch block 共享一组 set 位置
- block 内多条 branch 在该 set 中用不同 way 区分

这也是为什么当前配置使用 `8-way`。它不是随便选出来的 associativity，而是直接对应：

- 一个 fetch block 内可能有较多 branch 候选
- `AheadBTB` 希望能把这些 branch 的 target 信息尽量一起吐出来
- 然后再和 `uBTB` 结果去重，交给 `MicroTAGE` 做更细的方向处理

必要时，它也可以用自己的 `2-bit ctr` 给出初步方向预测。

## 7. 为什么会有 alias 和 way 压力

`AheadBTB` 的 ahead pipeline 组织天然会带来一类特殊 alias 风险。

因为它的索引来自“上一拍的 `startPC`”，而 tag 来自“这一拍的 `startPC`”。所以可能出现这样的情况：

- 同一个前序 fetch block `A`
- 在不同路径上，可能跳到不同后继块 `B1 / B2 / B3`
- 这些路径共享相同的 ahead index
- 但依赖不同的 tag 区分

如果 `B1 / B2 / B3` 本身又都是分支密集块，那么问题会进一步放大：

- 每个 `Bx` 自己内部可能就有多条 branch
- 它们又都要占用同一个 set 下的 way
- 当前 `8-way` 在这种情况下仍可能不够

所以这里的问题不只是“tag alias”，还包括：

- 同一前序块对应多个后继块
- 多个后继块内部又各自有多条 branch
- 最终把 way 压力叠加到同一个 set 上

长 tag 可以缓解 alias，但无法解决 way 不足。

不过，这个问题目前的严重性相对可控，因为：

- `AheadBTB` 不是最终兜底预测器
- 它 miss 了，主要表现为更大的 override bubble
- 后续仍有 `mBTB` 兜底

因此，这类问题更像是：

- `AheadBTB` 覆盖不足
- 性能下降

而不是功能性错误。

## 8. 为什么默认使用 S3 结果更新

当前 `AheadBTB` 默认启用 `usingS3Pred`。

这意味着它的默认更新路径不是依赖普通 commit / resolve update，而是优先使用 `S3` 的最终预测结果来更新自身。

这样做的原因和 `uBTB` 类似，但目标更偏“覆盖能力对齐”：

- `AheadBTB` 在 `S1` 给出较大的早期 target 候选
- `S3` 则给出更晚但更完整的最终结果
- 用 `S3` 更新 `AheadBTB`，可以让前级较大容量预测器尽量贴近顶层最终选择

当然，它仍然支持普通 update 通路；但从职责上看，`usingS3Pred` 更符合它“为后级结果提前铺路”的定位。

## 9. 设计总结

`AheadBTB` 当前设计可以概括成五句话：

1. 它是 `uBTB` 之上的一层更大容量的早期 target predictor。
2. 因为容量更大，无法像 `uBTB` 那样同拍完成普通访问，所以采用 ahead pipeline。
3. 它虽然名字叫 BTB，但存储语义更接近 `startPC + offset` 的 FTB 风格，而不是严格对齐块 BTB 风格。
4. 同一个 fetch block 内多 branch 存在同一个 set 的不同 way 中，因此 `8-way` 既承担 block 内分支共存，也承担不同路径 alias 的压力。
5. 它 miss 了主要会增加 override bubble，但后续仍有 `mBTB` 兜底，因此问题更多体现为性能损失而不是功能缺陷。

所以，`AheadBTB` 的核心不是“又一个 BTB”，而是：

- 一个通过 ahead pipeline 抢时间的较大容量早期 target predictor

## 10. 实现锚点

当前最相关的实现文件有：

- `src/cpu/pred/btb/abtb.hh`
- `src/cpu/pred/btb/abtb.cc`
- `src/cpu/pred/BranchPredictor.py`
- `src/cpu/pred/btb/test/abtb.test.cc`

## 11. 参考资料

- `docs/design-docs/frontend/bpu_top_level.md`
- `docs/design-docs/frontend/ubtb_design.md`
- `docs/design-docs/frontend/mbtb_design.md`
- `docs/design-docs/frontend/btb_tage_design.md`
- `docs/design-docs/images/BPU-TAGE.jpeg`
