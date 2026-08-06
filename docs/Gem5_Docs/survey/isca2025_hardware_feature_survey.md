# ISCA 2025: 面向当前 KMHv3/GEM5 的纯硬件 Feature 调研

> 调研日期：2026-08-05。本文针对本仓库当前 `configs/example/kmhv3.py` 的
> XiangShan/KMHv3 O3 CPU 和 classic cache 路径，而不是对论文原始平台或任何 CPU 的
> 横向排名。本文新建于 ISCA 2025 proceedings；同目录的 ISCA 2026 文档仅是格式参考，
> 不能作为本文的论文证据。

## 结论

ISCA'25 的 135 篇论文中，能够严格满足“不改编译器、二进制、ISA、OS 或运行时，硬件本身就
可以产生效果”，又与当前 CPU/cache 模型有直接联系的条目并不多。推荐的实施顺序不是按论文
最高加速比排序，而是按当前 GEM5 能否保留完整的性能因果链、硬件开销和建模/验证风险排序：

1. **P0: PACIPV（轻量、prefetch-aware LLC replacement）**。最适合先做。它把
   demand/prefetch 的 insertion/promotion 映射到 RRPV 向量；当前 L2 已有
   `XSDRRIPRP`、`originBit`、prefetch provenance 和 RRPV，因此第一版可以不增加每条
   cache line 的动态状态。论文在 instruction-heavy 工作负载上相对 LRU +3.3%，相对
   SRRIP +1.1%，相对 Mockingjay +0.1%。
2. **P1: Practical Ahead Prediction（低能耗提前方向预测）**。论文以受控的历史候选集让
   TAGE 在真正需要预测之前开始查询，报告 +4.4% 性能，预测器能耗为原预测器的 1.5x，而非
   全枚举缺失历史时的 14.6x。当前 decoupled BPU 已有延迟化 TAGE stage，是可研究的前端
   项；但它必须完整建模候选 history、查询端口、结果到达和 squash，不能在模型中把预测变成
   零延迟。
3. **P2: Garibaldi（instruction-data pairwise LLC management）**。纯硬件、潜在收益高，
   但论文假定 40-core/30 MB shared LLC 和 server workload，而当前默认是每核 2 MB L2。
   它适合在有 shared LLC 的多核配置中单立项目。论文给出 193.9 KB（40 cores）辅助状态，
   加 1 bit/line 后为 30 MB LLC 的 0.8%，并报告相对 LRU +13.2%、相对 Mockingjay +6.1%。
4. **P2（特定场景）: CHESS/SBP（利用请求间控制流相似性的 cold-start BPU）**。仅当 GEM5
   有硬件可见的 microservice request 边界和重复请求流时才值得做。它不需要改应用或 ISA，但
   当前 SPEC checkpoint 不会暴露论文的冷启动机会。论文报告额外 18.1 KB，cold BPU 的 branch
   MPKI -94%，相对 state-of-the-art predictor -78%，平均恢复到 warm baseline 的 95%。

**不建议把下列看似相关的论文直接立项：** Prophet 和 Magellan 的主要收益分别需要
profile/hint binary 与编译器插入的软件预取；A4 依赖 Intel DCA 和 network/storage I/O 运行时
管理；XOR Cache 主要换取面积/功耗而非提高固定容量当前 CPU 性能；CORD、HardHarvest、DREAM
是需要 coherence/VM/DRFM 平台模型的系统项目。它们的论文数字不应被误用为当前单核/少核
KMHv3 的 IPC 预期。

所有百分比都是**论文在自身基线、配置和工作负载下的结果**，不是本仓库的预期性能。当前 GEM5
是否获益只能由相同 checkpoint、相同 warmup/ROI、reset 后 final stats 的 feature-off/on A/B 得出。

## 范围、证据与筛选方法

### 硬件-only 的严格定义

主候选必须同时满足：

- 不依赖编译器改写、profile-guided binary、软件 hint、OS allocator/scheduler、ISA 扩展、
  未来访问 oracle 或人工离线 trace；
- 利益来自有界的 CPU、BPU、cache、TLB 或 memory-controller 状态/控制动作，而非用无限带宽、
  无限端口或“预测总是正确”的函数模拟；
- 能说明状态位、表项/队列、访问端口、更新/恢复时机以及导致的 cache miss、front-end stall、
  误预测或带宽竞争；
- 对当前单核/少核 KMHv3 的 core/cache 路径有合理的落点。需要新的 multi-PU coherence、
  device/VM、DRAM command 或 accelerator 的工作只列为延后项。

### 论文目录与访问边界

- 官方 proceedings: [ACM DOI 10.1145/3695053](https://dl.acm.org/doi/proceedings/10.1145/3695053)，
  即 *Proceedings of the 52nd Annual International Symposium on Computer Architecture*，
  ISCA'25，Tokyo，2025-06-20。
- 目录完整性由 [DBLP ISCA 2025](https://dblp.org/db/conf/isca/isca2025.html) 和
  [Crossref proceedings record](https://api.crossref.org/works/10.1145/3695053) 交叉核对：共有
  135 篇 proceedings articles。本报告逐题名初筛后，阅读相关论文的公开摘要；Garibaldi、
  Prophet 可取得作者公开版全文。
- ACM 的自动 PDF 请求在调研环境被 Cloudflare challenge 拦截；用户提供的
  `172.38.8.77:7897` HTTP proxy 也获得同一 403 challenge。因此 PACIPV、Ahead 和 CHESS
  的精确数字只转述可核实的 ACM/语义索引摘要；没有把未取得全文的面积/bit 数猜成论文事实。
- “当前树映射”来自本 checkout 的源码检查。源码优先于旧文档，关键入口为
  `configs/example/kmhv3.py`、`src/cpu/pred/btb/`、`src/cpu/o3/fetch.*`、
  `src/mem/cache/`、`src/mem/request.hh` 和
  `src/mem/cache/replacement_policies/xs_drrip_rp.*`。

### 建模口径

每个候选必须保留如下链路，而不是只在 GEM5 中改变一个统计计数器：

```text
workload event -> 有界硬件状态/端口 -> cache 或 BPU 的竞争/恢复
               -> miss、stall、flush、traffic 的变化 -> ROI stats
```

对每个模型，关键的 request、FTQ、cache tag、MSHR、queue 和 lookup port 需要细建；未决定
外部阻塞或完成的内部组合逻辑可以压成参数化 `latency`、`width`、`threshold` 或有限表项。
热路径只能做固定上限的 table lookup、bitset/有限候选枚举或 queue dequeue，不能每周期扫描 ROB、
所有 history 或全部 cache set。

## 决策总表

| 优先级 | 论文/feature | 论文结果与硬件成本 | 当前 GEM5 落点 | 结论 |
| --- | --- | --- | --- | --- |
| P0 | PACIPV | LRU +3.3%，SRRIP +1.1%，Mockingjay +0.1%；论文摘要称显著低于 Mockingjay 的硬件开销，但未给出可访问的绝对 bit 数 | `xs_drrip_rp.*`、`ReplacementPolicies.py`、`kmhv3.py` | 先做，新增动态 tag 状态可为 0 |
| P1 | Practical Ahead Prediction | +4.4%；能耗 1.5x，朴素缺失-history 全枚举为 14.6x；公开摘要未给绝对 SRAM 大小 | `btb_tage.*`、`decoupled_bpred.*`、`history_manager.*` | 有价值，但有 BPU 恢复/时序风险 |
| P2 | Garibaldi | LRU +13.2%，Mockingjay +6.1%；193.9 KB/40 cores，另加 1 bit/cache line 时为 30 MB LLC 的 0.8% | shared L3/LLC cache path、request PC metadata、replacement policy | server/多核 shared LLC 专题，不先嫁接到每核 L2 |
| P2 条件项 | CHESS/SBP | cold predictor MPKI -94%，相对 SOTA -78%，达到 warm baseline 95%；18.1 KB | `DecoupledBPUWithBTB` 新 side predictor + request epoch input | 仅有硬件可见 service request 边界时开展 |
| P3/不立项 | XOR Cache | 节省 LLC area 1.93x、power 1.92x；相对更大未压缩 cache 有 2.06% performance overhead，EDP -26.3% | 新 cache data layout/compression controller | 是面积/能耗优化，不是当前固定容量 CPU 加速 |
| 排除 | Prophet | 相对 Triangel +14.23%，但必须 profile、分析、插入 hints 到优化 binary；multi-path victim buffer 0.19 KB | temporal prefetcher | 不满足 hardware-only |
| 排除 | Magellan | 平均 1.14x、最高 1.41x；论文是 compiler/loop-guided software prefetcher | software/编译器 prefetch | 不满足 hardware-only |
| 延后 | CORD / HardHarvest / DREAM / A4 | 分别是 multi-PU coherence、VM core harvest、DRFM Rowhammer、Intel DCA I/O 管理 | Ruby/device/VM/DRAM protocol | 平台前提不在当前 CPU/cache 路径 |

`P0/P1/P2` 表示风险调整后的启动顺序，不是不同论文 IPC 的横向比较。PACIPV 的论文收益较小，
但它能以最小硬件增量验证当前 L2 中 demand/prefetch 插入与 promotion 的因果链；Garibaldi 的论文
收益最大，但它的缓存层级和 workload 与当前默认配置差异也最大。

## 主候选

### P0: PACIPV - prefetch-aware coarse-grained insertion/promotion vectors

论文：[Light-weight Cache Replacement for Instruction Heavy Workloads](https://doi.org/10.1145/3695053.3730993)。

**论文机制。** PACIPV（Prefetch Aware Coarse-grained Insertion and Promotion Vectors）将 RRIP
的一次固定 insertion/promotion 规则推广为少量向量：根据 demand/prefetch 和访问阶段选择一个
RRPV 值。其核心不是更大的 PC/history predictor，而是让 aggressive prefetch 下的 insertion 和
promotion 不再一刀切，从而避免对 instruction-heavy workload 既过早驱逐 demand line、又过度保留
低价值 prefetch line。

**论文结果和成本。** 摘要报告相对 LRU +3.3%，相对 SRRIP +1.1%，相对硬件显著更重的
Mockingjay +0.1%。论文摘要只称其为 low/minimal-overhead，没有公开绝对 bit/area 数；在 ACM
全文不可自动取得的前提下，这里不杜撰该数字。对于本树的工程估算，若只选择 vector 而不做在线
search，则复用每 line 已存在的 RRPV 与 `originBit`：**新增每 line 动态状态为 0 bit**，仅需一个
固定小策略表/参数和 access-class decode。这个估算不是论文面积声明；若未来实现可综合，仍需将
比较器、RRPV 写端口和 policy ROM 一并计入。

**当前树匹配。** `kmhv3.py` 对 classic 2 MB L2 配置
`XSDRRIPRP(mode=2, num_sets=4096)`；切片 L2 同样配置该 policy。现有
`XSDRRIP::getRRPV()` 已区分 refill/reuse、release、`pkt->req->isPrefetch()`，`touch()` 在 hit 时
提升 RRPV，`reset()` 在 refill 时插入。它正是 PACIPV 的自然落点。需要注意，当前实现把普通
non-prefetch refill 置为 RRPV=0、prefetch refill 置为 RRPV=1；直接替换这些语义会改变当前
XiangShan 对齐策略，所以 feature 默认必须关闭，且保持原路径作为 baseline。

**最小建模合同。**

- 因果链：demand/prefetch refill 或 hit -> PACIPV vector 选 RRPV -> set 内 line lifetime/victim
  改变 -> demand miss、MSHR/DRAM 竞争与 IPC 改变。
- 必细建：request class、refill/hit 的 vector lookup、已有 8-way victim aging、prefetch 与 demand
  对相同 tag/data port 和 MSHR 的竞争。
- 可粗建：vector 可实现为常量数组或 `VectorParam`，每次访问 O(1)；不在热路径做训练集搜索或
  全历史扫描。论文的离线 exhaustive policy search 只能用来选默认参数，不能作为运行时 oracle。
- 参数：`enable_pacipv`（默认 false）、demand/prefetch 的 insertion vector、demand/prefetch 的
  promotion vector、可选 instruction/data class；默认 vector 精确还原当前 `XSDRRIPRP` 行为。
- stats：按 access class 记录 insertion/promotion、RRPV 分布、victim class、prefetch-induced
  demand eviction、demand/prefetch MPKI、prefetch useful/late/unused、MSHR full、DRAM traffic。

**实施和验证。** 先做不区分 instruction/data 的 demand-vs-prefetch PACIPV；它能确保和当前
`originBit`/prefetch 流程对齐。实验至少有 baseline、只改 insertion、完整 insertion+promotion 三组。
只有完整组更好且 RRPV/victim/prefetch-usefulness 统计能解释差异时，才可归因于 PACIPV；若只有
baseline IPC 改动而 miss/eviction 分布不变，则通常是模型接线或资源建模错误。该 feature 是最适合
先用现有 SPEC checkpoint 做 A/B 的条目。

### P1: Enabling Ahead Prediction with Practical Energy Constraints

论文：[Enabling Ahead Prediction with Practical Energy Constraints](https://doi.org/10.1145/3695053.3730998)。

**论文机制。** 多周期 TAGE 在所需 branch 到来前发起查询可以隐藏预测延迟，但在预测发起到实际
使用之间，N 个中间 branch 的方向未知。朴素做法需为全部 `2^N` history 补全模式并行查询，论文
估计能耗增至 14.6x。该工作观察到实际 materialize 的缺失 history pattern 通常仅一两个，因此修改
TAGE，以有限的已观测 pattern 避免为永远不会出现的中间方向做查询。它是纯预测器控制/存储机制，
不需要程序、ISA 或 OS 的支持。

**论文结果和成本。** 论文摘要报告 +4.4% performance，能耗为原预测器的 1.5x；对比朴素 14.6x
的全枚举。公开摘要没有给出 table entries、area 或绝对 KB，故本报告把存储绝对值记为“待阅读全文
核实”，不把 1.5x energy 误写成 1.5x core energy。硬件实现的实际代价包括候选 pattern filter/record、
并行或时间复用的 TAGE lookup port、in-flight lookup metadata 和丢弃无效候选的控制；不能只加一个
零延迟 `predict()` 调用。

**当前树匹配与缺口。** KMHv3 固定使用 `DecoupledBPUWithBTB`。其包含 `ubtb`、`abtb`、
`microtage`、`mbtb`、`tage`、`ittage`、`mgsc` 等分级组件；`BTBTAGE.numDelay=2`、
`BTBMGSC.numDelay=2`，`numStages=4`。这已经体现慢预测组件的可见 latency。`Fetch` 从
FTQ/FSQ 消费 prediction；`DecoupledBPUWithBTB` 维护 speculative history，并在 squash 时调用
history recovery。

这里的 `AheadBTB` 是当前 BPU 中的一个 BTB 组件，**不等于本论文的 Ahead TAGE direction
prediction**。论文原型不应直接改名覆盖它。建议增加独立的 `AheadBTBTAGE` wrapper/side component，
由它提前向现有 `BTBTAGE` 发起有界查询，或首先在常规 TAGE 配置上复现实验；两者的 block-based
direction/target 语义不完全相同，不能宣称已逐行复现论文。

**最小建模合同。**

- 因果链：预测流中未来 branch 的 PC + 不完整 history -> finite candidate set -> ahead lookup
  port/latency -> 当 history materialize 时命中候选并及时供应 direction -> fetch bubble/FTQ 断供或
  misprediction recovery 改变。
- 必细建：ahead distance、每 cycle lookup quota、in-flight candidate 数、lookup ready tick、
  candidate match/drop、预测 metadata 随 FTQ/FSQ 传播、squash 后的精确取消/recovery。
- 可粗建：TAGE 的内部 hash/folded-history 门级逻辑仍由既有 table 表达；候选集上限由参数控制，
  可用固定数组/bitset，不能在每周期枚举所有 `2^N` history。
- 参数：`enable_ahead_tage`（默认 false）、`ahead_distance`、`max_candidates`、pattern-table
  entries/associativity、lookup ports、lookup latency、energy-per-lookup accounting。默认关闭时必须
  bit-for-bit 保持原 BPU 时序。
- stats：ahead started/candidate count/matched/useful/late/dropped/squashed、TAGE port busy、
  FTQ/FSQ empty、direction MPKI、frontend bubble cycles、mispredict recovery cycles、lookup-energy
  proxy。只报 final MPKI 而没有 timely/late/drop 不足以证明机制起效。

**实施和验证。** 先实现 `looked-up but not consumed` 以验证候选覆盖，再允许其覆盖 baseline
slow prediction。必须做 `ahead_distance=0`（退化为 baseline）、不同 `max_candidates`、不同
lookup latency/port 的趋势测试；candidate 变多应减少 unrepresented history 但会增加 lookup/energy，
而不是无条件变快。由于当前 BPU 是 BTB block predictor，首轮只评估相对当前 BPU 的行为收益，不能
直接把论文的通用 TAGE +4.4% 当作 KMHv3 预期。

### P2: Garibaldi - instruction-data pairwise LLC management

论文：[Garibaldi: A Pairwise Instruction-Data Management for Enhancing Shared Last-Level Cache Performance in Server Workloads](https://doi.org/10.1145/3695053.3731029)
([作者公开版](https://arxiv.org/abs/2505.18554))。

**论文机制。** Garibaldi 观察到 server workload 的 instruction miss 往往先于一串 data access，
而 data-oriented LLC 管理会牺牲此类“高 miss-cost” instruction line。它以 physical instruction line
为 key 维护 pair table，记录相关 data line 和可老化的 miss cost；高代价 instruction line 得到选择性
保护。当未保护 instruction line miss 时，它还能对配对 data line 保守预取。该机制完全在硬件
cache/controller 内完成，不要求程序 hint。

**论文成本。** 对 40-core、30 MB LLC，论文默认 `k=1` 的 state 为：16,384-entry main pair table
120 KB（34-bit control plus data-line field）、8,192-entry D_PPN table 32 KB、每核 1 KB helper table，
合计 193.9 KB；加入 1 bit/LLC line 的 instruction indicator 后为 LLC 容量的 0.8%。论文用
CACTI 7（22 nm）给 pair-table lookup 0.331 ns，并在 simulator 以 1 cycle lookup、每次 eviction
最多 2 次 lookup 建模。这是比 PACIPV 大两个数量级的 metadata/port 项，不能视为免费。

**论文效果。** 摘要报告相对 LRU +13.2%、相对 Mockingjay +6.1%。公开版的 server workload
敏感性显示收益并不均匀，文中存在从 -18% 到 +65% 的单 workload 差异；instruction victim 少或
data 也很冷时，保护 instruction 会损害 data caching。该退化风险是模型必须保留并报告的结果，
不能只挑选有利 workload。

**当前树匹配与范围。** 当前默认是每核 2 MB L2（classic 路径配 `XSDRRIPRP`），不是论文的
40-core shared 30 MB LLC。因此直接把 194 KB state 塞入 per-core L2 后再引用论文 +13.2% 是不成立
的。适合的 GEM5 路线是先明确 shared L3/LLC 的多核配置，再把 pair table 放在 shared cache
controller；`Request::isInstFetch()` 可区分 I/D，data request 已有 `hasPC()/getPC()`，cache path 也
已有 prefetch provenance 和 MSHR/eviction hooks。需要新增的是 instruction-PC 到 physical instruction
line 的 helper mapping、每 line instruction provenance、pair-table sideband 和 lookup arbitration。

**最小建模合同。**

- 因果链：LLC I-miss/与其关联的 data access -> pair-table miss cost/associated data -> selective
  instruction protection 或 one-hop paired-data prefetch -> LLC victim/data miss/traffic -> I-fetch stall
  与 weighted speedup。
- 必细建：shared table 容量/相联度、1-cycle lookup、每 cycle lookup port、eviction 的 max attempts、
  instruction indicator、threshold aging、paired prefetch 的 queue/MSHR admission 与 drop。处理
  instruction protection 和 data pollution 的 tradeoff 是核心，不可粗化。
- 可粗建：D_PPN compression 可以先用一个明确 bit budget 的 fixed record；初版不必逐地址重现
  paper 的所有 page-level 压缩细节。pair table 只在 request/refill/eviction event 查一次，禁止扫描
  全部 table 或 ROB。
- 参数：pair/helper/D_PPN table entries、`paired_data_lines`、lookup latency/ports、max eviction
  attempts、aging period、protect threshold、prefetch degree、feature enable；默认关闭。
- stats：pair lookup hit/miss/evict、protected/evicted instruction lines、threshold movement、paired
  prefetch issued/useful/late/unused/drop、I/D LLC MPKI、I-fetch stall cycles、data MPKI、MSHR/DRAM
  traffic、per-workload slowdowns。

**实施和验证。** 分三个层次：`protection-only`（不发 paired prefetch）、`pair learned but not
issued`、full Garibaldi。先验证大表/低阈值会增加 protected lines，lookup ports/latency 增大时会出现
可解释的竞争，再跑 server-style workload；在当前 SPEC slice 上若 I-side footprint 没有进入 LLC，
收益很小是正确而非失败。该项目应有独立 ExecPlan，不与 PACIPV 混在同一个 replacement-policy
patch 中。

### P2 条件项: CHESS/SBP - request-similarity branch prediction

论文：[Leveraging control-flow similarity to reduce branch predictor cold effects in microservices](https://doi.org/10.1145/3695053.3731059)。

**论文机制。** SBP 以过去 request 的参考控制流 trace 增强传统 history predictor，对传统方法难以
预测的 branch 在相似 request 中复用方向和 target。CHESS 是具体实现，组合 conventional
history-based fetch predictor、static-hint predictor 和 similarity predictor。其目标是服务交错执行、
core power-gating 或 request 间 predictor 被冷却后的 cold effect，不是稳态 SPEC 的普通 branch
predictor。

**论文结果和成本。** 摘要报告 18.1 KB 额外状态；相对 cold fetch predictor branch MPKI -94%，
相对 state-of-the-art predictor -78%，并让平均性能达到 warm baseline 的 95%。它恢复的是 cold
loss，并非承诺比 warm 当前 BPU 再快 95%。公开摘要未给出独立 area/energy 和 trace port 数，
实现评估必须补充这些项目。

**严格硬件边界与当前风险。** 论文机制本身把 reference trace、相似度选择和预测放在硬件中，
不需要修改应用 binary；但它需要硬件知道一个 request 的开始/结束或等价 epoch。真实服务器可由
NIC receive/doorbell、VM dispatch 或硬件 work-queue boundary 提供，而当前 KMHv3 SPEC checkpoint
没有该输入。没有边界却用 host script 人工切段，实质上又引入了离线 oracle，不满足本调研的标准。
故将其列为“硬件-only 但当前 workload 条件不满足”的 P2 项，而不是默认 BPU P0。

**当前树与建模合同。** `DecoupledBPUWithBTB` 已有分级 `BTBTAGE/MicroTAGE/MGSC` 和 FTQ
metadata/recovery。实现应增加 `SimilarityPredictor` side component，而不是改写全部 predictor：

- 事件链：hardware request-boundary -> committed reference-trace selection -> hard-branch lookup ->
  direction/target override -> FTQ supply 或 control squash -> cold MPKI/tail latency。
- reference trace 必须是固定容量且只由 resolved/committed control flow 更新；wrong-path 不能污染
  下一 request。每次 fetch 只做 O(1) 或小常数 window lookup。
- 参数：reference trace bytes、hard-branch table entries、match window、request epoch source、
  override threshold、lookup latency/port；默认完全关闭。
- stats：request count、reference hit/miss/evict、similarity override/correct/wrong、cold/warm
  conditional MPKI、FTQ supply gaps、tail latency。没有 request epoch coverage 时禁止报告 CHESS IPC。

验证必须使用带 network/request boundary 的 microservice workload；单纯用 SPEC checkpoint 的分支
MPKI 只能验证“关闭后不回归”，不能验证论文的主张。

## 相关但不进入 strict shortlist 的论文

| 论文 | 论文机制、成本和效果 | 排除/延后原因 |
| --- | --- | --- |
| [Profile-Guided Temporal Prefetching (Prophet)](https://doi.org/10.1145/3695053.3731070) | 使用 PMU profile、离线 analysis，向优化 binary 注入 metadata-table hints；相对 Triangel +14.23%，multi-path victim buffer 0.19 KB | 论文明确是 hardware-software co-design，收益依赖 profile 和 binary/hint，违反 strict hardware-only |
| [Magellan](https://doi.org/10.1145/3695053.3731054) | compiler 从 loop dependence graph 提取 IMA prefetch；14 个 benchmark 平均 1.14x、最高 1.41x，cache miss -25%、dynamic instructions -14% | 是 loop-guided **software prefetcher**，没有编译器/插入 prefetch 指令就不是论文机制 |
| [A4](https://doi.org/10.1145/3695053.3731114) | 用 Intel DCA ways、inclusive-way migration 和 runtime LLC management 缓解 network/storage I/O 争用；latency-sensitive workload +51% | 特定 Xeon DCA、DMA 和 I/O runtime 环境；当前 KMHv3 classic cache 无相应 device/cache protocol |
| [The XOR Cache](https://doi.org/10.1145/3695053.3730995) | 以 line pair XOR/inter-line compression 将 LLC area/power 分别省 1.93x/1.92x；相对更大未压缩 cache performance -2.06%，EDP -26.3% | 纯硬件但主要是面积/能耗优化；固定容量当前 L2 的 CPU 性能不应预设增加，且需新 data layout、reconstruct port 与 coherence contract |
| [CORD](https://doi.org/10.1145/3695053.3731074) | directory ordering 支持 write-through release consistency；多 PU 上 +24%、traffic -13%、storage/area/power <1% | 很有价值的纯硬件 coherence 协议，但需要 CPU/GPU 多 PU、directory/Ruby/write-through 路径，超出当前 core/cache scope |
| [HardHarvest](https://doi.org/10.1145/3695053.3731071) | VM 之间的硬件 core harvesting、TLB/cache partition；core utilization 1.5x、Harvest throughput 1.8x、Primary tail latency 6.0x | 需要 VM/primary-harvest request scheduler 和 multi-core partitioning；不是当前单核 IPC feature |
| [DREAM](https://doi.org/10.1145/3695053.3731117) | DRFM-aware Rowhammer mitigation；DREAM-R 将 PARA overhead 从 12.7% 降至 4.24%，DREAM-C 1 KB/bank、比 Graphene 小 8x | 只在已建模 DRFM/Rowhammer 防护下减少安全开销；当前 DRAMsim3/default DDR4 路径没有该协议，不能直接声称 CPU 加速 |

## 实施路线与统一验证

### Phase 0: 固定基线

1. 固定 commit、`kmhv3.py` 参数、core count、memory ini、checkpoint、warmup 和 ROI；checkpoint
   slice 不加 `--raw-cpt`。
2. 对 baseline 先保存 reset 后的最终 `stats.txt`，不把 warmup 和 ROI 计数混合。
3. 每个 feature 都提供 feature-off、子机制-off、feature-on 配置，默认关闭时应保持现有行为。
4. 先定位机会：PACIPV 看 L2 demand/prefetch eviction；Ahead 看 FTQ supply 和 conditional MPKI；
   Garibaldi 看 LLC I-side MPKI/I-fetch stall；CHESS 看 cold request boundary 后 MPKI。

### Phase 1: 两个可归因 P0/P1 A/B

- **PACIPV：** 保持现有 `XSDRRIPRP` 的 data structure，只引入参数化 vector lookup 与 reason
  stats。先固定 policy，不在运行中做离线/全局搜索。先跑同一组 checkpoint 的
  `baseline / insertion-only / full`。
- **Ahead Prediction：** 先建候选队列、ready tick 和 squash cleanup，保留 baseline direction；在
  coverage/late/drop 足够清楚后，再允许 ahead result 覆盖 slow TAGE output。首轮不可把 lookup
  latency 设为零。

### Phase 2: 需要系统边界的项目

- **Garibaldi：** 先决定 shared LLC 的配置和请求 PC/physical instruction line metadata contract，
  再做 protection-only 和 pairwise prefetch。将 table 存储、lookup port、traffic 和退化 workload
  作为 acceptance gate。
- **CHESS：** 先把 request epoch 建模为硬件 event，并证明它没有由 host/offline oracle 填充；只有
  microservice request workload 可用时才推进 similarity prediction。

### 必报的 stats 和判定门槛

每项至少报告：

1. **性能：** ROI committed instructions、ticks/IPC、每 benchmark 的结果和退化项；多 workload
   报加权/几何平均的定义。
2. **机制覆盖：** vector class、ahead match、pair hit、CHESS override 等，证明 feature 真被使用。
3. **副作用：** prefetch useful/late/unused、victim class、MSHR/queue full、BPU port busy、traffic、
   squash/recovery。缺失这些 stats 的 IPC 变化不可归因。
4. **趋势/消融：** 增大 table/queue/port 通常应减少 full/drop，增加 latency 应增加等待，修改阈值
   应有保护/污染 tradeoff。没有可解释趋势不能视为可信性能模型。

构建和 checkpoint 运行入口保持仓库约定：

```bash
scons build/RISCV/gem5.opt --gold-linker -j64
GCBV_REF_SO=/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-interpreter-so \
  ./build/RISCV/gem5.opt ./configs/example/kmhv3.py \
  --generic-rv-cpt=<checkpoint-slice.zstd>
```

本文是调研而不是实现结果：没有宣称上述任一 feature 已在当前 GEM5 编译、运行，或已复现论文性能。

## 参考论文和来源

| 机制 | 论文 | 证据源 |
| --- | --- | --- |
| PACIPV | *Light-weight Cache Replacement for Instruction Heavy Workloads* | [ACM DOI](https://doi.org/10.1145/3695053.3730993)，公开摘要 |
| Ahead Prediction | *Enabling Ahead Prediction with Practical Energy Constraints* | [ACM DOI](https://doi.org/10.1145/3695053.3730998)，公开摘要 |
| Garibaldi | *Garibaldi: A Pairwise Instruction-Data Management for Enhancing Shared Last-Level Cache Performance in Server Workloads* | [ACM DOI](https://doi.org/10.1145/3695053.3731029)，[arXiv 2505.18554](https://arxiv.org/abs/2505.18554) 全文 |
| CHESS/SBP | *Leveraging control-flow similarity to reduce branch predictor cold effects in microservices* | [ACM DOI](https://doi.org/10.1145/3695053.3731059)，公开摘要 |
| Prophet | *Profile-Guided Temporal Prefetching* | [ACM DOI](https://doi.org/10.1145/3695053.3731070)，[arXiv 2506.15985](https://arxiv.org/abs/2506.15985) 全文 |
| Magellan | *Magellan: A High-Performance Loop-Guided Prefetcher for Indirect Memory Access* | [ACM DOI](https://doi.org/10.1145/3695053.3731054)，公开摘要 |
| XOR Cache | *The XOR Cache: A Catalyst for Compression* | [ACM DOI](https://doi.org/10.1145/3695053.3730995)，公开摘要 |
| CORD | *CORD: Low-Latency, Bandwidth-Efficient and Scalable Release Consistency via Directory Ordering* | [ACM DOI](https://doi.org/10.1145/3695053.3731074)，公开摘要 |
| HardHarvest | *HardHarvest: Hardware-Supported Core Harvesting for Microservices* | [ACM DOI](https://doi.org/10.1145/3695053.3731071)，公开摘要 |
| DREAM | *DREAM: Enabling Low-Overhead Rowhammer Mitigation via Directed Refresh Management* | [ACM DOI](https://doi.org/10.1145/3695053.3731117)，公开摘要 |

**证据边界。** 没有标成“论文结果”的开销和落点是针对当前 GEM5 的工程判断；没有标成
“当前 GEM5 A/B”的数字全部不能解读为本仓库性能承诺。PACIPV、Ahead 和 CHESS 的 ACM 全文
因访问限制未自动取得，故绝对 storage/area 缺口保留在文中；在获得原文前，实施时应先核查其
table/port 定义，再固定默认参数和实际 RTL 面积预算。
