# HPCA 2026 面向 Kunminghu v3 GEM5 的硬件特性筛选

**日期：** 2026-08-05

**目标：** 从 HPCA 2026 中筛选不改软件即可在当前 Kunminghu v3 GEM5 CPU 上做 A/B 实验的微结构特性，并按预期性能收益、实现可行性和硬件开销排序。

## HPCA 2026 概览和筛选口径

从官方日程可见，HPCA 2026 的工作覆盖处理器前端/分支、寄存器和后端、缓存与预取、主存、可靠性/间歇计算、coherence，以及加速器和系统软件等方向。本报告只保留能在 CPU 数据通路中由硬件独立启用、并且有机会改变吞吐或可见延迟的机制：分支预测、rename/PReg、预取、cache/memory controller。由于共享文件夹不可访问，下面是基于公开日程和摘要能够核对的相关论文清单，不声称覆盖受保护目录中的其他全文材料。

筛选条件是：

- 不改编译器、OS、运行时、ISA 或应用输入；硬件模型可通过参数和开关启用。
- 机制必须落在当前 GEM5 已有或可明确扩展的队列、表、端口、bank、带宽或恢复状态上。
- 论文实测效果、论文硬件成本、当前 GEM5 工程预期分栏记录；没有绝对成本或 IPC 的摘要不补猜。
- 能源收集断电、特定 TSO/write-through coherence、专用 accelerator 或纯软件调度只有在当前 CPU 环境匹配时才纳入；否则列入排除项。

## 结论先行

建议按下面的顺序推进：

1. **P0 - I-POP：预取器效用（positive-PE）管理器。** 当前 L2 已经有多个可独立开关的预取器和按来源统计，接口匹配度最高，新增状态很小，适合先做行为级原型。
2. **P1 - Streamline：精简的片上 temporal prefetching。** 论文的 coverage/metadata 结果有吸引力，但需要新增 correlation metadata 和公平的容量对比。
3. **P1 - Tempranillo：非投机早期物理寄存器释放。** 直接针对当前 O3 rename 的物理寄存器压力；但异常、squash 和消费者 use-count 是功能正确性的主要风险。
4. **P2 - LLBP-X：动态上下文深度的 last-level branch predictor。** 方向可能有价值，但论文的 LLBP 与当前 block-based `BTBTAGE` 不是同一个预测层级，映射风险高。
5. **P2（有条件）- BARD：DDR5 dirty-victim cleansing。** 只有先把实验内存切到 DDR5 并实现写 bank 并行模型后才有意义；当前默认是 DRAMsim3 DDR4。
6. **暂缓 - Athena：RL 协同预取和 off-chip predictor。** 需要当前 GEM5 不存在的 off-chip predictor 和较大的控制/训练面，摘要也没有给出足够的成本与效果细节。

这些排序不是对论文贡献的排名，而是“在当前 GEM5 上做出可信硬件-only A/B 结果”的排序。论文中的准确率、coverage、百分点或多核结果不应直接当作当前单核 GEM5 的 IPC 预测。

### 论文实测与 GEM5 工程预期

| 候选 | 当前 GEM5 的可检验预期（不是论文数字） |
|---|---|
| I-POP | 非正 PE 源被抑制后，预取 unused/pollution 和 L2 prefetch queue pressure 应下降；若当前 workload 的 demand miss 被预取带宽拖慢，IPC/simTicks 才会改善。收益大小必须由每源 PE、demand miss latency 和 ROI IPC 实测。 |
| Streamline | 在相同 metadata bit budget 下，correlation hit/coverage 和 LLC metadata traffic 应优于简单 temporal baseline；只有 coverage 转换为更少的 demand miss 或更短的 miss latency，才预期 CPU 性能提升。不能把论文 8-core 的 +6.7 个百分点外推到当前单核。 |
| Tempranillo | 若 `fullRegistersEvents`/rename block cycles 是瓶颈，提前释放应提高 free-list occupancy、减少 rename stall，并可能提高 IPC；若 ROB/LSQ/内存才是瓶颈，性能可能接近零。先证明 release 生命周期正确，再做归因。 |
| LLBP-X | 预期首先体现在 branch misprediction、FTQ redirect 和 fetch bubbles；只有前端恢复流量下降才会传导到 IPC。论文的平均 3.6% 是 LLBP accuracy 增量，不是 GEM5 IPC 目标。 |
| BARD | 仅在 DDR5 bank-group timing 和 dirty writeback 成为瓶颈时，预期降低 write-to-read turnaround 和读延迟；默认 DDR4 配置不构成有效验证。 |
| Athena | 在没有 OCP 和 RL 状态实现前没有可信的当前 GEM5 预期；应视为 I-POP 之后的策略扩展，而不是独立的第一批 feature。 |

## 资料范围和证据等级

用户提供了受保护的 [OneDrive HPCA26 文件夹](https://1drv.ms/f/c/2c39873ef9f8547e/IgDmFykkASS4SIvNoa2ZKCO_AUi4HiCk4f562DIUGKUsjsc?e=a9kIw7)，口令为 `HPCA26`。截至本文日期，浏览器转发页返回“Something went wrong. This item might have been deleted, expired, or you might not have permission ...”，通过 `172.38.8.77:7897` 的请求则返回 Microsoft “The request is blocked.”；两种页面都没有出现密码输入框。因而本报告使用可公开访问的 [HPCA 2026 官方日程](https://2026.hpca-conf.org/program/program-hpca-2026/)和 [IEEE Xplore](https://ieeexplore.ieee.org/) 摘要页取证；没有声称读到了共享目录中的全文、附录或硬件表格。

表中的“摘要级”表示数字或结论能在 IEEE 摘要中核对，但可能缺少实验平台、面积分解和置信区间。没有公开数字的地方明确写“摘要未给出”，不补猜 IPC、面积或 KiB。

## 候选总表

| 论文（IEEE 文档） | Hardware-only 机制 | 论文公开效果（保持原口径） | 硬件开销证据 | 当前 GEM5 落点与判断 |
|---|---|---|---|---|
| [I-POP: Ignite Positive Prefetchers](https://ieeexplore.ieee.org/document/11408454)（11408454） | 以 positive effect（PE）衡量每个预取器的收益/代价，动态调 degree，并关闭非正 PE 预取器 | 相对 Alecto：三套单核 workload **+3.5%**，16 核 **+8.6%**；相对 Bandit：单核 **+4.2%**，16 核 **+6.6%** | **1.46 KiB**（摘要级） | L2 已有 `L2CompositeWithWorkerPrefetcher`、BOP/CDP/CMC/Despacito 和按 source 的 issued/useful 统计。**P0**，低风险原型。 |
| [Streamlined on-Chip Temporal Prefetching](https://ieeexplore.ieee.org/document/11408470)（11408470） | 去除 temporal correlation 冗余，按 utility 保留 correlation，避免 Triangel resize 带来的 LLC traffic | 8-core 比 Triangel 高 **6.7 个百分点**；保存 **33% 更多 correlation**，coverage 高 **12.5 个百分点**；达到 Triangel 使用两倍 metadata 时的性能（摘要级表述） | 以 bounded correlation metadata 为主；公开摘要没有绝对面积 | `src/mem/cache/prefetch/` 有 CMC/STeMS/PIF 等 temporal 基础，但没有 Streamline 本身。**P1**，需要新表和容量公平 A/B。 |
| [Tempranillo: Non-Speculative Early Register Release](https://ieeexplore.ieee.org/document/11408522)（11408522） | 在确认不再有消费者、且不依赖恢复的条件下提前释放旧物理寄存器 | 单线程最高 **3.3%**；2-way SMT 最高 **11.8%**；每 KiB 性能收益最高分别 **2.6%/9.3%** | 摘要称 storage overhead modest，没有绝对 bit 数 | `kmhv3.py` 有 224 int/256 fp PReg；`rename.cc` 当前只在 commit/squash 历史处理释放，`renameSrcRegs()` 不增加 source use-count。**P1**，收益直接但必须先补生命周期模型。 |
| [The Last-Level Branch Predictor Revisited](https://ieeexplore.ieee.org/document/11408567)（11408567） | LLBP-X 根据动态上下文深度减少 metadata set contention 和模式重复 | 相对 baseline LLBP 的准确率增加 **0.8%-11.5%**，平均 **3.6%**；这是 accuracy，不是 IPC | 摘要只说 incremental hardware changes small，未给总存储量 | 当前没有 LLBP，主方向预测器是 8 表、2-way、32B block 的 `BTBTAGE`。需从 `btb_tage.*`、`decoupled_bpred.*`、FTQ/history path 切入。**P2**。 |
| [BARD: Reducing Write Latency of DDR5 Memory by Exploiting Bank-Parallelism](https://ieeexplore.ieee.org/document/11408565)（11408565） | 选择 dirty victim 做 cleansing，提升 DDR5 写请求的 bank parallelism | 不同 bank group/同 bank group/同 bank 写成本约 **1x/6x/24x**；BARD-E/C 的 bank parallelism **+30%**；BARD-H 在 SPEC2017/LIGRA/STREAM/Google server traces 平均 **+4.3%**、最高 **+8.5%** | **8 B SRAM/LLC slice**（摘要级） | 当前默认 `DRAMsim3` 配置为 XiangShan DDR4。需扩展 cache victim 信息、写队列和 DDR5 timing。**P2 条件项**。 |
| [Athena: Synergizing Data Prefetching and Off-Chip Prediction via Online Reinforcement Learning](https://ieeexplore.ieee.org/document/11408449)（11408449） | RL 按 epoch 观察准确率、带宽等状态，联合选择多级预取器开关和 aggressiveness，并协同 off-chip predictor | 摘要确认机制，但本文可取得的摘要没有完整定量效果 | RL 状态、策略表、epoch 更新和 off-chip predictor 接口；绝对开销未取得 | 当前有多预取器，但没有 OCP。先做 I-POP 后再评估是否值得引入。**暂缓**。 |
| [Intermittence-Aware Cache Compression](https://ieeexplore.ieee.org/document/11408535)（11408535） | 面向 energy-harvesting 频繁断电系统的可恢复 cache compression（Kagura） | 平均 **+4.74%**，最高 **+17.87%**，但收益场景是断电/恢复 | 压缩 metadata 和 checkpoint/recovery 状态；当前 CPU 没有断电模型 | 不满足当前 Kunminghu v3 的性能目标和硬件环境。**排除**。 |
| [PhasedStore: Supporting High-Performance Write-Through Cache-Coherence Protocols Under TSO](https://ieeexplore.ieee.org/document/11408509)（11408509） | TSO 下 write-through coherence 的分阶段 store 处理 | 论文针对特定 coherence/TSO 平台；本文未取得可比的当前 CPU 数字 | 需要 write-through coherence、排序和 phase 状态 | 当前 RISC-V Kunminghu v3 默认单核路径不是该协议。**排除**。 |

## 推荐项的实现合同

下面的合同遵循“workload event -> abstract resource state -> contention/backpressure -> observable stats”链条。默认 A/B 只改变硬件参数或硬件模型开关，不改二进制、OS、ISA 和 checkpoint。

### 1. I-POP（P0）

**机制。** 给每个 L2 预取源维护一个短期 epoch 窗口。预取发出、被 demand 命中、被丢弃、污染/驱逐和占用带宽分别计分，形成 PE；正 PE 的源保留并可提高 degree，非正 PE 的源降 degree 或停发。优先实现每源 admission/degree 控制，RL 不在第一版范围内。

**硬件开销。** 论文摘要给出 1.46 KiB。GEM5 行为模型只需要每源的有限计数器、阈值和 degree 状态；真正硬件实现的位宽、epoch 数和每源数量应作为参数，不能把 1.46 KiB 当成所有配置的固定成本。

**GEM5 落点。** `configs/common/PrefetcherConfig.py` 和 `src/mem/cache/prefetch/l2_composite_with_worker.*` 已经把 BOP、CDP、CMC、Despacito 组成一个 L2 管理对象；`prefetchStats.pfIssued_srcs` 与 `pfUseful_srcs` 可复用。建议新增一个 `PrefetchUtilityManager`（或等价的组合器层状态），不要在每个子预取器中复制策略。

**建模合同和复杂度。**

- 事件：预取 issue、prefetch hit/useful、late/unused、cache eviction、队列或 MSHR 拒绝。
- 状态/资源：每源 PE 累加器、degree、enabled/admission 位；L2 prefetch queue、MSHR 和下游带宽 token。
- 转移：`enabled -> issued -> useful/unused -> next-epoch update -> issue/drop`；队列满时保留现有 backpressure。
- 参数：`epoch_size`、PE 权重、正值阈值、最小/最大 degree、每周期 quota、是否允许 offload。默认配置应等价于当前所有源开关和 degree。
- 复杂度：每请求 O(1)，每 epoch 只遍历固定数量的预取源 O(P)，P 不随 trace 长度增长。
- stats：每源 issued/useful/late/unused、PE、degree 变化、admission drop、L2 queue occupancy、MSHR 使用率和 demand miss latency。

**A/B。** 先固定一个 SPEC checkpoint 的 warmup/ROI，比较 baseline、只启用正 PE、关闭非正 PE、degree sweep 四组。判定不仅看 IPC/simTicks，还要确认 demand miss latency 不恶化、prefetch pollution/unused 下降，以及 `pfIssued_srcs` 与 `pfUseful_srcs` 能解释收益。单核结果和 16 核结果分开报告，不能用论文的 16-core 百分比替代 GEM5 单核结论。

### 2. Streamline（P1）

**机制。** 对 temporal correlation 做去冗余和 utility 淘汰；表满时按 utility/年龄保留最有价值的 correlation。避免为了扩大表而搬运整个 metadata 到 LLC 的 resize traffic。硬件仍是 bounded table、tag/index、utility bits 和有限更新逻辑，不依赖软件提示。

**硬件开销。** 摘要报告存储 33% 更多 correlation、coverage 多 12.5 个百分点，但未给绝对面积或 bit 数；因此 GEM5 方案必须把 `metadata_entries`、每项 tag/target/utility/age 位宽和更新带宽显式参数化，并用相同的总 bit budget 与 Triangel 对比。

**GEM5 落点。** 可复用 `src/mem/cache/prefetch/cmc.*`、`spatio_temporal_memory_streaming.*`、`pif.*` 的训练/temporal 事件和 `L2CompositeWithWorkerPrefetcher` 的发射队列，但不能把现有 CMC 的 `enable_temporal` 直接宣称为 Streamline。第一版建议新建独立 `StreamlinePrefetcher`，通过现有 `BasePrefetcher` 参数接入 L2。

**建模合同和复杂度。** 训练和请求都用 hash index + bounded set/way；每次训练 O(1) 或固定小 `k`，淘汰使用 utility/age 的有限候选，不扫描无限历史。必须保留 correlation hit/miss、metadata eviction、duplicate drop、prefetch issue/useful 和 LLC traffic 这些会改变性能的控制结果；实际地址内容只保留 block/offset/age。

**A/B。** 在相同 metadata bit budget 下比较 baseline temporal、Streamline、Streamline + 33% entries，记录 coverage、accuracy、metadata hit、LLC read/write traffic、prefetch queue occupancy 和 demand IPC。额外做“相同 correlation 数”和“相同总 bit 数”两种公平基线，否则无法把收益归因到算法而不是容量。

### 3. Tempranillo（P1）

**机制。** 当旧物理寄存器的最后一个真正消费者完成，且该消费者已跨过可能回滚的安全点时，非投机地把寄存器放回 free list。若发生异常、squash 或 replay，仍保留需要恢复的映射；核心不依赖投机 release 后再恢复，因此不会引入错误路径。

**当前模型的关键差距。** `src/cpu/o3/rename.cc::removeFromHistory()` 当前在提交历史时释放旧映射，`doSquash()` 在回滚时处理新映射；`renameSrcRegs()` 只查找和绑定 source physical register，不增加 source-consumer 引用。`SimpleRenameMap::rename()` 的 refcount 主要表示映射/分配生命周期，不能直接当作 Tempranillo 所需的 use-count。这个差距必须先解决，不能简单提前调用 `tryFreePReg()`。

**硬件开销。** 摘要只称 modest storage overhead，并报告每 KiB 性能收益，没有绝对表项数。GEM5 设计应为每个 in-flight destination 维护 bounded consumer count 或 last-use token，附带 commit/squash-safe 标记；位宽和最大消费者数由参数控制。

**建模合同和复杂度。**

- 事件：rename source bind、producer writeback/complete、consumer issue/complete、commit、squash、exception/replay。
- 资源：int/fp free list、ROB/LSQ 中的 in-flight consumer identity、release width（当前 8）。
- 转移：`renamed -> tracked -> last-consumer-complete -> safe-to-release -> free`；任何未确认的 squash/exception 依赖都停留在 tracked。
- 参数：`enable_early_release`、安全窗口/恢复边界、最大 use-count、每周期 release width；默认关闭以保持旧行为。
- 复杂度：每条指令只处理其固定数量 operands O(src+dest)，每周期释放最多 `phyregReleaseWidth` 个，禁止全 ROB 无界扫描。
- stats：register-full stall、early-release count、release blocked reason、use-count overflow、squash/exception cancellation、free-list occupancy 和 rename-to-commit latency。

**A/B。** 先以单线程、无异常微测试验证“最后消费者完成后 free-list 增长且结果一致”，再用分支密集/长依赖 SPEC checkpoint 测 register-full cycles、rename stalls、IPC。随后才启用异常和 SMT（2-way）测试。任何提前释放导致的 mismatch 都优先回到生命周期合同，而不是放宽 guard。

### 4. LLBP-X（P2）

**机制。** LLBP-X 让 last-level predictor 根据动态上下文深度选择 metadata，减少 set contention 和重复模式；论文相对 baseline LLBP 报告的是方向预测准确率提升 0.8%-11.5%，平均 3.6%。

**GEM5 映射风险。** 当前 [BTBTAGE 文档](../../src/cpu/pred/btb/docs/btb_tage.md)描述的是 block-based TAGE 方向层：`BranchPredictor.py` 中 8 个表、每表 2048 项、2-way、32B block、4-bank 模拟，`DecoupledBPUWithBTB` 还串联 uBTB/ABTB/MicroTAGE/MBTB/ITTAGE/MGSC/RAS。仓库没有 LLBP 实现；因此不能把增加 BTBTAGE 表项直接称为 LLBP-X。

**实现建议。** 先定义一个独立 `LastLevelBP` 接口，输入 PC、路径/全局 history 和 dynamic context depth，输出 prediction + metadata；由 FTQ 保存最小更新信息，在 resolve/commit 时更新。用可配置 context-depth set、way、tag bits、metadata bits 和 bank/lookup latency，保持每次 lookup O(number of selected contexts)，不复制论文的无界搜索。

**A/B 和成本闸门。** 先在同一 BTB 容量和访问端口下比较 misprediction、FTQ redirect、fetch bubbles、MPKI 和 storage bits；只有 accuracy 的提升转化为 fetch/ROB 进展，且 timing/area budget 可接受，才继续做复杂的动态深度策略。报告中不把 3.6% accuracy 写成 3.6% IPC。

### 5. BARD（P2，条件项）

**机制。** BARD 利用 dirty victim/cleansing，把写回工作安排到可并行的 DDR5 bank，降低同 bank group 和同 bank 的写冲突。摘要给出的相对写成本约为 1x/6x/24x，BARD-E/C 的 bank parallelism 增加 30%；混合策略 BARD-H 在 SPEC2017、LIGRA、STREAM 和 Google server traces 上平均性能 +4.3%、最高 +8.5%，开销为每 LLC slice 8 B SRAM。该结果仍受 DDR5 on-die ECC、bank-group timing 和写回流量条件约束。

**当前模型差距。** `configs/common/xiangshan.py` 在 `mem_type=DRAMsim3` 且未指定 ini 时选择 `ext/dramsim3/xiangshan_configs/xiangshan_DDR4_8Gb_x8_3200_2ch.ini`。`XSDRRIPRP::getVictim()` 的输入是 replacement candidates，现有 replacement data 没有足够的 dirty victim、bank group、写队列和 DRAM timing 信息。BARD 需要 cache/tag 传递 dirty/address/bank metadata，memory controller 维护 cleansing/write window，并在 DRAMsim3 或 Ramulator2 中提供 DDR5 timing，不能只改 replacement policy。

**A/B。** 先用 DDR5 配置校准 row/bank-group timing，再比较 baseline、仅 bank-aware write scheduler、scheduler + cleansing。统计 write queue occupancy、row-hit、bank parallelism、write-to-read turnaround、dirty victim 次数、读延迟和 CPU IPC；在 DDR4 默认路径上不宣称 BARD 收益。

### 6. Athena（暂缓）

Athena 用 online RL 以 epoch 为粒度观察预取准确率、带宽等状态，同时选择多级预取器的开关和 aggressiveness，并协同 off-chip predictor。它满足硬件-only 的概念，但当前 L2 组合器虽然已有多个预取源，仓库没有 OCP、RL policy table 或训练/恢复接口。建议把它作为 I-POP 的后续控制器实验：先复用 I-POP 的 per-source PE 和 bounded epoch state，再以固定小策略表替代通用 RL；在没有全文成本表和 OCP 需求定量前，不应把 Athena 作为第一批 GEM5 feature。

## 不推荐项和边界

- **Intermittence-Aware Cache Compression / Kagura**：摘要报告平均 4.74%、最高 17.87%，但目标是频繁断电的 energy-harvesting 系统。当前 CPU 没有断电、恢复和压缩 checkpoint 能耗模型，无法证明对正常 Kunminghu v3 性能有效，排除。
- **PhasedStore**：依赖 TSO 下 write-through coherence 的协议和排序前提；当前 RISC-V Kunminghu v3 默认路径不是该 coherence 场景，排除。
- **SMT/安全内存/加速器/软件调度类工作**：除非能把机制完整落到当前 CPU 的硬件控制路径并保持软件不变，否则不纳入本次硬件-only 清单。仅把编译器提示、OS 调度或专用加速器换成 GEM5 参数不算满足要求。

## GEM5 当前事实和建议入口

以下是本次判断所依据的源码事实，便于后续实现时避免把论文平台假设误带入模型：

| 主题 | 当前代码事实 | 相关文件 |
|---|---|---|
| O3 rename/PReg | int PReg=224、fp PReg=256、release width=8；rename 在 commit/squash 历史路径处理释放 | `configs/example/kmhv3.py`；`src/cpu/o3/rename.cc`；`src/cpu/o3/rename_map.cc` |
| BPU | `DecoupledBPUWithBTB` 包含 uBTB/ABTB/MicroTAGE/MBTB/BTBTAGE/ITTAGE/MGSC/RAS；BTBTAGE 默认 8 表、2-way、32B block、4-bank 参数 | `src/cpu/pred/BranchPredictor.py`；`src/cpu/pred/btb/btb_tage.*` |
| L2 prefetch | `L2CompositeWithWorkerPrefetcher` 可开关 BOP/CDP/CMC/Despacito，并按 source 记录 issued/useful | `configs/common/PrefetcherConfig.py`；`src/mem/cache/prefetch/l2_composite_with_worker.*` |
| L2 replacement | 当前 `XSDRRIPRP(mode=2)` 是 DRRIP；victim 接口主要提供 replacement candidates | `configs/example/kmhv3.py`；`src/mem/cache/replacement_policies/xs_drrip_rp.*` |
| DRAM | DRAMsim3 默认 XiangShan DDR4 ini；BARD 的 DDR5 bank timing 尚未建模 | `configs/common/xiangshan.py`；`src/mem/dramsim3*` |

## 统一验证流程

1. **基线固定。** 记录 git commit、`kmhv3.py` 参数、checkpoint、warmup/ROI、核心数和内存 ini；比较 reset 后 ROI stats，不混用 warmup 计数。
2. **最小可归因 A/B。** 每个 feature 先只改变一个开关或一个参数，保留旧默认行为为 baseline；为关键状态加 bounded counters 和 reason stats。
3. **趋势检查。** 调大队列/metadata 应减少 full/drop，调大带宽应减少 busy，调高延迟应增加等待；若 IPC 变化不能由 occupancy、latency、replay 或 bandwidth stats 解释，不接受结果。
4. **功能和时序闸门。** PReg 必须通过 squash/异常/difftest；预取器必须确认 demand miss 没有恶化；BPU 必须确认 history recovery；BARD 必须先通过 DDR5 timing 校准。之后再跑目标 SPEC checkpoint 和多核扩展。
5. **成本记录。** GEM5 行为模型报告 entry 数、字段位宽、端口/队列 token 和每周期更新上限；若要映射真实硬件，另行给 SRAM bit、比较器、更新端口和时序估算，不用论文的单一百分比替代面积。

## 参考来源

- [HPCA 2026 官方日程](https://2026.hpca-conf.org/program/program-hpca-2026/)
- [The Last-Level Branch Predictor Revisited, IEEE 11408567](https://doi.org/10.1109/HPCA68181.2026.11408567)
- [Tempranillo, IEEE 11408522](https://doi.org/10.1109/HPCA68181.2026.11408522)
- [I-POP, IEEE 11408454](https://doi.org/10.1109/HPCA68181.2026.11408454)
- [Athena, IEEE 11408449](https://doi.org/10.1109/HPCA68181.2026.11408449)
- [Streamlined on-Chip Temporal Prefetching, IEEE 11408470](https://doi.org/10.1109/HPCA68181.2026.11408470)
- [Intermittence-Aware Cache Compression, IEEE 11408535](https://doi.org/10.1109/HPCA68181.2026.11408535)
- [BARD, IEEE 11408565](https://doi.org/10.1109/HPCA68181.2026.11408565)
- [PhasedStore, IEEE 11408509](https://doi.org/10.1109/HPCA68181.2026.11408509)

**证据边界：** 以上定量数字均按论文公开摘要的原始口径转述；除非标明“GEM5 预期”，否则不是对当前 CPU 的性能承诺。下一步实现应先完成 I-POP 的可归因原型，再决定是否投入 Streamline/Tempranillo 的更大状态和验证成本。
