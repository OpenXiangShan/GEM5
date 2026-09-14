# 行为级建模决策

用于新模型、重大行为变化或需要检查建模取舍的评审；不要求每个小修改逐项回答。

## 建模决策参考

### 1. 高性能建模

模拟器热点应使用固定容量队列、readyTime 排序、bitset、索引表、有限枚举、事件驱动和每周期 quota。高频路径应避免复制 RTL 的大规模组合搜索。

根据资源语义选择：

- 对队列、窗口、端口、bank、MSHR 这类资源给出容量和访问复杂度。
- 用 head/tail、ready list、free list、hash index、busy bitmap 表达资源可用性。
- 每周期处理量由参数限制，例如 issue width、completion width、dequeue width、command bandwidth。
- 对按就绪时间有序的资源，可用 `readyTime` 或事件等待；允许越过未就绪项的调度资源需要保留独立项前进语义。

评审检查项：

- 新增信号级临时状态必须对应新的性能后果。
- 每周期遍历必须有上界或早停条件。
- debug/统计数据不得扩大热路径主结构的复杂度。

### 2. 参数化建模

需要探索的资源容量、延迟和策略应复用或增加微结构语义参数。固定的正确性约束无需开关；不要仅为满足模板给 bug fix 添加参数。

常见参数维度（按当前任务选取）：

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

- 结合瓶颈假设检查参数趋势，例如队列容量与 full stall、带宽与 busy、延迟与等待的关系；反馈、竞争和瓶颈迁移可能打破简单单调性。
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
