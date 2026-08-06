# ISCA 2024：面向当前 KMHv3/GEM5 的纯硬件 Feature 调研

> 调研日期：2026-08-05。目标配置是本仓库的 `configs/example/kmhv3.py` 单核/少核
> Kunminghu v3 O3 CPU。论文中的 speedup、流量和面积数字来自论文自己的基线与工作负载，
> 不是当前 GEM5 的性能承诺。

## 结论

严格要求“只改硬件就能生效”后，ISCA 2024 最值得在当前 GEM5 上立项的方向如下：

1. **P0：Constable，安全消除稳定 load 的地址计算和数据访问。** 当前树已有 O3 rename、
   load queue 和 value-prediction 接口；论文的 12.4 KiB/core 状态较小，收益链条直接指向
   load-port、RS 和数据依赖压力。这是第一优先级，但必须先解决 store/snoop、异常和 squash
   的精确失效语义。
2. **P0：Triangel，带采样和效用控制的 temporal prefetcher。** 论文给出 17.6 KiB 专用
   状态和一个可调的 1 MiB Markov-table cache 分区；当前 `QueuedPrefetcher`、组合预取器和
   L2 统计接口可以复用。实现工作量大于普通 stride，但能保留 prefetch 准确率、时效性、带宽
   和污染的完整因果链。
3. **P1：UDP/UFTQ，面向 FDIP 的 utility-driven instruction prefetch。** 论文报告平均
   +3.6%、最高 +16.1%，存储开销 8 KiB；当前有 decoupled BPU/FTQ，但没有论文中的 FDIP
   utility 和错误路径预取队列，需要先确认 block/target 语义映射。
4. **P1：AVM-BTB，自适应借用 I-cache/uop-cache 容量作为多级 BTB。** 论文在 1253 条 trace
   上平均 +18.22%，且不增加 SRAM；但需要 cache data 与 BTB metadata 的动态容量重分配，
   对当前没有 uop cache、只有 block-based BTB 的 GEM5 来说是中高风险前端项目。
5. **P2：Alternate Path Fetch (APF)，在难预测分支的另一条路径上做并行预测、取指、译码和
   部分 rename。** 论文对 aggressive 8-wide core 报告 geomean +5%；它不占用错误路径的
   后端执行资源，但要新增一条前端/部分 rename 通路，建模和恢复风险高。
6. **P2：Alternate Path micro-op Cache Prefetching (UCP)，只为可能的 pipeline refill 预取
   alternate-path uop。** 论文平均 +1.9%--2%，最高 12%，额外 8.95 KiB（带 4 KiB alternate
   indirect predictor 为 12.95 KiB）。当前配置没有 uop cache，因此应在建立 uop-cache 基线后
   再做，而不是直接嫁接到 L1I prefetch。
7. **P2/P3：Twilight/T-LITE neural temporal prefetching。** 论文摘要给出 Twilight 相对
   Voyager 延迟降低 988x、存储缩小 10.8x，混合 irregular benchmark speedup 多 4%；
   T-LITE 相对 Voyager 为 1421x/142x，并比 Triage 高 5.9%。但 Twilight 仍被论文称为
   “not practical”，没有可直接复用的当前 GEM5 neural timing/area 模型，暂缓。
8. **P3 平台专题：DyLeCT 与 Native DRAM Cache。** 两者分别报告硬件压缩内存翻译平均
   +10.25%，以及 Native DRAM Cache 在 SPEC/NPB/GAP 上 +2.8%/+52.5%/+44.2%；它们都要
   新的 memory-controller/DRAM 组织。当前 GEM5 默认 DDR4/传统 DRAM 路径无法做可信的首批
   CPU A/B，只能在扩展平台模型后开展。

风险调整后的推荐顺序不是按论文最大 speedup 排名，而是按“当前 GEM5 能否保留性能因果链、
硬件开销是否有界、结果能否通过同一 checkpoint A/B 验证”排序。第一批建议是
`Constable -> Triangel -> UDP`；AVM-BTB 适合单独的前端容量项目，APF/UCP/Twilight 作为后续
前端研究，DyLeCT/NDC 作为平台项目。

## 论文集和筛选口径

书目入口：[ISCA 2024 DBLP TOC](https://dblp.org/db/conf/isca/isca2024.html)。2026-08-06 核验发现
`10.5555/3744734` 在 `doi.org` 返回 404，因此它不能再被标为 proceedings DOI；本报告以 DBLP 和各篇
IEEE DOI 作为可复核书目入口。DBLP 列出 88 条记录，
主题覆盖 GPU/加速器、PIM/CXL、量子、云服务、可靠性/安全、缓存/预取和 CPU 前端/后端。
本报告首先逐题名筛选，再对 CPU/cache/memory 相关条目读取 Semantic Scholar 摘要和公开作者版：

- [Constable arXiv 2406.18786](https://arxiv.org/abs/2406.18786) 和
  [Triangel arXiv 2406.10627](https://arxiv.org/abs/2406.10627) 可取得全文；
- [Alternate Path micro-op Cache Prefetching 作者版](https://hal.science/hal-04675260)
  可取得全文；
- 其余条目的机制和定量数字来自 IEEE DOI 页面可核对的摘要/语义索引。ACM/IEEE 自动 PDF
  请求在本次调研环境遇到 403/挑战页面，即使使用用户给出的 `172.38.8.77:7897` 也未获得
  受保护全文。因此没有把缺失的面积、端口或表项数字猜成论文事实。

### hardware-only 的严格定义

主候选必须同时满足：

- 不改编译器、二进制 metadata、软件 wrapper、OS/运行时、ISA 或离线 profile；
- 收益由 CPU、BPU、cache、TLB、prefetcher 或 memory controller 中的有限状态、端口、队列、
  延迟和带宽控制产生；
- 可描述失效、squash、coherence、MSHR/队列反压和 cache 污染，不能把“预测正确”当作无限资源；
- 在当前单核/少核 KMHv3 配置中有明确落点，或明确标记为需要新平台的延后项。

“硬件-only”不等于“在 GEM5 中容易写”。论文数字是论文事实；“GEM5 预期”只表示可验证的
假设，必须通过相同 workload、checkpoint、warmup/ROI 和 reset 后 final stats 的 feature-off/on
A/B 得到。

## 决策总表

| 优先级 | 论文/feature | 论文效果与论文硬件代价 | 当前 GEM5 落点 | 结论 |
| --- | --- | --- | --- | --- |
| P0 | **Constable** | 平均 +5.1%（最高 31.2%）；核心动态功耗 -3.4%；2-way SMT +8.8%；12.4 KiB/core（SLD 7.9、RMT 0.4、AMT 4.0 KiB；另有 32-entry xPRF） | `src/cpu/valuepred/`、rename/LSQ/commit、snoop/store invalidation | 首批后端项目，收益和状态规模平衡最好 |
| P0 | **Triangel** | 相对 stride-only +26.4% geomean，Triage +9.3%；内存流量仅 +10%（Triage +28%）；17.6 KiB side state + 最大 1 MiB L3 Markov partition，metadata hit 25 cycles | `src/mem/cache/prefetch/Prefetcher.py`、`QueuedPrefetcher`、L2/L3 cache partition | 首批预取项目；必须显式建模容量占用和带宽 |
| P1 | **UDP/UFTQ** | 10 个 datacenter workload 平均 +3.6%、最高 +16.1%；存储 8 KiB，摘要称 moderate hardware modification | `src/cpu/pred/btb/`、FTQ/FSQ、L1I prefetch queue | 有价值但依赖 FDIP/utility 接口，先做受限原型 |
| P1 | **AVM-BTB** | 1253 traces 平均 +18.22%，功耗 -2.77%；不增加额外 SRAM，合理实现工作量 | Decoupled BPU/BTB + I-cache/uop-cache metadata 共享 | 潜在收益最高之一，但容量借用和时序风险高 |
| P2 | **Alternate Path Fetch** | aggressive 8-wide O3 geomean +5% | `fetch/decode/rename`、FTQ/FSQ、branch recovery | 需双前端和 partial-rename，后续研究 |
| P2 | **UCP** | 平均 +1.9%--2%，最高 12%；8.95 KiB，带 4 KiB alternate ITTAGE 为 12.95 KiB；约 0.19 KiB uop-cache MSHR、0.25 KiB L1I PQ | 当前无 uop cache；需新 uop cache、MSHR 和 alternate-path generator | 先建立 uop-cache baseline，再做 |
| P2/P3 | **Twilight/T-LITE** | Twilight 比 Voyager 延迟低 988x、存储小 10.8x，混合 benchmark 多 4%；T-LITE 为 1421x/142x，并比 Triage 高 5.9% | L1D/L2 temporal prefetch path + neural timing model | 论文仍称 Twilight 不实用，暂缓 |
| P3 | **DyLeCT** | 相对 prior art 平均 +10.25%；128 KiB translation cache 主要存 2-bit short entries，可达约 2 GiB translation reach | 新 compressed-memory translation、memory controller | 平台级，当前模型没有对应路径 |
| P3 | **Native DRAM Cache** | SPEC/NPB/GAP +2.8%/+52.5%/+44.2%，最高 8.4%/140.6%/85.5% | 新 DRAM subarray tag match/way select、DRAM interface | 需要 DRAM 芯片/控制器模型，暂不进入核心 backlog |

## 主候选详情

### P0：Constable —— 消除稳定 load 的完整执行

论文：[Constable DOI 10.1109/ISCA59077.2024.00017](https://doi.org/10.1109/ISCA59077.2024.00017)。

**Feature。** 对同一 PC 反复从同一地址读到同一值的 load，Stable Load Detector (SLD) 用
PC-indexed 表学习稳定性。当置信度达到阈值时，后续实例在 rename 阶段直接变成从小型 xPRF
取值的 move，跳过地址生成、RS 占用、AGU/load port 和 cache 数据访问。Register Monitor Table
(RMT) 监视源架构寄存器写入，Address Monitor Table (AMT) 监视相同物理地址的 store/snoop；任一
变化都会撤销 `can_eliminate`。非投机的真实 load 完成后才训练，错误的 in-flight 消除由现有
memory-disambiguation 触发 flush/replay。

**论文效果和开销。** 论文在 90 个 workload（含 SPEC CPU2017）上相对已经包含 memory
renaming、move/zero/constant/branch folding 的强基线，平均性能 +5.1%（最高 31.2%），核心
动态功耗 -3.4%；2-way SMT 平均 +8.8%。与 EVES load-value predictor 组合时，在无 SMT/2-way
SMT 额外分别 +3.7%/+7.8%。每核存储 12.4 KiB：SLD 512 entries/7.9 KiB、RMT 0.4 KiB、
AMT 256 entries/4.0 KiB；论文另使用 32-entry xPRF（作为比较方案共用的小型 value file）。
论文的 14 nm CACTI 估计为 SLD 0.211 mm2、RMT 0.004 mm2、AMT 0.017 mm2；SLD 为 3R/2W，
AMT 为 1R/1W，端口和失效广播是主要时序风险。

**当前 GEM5 落点。** `src/cpu/valuepred/ValuePredictor.py` 已有 `MemoryRenaming`，但其当前
实现主要维护 store-load table，stage-1 `predict()` 不注入 speculative value；不能把它误当作
Constable 已经存在。推荐在 `src/cpu/o3/rename.*` 增加 stable-load classification 和 xPRF
绑定，在 `src/cpu/o3/lsq.*` 保留消除 load 的物理地址并参与 RAW/依赖检查，在 `commit.*` 以真实
完成更新 SLD，并从 store/缓存一致性通知调用 AMT 失效。默认 `enable_constable=false`，保持
旧 value prediction 路径不变。

**最小建模合同与验证。** 必须细建 source-arch-reg 版本、物理地址匹配、in-flight store
disambiguation、异常/MMIO/atomic 排除、squash 后不能留下 xPRF 引用，以及 xPRF 满时 fallback。
可先忽略 SRAM gate-level，使用参数化读写延迟/端口。统计 `stable_seen/qualified/eliminated`、
`register_invalidate/address_invalidate/snoop_invalidate`、xPRF full、load-port/RS 使用量、
replay/flush 和真正节省的 L1D 请求。A/B 顺序为：baseline -> 只学习不消除 -> 消除但关闭
store/snoop（仅用于暴露错误）-> 完整安全版；只有完整版功能一致且资源/请求统计解释 IPC，才可归因。

### P0：Triangel —— 采样驱动的 temporal prefetch

论文：[Triangel DOI 10.1109/ISCA59077.2024.00090](https://doi.org/10.1109/ISCA59077.2024.00090)，
[作者公开版](https://arxiv.org/abs/2406.10627)。

**Feature。** Triangel 在 Triage 式 per-PC training table 和 L3 分区 Markov table 上增加四类
有限硬件：History Sampler 识别会在容量内重复的长模式；Second-Chance Sampler 捕获首次判断过早
的模式；ReuseConf/BasePatternConf/HighPatternConf 控制是否训练、是否发出以及 degree/lookahead；
Metadata Reuse Buffer (MRB) 避免高 degree 链式查表的重复访问；Set Dueller 在 Markov metadata
和 L3 data 之间动态分配容量。默认高置信度可使用 degree-4、lookahead-2，但坏模式会被抑制。

**论文效果和开销。** 论文用 gem5 v23、7 个最不规则 SPEC CPU2006 workload，报告相对 stride-only
baseline 的 26.4% geomean speedup（Triage 为 9.3%，Triage degree-4 为 14.2%），DRAM traffic
仅高 10%（Triage 高 28%）；Set-Dueller 版的估计 DRAM+L3 动态能量为 baseline 的 14% 额外，
而 Triage 为 36%。专用 side state 共 17.6 KiB：Training Table 7808 B、History Sampler 6080 B、
Second-Chance 584 B、MRB 1472 B、Set Dueller 2106 B。另需最多 1 MiB/196608-entry 的 Markov
partition（从 LLC data 容量中借用，而不是“免费”新增 SRAM），每次 Markov lookup 建模为 25 cycles。
论文明确显示去掉 Set Dueller 或放宽 degree 会增加带宽和污染；Graph500 无 temporal correlation
时应保持不发出或可能 slowdown。

**当前 GEM5 落点。** 当前 `src/mem/cache/prefetch/Prefetcher.py` 已有 `QueuedPrefetcher`、
`XSCompositePrefetcher` 的 region/PHT/filter/temporal 子模块和有界 queue；L2 使用
`L2CompositeWithWorkerPrefetcher`。建议新增独立 `TriangelPrefetcher`，不要先改写 KMH-aligned
SMS 训练规则。Markov entries 应作为 cache-resident metadata，显式占用可配置的 L3/L2 way/容量；
prefetcher queue、MSHR、tag/data port 和 25-cycle lookup 要计入时序。

**最小建模合同与验证。** 必须建模 per-PC training/History/SCS/MRB/Set-Duell 状态、固定 lookahead
和 degree、训练/发射阈值、metadata eviction、queue drop、late/useful/useless 及 bandwidth。
先比较 no temporal PF、Triage-like degree-1、Triangel-NoMRB、完整 Triangel 四组；对当前 KMHv3
必须报告 L1/L2/L3 demand MPKI、coverage、accuracy、late、prefetch pollution、Markov lookup
cycles、MSHR occupancy、DRAM traffic 和 ROI IPC。不能只复制论文的 26.4%，因为当前 L2/L3 容量、
预取组合和 checkpoint 与论文不同。

### P1：UDP/UFTQ —— utility-driven FDIP instruction prefetch

论文：[UDP DOI 10.1109/ISCA59077.2024.00089](https://doi.org/10.1109/ISCA59077.2024.00089)。

**Feature。** UDP 观察 datacenter 大代码 footprint 中，FDIP 的错误/过晚预取造成前端停顿。它
用 utility/应用阶段反馈筛掉低收益的 instruction prefetch，同时保留正确路径和可利用的 wrong-path
预取；UFTQ 为这些请求提供独立的 utility-aware fetch queue，使准确率提高而不牺牲 timeliness。
该机制不需要程序 hint 或编译器改写。

**论文效果和开销。** 论文在 10 个 datacenter workload 上报告平均 IPC +3.6%、最高 +16.1%，并
称只需 moderate hardware modification、存储开销 8 KiB。摘要未公开 8 KiB 的表项拆分和端口，
因此这里不补猜；UFTQ 的深度、每周期发射数、错误路径带宽和 instruction-cache pollution 必须
作为有界参数。

**当前 GEM5 落点。** `src/cpu/pred/btb/decoupled_bpred.*` 已有 FTQ/FSQ、分级 BTB/TAGE 及
可见 `numDelay`；`src/mem/cache/prefetch/Prefetcher.py` 有 `QueuedPrefetcher`。当前树没有
明确 FDIP/UFTQ 模块，推荐先在 decoupled BPU 旁增加 `UdpUtility` 状态和独立有界 instruction
prefetch queue，并在 L1I fill 中携带 source/utility metadata。不要以普通 L1I next-line PF 或
把 wrong-path 当作正确路径来替代论文语义。

**验证。** 先做 FDIP baseline、只加 utility admission、再加 UFTQ/wrong-path 三组；统计
instruction PF accuracy/coverage/late/unused、UFTQ enqueue/drop、L1I MPKI、FTQ empty、wrong-path
带宽和前端 stall cycles。若当前 workload 没有 datacenter 大 footprint，收益很小或为负是合理结果。

### P1：AVM-BTB —— 自适应、虚拟化的多级 BTB

论文：[AVM-BTB DOI 10.1109/ISCA59077.2024.00012](https://doi.org/10.1109/ISCA59077.2024.00012)。

**Feature。** AVM-BTB 根据 BTB error rate 和 effective access 动态判断前端是否 BTB-bound，
把 I-cache/uop-cache 中暂时低效的 instruction data capacity 借给 BTB metadata，需求降低时归还。
它强调应用甚至同一应用不同阶段的 BTB 需求不同，因此无需固定扩大 SRAM；并结合 FDIP 缓解增大
BTB 后 instruction-cache reach 下降的问题。

**论文效果和开销。** 论文评估 1253 条 trace：相对 baseline 平均性能 +18.22%、功耗 -2.77%，
相对五种 state-of-the-art 方案 IPC 还高 6.26%--18.26%。摘要称不消耗额外 SRAM、实现工作量合理；
没有公开可核对的绝对控制状态/端口面积，故工程上应把 cache/BTB mux、metadata/data invalidation、
容量切换延迟和错误率计数器列为开销，而不能写成“零面积”。

**当前 GEM5 落点。** 当前 KMHv3 的 `DecoupledBPUWithBTB` 在 `src/cpu/pred/btb/`，64-entry
FTQ/FSQ，BTBTAGE/TAGE/MGSC 分级；`configs/example/kmhv3.py` 的 L1I 为 64 KiB，且树中没有
uop cache。第一版可以只做“L1I way/line metadata 借用给 BTB”的固定窗口，先不实现 uop-cache
共享；需要在 cache tag/data 和 BPU 之间增加 ownership、回收和 squash-safe 的 sideband。

**验证。** baseline、固定 BTB 扩容、AVM-BTB 反馈三组必须在相同 cache 容量下比较；统计 BTB
hit/miss/target miss、预测器 effective access、借用/归还次数、L1I miss/eviction、FTQ empty、
branch MPKI、前端 stall 和 SRAM access proxy。若只增加 BTB 容量就获益，不能把收益归因给动态
virtualization；若 cache reach 损失抵消 BTB 收益，应保留该结果。

### P2：Alternate Path Fetch (APF) —— 难预测分支的并行部分前端

论文：[APF DOI 10.1109/ISCA59077.2024.00091](https://doi.org/10.1109/ISCA59077.2024.00091)。

APF 把前端拆成 regular path 和 alternate path 两条并行管线。alternate path 对 hard-to-predict
conditional branch 进行 branch-predict、fetch、decode 和 partial rename，只为错误路径之后的
可能 refill 准备指令，不进入完整执行/提交，因此比完整错误路径执行更省后端资源。论文在 aggressive
8-wide O3 core 上报告 geomean +5%，收益来自缩短 misprediction pipeline refill。

论文摘要没有给绝对 SRAM/面积数字；可确认的硬件代价是重复的预测/取指/译码、alternate-path
状态、部分 rename 资源、两个路径的 arbitration 和 recovery metadata。当前 `fetch.cc`、
`decode.cc`、`rename.cc`、FTQ/FSQ 和 squash 逻辑可作为落点，但需要新的 dual-path scheduling
和 checkpoint/restore；不能只把 fetch width 加倍模拟 APF。建议在 UDP/AVM-BTB 之后开展。

### P2：Alternate Path micro-op Cache Prefetching (UCP)

论文：[UCP DOI 10.1109/ISCA59077.2024.00092](https://doi.org/10.1109/ISCA59077.2024.00092)，
[作者公开版](https://hal.science/hal-04675260)。

UCP 用 branch predictor confidence 识别可能误预测的 H2P branch，只沿预测相反的 alternate
path 预取关键 uop，目标是缩短 refill 而不是把整个 wrong path 填入 uop cache。论文平均 speedup
为 1.9%--2%，最高 12%（CVP-1 子集）；无 alternate indirect predictor 时额外存储 8.95 KiB，
加入 4 KiB ITTAGE 后为 12.95 KiB，其中包含 32-entry uop-cache MSHR 0.19 KiB、L1I PQ 0.25 KiB、
decode queue 0.12 KiB 和 Alt-RAS 0.06 KiB。平均约 10 条 cache line/alternate path、prefetch
accuracy 67.7%；过长 threshold 会造成 uop-cache thrashing，存在约 -1.3% 至 -1.4% 的回退。

当前 KMHv3 没有 uop-cache SimObject 或 decoder/uop-cache mode；因此首先要建立固定容量、命中
延迟、切换 penalty 和 invalidation 的 uop-cache baseline。之后再把 H2P/alternate-path generator
挂到 `src/cpu/pred/btb/` 和 `src/cpu/o3/fetch.*`，而不是把论文数字映射成普通 L1I prefetch。

### P2/P3：Twilight/T-LITE —— 可部署方向的 neural temporal prefetch

论文：[A New Formulation DOI 10.1109/ISCA59077.2024.00088](https://doi.org/10.1109/ISCA59077.2024.00088)。

工作把每个地址“任意 successor”的高维预测改成少量 successor 的新地址抽象，并据此构建
Twilight neural temporal prefetcher。摘要报告 Twilight 相对 Voyager 延迟降低 988x、存储缩小
10.8x，混合 irregular SPEC06/SPEC17/GAP speedup 多 4%，并能预测训练集之外的新相关。更小的
T-LITE 可跨程序运行：相对 Voyager 延迟降低 1421x、存储缩小 142x，性能匹配 Voyager，并比
实用 Triage 高 5.9%。摘要没有绝对 KiB/面积；论文仍明确指出 Twilight 本身尚不 practical。

在 GEM5 中实现需要固定宽度/延迟的 neural inference pipeline、训练/更新能量代理和有限地址
抽象表，并接入现有 `QueuedPrefetcher` 的 MSHR/带宽/过滤；不能用 Python/离线 oracle 直接给地址。
由于当前树已有 temporal、BOP、CMC 等组合预取，先做 Triangel/Triage baseline，再考虑 T-LITE
的有限表版本；完整 Twilight 属于 P3。

## 平台级延后项

### DyLeCT：硬件压缩内存的动态短翻译

论文：[DyLeCT DOI 10.1109/ISCA59077.2024.00085](https://doi.org/10.1109/ISCA59077.2024.00085)。
压缩内存会在 memory controller 增加一层地址翻译；DyLeCT 将 hot page 迁移到可用少量 bit 编码
的 DRAM 位置，hot entry 可缩到 2 bit，冷页保留 full-length translation。摘要报告相对 prior art
平均性能 +10.25%，示例为 128 KiB translation cache 达约 2 GiB translation reach。它是纯硬件
控制，但当前 GEM5 没有 compressed-memory translation/cache 或 page migration 带宽模型；需要
先扩展 `src/mem/mem_ctrl.*`、`src/mem/dram_interface.*` 和压缩内存后再做，不能在现有 TLB
计数器上虚增命中。

### Native DRAM Cache：DRAM 内 tag match/way selection

论文：[NDC DOI 10.1109/ISCA59077.2024.00086](https://doi.org/10.1109/ISCA59077.2024.00086)。
Native DRAM Cache 在 DRAM subarray 内做 tag matching 和 way selection，复用 precharge transistor，
把 DRAM 变成高容量 LLC。摘要报告 SPEC/NPB/GAP 相对现有 DRAM-cache 方案分别 +2.8%/+52.5%/+44.2%，
最高 +8.4%/+140.6%/+85.5%。摘要没有绝对面积/位数；硬件代价是新的 DRAM array/peripheral、tag/data
协议和控制器时序。当前 `src/mem/dram_interface.*`、`src/mem/mem_ctrl.*` 与默认 DRAMsim3 DDR4
模型不提供 subarray tag match，因此列为平台项目。

## 不纳入首批的论文

| 论文 | 原因 |
| --- | --- |
| `(MC)^2: Lazy MemCopy at the Memory Controller` ([DOI](https://doi.org/10.1109/ISCA59077.2024.00084)) | 机制本身有 memory-controller tracking，但论文明确需要新的 ISA instruction 和 software wrapper；因此不满足“只改硬件”。摘要报告 Protobuf +43%、Linux COW copy latency 低 250x，不能把这些数字用于硬件-only 排名。 |
| `Alternate Path Fetch` 之外的完整错误路径执行方案 | 需要大量 speculative backend/rename 资源或程序边界；APF 已是只保留 partial rename 的较轻版本，其他方案不适合当前首批。 |
| `The Maya Cache` ([DOI](https://doi.org/10.1109/ISCA59077.2024.00013)) | 主要目标是安全和面积：16 MiB secure baseline 上 area -28.11%、static power -5.46%，不是固定容量 CPU 的性能提升；当前默认是每核 2 MiB L2 而非共享 fully-associative LLC。 |
| `PrIDE` ([DOI](https://doi.org/10.1109/ISCA59077.2024.00087)) | 4-entry/10-byte in-DRAM tracker 是安全机制；与 RFM 配合仍有 1.6% slowdown，不能作为当前 CPU 性能 feature。 |
| `Counter-light Memory Encryption` ([DOI](https://doi.org/10.1109/ISCA59077.2024.00058)) | 在已有内存加密前提下减少加密开销（摘要称达到无加密性能的 98%），但当前 GEM5 没有内存加密路径；属于安全平台扩展，不是裸 CPU 加速。 |
| `SmartOClock`、Perspective、Derm、EcoFaaS 及各类 accelerator/GPU/PIM/量子论文 | 依赖云端功耗/OS/运行时、编译器或专用加速器/存储，不满足当前 CPU 的 hardware-only 范围。 |

## 建议的 GEM5 立项顺序

### 阶段 1：后端和数据预取

1. **Constable 原型**：先扩展 SLD/RMT/AMT 和 xPRF，但只对 demand、非 MMIO/atomic/load-resolved
   指令启用；加入 feature-off/on correctness check 和 `stable_*` stats。
2. **Triangel 原型**：从独立 prefetcher 开始，配置 17.6 KiB side state、可调 256 KiB/1 MiB
   Markov partition 和 25-cycle metadata latency；用同一 checkpoint 比较 coverage/accuracy/
   late/traffic/IPC。

### 阶段 2：前端预测和指令预取

3. **UDP**：在现有 FTQ/FSQ 上加入有限 utility/UFTQ，首先禁止 wrong-path 发射，只验证 utility
   admission；再打开 wrong-path instruction prefetch。
4. **AVM-BTB**：先做固定 cache-way ownership 和可观测的 BTB error-rate feedback，再实现动态
   borrow/return；避免一开始改变所有 L1I replacement。

### 阶段 3：结构性前端/平台

5. **APF/UCP**：只有在建立 dual-path 或 uop-cache baseline 后，才评价其额外状态和 refill
   latency；所有 alternate-path state 必须 checkpoint/squash-safe。
6. **Twilight/DyLeCT/NDC**：分别作为 neural prefetch、compressed-memory 和 DRAM architecture
   专题，不与核心 CPU A/B 混跑。

每个阶段都应使用相同 checkpoint、相同 warmup/ROI，并在统计 reset 后比较最终 `simTicks`/IPC。
预取项目至少报告 `useful/late/useless`、MSHR/queue occupancy、带宽和污染；前端项目报告
BTB/branch MPKI、FTQ empty、L1I MPKI 和 stall；Constable 报告消除数量、失效/恢复和 load-port/RS
占用。若 intervention coverage 很低，应把“论文潜在收益高”与“当前 workload 没有收益”分开。

## 参考来源

- Proceedings/目录：[DBLP ISCA 2024](https://dblp.org/db/conf/isca/isca2024.html)。原稿使用的
  `10.5555/3744734` 在 2026-08-06 经 `doi.org` 核验返回 404，已不再作为 DOI 证据。
- [Constable DOI](https://doi.org/10.1109/ISCA59077.2024.00017)，
  [arXiv 2406.18786](https://arxiv.org/abs/2406.18786)。
- [AVM-BTB DOI](https://doi.org/10.1109/ISCA59077.2024.00012)。
- [UDP DOI](https://doi.org/10.1109/ISCA59077.2024.00089)。
- [Triangel DOI](https://doi.org/10.1109/ISCA59077.2024.00090)，
  [arXiv 2406.10627](https://arxiv.org/abs/2406.10627)。
- [APF DOI](https://doi.org/10.1109/ISCA59077.2024.00091)。
- [UCP DOI](https://doi.org/10.1109/ISCA59077.2024.00092)，
  [HAL 作者版](https://hal.science/hal-04675260)。
- [A New Formulation of Neural Data Prefetching DOI](https://doi.org/10.1109/ISCA59077.2024.00088)。
- [DyLeCT DOI](https://doi.org/10.1109/ISCA59077.2024.00085)。
- [Native DRAM Cache DOI](https://doi.org/10.1109/ISCA59077.2024.00086)。

## 当前树映射的关键入口

- `configs/example/kmhv3.py`：KMHv3 的 fetch/rename/ROB/LSQ、BPU 和 cache 参数。
- `src/cpu/o3/rename.*`、`src/cpu/o3/lsq.*`、`src/cpu/o3/commit.*`：Constable 的分类、消除、
  失效和恢复。
- `src/cpu/valuepred/ValuePredictor.py`、`src/cpu/valuepred/memory_renaming.*`：现有 value/
  memory-renaming 接口；当前 MemoryRenaming 不等于 Constable。
- `src/cpu/pred/btb/decoupled_bpred.*`、`src/cpu/pred/BranchPredictor.py`：UDP、AVM-BTB、APF、
  UCP 的前端/FTQ/延迟落点。
- `src/mem/cache/prefetch/Prefetcher.py`、`configs/common/PrefetcherConfig.py`：Triangel、
  Twilight 的 queue/filter/composite 预取落点。
- `src/mem/dram_interface.*`、`src/mem/mem_ctrl.*`：DyLeCT/NDC 所需但当前缺失的 DRAM
  控制器扩展边界。
