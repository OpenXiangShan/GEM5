# STEP 预取器背景

## 论文信息

- 论文：`STEP: Spatial Footprint Prefetcher with Multi-Point Temporal Triggers`
- 作者：Yuanji Ye、Oliver Lenke、Thomas Wild、Andreas Herkersdorf
- 会议：ISCA 2026，DOI: `10.1109/ISCA66397.2026.00095`
- 本地原文：工作区根目录的 `step-prefetch.pdf`

本文中的论文页码以 PDF 物理页为准。所有性能数字都是论文在
ChampSim trace 环境中的结果，不能直接视为 KMHv3/GEM5 的预期收益。

## 背景与动机

空间足迹预取器把一个 region 内被访问的 cache line 表示为位图。经典
SMS 使用三个生命周期结构：FT 过滤低活跃 region，AT 为活跃 region
累积足迹，AT 驱逐时把完成的足迹与事件键写入 PHT；后续同类事件命中
PHT 后发出相应 footprint 的预取（论文 PDF p.2，Sec. II-A）。

传统 footprint 预取通常固定在一个时点决策。首访问触发能尽早隐藏延迟，
但只有很少上下文，容易将不同模式混在一起；晚访问触发有更多 offset
上下文，预测更准确，却可能等不到第二次或第三次访问，错过短暂的预取
机会。增加更丰富的 key、fallback 或候选聚合可以改善固定时点的命中质量，
但不能回答“当前证据是否已经足够发射”这个问题（PDF p.1--3，Fig. 2）。

STEP 将触发时间本身变成运行时决策：在同一 region 的多个连续访问点都
允许查询历史；若候选 footprint 已经收敛则立即发射，否则等待更多 offset
消除歧义。它因此同时保留早触发的机会和晚触发的判别力，而不是选择其中
一个固定时点。

## 算法设计

### 三个时序事件

- `FOE`（first-offset event）：观察到第一个 offset，机会最早但歧义最大。
- `SOE`（second-offset event）：观察到前两个 offset。
- `TOE`（third-offset event）：观察到前三个 offset，事件键最具体。

FOE、SOE、TOE 不是三套互斥预取器，而是一个 region 生命周期内逐级
执行的决策点。已经在前一阶段发射的 region 不会在后续阶段重复发射
footprint（PDF p.4--6，Sec. III-A/D）。

### 置信度判定

FOE/SOE 从 PHT 取最近 `N` 条匹配的完整 footprint，默认 `N=3`。将最新
footprint 与另外 `N-1` 条分别计算 Jaccard 相似度：

```text
J(A, B) = popcount(A & B) / popcount(A | B)
```

所有比较均不小于阈值 `T=0.75` 时，候选被视为收敛；输出是 footprint
的交集而非并集，以准确率优先。置信度不足时不产生预取，等待下一个
事件。TOE 使用完整三 offset 进行精确匹配，命中时直接输出该完整
footprint（PDF p.4--6，Sec. III-B/D）。

### FOE 冷启动保护

只有一个 offset 的 FOE 最容易误匹配。论文在 FOE lookup 中追加了
12-bit hashed PC。若 FOE 只命中一条 PHT 项，不能计算跨项相似度，必须
再检查该项的 1-bit `maturity`：新写入项初始不成熟，不允许单项 FOE
发射；写入时若它替换的同一 PHT 位置带有相同 hashed PC，则将新项标为
成熟，近似表示该上下文已重复出现（PDF p.4--5，Sec. III-C）。

### 数据结构和学习流

论文的一个统一 PHT 支持三个事件，避免为 FOE/SOE/TOE 各复制一张表：

| 结构 | 作用 | 论文默认配置 |
| --- | --- | --- |
| FT | 保存 page、hashed PC、前两个 offset 和 `issued`，过滤少于三次访问的 region | 256 entries，8-way |
| AT | 从第三次访问开始累积 64-bit footprint，并保存前三个 offset | 128 entries，8-way |
| PHT | 以 FO 索引、SO+TO 为 tag；保存 footprint、hashed PC、maturity | 512 entries，8-way |
| PB | 预取队列满时保留待发 footprint | 32 entries |

FT 首次访问记录 FO；第二次记录 SO；第三次访问先进行 TOE 决策，随后将
region 转入 AT。AT 驱逐时将完整 footprint 写入 PHT。FOE 用 `FO+PC` 匹配，
SOE 用 `FO+SO` 匹配，TOE 用 `FO+SO+TO` 匹配。论文总逻辑存储为 10.50 KB，
region 为 4 KB、cache line 为 64 B（PDF p.5--6，Fig. 4、Table I）。

论文主实验将 SOE 发射关闭，因为 `FOE+TOE` 的平均性能最高；SOE 对部分
工作负载仍有价值，因此它是参数，而不是算法中被删除的路径（PDF p.7、p.10，Sec. V/E）。

## 实验设置与结果

论文使用 ChampSim，50 M 指令 warmup、100 M 指令统计；仅选无预取 LLC
MPKI 不小于 1 的 trace，共 130 条：SPEC CPU2006 39 条、SPEC CPU2017
39 条、CloudSuite 52 条。主实验是 L2 单核预取，所有方案使用相同的
MSHR 与 prefetch queue 配置（PDF p.6--7，Sec. IV、Table II/III）。

| L2 单核几何平均 speedup（相对 no-prefetch） | SPEC06 | SPEC17 | CloudSuite | 总体 |
| --- | ---: | ---: | ---: | ---: |
| SMS | 1.43x | 1.28x | 1.05x | 1.23x |
| Gaze | 1.45x | 1.32x | 1.05x | 1.24x |
| eBingo | 1.47x | 1.34x | 1.07x | 1.26x |
| STEP | 1.49x | 1.40x | 1.07x | 1.28x |

STEP 的平均准确率为 74%、coverage 为 51%，eBingo 为 73%/50%，Gaze 为
67%/48%。论文的消融表明：关闭 FOE 主要损失 SPEC06 的及时性；关闭 TOE
主要降低复杂 workload 的准确率；关闭 SOE 的主配置总体最好，但
`cactuBSSN`、`sphinx3`、`roms` 等部分程序仍从 SOE 获益（PDF p.8、p.10，
Fig. 5、6、12、13）。

其他边界结果包括：L1 单级 STEP 总体 1.28x，高于 Gaze 1.25x 和 eBingo
1.23x；限制预取只能占一个 L2 way 后，STEP 仍为 1.263x、eBingo 为 1.245x；
存储由约 10 KB 扩到约 32 KB 后 STEP 收益趋于饱和（PDF p.9、p.12--13，
Fig. 10、19、20）。

## 对 GEM5 落地的约束

1. 论文使用 ChampSim L2 trace，当前仓库使用 KMHv3 全系统 checkpoint、
   L1 `XSCompositePrefetcher` 和多级预取转发；数值不能外推。
2. STEP 的核心依赖多条原始 footprint。当前 `sms.cc` PHT 只保存一条
   相对 offset 饱和计数器向量，不能拿它伪造 Jaccard 或交集。
3. 当前复合预取器已经提供训练入口、上下文隔离、去重过滤、发射队列和
   多级 hint 通道；STEP 应复用这些通用机制，只新增自己的 FT/AT/PHT。
4. 初版必须使 `enable_step=False` 时逐位保持原 SMS 路径，且 `enable_step=True`
   时用 STEP 替换 SMS PHT 的发射，而非把两个 PHT 叠加后把收益归给 STEP。
5. 初版参数采用论文的 4 KB region、256/128/512 entries、8-way、`N=3`、
   `T=75%`、SOE 默认关闭；它们将在后续 DSE 中作为中心点，而非宣称已针对
   KMHv3 最优。
