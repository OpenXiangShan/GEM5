---
name: cpu-performance-modeling
description: 由 CPU 性能模拟器建模经验蒸馏而成的行为级建模规范。强调高性能实现、参数化资源模型、细粒度/粗粒度取舍、控制流抽象、复杂度约束和 stats 驱动验证。适用于新增或修改性能模型、把 RTL 机制抽象成模拟器模型、设计参数化微结构模型、决定哪些行为需要细粒度建模、评审 AI 生成的 CPU 性能模拟代码时。
---

# CPU 性能建模

## 基本立场

本 skill 规定 CPU 性能模型的建模流程和评审标准。任何模型都应先明确同一条因果链：

```text
workload event -> abstract control/resource state -> contention/backpressure
    -> latency/retry/progress -> observable stats
```

建模目标是复现真实性能后果，而非逐信号复刻 RTL。模型可以在明确说明误差边界的前提下牺牲少量拍级细节，以换取模拟速度和可参数化能力；关键瓶颈、参数趋势和 stats 归因必须保留。

## 工作流

做新增、修改或评审前，按顺序执行：

1. **读现有模型**：调研同类资源的本地实现，提取状态、队列、token、参数、stats 和 debug 输出；RTL 信号映射仅作为后续校验材料。
2. **写建模合同**：在方案或最终答复中明确合同；非平凡改动必须先形成合同。
3. **判定粒度**：把行为分成必须细建、参数化粗建、只保功能正确三类。
4. **选算法**：实现前确定热路径复杂度和数据结构。
5. **落参数和 stats**：所有性能相关行为必须有参数入口和可观测结果。
6. **验证趋势**：用最小 workload、debug trace、stats 或 A/B 参数变化证明模型保留了性能因果链。

## 建模合同

合同必须具体到能指导代码结构，至少包含：

1. **性能问题**：模型要反映吞吐、延迟、端口竞争、队列占用、replay、flush、miss/hit、bank conflict、turnaround、公平性中的哪几类。
2. **外部症状**：用户或上游模块能观察到什么变化，例如发射受阻、请求被拒、响应提前、load replay、store drain、queue full、prefetch 被丢弃。
3. **控制状态机**：列出状态和转移，例如 `ready -> selected -> issued -> blocked -> replayed -> completed -> released`。
4. **资源 token**：列出瓶颈资源，例如 issue port、read/write port、pipeline slot、queue entry、MSHR、bank、tag path、store buffer line、command window。
5. **参数面**：列出宽度、深度、延迟、阈值、策略、开关、dequeue rate、window size；默认值必须说明是否保持旧行为。
6. **粒度选择**：逐项说明哪些细粒度建模，哪些合并为 latency/token/queue/threshold/window，哪些只维护功能正确。
7. **算法复杂度**：说明每周期、每请求、每完成事件的复杂度上界；热路径不能依赖无界扫描。
8. **可观测性**：列出 stats/debug/minimal workload；新增 blocked/replay/merge/drop/evict 原因必须可观测。

## 六条硬原则

### 1. 高性能建模

模拟器热点应使用固定容量队列、readyTime 排序、bitset、索引表、有限枚举、事件驱动和每周期 quota。高频路径应避免复制 RTL 的大规模组合搜索。

必须做到：

- 对队列、窗口、端口、bank、MSHR 这类资源给出容量和访问复杂度。
- 用 head/tail、ready list、free list、hash index、busy bitmap 表达资源可用性。
- 每周期处理量由参数限制，例如 issue width、completion width、dequeue width、command bandwidth。
- 队头未就绪时通过 `readyTime` 或事件重调度等待，而不是每拍扫描所有 entry。

评审检查项：

- 新增信号级临时状态必须对应新的性能后果。
- 每周期遍历必须有上界或早停条件。
- debug/统计数据不得扩大热路径主结构的复杂度。

### 2. 参数化建模

性能相关行为必须由微结构语义参数控制，而不是硬编码常量或 RTL 信号名。

必须参数化：

- 宽度：fetch/decode/rename/commit/issue/load/store/writeback/completion。
- 深度：ROB、IQ、LQ、SQ、RAR/RAW queue、MSHR、write buffer、store buffer。
- 延迟：前后级 TimeBuffer、pipeline stage、cache access、frontend/backend memory latency、turnaround。
- 带宽和窗口：port token、dequeue per cycle、bank/tag access、command window、min reads/writes per switch。
- 策略和开关：调度策略、replay 策略、bank conflict check、miss replay、strict wait、prefetch admission。

参数默认值的规则：

- 默认值优先保持旧行为或当前配置的性能趋势。
- 参数名描述模拟器语义，例如 `LoadCompletionWidth`；避免使用 RTL wire 名。
- 新参数必须在配置入口、构造函数、stats 解释之间能串起来。

### 3. 细粒度和粗粒度的平衡

细粒度给关键控制流，粗粒度给不决定性能的内部细节。

必须细建的行为：

- 会改变发射、唤醒、提交、请求发送、响应返回、replay、flush、drain 的转移。
- 会造成主瓶颈的资源竞争，例如端口、bank、MSHR、队列满、读写切换。
- 会决定是否提前返回、是否 full/partial forward、是否 merge/drop、是否 backpressure 上游。
- 会显著改变 stats 趋势或 benchmark 排名的行为。

应粗建的行为：

- 只改变内部流水分拍，但不改变对外可见的阻塞、完成、重试或带宽。
- 多个 RTL 信号共同产生同一种性能后果，可合并成枚举原因。
- 数据搬运细节不影响 forwarding、ordering、异常或功能正确时，可压成 id/mask/block/age/sequence。

粒度判定算法：

1. 写出该机制所有外部后果。
2. 标出会改变后果的最小控制条件。
3. 若条件会频繁触发且影响 IPC/latency，细建。
4. 若条件只改变等待时间或吞吐上限，用参数化 latency/bandwidth/threshold/window 吸收。
5. 若条件只影响内部 debug 或数据内容，保留最小功能状态。
6. 对每个被粗建的点说明损失了什么拍级细节，以及为什么不影响目标趋势。

### 4. 行为级性能建模

行为级模型以性能后果为目标，不以 1:1 RTL 复刻为目标。

高准确度标准：准确度来自保留关键因果链，而不是更像 RTL：

- 参数变化应产生合理趋势：更深队列减少 full stall，更大带宽减少 busy，更长 latency 增加等待，更高阈值改变 drain/turnaround。
- 不同 replay/blocked 原因若等待行为不同，必须区分；若外部后果相同，可以合并。
- 读写请求的 merge、forward、drop、early response 必须保留，因为它们直接改变队列压力和响应时间。
- 每个关键性能变化都应能被 queue occupancy、port busy、replay reason、latency sample 或 bandwidth/window stats 解释。
- 若为了速度牺牲拍级细节，必须说明保留了哪些控制结果，损失了哪些内部时序。

评审时重点问：

- 模型是否解释了性能变化。
- A/B 改参数时，趋势是否可预测且可用 stats 归因。
- 新增状态是否影响 progress、stall、retry、latency 或 bandwidth。

### 5. 控制流优先于数据流

性能模拟器先建控制流，再保留最小数据。

优先保留：

- 请求 identity：inst seq、thread id、request id、block address、QoS、age/order。
- 覆盖关系：byte mask、split chunk、full/partial coverage、newer/older。
- 时序关系：readyTime、entryTick、nextReqTime、busy bitmap、completion index。
- 控制原因：blocked/replay/forward/merge/drop/evict/turnaround reason。

只在这些情况保留数据内容：

- store-to-load forwarding 或 store buffer forwarding 需要判断完整/部分覆盖。
- atomic/LLSC、异常、device/local access、functional correctness 需要真实数据。
- debug 或 golden memory 需要最终写入内容。

其余数据流应压缩为 mask、block、burst、age、sequence、枚举或统计计数。

### 6. 算法设计优先

建模不是信号翻译，是约束下的算法设计。

优先选择这些模式：

- **端口 token + busy bitmap**：适合 issue/FU/cache port。
- **readyTime queue**：适合 MSHR、write buffer、memory request queue。
- **completion index + dequeue quota**：适合有序完成、RAR/RAW、commit/drain。
- **hash index + LRU/free list**：适合 store buffer/write queue merge/forward。
- **有限 replay reason**：适合 bank/tag/MSHR/cache blocked/nuke/RAW/RAR。
- **window/threshold state machine**：适合 memory controller、drain、读写切换。

复杂度要求：

- 热路径目标是 O(1)、O(log N) 或 O(k)，其中 k 是参数化小上限。
- 扫描必须有自然边界，例如 queue depth、source operands、burst chunks、active priorities。
- 若扫描可能很大，增加索引、ready list、age pointer、completion index 或缓存上次结果。
- stats 不能改变模型行为，也不能迫使热路径做额外大扫描。

## 参考示例

需要具体伪代码时读取 [references/modeling-examples.md](references/modeling-examples.md)。其中包含高性能端口仲裁、LSQ replay、RAR/RAW 队列、store buffer merge/forward、cache/MSHR readyTime、memory controller window/threshold 调度等可迁移模板。

## 输出要求

使用本 skill 完成代码任务时，最终说明必须包含：

- 建模合同摘要。
- 细粒度/粗粒度取舍，尤其是关键路径和非关键路径的分界。
- 参数列表、默认值和配置入口。
- 核心数据结构和复杂度。
- 新增或复用的 stats/debug。
- 准确度论证：保留了哪些性能因果链，损失了哪些拍级或数据流细节，为什么可接受。
- 验证命令和结果；不能验证时说明剩余风险。

评审 AI 生成代码时，优先指出以下问题：关键瓶颈缺失、不可参数化、热路径无界扫描、stats 不可归因、RTL 信号机械映射、数据流过重、缺少 A/B 趋势验证。
