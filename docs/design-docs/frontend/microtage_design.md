# Kunminghu MicroTAGE 设计说明

## 1. 定位

`MicroTAGE` 是放在 `S1` 的轻量方向预测器。

它的作用很直接：

- 接收 `uBTB` / `AheadBTB` 给出的 branch 候选
- 在 `BTBTAGE` 之前先做一轮更早的方向判断

所以它的价值不在于“单独预测得更强”，而在于“更早可用”。

## 2. 和 BTBTAGE 的关系

`MicroTAGE` 基本是从 `BTBTAGE` 复制出来的轻量版本。

它保留了同样的基本问题设定：

- 仍然面向一个 fetch block 内多条 branch
- 仍然使用 `startPC + folded history` 做 index
- 仍然用 `position` 进入 tag 来区分 block 内不同 branch

但它做得更轻：

- 表更小
- 结构更简单
- 延迟更早

所以更适合把它理解成“前级版 TAGE”，而不是“小号但完整等价的 BTBTAGE”。

## 3. 核心设计点：ahead index pipeline

`MicroTAGE` 最关键的设计点，是 ahead index pipeline。

如果完全照 `BTBTAGE` 的方式做，那么：

- 当前拍更新出来的 index folded history
- 当前拍就要立刻参与查表

这对一个要放在 `S1` 的 predictor 来说，时序仍然偏重。

因此当前实现做了一个专门的折中：

- `tagFoldedHist` / `altTagFoldedHist` 立即更新
- `indexFoldedHist` 延后一拍可见

从代码上看：

- `doUpdateHist()` 会先生成新的 index folded history
- 但不立即让它成为当前拍的 `indexFoldedHist`
- 而是先写入 `aheadindexFoldedHist`
- 下一拍再接管

所以它本质上是在做：

- tag 用“当前可见”的 folded history
- index 用“前一拍准备好”的 folded history

这就是 `MicroTAGE` 能放到更前面工作的关键。

## 4. 为什么不展开更多细节

`MicroTAGE` 和 `BTBTAGE` 在实现上还有不少差异，例如：

- fallback 更简单
- provider 逻辑更轻
- `useful` / allocation 也有不同

但这些不是当前文档的重点。

当前更重要的结论只有一个：

- `MicroTAGE` 存在的核心意义，是以前级轻量 predictor 的形式，尽早给出一版比 base ctr 更好的方向预测

## 5. 总结

`MicroTAGE` 可以概括成三句话：

1. 它是 `S1` 的轻量方向预测器。
2. 它沿用 `BTBTAGE` 的基本思路，但做得更轻、更早。
3. 它最关键的工程点是 ahead index pipeline。

## 6. 实现锚点

- `src/cpu/pred/btb/microtage.hh`
- `src/cpu/pred/btb/microtage.cc`
- `src/cpu/pred/BranchPredictor.py`

## 7. 参考资料

- `docs/design-docs/frontend/bpu_top_level.md`
- `docs/design-docs/frontend/btb_tage_design.md`
- `docs/design-docs/frontend/abtb_design.md`
