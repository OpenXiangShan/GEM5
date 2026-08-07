# ASPLOS 2022：面向 KMHv3/GEM5 的硬件 Feature 全量筛选

> 书目入口：[ASPLOS 2022 proceedings](https://dl.acm.org/doi/proceedings/10.1145/3503222) 与 [DBLP 目录](https://dblp.org/db/conf/asplos/asplos2022.html)。按全目录初筛，再用 DOI/公开摘要核对。论文效果只适用于原论文平台与 workload。

## 结论

本届 CPU/内存论文很多，但最有影响力的机制通常要求 compiler hint、page-table layout、guest/host 协议或新 ISA。严格 hardware-only 口径下没有可直接列为 P0 的完整方案；仍保留 CRISP 和 Every walk's a hit 作为“可拆出纯硬件消融”的条件候选，以免漏掉可复用的 scheduler/PTE-protection idea。

|状态|论文/feature|论文效果与成本（证据级别）|当前 GEM5 判断|
|---|---|---|---|
|条件 P1|CRISP|关键 slice 优先执行；memory-bound 平均 +8.4%、最高 +38%；硬件 priority/queue 修改有限，但 slice 分类和 prefix 在软件|只能实现 hardware-only critical-load predictor 消融，不能宣称复现 CRISP|
|条件 P1|Every walk's a hit|flattened page table + PTE cache priority；native +9.2%、virtualized +14.0%；需 OS page-table layout|可拆出 TLB-pressure-aware PTE replacement，完整方案仍非硬件-only|
|条件 P2|Nested elastic cuckoo page tables|nested translation 平均约 1.19--1.24×；需 guest/host page-table 结构与迁移|不适合当前 bare-metal/single-core 配置|
|排除|Pinned loads、SparseCore、TaskStream 等|ISA/compiler/programming-model 改动是机制的一部分|不进入当前 CPU feature backlog|

## 论文全集、口径和证据边界

### 扫描步骤

逐篇读取 ASPLOS 2022 proceedings/DBLP 题名，先保留 CPU front-end/backend、TLB/page table、cache、memory-system 和 prefetch 方向，再检查四个门槛：软件不变、有限硬件状态、当前 GEM5 可挂接、性能因果可用 counters 验收。系统、GPU、PIM、存储、编译器和安全项不删除，而是在排除表中写出原因。

`A` 证据表示 DOI/DBLP 书目；`B` 表示摘要或作者公开版明确给出的机制、baseline、数字；`C` 表示当前 GEM5 源码映射。摘要没有写出的面积、端口、bit 数统一记为“未披露”，不以标题或搜索摘要补齐。

## 条件候选详情

### CRISP：关键 slice 优先执行

论文：[CRISP DOI 10.1145/3503222.3507745](https://doi.org/10.1145/3503222.3507745)。CRISP 将 delinquent load/branch 的关键 slice 放到优先级更高的 scheduler 路径，用软件识别、slice 提取和 instruction prefix 告知硬件。论文对 memory-latency-bound 应用报告平均 IPC +8.4%、最高 +38%。

**为什么不是完整 hardware-only。** 如果删除软件分类，硬件仍需知道哪些指令属于 slice；若由 binary hint 传递，既有二进制不再保持不变。因此论文数字不能直接写成 GEM5 预期。可单独研究的衍生 feature 是在线 critical-load predictor：用 PC、ROB age、load miss/replay、consumer stall 建表，不读取 prefix。

**GEM5 合同。** 在 `src/cpu/o3/inst_queue.*`/`issue_queue.*` 增加有限 priority bit、lookup/update latency 和 aging；优先级不能饿死普通指令，必须保留 wakeup/select port、memory-dependence、squash 和 queue backpressure。比较 `baseline / predictor-only / predictor+priority`，统计 priority hit、误判、issue starvation、load-use stall、IQ occupancy 和 IPC。衍生实验不得引用 CRISP 的 +8.4%。

### Every walk's a hit：flattened page table 与 PTE priority

论文：[Every walk's a hit DOI 10.1145/3503222.3507718](https://doi.org/10.1145/3503222.3507718)。通过 page-table flattening 减少 walk 层数，并在 cache replacement 中优先保留 PTE，论文报告 native +9.2%、virtualized +14.0%，同时降低 cache/DRAM dynamic energy。

**条件边界。** flattening、self-reference 和物理页布局需要 kernel/allocator 合同；只在 GEM5 replacement 中保护 PTE 只能证明其中一个硬件子机制。当前 RISC-V PTW 还必须显式标记 request source，不能按地址猜 PTE。

**GEM5 合同。** 在 `src/arch/riscv/pagetable_walker.*` 为 PTE request 加 bounded `PT_WALK`/instruction-data sideband；在 `src/mem/cache/replacement_policies/` 增加 PTE class bit、插入/驱逐优先级和固定 epoch。统计 PTE L2 hit/miss、PTW latency、data-line eviction、memory traffic 和 i/d TLB MPKI。先做 `PTE-protect-only`，再评估是否有 OS/allocator 许可实施 flattening。

### Nested elastic cuckoo page tables

论文：[Nested elastic cuckoo page tables DOI 10.1145/3503222.3507720](https://doi.org/10.1145/3503222.3507720)。方案重构 guest/host page table，在并行访问约束下把最坏 nested translation 步数压低，摘要级结果约为 1.19--1.24×。guest/host 表结构、迁移和一致性是必要条件；在当前单核普通 RISC-V checkpoint 中只实现一个 TLB 表会隐藏主要成本，因此列为系统研究而非局部 feature。

## 全量边界与排除项

|方向/论文|为什么值得记录|硬件-only 决策|
|---|---|---|
|Pinned loads|load pinning 可降低 memory latency|依赖 ISA/编译器标注，排除|
|SparseCore、TaskStream|分别针对稀疏执行和任务编程模型|需要新 ISA/编程模型/调度器，排除|
|vMitosis、KLOCs、OS page management|解决 VM/allocator/内核路径|没有 OS 改动无法复现，排除|
|GPU/PIM/NDP/CXL/SSD/accelerator|有硬件性能但平台和 workload 全变|不映射当前 O3 CPU，排除|
|安全、验证、分析论文|目标不是正向 CPU throughput|保留书目，不列 feature|

## 当前 GEM5 实施与验证计划

1. **先做观测。** 固定 `kmhv3.py`、checkpoint、warmup/ROI，统计 i/d TLB miss、PTW 层数、PTE line hit/miss、IQ priority candidate 和 memory-bound stall。
2. **PTE 消融。** 只启用有限 PTE priority，比较 ordinary LRU/SRRIP 与 PTE-protect；所有 extra metadata、lookup latency、端口和 queue full 都要计数。
3. **critical-load 消融。** 只在 predictor confidence 达标时改变 issue priority；用 age cap/hysteresis 避免 starvation。
4. 对每项报告 `IPC/simTicks`、MPKI、PTW cycles、L2/DRAM traffic、queue/MSHR occupancy、误判和回退原因。论文 +9.2%/+14.0%/+8.4% 仅作背景，不作为验收阈值。

### 条件候选的实施分解

|阶段|机制范围|新增状态/端口|必须回答的风险|
|---|---|---|---|
|CRISP-0|只做 critical-load observer|PC/ROB-age/replay signature、固定 counter|在线特征能否识别 software slice 的一部分|
|CRISP-1|priority bit，不改执行语义|IQ priority、aging、select tie-break|是否产生 starvation、误判和额外 select latency|
|PTE-0|PTW source 标记|request sideband、walker queue bookkeeping|normal/two-stage/retry 路径是否都带标记|
|PTE-1|PTE insertion/victim priority|cache-line class、epoch/hysteresis|PTE hit 收益是否抵得上 data pollution|
|Nested-0|只观测 guest/host walk|两级 walk trace、walk depth/latency|当前 checkpoint 是否有 nested translation 压力|

### 资源与正确性闸门

CRISP 的 priority 不能改变 precise exception 或跳过等待的真实 load；PTE policy 不能通过虚拟地址范围硬编码 PTE，更不能让 instruction/data/two-stage 请求共享错误的 class。所有 predictor table 都要记录 bits、tag、counter saturation、lookup/update latency、table full/drop。nested page-table 方案需额外建 guest/host permission、ASID/VMID、migration 和 invalidate；在没有这些对象前只能做 observer。

### 结果解释模板

每个 workload 输出四层因果：`TLB/PTW`（miss、walk depth、PTE L2 hit、memory latency）→ `cache/queue`（PTE protection、data eviction、MSHR/DRAM）→ `core`（IQ priority、load-use/branch stall）→ `IPC/simTicks`。若 CRISP predictor 命中但 issue starvation 上升，或 PTE hit 上升但 data MPKI/traffic 增长，应保留负结果。论文摘要的 +8.4%、+38%、+9.2%、+14.0% 只作为原平台对照列。

### 候选输入/输出与故障语义

|候选|输入信号|硬件动作|不可隐藏的失败|
|---|---|---|---|
|CRISP-derived|PC、ROB age、load miss/replay、consumer wait|priority/aging/select tie-break|priority starvation、误判、wakeup/select port 冲突、squash|
|PTE-protect|PTW sideband、PTE class、L2 pressure|insertion/victim priority、epoch switch|PTE class 漏标、data pollution、sfence/fault、two-stage retry|
|Nested table observer|guest/host walk depth、ASID/VMID、permission|仅记录 candidate walk/latency|migration、permission fault、nested invalidation、restore|

CRISP-derived predictor 的 confidence 不能直接等价于 compiler slice membership；应同时输出 predictor-only 与 policy-on。PTE-protect 需要把 instruction/data PTW 分开，否则 instruction walk 可能把 data PTE protection 错计为收益。Nested observer 即使不实施 elastic cuckoo，也要记录 guest/host walk 的独立队列和 cache hit，防止把单级 TLB 缓存误报为 nested 结果。

### 全量复核台账

|主题|已检查的代表性论文|最终处置|
|---|---|---|
|slice/critical execution|CRISP、Pinned loads|CRISP 做硬件衍生消融；Pinned loads 因 ISA/compiler 排除|
|translation/page table|Every walk's a hit、Nested elastic cuckoo page tables|PTE priority 条件项；nested 需 OS/hypervisor|
|sparse/task execution|SparseCore、TaskStream|编程模型/ISA 改动，排除|
|system/device|vMitosis、KLOCs、CXL/GPU/PIM/SSD|平台或 OS 依赖，排除但保留书目|

## 来源与限制

- [ASPLOS 2022 proceedings](https://dl.acm.org/doi/proceedings/10.1145/3503222)、[DBLP TOC](https://dblp.org/db/conf/asplos/asplos2022.html)。
- 主候选 DOI 和数字均标注为摘要级时，不推断未公开的 storage/area/energy 子项。
- ACM 全文不可访问时保留“待原文核对”标记；不以搜索结果替代论文表格。
