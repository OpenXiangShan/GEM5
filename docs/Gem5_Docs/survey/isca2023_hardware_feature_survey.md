# ISCA 2023：面向 KMHv3/GEM5 的硬件 Feature 全量筛选

> 书目入口：[ISCA 2023 proceedings DOI 10.1145/3579371](https://dl.acm.org/doi/proceedings/10.1145/3579371) 与 [DBLP 目录](https://dblp.org/db/conf/isca/isca2023.html)。本报告只把本届论文放入主表；MICRO 2023 的 Victima 在跨会议参考节，不再混淆归属。

## 结论

ISCA 2023 适合当前树的首选是 **EMISSARY**：它只改变 L2 instruction-line insertion/replacement，性能因果可通过前端停顿和 cache 统计闭环。**Orinoco** 具有较大潜在收益，但涉及 ordered issue/unordered commit、precise exception 和 memory ordering，必须分阶段。Imprecise Store Exceptions、Utopia、Contiguitas、K-D Bonsai 等不能满足严格 hardware-only 口径。

|优先级|论文/feature|论文效果/开销|GEM5 建议|
|---|---|---|---|
|P0|EMISSARY|L2 instruction replacement，平均 +3.24%、最高 +23.7%，平均能耗 -2.1%|局部 cache policy；首批 A/B|
|P1|Orinoco|non-collapsible queue 的 ordered issue/unordered commit；IPC +14.8%，数 KiB SRAM|后端范围大，按 matrix/commit 两阶段验证|
|跨会议 P1|Victima（MICRO 2023）|native/virtualized +7.4%/+28.7%，area/power +0.04%/+0.08%|translation/L2 协同参考，非本届候选|
|排除|Imprecise Store Exceptions 等|需 RISC-V/OS memory-model、page placement 或 ISA|不列纯硬件 shortlist|

## 全集扫描、口径与证据

从 ISCA 2023 目录逐题名分类，而非只按标题关键词筛选。保留项目需：既有二进制、OS、ISA、平台不变；所有 tables/queues/ports/latency 有界；可在当前 O3/cache 路径中做 reset-stats A/B；能解释错误预测、pollution、squash 或 bandwidth 代价。`A`=书目，`B`=公开摘要/作者版，`C`=GEM5 映射；缺失的 bit/area 保持 unknown。

## 主候选详情

### P0：EMISSARY —— 按前端停顿代价的 L2 I-cache replacement

论文：[DOI 10.1145/3579371.3589097](https://doi.org/10.1145/3579371.3589097)。EMISSARY 不以 I-cache miss 数最小化为唯一目标，而是优先保护导致 decode starvation 的 instruction line；论文在带 FDIP 的 server 系统中报告平均 +3.24%、最高 +23.7%、平均能耗 -2.1%。

**状态/时序合同。** 每条 instruction line 保存或由有限 predictor 产生 miss-cost/occupation class；demand/prefetch/wrong-path fill 的训练和插入必须区分。replacement 仍在原 set/way、tag/data port、fill bandwidth 下运行；被保护 line 对 data 或低价值 instruction line 的挤出要可统计。不能仅把 I-fill 的 RRPV 调低却不提供 cost 来源。

**当前 GEM5 落点。** `kmhv3.py` L2 的 `XSDRRIPRP` 和 `src/mem/cache/replacement_policies/xs_drrip_rp.cc` 是最接近 hook；需让 cache fill/request metadata 区分 instruction source。A/B：`baseline XSDRRIP / observer-only cost tag / EMISSARY`，固定 capacity/prefetch/BPU。统计 L1I/L2I miss、decode/FTQ starvation、instruction fill/evict by class、victim reuse、prefetch pollution、MSHR/DRAM traffic 和 IPC。当前没有完整论文 FDIP 时，结果只能说明局部 replacement 可行性。

### P1：Orinoco —— 可排序但不搬移的后端队列

论文：[DOI 10.1145/3579371.3589046](https://doi.org/10.1145/3579371.3589046)。Orinoco 用 age-matrix bit-count encoding、commit-dependency matrix 和 memory-disambiguation matrix，让固定物理槽位的 non-collapsible queue 支持 ordered issue/unordered commit；论文以 8T SRAM 实现，状态为数 KiB，IPC +14.8%。

**为什么高风险。** GEM5 不能只在 scheduler 中打乱 `seqNum`。precise exception、load/store ordering、squash、replay、branch recovery、commit bandwidth 以及 rename/free-list 释放时刻必须全部有明确定义。先实现 `age matrix` 作为 selection observability，仍保持 in-order commit；再加入 independent completion/commit dependency；最后才讨论 unordered commit。每步都有 microbenchmark（exception、store-load violation、branch squash）和功能检查。

**统计。** matrix occupancy/update/bit cost、select winner/blocked reason、issue/commit width、queue fragmentation、replay/squash/exception、memory disambiguation conflict、IPC；与普通 collapsing IQ 保持同一 entries、ports、latency。

## 跨会议参考：Victima（MICRO 2023）

论文：[DOI 10.1145/3613424.3614276](https://doi.org/10.1145/3613424.3614276)。Victima 根据 PTW cost 把高代价 translation 以 L2-backed entry 形式保留，并用 TLB-aware replacement 约束 data-cache pollution；它符合硬件透明定义，但属于 MICRO 2023。GEM5 上需完成 RISC-V PTW source tag、L2 metadata、TLB/PWC fill/evict lifecycle；单独的实现/验证应回到 `micro2023_hardware_feature_survey.md`。

## 全量边界与排除

|方向/论文|原因|
|---|---|
|Imprecise Store Exceptions|异常与 retired store 语义需要 ISA/OS 协作|
|Utopia、Contiguitas|OS allocator/physical mapping 是收益前提|
|K-D Bonsai|需要 ISA extension|
|GPU/NPU/PIM/CXL/SSD/accelerator|新增平台、协议或编程模型|
|security/reliability/analysis|可作为约束来源，但非透明吞吐 feature|

## 统一验证与来源

- 路径：`configs/example/kmhv3.py`、`src/mem/cache/replacement_policies/`、`src/cpu/o3/`、RISC-V TLB/PTW。
- 默认关闭；显式建 metadata bits、lookup/update latency、way/port contention、table full/drop/retry 与 checkpoint restore。
- 固定 workload/checkpoint/warmup/ROI，reset 后报告 IPC、cache/TLB/FTQ/IQ counters、traffic 和 feature-specific failure reasons。
- 来源：[ISCA 2023 DBLP](https://dblp.org/db/conf/isca/isca2023.html) 与正文 DOI；论文平台的 +3.24%/+14.8% 不作为 GEM5 验收门槛。

### EMISSARY/Orinoco 分阶段闸门

|阶段|对象|实现边界|关键 counters|
|---|---|---|---|
|EM-0|instruction source observer|给 L2 fill/evict 标记 I/D、demand/prefetch/wrong-path|L1I/L2I MPKI、decode starvation、victim reuse|
|EM-1|cost predictor only|固定 PC/line/cost metadata，不改 victim|confidence、collision、update latency|
|EM-2|replacement-on|保持 XSDRRIP ways/ports/latency|victim reason、pollution、MSHR/DRAM、IPC|
|OR-0|age/dep matrix observer|不改变 commit/order|matrix bits/update、ready/select distribution|
|OR-1|ordered issue/unordered completion|仍保持 precise in-order commit|replay/squash、dependency blocked、queue fragmentation|
|OR-2|commit dependency policy|逐项开放 unordered commit|exception/store ordering/branch recovery|

EMISSARY 不能通过无条件保护所有 I-lines；若 decode starvation 没有下降而 data pollution 上升，应回退。Orinoco 的 matrix bit-count、cross-entry update、select/forwarding port 和 fixed slots 要算面积/时序；`seqNum` 只是年龄标签，不能单独证明 unordered commit 正确。任何 exception、memory-order violation、replay、squash 或 checkpoint restore 失败都阻止性能 run。

### 本届目录审计台账

|主题|代表性条目|处置|
|---|---|---|
|instruction cache|EMISSARY|P0，L2 policy 直接可挂接|
|backend queue|Orinoco|P1，分阶段验证|
|store/ISA semantics|Imprecise Store Exceptions|需要 ISA/OS memory-model，排除|
|page layout|Utopia、Contiguitas|OS allocator/mapping，跨栈参考|
|ISA extension|K-D Bonsai|新 ISA，排除|
|platform/security|GPU/NPU/PIM/CXL/SSD/security|目标平台或指标改变，排除|

### 统一验收字段

EMISSARY 输出 I-cache/L2I miss、decode/FTQ starvation、I/D victim、prefetch pollution、MSHR/DRAM traffic、cost predictor hit/evict 和 IPC；Orinoco 输出 issue/commit width、matrix occupancy/update、queue full/fragmentation、dependency blocked、replay/squash/exception、memory-order violation 和 IPC。论文 +3.24%/+23.7%/+14.8% 不能替代这些 counters。

### 基线、预算和 trace 复用

EMISSARY 的论文基线含 FDIP；当前 GEM5 若没有同等 FDIP，应将 `current XSDRRIP`、`instruction-only observer` 与 `cost-aware replacement` 分列，不能把普通 L2I improvement 写成复现论文结果。Orinoco 的数 KiB SRAM 要拆成 age/dependency/disambiguation matrix bits、slot valid、update/read port 和 arbitration；固定物理槽位节省的搬移能耗不能隐藏额外矩阵更新。

复用 trace 时保存 instruction source、fill cause、decode starvation interval、ROB/IQ age、load/store dependency、squash/exception、cache line eviction。这样能将 EMISSARY 的 cost predictor 训练与 Orinoco 的 queue selector 分开重放，并在不重跑完整 workload 时验证错误路径。任何 trace replay 只可作为行为对拍，最终性能仍需 timing GEM5 A/B。

### 功能验收清单

EMISSARY：I-cache miss、decode starvation、branch redirect、instruction/data alias、prefetch fill；Orinoco：ready/wakeup/select、cross-cluster forward、store-load violation、precise exception、interrupt、branch squash、commit ordering。若某项没有对应 microbenchmark 和 stats，就保持 P1/P2，不升级为“可实现 P0”。
