---
name: cpu-performance-modeling
description: 设计或评审 CPU 性能模型的行为抽象、资源竞争、建模粒度及模拟开销。
---

# CPU 性能建模

保留 `workload event -> resource/control state -> contention/backpressure -> latency/progress -> stats` 的性能因果链。建模目标是复现关键瓶颈与参数趋势，并明确与 RTL 的误差边界。

## 按改动规模工作

- 新模型或重大行为变化：实现前说明性能问题与证据、外部可见后果、资源/控制状态、粒度取舍、参数默认行为、热路径复杂度和验证方法。先解释机制与 tradeoff，再推进实现；无需单独创建合同文件。
- 小型行为修复：只解释受影响机制、旧/新行为及针对性验证，复用已有参数和 stats。
- 只读评审：定位因果链缺失、资源竞争错误、复杂度或证据缺口；不要求补一份完整设计或新增实验。

## 关键约束

- 保留影响 progress、stall、replay、flush、ordering、forwarding 和功能正确性的状态；仅无关内部细节可粗化。
- 需要探索的容量、延迟或策略用语义参数表达；固定正确性约束和 bug fix 不必增加开关。
- 热路径处理量应有资源边界，避免无界扫描。数据结构依照访问和调度语义选择；有界扫描可在复杂度与收益合理时保留。
- 复用或增加能解释目标瓶颈的 stats；统计本身不得改变模型行为。
- 用最小复现、trace、stats 或参数 A/B 验证相关机制，区分功能正确性和性能拟合；不把参数趋势当作无条件单调保证。

## 按需参考

- 需要资源、粒度、参数和复杂度检查维度时读取 [modeling-principles.md](references/modeling-principles.md)。
- 需要仲裁、LSQ、store buffer、MSHR 或 memory controller 伪代码时读取 [modeling-examples.md](references/modeling-examples.md)。

交付时说明改动及原因、关键取舍、验证证据和剩余风险；只列本次涉及的参数、stats 和复杂度变化。
