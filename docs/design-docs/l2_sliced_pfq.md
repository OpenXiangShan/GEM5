# L2 Per-Slice PFQ 对齐设计

## 背景

当前 aligned L2 使用一个中央 `L2CompositeWithWorkerPrefetcher` 观察所有
L2 slices 的训练流，并通过 `L2CacheWrapper` 将生成的预取请求按物理地址
发送到目标 slice。中央预取器保留了全局预取视野，但当前 PFQ 和发送路径仍按
单队列建模：所有请求进入同一个 `Queued::pfq`，wrapper 每周期只从队头取出
一个请求，并使用一组全局的 outstanding/block 状态。

RTL 为每个 L2 slice 配置一条独立 PFQ。以四个 slices 为例，共有四条 16 项
FIFO，总容量仍为 64 项。每条 slice PFQ 每周期最多接收一个请求，也最多向
对应 slice 发送一个请求；不同 slices 可以并行。因此 RTL 在同一周期最多完成
四次 PFQ 入队和四次 PFQ 出队。

本文及实现统一使用 `slice`/`per-slice PFQ`，不再将这些队列称为 bank，避免
与每个 L2 slice 内部的 DataSram/DirSram bank 混淆。

本设计在保留中央预取算法的前提下，将 GEM5 的 PFQ 容量、仲裁、吞吐和
backpressure 行为对齐到上述 RTL 结构。

## 目标

本次改动需要满足以下行为：

1. 预取请求按物理地址映射到对应 slice PFQ。
2. 四个 slices 配置下，每条 slice PFQ 是 16 项 FIFO，总容量为 64 项。
3. 每条 slice PFQ 每周期最多接收一个请求，不同 slices 可以并行入队。
4. 每条 slice PFQ 每周期最多发送一个请求，不同 slices 可以并行出队。
5. slice PFQ 满时仍允许新请求进入，同时丢弃该队列中最旧的请求。
6. 每个输入 source 每周期最多提供一个请求参与 PFQ 入队仲裁。
7. L1 downstream hint source 的优先级高于 L2 自身生成的请求。
8. L2 本地来源在同一 slice 冲突时使用现有优先级：
   `largeBOP > smallBOP > despacitoStream > LLDP > CDP > CMC`。
9. 不同 source 若映射到不同 slice，可以在同一周期分别入队，不再先全局
   仲裁成单个请求。四个 slice 的聚合入队上限仍为四条请求每周期。
10. 某个 slice 的发送阻塞只影响该 slice，不阻塞其他 slices 的预取发送。
11. 普通 demand 请求继续优先于预取请求使用 slice 入口。

本次改动不复制四份预取算法实例。BOP、CDP 等训练状态仍由一个中央
`L2CompositeWithWorkerPrefetcher` 维护。

## RTL 行为模型

### 输入来源

每周期可能同时存在以下预取候选：

- L1 通过 `add_pf_downstream()`/`rxHint()` 下传的 prefetch-ahead hint；
- L2 自身的 `largeBOP` 请求；
- L2 自身的 `smallBOP` 请求；
- L2 自身的 `despacitoStream` 请求；
- 启用 `--l2-enable-lldp` 时，L2 composite 内建 LLDP 的请求；
- L2 自身的 CDP 请求；
- L2 自身的 CMC 请求。

本次改动中的“L1 source”只指上述 downstream hint 路径。L1 cache miss 后沿
普通 timing port 进入 `L2CacheWrapper` 的请求不属于本次 PFQ 改动，继续沿
现有 wrapper-to-slice 路径传递。

每个 source 每周期最多向输入仲裁器提供一个候选。这里的 source 包括 L1
downstream hint、各个 L2 本地预取器以及 L2 内建 LLDP。因此，同一周期可以
有多个 source 分别写入不同 slice，但单个 source 不能在同一周期写入多个
slices。

每个候选在物理地址可用后计算 slice：

```text
slice_id = getSliceId(physical_address)
```

输入仲裁按 slice 独立进行。多个候选映射到不同 slice 时可以全部获胜；映射到
同一 slice 时只允许一个获胜。

同一 slice 的优先级分为两层：

1. L1 downstream hint 优先于所有 L2 本地请求；
2. L2 本地请求按
   `largeBOP > smallBOP > despacitoStream > LLDP > CDP > CMC` 仲裁。

未获胜候选不得因为 PFQ 写端口冲突而静默丢失。本模型要求它继续保留在来源
buffer 中，并在后续周期重新参与仲裁。PFQ 容量满导致的“丢旧请求”只发生在
候选已经赢得目标 slice PFQ 写端口之后。

L2 内建 LLDP 必须共享同一个 per-slice PFQ 写端口，不能绕过输入带宽直接进入
发送路径。它每周期最多提供一个候选，优先级位于
`despacitoStream` 之后、CDP 之前。L1 内建 LLDP 的普通请求路径不作为独立的
L2 本地 source；只有通过 `rxHint()` 到达的请求才归入 L1 source。

### PFQ 存储

每条 slice PFQ 的队列行为是 FIFO：

```text
capacity(slice_pfq) = 16
enqueue(slice_pfq)  = push_back
dequeue(slice_pfq)  = pop_front
```

当 slice 已满且本周期有新请求赢得写端口时，先删除该 slice 当前最旧的排队
请求，再将新请求追加到队尾。因此新请求始终可以被 PFQ 接收：

```text
if slice_pfq.size == 16:
    drop(slice_pfq.front)
slice_pfq.push_back(new_request)
```

“丢旧请求”只在目标 slice 内发生，不能淘汰其他 slice 的请求。

### 同周期入队和出队

每条 slice PFQ 支持同周期一次入队和一次出队。模型应先确定本周期成功发生的
dequeue 和 enqueue，再统一提交状态，不能依赖 C++ event 或函数调用的偶然
先后顺序。

若已满的 slice PFQ 在同周期成功移出一个请求并接收一个新请求，最终
occupancy 保持 16，不需要额外丢弃另一个排队请求。若出队未发生而新请求成功入队，则按
“满时丢最旧”处理。

### 输出和 backpressure

每条 slice PFQ 每周期最多选择 FIFO 队头的一个请求发送到对应 slice。四条
slice PFQ 可以并行发送，因此四 slice 配置的峰值发送吞吐为四条请求每周期。

GEM5 timing port 在 `sendTimingReq()` 返回 false 后要求发送方保存原请求并
等待 retry。为避免被后续“满时丢旧”覆盖，每个 slice 使用一个独立的
outstanding 输出槽：

```text
slice PFQ -> per-slice outstanding -> slice request port
```

请求移入 outstanding 后即视为已经从 PFQ 出队。发送失败时只保持在对应
slice 的 outstanding 中；其他 slice 仍可继续出队和发送。该输出槽是 timing
接口的保持状态，不计入 16 项 PFQ 容量。

## 当前 GEM5 偏差

### 全局 PFQ

`Queued` 当前只维护一个 `std::list<DeferredPacket> pfq`。容量检查、最低优先级
淘汰和队头选择均在全局范围执行。因此一个 slice 可以占用大部分甚至全部
PFQ，也可能因为自身的新请求淘汰其他 slice 的请求。

aligned L2 在启用 L1-to-L2 hint 时目前还会把 `queue_size` 配置为 128，与四条
slice PFQ、每条 16 项的 RTL 总容量 64 不一致。本次改动需要同步改为 64，并在
初始化时检查总容量与 slice 配置一致。

### 全局输入互斥

`L2CompositeWithWorkerPrefetcher::GetPFRequestsFromBuffer()` 使用一个
`L2PFsent` 标志。`WorkerPrefetcher` 本周期只要搬运过 L1 hint，
`latestTransferTick` 就会阻止所有 L2 本地来源继续进入 PFQ。随后
`largeBOP`、`smallBOP`、`despacitoStream`、CDP 和 CMC 也通过同一个标志
串行选择。

这等价于整个 L2 PFQ 只有一个全局写端口，无法表达不同 slice 的并行入队。

另一方面，`WorkerPrefetcher::transfer()` 当前使用固定 `depth = 4`，允许单个
L1 source 每周期搬运最多四项。RTL 对齐后，L1 source 也必须限制为每周期一
项；四条每周期的聚合上限来自四个不同 source 写入四个不同 slice，而不是单个
source 同周期写入四个 slices。

### 全局单发和全局阻塞

`L2CacheWrapper` 当前只保存一个 `outstanding_prefetch` 和一个
`prefetch_blocked`。每次发送事件只调用一次 `getPacket()`，成功后再安排下一
周期发送。任意 slice 返回 retry 都通过同一组全局状态处理。

因此当前实现同时存在以下偏差：

- 总出队吞吐只有一条请求每周期；
- 一个 slice 阻塞会停止所有 slices 的预取发送；
- 全局队头可能造成跨 slice head-of-line blocking；
- 被预取阻塞的 slice 状态还会不必要地影响其他 slice 的 demand 路径。

## 设计方案

### 模块职责

#### `Queued`

`Queued` 保持为通用预取队列基础设施，继续负责：

- 候选地址到 `DeferredPacket`/`Packet` 的构造；
- 地址翻译和 pending translation queue；
- duplicate filtering；
- demand 命中时的 queued-prefetch squash；
- 通用 packet ownership 和统计更新。

`Queued` 不感知 slice 数量、地址到 slice 的映射或 per-slice 容量。

为了避免 L2 实现复制通用逻辑，可以为 ready queue 增加少量 protected 扩展
点，例如：

```cpp
virtual void enqueueReadyPacket(DeferredPacket &dpp);
PacketPtr dequeueReadyPacket(iterator it);
```

默认实现继续使用现有单队列行为。只有 L2 composite 重写 ready-queue 的存储
策略。translation queue 和其他预取器行为保持不变。

#### `L2CompositeWithWorkerPrefetcher`

L2 composite 是所有 per-slice PFQ 的所有者，负责：

- 收集 L1 downstream hint 和所有 L2 本地预取来源；
- 等待候选获得物理地址；
- 计算 `slice_id`；
- 执行 per-slice 输入仲裁；
- 实施每 slice 16 项 FIFO 和满时丢旧；
- 向 wrapper 提供 per-slice ready time、pending 状态和 dequeue 接口；
- 记录 per-slice PFQ 统计。

来源仲裁必须由显式的周期事件完成，不能依赖各来源调用 `addToQueue()` 的事件
顺序。推荐增加一个 `processPfqIngressEvent()`：

```text
for each source:
    peek one physically-ready candidate

classify candidates by slice

for each slice:
    select at most one winner by source priority
    pop only the winner from its source buffer
    enqueue winner into the selected slice PFQ
```

来源需要支持非破坏性 `peek` 和获胜后的 `pop/commit`。当前直接弹出请求的
`GetPFRequestsFromBuffer()` 接口需要拆分或补充 peek/commit 接口，否则无法
保证同 slice 仲裁失败者保留在原 buffer 中。每个 source 每周期只执行一次
`peek`，即使多个 slices 空闲也不能继续取该 source 的第二个候选。

跨页翻译尚未完成的候选不能参与 slice 仲裁。translation 完成且物理地址可用
后，它作为 physically-ready source 参加后续周期的 per-slice 仲裁。
如果实现需要在地址翻译前从算法 buffer 取出候选，则该候选必须进入归属于原
source 的 pending/ready staging；仲裁失败时继续保留在该 staging 中，不能因
数据结构搬移而被视为已经发送或丢弃。

#### `L2CacheWrapper`

Wrapper 只负责将每条 slice PFQ 的输出发送到相同编号的 slice，不负责 PFQ
容量、FIFO 淘汰或来源优先级。

当前标量状态：

```cpp
PacketPtr outstanding_prefetch;
bool prefetch_blocked;
```

改为 per-slice 状态：

```cpp
std::vector<PacketPtr> outstandingPrefetch;
std::vector<bool> prefetchBlocked;
```

发送事件每周期遍历所有 slices：

```text
for each slice:
    if this slice is blocked:
        continue
    if no outstanding request:
        dequeue at most one request from the corresponding slice PFQ
    try send outstanding request
```

`recvReqRetry()` 只解除产生 retry 的 slice。Wrapper 还需要记录被拒绝 demand
对应的 slice，其他 slice 的 retry 不能误触发上游 `sendRetryReq()`。若同一
slice 同时涉及 demand retry 和预取 outstanding，必须继续保持 demand 优先，
不能在发送 demand retry 前抢先重新发送预取。

### PFQ 数据结构

首选低侵入实现继续复用 `Queued::pfq` 的单个
`std::list<DeferredPacket>`，但将它解释为四条逻辑 slice FIFO，而不是改成
四个物理容器。

对 L2 PFQ 使用以下操作：

- 入队：追加到全局 list 尾部；
- slice occupancy：扫描并统计属于该 slice 的 entry；
- slice head：返回第一个属于该 slice 的 entry；
- slice PFQ full：删除该 slice 最旧的 entry，再追加新 entry；
- slice dequeue：仅当该 slice 最旧的 entry 已经 ready 时删除该 entry。

全局追加顺序能够保持每个 slice 内部的 FIFO 顺序。PFQ 总共只有 64 项，四个
slice 每周期扫描的复杂度上限约为 `4 * 64`，相对于 cache/prefetch 仿真路径可
忽略。首版不缓存 per-slice occupancy，避免 `Queued` 中已有的 demand squash
等直接删除路径导致计数失配。

`Queued::addToQueue()` 默认会按 request priority 排序，重复项提升优先级时也
可能重新排序。`kmhv3` 默认开启 PF buffer，当前 `PFSendEventWrapper()` 会把
priority 扁平化为 1，因此已有路径通常表现为 FIFO；这个配置与本设计兼容，
但不能代替 per-slice PFQ 的显式 FIFO 规则。L2 per-slice PFQ 的入队和满队列淘汰
必须始终按目标 slice 的到达顺序处理，不因关闭 PF buffer 或 priority 值变化而
改变。其他通用 `QueuedPrefetcher` 继续保留现有 priority queue 行为。

slice 查询、FIFO head 查找和满时淘汰可以封装为
`L2CompositeWithWorkerPrefetcher` 的私有 helper。除非未来其他 cache level
也需要相同行为，不新增通用的 sliced queue 基类。

### Slice 映射

每条 slice PFQ 必须和 `L2CacheWrapper::getSliceId()` 使用完全相同的物理地址映射。
不应在 prefetcher 中复制一份可能漂移的哈希算法。

推荐由 wrapper 初始化 prefetcher 时提供 slice 数和地址映射 callback：

```cpp
configureSlices(num_slices, [this](Addr addr) {
    return getSliceId(addr);
});
```

该接口可以通过一个窄的 slice-aware prefetcher interface 暴露，避免 wrapper 直接
依赖 `L2CompositeWithWorkerPrefetcher` 的具体类型。

### Wrapper 与预取器接口

slice-aware prefetcher 至少需要提供：

```cpp
void configureSlices(unsigned num_slices, SliceMapper mapper);
unsigned numSlices() const;
bool hasPendingPacket(unsigned slice) const;
Tick nextPrefetchReadyTime(unsigned slice) const;
PacketPtr getPacket(unsigned slice);
bool hasPendingIngress() const;
void processIngress();
```

现有无 slice 参数的接口保留给其他 cache/prefetcher。Aligned L2 wrapper 使用
sliced 接口；非 sliced cache 和其他通用预取器不改变行为。

### 实现落点

本次实现使用 `SlicedPrefetcher` 作为 wrapper 和 L2 composite 之间的窄接口。
`L2CacheWrapper` 在构造时传入 slice 数量以及自身的 `getSliceId()` callback，
因此 slice PFQ 与实际请求路由共用同一份地址映射。

`Queued::DeferredPacket` 和 buffered command 增加只由 owner 解释的 routing tag。
L2 composite 用该 tag 区分两个 BOP 实例以及其他 source，并让 tag 跨地址翻译
保留。`Queued` 的默认 priority queue 行为不变；L2 仅覆盖 ready PFQ 的重复项
更新策略，禁止 priority 更新重排对应 slice PFQ 的 FIFO。因此无论 kmhv3 的
priority 扁平化开关是否打开，slice 内顺序都只由进入 PFQ 的先后决定。

各 source 的 physically-ready 请求保存在 L2 composite 的 source staging 中。
每个 source 每周期只有队头参与一次仲裁；失败者留在原 staging，后续请求不能
越过它。L1 source 只表示 `add_pf_downstream()`/`rxHint()` 路径，普通 L1
timing request 不经过这些 staging。

wrapper 的发送事件固定按以下顺序提交：

1. 每个未阻塞 slice 最多从对应 slice PFQ 移出一项到 outstanding；
2. 尝试发送各 slice outstanding；
3. 调用 `processIngress()`，每 slice 最多接收一个仲裁赢家。

因此同周期 dequeue 明确早于 enqueue 生效，不依赖 event 注册顺序。发送失败的
packet 留在 per-slice outstanding 中；同 slice 的 demand retry 优先，其他
slices 不受影响。

## 为什么不在 Wrapper 中建立四个 PFQ

由 wrapper 持有四个 `deque<PacketPtr>` 看起来改动直接，但会产生两个队列
所有者：prefetcher 内已有一个 `Queued::pfq`，wrapper 又增加一个实际 PFQ。

这种方案存在以下问题：

- 请求移入 wrapper 后，`Queued` 的 duplicate filtering 看不到它；
- demand squash 无法删除 wrapper 中同地址的未发送预取；
- late-prefetch 判断无法统一观察完整 PFQ；
- PFQ full/drop、dequeue 和 issued stats 被分散到两个类；
- prefetcher PFQ 和 wrapper PFQ 形成额外缓冲，实际容量超过 RTL；
- L1 与多个 L2 本地来源的 per-slice 输入仲裁仍必须在 composite 中实现。

因此 wrapper-owned PFQ 只适合快速验证并行发送上界，不适合作为最终 RTL
对齐结构。将 PFQ 所有权保留在 L2 composite 中，模块边界和可观测性更清晰。

## 参数

建议 L2-specific 配置显式使用每 slice 容量，并固定下列 RTL 带宽约束：

| 参数 | 对齐值 | 含义 |
| --- | ---: | --- |
| `num_slices` | 4 | L2 slice 数量，由 L2 wrapper 配置。 |
| `pfq_entries_per_slice` | 16 | 每条 slice PFQ 的 FIFO 容量。 |
| source output width | 1 | 固定约束；每个输入 source 每周期最多提供一个候选。 |
| `pfq_enq_width_per_slice` | 1 | 每 slice 每周期最大入队数。 |
| `pfq_deq_width_per_slice` | 1 | 每 slice 每周期最大出队数。 |

总 PFQ 容量由 `num_slices * pfq_entries_per_slice` 得到。四 slice 对齐配置下
应为 64。现有 `queue_size` 可以暂时保留用于兼容，但构造时应检查它与派生总
容量一致，避免配置看似为 64、实际却按其他容量运行。

## 统计与可观测性

本次实现新增以下统计：

- `pfqEnqueued[slice]`：每 slice 成功入队数；
- `pfqDequeued[slice]`：每 slice 从 PFQ 移入 outstanding 的请求数；
- `pfqDroppedOldest[slice]`：每 slice 满时丢弃旧请求数；
- `pfqInputConflict[slice]`：同周期多个来源竞争同一 slice 的次数；
- `pfqInputWonBySource[source]`：各来源赢得 slice 写端口的次数；
- `pfqEnqueueWidth`：活跃 ingress 周期内成功写入多少个 slice 的分布；
- `pfqOccupancy[slice]`：每 slice 当前 occupancy；
- `pfqMaxOccupancy[slice]`：每 slice 最大 occupancy；
- `pfqDequeueWidth`：每周期从多少个 slice 移出请求的分布；
- `pfqSendBlocked[slice]`：各 slice 拒绝预取发送的次数。

`pfqResidenceCycles` 和严格的 blocked-cycle 累计可在需要更细性能归因时补充，
不参与本次功能语义。

现有 `pfIdentified`、`pfDequeued` 和 `pfIssued` 的定义需要保持稳定。特别是
`pfDequeued` 应在请求离开 PFQ 进入 per-slice outstanding 时更新，
`pfIssued` 应继续反映项目当前定义的实际发出位置，不能因批量接口重复计数。

## 精度边界与风险

### 逻辑 Slice PFQ 使用单个物理 list

单个 list 只是仿真数据结构选择，不代表 RTL 有全局仲裁。所有容量、FIFO、
输入带宽和输出带宽判断都必须以 slice 为单位。只要这些外部可见行为得到
保留，物理容器是否拆分不影响模型语义。

### 输入仲裁和地址翻译

目标 slice 只能由物理地址决定。任何尚未完成翻译的候选都不能提前占用 slice
PFQ 写端口。翻译完成事件可能在一个周期内产生多个 ready candidate，因此仍需进入
同一 per-slice 仲裁，而不能直接插入 PFQ。

### 事件顺序

同周期入队、出队、demand squash 和 retry 可能由不同 GEM5 events 触发。
实现必须使用明确的周期状态和提交顺序，不能依赖 event 注册顺序得到 RTL
优先级。需要重点检查：

- full slice PFQ 同周期 dequeue + enqueue；
- demand 命中即将发送或正在 outstanding 的预取地址；
- slice retry 和新 demand 同周期到达；
- 多来源同周期映射到同一 slice；
- translation completion 与普通来源同周期竞争。

### LLDP

当前 L2 内建 LLDP 有独立的 `getPacket()` 路径。对齐后它不能绕过 per-slice
PFQ，其 physically-ready 请求必须作为单独 source 接入相同的 per-slice 输入
仲裁，每周期最多提供一个候选，优先级为 `despacitoStream` 之后、CDP 之前。

## 验证计划

### 定向功能测试

构造可控的物理地址，使地址分别映射到四个 slices，并检查：

1. 四个不同 source 的候选映射到四个不同 slice 时能同周期全部入队；
2. 同一 slice 的多个候选每周期只有一个获胜；
3. L1 downstream hint 和 L2 请求竞争同一 slice 时 L1 获胜；
4. L2 本地冲突遵循
   `largeBOP > smallBOP > despacitoStream > LLDP > CDP > CMC`；
5. 未获胜请求仍保留在来源 buffer；
6. 同一 source 即使缓存了多个不同 slice 的候选，每周期也只提供一个；
7. L1 source 即使 `localBuffer` 中有多项，每周期也只提供一个；
8. 每 slice 严格保持 FIFO；
9. 第 17 次连续向同一 slice 入队会删除该 slice 最旧请求；
10. 一个 slice 满不会删除其他 slice 的请求；
11. 四个非空 slice 能在同周期各出队一条；
12. 一个 slice 返回 false/retry 时，其他 slices 仍继续发送；
13. full slice PFQ 同周期成功 dequeue + enqueue 时 occupancy 保持 16；
14. demand squash 能删除尚在 PFQ 中的对应预取。

### 统计不变量

运行期间检查：

```text
0 <= occupancy(slice) <= 16
sum(occupancy(slice)) == pfq.size()
enqueue_count(slice, cycle) <= 1
dequeue_count(slice, cycle) <= 1
candidate_count(source, cycle) <= 1
aggregate_enqueue_count(cycle) <= num_slices
aggregate_dequeue_count(cycle) <= num_slices
```

所有 outstanding packet 必须唯一归属于一个 slice，且 timing port 返回 false
后重试的必须是同一个 `PacketPtr`。

### 性能 A/B

选择能够形成 PFQ 积压并覆盖多个 slices 的 workload，对比旧模型和 per-slice
模型：

- 预取识别数和来源分布；
- per-slice occupancy、full drop 和 residence latency；
- 预取 late/useful/unused；
- L2 demand miss 和平均 miss latency；
- slice request buffer occupancy 和 retry；
- IPC。

在没有 PFQ 积压或请求集中到单一 slice 的 workload 中，吞吐收益应较小；
当多个 slice 同时有积压时，`pfqDequeueWidth` 应出现 2 到 4，PFQ residence
latency 应下降。若性能变化来自预取总数大幅增加而不是排队延迟下降，需要
检查输入仲裁、full-drop 和统计计数是否发生了非预期改变。

### 已完成验证

实现完成后执行了以下检查：

- `scons build/RISCV/gem5.opt --gold-linker -j16` 完成全量链接；
- `git diff --check` 通过；
- 修改的 Python 配置文件通过 `python3 -m py_compile`；
- classic L2 的 RISC-V hello smoke 正常退出，确认未通过 wrapper 配置 slices 的
  `L2CompositeWithWorkerPrefetcher` 仍使用原有单队列路径；
- 四流 RISC-V SE 微测试在四 slice、每 slice 16 项、PF buffer 开启配置下
  正常退出。最终统计中 `pfqEnqueueWidth` 和 `pfqDequeueWidth` 的最大值均为
  4，四个 slice 均有大量入队和出队；L1、largeBOP、smallBOP 和 CDP 都获得过
  写端口，同 slice conflict 也被实际触发；
- 同一微测试记录到四个 slice 的发送拒绝和 retry，仿真继续完成，验证了
  per-slice outstanding/retry 状态没有形成全局阻塞；
- 使用测试覆盖参数将每 slice 容量临时改为 1、总容量改为 4 后，四个 slice 的
  `pfqMaxOccupancy` 均保持为 1，并发生 5 次 `pfqDroppedOldest`；通用
  `pfRemovedFull` 总数同样为 5；
- 关闭 PF buffer 的四流测试运行到 400000 条指令限制后正常退出，覆盖了直接
  generation、CDP hit/fill side path 和 sliced ingress；
- 启用 `--l2-enable-lldp` 的 RISC-V hello smoke 正常退出；
- 默认 PF buffer 开启、关闭 PF buffer、启用 L2 LLDP 三种配置均确认
  `queue_size=64`、`pfq_entries_per_slice=16`；
- PFQ 术语改为 slice 后重新运行 aligned L2 + LLDP hello smoke，确认配置只导出
  `pfq_entries_per_slice`，统计组只使用 `slicedPfq`，不再出现旧 bank 命名。

测试输出保存在：

- `/tmp/codex_l2_pfq_classic_hello_20261012_v2`
- `/tmp/codex_l2_pfq_stream_final_20261012_v2`
- `/tmp/codex_l2_pfq_capacity1_20261012_v1`
- `/tmp/codex_l2_pfq_nobuf_stream_20261012_v2`
- `/tmp/codex_l2_pfq_lldp_hello_20261012_v1`
- `/tmp/codex_l2_pfq_slice_rename_20261013_v1`

前五个目录记录术语重命名前完成的行为回归；最后一个目录记录重命名后的配置、
统计名称和运行结果。

当前仍缺少直接注入候选的确定性单元测试，因此以下场景主要由实现不变量、
assert 和集成测试覆盖：精确检查同 slice 每一级 source 优先级、逐项验证 FIFO
请求身份、强制构造 full slice PFQ 同周期 dequeue+enqueue，以及固定阻塞一个
slice 并逐周期检查其他三个 slice 的发送。后续若为 per-slice PFQ 增加独立测试夹具，
应优先覆盖这四项。

## 代码入口

预期涉及的主要文件：

- `src/mem/cache/prefetch/queued.hh`
- `src/mem/cache/prefetch/queued.cc`
- `src/mem/cache/prefetch/sliced.hh`
- `src/mem/cache/prefetch/worker.hh`
- `src/mem/cache/prefetch/worker.cc`
- `src/mem/cache/prefetch/l2_composite_with_worker.hh`
- `src/mem/cache/prefetch/l2_composite_with_worker.cc`
- `src/mem/cache/xs_l2/L2CacheWrapper.hh`
- `src/mem/cache/xs_l2/L2CacheWrapper.cc`
- `src/mem/cache/prefetch/Prefetcher.py`
- `src/mem/cache/xs_l2/CacheWrapper.py`
- `configs/common/PrefetcherConfig.py`

实现应优先保持 `Queued` 的默认单队列行为不变，并通过 L2-specific 扩展点完成
per-slice PFQ。Classic L2、L1 和 L3 预取器不应因本次改动改变队列语义。
