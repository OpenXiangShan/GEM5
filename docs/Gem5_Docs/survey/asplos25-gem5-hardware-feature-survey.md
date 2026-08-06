# ASPLOS 2025 Volume 1 面向 Kunminghu v3 GEM5 的硬件特性筛选

**调研日期：** 2026-08-05

**范围：** 用户给出的 [ASPLOS 2025 Volume 1 proceedings](https://doi.org/10.1145/3669940)，而不是未在输入中指定的其他 ASPLOS 2025 volume。本文面向当前 `configs/example/kmhv3.py` 的 RISC-V Kunminghu v3/GEM5 核心、其 RISC-V MMU、L1/L2 cache 和 DRAM 路径。

## 结论

这一个 volume 的 72 篇论文以系统、云、存储、GPU/AI、编译器与专用加速器为主。按“无需改变既有应用、编译器、OS、运行时、ISA 或外部平台；仅增加有限的 CPU/cache/memory-controller 硬件状态即可改变当前 CPU 的可见性能”筛选后，**只有一项应进入当前 GEM5 的第一批研究队列**：

| 优先级 | 论文/feature | 论文报告效果和开销 | 只改硬件是否成立 | 当前 GEM5 判断 |
| --- | --- | --- | --- | --- |
| P0 | **iTP + xPTP**：Instruction Translation Prioritization + extended Page Table Prioritization | 相对论文的 LRU STLB+L2C baseline，单核几何平均 **+18.9%**，SMT co-location **+11.4%**。论文绝对 storage/area/energy 在当前可获得的摘要中**未披露**。 | 是。核心动作是 shared last-level TLB（STLB）和 L2 replacement，不需要修改二进制或 ISA。 | 值得先做被动统计和分阶段 A/B；但现有 unified L2 TLB、压缩/预取和 `XSDRRIPRP` 与论文 LRU STLB+L2C 不同，不能把 +18.9% 当作本树预期。 |

这里没有按论文加速比硬凑“Top N”。`pulse`、MOAT、RANGE-BLOCKS 等工作含有硬件，但分别依赖新的近内存/网络平台、只解决安全约束且带来 slowdown、或只针对 128-tile DSA，不会仅改当前通用 O3 CPU 就复现论文收益。详细边界见下文。

## 筛选口径和证据边界

### 严格的 hardware-only 定义

主候选必须同时满足：

- 已有程序和 RISC-V ISA 不变，收益不依赖编译器插桩、运行时 API、OS 分配策略、离线 profile 或新的设备协议；
- 机制能落为有限表项、cache-line metadata、队列、计数器、端口或有界延迟，而不是零代价 oracle；
- 性能因果链落在当前 CPU 的翻译、fetch、cache、DRAM 或后端路径上；
- 能定义正确的 A/B：相同 checkpoint、warmup 后 reset、相同 prefetch/memory 配置，并用统计量解释 IPC 变化。

论文中的数字一律是论文平台、workload 和 baseline 下的结果。除非明确写为“当前 GEM5 原型的硬件下界”，本文不把它们外推为 Kunminghu v3 的 IPC 预期。

### 论文集获取情况

- 给定 ACM 链接及用户提供的 `172.38.8.77:7897` proxy 在 2026-08-05 均返回 HTTP 403，无法读取 proceedings 页面或下载 iTP+xPTP 的全文。
- [Crossref records](https://api.crossref.org/works/10.1145/3669940) 确认该 DOI 是 *Proceedings of the 30th ACM International Conference on Architectural Support for Programming Languages and Operating Systems, Volume 1*，出版日期为 2025-03-30。
- 公开的 [DBLP Volume 1 目录](https://dblp.org/db/conf/asplos/asplos2025-1.html) 给出 72 篇文章及逐篇 DOI；本文先对这 72 个题目全量筛选，再对候选和边界项读取可获得摘要或作者公开版。
- iTP+xPTP 的机制和 +18.9%/+11.4% 来自 OpenAlex 与 Semantic Scholar 提供的一致摘要，属于**摘要级定量证据**。因此论文自己的绝对硬件成本必须标记为 unavailable。

## 主候选：iTP + xPTP

**论文：** Dimitrios Chasapis, Georgios Vavouliotis, Daniel A. Jimenez, Marc Casas, [*Instruction-Aware Cooperative TLB and Cache Replacement Policies*](https://doi.org/10.1145/3669940.3707247).

### 问题和机制

大 server/data-center workload 的指令 footprint 可能同时压迫 I-cache 和 iTLB。iTLB miss 会阻断 fetch，往往比普通 data access 的可隐藏性更差。论文提出两级协同而不是孤立扩大 TLB：

1. **iTP（STLB）：** 在 shared last-level TLB 的替换中优先保留 instruction translation。这会把有限 STLB 容量从 data translation 挪给 instruction translation，预期减少前端的 iTLB miss 和 pipeline stall，但会增加 data page walk。
2. **xPTP（L2C）：** L2 replacement 识别/优先保留 data page walk 所需要的 cache line，补偿 iTP 引入的 data PTW 压力，降低 data PTW 的 L2 miss。
3. **adaptive L2 policy：** 根据虚拟内存子系统压力在 xPTP 和普通 LRU 之间切换，避免对翻译不敏感的 workload 无条件污染 L2。

因果链是：

```text
iTP 保住 instruction translation
  -> iTLB/STLB miss 和 fetch stall 下降
  -> data translation 容量受挤压、data PTW 可能上升
  -> xPTP 保住 data-PTW 的 L2 PTE line
  -> data PTW 的 L2 miss/延迟回落
  -> 只有前端收益大于 data-side 代价时，最终 IPC 才提高
```

这条链是本 feature 的验收条件。只看到 L2 hit 率或只看到 IPC 上升都不足以证明“协同”机制发挥了作用。

### 论文效果、成本和适用边界

| 项目 | 证据 | 结论 |
| --- | --- | --- |
| 单核效果 | 摘要级 | 相对 LRU STLB+L2C 的单核几何平均性能 +18.9%。 |
| SMT 效果 | 摘要级 | SMT co-location 的性能提升为 +11.4%。当前 `kmhv3.py` 默认单核路径不能据此声称 SMT 收益。 |
| 论文绝对 storage/area/energy | 无法核对 | ACM 全文不可访问，摘要未给出表项位宽、面积或功耗；本文不估造论文成本。 |
| workload 条件 | 摘要级 | 目标是大 instruction footprint 的 contemporary server workload。若当前 SPEC checkpoint 的 iTLB miss 很低，feature 的合理结果可能是零收益或负收益。 |
| software/ISA 前提 | 摘要级 | 摘要描述的是 STLB 和 L2C replacement，不依赖新的应用/ISA/OS 接口，符合本文 hardware-only 口径。 |

### 当前 GEM5 的对应关系

| 论文概念 | 当前实现事实 | 影响 |
| --- | --- | --- |
| shared STLB | [`src/arch/riscv/RiscvMMU.py`](../../src/arch/riscv/RiscvMMU.py) 将 ITB/DTB 的 `next_level` 都连到 unified `RiscvTLBL2`。 | 可以把它作为论文 STLB 的最近似映射。 |
| STLB capacity/replacement | [`src/arch/riscv/RiscvTLB.py`](../../src/arch/riscv/RiscvTLB.py) 默认 L2TLB 各层为 L3/L2/L1/SP 各 16、L0 为 128，`l2tlb_line_size=8`；[`src/arch/riscv/tlb.cc`](../../src/arch/riscv/tlb.cc) 以 `lruSeq` 做分层/分组 LRU。 | 不是论文中普通 set-associative LRU STLB。iTP 必须保留现有 page-level partition、8-entry line 与两阶段翻译语义。 |
| instruction source | [`TlbEntry`](../../src/arch/riscv/pagetable.hh) 没有 `is_instruction`/`instruction_seen` 字段。 | iTP 不能仅在 `l2TLBEvictLRU()` 里猜测 victim；需要在 TLB refill path 传播一位 instruction/data 来源信息。 |
| PTW request 标识 | [`Request`](../../src/mem/request.hh) 已有 `PT_WALK`/`isPTWalk()`，但当前 RISC-V walker 的 normal path 在 [`pagetable_walker.cc`](../../src/arch/riscv/pagetable_walker.cc) 用 `Request::PHYSICAL` 创建请求，当前 RISC-V 源码没有设置 `PT_WALK`。 | xPTP 当前无法可靠识别 PTE fill。先补充硬件 sideband/flag，再讨论 cache policy。 |
| L2 replacement hook | [`kmhv3.py`](../../configs/example/kmhv3.py) 对 2 MiB L2 使用 `XSDRRIPRP(mode=2)`；[`xs_drrip_rp.cc`](../../src/mem/cache/replacement_policies/xs_drrip_rp.cc) 的 `touch/reset` 已接收 `PacketPtr`。 | 可在已有 cache request metadata 和 RRIP victim path 上实现 xPTP；不能把 policy 换成 LRU 后直接引用论文数字。 |
| PTW observability | [`WalkerStats`](../../src/arch/riscv/pagetable_walker.hh) 已有 `ptwMemCount`、`ptwMemCycle`、`ptwAvgMemLatency`。 | 可复用为总量指标，但需要补 i/d、L2-hit/miss 和被替换原因的细分统计。 |

### GEM5 原型的硬件成本下界

以下是**当前模型的最小可验证设计**，不是论文报告的实现成本：

| 新状态/逻辑 | 最小行为合同 | 当前配置的 metadata 下界 | 实现/时序风险 |
| --- | --- | --- | --- |
| `instruction_seen` | 每个 shared-L2-TLB translation 保存“曾为 instruction translation 服务”的位；data hit 不得把已确认的 instruction 身份清掉。 | 以默认 `(16 + 16 + 16 + 128 + 16) * 8 = 1536` 个 L2TLB entry 计，1 bit/entry 为 **192 B**。 | 低 storage；需要穿过 refill、compressed entry、prefetch、squash、serialize/unserialize 和 `sfence.vma` 生命周期。 |
| `data_ptw_fill` | L2 line 记录其填充/最近关键访问是否来自 data PTW；只允许 xPTP 在其定义的 class 上改变 insertion/victim priority。 | 2 MiB/64 B = 32768 line，1 bit/line 为 **4 KiB**。 | 低 storage、中等时序：`getVictim()` 已遍历同 set 的 ways，新 class 比较会增加组合选择；应在现有 victim pipeline 内参数化，而不是零延迟假设。 |
| PTW source sideband | walker request 带 `PT_WALK`，并保留 `walk_is_instruction`，以便区分 data PTW 与 instruction PTW。 | request/queue sideband，非 cache SRAM 主体；位宽为常数。 | 中等：所有 normal、two-stage、retry 的 PTW packet 必须一致携带，不能漏掉回放路径。 |
| adaptive controller | 独立的饱和计数器、hysteresis 和 mode bit，输入为 i/d STLB miss、data-PTW L2 miss 等有界事件。 | `c` bits counter + mode bit。 | 低 storage；不能复用 `XSDRRIP` 的 10-bit DRRIP PSEL，因为两者训练目标不同。 |

只计两个 cache/TLB metadata 位时，当前单核 2 MiB 配置的下界约为 **4,288 B + controller state**。这不是面积结论：真实 RTL 还要考虑 tag/data array 布局、比较器、更新端口、跨页/两阶段翻译的 tag 宽度和时序。该数字仅用于防止 GEM5 原型把 metadata 当成零成本。

### 建模合同和分阶段 A/B

1. **Phase 0：只观测，不改策略。** 按 instruction/data 分别统计 L1/STLB hit、miss、eviction 和 PTW；给每个 PTW request 加原始类别，统计其 L2 hit/miss、memory latency、queue/full/retry。若目标 checkpoint 的 instruction STLB miss 没有可观比例，不进入后续阶段。
2. **Phase 1：iTP-only。** 仅改变 shared L2TLB 的 victim ranking；同一现有 L2 page-level bucket 内，优先驱逐 data-only entry，保持原有 LRU 作为同类 tie-break。记录 iTLB benefit 与额外 data PTW，默认关闭。
3. **Phase 2：xPTP-only。** PTW flag/source 完整后，在 `XSDRRIP` 中单独启用 PTE-line priority。比较 `baseline / learned-but-no-policy / xPTP`，证明 L2 data-PTW miss 和 latency 的变化来自 policy 而非标识开销。
4. **Phase 3：combined + adaptive。** 只在 Phase 1 确认 iTP 用 data PTW 代价换到 instruction-side 收益、且 Phase 2 能回收这部分代价时启用。adaptive policy 需显示每个 mode 的周期数、切换次数和控制器饱和情况。

每一阶段固定 commit、checkpoint、warmup、ROI、`kmhv3.py`、core count、prefetcher 与 DRAM ini，只比较 reset 后的最终 stats。对于任何导致 `sfence.vma`、squash、two-stage translation 或 checkpoint restore 出错的版本，先修生命周期而不是减少统计或放宽断言。

建议新增的最小统计量：

- `stlbInstHits/Misses`、`stlbDataHits/Misses`、按 source 的 L2TLB eviction 和 `iTPForcedDataVictim`；
- `instPtw/dataPtw` 的发射数、L2 hit/miss、memory latency、retry/queue-full；
- `xptpPtwFill/Hit/Victim/ProtectedEvict`，以及 baseline policy 下同类事件；
- adaptive controller 的 counter、mode cycle、transition、fallback reason；
- instruction-side miss/stall、L2 demand miss、MSHR occupancy、DRAM request traffic、committed IPC。

验收不是“data PTW 必须下降”。iTP 可能有意提高 data PTW；成功的 combined policy 必须能量化 `iTLB/STLB/fetch` 收益、data-side 回退量及带宽/污染代价，并在最终 ROI IPC 上证明净收益。

## 接近但不纳入当前 CPU feature 的工作

| 论文 | 论文中的 feature、开销和效果 | 为什么不列为当前硬件-only 性能候选 |
| --- | --- | --- |
| [pulse](https://doi.org/10.1145/3669940.3707253) | 在 rack-scale disaggregated memory 的 memory node 部署 pointer-traversal accelerator。作者公开扩展版明确有 iterator interface、把 iterator code 编译到受限 pulse ISA，并使用 SmartNIC/交换机/近内存管线。 | 收益需要 disaggregated memory、近内存 accelerator、网络 routing 和编译/接口适配；不是仅给现有 O3 core 加一项透明硬件 feature。 |
| [MOAT](https://doi.org/10.1145/3669940.3707278) | DDR5 PRAC/ABO RowHammer 防护；摘要报告 **7 B SRAM/bank**，ATH=64 时 SPEC/GAP 平均 **0.27% slowdown**。 | 是硬件-only security mechanism，但在未建 RowHammer mitigation 的当前 DRAM baseline 上不会提升 CPU 性能，反而有控制开销；需要 DRAM bank/refresh/ALERT 模型。 |
| [RANGE-BLOCKS](https://doi.org/10.1145/3669940.3707225) | 为 DSA 提供 key-range synchronization；摘要报告 2 KiB table、128-tile DSA 上 +15x、DRAM traffic -4x、on-chip traffic -70%。 | 适用对象是 dataflow DSA，不是通用 RISC-V O3 核。实现它等于新建 accelerator、命令接口和 workload，而不是当前 CPU/cache 的局部改动。 |
| [Segue & ColorGuard](https://doi.org/10.1145/3669940.3707249) | Segue 通过 x86-64 segmentation 减少 SFI instrumentation；ColorGuard 利用 MPK。摘要给出 Wasm SPEC subset overhead -44.7%、Firefox font rendering overhead -75%。 | 收益前提是 Wasm/SFI compiler instrumentation 和特定 x86 mechanism，且作者讨论多个 toolchain；不满足 RISC-V 既有二进制只改硬件。 |
| [Efficient Lossless Compression of Scientific Floating-Point Data on CPUs and GPUs](https://doi.org/10.1145/3669940.3707280) | 四种 CPU/GPU 可兼容的浮点压缩**软件算法**；摘要举例 RTX 4090 上 >500 GB/s。 | 程序必须调用新算法/库，性能数字不是通用 CPU 微结构收益；硬件压缩器是另一个未在本文证明的设计。 |
| [CRUSH](https://doi.org/10.1145/3669940.3707273) | Dynamatic HLS 的 FU-sharing 编译策略；摘要报告 DSP -12%、FF -15%、优化时间 -90%。 | 作用于 HLS 生成的 dataflow circuit，而不是当前 O3 issue queue；既不是运行期 CPU feature，也不保证 IPC 提升。 |
| AnyKey、ByteFS、EDM、ZRAID、Tela、FleetIO、Fusion 等 | KV SSD、CXL memory-semantic SSD、网络 fabric、RAID/云/存储放置。 | 它们可能包含设备硬件，但收益依赖新的存储/网络/OS 系统部署，不能在现有 CPU cache/DRAM 路径中做同口径 A/B。 |
| GPU/AI/cryptography/quantum/CGRA/SLAM 组 | 例如 NTT/RAG/BatchZK、ARC、Cinnamon、AnA、HDC、NPU、UniZK、RASSM、SuperNoVA。 | 目标为 GPU、NPU、FPGA/ASIC、特定 DSA 或算法硬件协同；将它们放进 GEM5 需要新 accelerator 和对应 workload，不是提高当前 CPU 的局部 feature。 |
| 编译器、runtime、分析、隔离、ML 调度和验证组 | 例如 ClosureX、Exo 2、Faster Chaitin、PartIR、vAttention、Coach、Rethinking Java Performance Analysis、RTL Verification。 | 性能来自 source transformation、runtime/resource policy 或验证方法，无法满足软件不变的范围。 |

## 推荐次序

1. **先做 iTP+xPTP 的 Phase 0 instrumentation。** 这一步会回答当前 checkpoint 是否真的有可优化的 instruction translation pressure，成本最低，也能防止对论文速度up的错误外推。
2. **只在有 instruction-STLB bottleneck 时做 iTP-only。** 先证明“给 instruction translation 腾出空间”真的改善 fetch，而不是只改变 L2TLB victim 计数。
3. **再做 xPTP-only 和 combined。** RISC-V PTW request 的 `PT_WALK`/source 标签是功能前置条件；没有它，任何根据地址猜 PTE 的 L2 policy 都不够可靠。
4. **不建议在此 corpus 内启动第二个 current-CPU feature。** 其余硬件论文要么需要软件/ISA，要么首先需要 accelerator、DDR5 RowHammer 或 CXL/rack-scale memory 平台模型。把这些包装成 cache policy 会失去论文原有性能因果链。

## 参考来源

- [ASPLOS 2025 Volume 1 official DOI](https://doi.org/10.1145/3669940)
- [DBLP Volume 1 table of contents](https://dblp.org/db/conf/asplos/asplos2025-1.html)
- [iTP+xPTP DOI](https://doi.org/10.1145/3669940.3707247)，作者 Chasapis, Vavouliotis, Jimenez, Casas；机制和 +18.9%/+11.4% 为 OpenAlex/Semantic Scholar 摘要级证据。
- [pulse DOI](https://doi.org/10.1145/3669940.3707253)；作者公开扩展版：[arXiv:2305.02388](https://arxiv.org/abs/2305.02388)。
- [MOAT DOI](https://doi.org/10.1145/3669940.3707278)；公开版：[arXiv:2407.09995](https://arxiv.org/abs/2407.09995)。
- [RANGE-BLOCKS DOI](https://doi.org/10.1145/3669940.3707225)、[Segue & ColorGuard DOI](https://doi.org/10.1145/3669940.3707249)、[CRUSH DOI](https://doi.org/10.1145/3669940.3707273)。

**验证状态：** 本次是调研交付，没有修改 GEM5 模型代码，也没有声称编译通过或获得当前 CPU 的 IPC 结果。Markdown、指定源码路径和 DOI 链接语法需要在提交前做本地检查；ACM 全文不可读是 iTP+xPTP 论文绝对成本仍为 unavailable 的原因。
