# ASPLOS 2024 Volume 3 面向 Kunminghu v3/GEM5 的硬件特性筛选

**调研日期：** 2026-08-05

**范围：** 用户给出的 [ASPLOS 2024 Volume 3 proceedings](https://dl.acm.org/doi/proceedings/10.1145/3620666)。Crossref 将该 DOI 标为 *Proceedings of the 29th ACM International Conference on Architectural Support for Programming Languages and Operating Systems, Volume 3*；DBLP 目录与 DOI 子项交叉核对为 1 个 volume 级记录和 70 个条目（其中包含 vision/industry talk，全部按题名完成初筛）。Volume 1（10.1145/3617232）与 Volume 2（10.1145/3620665）不在本次 DOI 范围内。

## 结论

在“已有二进制、编译器、OS、运行时、RISC-V ISA 和外部平台均不变，只增加有限 CPU/cache/TLB/预取/内存控制器硬件状态”的严格口径下，**本 volume 只有一项值得直接进入当前 GEM5 CPU 的第一批研究：PATHFINDER 预取器**。另有一项低成本的 Limoncello-inspired 带宽门控可做微实验，但它是把论文的一部分策略改写成硬件，不能引用论文的完整收益。

| 优先级 | feature | 论文报告的效果 | 论文报告的硬件开销 | 只改硬件是否成立 | 当前 GEM5 判断 |
| --- | --- | --- | --- | --- | --- |
| **P0** | **PATHFINDER：SNN + STDP 在线 delta 数据预取** | 摘要称在多组 benchmark 上与 state-of-the-art 预取器有竞争力；没有可核对的统一 IPC 百分比。 | 12 nm 下 **0.23 mm2、0.5 W**。摘要未给表项/位宽分解。 | **是**：学习和推理都在硬件预取器中进行，不要求离线训练数据。 | 最值得做，但必须先把 SNN 事件、STDP 更新、预取时序和带宽污染建模为有界状态；论文面积/功耗不能直接外推为 GEM5 IPC。 |
| **P1（对照实验）** | **Limoncello 的硬件子集：带宽感知的 HWP admission/throttle** | 完整 Limoncello 在 Google fleet 吞吐 **+10%**、内存延迟 **-15%**；其中还依赖 targeted software prefetch。硬件子集的效果未知。 | 论文没有新增硬件，完整机制的软件控制成本也未给绝对值。若仅做硬件门控，成本是带宽/occupancy 计数器、阈值和每源 gate。 | **仅硬件子集成立**；论文完整方案不成立。 | 当前 `Queued` 已有按窗口、按源 admission 和 PFBad 自适应，可作为实现入口；需加入真实 memory-bandwidth/queue-pressure 输入，并把结果标为新假设。 |
| **P2（条件项）** | **BeeZip：BeeHash + HiveMatch 压缩 accelerator** | Silesia 上最高吞吐 **10.42 GB/s**（压缩比 2.96），最佳压缩比 3.14 时吞吐 5.95 GB/s；相同压缩比下相对单线程/36 线程软件分别 **23.2x/2.45x**，压缩比至少提高 9%。 | 需要 hash engine、match engine、大滑动窗口存储、动态调度和新的两阶段算法；摘要未给绝对面积/功耗。 | **不满足当前 CPU 的严格范围**：算法为 accelerator parallelism 改写，需软件/接口协同。 | 只有在明确增加压缩协处理器、调用协议和压缩 workload 时才值得建模；不是当前通用 O3 的局部 feature。 |

因此，不建议因为某篇论文的 accelerator speedup 很大，就把专用硬件直接移植到 `kmhv3.py` 并宣称 CPU 性能会提高。当前可执行的首选是 PATHFINDER 的被动统计和硬件预取 A/B；Limoncello 子集是低成本的控制实验；其余工作留在排除或条件清单。

## 筛选口径与证据边界

### 严格的 hardware-only 定义

候选必须同时满足：

- 现有程序和既有 RISC-V ISA 不变；不需要编译器插桩、软件 API、运行时、OS 页面/内存分配策略或离线 profile；
- 行为可以实现为有限表项、cache-line metadata、队列、饱和计数器、端口或有界延迟；不把 oracle 正确率当作零成本硬件；
- 性能因果链落在当前 CPU 的 fetch、branch、TLB、cache、预取或 DRAM 路径，而不是新 GPU/NPU/DSA、SmartNIC、CXL pod 或 NAND 设备；
- 能使用相同 checkpoint、warmup/ROI 和内存配置做 A/B，并用 miss、latency、queue、bandwidth 和 IPC 统计解释结果。

“论文有硬件”不等于“当前 CPU 只加硬件即可复现”。例如 BeeZip 的硬件单元配套重新组织后的压缩算法，ACES 的 cache policy 与 SpMM accelerator 的执行流/非阻塞 buffer 绑定，这些都不满足透明通用 CPU feature 的定义。

### 目录与摘要获取

- [Crossref proceedings record](https://api.crossref.org/works/10.1145/3620666)：确认 DOI、Volume 3、出版日期 2024-04-27。
- [DBLP Volume 3 table of contents](https://dblp.org/db/conf/asplos/asplos2024-3.html)：确认 70 个条目题名及逐篇 DOI。
- [PATHFINDER Semantic Scholar record](https://api.semanticscholar.org/graph/v1/paper/DOI:10.1145/3620666.3651332?fields=title,abstract)：提供机制和 0.23 mm2/0.5 W 的摘要级定量证据。
- [Limoncello Semantic Scholar record](https://api.semanticscholar.org/graph/v1/paper/DOI:10.1145/3620666.3651373?fields=title,abstract)：提供 software-centric、+10% throughput、-15% memory latency 的摘要级证据。
- [BeeZip Semantic Scholar record](https://api.semanticscholar.org/graph/v1/paper/DOI:10.1145/3620666.3651323?fields=title,abstract)：提供 accelerator 组成和吞吐/压缩比数字。

ACM proceedings 页面和 PDF 在当前环境经 Cloudflare 返回 403；因此文中的量化结果只使用可复核的 Crossref/DBLP/Semantic Scholar 摘要。摘要没有给出的表项位宽、端口数、面积分解或完整 workload 几何平均值，均明确写为 unavailable，不用猜测补齐。

## P0：PATHFINDER

**论文：** [PATHFINDER: Practical Real-Time Learning for Data Prefetching](https://doi.org/10.1145/3620666.3651332)，Lin Jia、James McMahon、Sumanth Gudaparthi、Shreyas Singh、Rajeev Balasubramonian，ASPLOS 2024 Volume 3，pp. 785-800。

### 机制摘要

传统 stride、address-correlating、delta 和 irregular prefetcher 通常用固定规则；神经预取器能学复杂模式，但依赖大 benchmark 的离线训练，模型大、推理成本高且对未见模式泛化差。PATHFINDER 用**脉冲神经网络（SNN）**表示访问流中的 delta，并用**局部 STDP（spike-timing-dependent plasticity）**在推理时同步学习：每次访问产生有限脉冲/事件，局部突触权重据时间关系更新，随后生成未来 delta/block 地址。

这条机制满足硬件-only 的关键原因是：没有训练数据文件、编译器提示、ISA 扩展或 OS 协议；训练和 inference 都是硬件预取器内部的在线 bounded state。它也不是“用一个不可实现的神经网络 oracle”掩盖成本，论文给出综合后的面积和功耗。

### 效果与开销

| 项目 | 论文证据 | 对当前 GEM5 的解释 |
| --- | --- | --- |
| 准确性/性能 | 摘要称在一组 benchmark 上与 state-of-the-art prefetcher **competitive**，但没有统一 IPC、coverage 或 speedup 数字 | 只能把论文结论作为方向性证据；当前 SPEC/checkpoint 的收益必须实测，不能写成“预期 +X% IPC” |
| 面积 | 12 nm **0.23 mm2** | 是论文实现的总 footprint，不等于 GEM5 对象大小；应作为面积预算参考，并保留工艺/电压差异说明 |
| 功耗 | **0.5 W** | 论文摘要级数字；需要确认是预取器独立功耗还是指定配置下的估算，当前不能转成系统能耗结论 |
| 结构成本分解 | 摘要未给 SNN neuron/synapse 数、权重位宽、表项数、端口和每访问更新能量 | GEM5 原型必须把这些设为参数，不能把 0.23 mm2 当成任意配置的固定成本 |
| 学习成本 | STDP 是局部更新，目标是实时学习未见 delta pattern | 仍需建模每周期 lookup、脉冲传播、权重读写和 update latency；不能假设更新完全免费或不占端口 |

### 当前 GEM5 映射

当前 `kmhv3.py` 的 L1D/L2 路径已经有可复用的预取基础设施：

| 论文组件 | 当前实现事实 | 设计影响 |
| --- | --- | --- |
| 数据访问输入 | `BasePrefetcher::PrefetchInfo` 从 cache access 传入地址、PC/安全属性等信息；`Queued` 负责延迟队列、翻译队列和请求发射 | PATHFINDER 可先作为 `QueuedPrefetcher` 子类接到 L2；第一版不需要修改 CPU ISA 或软件 |
| 当前 L1D 组合 | `configs/common/PrefetcherConfig.py` 默认 `XSCompositePrefetcher`，可组合 active-page/PHT/Berti/temporal 等源 | 应做 `baseline / PATHFINDER-only / composite+PATHFINDER` 三组，避免把现有源的收益算给新 SNN |
| 当前 L2 组合 | `L2CompositeWithWorkerPrefetcher` 组合 BOP、CDP、CMC、Despacito；`kmhv3.py` 的 L2 replacement 是 `XSDRRIPRP(mode=2)` | 建议先挂 L2，固定现有 source 和 queue；若改组合器，必须按 source 记录命中/污染 |
| 已有 bounded 控制 | `Queued` 已有 `pf_control_window`、按源 admission、PFBad table 和 `pf_adaptive_*` 统计；默认自适应开关由配置决定 | 可复用队列、filter、stats 和 admission，不要复制一套无限历史。PATHFINDER 的 SNN/STDP state 仍需独立，以免混淆训练语义 |
| 已有关键统计 | `pfIssued/pfUseful/pfUnused/pfBad` 及按源向量，另有 demand MSHR miss、late、queue full/drop 统计 | 这些能支持第一轮 coverage/accuracy/pollution A/B，但还需加 SNN lookup/update、synapse hit、weight saturation、event queue full 和 training latency |

### GEM5 原型的硬件合同

建议把第一版限定为一个可配置的小型 SNN delta prefetcher：

1. **事件。** 每个 demand line（必要时只取 L2 miss）转成 block delta；delta 经过有界编码后触发固定数量的 pre-synaptic spikes。
2. **状态。** 维护固定数量的 neuron/synapse entries、有限位宽权重、last-spike timestamp/age、valid/tag 和饱和标志；entry 满时用明确的 age/utility victim，不扫描完整历史。
3. **更新。** 按 STDP 的局部时间窗口更新权重；读写端口、每周期最大 update 数和 update latency 是显式参数。权重溢出、事件队列满、同周期冲突必须有统计和确定的 tie-break。
4. **发射。** 仅从当前命中且达到 confidence threshold 的 delta 生成地址，遵守 page boundary、cache snoop、MSHR/PFQ 和既有 backpressure；不允许为方便对拍而绕过翻译或队列限制。
5. **开关。** `enable_pathfinder=False` 时行为和统计语义与 baseline 相同；SNN lookup latency、STDP update latency、table entries、weight bits、delta range、degree 和 admission pct 全部可配置。

这个合同的复杂度是每个 access O(1)（固定小表/固定 fan-out），每个周期最多处理 `lookup_width` 个事件和 `update_width` 个权重更新；不能随 trace 长度增长。真实硬件成本至少包括 synapse SRAM、tag/compare、脉冲/时间比较、权重加法器、读写端口和预取队列带宽。论文只给 0.23 mm2/0.5 W，故 GEM5 文档中必须另外记录“模型参数 -> SRAM bits/比较器/端口”的下界，而不是伪造面积。

### 分阶段 A/B 与验收

**Phase 0：观测。** 在固定 checkpoint、warmup、ROI、core count、prefetcher、DRAM ini 下，先导出 demand block delta 的直方图、重复率、页边界比例、L2 miss latency、PFQ/MSHR occupancy 和 memory bandwidth。若 workload 没有足够的可学习 delta 或预取已经被带宽限制，不进入完整 SNN。

**Phase 1：SNN 旁路学习。** PATHFINDER 接收同一访问流、执行 lookup/STDP，但不发请求；验证 event/update 数有界、权重饱和可恢复，且不会改变 baseline timing。记录 table hit、unknown delta、update overflow 和每周期 update stall。

**Phase 2：PATHFINDER-only。** 关闭其他 L1D/L2 prefetch source，只启用 PATHFINDER；比较 `off / baseline source / Pathfinder`。至少报告 `pfIssued/pfUseful/pfUnused/pfBad`、late、coverage、accuracy、demand miss latency、MSHR/PFQ occupancy、DRAM requests/bandwidth 和 committed IPC。

**Phase 3：组合与压力。** 在固定总 queue/MSHR/bandwidth 下比较 `baseline composite` 与 `composite+PATHFINDER`；额外 sweep SNN entries、weight bits、lookup/update width、degree。只有当 IPC 改善能由 demand latency/late/pollution 统计解释，且总带宽和队列压力没有不可接受恶化，才把它列为实现候选。

论文没有给当前 GEM5 workload 的 IPC 预期。任何“competitive”结果都必须标记为论文跨 benchmark 的定性结论；不能把 0.23 mm2 或 0.5 W 直接当成 XiangShan RTL 面积/功耗，也不能把 SNN 的准确率提升直接写成 IPC 提升。

## P1 对照：Limoncello 的硬件子集

**论文：** [Limoncello: Prefetchers for Scale](https://doi.org/10.1145/3620666.3651373)，Akanksha Jain 等，pp. 577-590。

### 论文机制和为什么不直接纳入

Limoncello 在高利用率数据中心动态配置预取器：内存带宽利用率高时关闭硬件预取器，再用 targeted software prefetch 弥补 cache miss。Google fleet 结果是应用吞吐 **+10%**、内存延迟 **-15%**，并保持目标库函数 cache miss 变化很小。论文明确称其为 **software-centric** 且“不需要修改硬件”。

因此完整 Limoncello 不满足本调研的 hardware-only 条件，+10%/+15% 也不能归因给当前 GEM5 的硬件门控。可以做的只是一个**新假设**：保留带宽感知的 hardware-prefetch admission/throttle，删除 software-prefetch 部分。该子集的净收益没有论文证据，必须独立 A/B。

### 低成本硬件实验合同

- 输入：DRAM controller 的 busy cycles、读写队列 occupancy、每周期下行带宽和 demand miss latency；若暂时拿不到 controller 信号，先用 L2 miss queue/MSHR occupancy 作为代理并明确标记。
- 状态：每个 prefetch source 一个饱和 admission/gate 位或 2-bit hysteresis；窗口计数器、high/low watermark、切换次数统计。固定源数下是 O(1) 更新，不需要软件调用。
- 动作：高压时降低/关闭 HWP admission，低压时恢复；必须保留 demand request 优先级、PFQ/MSHR 上限和已有 `pfUnused/pfBad` 统计。
- 当前落点：`Queued` 已有窗口化、按源 admission 和 PFBad adaptive，但现有控制目标是 usefulness/miss rate，不是实际 DRAM bandwidth；应扩展输入而不是重写队列。

验收必须包含 `baseline / paper-like software control（若有） / hardware-only gate` 三组，报告 bandwidth、memory latency、cache pollution、prefetch accuracy/coverage 和 IPC。若只关闭预取而没有软件补偿导致 IPC 下降，这是硬件子集的真实结果，不是实现失败。

## 近似但不推荐移植的论文

| 论文 | feature、效果和成本证据 | 排除原因 |
| --- | --- | --- |
| [BeeZip](https://doi.org/10.1145/3620666.3651323) | BeeHash 动态 hash 调度 + HiveMatch 可扩展匹配单元，Silesia 最高 10.42 GB/s，压缩比 2.96；相同压缩比下相对单线程/36 线程软件 23.2x/2.45x，压缩比至少高 9%。 | 两阶段压缩算法专门为 accelerator parallelism 改写，需要新 accelerator、存储和调用协议；不能让既有通用二进制透明受益。绝对面积/功耗未在摘要披露。 |
| [ACES](https://doi.org/10.1145/3620666.3651381) | SpMM 专用 accelerator，adaptive execution、locality-concurrency cache replacement、non-blocking buffer，摘要报告 2.1x speedup。 | cache policy 与 SpMM 执行流和专用数据通路耦合；移植到通用 L2 会失去 workload/metadata 语义，需要新的 accelerator 和 workload。 |
| [A Midsummer Night's Tree](https://doi.org/10.1145/3620666.3651354) | 面向 SCM 完整性的 tree-within-tree；相对已有方案平均减少执行开销 41%，并降低片上安全 metadata 面积。 | 摘要明确把硬件复杂度 offload 到 software；目标是 secure SCM/持久化，不是当前 DRAM/L2 CPU 性能。 |
| [AERO](https://doi.org/10.1145/3620666.3651341) | NAND erase latency 自适应；160 个 3D NAND 芯片验证寿命 +43%，11 个 workload 的 SSD read tail latency 平均 -34%。 | 修改 SSD NAND/FTL 控制，不改变当前 CPU pipeline；需要 SSD/flash 模型，不能在 `kmhv3.py` 单核 A/B 中归因。 |
| [Harp](https://doi.org/10.1145/3620666.3651331) | 基因组 sequence-to-graph accelerator，平均相对软件 140x、相对 SOTA accelerator 23.6x，芯片面积减少 72%。 | 专用算法数据结构和 accelerator，不是通用 CPU feature。 |
| [FEASTA](https://doi.org/10.1145/3620666.3651336) | 稀疏张量代数 ISA + instruction-driven accelerator，能效 geomean +5.40x，相对 CPU extension 1.47x/3.19x。 | 明确新增 SpTA ISA 和专用单元，违反既有 ISA/软件不变约束。 |
| [TAROT](https://doi.org/10.1145/3620666.3651325) | H-TAROT 软件扫脆弱地址；SmartNIC 版本卸载 RowHammer therapy。 | 是安全/可靠性机制，且依赖 DRAM 地址探测、CXL SmartNIC；不以 CPU 性能提升为目标。 |
| [Longnail](https://doi.org/10.1145/3620666.3651375) | 从 CoreDSL 描述自动生成可移植 RISC-V custom instruction extension。 | 收益依赖新指令、编译器生成和应用重编译；不是只改硬件。 |
| [Energy-Adaptive Buffering](https://doi.org/10.1145/3620666.3651370) | REACT 可变电容 buffer；相对固定 buffer 有用能量 +25.6%、响应快 7.7x。 | 目标是 batteryless energy-harvesting system，当前 CPU 没有该电源/断电模型。 |
| [Limoncello](https://doi.org/10.1145/3620666.3651373) | 完整方案的 fleet throughput +10%、memory latency -15%。 | 需要 targeted software prefetch；完整机制不是 hardware-only，单独硬件 gate 只能列为新假设。 |

其余文章的主贡献分别属于 compiler/DSL/runtime/OS、GPU/NPU/PIM/FPGA/quantum accelerator、云调度/网络/存储、软件安全/验证或分析工具；它们不满足当前 CPU 的透明硬件 feature 条件。没有必要把这些工作包装成 cache 参数来制造“可移植性”。

## Volume 3 全量初筛清单

以下按 DBLP Volume 3 的 70 个条目题名分组；“候选”只表示进入上文的详细分析，不表示论文质量排序。

| 分类 | 文章 |
| --- | --- |
| **候选/对照** | PATHFINDER；Limoncello |
| **vision/industry talk（排除）** | Societal infrastructure in the age of Artificial General Intelligence；Harnessing the Power of Specialization for Sustainable Computing |
| **专用 accelerator/ISA（排除）** | 8-bit Transformer Inference and Fine-tuning for Edge Accelerators；ACES；AWS Trainium；Accelerating Multi-Scalar Multiplication for Efficient Zero Knowledge Proofs with Multi-GPU Systems；BeeZip；Boost Linear Algebra Computation Performance via Efficient VNNI Utilization；C4CAM；DTC-SpMM；Dr. DNA；EVT；FEASTA；GSCore；Harp；Hector；IANUS；NDPipe；NeuPIMs；Optimal Kernel Orchestration for Tensor Programs with Korch；PrimePar；Promatch；ProxiML；SpecInfer；SpecPIM；TAPA-CS；TinyForge |
| **存储/内存设备或平台依赖（排除）** | AERO；A Midsummer Night's Tree；Challenges and Opportunities for Systems Using CXL Memory；Energy-Adaptive Buffering for Efficient, Responsive, and Persistent Batteryless Systems；FaaSMem；GMT；MemSnap μCheckpoints；More Apps, Faster Hot-Launch on Mobile Devices via Fore/Background-aware GC-Swap Co-design；TAROT |
| **编译器/运行时/软件优化（排除）** | A shared compilation stack for distributed-memory parallelism in stencil DSLs；AdaPipe；Felix；Fermihedral；Flexible Non-intrusive Dynamic Instrumentation for WebAssembly；Fractal；FUYAO；Getting a Handle on Unmanaged Memory；Kaleidoscope；Longnail；MAGIS；Merlin；MorphQPV；OnePerc；SIRO；SlimSLAM；SmartMem；Thesios |
| **云/网络/系统策略（排除）** | AUDIBLE；Centauri；Characterizing a Memory Allocator at Warehouse Scale；Characterizing Power Management Opportunities for LLMs in the Cloud；Going Green for Less Green；NetRen；TCCL |
| **安全、验证和分析（排除）** | CSSTs；Enforcing C/C++ Type and Scope at Runtime for Control-Flow and Data-Flow Integrity；Explainable Port Mapping Inference with Sparse Performance Counters for AMD's Zen Architectures；Pathfinder: High-Resolution Control-Flow Attacks Exploiting the Conditional Branch Predictor；Pythia；RTL-Repair；Zoomie |

题目跨域（例如 Kaleidoscope、Fermihedral、Harp）时按“为什么不能在当前 CPU 透明实现”的主因只归入一个分类。Volume 3 的 vision/industry talk 也完成了筛选，但不是可直接实现的 feature。

## 推荐实现顺序

1. **PATHFINDER Phase 0/1：** 先采集 delta 可学习性和 SNN 旁路训练统计，验证 state/端口/事件有界。
2. **PATHFINDER-only A/B：** 固定队列、MSHR、DRAM 和其他 prefetch source，测真实 checkpoint 的 coverage、accuracy、late、pollution、带宽和 IPC。
3. **PATHFINDER 与现有组合器：** 只有单独 PATHFINDER 不恶化 demand latency 且收益可解释时才接入 `XSCompositePrefetcher` 或 L2 composite。
4. **Limoncello hardware subset：** 作为低成本压力控制对照，接入实际 memory-controller occupancy；不把论文 fleet 数字写入 GEM5 预期。
5. **BeeZip/ACES 等：** 只有在项目范围明确新增 accelerator、ISA/software interface 和对应 workload 时另立项目；不作为当前 CPU feature 排期。

## 统一验证要求

- 固定 git commit、`kmhv3.py` 参数、checkpoint、warmup/ROI、core count、prefetcher profile 和 DRAM ini；只比较 warmup 后 reset 的 ROI stats。
- 每个开关先做 `off / baseline / feature-only / combined`，确保单变量归因；记录 table entries、metadata bits、queue size、更新宽度和每周期端口约束。
- 预取器必须同时报告 demand miss latency、MSHR/PFQ occupancy、DRAM traffic/bandwidth、late/unused/PFBad 和 IPC；不能只看 hit rate。
- 若 SNN weight saturation、event queue overflow、translation retry 或 checkpoint restore 出错，先修正生命周期/时序合同，不要放宽断言或丢弃统计。
- 论文数字是其平台、工艺、workload 和 baseline 下的结果。除 PATHFINDER 论文报告的 0.23 mm2/0.5 W 和各近似项明确列出的原始数字外，本文不外推当前 XiangShan RTL 或 GEM5 的面积、功耗和 IPC。

## 参考来源

- [ASPLOS 2024 Volume 3 proceedings DOI](https://dl.acm.org/doi/proceedings/10.1145/3620666)
- [Crossref metadata for 10.1145/3620666](https://api.crossref.org/works/10.1145/3620666)
- [DBLP ASPLOS 2024 Volume 3](https://dblp.org/db/conf/asplos/asplos2024-3.html)
- [PATHFINDER DOI](https://doi.org/10.1145/3620666.3651332)；[Semantic Scholar abstract record](https://api.semanticscholar.org/graph/v1/paper/DOI:10.1145/3620666.3651332?fields=title,abstract)
- [Limoncello DOI](https://doi.org/10.1145/3620666.3651373)；[Semantic Scholar abstract record](https://api.semanticscholar.org/graph/v1/paper/DOI:10.1145/3620666.3651373?fields=title,abstract)
- [BeeZip DOI](https://doi.org/10.1145/3620666.3651323)；[ACES DOI](https://doi.org/10.1145/3620666.3651381)
- [AERO DOI](https://doi.org/10.1145/3620666.3651341)；[A Midsummer Night's Tree DOI](https://doi.org/10.1145/3620666.3651354)
- [Harp DOI](https://doi.org/10.1145/3620666.3651331)；[FEASTA DOI](https://doi.org/10.1145/3620666.3651336)；[Longnail DOI](https://doi.org/10.1145/3620666.3651375)

**验证状态：** 本次交付只新增调研文档和 ExecPlan，没有修改 GEM5 模型代码；未声称构建通过、跑分完成或获得当前 CPU 的 IPC 提升。ACM 全文 PDF 在当前网络经 Cloudflare 返回 403，候选定量证据以 Crossref/DBLP/Semantic Scholar 可复核摘要为准。
