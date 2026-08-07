# HPCA 2021：面向 KMHv3/GEM5 的硬件 Feature 全量筛选

> 书目入口：[HPCA 2021 DBLP 目录](https://dblp.org/db/conf/hpca/hpca2021.html)。HPCA 由 IEEE 出版；本文用 DBLP/IEEE DOI 核对书目，机制和数字仅引用公开摘要或作者版明确的内容。论文 speedup 不是当前 GEM5 承诺。

## 结论

HPCA 2021 的首选不是大规模重构，而是**有限预测状态驱动的 TLB/L2 admission 和 replacement**。Dead Page + Dead Block 在当前单核 L2 上可先拆成两个局部消融，RLR 可以在 replacement-policy 框架中做 iso-storage 对照。BlockHammer 是硬件-only 但安全基线 feature，不能把它误当作裸性能加速。

|优先级|论文/feature|论文效果与硬件成本|当前 GEM5 判断|
|---|---|---|---|
|P0|Dead Page + Dead Block Predictors|11 KB；14 个 memory-intensive workload IPC 平均 +8.3%|TLB/L2 admission/dead-block predictor，状态小、因果清晰|
|P1|Reinforcement Learned Replacement (RLR)|2 MiB LLC 16.75 KB、8 MiB LLC 67 KB；相对 LRU 单核 +3.25%、4 核 +4.86%|可复用 replacement hook，但论文 LLC 与当前每核 L2 不同|
|P2|BlockHammer|RowHammer blacklist/调度，低成本安全机制|需要 RowHammer mitigation/DRAM bank model；性能可能为负|

## 全集扫描、口径和证据边界

对 HPCA 2021 目录逐题名分类：core/cache/TLB/DRAM、OS/系统、GPU/PIM/accelerator、可靠性/安全。主候选必须软件与 ISA 不变、硬件状态/端口有界、能在 `kmhv3.py` 所用的 O3/cache/DRAM 路径产生可测因果。`A`=书目/DOI；`B`=摘要/作者版机制和数字；`C`=GEM5 工程映射。未在公开摘要披露的 tag 宽度、表项拆分、面积或能耗统一标 unknown。

## 主候选详情

### P0：Dead Page and Dead Block Predictors

论文：[DOI 10.1109/HPCA51647.2021.00050](https://doi.org/10.1109/HPCA51647.2021.00050)。机制先预测 last-level TLB 中 dead-on-arrival page，再利用 page 信息预测 LLC dead block，避免无复用数据占据容量。论文在 14 个 memory-intensive workload 报告平均 IPC +8.3%，总 storage 11 KB。

**最小状态/时序合同。** 将 11 KB 作为总预算，而非无限大 PC/page signature 表。每次 fill/hit/evict 更新固定路数中的 predictor entry；预测 dead 时只能改变 insertion/bypass/victim priority，不得悄悄扩大 L2。必须统计 false-dead、lost reuse、table collision/full、bypass 后的 demand refetch、TLB 和 data-cache occupancy。预测器 lookup/update 的端口和 latency 明确参数化。

**当前 GEM5 映射。** `src/arch/riscv/tlb.*` 的 L2TLB refill/evict 以及 `src/mem/cache/replacement_policies/`/L2 fill 是最近似落点。当前 `kmhv3.py` 没有论文的共享 LLC/last-level-TLB 组合，因此按以下阶段验证：`dead-block-only`（L2）、`dead-page-admission-only`（L2TLB）、`combined`。每阶段比较相同 storage budget 的 LRU/SRRIP，报告 i/d TLB miss、L2 MPKI、victim reuse、PTW/DRAM traffic 和 IPC。

### P1：RLR —— Reinforcement Learned Replacement

论文：[DOI 10.1109/HPCA51647.2021.00033](https://doi.org/10.1109/HPCA51647.2021.00033)。RL 仅在设计阶段发现特征，部署时是纯硬件 replacement policy，不在运行时执行软件训练。论文报告相对 LRU 单核 +3.25%、4 核 +4.86%，并给 2 MiB/8 MiB LLC 16.75/67 KB 状态。

**GEM5 合同。** 以当前 `XSDRRIPRP` 的 touch/reset/getVictim 作为入口，新增 policy-local metadata；不能在每次 victim 选择遍历无界历史或调用 RL runtime。建立 iso-storage、iso-way-scan、iso-latency 的 `LRU / SRRIP / XSDRRIP / RLR` 对照，明确每 line bits、global predictor bits、lookup port、policy update epoch。统计 insertion/victim reason、reuse-distance bin、metadata hit/collision、miss overlap 和 MPKI。

**范围风险。** 论文重点是 LLC/多核，当前每核私有 L2 的一次结果只能证明局部重用特征是否存在，不能引用 +4.86%。若推进多核，共享 cache/coherence/公平性必须另建基线并报告 per-core slowdown。

### P2：BlockHammer（安全/DRAM 平台候选）

BlockHammer 用 RH blacklisting 和 memory-controller scheduling 抑制 RowHammer。它可完全由硬件生效，但研究目标是安全并控制 mitigation 开销；在没有 RowHammer 攻击/refresh baseline 的普通 DDR4 时序模型中，它很可能只增加限制，不能作为正向 IPC feature。只有在添加 DRAM bank activation、refresh/blacklist 和攻击 workload 后，才讨论性能成本、FR-FCFS interaction 和公平性。

## 全量边界与排除项

|方向|为什么不列当前 P0/P1|
|---|---|
|GreenDIMM、FAM/disaggregated memory|需要内存设备/协议和系统软件，改变当前平台|
|HoPP、NDP/PIM、FPGA/GPU accelerator|需要新执行/设备模型和对应 workload|
|persistent memory、storage/OS resource management|收益依赖持久化协议、内核或 runtime|
|可靠性/安全分析项|即使有硬件开销，也不等价于既有应用的 CPU 吞吐优化|

## 统一验证与来源

1. 固定 commit、`kmhv3.py`、checkpoint、warmup/ROI、L1/L2/DRAM 和 core count；只使用 reset 后 final stats。
2. 预测/替换项最低报告 IPC、L2/TLB MPKI、victim reuse、predictor collision/full、metadata bits、MSHR/DRAM traffic；BlockHammer 另报 activate/blacklist/throttle/refresh。
3. 默认关闭，新 feature 的表满、端口争用、错误预测和 restore 路径均有 counter；无法解释的 IPC 变化不作为成功。

来源：[HPCA 2021 DBLP](https://dblp.org/db/conf/hpca/hpca2021.html)，Dead Page/Block 与 RLR DOI 见正文。IEEE/ACM 全文不可读时，任何未在摘要出现的精确成本仍应保持 unknown。

### 分阶段实验计划

|阶段|目标|关键控制变量|输出|
|---|---|---|---|
|DPB-0|确认当前 translation/cache 是否有 dead-on-arrival 压力|L2TLB/L2 capacity、prefetch、PC/page signature|dead candidate rate、TLB/L2 reuse histogram|
|DPB-1|只做 dead-page admission|固定 predictor 11 KB budget、bypass/insert latency|false-dead、lost reuse、TLB MPKI/PTW traffic|
|DPB-2|只做 dead-block victim|相同 L2 ways/sets、replacement scan|victim reuse、refetch、L2 MPKI/IPC|
|DPB-3|combined|两个 predictor 总预算不超过论文 11 KB 等效|translation benefit 与 data pollution 的净因果|
|RLR-0|复现 reuse feature distribution|LRU/SRRIP/XSDRRIP 同容量|distance class、metadata collision|
|RLR-1|启用 fixed hardware policy|entry bits、counter/epoch、lookup latency|victim reason、MPKI、traffic、per-core fairness|

### 成本和失败模式

Dead predictor 的 storage 不能只按表项数量汇报，还要列 tag/signature、confidence、valid、epoch、read/write port；dead bypass 失败时必须重新走真实 memory path。RLR 的训练结果不能在运行时调用 Python/RL；部署模型只包含固定 comparator、counter update 和有限 history。BlockHammer 还需 bank activation counter、blacklist table、refresh/priority queue、throttle 和 timing penalty，不能在普通 DDR baseline 中隐去。

### 论文全集审计台账

|类别|代表性条目|为什么保留/排除|
|---|---|---|
|translation/cache|Dead Page + Dead Block、RLR|软件透明、状态有界，进入主候选|
|DRAM safety|BlockHammer|硬件-only，但需要攻击/mitigation baseline，列 P2|
|memory platform|GreenDIMM、FAM/disaggregated memory|设备/协议/OS 改变，排除当前单核|
|accelerator|HoPP、NDP/PIM、FPGA/GPU|新执行资源和 workload，排除|
|OS/persistent|resource management、persistent memory|kernel/protocol 条件，排除|

### 统一表格字段

每个 ROI 至少有：`feature_on/off`、paper baseline、current GEM5 baseline、storage bits、lookup/update latency、L2/TLB MPKI、victim reuse、PTW/DRAM bytes、MSHR/queue occupancy、false prediction、IPC/simTicks。若没有 workload-level 压力，报告“未触发”而不是删除候选。

### 预测器状态拆解

Dead Page/Block 的 11 KB 总预算应在设计文档中拆成 page signature/tag、dead confidence、block reuse class、valid/epoch 和更新端口；当 page predictor 判 dead 但 line 被重新访问时，必须记录 false-dead 和恢复动作，而不是把后续 miss 归给 baseline。RLR 记录 PC/region/context 是否存在、counter 位宽、replacement candidate 数、global/local history 和每 epoch 更新次数；运行时不加载 RL 模型。BlockHammer 另列 activation counter、blacklist window、refresh/priority queue、throttle cycles 和 bank fairness。

### 负结果也要有解释

对于每个 workload，按 `predictor signal -> policy decision -> resource effect -> IPC` 保存一行归因：例如 dead-block hit 上升但 L2 refetch 增加，或 RLR victim reuse 改善但 metadata lookup 让 critical path 变长。BlockHammer 应分别跑无攻击、低强度攻击、高强度攻击，报告安全事件和 slowdown；不要在无 RowHammer baseline 中把“没有错误”写成性能增益。
