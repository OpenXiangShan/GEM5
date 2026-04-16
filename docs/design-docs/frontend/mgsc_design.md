# Kunminghu MGSC 设计说明

## 1. 文档范围

本文档说明 Kunminghu v3 中 `MGSC` 的设计定位和几个关键取舍，重点回答：

- 为什么在已经有 `BTBTAGE` 的情况下仍然保留 `MGSC`
- Kunminghu v3 的 `MGSC` 和传统单分支 `SC` 相比，工程问题有什么不同
- 当前实现里哪些结构被保留，哪些结构被有意简化
- 当前实现中还有哪些值得注意的工程约束

本文档不展开：

- `BTBTAGE` 本体
- `MicroTAGE`
- `ITTAGE`
- 顶层 redirect / override 选择逻辑

这些应由单独文档说明。

## 2. 基本定位

`MGSC` 可以理解成挂在 `BTBTAGE` 后面的 statistical corrector。

它不负责独立产生一套完整的 branch 候选，而是只在 `BTBTAGE` 已经给出方向预测后，再回答一个问题：

- 当前这条 conditional branch，是否值得用一组额外历史模式去推翻 `TAGE` 的结论

所以它的职责不是替代 `TAGE`，而是：

- 在 `TAGE` 信心不够强时提供二次校正
- 尽量纠正那些 `TAGE` 单独不容易学好的方向模式

目前经验收益大致在 `0.3 分/GHz` 左右，因此它仍然是 Kunminghu v3 中值得保留的一层增强预测器。

## 3. SC 原理速记

传统 `SC` 的基本思路并不复杂：

- 用多组不同类型、不同历史长度的表分别建模 branch 方向
- 每组表给出一个 signed contribution
- 把这些 contribution 累加成一个总和
- 当总和足够大时，用它去覆盖 `TAGE` 的原始预测

如果只保留最粗的一层理解，可以把它看成：

- `TAGE` 擅长找“谁来当 provider”
- `SC` 擅长在 provider 不够稳时，再用更多历史视角做一次投票

Kunminghu v3 的 `MGSC` 基本沿用了这条思路，没有重新发明一套新的 `SC` 原理。

## 4. Kunminghu v3 中真正的新问题

和论文或传统单分支模拟器不同，Kunminghu v3 的 `MGSC` 不是对“每拍输入一条 branch”工作，而是嵌在块级 BPU 流水里。

这带来两个实际差异。

### 4.1 它服务的是 fetch block 流水，而不是独立单分支接口

`MGSC` 的输入并不是裸的 `branchPC`，而是：

- `mBTB` / `BTBTAGE` 已经筛出的 conditional branch 候选
- `BTBTAGE` 传下来的方向、置信度等信息
- 当前 fetch block 对应的一组历史快照

因此它在预测时的组织方式是：

- 用 `startPC + folded history` 生成各类表的 index
- 再用具体 `branchPC` 选中 SRAM line 中对应的 lane
- 最后对 block 内每条 conditional branch 分别形成 `MgscPrediction`

所以它虽然仍然是“按分支给出纠正结果”，但工程上已经是一个 block-level predictor 的后级组件。

### 4.2 它依赖的历史种类更多

当前 `MGSC` 不只依赖普通全局历史，还同时使用：

- `GHR`
- `PHR`
- backward branch history
- IMLI history
- first local history

这些历史都要接入顶层统一的 speculative update 和 squash recover 流程。

因此，`MGSC` 真正难的地方不在公式本身，而在于：

- 不同历史的折叠状态要和顶层预测流水严格对齐
- 预测、回滚、再更新必须保持一致

## 5. 当前实现保留了哪些 SC 结构

Kunminghu v3 这版 `MGSC` 仍然保留了比较典型的多表结构：

- `BW`：backward branch history
- `L`：first local history
- `I`：IMLI history
- `G`：global history
- `P`：path history
- `Bias`：与 `TAGE` 结果相关的偏置项

这些表共同输出 percsum，再组成 `total_sum`。

当前最终是否启用 `MGSC` 覆盖 `TAGE`，主要取决于两件事：

- `TAGE` 给出的置信度等级
- `abs(total_sum)` 是否超过当前 threshold

所以它本质上不是“总是和 TAGE 对着干”，而是一个带置信度门控的 correction 层。

## 6. 当前实现里有意做的简化

相对于一份“完全展开”的传统 `SC`，Kunminghu v3 当前实现做了几处很现实的折中。

### 6.1 Weight framework 还在，但默认不再作为核心设计点

代码里仍然保留了：

- weight table
- weight-related stats
- weight-scale-diff 这类观测量

但当前 `calculateScaledPercsum()` 实际上不再对 percsum 做额外缩放。

这不是单纯的“还没做完”，而是一个有意的工程取舍：

- 这部分收益目前看不大
- RTL 侧为了节省面积，暂时不做这套权重缩放机制

因此，当前更准确的理解应是：

- `MGSC` 的主收益来自多类历史表本身
- weight 相关框架更多是保留了探索接口和统计观测，而不是当前版本的主角

### 6.2 Threshold 机制以全局门控为主

当前实现同时保留了：

- global threshold
- 可选的 PC-indexed threshold

但默认配置下，真正主导行为的是 global threshold，`PC threshold` 更多是实验开关。

这也符合当前版本的取向：

- 先把 `MGSC` 作为一层稳定、可控的 correction 机制接入
- 不把过多收益不确定的调参结构堆进默认路径

### 6.3 Update 策略偏保守

当前并不是每次都训练 `MGSC`。

它主要在以下场景更新：

- `SC` 预测错误
- 或者虽然预测正确，但 margin 不够高

这说明当前 `MGSC` 的训练思路更像：

- 在“明显没学好”或“学得还不够稳”时再介入

而不是把它当成一个完全独立、强侵入性的替代预测器。

## 7. 为什么说它更像工程化的 MGSC，而不是论文复刻

如果只看原理，`MGSC` 很像传统 `SC`；但如果看代码接线，它更像一个为了 Kunminghu v3 顶层 BPU 落地而工程化改写过的版本。

最明显的特征有三个：

1. 它依赖 `BTBTAGE` 先给出 `tage_pred_taken`、主预测置信度和 bias 相关信息。
2. 它要和顶层 block 预测流水共享 speculative / recover history 机制。
3. 它的很多结构选择更强调“能稳定接进现有 BPU”而不是“把 SC 论文里的所有技巧都做满”。

所以这版 `MGSC` 更适合被理解成：

- Kunminghu v3 中的一层 practical statistical corrector

而不是：

- 对经典 `TAGE-SC-L` 论文结构的逐项翻译

## 8. 工程提示

`MGSC` 当前有一个需要简单记住的工程特点：它的收益归因不像 `TAGE` 那样直观。

原因也比较直接：

- 某些表训练不到
- 前级没有把这条 branch 送到 `MGSC`
- `TAGE` 置信度过高，导致 `MGSC` 没有介入
- 历史恢复路径让某类表没有积累出有效模式
- 某些模式只在少量程序上生效，收益解释不够直接

当前实现已经提供了一些观测接口，例如：

- `MGSCTRACE`
- 一组较细的 `mgscStats`
- 单元测试

但这些手段更适合确认“机制是否在工作”，还不太擅长快速回答更细的收益归因问题。因此当前文档只简单提示这一点，不展开太多调参经验。

## 9. 设计总结

`MGSC` 当前设计可以概括成四句话：

1. 它是挂在 `BTBTAGE` 后面的 statistical corrector，而不是独立的方向预测器。
2. 它沿用了传统 `SC` 的多历史表 + threshold gating 思路，但被嵌进了 Kunminghu v3 的 block-level BPU 流水。
3. 当前版本保留了 `SC` 的主体结构，但有意弱化了 weight scaling 等收益有限、面积不划算的部分。
4. 实现上的关键约束主要来自多历史 speculative/recover 一致性，以及当前仍较有限的收益观测能力。

## 10. 实现锚点

当前最相关的实现文件有：

- `src/cpu/pred/btb/btb_mgsc.hh`
- `src/cpu/pred/btb/btb_mgsc.cc`
- `src/cpu/pred/btb/test/btb_mgsc.test.cc`
- `src/cpu/pred/btb/decoupled_bpred.cc`
- `src/cpu/pred/BranchPredictor.py`

## 11. 参考资料

- `docs/design-docs/frontend/bpu_top_level.md`
- `docs/design-docs/frontend/btb_tage_design.md`
- `docs/design-docs/frontend/phr_design.md`
- `src/cpu/pred/btb/docs/btb_mgsc.md`
