# Mockingjay L2 实现约定

## 范围与归属

目标是 `configs/example/kmhv3.py` 使用的对齐 L2 路径。`L2CacheWrapper` 负责
请求路由并拥有 wrapper 级共享预取器；每个 `L2CacheSlice` 则拥有独立的
`inner_cache`。因此 replacement policy 必须挂在每个 `inner_cache` 上，使
每个 `(CPU, slice)` 都有独立的采样 cache、RDP、set clock 和 ETR 状态，任何
预测器状态都不能跨 slice 共享。

实现新增 `src/mem/cache/replacement_policies/mockingjay_l2_rp.{hh,cc}`，将
其注册为 SimObject，并在 `kmhv3.py` 遍历 `l2_wrapper.slices[j]` 时为每个
`inner_cache` 创建新对象。缓存 wrapper、请求路由和原有 replacement-policy
接口保持不变；不在 `BaseCache`、tags 或 MSHR 中加入 Mockingjay 专用路径。

## 建模合同

| 项目 | 约定 |
| --- | --- |
| 性能问题 | 建模 L2 容量 miss 中由淘汰顺序不佳造成的损失，以及低复用 line 被留下过久的问题。 |
| 可观察后果 | line 可以被插入、命中提升、选为正/负 ETR victim，或以 `+INF_ETR` 插入并在后续正常选择中优先于绝对 ETR 更小的 line 淘汰。 |
| 控制状态机 | `hit -> 采样历史更新 -> set aging -> 提升`；fill 走原有 cache 分配流程，`reset` 完成采样、aging 和 ETR 插入。 |
| 资源状态 | 固定容量 RDP、固定大小采样 cache bucket、每 set 的有限 line 指针、每 line 的 ETR、每 set 的 clock/时间戳。 |
| 距离域 | 同一物理 L2 slice、同一 set 的访问次数；不是全局访问次数、cycle 或 instruction 数。 |
| 热路径复杂度 | RDP/采样索引为 O(1)，一次周期 aging 只扫描该 set 的 O(ways) 个 line，victim 选择为 O(ways)；目标 slice 为 8-way。 |
| 功能边界 | replacement policy 不改变 hit/miss 查找、coherence、MSHR 仲裁或 slice 路由；所有 fill 都沿 GEM5 原有流程完成。 |

论文中的真实缓存旁路需要改动响应、tags 和临时块生命周期。
本端口明确不实现这部分：在更新采样历史之前，若一次 fill 的 RDP 预测为 scan，
或其预测 ETR 大于本次选中牺牲行的绝对 ETR，缓存行仍正常分配并设置
`+INF_ETR`。`selectVictim` 会选择绝对 ETR 最大的缓存行，因此该缓存行会在后续
竞争中优先于绝对 ETR 更小的缓存行被淘汰，同时不会改变 cache 的功能时序。若
存在绝对值相同的负 ETR writeback，负值优先的既有 tie-break 仍先选 writeback。

## 状态与默认参数

所有规模都从参数进入。`kmhv3.py` 的初始值由每个 slice 的实际 geometry
计算，而不是在策略中写死：

| 参数 | 512 KB、8-way slice 的初始值 | 含义 |
| --- | ---: | --- |
| `num_sets` | `inner_cache.size / (64 * inner_cache.assoc)` = 1024 | slice 内 set 数 |
| `num_ways` | `inner_cache.assoc` = 8 | associativity |
| `block_bits` | 6 | 64 B cache line 的 log2 |
| `slice_bits` | 2 | 提取采样地址前去掉的交错 slice 位 |
| `history_multiplier` | 8 | 以 way 数为单位的历史长度 |
| `aging_granularity` | 8 | 每 8 次 set 访问进行一次 ETR aging |
| `sampled_sets` | 8 | 采样的物理 set 数 |
| `sampled_cache_sets_per_set` | 16 | 每个采样 set 的低位 block-tag bucket 数 |
| `sampled_cache_ways` | 5 | 每个 bucket 的 associativity |
| `sampled_tag_bits` | 12 | 截断后的采样 tag 宽度 |
| `rdp_entries` | 512 | 每个 slice 的 direct-mapped RDP 表项数 |
| `temporal_difference_threshold` | 16 | RDP temporal-difference 更新阈值 |
| `scan_threshold_margin` | 22 | `MAX_RD = INF_RD - 22` 的 margin |
| `prefetch_penalty_percent` | 200 | 以预取结束的区间的复用距离倍率 |
| `timestamp_bits` | 8 | 采样历史时间戳宽度 |

派生值为：

* `INF_RD = num_ways * history_multiplier - 1`；
* `MAX_RD = INF_RD - scan_threshold_margin`；
* `INF_ETR = (num_ways * history_multiplier / aging_granularity) - 1`。

初始 8-way slice 的三个值分别是 63、41、7。构造函数会检查非零参数、需要
幂次方索引的 geometry、距离和 ETR 的可表示范围，并拒绝会越过 `Addr` 位宽的
地址字段布局、溢出的采样 cache bucket 数、以及模数不大于采样历史窗口的
`timestamp_bits`。默认配置的 `block_bits=6`、`slice_bits=2` 和 8 位时间戳
均满足这些约束。

`MockingjayReplData` 保存 `valid`、`set_id`、`way_id` 和有符号 `etr`。在一次
标准 victim 选择与随后的 `reset` 之间，它还暂存被选中旧 line 的 `victim_etr`
及其有效位；这只服务于新 fill 的最大 ETR 判定，不改动 cache 的分配接口。策略
拥有以下固定大小结构：

* `entries_by_set`：每个 set 的 way replacement-data 指针列表；
* 每 set 一个 aging clock；
* 采样 set 的时间戳和固定 associativity 的采样 cache bucket；
* 带 valid 位和预测复用距离的 direct-mapped RDP，索引为 PC/state CRC hash
  的低 `log2(rdp_entries)` 位。

访问路径不使用无界增长的 map。

论文和公开 ChampSim 参考实现都保留 CRC hash 的低 `PC_SIGNATURE_BITS` 位。
本实现以 `hash(input) & (rdp_entries - 1)` 索引 power-of-two RDP，语义与
参考实现的左移再右移截断等价。

## 算法细节

1. `touch(data, pkt)` 处理 L2 hit：记录 hit signature，在采样 set 上更新
   采样历史，执行周期性 set aging，然后用 RDP 结果提升 line 的 ETR。
2. 可训练 fill（普通请求和硬件预取）由 GEM5 原有 tags/cache 流程选择 victim；
   `getVictim` 在该选择发生时保存有效 victim 的 ETR，`reset(data, pkt)` 先读取
   训练前预测，再记录 miss、更新采样历史并执行 set aging。若训练前 RDP 预测
   为 scan，或预测 ETR 大于已保存 victim 的绝对 ETR，则新 line 固定为
   `+INF_ETR` 并递增 `maxEtrInsertions`，但不跳过分配、响应或 refill
   notification。其他可训练填充使用训练后的预测 ETR。软件预取、eviction、
   `WriteClean` 和 cache-maintenance 流量不训练 RDP、不推进采样时间戳，也不
   改变已有 line 的普通命中 ETR；writeback fill 保持 `-INF_ETR`。软件预取在
   cache 内部会生成不带 PC 的复制 Request，因此显式排除，避免将其误训练到
   保留的 no-PC 表项。
3. 采样历史命中时训练旧 PC signature 的复用距离；记录被 aging 淘汰或被
   LRU 替换时按 scan 训练。以预取结束的区间在训练前按参数放大。普通训练
   使用参考实现的整数 temporal-difference 规则：距离差小于阈值时不变，
   否则每次只移动一步；scan 训练向 `INF_RD` 移动。
4. 每 set 每八次访问，其他有效且非 scan 的 line 的 ETR 减一，并限制在
   `-INF_ETR`；scan line 不 aging。
5. victim 选择先返回 invalid way；否则选 `abs(ETR)` 最大者，绝对值相同则
   负 ETR 优先。由 `+INF_ETR` 插入的低复用 line 因而会在正常替换竞争中优先
   于绝对 ETR 更小的 line 离开 cache；与 `-INF_ETR` writeback 平局时，后者先
   被选择。等优先级保持确定性的 resident tie-break。

没有 PC 的请求使用保留的 no-PC signature；它们仍然 cacheable 并计数，但不与
普通 load PC 共享预测相关性。packet-less 的 `touch/reset` 只维护 replacement
状态，不训练 RDP。

## 统计量

策略导出以下计数器：采样命中/未命中、复用和 scan 训练、RDP 命中/未命中、
无 PC 请求、命中提升、普通插入、writeback 插入、周期 aging、
`maxEtrInsertions`、正/负 ETR 候选和 invalid 候选。`maxEtrInsertions` 表示
最大 ETR 的快速淘汰插入次数，不是缓存旁路次数。候选统计在
`getVictim()` 返回时递增，早于 cache 的 `handleEvictions()`，因此不能当作实际
完成淘汰数；它们用于区分学习活动、低复用 line 插入和替换选择趋势。

## 验证计划

1. 生成 SimObject 参数并编译优化版 RISC-V binary。
2. 运行 policy GTest，覆盖 geometry、采样复用、scan detraining、每 set 隔离、
   有符号 ETR tie-break、无 PC 行为和 `+INF_ETR` 插入。
3. 用短 checkpoint smoke 确认四个 L2 slice 各自构造 policy，检查
   `config.ini` 和每个 slice 的采样、RDP、插入、aging 统计。
4. 在明确批准后，按 `mockingjay_l2_progress.md` 中冻结的 GCC15 SPEC06
   合同运行 A/B；在归档 `config.ini`、`score.txt` 和 manifest 前不做性能结论。

## 精度边界

这是行为级性能模型，不是 RTL 的逐拍实现。它保留论文的 PC 预测、按 set 的
时间域、有符号 ETR 排序、预取区间惩罚和低复用 line 的快速淘汰趋势；没有实现
论文的真实缓存旁路，也没有改变 GEM5 cache 的 coherence 或响应时序。
为此损失的细节是：被判定为 scan 的请求仍占用一次正常 fill，并在短时间内
占据一个 way；这会保留 cache 容量和带宽的真实竞争，但不模拟 bypass 省下的
一次 line residency。论文没有完全规定同号 ETR 平局、分数 TD 舍入和查找/递增
顺序，当前实现把这些选择固定为可测试的确定性规则。时间戳模数被限制为大于
历史窗口，但整整一个计数周期未触及的项仍可能发生别名，这是有界历史近似。
对合并 MSHR 的 fill，`reset(data, pkt)` 接收的是下行响应携带的初始 miss
Request；因此该次训练归属初始发起者，而不随之后合并的最终消费者改变。保持
这一现状可避免向 cache/MSHR 引入 Mockingjay 专用元数据，但它是本模型的归因
边界。
