# Kunminghu uBTB 设计说明

## 1. 文档范围

本文档说明 Kunminghu v3 中 `uBTB` 的设计定位和几个关键取舍，重点回答：

- 为什么在 Kunminghu v3 中仍然保留 `uBTB`
- 为什么 `uBTB` 只存 taken branch
- 为什么 `uBTB` 默认使用 `S3` 最终预测结果更新

本文档不展开：

- `AheadBTB`
- `MicroTAGE`
- `2taken` 新方案细节

这些应由单独文档说明。

## 2. 基本定位

`uBTB` 基本继承自 Kunminghu v2 时代的思路，本质上是一个非常靠前、非常轻量的 taken-target predictor。

从实现上看，它的结构很直接：

- 默认组织为 `1 set × 32 ways`，等价于原来的 32 路全相连
- 也可以通过 `numSets` 和 `numWays` 配置为规则的组相连结构
- `S1` 零延迟预测
- 只存一个 taken branch 的基本信息

这里最重要的不是它“预测得有多准”，而是它“出结果足够快”。

如果只保留最粗的一层理解，可以把 `uBTB` 看成：

- 前端最早可用的 target 候选提供者
- 在更重预测器尚未完成前，先给出一个很快的 taken-target 方向

## 3. 为什么 Kunminghu v3 仍然保留 uBTB

在 Kunminghu v3 中，更强的早期预测器其实已经存在，例如：

- `AheadBTB`
- `MicroTAGE`

因此，单从“正常预测路径上的精度和覆盖能力”看，`uBTB` 并不是唯一的早期预测来源。

但 `uBTB` 仍然保留，最核心的原因不是它比 `AheadBTB` 更强，而是它在 redirect 场景下更快。

这是因为：

- `AheadBTB` 和 `MicroTAGE` 属于 ahead pipeline 结构
- 它们依赖前面若干拍已经铺好的预测流水
- 当发生 predecode redirect 或 resolve redirect 时，它们无法在 redirect 当拍立即重新给出预测结果

而 `uBTB` 是真正的 `S1` 直接预测器，因此在 redirect 发生后，它仍然可以立即给出一个 target 候选。

所以，Kunminghu v3 保留 `uBTB` 的首要原因，不是“它本身预测最好”，而是：

- 在最早时刻提供 taken target
- 尤其在 redirect 场景下提供单拍可用的快速恢复能力

`uBTB` 还有一个附带动机，是历史上便于做 `2taken` 扩展；但在当前文档里，这不是主线。

## 4. uBTB 的 set-way 组织

当前默认配置是 `1 set × 32 ways`，因此保持了原有的 32 路全相连行为；实验配置可以显式指定规则的 `numSets × numWays` 组织，例如 `8 sets × 4 ways`。

参数语义是：

- `numSets`：索引集合数，必须是 2 的幂，以便使用 mask 提取 set index
- `numWays`：每个 set 的 way 数，只要为正数即可；SMT 按线程切分 way 时需要为偶数
- 总容量由 `numSets × numWays` 派生，不再单独配置 `numEntries`

这样组织的原因是：

- `uBTB` 位于最早预测阶段，默认配置仍保持最短的全相连访问路径
- 需要研究容量冲突时，可以增加 set 数或调整相联度，而不引入不整除的容量参数
- 组内查找和替换只遍历目标 set 的有限 ways，热路径复杂度为 `O(numWays)`

相联度变大时，tag 比较和替换的代价也会随 `numWays` 上升，因此它并不是一个适合无限扩张的方向。

也正因为如此，Kunminghu v3 里更大、更复杂的块级预测能力并没有继续堆到 `uBTB` 上，而是交给了后面的 `AheadBTB`、`mBTB` 和 `TAGE` 系列。

## 5. 为什么 uBTB 只存 taken branch

这是 `uBTB` 最重要的容量取舍。

当前 `uBTB` 并不试图把一个 fetch block 内所有 branch 都存下来。它只保留这个 block 当前代表性的 taken branch。

这样设计的原因是：

- `uBTB` 容量很小
- 又要保持全相连和极短延迟
- 如果同时存 not-taken branch，entry 很快就会不够用

因此，`uBTB` 的基本哲学是：

- 它不是完整的 block 结构缓存
- 它只是最早阶段的 taken-target 候选缓存

从预测语义上看：

- 如果 `uBTB` 命中一个条件分支 entry，它默认把它视为 taken
- `uBTB` 不负责细致的方向判定
- 更重的方向预测和最终选择由后级预测器完成

所以，`uBTB` 的优势是快，而不是细。

## 6. 只存一个 taken branch 带来的代价

一个 fetch block 在 `uBTB` 中只保留一个 taken branch，确实节省了容量，但也天然带来代价。

如果同一个 fetch block 的“代表 taken branch”发生变化，那么 `uBTB` 里的旧 entry 就需要被替换掉。

这意味着：

- 不同 taken branch 可能围绕同一个 block entry 来回替换
- 从而出现 ping-pong 效应

换句话说，`uBTB` 的容量节省不是没有代价的。它用“只保留一个 taken branch”换来了更小、更快的结构，但也接受了某些场景下稳定性更差的问题。

## 7. 为什么默认使用 S3 结果更新

当前 `uBTB` 默认启用 `usingS3Pred`。

这意味着，`uBTB` 的主要更新来源不是简单依赖 commit / resolve 更新通路，而是优先使用 `S3` 的最终预测结果来修正自己。

这样做的好处是：

- `uBTB` 可以更直接地对齐当前顶层预测链最终选择出来的 taken branch
- 更快地修正自己在 `S1` 做出的粗预测

这与 `uBTB` 的职责是匹配的。因为它本来就不是最终裁决者，而是：

- 在前面先给一个足够快的候选
- 再由后级更强预测器给出最终结论
- 然后 `uBTB` 用这个最终结论来校正自己

当然，`uBTB` 也仍然支持普通 update 通路：

- 如果关闭 `usingS3Pred`
- 它可以走 commit / resolve 相关的更新路径

目前从经验上看，这几种更新方式的性能差异并不大。因此，当前默认采用 `S3` 更新，更像是让 `uBTB` 与顶层最终预测保持一致的工程选择，而不是决定性性能来源。

## 8. 设计总结

`uBTB` 当前设计可以概括成四句话：

1. 它是 Kunminghu v3 中最早、最快的 taken-target 候选提供者。
2. 它保留的核心原因，是 redirect 场景下仍能单拍给出预测。
3. 它只存一个 taken branch，并默认使用 `1 set × 32 ways` 保持轻量的全相连结构，同时支持组相联实验。
4. 它默认使用 `S3` 最终预测结果更新，使自己持续对齐顶层最终选择。

所以，`uBTB` 不应被理解成一个“小号 mBTB”，而更像是：

- 一个非常快的前级 taken-target 缓冲
- 为更强但更晚的预测器争取时间

## 9. 实现锚点

当前最相关的实现文件有：

- `src/cpu/pred/btb/btb_ubtb.hh`
- `src/cpu/pred/btb/btb_ubtb.cc`
- `src/cpu/pred/BranchPredictor.py`

## 10. 参考资料

- `docs/design-docs/frontend/bpu_top_level.md`
- `docs/design-docs/frontend/mbtb_design.md`
- `docs/design-docs/frontend/btb_tage_design.md`
