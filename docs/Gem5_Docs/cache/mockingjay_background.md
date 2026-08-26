# Mockingjay 背景

## 来源

本文总结 Ishan Shah、Akanksha Jain 和 Calvin Lin 在 HPCA 2022 发表的论文
《Effective Mimicry of Belady's MIN Policy》（DOI:
`10.1109/HPCA53966.2022.00048`）。原始论文的本地副本是原工作区中的
`shah2022.pdf`，下文的章节和图号均指向该论文。

## 动机

Belady 的 MIN 策略会淘汰下一次使用距离最远的 cache line，但模拟器无法直接
知道未来的访问流。SHiP、Hawkeye 等学习型策略通常把问题压成二分类：cache
line 是 Cache Friendly 还是 Cache Averse，或者之后会不会复用。论文指出这种
粗粒度分类有两个问题：

* 小的预测误差会直接翻转类别，而不是只小幅改变淘汰顺序。
* 同一类别中的 line 经常出现平局，策略只能退回 LRU。

较早的复用距离策略 KPK 和 IbRDP 虽然预测了预计到达时间（ETA），却使用
`max(ETR, age)` 选择 victim。接近预测复用点时，age 可能覆盖 ETA，方向与
MIN 的目标相反。KPK 的历史长度也不足，IbRDP 则使用全局访问距离而不是目标
set 的访问距离。论文报告 IbRDP 的复用距离预测准确率为 43%，Mockingjay 为
85%，提升主要来自更长的历史以及按受影响 set 统计距离。

Mockingjay 因此为每个 load PC 预测多档复用距离，再转换为 ETA/ETR；只有在
确实需要替换时才比较各 line 的优先级。预测误差只有在改变候选 line 的相对
ETA 顺序时才会改变 victim，因此比二分类策略更稳健。

## 设计

预测器以 PC 为索引，而不是联合 PC 和地址。地址用于定位 cache set，并在采样
历史中识别同一个 block。采样历史保存最近访问该 block 的 PC，再用此前的 PC
训练 RDP。

### 采样 cache

采样 cache 只为少量物理 cache set 保存较长历史。每条记录包含 block 地址的
tag/hash、最近访问时间戳和最近的 PC signature。访问采样 set 时：

1. 命中时，用同一 set 内经过的访问次数训练旧 signature，然后用当前
   signature 和时间戳刷新记录。
2. 未命中时，淘汰采样 cache 的 LRU 记录；因为该 block 在历史窗口内没有再次
   出现，旧 signature 按 scan、距离 `INF_RD` 训练。
3. 最后递增该采样 set 的时间戳。时间戳只在同一个采样 set 内比较；实现要求其
   模数严格大于采样历史窗口，避免历史边界恰好回绕成零。整整一个计数周期都没
   有访问的记录仍只能按有界历史近似处理。

论文的单核 2 MB、16-way LLC 使用 8 倍于 associativity 的历史，即 128 次
 set 访问，并用 32 个采样 set 和 512 个 5-way 采样 cache bucket 表示。历史
 只保存唯一 block，不保存数据内容。

### 复用距离预测器

Reuse Distance Predictor（RDP）是 direct-mapped 表。论文的单核配置把 PC、
hit/miss 状态的 11 位 hash 映射到 7 位复用距离；带预取器的形式还加入预取
标志。新 signature 用采样距离初始化，已有表项用 temporal-difference 规则向
样本移动，并限制离群样本的影响。

`INF_RD` 表示 scan。论文使用 `INF_RD = 127`，并把接近该值的预测视为 scan；
公开 ChampSim 实现使用可缩放形式 `INF_RD = associativity * 8 - 1`、
`MAX_RD = INF_RD - 22`。本实现采用后者，使 8-way L2 slice 仍保持 8 倍
associativity 的历史长度。

### ETR 与 victim 选择

插入或命中提升时，RDP 预测初始化 line 的 Estimated Time Remaining（ETR）。
ETR 是 ETA 的粗粒度表示：每访问同一 set 八次，所有非 scan line 的 ETR 减一；
ETR 经过零点后仍保留符号。正值表示预测复用尚未到达，负值表示预测时间已
经过。scan line 不参与 aging。

发生 miss 时，Mockingjay 选择 `abs(ETR)` 最大的有效 line；绝对值相同时优先
负 ETR。这保持了直接的 ETA 顺序。论文还定义了对低复用预测进行缓存旁路
（cache bypass）：预测为 scan，或其预测 ETR 大于当前 victim 的绝对 ETR 时，
均可不保留该 fill。本 GEM5 端口不改动 cache 的正常 fill/响应流程；它在更新
采样历史之前保留 scan 判定和预测 ETR，并将命中对应条件的 line 正常写入 cache、同时把 ETR
设为 `+INF_ETR`。随后正常 victim 选择会优先于所有绝对 ETR 更小的 line 淘汰
它；若存在 `-INF_ETR` writeback，则既有的负 ETR 平局规则会先选 writeback。
这样可以保留替换趋势，同时不引入跨 BaseCache、tags 和 MSHR 的旁路接口。
writeback 继续使用低优先级的负 scan ETR。

有预取器时，论文用 Flex-MIN 的近似方法处理以预取结束的采样区间（`*-P`）：
放大训练得到的复用距离。单核使用 2 倍，多核使用 1.5 倍，并在 `INF_RD` 处
饱和。

## 论文中的实验结果

论文在 ChampSim 的 2 MB、16-way LLC 上评估了 33 个内存敏感的 SPEC06、
SPEC17 和 GAP 程序，以及 100 组四核混合 workload；所有策略的 replacement
state 预算为 32 KB。

| 配置 | Mockingjay 相对 LRU | 对照策略 |
| --- | ---: | --- |
| 单核、无预取器 | IPC +5.7% | SHiP +3.4%，Hawkeye 约 +4.4% |
| 四核、无预取器 | 加权 speedup +15.2% | SHiP +7.6%，Hawkeye +12.9% |
| 单核、有预取器 | +3.6% | Harmony +2.0% |
| 四核、有预取器 | 加权 speedup +13.3% | SHiP +6.7%，Harmony +11.1% |
| 高 MPKI CVP、有预取器 | +20.1% | Harmony +13.4% |

论文还报告：将 IbRDP 改为按 set 统计距离后，预测准确率从 43% 提升到 85%；
Mockingjay 退回类似 LRU 淘汰的比例为 7.8%，Hawkeye 为 13.8%。在论文评估
的单核预取配置中，相比 Hawkeye，uncore energy 低 9.1%，DRAM traffic 低
9.8%。

## 迁移到 L2 的注意事项

论文评估的是 LLC，而不是 Kunminghu 的切片私有 L2。因此论文数字只能作为
机制依据，不能直接当作本端口的性能目标。在目标层次中，L1 filtering 会改变
PC 分布和复用距离，每个 L2 slice 只观察路由到自己的地址子集。实现必须保持
按 set 统计距离的语义，再通过约定的 GCC15 SPEC06 CI A/B 实验确认收益。
