# SMS 历史 first-touch 顺序调度设计

## 1. 文档范围

本文档固化 `XSCompositePrefetcher` 中 SMS/PHT 预取请求的第一阶段调度改进：

- PHT 仍决定哪些 offset 可以成为候选；
- region 之间继续使用当前的 round-robin 仲裁；
- 只改变选中 region 内 offset 的发送顺序；
- offset 顺序只依据历史 first-touch order；
- 不使用 PHT counter 大小、地址距离、region 年龄、请求年龄或 late/useful 反馈参与排序。

本文档是实现前的设计合同，不代表 `sms.cc` 已经完成对应修改。源码行为仍是最终依据。

## 2. 动机与目标

当前 SMS filter 在一个 region 内按地址方向发送：

- forward 模式选择最低的 pending offset；
- backward 模式选择最高的 pending offset。

这种顺序不表示历史 demand 的先后关系。目标是让发送顺序更接近历史上该 PC 在一个 region generation 中第一次触碰各个 block 的顺序，从而优先发出更早可能被 demand 使用的候选。

本方案只研究 offset 调度，不改变候选集合。这样可以把实验因果链限制为：

```text
历史 first-touch 顺序
    -> filter 内 offset 选择
    -> 预取 issue 顺序
    -> late / useful / coverage
```

## 3. 不变量

### 3.1 Counter 的职责边界

现有 PHT `hist` counter 继续使用原来的阈值判断候选是否成立：

```text
hist[delta].calcSaturation() > 0.5
```

但是，一旦多个 offset 都通过候选判断，`hist` 的数值大小不得参与它们之间的排序。换句话说：

```text
counter 负责 candidate membership
first-touch order 负责 candidate ordering
```

不能使用如下组合优先级：

```text
counter + order
counter * order
counter 优先、order 次优先
```

### 3.2 Region 仲裁不在本阶段修改

`phtSentPrefetch[0..2]` 的 current/increment/decrement region 生成方式、`phtSendEventWrapper()` 的插入顺序，以及 `sms_pfFilter` 的 region-level round-robin 均保持不变。

本方案只改变已经选中的一个 region 内，哪个 pending offset 先发送。

### 3.3 Tie-break

如果两个候选的 learned order 相同，唯一的确定性选择规则是 region-relative `offset index` 较小者优先。

```text
orderScore 相同 -> offset index 较小者优先
```

不再使用 `decr_mode` 作为 order 相同情况下的替代顺序。`decr_mode` 可以继续保留为 entry 元数据，但 first-touch policy 下不参与 offset 仲裁。

## 4. First-touch 的定义

first-touch 指一个 ACT region generation 内，某个 offset 第一次令 `regionBits[offset]` 从 0 变成 1 的事件。

例如，训练可见的访问序列为：

```text
offset 3 -> 8 -> 5 -> 12
```

则该 generation 的 touch rank 为：

```text
offset 3  -> rank 0
offset 8  -> rank 1
offset 5  -> rank 2
offset 12 -> rank 3
```

rank 是顺序编号，不是 cycle timestamp。两次 touch 之间的时间间隔不在本方案中建模。

当前 SMS 的训练入口只在 `cache miss` 或 `prefetch first hit` 等训练可见事件上更新 ACT。因此这里的顺序是：

> SMS training-visible first-touch order

它不等同于程序的所有 demand hit 的完整时间顺序。

## 5. ACT 状态

每个 `ACTEntry` 增加一个固定长度的 touch-rank 数组：

```text
touchOrder[regionBlks]
```

建议语义：

- `touchOrder[offset]` 保存该 offset 在当前 generation 中的 rank；
- 未访问 offset 使用 invalid 标记；
- `accessCount` 继续表示已经观察到的不同 offset 数量，同时作为下一个 rank；
- `regionBits` 继续作为 offset 是否已经访问过的 valid bitmap。

新 region 分配：

```cpp
regionBits = 1 << trigger_offset;
touchOrder[trigger_offset] = 0;
accessCount = 1;
```

ACT 命中且观察到新 offset：

```cpp
if (!(regionBits & (1ULL << offset))) {
    touchOrder[offset] = accessCount;
    accessCount++;
    regionBits |= 1ULL << offset;
}
```

重复访问同一 offset 不得修改它的 rank。

## 6. PHT 的历史 order

PHT 保留现有 `hist[2 * (regionBlks - 1)]`，并增加与 delta 对齐的 order 状态：

```text
orderScore[2 * (regionBlks - 1)]
orderValid[2 * (regionBlks - 1)]
```

每个 `orderScore` 表示该相对 delta 的历史 first-touch rank，数值越小表示历史上越早出现。建议使用 Q4 定点表示：

```text
sample = touchRank << 4
```

初次观察该 delta：

```text
orderScore = sample
orderValid = true
```

后续 generation 观察到同一 delta：

```text
orderScore = orderScore + (sample - orderScore) / 4
```

这里的 `1/4` 是 order 历史的平滑系数，不是 PHT counter，也不参与候选资格判断。第一版可将其作为固定默认值；如果后续需要探索，再抽象为 `sms_order_ewma_shift` 参数，默认值为 2。

某次 generation 没有访问某个 delta 时，不更新该 delta 的 order。未出现信息仍由原有 PHT confidence 负责，不能把“本次未出现”编码成一个假的 late rank。

## 7. Early update 与重复训练

当前实现可能在 ACT 命中路径上 early-update PHT，而在 ACT 淘汰时再次更新 PHT。order 信息必须避免同一 generation 对同一 delta 重复贡献。

因此 ACT 还需要维护：

```text
orderTrainedBits
```

其用途是标记该 generation 中哪些 offset/delta 已经写入 PHT order：

- PHT 已存在时，新 offset first touch 可以立即更新对应 order，并设置标记；
- PHT 在访问中途首次分配时，用 ACT 中已经收集的 `touchOrder` 初始化 order，并设置相应标记；
- ACT 淘汰时，只补训尚未标记的 offset/delta；
- ReACT 对 confidence 的额外增强不应重复增强 order。

confidence update 和 order update 的触发条件必须分开：confidence 保持现有语义，order 只接受 generation 内第一次 touch。

## 8. PHT lookup 与 filter 传递

`phtLookup()` 仍按现有逻辑生成三类 region 位图：

```text
current region
increasing-address region
decreasing-address region
```

对每个生成的 bit，同时携带对应的 `orderScore`。因此 `phtsentInfo` 不能只保存 `region_bits`，还要保存该 region 每个 offset 的 order payload。

顺序链路为：

```text
PHT candidate bit + orderScore
    -> phtSentPrefetch
    -> sms_pfFilter.Insert()
    -> PrefetchFilter::Entry
    -> GetPFAddrL{1,2,3}()
```

当同一个 filter entry 已存在并再次插入 region bits 时，采用 first-writer-wins：

- 之前已经存在的 bit 保留原 order；
- 只有新加入 `region_bits` 的 bit 写入 incoming order；
- 已经在 `filter_bits` 中的 bit 仍不会再次发送。

这样可以避免后续重复 PHT lookup 不断重排已经等待中的请求。

## 9. Offset 发送算法

region 选中后仍先计算：

```text
pending = region_bits & ~filter_bits
```

新策略在 pending 中选择 learned order 最小者：

```cpp
best_offset = invalid;
for (offset = 0; offset < regionBlks; ++offset) {
    if (!(pending & (1ULL << offset)))
        continue;

    if (best_offset == invalid ||
        orderScore[offset] < orderScore[best_offset] ||
        (orderScore[offset] == orderScore[best_offset] &&
         offset < best_offset)) {
        best_offset = offset;
    }
}
```

选择后沿用现有流程：

1. 从 filter entry 取出该 offset 的 trigger metadata；
2. 设置 `filter_bits[offset]`；
3. 更新 filter replacement state 和 round-robin pointer；
4. 生成一个 `AddrPriority`；
5. 交给后续 PFQ/L2 issue 流程。

`regionBlks` 默认是 16，因此 offset 选择是有界的 O(`regionBlks`) 扫描，不引入无界搜索或全表排序。

## 10. 状态规模与复杂度

默认 `regionBlks = 16`、PHT delta 数为 30 时，新增逻辑状态约为：

| 位置 | 状态 | 估算规模 |
|---|---|---:|
| ACT entry | `touchOrder[16]` | 16 个小整数 |
| ACT entry | `orderTrainedBits` | 64 bit（按 PHT delta 标记，当前使用 30 bit） |
| PHT entry | `orderScore[30]` | 30 个 Q4 score |
| PHT entry | `orderValid[30]` | 30 bit |
| SMS filter entry | 每个 offset 的 order payload | 16 个 Q4 score |

复杂度保持为：

- ACT 新 offset 训练：O(1)；
- PHT generation：现有有限的 `regionBlks` 扫描，额外携带 order，不改变数量级；
- filter merge：O(新增 bit 数)；
- 每次 offset 发送：O(`regionBlks`)；
- region 选择：保持当前最多扫描 filter 表容量的 round-robin。

## 11. 建模合同

实现使用 `enable_sms_first_touch_order` 作为 A/B 开关：SimObject 默认值为
`False`，从而保持通用配置的旧发送顺序；`kmh_align` profile 显式设为
`True`。该参数只控制 first-touch 元数据的学习/传递以及最终 offset
选择器，不控制 PHT 候选、region 仲裁、filter 插入、目标层级或请求数量。

### 性能问题

本设计只针对预取 buffer 内同一 region 的 offset 竞争，目标是改变请求到达 L2 issue boundary 的顺序，观察 late/useful/coverage 变化。

### 控制状态

```text
ACT observe
    -> first-touch rank assigned
    -> PHT order learned
    -> candidate bit generated by existing confidence
    -> filter pending
    -> region selected by existing RR
    -> offset selected by learned order
    -> filter_bits marked
    -> request issued
```

### 保留的细粒度行为

- ACT generation 内第一次 touch 的顺序；
- PHT delta 与 region offset 的映射；
- filter 中 pending bit 的消费顺序；
- 每次只发送一个 block 的现有带宽约束。

### 有意不建模的行为

- touch 之间的 cycle 间隔；
- demand deadline 和内存返回延迟；
- PHT counter 的强弱排序；
- region 年龄与全局跨 region touch 顺序；
- late/useful 反馈对 order 的在线修改。

这些因素属于后续独立实验，不能在本方案中混入。

## 12. 可观测性与验证

实现增加以下 stats：

```text
smsOrderUpdates
sms_pfFilter.orderSelections
sms_pfFilter.orderTieSelections
sms_pfFilter.orderChangedSelections
sms_pfFilter.orderSelectedByRank[0..regionBlks]
```

其中：

- `orderChangedSelections` 表示新策略选择的 offset 不同于旧的 ctz/clz 顺序；
- `orderTieSelections` 用于判断 Q4 量化是否导致大量退化到 offset index；
- `orderSelectedByRank` 的最后一个 bucket 表示该 offset 没有有效 order 历史。

最小验证应包含：

1. 单 region，访问顺序为 `3 -> 8 -> 5 -> 12`，确认发送顺序为 `3/8/5/12` 对应的预测 rank 顺序；
2. 同一个 offset 重复访问，确认 rank 不变；
3. 多次 generation，确认 order 使用 EWMA 而不是最近一次样本覆盖；
4. 两个 offset order 相同，确认永远选择较小 offset index；
5. 同一 region 重复 filter insert，确认 existing pending bit 的 order 不被覆盖；
6. 对照 baseline，确认 candidate region bits 不变，变化只来自 offset issue 顺序。

SPEC06 A/B 需要比较：

```text
SMS issued
SMS useful / issued
SMS late / issued
SMS accuracy（useful / (useful + unused)）
SMS coverage
sms_pfFilter.orderChangedSelections
sms_pfFilter.orderTieSelections
```

如果 candidate bitset 相同但 late 没有下降，说明主要瓶颈不在 offset 仲裁，而可能在 PHT 触发时机、region 选择或下游带宽。

## 13. 风险与边界

1. first-touch 只覆盖 SMS training-visible 事件，不保证等于真实 demand 的完整顺序。
2. orderScore 是历史平均顺序，不是当前请求的 deadline；它可能优先发送一个已经来不及的早期 offset。
3. PHT/filter entry 被替换时，order 历史会随 entry 丢失，这是容量模型的一部分。
4. `phtSentPrefetch[0..2]` 仍是有限槽位；上游覆盖会丢失某些候选及其 order，本方案不修复该问题。
5. first-writer-wins 保证等待中请求稳定，但可能保留一个已经过时的 order；这是为了隔离本实验变量而接受的取舍。

## 14. 设计总结

本方案的最终规则是：

```text
PHT counter：只决定 offset 是否进入候选集合
ACT：记录 generation 内每个 offset 的 first-touch rank
PHT：对相对 delta 学习 first-touch rank 的 EWMA
Region：保持现有 round-robin
Offset：pending 中 learned order 最小者优先
Tie：只按 offset index 较小者优先
```

因此该方案是一个单变量、可归因的 offset-order A/B 实验：它不改变 SMS 预测了什么，只改变同一批预测请求在 buffer 中先发送什么。
