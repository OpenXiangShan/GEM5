# MICRO 2022：面向 KMHv3/GEM5 的硬件 Feature 全量筛选

> 书目入口：[MICRO 2022 DBLP 目录](https://dblp.org/db/conf/micro/micro2022.html) 与 [proceedings DOI 10.1109/MICRO56248.2022](https://doi.org/10.1109/MICRO56248.2022)。本文全量扫描后按现有 O3、BPU、prefetch、cache/DRAM 路径排序；论文性能与成本均是原平台事实。

## 结论

本届候选可以分成 **低风险 prefetch（PMP/Berti/PPM）**、**中风险提前 memory 请求（Hermes）** 与 **高风险 O3 scheduler 组织（Ballerino）**。推荐第一批从 PMP 或 Berti 的 iso-bit/iso-bandwidth A/B 开始；Ballerino 不因能效数字漂亮而跳过 wakeup/select、memory dependence 和 phase transition 的功能合同。

|优先级|论文/feature|效果与硬件成本证据|GEM5 可实现性|
|---|---|---|---|
|P0|Pattern Merging Prefetcher (PMP)|相对 enhanced Bingo +2.6%、Pythia +8.2%；pattern storage 分别少 30×/6×|bounded region-pattern table，最适合 L2 prefetch A/B|
|P1|Berti|2.55 KB；相对 IP-stride +8.5%、IPCP +3.5%，相对 IPCP dynamic energy -33.6%|L1D local-delta prefetch，可复用 queue/统计|
|P1|Page Size Aware Prefetching (PPM)|L1 MSHR +1 bit；80 个 memory-intensive workload 上 +2.1%--8.1%|page-size sideband + spatial PF，需真正在请求路径起作用|
|P1|Hermes|预测 off-chip load 并提前绕过 hierarchy；摘要称跨配置稳定、storage modest|core-to-memory speculative path，接口/取消逻辑中风险|
|P2|Ballerino|12 个级联/簇式 in-order IQ 接近 8-wide OoO，core-wide energy efficiency +20%|scheduler/issue queue 大范围改动|

## 全集扫描和证据等级

以 DBLP 题名全集为入口，对 cache/prefetch/core/memory 先收录，之后检查 compiler/binary/ISA、OS/runtime、平台/accelerator 和 security gate。`A`=书目、`B`=摘要/作者公开版、`C`=源码映射；摘要未给统一 IPC/bit/port 的 Hermes 不以“modest”编造绝对数字。

## 候选详情

### P0：PMP —— 相似 region pattern 合并

论文：[DOI 10.1109/MICRO56248.2022.00071](https://doi.org/10.1109/MICRO56248.2022.00071)。PMP 以 region 首次访问 offset 聚类，把相似 footprint 在训练阶段合并，发射时依访问频率选择 prefetch target；论文相对 enhanced Bingo +2.6%、Pythia +8.2%，且 pattern storage 分别少 30×/6×。

**最小合同。** 固定 set/way、region/tag、offset bitset、merge threshold、frequency field、candidate list、lookup/update port 和 replacement；每个 demand 的训练/发射只能在固定候选内完成。merge false-positive 会造成 pollution，metadata eviction/queue full/late request 不能忽略。当前落点是 `src/mem/cache/prefetch/`；和现有 region/PHT 类模块并列实现，避免改写既有 KMH policy。

**A/B。** `no PF / existing baseline / PMP same-bit-budget`，固定 MSHR、queue、degree、DRAM；报告 metadata hit/merge/split/evict、coverage、accuracy、late/unused、demand MPKI、pollution、traffic、MSHR/queue 和 IPC。只减少表项但不比较 bit budget，不能证明 PMP。

### P1：Berti 与 PPM

Berti：[DOI 10.1109/MICRO56248.2022.00072](https://doi.org/10.1109/MICRO56248.2022.00072)。它按 load PC 选择 local delta，公开摘要给 2.55 KB、相对 IP-stride +8.5%、IPCP +3.5%、相对 IPCP memory-hierarchy dynamic energy -33.6%。模型需固定 PC/delta table、confidence、page/region boundary、degree 和 queue；L1D hit/miss 训练时刻要明确。

PPM：[DOI 10.1109/MICRO56248.2022.00070](https://doi.org/10.1109/MICRO56248.2022.00070)。它把 page-size 信息传给低层 spatial prefetcher，论文只需为 L1 MSHR 增 1 bit，在 80 memory-intensive workloads 上使所评估 prefetcher 单核 +2.1%--8.1%。该 1 bit 必须随 miss allocate/merge/retry/response 传播；若只在配置中查看 page size 而不改变 request/degree/boundary，不能算 feature。Berti/PPM 的 cache level、baseline 与 page-size 假设不同，禁止横向合并数字。

### P1：Hermes —— off-chip load prediction

论文：[DOI 10.1109/MICRO56248.2022.00015](https://doi.org/10.1109/MICRO56248.2022.00015)。Hermes 用 PC sequence、load byte offset 等轻量 perceptron 特征预测 off-chip load，地址生成后同时触发 hierarchy access 和 speculative memory request，以隐藏 cache lookup。公开材料未给可统一核对的 IPC/bit 数，因此按 unknown 成本处理。

实现应给 predictor entries/history/weights、lookup latency、speculative request queue、merge/cancel、DRAM priority 和 error traffic 明确预算。复用已有 packet/MSHR/retry 语义；错误预测的 L1/L2 hit 仍消耗 memory queue。统计 correct/wrong off-chip、saved cycles、extra/cancelled traffic、queue full、L2/DRAM contention、load-use stall 和 IPC。

### P2：Ballerino —— 重构 OoO issue queue

论文：[DOI 10.1109/MICRO56248.2022.00023](https://doi.org/10.1109/MICRO56248.2022.00023)。Ballerino 组合 readiness/dependence IQ，采用 dispatch filter、memory-dependence steering 和 shared IQ，使 12 个 in-order/clustered queues 达到接近 8-wide OoO IQ 的性能，核心能效提升 +20%。

落点是 `src/cpu/o3/inst_queue.*`/scheduler；必须定义 cluster mapping、select/wakeup port、cross-cluster forwarding、steering update、queue-full、phase change、load dependence、squash/replay/exception。第一阶段只采样当前 IQ ready/dependence distribution；第二阶段建立 fixed cluster selector 仍保持单一 commit；最后开放 policy。比较同 entries、ports、latency 的 baseline，报告 issue width、wakeup/select、IQ occupancy、cross-cluster stall、load dependence stall、IPC 与 energy proxy。

## 全量边界与排除

|方向|为什么不进 strict shortlist|
|---|---|
|Speculative Code Compaction、OCOLOS、Treebeard|compiler/binary transformation 是收益前提|
|SwiftDir、Eager Memory Cryptography|安全/协议机制；普通 baseline 中不等价正向加速|
|GPU/PIM/crypto/persistent memory/network accelerator|需要专用平台、驱动、协议或 workload|
|OS/runtime/系统调度项|软件策略无法在既有 checkpoint 只改硬件复现|

## 统一验证与来源

- 入口：`src/mem/cache/prefetch/`、`src/cpu/o3/inst_queue.*`/`issue_queue.*`、`configs/common/PrefetcherConfig.py`、`kmhv3.py`。
- 所有表/queue/degree/port/latency 参数化且默认关闭；用相同 checkpoint、warmup/ROI、DRAM 和 baseline policy。
- reset stats 最低包括 IPC、coverage/accuracy/late/traffic 或 IQ/wakeup/issue/queue stall、metadata bits 和 failure reasons。
- 来源：[MICRO 2022 DBLP](https://dblp.org/db/conf/micro/micro2022.html) 与正文 DOI；全文未公开的成本不作猜测。

### 预取候选的统一阶段

|阶段|对象|固定预算|必测结果|
|---|---|---|---|
|PMP-0|pattern trace|region/set/way/offset bits|merge candidate、pattern entropy、baseline coverage|
|PMP-1|merge/train|frequency、threshold、update port|metadata hit/merge/evict、false pattern|
|PMP-2|issue|degree、queue/MSHR/DRAM|accuracy、late/unused、pollution、IPC|
|Berti-0|PC/local delta observer|2.55 KB equivalent|delta hit、page boundary、confidence|
|PPM-0|page-size sideband|L1 MSHR +1 bit|bit propagation、prefetcher mode change|
|Hermes-0|off-chip classifier|history/weight/queue bits|correct/wrong、candidate latency|

Berti 与 PPM 不能合并成一个“page-aware prefetcher”数字：cache level、baseline、page-size 假设和 energy 口径不同。Hermes 的 speculative request 要通过现有 packet/MSHR/retry、支持 duplicate merge/cancel，并承担错误预测的 DRAM queue/port 成本。Ballerino 则先做 observer，再做 fixed cluster/steering；不能把 IQ entries 减少当成能效提升而忽略 wakeup/select、cross-cluster forwarding、memory dependence、squash/replay。

### 全量目录台账

|主题|代表性条目|处置|
|---|---|---|
|pattern/data prefetch|PMP、Berti、Page Size Aware/PPM|P0/P1，有限表/sideband|
|speculative memory|Hermes|P1，接口风险与未知成本|
|issue queue|Ballerino|P0/P1 研究原型，功能风险高|
|compiler/code|Speculative Code Compaction、OCOLOS、Treebeard|compiler/binary 依赖，排除|
|security/system|SwiftDir、Eager Memory Cryptography|协议/安全目标，非正向 throughput|
|platform|GPU/PIM/crypto/persistent/network|新设备/workload，排除|

### 统一结果字段

预取项报告 table bits、merge/lookup/update cycles、coverage/accuracy/late/unused、demand MPKI、pollution、MSHR/queue/DRAM bytes、port conflicts 和 IPC；Hermes 另报 speculative hit/miss/cancel、saved/extra cycles；Ballerino 报 issue/select/wakeup、IQ occupancy/fragmentation、cross-cluster stall、memory-dependence/replay、IPC 与 energy proxy。论文 +2.6%/+8.2%、+8.5%、+2.1%--8.1%、+20% 仅作对照。

### 互斥配置和 storage fairness

PMP、Berti、PPM 的 baseline/cache level 不同，实验配置文件必须显式写出 `cache_level / degree / page_size / table_bits / queue_depth / DRAM`，不能把 PPM 的 1-bit sideband 与 Berti 的 2.55 KB predictor 合成一个成本。Hermes 的 off-chip classifier 若与普通 prefetcher 同时开启，要分离 speculative request source；Ballerino 的 IQ 结构比较则固定总 entry、wakeup/select ports、dispatch/commit width 和 timing。

### 功能微测试

PMP/Berti/PPM：region boundary、page-size change、pattern merge false positive、MSHR full、late/unused、prefetch kill；Hermes：L1 hit/miss、wrong off-chip prediction、duplicate memory response、cancel、store/exception；Ballerino：cluster steering、cross-cluster wakeup、memory dependence、queue fragmentation、branch squash、precise exception。没有这些路径的 stats，只能报告 trace-level prediction，不交付 IPC 结论。
