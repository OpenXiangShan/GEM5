# ISCA 2022：面向 KMHv3/GEM5 的硬件 Feature 全量筛选

> 书目入口：[ISCA 2022 DBLP 目录](https://dblp.org/db/conf/isca/isca2022.html) 与 [proceedings DOI 10.1145/3470496](https://dl.acm.org/doi/proceedings/10.1145/3470496)。本文逐题名筛选，不以大 speedup 的软件协同论文填充 hardware-only shortlist。

## 结论

严格筛选后，**Register File Prefetching（RFP）** 是唯一能在既有 O3 CPU 内构成完整硬件因果链的 P0。tākō、MAPLE、Free atomics、Thermometer 和 Sibyl 都有值得借鉴的结构思想，但软件 callback、编译器、ISA 或 OS policy 是其效果必要条件，故明确保留为排除项而非“漏掉的 P1”。

|优先级|feature|论文效果与硬件成本|GEM5 判断|
|---|---|---|---|
|P0|Register File Prefetching (RFP)|43.4% loads 从 L1 预取到 RF；Tiger Lake-like +3.1%，放大 core +5.7%，与 value prediction 合用 +4.1%|最贴近 O3 load/RF path；需精确 recovery、liveness 与端口模型|
|排除|tākō polymorphic cache|1.4--4.2×、约 +5% 多核面积|cache miss/eviction 触发软件 callback 与近 cache engine，非软件透明|
|排除|Tiny but mighty/MAPLE|相对既有硬件方法 1.72--1.82×|硬件-软件异步接口与自动编译不可省略|
|排除|Free atomics/Thermometer/Sibyl|ISA、profile 或 OS placement 路径|不能在既有 binary/OS 上只改硬件生效|

## 全集扫描与证据等级

以 DBLP/proceedings 为全集，CPU-core/cache/memory/translation 条目先入候选池，再按 software/ISA/OS/platform gate 筛选。`A`=DOI/DBLP 书目；`B`=摘要/公开作者版中的机制/数字；`C`=GEM5 工程映射。未公开 RFP table 项/bit/area 细节时，不以“轻量”补成具体成本。

## P0 详情：Register File Prefetching

论文：[DOI 10.1145/3470496.3527398](https://doi.org/10.1145/3470496.3527398)。RFP 使用 OoO scheduler 的 foresight 和空闲 L1/RF 带宽，在 load 真正需要前将已命中 L1 的数据写入目标物理寄存器或 side buffer；论文在 65 workload 中预取 43.4% load，Tiger Lake-like baseline +3.1%，更宽的 core +5.7%，与 value prediction 组合 +4.1%。

### 必须建模的正确性和资源合同

1. **版本/liveness。** RFP record 至少绑定 load sequence、destination physical register、rename version、地址/回填状态。重命名复用、squash、replay、store-set violation、exception、fault、MMIO/atomic 都要让无效 record 不能唤醒消费者。
2. **端口与仲裁。** 提前 L1 read、RF/side-buffer write 和正式 writeback 都占有限端口；RFP 不得从普通 load 完成前偷送 data。RF port 不足时要 drop/delay，统计原因。
3. **内存一致性。** RFP 只对确认的 L1 hit 和可预取 load 发起；地址匹配的更老 store、snoop/invalidations、cache miss/retry 必须撤销或重放。不能把普通 L1 access latency 直接减掉。
4. **功耗/面积代理。** 明确 per-inflight-load bit、record FIFO depth、RF/side-buffer entries、extra read/write port 和 arbitration；同样记录被节省或新增的 L1/RF access。

### 当前 GEM5 路径与分阶段 A/B

落点是 `src/cpu/o3/lsq.*`、`iew.*`、physreg/scoreboard、rename/commit。先以 observer 统计“提前 N cycles 且 L1 hit 的候选 load”和 RF port demand；再只为 L1 hit 添 side-buffer-ready 状态；最后加入完整 store/snoop/replay invalidation。比较 `off / RFP / value-pred / RFP+value-pred`，且后两者不能共享未建模资源。

最低统计：RFP candidate/issued/hit/late/drop，drop 按 RF port/record full/L1 miss/squash/store 分解；load-use stall、wakeups、RF/L1 port conflict、replay/squash、extra/avoided access、IPC。用 L1-hit long-dependence microbenchmark 验证先后顺序，再跑相同 checkpoint ROI。

## 边界项和排除理由

|论文/方向|硬件含量|严格口径下的决定|
|---|---|---|
|[tākō](https://doi.org/10.1145/3470496.3527379)|near-cache reconfigurable engine|software callback 是机制入口，排除|
|[Tiny but mighty/MAPLE](https://doi.org/10.1145/3470496.3527400)|异步 memory engine/NoC|需要硬件-软件接口与自动编译，排除|
|Free atomics|更改 atomic/fence 语义|新 ISA/软件使用方式，排除|
|Thermometer、Sibyl|硬件辅助 profile/data placement|profile 或 OS policy 不可省略，排除|
|PIM/GPU/accelerator/quantum/security|各有硬件贡献|目标系统、软件接口或指标不等价当前 O3 throughput|

## 统一验收与来源

- 入口：`configs/example/kmhv3.py` 与 `src/cpu/o3/`；默认关闭，参数化 side-buffer/queue/port/latency。
- 固定 commit、checkpoint、warmup/ROI、cache/DRAM 和 value-pred configuration；只比较 reset 后 stats。
- RFP 若只表现为“L1 response 更早到达”而没有消耗端口、版本和失效路径，即判为不可信模型。
- 来源：[ISCA 2022 DBLP](https://dblp.org/db/conf/isca/isca2022.html)、RFP DOI；未读到全文的绝对 area/bit 保持 unknown。

### RFP 分阶段实施与资源闸门

|阶段|内容|验收指标|
|---|---|---|
|RFP-0|观察 L1 hit load 的提前完成窗口|候选 load、可提前 cycles、destination/liveness、RF port demand|
|RFP-1|side-buffer ready bit，不提前改变 consumer|L1 hit/miss、record full、port conflict、squash/replay|
|RFP-2|consumer wakeup 使用预取值|load-use stall、wakeup timing、store violation、false ready|
|RFP-3|完整 invalidation|snoop/store/exception/fault、checkpoint restore、precise state|

每个 in-flight record 至少绑定 load seqNum、destination PReg、rename version、地址/line identity、valid/ready/late；RF/side-buffer read/write、L1 data response、writeback 和 wakeup 共享有限端口。若 L1 miss、uncacheable/MMIO、atomic、store-set violation、branch squash 或 replay 发生，record 必须取消或回放，不能遗留 stale value。表项、FIFO、端口、lookup latency 和 queue-full/drop 作为显式参数。

### 目录审计台账

|主题|代表性条目|处置|
|---|---|---|
|register/data path|RFP|P0，软件透明、O3 直接落点|
|near-cache/dataflow|tākō、Tiny but mighty/MAPLE|callback/异步接口/编译器不可省略，排除|
|atomic/ISA|Free atomics|新 atomic/fence semantics，排除|
|profile/placement|Thermometer、Sibyl|profile 或 OS/data placement，排除|
|platform|PIM/GPU/accelerator/quantum/security|目标系统或指标改变，排除|

### 统一结果模板

RFP 实验按 `off / observer / RFP / value-pred / RFP+value-pred` 输出：RFP candidate/issued/hit/late/drop、L1/RF access、RF port conflict、wakeup/load-use stall、squash/replay、store/snoop invalidation、MSHR/queue、IPC/simTicks。论文 43.4%、+3.1%、+5.7%、+4.1% 只放原平台列；若当前 scalar workload 没有可提前窗口，应报告未触发。

### RFP 与现有 value-prediction 的边界

RFP 只在真实 L1 data 已可读、且 physical destination 仍属于该 load version 时提前填充；value prediction 则可能在 data 尚未返回时产生 speculative value。两者的 `ready`、confidence、replay 和 stats 必须分开，不能把一个 predictor 的命中记到另一个。若 `src/cpu/valuepred/` 已有 memory-renaming/VP 组件，也要记录它们是否占用相同 RF/LSQ/rename port；“RFP+value-pred”必须是资源共享模型，而不是两个独立的零成本加速器。

### 微测试与检查点顺序

先跑 L1-hit 长依赖、L1-miss、同地址 store invalidation、不同 PReg reuse、branch squash、exception/interrupt、atomic/MMIO 和 checkpoint restore；再跑固定 workload。每个失败都保存 load seqNum、PReg version、address/line、squash cause 和 port arbitration，方便区分 stale value、错误 wakeup 与普通 cache miss。没有完整 negative-path counter 时，RFP 的正向 IPC 不应交付。
