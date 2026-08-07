# ISCA 2021：面向 KMHv3/GEM5 的硬件 Feature 全量筛选

> 书目入口：[ISCA 2021 DBLP 目录](https://dblp.org/db/conf/isca/isca2021.html)。ISCA 由 IEEE 出版；本文按目录全量初筛，书目以 DOI/DBLP 为准，论文效果与成本只保留公开摘要或作者版可以核对的陈述。

## 结论

ISCA 2021 值得立即进入当前 GEM5 feature backlog 的是 **ATP+SBFP 的 translation prefetch**；Entangling instruction prefetcher 是独立的前端预取项目。Vector Runahead 和 PF-DRAM 有清晰硬件价值但分别需要核心 recovery 或 DRAM 芯片级时序；Zero Inclusion Victim 面向多核 inclusive LLC，需先建立平台匹配基线。

|优先级|论文/feature|论文效果与硬件成本|当前 GEM5 映射|
|---|---|---|---|
|P0|ATP + SBFP TLB prefetch|SPEC 几何加速 +11.1%，Qualcomm/SPEC/GAP 为 +16.2%/+11.1%/+11.8%，PTW memory refs 下降|RISC-V TLB/PTW、小型 predictor/filter、有限 prefetch queue|
|P1|Entangling instruction prefetcher|40 KB，性能最高 +23%|IFetch/L1I/L2 prefetch queue；须严控 pollution/bandwidth|
|P1|Vector Runahead|间接访问 workload 1.79×|in-core runahead/vectorization，先做受限 prefetch engine|
|P2|PF-DRAM|平均 +8.6%、最高 +24.3%、memory power -35.3%、面积 <9%|需要可编程 DRAM timing/bank model，不是 O3 局部改动|
|P2|Zero Inclusion Victim|放宽 inclusive LLC 对 mid-level cache 的强制驱逐|需 shared/inclusive hierarchy 才有同口径价值|

## 全集扫描与严格口径

从 DBLP 目录逐题名收集 cache/TLB/prefetch/core/DRAM/NoC、OS/runtime、GPU/PIM/accelerator、security/reliability。保留项需要二进制/OS/ISA 不变、状态/端口/带宽/延迟有界、能在 `kmhv3.py` A/B 中解释 IPC。`A`=书目、`B`=摘要/作者版、`C`=GEM5 工程事实；未披露的 bit/area 不补猜。

## 主候选详情

### P0：ATP + Sampling-Based Free TLB Prefetching（SBFP）

论文：[ATP DOI 10.1109/ISCA52012.2021.00016](https://doi.org/10.1109/ISCA52012.2021.00016)。ATP 组合 stride、PC、distance 等 TLB prefetch 视角，按 miss/收益动态选择；SBFP 从已取回 cache line 的相邻 PTE 中挑选高收益 entry，避免盲目扩大 walk traffic。论文报告 Qualcomm/SPEC/GAP 几何加速 +16.2%/+11.1%/+11.8%。

**最小硬件合同。** predictor table、confidence、sampled free-entry filter、每周期 lookup/update、prefetch degree 和 PTW queue 都要固定；TLB prefetch 与 demand walk 共用真实 L2/DRAM/MSHR，queue 满用现有 retry/backpressure。permission、ASID/VMID、page-size、sfence.vma、fault、squash/restore 不能从 demand 翻译路径分叉。

**GEM5 落点与验收。** `src/arch/riscv/tlb.*`、`src/arch/riscv/pagetable_walker.*`、`src/mem/cache/`；先作 observer，测 TLB miss PC/page stride、PTW reuse 和 free TLB entry。再比较 `off / ATP-only / SBFP-only / combined`，统计 i/d TLB MPKI、prefetch issued/hit/useful/late/unused、PTW L2/memory latency、extra requests、MSHR/DRAM occupancy 与 IPC。论文百分比不是通过门槛。

### P1：Entangling instruction prefetcher

论文：ISCA 2021 Entangling instruction prefetcher（目录书目见上）。机制用 instruction miss/region 关联捕获非简单顺序的 instruction-stream relationship；论文 40 KB 状态、最高性能提升 +23%。

当前落点为 BPU/FTQ 之后的 L1I/L2 instruction prefetch path，而不是 data `QueuedPrefetcher` 的直接复制。必须限制 entanglement table、target list、训练频率、prefetch degree、L1I/L2 port 和 queue；统计 branch redirect 后失效、wrong-path prefetch、I-cache pollution、coverage/accuracy/timeliness、FTQ empty 和 decode starvation。先建立 next-line/当前 instruction prefetch baseline，再测 Entangling。

### P1：Vector Runahead

Vector Runahead 在主线程长 miss 阻塞时抽取可向量化的间接访问，论文对目标 workload 报 1.79×。正确模型至少含 trigger、runahead context、lane/iteration bound、address fault/divergence、MSHR/带宽竞争和 return/recovery；不能把所有 dependent loads 静态变成预取。建议先在 `src/mem/cache/prefetch/` 做 bounded address-generation prototype，再决定是否进入 O3 rename/ROB。

### P2：PF-DRAM；Zero Inclusion Victim

PF-DRAM 通过 DRAM 组织/预取支持在 memory side 提前服务访问；论文平均 +8.6%、最高 +24.3%、memory power -35.3%、面积 <9%。需要 command timing、bank/subarray、queue policy 和 power model，当前 DDR4 controller 不能用“缩短 memory latency”替代。

Zero Inclusion Victim 消除 inclusive LLC 对 core private/mid-level cache 的强制驱逐。它的前置条件是 inclusive shared hierarchy；当前默认单核 L2 不具备。后续多核项目应先验证 inclusion traffic/forced eviction，再做 metadata/victim-buffer 的 iso-capacity 试验。

## 全量边界与排除

|方向|为什么不列当前首批|
|---|---|
|OS allocator、huge page、runtime policy|软件条件决定收益，不能硬件单独启用|
|GPU/PIM/NDP/FPGA/accelerator|新设备/编程模型和 workload，超出 O3 局部映射|
|NoC/cluster/异构平台|需多核拓扑和协议建模，先建平台再比较|
|安全、可靠性、性能分析|不提供既有应用透明 throughput feature|

## 统一实现与来源

- 当前入口：`configs/example/kmhv3.py`、RISC-V TLB/PTW、`src/mem/cache/prefetch/`、`src/cpu/pred/`、memory controller。
- 默认关闭；每个表/queue/port/latency 参数化，table full、queue drop、retry 和 restore 有统计。
- 固定 checkpoint/warmup/ROI，报告 IPC、TLB/cache MPKI、PTW cycles、prefetch useful/late、MSHR/DRAM traffic、前端 stall。
- [ISCA 2021 DBLP](https://dblp.org/db/conf/isca/isca2021.html) 与 ATP DOI 是书目证据；未经公开全文支持的绝对成本保持 unknown。

### 分阶段实施矩阵

|阶段|对象|先做什么|资源/正确性闸门|
|---|---|---|---|
|ATP-0|TLB predictor observer|记录 PC/stride/distance 与 miss outcome|不改变 walk，确认 prediction signal|
|ATP-1|bounded TLB prefetch|固定 degree、confidence、PTW queue|与 demand walk 共享 MSHR/DRAM、权限/ASID 正确|
|SBFP-0|sampled free entry|统计 PTE line 邻接和 TLB free capacity|不以地址猜 PTE；entry/page-size/VMID 完整|
|SBFP-1|prefetch-on|限制 filter、queue、update port|错误 PTE 不能污染 translation 或绕过 fault|
|Entangle-0|instruction trace|统计 region/branch/redirect 关联|wrong-path/redirect 失效可见|
|Vector-0|runahead observer|记录 trigger/stride/indirect chain|不改变 architectural state|

### 成本和流量纪律

ATP/SBFP 的额外 memory reference 会与普通 page walk 争用 L2、MSHR、DRAM；每次 prefetch 必须有 issued/useful/late/unused、queue-full/retry、PTW hit/miss 和 memory latency。Entangling 的 40 KB 需拆成 tag/target/history/valid/port，并保持 L1I/L2I 容量和 lookup latency；branch redirect 后 wrong-path entry 的 invalidation 计数必须单列。Vector Runahead 的 lane/context/loop-bound 状态不是免费 oracle，必须限制 max iterations、branch divergence、fault/recovery 和 bandwidth。

### 全量主题台账

|主题|代表性条目|处置|
|---|---|---|
|translation prefetch|ATP、SBFP|P0，当前 RISC-V PTW 直接落点|
|instruction prefetch|Entangling|P1，先做 trace/queue 模型|
|core speculation|Vector Runahead|P1，需 bounded engine|
|DRAM organization|PF-DRAM|P2，需 bank/timing/power model|
|cache inclusion|Zero Inclusion Victim|P2，先建 shared inclusive LLC|
|OS/ISA/platform|allocator、huge page、GPU/PIM/NDP/accelerator|strict hardware-only 排除|

### 统一实验字段

每个 workload 记录 i/d TLB MPKI、PTW levels/cycles、PTE L2/DRAM hit、prefetch coverage/accuracy/timeliness、I-cache/FTQ/decode stall、MSHR/DRAM bytes、metadata bits/ports 和 IPC/simTicks。论文 +11.1%、+16.2%/+11.1%/+11.8%、+23%、1.79× 仅作对照；当前配置若无压力应保留 zero/negative result。

### 预取请求的状态机

`candidate -> filtered -> queued -> sent -> merged/hit/miss -> useful/late/unused -> train` 是 ATP/SBFP 的最小状态机。每个状态都需要 sequence/ASID/page-size/source metadata；queue full、MSHR conflict、permission/fault、squash 和 `sfence.vma` 不能直接丢掉而不计数。Entangling 的 instruction request 另需 redirect generation/FTQ tag，以便 wrong-path response 不写入当前 path 的 cache priority。Vector Runahead 若生成多个 lane request，要记录 lane-to-line mapping、duplicate merge、branch divergence 和 abort reason。

### 参数扫参与公平性

ATP/SBFP 扫 predictor entries、degree、confidence、free-entry threshold、PTW queue depth；Entangling 扫 40 KB 内部 partition、target list、training epoch、L1I/L2I degree；Vector Runahead 扫 trigger latency、lane count、max iterations。任何 extra request 需在同一 DRAM/NoC bandwidth 下比较；多核 Zero Inclusion/PF-DRAM 实验还应报告 per-core slowdown、bank fairness 和 power proxy，而不是只给 aggregate IPC。
