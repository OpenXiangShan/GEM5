# MICRO 2025: GEM5/KMHv3 纯硬件性能 Feature 调研

> 调研日期：2026-08-05。结论面向本仓库 `configs/example/kmhv3.py` 的 O3 CPU、
> classic memory hierarchy 与单核/少核 checkpoint 工作流，而非任意处理器的通用排名。

## 结论

MICRO 2025 中最值得立项的机制不一定有最大的论文加速比。这里的排序要求它同时满足：
不改 binary、OS、ISA 或运行时协议；能在 GEM5 中保留完整的性能因果链；硬件状态、端口
和恢复语义能用有限、可参数化的模型表达。

第一轮只建议两个独立的 A/B 原型：

1. **Kairos**：使用访问指令相关性，过滤低价值 temporal metadata，并按预取潜力保留
   correlation。它可复用当前 L2 composite/CMC 的请求、去重、队列和按 source 统计，
   只需增加一个有界的 PC utility 层。
2. **ATR**：利用无条件分支且不会异常的 atomic commit region，提前释放不再需要的旧
   physical register。当前释放已经集中在 `Rename::removeFromHistory()`，所以 baseline
   和观察点明确；但先要证明 squash/exception 下寄存器生命周期正确。

之后才是 **Micro-MAMA**（多核预取协调）、**Drishti**（sliced LLC replacement）和
**Multi-Stream Squash Reuse**（控制独立指令重用）。它们都可由硬件实现，但分别需要共享
资源反馈、尚不存在的 LLC base policy，或跨 fetch/rename/ROB/LSQ 的恢复协议。**RICH**
只适合高延迟/CXL 内存专项，不应从当前 DDR4 单核基线直接立项。

所有百分比都是论文在其基线、workload 和配置下的结果，**不是当前 GEM5 的预期 IPC
收益**。本树的结论只能来自相同 checkpoint、warmup/ROI 和 reset 后 stats 的 A/B。

## 范围与证据

### 严格的“只改硬件”标准

纳入 shortlist 必须同时满足：

1. 既有 RISC-V binary、Linux/裸机环境、checkpoint 和输入保持不变；不是 compiler hint、
   PGO、OS page-table/allocator、runtime helper thread 或 software prefetch 带来的收益。
2. 决策、状态更新和资源仲裁可由 core/cache/memory hierarchy 硬件完成。训练表、计数器
   和 policy table 可以是硬件状态，但离线软件 oracle 不可以。
3. GEM5 能表达 `workload event -> state/resource -> contention or latency -> observable
   stats`；只改变功能结果而不保留性能因果链的模型不计入。

### 论文语料与证据等级

官方 ACM proceedings 是 [10.1145/3725843](https://dl.acm.org/doi/proceedings/10.1145/3725843)。
以其 container title 查询 Crossref 后得到 **123 篇**文章；[MICRO 58 官方日程](https://microarch.org/micro58/program/)
用于核对候选的 session、题名和作者。

本次直连和使用用户提供的 `172.38.8.77:7897` 代理访问 ACM proceedings、ePDF 和候选
PDF 都返回 Cloudflare `HTTP 403`。因此采用以下边界：

| 证据等级 | 本文能确认的内容 | 使用方式 |
| --- | --- | --- |
| A | 官方 proceedings DOI、官方日程的题名/作者/session | 论文身份和链接 |
| B | OpenAlex/Semantic Scholar 索引的论文摘要 | 机制、摘要明确写出的效果和开销 |
| C | 当前 GEM5 源码 | 插入点、可复用状态、实现风险和验证方案 |
| 不可得 | 只在全文表格/综合章节出现的面积、能耗、位宽或分项数据 | 标为“摘要未给出”，不作推断 |

没有绝对数字时，“开销”列写必须落地的 SRAM/寄存器/比较器/端口状态和参数化公式，
不把摘要中的 `lightweight` 误报为某个 KiB 数。

## 决策总表

| 优先级 | 机制 | 纯硬件 | 论文效果与开销（仅论文口径） | 当前 GEM5 结论 |
| --- | --- | --- | --- | --- |
| P0-A | **Kairos**: *Elevating Temporal Prefetching Through Instruction Correlation* | 是 | 相对 IP-stride 总体 **+25.2%**，相对 Triangel **+10.1%**；称 metadata 低两个数量级，摘要未给绝对 KiB | 最优先。扩展 L2 CMC/composite，先建 PC criticality 和 utility。 |
| P0-B | **ATR**: *Out-of-Order Register Release Exploiting Atomic Regions* | 是 | SPEC17int：64-entry PRF **+5.13%**，224-entry PRF **+1.48%**；称不需要 stack、queue、额外 memory 或 shadow cell | 最优先但有正确性闸门；release path 和 history 已存在。 |
| P1 | **Micro-MAMA**: *Multi-Agent Reinforcement Learning for Multicore Prefetching* | 是 | 8-core 相对独立 agents：吞吐 **+2.1%**、fairness **+10.4%**；只称 lightweight | 多核项目。需共享带宽/公平性 feedback 和 central supervisor。 |
| P2 | **Multi-Stream Squash Reuse** | 是 | SPECint2006/2017/GAP IPC **+2.2%/+0.8%/+2.4%**；摘要未给绝对面积 | 高风险前后端协同；先验证恢复和结果重用正确性。 |
| P2 | **Drishti**: sliced LLC replacement | 是 | 32-core 相对 LRU：Hawkeye **+5.6%**、Mockingjay **+13.2%**；无 Drishti 时 +3.3/+6.7% | 当前没有这两个 base policy，先补公平的基线。 |
| P3 | **RICH Prefetcher** | 是 | 高延迟内存相对 Bingo **+8.3%**、PMP **+6.2%**；常规系统相对 Bingo **+3.4%** | 只在 CXL/NVM/高延迟 memory model 中研究。 |

## 主候选

### P0-A: Kairos - 指令相关的 temporal metadata 选择

**论文概述。** Kairos 认为 temporal prefetcher 的 metadata 不应被所有 load instruction
同等训练和保留。它识别反复出现、对预取覆盖有贡献的关键 memory instruction，过滤低访问
概率 instruction 的干扰，并用 utility 保留高预取潜力 correlation。摘要报告相对 IP-stride
总体 +25.2%、相对 Triangel +10.1%，并称 metadata 开销比对照低两个数量级。可访问摘要
没有基准容量或绝对 KiB，不能写成固定 storage 值。

**硬件开销。** 需要有界 `CriticalPCTable`（PC tag、access/coverage/utility saturating
counter）和 temporal correlation storage（trigger/tag、target block/offset、utility/age）。
每次 access 最多一次 PC table lookup、一次 metadata lookup 和固定路数 victim 选择。
GEM5 参数应显式包含 `pc_entries`、associativity、counter bits、`metadata_entries`、
最大 degree 和 decay period；默认 `enable_kairos=false`，保持原 CMC 行为。

**当前 GEM5 落点。** `src/mem/cache/prefetch/l2_composite_with_worker.*` 已把 BOP、CDP、
CMC、Despacito 放在同一个 L2 manager，复用 `pfLRUFilter`、prefetch queue、MSHR
backpressure 和 `prefetchStats.pfIssued_srcs`/`pfUseful_srcs`。`cmc.*` 已按 `(PC, block
address)` 训练 temporal chain。第一版应新增独立 `KairosPrefetcher`，或给 CMC 增加
可关闭的 utility layer；不能把现有 CMC 直接改名为 Kairos。

**建模合同。**

- 事件：demand access/miss、prefetch issue、prefetch hit/useful、late/unused、metadata
  eviction。
- 状态/资源：PC criticality/coverage、entry utility/age、L2 prefetch queue、MSHR 和下游
  bandwidth token。
- 转移：`observe -> classify -> train or filter -> correlation hit -> issue ->
  useful/late/unused -> utility update or evict`。queue/MSHR 拒绝沿用现有 backpressure。
- 统计：每 PC/entry 的 train/filter/hit/evict、issued/useful/late/unused、queue occupancy、
  MSHR full、demand miss latency 和 L2 pollution。每请求 O(1)，每 epoch 仅遍历固定表。

**最小验证。** 同一 checkpoint 跑 baseline CMC、Kairos filtering、Kairos + utility
replacement 和 capacity sweep。IPC 上升不是充分条件：低 utility metadata/unused prefetch
必须下降，且 demand miss latency、MSHR pressure 和 queue occupancy 能解释变化。

### P0-B: ATR - atomic commit region 中的提前寄存器释放

**论文概述。** 常规 rename 要等重定义同一 architectural register 的指令 commit 才能
释放旧 physical register，造成已不再使用的寄存器长期占用。ATR 利用 atomic commit
region：region 内没有 conditional branch 或可能异常的指令，因而整体保证一起 commit 或
flush；在这一区间可安全提前释放。摘要给出 SPEC2017int 中超过 17% allocated register
位于此类 region，并报告 64-entry PRF +5.13%、224-entry PRF +1.48%。

**硬件开销。** 摘要明确称识别机制不需要 stack、queue、额外 memory 或 shadow cell，
但未给综合面积。实现仍有有限 control state：region open/close 和 history entry 的
release eligibility。GEM5 应把它们视为参数化 control bits，而不把该表述误读成零门数。

**当前 GEM5 落点。** `Rename::removeFromHistory()` 仅在 commit progress 后调用
`tryFreePReg()`，`Rename::doSquash()` 回收被撤销 mapping；`historyBuffer` 和
`ratSnapshotBuffer` 已提供 lifecycle/recovery 骨架。`renameSrcRegs()` 绑定 source，
`renameDestRegs()` 记录 `(newPhysReg, prevPhysReg)`。因此不能简单在 rename 时释放，
而应在这条已有生命周期中加 ATR gate。

**建模合同。**

- 事件：rename destination、region boundary、producer/consumer progress、commit、squash、
  trap/exception。
- 状态/资源：每 thread region state、history eligibility、int/fp free list 与
  `phyregReleaseWidth` token。
- 转移：`renamed -> region-tracked -> ATR-safe -> early-release`；一旦被 branch、可能
  exception 或 squash 打断，退回常规 commit release。每周期沿用现有 release width。
- 统计：`earlyReleaseCount`、`earlyReleaseBlocked{branch,exception,olderUse,squash}`、
  free-list occupancy、rename register-full stall、region length 和 cancellation。

**验证闸门。** 先以小 PRF（64/96/128）放大压力，跑 branch、faulting load、syscall/interrupt、
replay 和 squash 定向测试；随后 difftest/回归，最后才在 224 PReg checkpoint 上看 IPC。
任何 mismatch 都回到 lifetime/rollback 设计，不能放宽 guard。

### P1: Micro-MAMA - 多核预取器的共享资源协调

**论文概述。** 多个独立学习的 prefetch agent 在带宽有限时会争抢共享资源。Micro-MAMA
用 central agent 学习 joint policy，并把系统视角反馈给 local multi-armed-bandit agent。
摘要报告 8-core 下相对独立 agent 吞吐 +2.1%、fairness +10.4%，且带宽越紧优势越大。

**硬件开销。** 摘要只称 lightweight，未给 KiB/面积。至少需要每 core/source 的 local
action-value 表、固定 action 集（enable/degree/priority）、central joint-policy table、
epoch counter 和共享资源计数器。参数化大小为 `cores * agents * actions * counter_bits`
加 central table entries；每 epoch 更新有限表，而不是每 access 跑通用 RL。

**当前落点与判定。** 每 core L2 已可使用 composite prefetcher，且有按 source 的
issued/useful 计数；`configs/common/xiangshan.py` 支持 `--num-cpus`/SMT。缺口是跨核
DRAM bandwidth、queueing latency、per-core progress/fairness 和 policy broadcast。必须在
4/8 core workload mix 中报告 weighted speedup、Jain fairness、per-core prefetch stats、
memory queue occupancy 与带宽份额；单核调 degree 不算 Micro-MAMA。

### P2: Multi-Stream Squash Reuse - 跨多个错误路径保留结果

**论文概述。** 分支错预测时，传统核心无差别 squash 后续指令，即使 redirect 路径之后
仍会执行其中的 control-independent instruction。论文发现 redirect stream 能与多个、
不只是最近一个 squashed stream reconverge；只比较最后一个会遗漏平均 10%、最多 31% 的
opportunity。机制以 tagged rename mapping 比较多份执行状态并复用结果。摘要报告
SPECint2006/2017/GAP IPC +2.2%/+0.8%/+2.4%。

**硬件开销。** 摘要未给绝对面积。主要成本是保存 `K` 个可恢复 squashed stream 的 tagged
rename mapping、valid/age、结果有效性和比较端口；状态量约为
`K * N_arch_regs * (phys-reg id + version/tag + valid)`，并需与 ROB/LSQ/load-store
ordering 关联。`K`、最大 mapping 和每周期比较数必须参数化，不能线性扫描旧 stream。

**当前落点与风险。** 当前 `Commit/IEW/Rename` squash 让 ROB、IQ、LSQ 和 rename history
回到单一正确路径；`ratSnapshotBuffer` 只服务恢复，不保存可重用结果。原型必须从 fetch
redirect、`Rename::doSquash()`、ROB/LSQ ownership 建立 lifecycle，确认 operands、load
value、store/exception 和 side effect 都能证明有效。这是高风险跨模块项目。

**验证。** 先只允许无 load/store、无 exception 的 integer subset；记录
`candidate/tag-match/reuse/rejected{data,load,store,exception,version}`、squashed work 和
redirect cycles。随后才加寄存器依赖、cache miss 与 memory ordering。

### P2: Drishti - sliced LLC 的 reuse predictor 与动态 sampled cache

**论文概述。** Drishti 面向 many-core sliced LLC：每 slice 的 reuse predictor 只看到
局部访问，固定 sampled set 又可能缺少 LLC miss。它使用 per-core yet global reuse
predictor、per-slice local sampled cache 和动态 sample-set 选择。摘要在 32-core 配置中
报告，相对 LRU，Hawkeye/Mockingjay 从 +3.3%/+6.7% 提升为 +5.6%/+13.2%。该口径不能
外推到单核。

**硬件开销与落点。** 摘要未给绝对 storage/area。结构包括每 core reuse-predictor table、
每 slice sample-set metadata、dynamic selector 和跨 slice 更新端口，成本随 core/slice/
entries 线性增加。`kmhv3.py` 已配 L3 `num_slices = 4`，但树中直接配置的是 L2
`XSDRRIPRP`，没有 Hawkeye/Mockingjay。不能只给 DRRIP 多加 sample set 后仍称 Drishti；
应先引入并验证 base reuse policy，再比较 monolithic/sliced、fixed/dynamic、local/
per-core-global 设计，故是条件多核研究。

### P3: RICH - 用 off-chip metadata 换高延迟内存的 latency hiding

**论文概述。** RICH 面向 NVM 或 CXL pooled memory 等高容量/高带宽但高 latency 的系统。
它把多种 region size 和 trigger 的 spatial information 分层放在片上/片外，主动以
capacity 和 bandwidth 换 prefetch coverage。摘要称高 latency 下相对 Bingo +8.3%、
PMP +6.2%，常规系统相对 Bingo +3.4%。

**硬件开销和边界。** 片上部分是 metadata cache/selector，片外 metadata 明确消耗容量
并产生读写带宽；摘要未给绝对大小。当前默认是 XiangShan DDR4/DRAMsim3，L2 composite
也没有 RICH metadata memory protocol。应先校准 CXL/NVM latency、metadata traffic、
coherence 和 pollution，当前 DDR4 路径不应承诺论文的 3.4%。

## 明确排除或延后

| 论文/方向 | 论文摘要中的价值 | 为什么不符合本次 shortlist |
| --- | --- | --- |
| **TRRIP** | I-cache L2 MPKI -26.5%，PGO mobile code 上 +3.9% | 明确是 compiler code transform + OS code-page attribute + hardware；只改硬件没有输入。 |
| **Learning to Walk / LVM** | 翻译开销平均 -44%，程序 +2% 至 +27% | 需要 Linux OS extension 和新 page-table structure。 |
| **LoopFrog** | SPEC06/17 程序 +9.2%/+9.5% | 要 LLVM 插入 loop hint 才能 spawn threadlet。 |
| **Ghost Threading** | idle server 1.33x | 论文是 software-only helper-thread prefetch，依赖软件抽取和同步。 |
| **Software Prefetch Multicast**、**Symbiotic Task Scheduling and Data Prefetching** | 预取/调度研究 | 闭环包含 software prefetch 或 task scheduler。 |
| **Beyond Page Migration** | tiered-memory 管理 | page migration/allocator 是 OS policy。 |
| **SHADOW** | 九个 benchmark 平均 1.33x、最高 3.16x；称 1% area/power | 需新 InO context、TLP 线程供给和 runtime mix；不是当前单线程 checkpoint 的局部 feature。 |
| **CryptoBTB**、**ShadowBinding**、RowHammer/secure-memory | 降低安全防护开销 | 目标是安全等价下减小 overhead，不是提高当前非安全 baseline IPC。 |
| GPU/PIM/NPU/LLM/quantum/存储加速器 | 专用域性能可观 | 需要新 ISA、设备或应用映射，超出通用 O3 CPU 改动。 |

## 当前 GEM5 事实与实施顺序

| 主题 | 当前源码事实 | 对本调研的含义 |
| --- | --- | --- |
| L2 prefetch | `L2CompositeWithWorkerPrefetcher` 串联 BOP/CDP/CMC/Despacito，复用 filter、queue 和 source stats | Kairos 可复用现有 prefetch 生命周期。 |
| temporal baseline | `CMCPrefetcher` 按 `(PC, block address)` 训练 temporal chain | 可做 CMC-vs-Kairos A/B，不能把 CMC 算作 Kairos。 |
| PReg release | rename history 在 commit 释放 `prevPhysReg`，squash 回收新 mapping | ATR 必须同时满足 history、squash 和 free-list 生命周期。 |
| cache hierarchy | 每 CPU L2 默认 2 MB；L3 可配 4 slices；L2 使用 `XSDRRIPRP` | Drishti 的拓扑有落点，论文 base policy 没有。 |
| 多核 | config 支持 `--num-cpus` 和多核 reference 路径 | Micro-MAMA 只能在共享资源的 workload mix 中验证。 |

推荐顺序：

1. 固定 commit、`kmhv3.py` 参数、core 数、memory ini、checkpoint、warmup/ROI；只比较
   reset 后最终 stats，单核与多核分开。
2. 实现 Kairos：先 critical-PC filtering，后 utility replacement；记录 metadata 预算和
   每周期 lookup/update 上限，证明 queue/MSHR 压力趋势正确。
3. 实现 ATR：先用缩小 PRF 的定向正确性测试，后做 224-entry 配置 A/B。任何 mismatch
   回到 lifecycle/rollback，不放宽 assertion。
4. 前两项可归因后，再为 Micro-MAMA 建共享资源统计，或为 Drishti 补 base reuse policy；
   每项单独建立 ExecPlan。
5. Multi-Stream Squash Reuse 先做严格功能子集；RICH 先完成高延迟内存配置校准。两项
   都不应与 P0 feature 共用一次大改动。

## 统一验证口径

每个 feature 保持旧行为为默认值，并报告：

- **性能：** IPC/simTicks、ROI instruction、demand miss latency、frontend/rename stall；
  多核额外有 weighted speedup 与 fairness。
- **资源：** prefetch queue、MSHR、memory queue/bandwidth、free-list occupancy、ROB/IQ
  progress，按机制取用。
- **因果统计：** issue/useful/late/unused/evict，或 early-release/cancellation，或
  reuse/reject reason。只有 IPC 没有原因统计，不接受为 feature 结论。
- **趋势与正确性：** 扩大 table/PRF/带宽必须有可解释趋势；rename/squash feature 先过
  定向异常/分支测试和 difftest，再跑 checkpoint。
- **成本：** 报告 entry 数、字段位宽、队列/端口数量和热路径复杂度。以后取得全文再补
  synthesis/energy 数据，且与 GEM5 模型成本分栏。

## 参考论文与目录

| 机制 | 论文 | 作者（MICRO 58 官方日程） | ACM DOI |
| --- | --- | --- | --- |
| Kairos | *Elevating Temporal Prefetching Through Instruction Correlation* | Shuiyi He, Zicong Wang, Xuan Tang, Hao Tang, Dezun Dong, Liquan Xiao | [10.1145/3725843.3756133](https://doi.org/10.1145/3725843.3756133) |
| ATR | *ATR: Out-of-Order Register Release Exploiting Atomic Regions* | Yinyuan Zhao, Surim Oh, Mingsheng Xu, Heiner Litz | [10.1145/3725843.3756135](https://doi.org/10.1145/3725843.3756135) |
| Micro-MAMA | *Micro-MAMA: Multi-Agent Reinforcement Learning for Multicore Prefetching* | Charles Block, Gerasimos Gerogiannis, Josep Torrellas | [10.1145/3725843.3756096](https://doi.org/10.1145/3725843.3756096) |
| Multi-Stream Squash Reuse | *Multi-Stream Squash Reuse for Control-Independent Processors* | Qingxuan Kang, Trevor E. Carlson | [10.1145/3725843.3762248](https://doi.org/10.1145/3725843.3762248) |
| Drishti | *Drishti: Do Not Forget Slicing While Designing Last-Level Cache Replacement Policies for Many-Core Systems* | Sweta, Prerna Priyadarshini, Biswabandan Panda | [10.1145/3725843.3756028](https://doi.org/10.1145/3725843.3756028) |
| RICH | *RICH Prefetcher: Storing Rich Information in Memory to Trade Capacity and Bandwidth for Latency Hiding* | Ningzhi Ai, Wenjian He, Hu He, Jing Xia, Heng Liao, Guowei Zhang | [10.1145/3725843.3756081](https://doi.org/10.1145/3725843.3756081) |
| TRRIP (排除) | *A TRRIP Down Memory Lane: Temperature-Based Re-Reference Interval Prediction For Instruction Caching* | Henry Kao et al. | [10.1145/3725843.3756110](https://doi.org/10.1145/3725843.3756110) |
| LVM (排除) | *Learning to Walk: Architecting Learned Virtual Memory Translation* | Kaiyang Zhao et al. | [10.1145/3725843.3756093](https://doi.org/10.1145/3725843.3756093) |
| LoopFrog (排除) | *LoopFrog: In-Core Hint-Based Loop Parallelization* | Marton Erdos et al. | [10.1145/3725843.3756051](https://doi.org/10.1145/3725843.3756051) |
| Ghost Threading (排除) | *Ghost Threading: Helper-Thread Prefetching for Real Systems* | Yuxin Guo et al. | [10.1145/3725843.3756106](https://doi.org/10.1145/3725843.3756106) |

其他来源：

- [MICRO 58 official program](https://microarch.org/micro58/program/)
- [Crossref proceedings metadata](https://api.crossref.org/works/10.1145/3725843)
- 当前模型参考：[ISCA 2026 hardware feature survey](isca2026_hardware_feature_survey.md)、
  [HPCA 2026 hardware feature survey](../../design-docs/hpca26-gem5-hardware-feature-survey.md)

**证据边界：** 机制和定量结论来自可访问的 indexed abstracts；ACM 全文在调研时被
Cloudflare 拦截。未披露的绝对硬件开销保持为空，所有当前 GEM5 影响均为工程假设和
验证计划，而非已测得结果。
