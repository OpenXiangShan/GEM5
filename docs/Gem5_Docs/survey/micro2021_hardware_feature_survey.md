# MICRO 2021：面向 KMHv3/GEM5 的硬件 Feature 全量筛选

> 书目入口：[MICRO 2021 proceedings](https://dl.acm.org/doi/proceedings/10.1145/3466752) 与 [DBLP 目录](https://dblp.org/db/conf/micro/micro2021.html)。目录逐题名初筛、候选以 DOI/公开摘要核对。ATP/SBFP 属 ISCA 2021、Voyager 属 ASPLOS 2021，均只放跨会议参考，不混入本届主表。

## 结论

**Pythia** 和 **PDede** 是本届最直接的当前 GEM5 候选：前者是有界的 online bandit prefetch，后者是前端 BTB 容量编码；二者都能在明确 resource budget 下用现有 cache/BPU stats 解释收益。所有含 software hint、offline profile、GPU/OS 虚存或 accelerator 的论文均保留在边界表，避免“高 speedup”遮掩软件依赖。

|优先级|论文/feature|论文结果与硬件成本|GEM5 判断|
|---|---|---|---|
|P0|Pythia|单核相对 MLOP/Bingo +3.4%/+3.8%，12 核 +7.7%/+9.6%，面积 +1.03%|L2 prefetch queue/统计可复用；必须有 bounded action/context 与带宽反馈|
|P0|PDede|BTB miss 平均 -54.7%，IPC 平均 +14.4%（最高 76%）|BTB target dedup/delta 编码，当前 BPU 最适配|
|排除|Branch-Mispredict Level Parallelism|摘要级平均 +29%|需要 software hints|
|排除|Twig、Ripple|BTB/I-cache 提示方向|依赖 offline profile|
|排除|Trident、Morrigan 等|GPU/OS VM/平台方向|当前单核 O3 不可局部复现|

## 全集扫描和证据纪律

全量扫描从 MICRO 2021 DBLP 目录开始；任何 CPU/cache/BPU/TLB/DRAM 题名先纳入，再检查软件透明性、有限状态/端口、当前路径和 A/B 可验收性。`A`=DOI/DBLP，`B`=摘要/公开作者版，`C`=GEM5 映射。论文 speedup 仅是 `B` 级论文事实；未公开 bit/latency/port 不猜测。

## 主候选详情

### P0：Pythia —— hardware online bandit prefetching

论文：[DOI 10.1145/3466752.3480114](https://doi.org/10.1145/3466752.3480114)。Pythia 将 PC、地址/stride 等 context 与带宽/accuracy feedback 结合，使用 bandit reward 选择预取 action；论文报告单核相对 MLOP/Bingo +3.4%/+3.8%，12 核 +7.7%/+9.6%，synthesis 面积 +1.03%。

**状态/成本合同。** action/context table、reward/counter、epoch、lookup/update port、action fanout、prefetch queue 和 throttle 必须固定大小，且每 demand 只做 O(1) 或固定 action 数工作。reward 不能读取未来 demand；bandwidth feedback 使用实际 issued/useful/late/unused 或 DRAM occupancy。任何表项扩容都连同 bits、port 和更新时间一起扫参。

**GEM5 路径和 A/B。** 在 `src/mem/cache/prefetch/` 复用 `Queued`、MSHR、queue drop/retry 和 `PrefetchStats`，不要修改 KMHv3 对齐的既有 SMS/PHT 语义。比较 `off / current baseline prefetcher / Pythia / Pythia with bandwidth throttle`，固定 degree/queue/DRAM。报告 coverage、accuracy、timeliness、pollution、action selection、reward distribution、queue full、MSHR/DRAM traffic、IPC；多核 only 结果需另建共享层/公平性基线。

### P0：PDede —— 存储高效 BTB

论文：[DOI 10.1145/3466752.3480046](https://doi.org/10.1145/3466752.3480046)。PDede 按 region 分区，去重相同 target，并以 page 内 delta 编码 target，从而在同一 bit budget 下跟踪更多 branch；论文覆盖百余前端受限应用，报告 BTB miss -54.7%、IPC 平均 +14.4%（最高 76%）。

**状态/时序合同。** 必须明确 region/tag、target dictionary、delta width、dedup compare、overflow target、allocation/free/evict 与 decode latency。dictionary hit 和 delta reconstruction 占用 BPU lookup/bank port；长 target 或跨页 target 的 fallback 不能被当作命中。checkpoint/restore 和 ASID/context change 后 metadata identity 必须清楚。

**GEM5 路径和验收。** 在 `src/cpu/pred/btb/` 的 block/target metadata 建 `total-bit-equal` 实现；比较 conventional BTB、fixed-entry expanded BTB 和 PDede，区分容量收益与编码收益。统计 target/branch hit、alias、dedup hit、overflow、bank conflict、fetch bubble、FTQ empty、mispredict recovery 与 IPC。若只增加 entry 数而不扣 target metadata，不是 PDede 模型。

## 跨会议实现参考（不改变本届归属）

- **ATP+SBFP**：[ISCA 2021 DOI](https://doi.org/10.1109/ISCA52012.2021.00016)，适合 RISC-V TLB/PTW；详见 `isca2021_hardware_feature_survey.md`。
- **Voyager**：[ASPLOS 2021 DOI](https://doi.org/10.1145/3445814.3446752)，神经预取的推理/训练延迟需要独立建模；详见 `asplos2021_hardware_feature_survey.md`。

## 全量边界与排除

|论文/方向|不列入 strict hardware-only 的理由|
|---|---|
|Enabling Branch-Mispredict Level Parallelism|software hint 参与机制，硬件不能自行判定同样的并行级别|
|Twig、Ripple|profile-guided BTB/I-cache，offline information 是效果来源|
|Trident、Morrigan 多 GPU/OS 变体|目标 GPU 或虚拟内存软件栈，改变平台/OS|
|PIM/NDP/FPGA/accelerator、网络/SSD|新设备、driver/接口和 workload|
|安全、分析、验证|不构成透明的通用 CPU throughput feature|

## 统一验证与来源

- 入口：`configs/example/kmhv3.py`、`src/cpu/pred/`、`src/mem/cache/prefetch/`；新增参数默认关闭。
- 固定 checkpoint/warmup/ROI 后 reset stats，报告 IPC/simTicks、front-end/PF counters、queue/bandwidth/port 使用与 metadata budget。
- 不把 Pythia/PDede 论文数字当基准验收；必须用当前需求 MPKI、mispredict、useful/late 和 traffic 因果解释结果。
- 来源：[MICRO 2021 DBLP](https://dblp.org/db/conf/micro/micro2021.html) 及正文 DOI；全文不可读时保持成本字段 unknown。

### Pythia/PDede 分阶段计划

|阶段|对象|限制|验收|
|---|---|---|---|
|Pythia-0|现有 prefetch baseline|固定 degree/queue/DRAM|确认 workload 的 demand MPKI、bandwidth headroom|
|Pythia-1|bandit observer|context/action/reward table 只读|action distribution、reward、table collision|
|Pythia-2|admission/degree-on|固定 action fanout、update epoch|useful/late/unused、queue drop、traffic、IPC|
|PDede-0|branch target distribution|不改 BTB policy|offset width、region reuse、alias/overflow|
|PDede-1|dictionary/delta encoding|总 tag/target bits 等值|dedup hit、decode latency、bank conflict|
|PDede-2|capacity/replacement-on|fixed ways/ports|BTB/target MPKI、FTQ empty、mispredict recovery|

Pythia 的面积 +1.03% 不能替代当前表项/端口预算；reward 必须由已完成的 useful/late/unused/traffic 事件产生。PDede 的 target dictionary、delta reconstruction、cross-page fallback、region tag 和 free/evict metadata 要在同一总 bit budget 内，不能以扩大 entry 数代替编码收益。两项都必须保留 table-full/drop、retry/backpressure、checkpoint restore 和默认 off。

### 全量主题审计

|主题|代表性条目|处置|
|---|---|---|
|prefetch control|Pythia|P0，现有 L2 queue 可挂接|
|BTB compression|PDede|P0，BPU/BTB 直接落点|
|software-guided branch|Branch-Mispredict Level Parallelism|software hints 必要，排除|
|profile-guided frontend|Twig、Ripple|offline profile 必要，排除|
|GPU/VM/system|Trident、Morrigan 变体|平台/OS/workload 改变，排除|
|device/security|PIM/NDP/FPGA/SSD/网络/安全|不属于当前通用 CPU throughput|

### 统一结果表

Pythia 输出 action/epoch/reward、issued/useful/late/unused、prefetch/demand MPKI、pollution、MSHR/queue/DRAM bytes、metadata bits 和 IPC；PDede 输出 branch/target hit/miss、alias、dedup/overflow、delta decode、bank conflict、FTQ empty、frontend stall、recovery 和 IPC。论文 +3.4%/+3.8%、+7.7%/+9.6%、-54.7%、+14.4%/76% 只作原论文列，多核结果不得从单核外推。

### 组合与互斥实验

Pythia 与当前 BOP/MLOP/Bingo 的比较必须固定 demand/prefetch queue、degree、MSHR 和 DRAM bandwidth；如果 Pythia 只作 manager，要单独报告 address generator 不变的结果。PDede 与 BTB-X/其它 target compression 组合时，先做各自 total-bit-equal 消融，再做组合，避免 dedup dictionary 和 delta dictionary 重复计入。所有 profile 提供 `feature off / observer only / policy on / stress capacity` 四个点。

### 前端功能测试

PDede 需要覆盖短 offset、长 offset overflow、跨 page target、同 region target dedup、context/ASID change、BTB eviction 和 branch redirect；Pythia 需要覆盖 demand/prefetch merge、late response、queue full、bandwidth throttle、squash/flush。测试日志保存 branch/load sequence、table index、action、drop reason 和 response cycle，之后再切 checkpoint ROI。
