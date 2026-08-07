# HPCA 2023：面向 KMHv3/GEM5 的硬件 Feature 全量筛选

> 书目入口：[HPCA 2023 DBLP 目录](https://dblp.org/db/conf/hpca/hpca2023.html)。本报告逐题名筛选 CPU front-end/backend、cache/replacement、TLB/translation 和 DRAM 机制；书目用 DOI/DBLP 核验，论文机制和数字只以公开摘要或作者版为准。

## 结论

本届最适合当前树的两项是 **ACIC**（小型 I-cache admission/filter）和 **BTB-X**（同容量下增大 branch coverage）。CARE 对多核共享 cache 更有价值，Speculative Register Reclamation 的性能因果清楚但生命周期风险高。Baryon/ME-HPT 一类 translation 机制保留为条件跨栈项，不能因为有硬件 table 就掩盖 page-table layout 合同。

|优先级|feature|论文效果/成本|当前 GEM5 判断|
|---|---|---|---|
|P0|ACIC|admission-controlled I-cache，平均 1.0223×|小 i-filter+temporal predictor；前端局部且可先被动观测|
|P0|BTB-X|同存储下跟踪 conventional BTB 的 2.24× branch、PDede 的 1.3×|BPU/BTB entry 编码最贴近当前树|
|P1|CARE|4/8/16 核相对 LRU +10.3%/+13.0%/+17.1%|并发感知 replacement；当前单核不能外推多核收益|
|P1|Speculative Register Reclamation|RF 减半仍 1.05×、功耗 -26%；重投节省给其它结构可 1.14×|rename/free-list/replay/squash 范围大|
|条件 P2|Baryon/ME-HPT 类|相对 radix HPT 可显著减少 translation，但需 page-table layout/映射约束|保留为 OS+hardware 项，不列纯硬件主候选|

## 全集扫描和证据边界

HPCA 2023 目录先全量归为 CPU/cache/translation、OS/runtime/compiler、GPU/PIM/accelerator、memory-system、security/reliability；不靠“论文含 hardware”决定保留。主候选必须不改既有应用/ISA/OS、状态及端口有界、可在当前 GCB checkpoint 做 causal A/B。`A`=DOI/DBLP 书目，`B`=摘要/公开作者版，`C`=GEM5 映射；未披露绝对 area/bit 的条目均不估造。

## 主候选详情

### P0：ACIC —— admission-controlled I-cache

论文：[DOI 10.1109/HPCA56546.2023.10071033](https://doi.org/10.1109/HPCA56546.2023.10071033)。ACIC 以小 i-Filter 区分 spatial/temporal access，并由 temporal predictor 判断 burst 后是否还会重用，以决定 line 是否进入 I-cache。论文平均为 1.0223×，并称填补了 LRU 到 OPT gap 的一半以上。

**建模合同。** filter tag、reuse/counter、lookup/update port、admission/bypass latency 和 miss-fill arbitration 均为有限资源；bypass 的 line 仍要在下级 cache/memory 路径真实可取。至少统计 I-cache fill/admit/bypass、后续 reuse（true/false bypass）、filter collision、L1I/L2I MPKI、fetch/decode starvation、instruction prefetch interaction。不要只以 I-cache miss 降低代替前端性能。

**GEM5 落点。** 用 `src/mem/cache/` 的 L1I fill/replacement metadata 或 L2 instruction insertion 建最小 prototype，BPU/FTQ 方的实际 fetch bubble 从 `src/cpu/o3/` 或 decoupled BPU stats 验收。比较 `LRU / observer-only / ACIC`，容量、ways 和 prefetcher 不变。

### P0：BTB-X —— target offset 分级存储

论文：[DOI 10.1109/HPCA56546.2023.10070938](https://doi.org/10.1109/HPCA56546.2023.10070938)。BTB-X 利用多数 branch target offset 很短的分布，令不同 set-associative ways 保存不同范围的 offset bit，长 target 走 overflow/扩展路径；论文称同一存储预算下可跟踪 conventional BTB 2.24×、PDede 1.3× branch。

**GEM5 合同。** 当前 `src/cpu/pred/btb/` 的 target/block metadata 是落点。参数化每 way 的 offset range、tag/valid bits、overflow entries、decode latency 和 bank port；short/long target 的 hit/miss、overflow、alias、bank conflict 和 target reconstruction 必须可见。不得仅把有效 entries 增大而不给 offset decode 或 overflow 代价。

**A/B。** `baseline / fixed-capacity conventional / BTB-X` 相同总 bit 预算；报告 branch/target MPKI、BTB hit、target correct、overflow pressure、FTQ empty、fetch bubble、branch recovery 和 IPC。若工作负载 offset 分布不集中，零收益是预期结果。

### P1：CARE —— 并发感知 cache management

论文：[DOI 10.1109/HPCA56546.2023.10071125](https://doi.org/10.1109/HPCA56546.2023.10071125)。CARE 用 pure miss contribution 评估 outstanding miss 的有效代价，并随并发度调整 cache management；论文报告 4/8/16 core 对 LRU +10.3%/+13.0%/+17.1%。

状态包括 outstanding-miss/PMC estimate、epoch policy 和 victim metadata。当前单核可以做 instrumentation 和 local policy，但不能把多核数字前移。后续共享 L2/LLC 模型需统计 per-core MLP、miss overlap、queueing、victim reason、slowdown/fairness，而非只看 aggregate IPC。

### P1：Speculative Register Reclamation

论文：[DOI 10.1109/HPCA56546.2023.10071122](https://doi.org/10.1109/HPCA56546.2023.10071122)。它在 loop 重定义中推测旧 physical register 可提前释放，只为跨 iteration 实际使用的旧映射保留状态。论文报告 RF 减半仍约 1.05×、功耗 -26%，把节约资源重新分配到其它结构时可达 1.14×。

当前 `src/cpu/o3/rename.*`、free-list、scoreboard、commit/squash 是完整范围。不能直接提前 `tryFreePReg()`：需 source-use/redefine tracking、confidence、release-width、checkpoint/rollback、exception/replay 和 load/store side effect 排除。先作 observer 统计“理论可回收 PReg”，再在 smaller-RF 对照上启用安全 reclaim；报告 free-list pressure、false reclaim/rollback、rename stall、RF port、squash 和 IPC。

## 条件项与排除矩阵

|方向|边界/处置|
|---|---|
|Baryon、ME-HPT/translation layout|需要 OS page-table/mapping、migration 或 hypervisor 合同；仅作 cross-stack research|
|GPU/PIM/NDP/FPGA/DSA|新增执行资源和软件接口，不是局部 O3 feature|
|CXL/SSD/disaggregated memory|协议、设备和 OS placement 改变，需另建平台|
|安全、可靠性、分析|可补 observability，但不是当前吞吐候选|
|compiler/profile/ISA work|软件条件不可省略，排除 strict hardware-only|

## 统一验证与来源

- 目标入口：`configs/example/kmhv3.py`、`src/cpu/pred/btb/`、`src/mem/cache/`、`src/mem/cache/replacement_policies/`、`src/cpu/o3/rename.*`。
- 每项默认关闭，状态容量/延迟/端口参数化，固定 checkpoint、warmup/ROI 后重置统计。
- 最低报告：front-end 用 BTB/I-cache/FTQ/stall；cache 用 miss/MLP/victim/traffic；reclaim 用 free-list/RF/squash；全部报告 IPC 与 metadata cost。
- [HPCA 2023 DBLP](https://dblp.org/db/conf/hpca/hpca2023.html) 及正文 DOI 是书目入口；摘要未给出的成本保留 unknown。

### 前端/后端候选的阶段闸门

|阶段|对象|先观测什么|再启用什么|
|---|---|---|---|
|ACIC-0|L1I/L2I admission|spatial/temporal ratio、line reuse、fetch starvation|filter/admission bit 与 bypass|
|ACIC-1|temporal predictor|confidence、collision、update port|固定阈值和 replacement interaction|
|BTBX-0|branch target 分布|offset width、page-cross、alias/overflow|固定 range way 与 target reconstruction|
|BTBX-1|容量等值比较|total tag/target bits、bank port、decode cycles|PDede/expanded BTB 对照|
|CARE-0|多请求者并发|outstanding miss、overlap、per-core slowdown|policy epoch/victim priority|
|SRR-0|rename observer|可提前释放 PReg、跨迭代 use/redefine|bounded speculative reclaim|

### 生命周期、成本和公平性

ACIC bypass line 若后续复用，必须重新走 cache/memory，不能把 miss 隐藏；BTB-X 的 dictionary/overflow entry、target decode 和 bank conflict 要算进同一 bit budget；CARE 的 pure miss contribution 不能用 aggregate MPKI 代替，并发策略要报告 per-core fairness。Speculative Register Reclamation 的每条 candidate mapping 要绑定 ROB/iteration/checkpoint；store/load violation、exception、branch squash、replay 后必须 rollback，free-list 不能提前回收仍可能被 consumer 使用的 PReg。所有功能完成前只允许 observer 统计。

### 全量审计台账

|主题|代表性条目|处置|
|---|---|---|
|I-cache/BPU|ACIC、BTB-X|P0，当前前端最贴近|
|cache concurrency|CARE|P1，单核 instrumentation 后再扩多核|
|rename/register|Speculative Register Reclamation|P1，生命周期高风险|
|translation/layout|Baryon、Victima-like ME-HPT|OS/page-table 条件，跨栈参考|
|OS/ISA|CRISP、Utopia、Contiguitas、K-D Bonsai|软件/ISA/mapping 不可省略，排除|
|platform|GPU/PIM/NDP/CXL/SSD/accelerator/security|平台或目标改变，排除|

### 统一报告表

前端项必须同时报告 BTB/I-cache hit/miss、target overflow、FTQ empty、decode starvation、front-end stall 和 IPC；cache 项报告 MLP、victim/reuse、MPKI、traffic 和 per-core slowdown；rename 项报告 free-list/RF pressure、reclaim/rollback、squash/replay、port use。论文的 1.0223×、2.24×、+10.3%/+13.0%/+17.1%、1.05× 只作论文列，不作为 GEM5 threshold。

### 配置敏感性建议

ACIC 应扫 filter entries、temporal counter bits、admission threshold 和 L1I/L2I latency；BTB-X 应扫 short/long offset range、overflow capacity、dictionary tag bits 和 decode cycles；CARE 应扫 epoch、concurrency window、shared-cache core count 和 fairness weight；reclamation 应扫 confidence、release width、PReg count、checkpoint depth。每次 sweep 保留一个 iso-storage/iso-port 点和一个性能上界点，防止“状态变大”掩盖机制收益。

### 功能测试清单

在 checkpoint 之前用微测试覆盖：I-cache bypass 后再次 fetch、branch target page crossing/overflow、多个 core 同时 outstanding miss、loop redefine 后旧 PReg 被异常路径读取、store-load violation 后 squash、interrupt/exception 与 checkpoint restore。只有这些测试和 counter 通过，才进入前端/多核 ROI；否则报告只能停在 observer/upper-bound。
