# ASPLOS 2021：面向 KMHv3/GEM5 的硬件 Feature 全量筛选

> 书目入口：[ASPLOS 2021 proceedings](https://dl.acm.org/doi/proceedings/10.1145/3445814) 与 [DBLP 目录](https://dblp.org/db/conf/asplos/asplos2021.html)。本文按目录逐题名初筛，再用 DOI、Crossref/OpenAlex 书目记录和公开摘要核对。论文数字属于论文自己的平台和 baseline，不是当前 GEM5 的性能承诺。

## 结论

ASPLOS 2021 的 CPU 透明候选集中在 cache compression/deduplication、神经预取和大幅改变核心组织三类。严格要求“不改应用、编译器、OS、运行时、ISA”，建议排序如下；软件协同或专用系统项保留在边界表，防止漏检但不把它们误列为可直接移植的 feature。

|优先级|论文/feature|论文可核对的效果与硬件代价|当前 GEM5 判断|
|---|---|---|---|
|P0|BCD partial-line deduplication|SPEC 平均 +2.7%，综合 compression ratio 1.94×；需要 base/difference、引用计数、压缩/解压端口和 free-list|可落到 L2/内存控制器，但当前无共享 LLC，必须显式建时延、metadata 和容量收益|
|P1|Voyager hierarchical neural prefetching|irregular SPEC/GAP 相对 no-prefetch +41.6%；相较旧神经模型计算降低 15--20×、存储降低 110--200×|可做有界 trace-replay/硬件模型；训练、推理和带宽成本是首要风险|
|P2|DiAG dataflow CPU|512-PE RISC-V 原型相对激进 OoO 1.18×、能效 1.63×|硬件-only 但几乎重建 CPU pipeline，不适合作为 KMHv3 局部 feature|

## 论文全集、筛选口径和证据等级

### 全量扫描方法

1. 以 ASPLOS 2021 proceedings/DBLP 目录为全集，不因为论文标题含 accelerator、OS 或 compiler 就提前删除。
2. 对每篇标注 `CPU/cache/TLB/prefetch/memory`、`系统/OS/编译器`、`专用 accelerator`、`安全/分析` 四类；再检查收益是否在软件和 ISA 不变时仍存在。
3. 对保留项记录 DOI/摘要能直接支持的机制、baseline、效果和成本；摘要未给出的绝对 area/bit 数写成 `未披露`，不从搜索片段补全。

### hardware-only 定义

- 既有二进制和 RISC-V ISA 不变，不需要 compiler hint、profile、OS allocator、运行时 API 或专用设备协议。
- 机制可表示为有限表项、cache-line metadata、队列、端口、固定训练/推理延迟和 backpressure。
- 在 GEM5 中能定义错误、squash、回放、带宽和容量约束；“预测正确”不能被模拟成零延迟、零流量。

证据等级：`A`=DOI/DBLP/Crossref 书目；`B`=公开摘要或作者公开版明确写出的机制/数字；`C`=当前 GEM5 源码映射与工程判断。文中 `B` 级数字只说明论文结果，不表示 KMHv3 预期增益。

## 候选详情

### P0：BCD —— partial cache-line deduplication

论文：[BCD DOI 10.1145/3445814.3446722](https://doi.org/10.1145/3445814.3446722)。BCD 将 base/difference compression 与多条 cache line 的 partial dedup 结合，同时作用于 LLC 和主存。公开摘要/记录可核对 SPEC2017、DaCapo、TPC-DS/TPC-H 评估，综合压缩比 1.94×、SPEC 平均性能 +2.7%。

**硬件状态和成本。** 每个 compressed sector 需要 payload、base/difference pointer、长度/格式位、引用计数；还需要 dedup compare、allocation/free-list、压缩/解压 pipeline 和 writeback 扩展处理。`modest area` 是摘要级相对描述，绝不能解释成零面积或零时延。压缩失败会产生普通 line，写回/失效时要原子更新引用计数。

**GEM5 落点。** 当前 `configs/example/kmhv3.py` 主要是每核 2 MiB L2，没有论文意义上的共享 LLC。第一阶段应在 `src/mem/cache/` 与 memory-controller 之间增加可配置 compressor latency、有限 compressed-sector 容量和额外 metadata request；先不改变 cache associativity。模型至少记录 `compressed_bytes/raw_bytes`、dedup hit/miss、metadata read/write、decompression stall、writeback expansion、MSHR/DRAM traffic。

**最小建模合同。** 压缩判断和 metadata 查找占用固定端口；同一 sector 的并发读写串行化；evict、invalidate、snoop、checkpoint serialize/unserialize 都保持引用计数正确。A/B 依次为 baseline、compression-only、dedup-only、BCD；固定容量和带宽，避免把“容量变大”与“带宽变少”混为一因果。

### P1：Voyager —— 分层神经预取

论文：[Voyager DOI 10.1145/3445814.3446752](https://doi.org/10.1145/3445814.3446752)。它把地址分为 page 与 page-offset 两级关联，面向 irregular pointer chasing；论文在无预取器基线上的 irregular SPEC/GAP IPC 提升为 +41.6%，并报告相较旧神经模型计算降低 15--20×、存储降低 110--200×。这些数字是 `B` 级摘要/作者材料口径，不能直接外推到当前 L2。

**硬件状态和成本。** page/offset history、embedding/table、confidence、训练更新端口和 inference pipeline 都是有限但非零的资源；必须规定表项、位宽、每周期 lookup 数、训练 epoch 和 queue depth。论文明确指出训练/推理时延仍是部署障碍，因此不能在 GEM5 热路径调用无界神经网络。

**GEM5 落点与实验。** 在 `src/mem/cache/prefetch/` 新增独立 `VoyagerPrefetcher` 或离线 table replay，复用 `QueuedPrefetcher`/MSHR/backpressure。先做 trace replay 得到 page/offset accuracy、coverage、late、unused；再加入固定 inference latency、更新带宽和 request queue，比较 `off / replay-only / bounded hardware`。必须报告 prefetch traffic、MSHR occupancy、DRAM queue、pollution 和最终 ROI IPC。

### P2：DiAG —— 数据流化通用 CPU（长期参考）

论文：[DiAG DOI 10.1145/3445814.3446703](https://doi.org/10.1145/3445814.3446703)。register lanes 在硬件中隐式构建 dataflow graph，不依赖特殊语言或 compiler；论文实现是 512-PE RISC-V 原型，相对激进 OoO 1.18×性能、1.63×能效。

代价并非一个局部表：PE array、lane register file、graph construction、front-end/rename/commit 和 memory ordering 都要重构。若在 GEM5 只把 O3 width/issue width 放大，无法代表 DiAG。保留它的价值是给未来 dataflow-core 分支提供性能上界；当前不进入 KMHv3 P0/P1 backlog。

## 条件候选和排除矩阵

|论文/方向|硬件部分|不可省略的软件/系统条件|处置|
|---|---|---|---|
|NOREBA|硬件辅助 binary region/branch 优化|compiler/binary hint|条件参考，不列主候选|
|PIBE|预测/指令布局支持|profile-guided binary transformation|排除|
|PTEMagnet、vMitosis、KLOCs|页表/内存管理硬件|OS allocator、迁移或运行时策略|排除；只在跨栈项目讨论|
|NIC/FPGA/PIM/SSD/quantum/专用 accelerator|新设备或执行阵列|新 workload、驱动、编程模型|排除当前 CPU shortlist|
|安全、侧信道和测量论文|可观测性或防护逻辑|不以通用 CPU 加速为目标|不列性能候选|

## 当前 GEM5 统一实现、验证与验收

- 入口：`configs/example/kmhv3.py`；cache/compression：`src/mem/cache/`；预取：`src/mem/cache/prefetch/`；核心时序：`src/cpu/o3/`。
- 所有新状态必须有参数化容量、位宽、lookup/update latency、端口和 queue backpressure，默认关闭并能 checkpoint restore。
- 结果固定 commit、checkpoint、warmup/ROI、core count、cache/DRAM/prefetch 参数；reset 后比较 IPC/simTicks、MPKI、带宽和 feature-specific counters。
- 论文数字与 GEM5 数字分栏记录；若收益不能由容量、压缩、预取 useful/late 或延迟统计解释，则不接受为可实现结论。

### 分阶段执行清单

|阶段|只回答一个问题|必须保留的状态/统计|停止条件|
|---|---|---|---|
|BCD-0|当前 L2/内存流量是否存在可压缩或重复 sector|raw/compressed bytes、dedup candidate、metadata access、writeback|压缩率接近 1 或 metadata/延迟占主导时不做 policy|
|BCD-1|压缩本身是否改变 miss/带宽|compress/decompress latency、port busy、MSHR/DRAM bytes、capacity occupancy|若只靠无限容量获益，回退模型|
|BCD-2|partial dedup 是否净收益|dedup hit、refcount update、evict/reconstruct、extra read/write、IPC|任何引用计数/restore 错误都阻止性能实验|
|Voyager-0|page/offset 关联是否存在|offline accuracy/coverage、page-vs-offset entropy、候选 degree|没有可预测关联时不启用硬件训练|
|Voyager-1|固定推理预算是否能及时发射|table bits、lookup cycles、update bandwidth、queue drop、late/useful|推理晚于 demand 或带宽超预算时回退|
|DiAG-0|是否值得建立独立 dataflow-core 分支|PE/lane occupancy、graph construction cycles、memory dependence|不把现有 O3 width 改动冒充 DiAG|

### 共同的资源敏感性矩阵

|资源|BCD 影响|Voyager 影响|DiAG 影响|报告要求|
|---|---|---|---|---|
|SRAM/metadata|sector format、base/refcount/free-list|page/offset history、action/context table|PE register/lane state|以 bit/entry/port 给出预算，不能只写“modest”|
|时延/端口|compress/decompress、metadata 二次访问|inference、training update|graph build、lane wakeup|固定 latency 与并行度，统计 stall/queue full|
|带宽|dedup extra metadata/writeback|prefetch demand competition|vectorized speculative loads|报告 DRAM bytes、MSHR、pollution 和 backpressure|
|恢复/一致性|refcount、snoop、checkpoint|squash、page fault、context reset|precise exception、ordering|先通过功能 microbenchmark 再跑 ROI|

### 最小实验报告格式

每个 checkpoint 记录 baseline/feature-on 的 commit、core/cache/DRAM 参数、warmup 与 ROI 周期；表格至少包含 `IPC/simTicks`、L1/L2 MPKI、MSHR occupancy、DRAM request/bytes、feature hit/useful/late/unused、metadata full/drop、额外 latency 和 rollback/fault。论文 +2.7%、+41.6% 与 1.18× 只放在“论文结果”列，不能替代本表。

### 逐 feature 输入/输出契约

|feature|训练/输入|动作/输出|主要错误路径|
|---|---|---|---|
|BCD|cache line bytes、base candidate、sector occupancy、refcount|compressed payload、dedup pointer、有效容量/traffic 变化|压缩失败、同 sector 并发写、refcount underflow、metadata miss、writeback expansion|
|Voyager|load PC/page/offset history、bandwidth/queue feedback|page/offset target、degree、prefetch enqueue|prediction late、wrong page/offset、MSHR full、L2 pollution、训练更新超预算|
|DiAG|decoded op、register/data dependence、lane availability|dataflow token/PE issue、vectorized memory request|graph build stall、lane divergence、precise exception、memory ordering|

BCD 需要对重复 sector 的共享 payload 做原子引用更新；checkpoint 序列化时既要保存 compressed format 又要保存 free-list 顺序，否则 restore 后的 hit/miss 无法和 baseline 对齐。Voyager 的 page predictor 命中但 offset predictor 失败时应只发 page-local 可证明的请求，不能扩大到整页。DiAG 即使不实现，也应在报告中注明“改 O3 width、增加 issue queue、增加 prefetch degree”都不是等价替代，避免后续实验误命名。

## 来源与核验状态

- [ASPLOS 2021 proceedings](https://dl.acm.org/doi/proceedings/10.1145/3445814)、[DBLP TOC](https://dblp.org/db/conf/asplos/asplos2021.html)。
- BCD、Voyager、DiAG DOI 链接见各小节；其他题目以 DBLP 目录作书目证据。
- ACM 全文若遇 Cloudflare，只保留 DOI/摘要可核对内容；未披露的 area、bit、端口和 workload 子项明确标为未知。
