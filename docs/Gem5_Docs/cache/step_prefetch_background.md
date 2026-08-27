# STEP 预取器背景

## 论文信息

- 论文：`STEP: Spatial Footprint Prefetcher with Multi-Point Temporal Triggers`
- 作者：Yuanji Ye、Oliver Lenke、Thomas Wild、Andreas Herkersdorf
- 会议：ISCA 2026，DOI: `10.1109/ISCA66397.2026.00095`
- 论文原文（外部输入，未纳入 Git）：
  `/nfs/home/lixin/myworkspace/simulator/bankconflict/GEM5/step-prefetch.pdf`
  该绝对路径指向主工作树；其他关联 worktree 的根目录不保证有 PDF 副本。

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

这与 event key 的传统取舍相关：简单、高频的 key（例如 page offset）有较高
匹配率和较低存储开销，却更容易混入歧义模式；更具体的 key（例如完整地址或
page number）通常更准确，但 key 空间更大、命中机会更少且需要更多状态。Bingo、
Gaze 等工作是在固定触发时点内改善这一 accuracy--coverage--storage 取舍；STEP
增加的是正交的“何时证据足够”维度，而不是宣称替代更强的 key 或候选聚合
（PDF p.2--4，Sec. II-A/B，Fig. 1/2）。

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
执行的决策点。论文明确规定 FOE 成功后更新 FT 的 `issued`，SOE 会先检查
该位；TOE 段没有单独重述“已发射后的行为”。本仓库将“一次 region 生命周期
至多一次 STEP 发射”作为显式的 GEM5 去重策略，而不是把它写成论文给出的
逐条伪代码（PDF p.4--6，Sec. III-A/D）。

### 置信度判定

FOE/SOE 从 PHT 取最近 `N` 条匹配的完整 footprint，默认 `N=3`。将最新
footprint 与另外 `N-1` 条分别计算 Jaccard 相似度：

```text
J(A, B) = popcount(A & B) / popcount(A | B)
```

所有比较都严格大于阈值 `T=0.75` 时，候选被视为收敛；输出是 footprint
的交集而非并集，以准确率优先。置信度不足时不产生预取，等待下一个
事件。TOE 使用完整三 offset 进行精确匹配，命中时直接输出该完整
footprint（PDF p.4--6，Sec. III-B/D）。论文只明确规定 FOE 恰好单项
命中时需要检查 maturity。它没有为少于 `N`、但多于一项的返回结果给出独立
分支：默认 `N=3` 时，两项候选只有一次 Jaccard 比较，论文没有规定应据此
发射、继续等待，还是采用其他补偿规则；SOE 的单项命中也没有单列规则。
GEM5 对这些中间匹配数边界的选择必须写入实施文档和测试，不能反写为论文
规范。

### FOE 冷启动保护

只有一个 offset 的 FOE 最容易误匹配。论文在 FOE lookup 中追加了
12-bit hashed PC。若 FOE 只命中一条 PHT 项，不能计算跨项相似度，必须
再检查该项的 1-bit `maturity`：新写入项初始不成熟，不允许单项 FOE
发射；写入时若它替换的同一 PHT 位置带有相同 hashed PC，则将新项标为
成熟，近似表示该上下文已重复出现（PDF p.4--5，Sec. III-C）。

### 数据结构和学习流

论文的一个统一 PHT 支持三个事件，避免为 FOE/SOE/TOE 各复制一张表：

| 结构 | 作用 | 论文低存储基线 |
| --- | --- | --- |
| FT | 保存 page、hashed PC、前两个 offset 和 `issued`，过滤少于三次访问的 region | 256 entries，8-way |
| AT | 从第三次访问开始累积 64-bit footprint，并保存前三个 offset | 128 entries，8-way |
| PHT | 以 FO 索引、SO+TO 为 tag；保存 footprint、hashed PC、maturity | 512 entries，8-way |
| PB | 仅在预取队列满时暂存待发 footprint；后续访问触发时重试 | 32 entries |
| DPCT | 识别 dense-PC streaming 的独立检测器，不属于 FT/AT/PHT 主路径 | 8 entries |

Table I 给出了该基线的位级账本；它是论文的低开销 prototype，不是每个参数的
全局性能最优值：

| 结构 | 单项字段 | bits/entry | 容量 | 存储开销 |
| --- | --- | ---: | ---: | ---: |
| FT | tag 36、LRU 3、hashed PC 12、offset 13、`issued` 1 | 65 | 256 | 2.08 KB |
| AT | tag 36、LRU 3、hashed PC 12、FO/SO/TO 18、footprint 64 | 133 | 128 | 2.12 KB |
| PHT | SO/TO tag 12、LRU 3、footprint 64、hashed PC 12、maturity 1 | 92 | 512 | 5.88 KB |
| PB | tag 36、LRU 3、footprint 64 | 103 | 32 | 0.41 KB |
| DPCT | hashed PC 12、LRU 3 | 15 | 8 | 0.015 KB |

FT、AT、PHT 为 8-way；PHT 以 6-bit FO 选择组、以 6-bit SO 和 6-bit TO
构成 tag。论文说明了 page/PC 为 hash 后的字段宽度，但没有规定这些 hash 的
具体函数；GEM5 的 hash、上下文键和 replacement 选择必须作为本地实现细节单独
记录，不能反推为论文已经指定的硬件实现（PDF p.5--6，Sec. III-D/F，Table I）。

FT 首次访问记录 FO；第二次记录 SO；第三次访问先进行 TOE 决策，随后将
region 转入 AT。AT 驱逐时把完整 footprint 和前三个 offset 写入 PHT；PHT 对
FOE 返回最近的 `N` 个 `FO+PC` 匹配项，对 SOE/TOE 逐步使用更完整的 offset
tag。论文总逻辑存储为 10.50 KB，region 为 4 KB、cache line 为 64 B（PDF
p.5--6，Fig. 4、Table I）。

论文主实验将 SOE 发射关闭，因为 `FOE+TOE` 的平均性能最高；SOE 对部分
工作负载仍有价值，因此它是参数，而不是算法中被删除的路径（PDF p.7、p.10，Sec. V/E）。

论文还使用与 Gaze、eBingo 相同的 lightweight dense-PC streaming detector：DPCT
记录最近的 dense PC，使简单流式访问走专用路径，而不持续占用 STEP PHT 的
history capacity 干扰非流式 footprint 学习。因此，DPCT 是论文完整 STEP
prototype 的辅助组件；当前 GEM5 复用既有 stream/stride 的行为只能视作近似，
不能把二者的面积或流式发射行为直接等同（PDF p.6，Sec. III-E；PDF p.7，Sec. IV-C）。

## 实验设置与结果

论文使用 ChampSim，50 M 指令 warmup、100 M 指令统计；仅选无预取 LLC
MPKI 不小于 1 的 trace，共 130 条：SPEC CPU2006 39 条、SPEC CPU2017
39 条、CloudSuite 52 条。主实验是 L2 单核预取，所有方案使用相同的
MSHR 与 prefetch queue 配置（PDF p.6--7，Sec. IV、Table II/III）。

Table II 的基线是 1--8 个 4 GHz、4-wide OoO core（352 ROB），L1-D 为
48 KB/12-way/5 cycles，L2 为 512 KB/8-way/10 cycles，LLC 为每核
2 MB/16-way/20 cycles，DRAM 为 DDR4-3200。STEP 作为平行于 L2C 的
add-on，窥探 L1--L2 总线并向 L2 PQ 插入请求；论文为公平起见使所有对比
预取器在 L2C、相同 MSHR/PQ 条件下运行。eBingo 还使用与 STEP 相同的
DPCT streaming support，因此它是比普通 Bingo 更严格的固定触发对照
（PDF p.6--7，Sec. III-G/IV-A/C，Table II）。

| L2 单核几何平均 speedup（相对 no-prefetch） | SPEC06 | SPEC17 | CloudSuite | 总体 |
| --- | ---: | ---: | ---: | ---: |
| SMS | 1.43x | 1.28x | 1.05x | 1.23x |
| Gaze | 1.45x | 1.32x | 1.05x | 1.24x |
| eBingo | 1.47x | 1.34x | 1.07x | 1.26x |
| STEP | 1.49x | 1.40x | 1.07x | 1.28x |

STEP 的平均准确率为 74%、coverage 为 51%，eBingo 为 73%/50%，Gaze 为
67%/48%。论文的消融表明：关闭 FOE 会损失早期 coverage 和及时性，而准确率
变化较小；关闭 TOE 会使所有 suite 的准确率下降、coverage 基本不变，且在
SPEC17 与 CloudSuite 更明显；关闭 SOE 的主配置总体最好，但
`cactuBSSN`、`sphinx3`、`roms` 等部分程序仍从 SOE 获益（PDF p.8、p.10，
Fig. 5、6、12、13）。

这里的指标必须和 GEM5 stats 分开理解。论文的定义及分母是：

```text
accuracy        = N_useful / (N_useful + N_useless)
coverage        = (N_miss_base - N_miss_pf) / N_miss_base
overprediction  = (N_useless + N_cache_hit) / N_miss_base
```

其中 `N_useful` 是发出后、驱逐前被 demand 消费的预取，`N_useless` 是驱逐前
未被使用的预取，`N_cache_hit` 是目标 line 已在 cache 中的重复预取；
`N_miss_base` 和 `N_miss_pf` 是同一 cache level 的 no-prefetch 与有预取 demand
load miss。论文将 overprediction 以 `N_miss_base` 的百分比报告（PDF p.7，
Sec. IV-D）。因此，在 GEM5 侧复刻 coverage 必须有同配置、同切片的无预取
对照；任一单独的 `pfUseful`、`pfUnused` 或 source 计数都不能等同论文 coverage。

其他边界结果包括：L1 单级 STEP 总体 1.28x，高于 Gaze 1.25x 和 eBingo
1.23x；限制预取 line 只能占一个 L2 way 后，STEP 仍为 1.263x、eBingo 为
1.245x。这一 limited-way 实验主动缓解不准确或不及时预取引起的 cache pollution，
是检验 STEP 收益是否仅来自低污染的压力测试；结果说明分阶段 trigger 仍能通过
更早机会和更晚消歧带来价值，而非只改善污染。

Fig. 19 的 storage sweep 显示 STEP 在低到中等 metadata 容量已较强，约 32 KB
后继续扩容的边际收益变小、曲线趋平；增强 Bingo/eBingo 则需远超过 100 KB
metadata 才接近 STEP 约 10 KB 的工作点。该图对不同设计按各自主要 history
结构扩容（STEP/Gaze/Bingo 主要扩 PHT，IPCP 扩 IP table，vBerti 扩 history/delta
table），因此它是论文的性能--存储关系证据，不是 GEM5 中单一表项参数的直接
推荐值（PDF p.12，Sec. V-I，Fig. 19）。

多核实验同时覆盖 homogeneous 和 heterogeneous workload：前者复制同一 trace
到所有 core；后者用实验日期作为固定随机种子，在 2/4/8 core 各构造 50 个
mixed-workload groups。所有 core 都执行到各自 measurement window 结束，因而
这不是任意混合或只等最先完成 core 的构造。homogeneous 中 STEP 在 1--8 core
始终领先，且随 core 数增加相对优势更明显；heterogeneous 共享资源干扰更强，
STEP 仍处于最强组，但 eBingo 在 8 core 可与其持平。因此不能把论文结论简化为
所有多核点都严格领先（PDF p.6、p.8--9，Sec. IV-A/V-B，Fig. 9）。

论文的 ChampSim trace 机制案例说明收益并非单一来源，而不是 GEM5 的 `mcf`
验证结果。`mcf-192` 中一个 PC 在短时间内跳过许多 page；与其他访问交错时，FT
必须同时跟踪许多 active page，晚触发常在第二次访问前丢失上下文，FOE 因而能
抢到短生命周期机会。`mcf-484` 的 region `0xfe3` 在 SOE 仍有 C0/C1/C2 三个
候选；TOE 才选中实际 demand footprint。若提前选 C1，会多发 30 条无用 line；
选 C2 则会多发 12 条无用 line 并漏掉后来 demand 的 11 条。后一个案例里 eBingo
反而更快，因为其 richer fixed-trigger key 能更早区分部分 pattern；STEP 并非逐
trace 无条件支配（PDF p.10--11，Sec. V-F，Fig. 15/16）。

多级 L1+L2 组合并非可简单相加，因为 L1 会改变 L2 看到的学习流并引入额外
带宽竞争。论文中 `STEP+STEP` 和 `STEP+eBingo` 的总体几何平均 speedup 均为
1.277x，随后是 `Gaze+STEP` 1.266x 和 `STEP+Gaze` 1.262x；这说明 L1 或 L2
的 STEP 都可有效，但不能据此直接推导 KMHv3 的最佳层级组合（PDF p.9，Sec. V-D，Fig. 11）。

参数与系统敏感性实验也限定了“论文基线”的含义：FT 从 32 增至 256 entries
持续改善、之后边际收益变小；AT 增至 256 后饱和；PHT 相联度从 8-way 增至
128-way 只使总体 speedup 从 1.28x 增至 1.29x。论文还在 DRAM 带宽、LLC 和
L2 容量 sweep 中保持相对领先；这些是设计鲁棒性证据，不是 KMHv3 参数的直接
推荐值（PDF p.11--12，Sec. V-G/H/I，Fig. 17--19）。

## 对 GEM5 落地的约束

1. 论文使用 ChampSim L2 trace，当前仓库使用 KMHv3 全系统 checkpoint、
   L1 `XSCompositePrefetcher` 和多级预取转发；数值不能外推。
2. STEP 的核心依赖多条原始 footprint。当前 `sms.cc` PHT 只保存一条
   相对 offset 饱和计数器向量，不能拿它伪造 Jaccard 或交集。
3. 当前复合预取器已经提供训练入口、上下文隔离、去重过滤、发射队列和
   多级 hint 通道；STEP 复用这些通用机制，并保持独立的 FT/AT/PHT 与
   `stepPb`。它不能复用 legacy SMS 的 `sms_pfFilter`，否则 STEP 与
   SMS 会错误地共享缓冲状态和仲裁来源。
4. 初版必须使 `enable_step=False` 时逐位保持原 SMS 路径，且 `enable_step=True`
   时用 STEP 替换 SMS PHT 的发射，而非把两个 PHT 叠加后把收益归给 STEP。
5. 初版以论文的低存储基线为中心：4 KB region、FT/AT/PHT
   `256/128/512` entries、8-way、`N=3`、`T=75%`、SOE 关闭。这些是论文
   L2C prototype 的基线，不是对 KMHv3 或论文全参数空间的最优性结论。论文的
   敏感性实验显示 FT 增长至 256 后收益趋缓、AT 增长至 256 后饱和、PHT 从
   8-way 增至 128-way 只将总体 speedup 由 1.28x 提至 1.29x；后续 DSE 应以
   这些趋势约束搜索范围。
6. `step_pf_level` 是单变量的落点选择：当前实现支持 L1、L2、L3 三个
   target level。L1 为本层请求，L2/L3 使用既有 `pfahead` 通道；其性能必须
   在相同 checkpoint 和其他参数不变的条件下分别验证。

## 论文结构与 GEM5 对应

| 论文结构 | 当前 GEM5 对应 | 可比性与边界 |
| --- | --- | --- |
| FT / AT / PHT | `StepSpatialPrefetcher` 的三张独立 `AssociativeSet` | 保留三次访问、footprint、容量和组相联替换等核心状态；GEM5 为了从一个 set 中选最近 history，按 `sequence` 排序，属于实现选择。 |
| 置信度 evaluator | `footprintsConverge()` 与 `intersectFootprints()` | 保留 `N=3`、严格大于 75% 的 Jaccard 和交集发射。 |
| PB | `stepPb` 加既有 `Queued` | 论文仅在 PQ 满时暂存且由未来访问重试；GEM5 每个通过预检的候选都先进入 PB，并在后续 cycle drain。两者都受有限容量约束，但不是逐周期等价。 |
| dense-PC streaming (DPCT) | 保留仓库既有 stream/stride 组件 | 当前没有实现 STEP 私有 DPCT；它与 STEP 主空间 footprint 路径分开，不能把完整组合称为论文中的完整 STEP。 |
| 10.50 KB 面积 | 无直接对象 | 论文统计含 DPCT，且硬件 PB 只存 tag/footprint；GEM5 PB 还保存 trigger、上下文和 C++ 对象。不能把 `sizeof` 或 entry 数直接与论文面积比较。 |
