# MICRO 2024：面向当前 KMHv3/GEM5 的纯硬件性能 Feature 调研

> 调研日期：2026-08-05。目标是从 MICRO 2024 的 123 个官方日程条目中，筛选能在本仓库
> `configs/example/kmhv3.py` 的 O3 CPU、classic cache 和单核/少核 checkpoint 工作流上
> 通过硬件机制获得性能收益的方向。本文不是对所有处理器的论文排名，也不把论文平台的
> 加速比当作当前 GEM5 的预期 IPC。

## 结论

按“纯硬件、当前代码可接入、收益能被 A/B 统计解释、硬件开销可控”的风险调整顺序，建议：

1. **P0-A：ENTROPyINDEX 动态 cache indexing。** 先放在物理索引的 L2 做有限状态原型；
   当前 GEM5 已有 `SetAssociative::extractSet()` 和独立 indexing policy，改动局部。论文在
   有预取时平均 IPC +1.42%，无预取时 +3.39%，但高收益是 workload-specific。
2. **P0-B：UBS（Uneven Block Size）指令缓存。** 当前 64 KB、2-way、64 B line 的 L1I
   正好提供清晰 baseline；论文显示约 32 KB 存储接近传统 64 KB I-cache 的效果。真实变长
   line 会触及 tags、refill、Fetch 对齐和跨 line 请求，建议先做 16/32/64 B 的有限集合。
3. **P1-A：LLBP（Last-Level Branch Predictor）。** 论文用额外 512 KB backing 给 64 KB
   TAGE-SC-L，平均 MPKI -8.9%。收益潜力大，但当前树的 `BTBTAGE`/MGSC 不是论文 baseline，
   且高容量表、context 预测和 metadata prefetch 会增加面积、端口和 timing 风险。
4. **P1-B：Timely branch precomputation。** 论文报告相对 aggressive OoO baseline +10.1%，
   并声称复用 core execution resources 和 early-flush path；但是摘要没有面积/位宽，也没有
   证明不需要编译器生成的 precomputation 信息。先做 binary-independent 的硬件可行性审计，
   不应直接宣称已满足 hardware-only。
5. **P1-C：FSDetect/FSLite false-sharing repair（多核条件项）。** 对出现 false sharing 的
   多线程程序平均 1.39x；单核 checkpoint 不会触发收益。需要在一致性状态机增加有限 coherence-
   miss 计数、privatization 和 LLC byte merge，适合在多核模型稳定后研究。
6. **P2：Self-Managing DRAM（SMD）。** 论文报告 DRAM 芯片面积 +1.1%、延迟开销为 row
   activation 的 0.4%，20 个四核 memory-intensive workload 平均 +4.1%。这是纯硬件/DRAM
   接口机制，但当前默认 `DRAMsim3` 配置是 XiangShan DDR4，需先扩展内存设备模型。

  **第一轮实际启动顺序建议为 ENTROPyINDEX -> UBS -> LLBP。** Timely、FSLite 和 SMD 分别
  受 binary-independent 约束、多核一致性和 DRAM 设备模型限制，不应与前三项混成一次大改动。

## 范围与证据边界

### Hardware-only 的严格判定

纳入主候选必须同时满足：

- 不改编译器、binary、ISA、OS 页表/分配器、运行时线程或输入 metadata；
- 性能决策由 CPU、cache、TLB、预取器、coherence 或 memory device 中的有限硬件状态完成；
- 可以明确 table/queue/port/latency/backpressure，不能把“预测正确”当成零成本黑盒；
- 能在当前 GEM5 中保留 `workload event -> state -> resource contention/latency -> stats` 的
  因果链，并能用相同 checkpoint 做 A/B。

“纯硬件”不代表“对当前 CPU 立即有效”。论文若依赖 x86-TSO、GPU、OS 介入、特殊 DRAM 标准、
many-core 拓扑或编译器生成提示，会在本文中标为条件项或排除项。

### 论文目录和可用资料

- [MICRO 2024 官方主程序](https://microarch.org/micro57/program/)：列出 123 个论文/海报条目，
  用于题名、作者和 session 核对。
- 用户提供的 [ACM proceedings catalog](https://dl.acm.org/doi/proceedings/10.5555/979-8-3503-5057-9)。
  该 identifier 在 2026-08-06 经 `doi.org` 核验返回 404，不作为 DOI 书目证据；ACM 页面在本次
  调研中返回 Cloudflare 403，因此不声称取得了受保护全文。
- 论文机制和定量结果通过 [OpenAlex DOI records](https://api.openalex.org/works/https://doi.org/10.1109/MICRO61859.2024.00041)、
  [Semantic Scholar DOI records](https://api.semanticscholar.org/graph/v1/paper/DOI:10.1109/MICRO61859.2024.00042)
  等公开摘要索引交叉核对；论文链接统一保留 IEEE DOI。
- 当前 GEM5 判断来自本 checkout 的源码，关键入口为 `configs/example/kmhv3.py`、
  `configs/common/Caches.py`、`src/cpu/pred/`、`src/cpu/o3/issue_queue.*`、
  `src/mem/cache/tags/indexing_policies/` 和 `configs/common/xiangshan.py`。

证据等级：**A**=官方日程/DOI 身份，**B**=公开摘要中的效果或开销，**C**=当前 GEM5 源码事实，
**E**=本文的工程估算/验证计划。B 级数字只保持论文原始基线和 workload 口径；E 级内容不是已测得
的面积或 IPC。

## 决策总表

| 优先级 | 论文/机制 | Hardware-only 判定 | 论文效果（原口径） | 论文硬件开销证据 | 当前 GEM5 判断 |
| --- | --- | --- | --- | --- | --- |
| P0-A | [Customizing Cache Indexing Through Entropy Estimation](https://doi.org/10.1109/MICRO61859.2024.00041)（ENTROPyINDEX） | 是 | SPEC06/17、PARSEC、GAP 无预取 geomean IPC **+3.39%**，最高 **+52.2%**；有预取 **+1.42%** | 摘要称 minimal computational cost，未给绝对面积 | 物理索引 L2 可复用 `extractSet()`；先避开 VIPT 虚拟地址约束 |
| P0-B | [Weeding out Front-End Stalls with Uneven Block Size Instruction Cache](https://doi.org/10.1109/MICRO61859.2024.00102)（UBS） | 是 | 平均 storage efficiency **+32 个百分点**；相同预算可容纳超过 2 倍 blocks；约 32 KB 存储接近传统 64 KB I-cache | 摘要未给绝对面积；成本是多 line size tags/refill/Fetch 逻辑 | 当前 L1I 64 KB、2-way、64 B；实现面局部但时序/line 语义风险高 |
| P1-A | [The Last-Level Branch Predictor](https://doi.org/10.1109/MICRO61859.2024.00042)（LLBP） | 是 | 512 KB backing + 64 KB TAGE-SC-L，MPKI **-0.5% 至 -25.9%**，平均 **-8.9%** | 明确 512 KB backing；另有小型 in-core context/metadata buffer，未给总 area | 当前是 `BTBTAGE`/MGSC，需新 context predictor、metadata backing 和 prefetch path |
| P1-B | [Timely, Efficient, and Accurate Branch Precomputation](https://doi.org/10.1109/MICRO61859.2024.00043)（TEA） | **待确认**：摘要未证明无 compiler/binary support | aggressive OoO baseline **+10.1%** | 摘要只称复用 core execution resources，未给表项、面积或能耗 | Fetch/IEW/ROB 没有 precomputation thread 接口；先做硬件-only 可行性闸门 |
| P1-C | [Leveraging Cache Coherence to Detect and Repair False Sharing On-the-fly](https://doi.org/10.1109/MICRO61859.2024.00066)（FSDetect/FSLite） | 是，多核条件 | 受 false sharing 影响应用平均 **1.39x**；FSDetect 识别开销 negligible | 每 cache block coherence-miss 计数；FSLite byte-level LLC merge；摘要称 minimal area increase | 单核无收益；多核需 coherence state、privatization、LLC byte merge |
| P2 | [Self-Managing DRAM](https://doi.org/10.1109/MICRO61859.2024.00074)（SMD） | 是，但属于 DRAM device | 20 个四核 memory-intensive workload 平均 **+4.1%** | DRAM area **+1.1%/45.5 mm²**，latency **0.4% row activation**，不增 DDRx pins | 当前默认 DRAMsim3 XiangShan DDR4 无 autonomous reject/maintenance；需扩展 device model |

## P0-A：ENTROPyINDEX 动态 Cache Indexing

### 论文概述

ENTROPyINDEX 观察地址位在连续 cache miss 中的变化频率，用有限周期窗口估计每一位的 entropy，
动态选择变化最多的地址位作为 set index，以改善访问分布、减少 set hotspot。论文在 SPEC 2006、
SPEC 2017、PARSEC 3.0 和 GAP 上相对 power-of-two modulo baseline 报告：无预取 geomean IPC
**+3.39%**（最高 **+52.2%**）；有硬件预取时 **+1.42%**（最高 **+30.1%**）。非均匀 workload
无/有预取分别为 **+5.58%/+2.08%**。这些数字来自论文自己的 cache、index function 和 workload
配置，不能直接换算为 KMHv3 IPC。

### 硬件成本与可实现性

摘要只给 “minimal computational cost”，没有 SRAM bit 或综合面积。可审计的最小硬件状态为：

- 每个候选物理地址位一个小的变化计数器（`entropy_window`、`counter_bits` 参数）；
- 上一个 miss 地址或其有限摘要；
- top-k 选择器和当前 index-bit mask；
- epoch 计数器、更新端口和在切换期间保持旧 mask 的控制位。

若以 48-bit physical address、4-bit saturating counter、每 epoch 选择 9 个 set bits 为例，计数器和
mask 只有数百 bit 量级；真正的代价在周期性比较器、index mux、tag/data SRAM 的 bank 访问时序，
需要用综合或时序模型确认，不能把这个数量级当成论文面积结果。更新应为每 miss O(1)，每个固定 epoch
只做 O(address bits × k) 的有限选择。

### GEM5 落点和限制

`src/mem/cache/tags/indexing_policies/set_associative.cc::SetAssociative::extractSet()` 当前按
固定位段生成 set。可以新增 `EntropyIndexing` policy，在 **physically indexed L2** 上选择 mask，
把 `epoch` 更新放在 miss 观察路径；`configs/example/kmhv3.py` 的 classic L2 为 2 MB、8-way、
64 B line、4096 sets（inner sliced path 为每 slice 512 KB/1024 sets），适合做容量公平 A/B。

不要第一版直接改 L1D：当前 `L1_DCache` 的 tags 是 `VIPTSetAssoc`，动态使用高位 physical bits
会与 translation timing、alias bits 和请求重试耦合；若 index bits 只取 page offset，则无法复现论文
“选择高 entropy physical address bits”的核心收益。

### 最小建模合同与验证

- **事件：** demand/prefetch miss、地址位变化、epoch 到期、index-mask 切换。
- **资源：** set mapping、tag lookup、bank conflict、MSHR、prefetch queue；保留原 cache latency。
- **stats：** set occupancy/热点、conflict miss、capacity miss、mask changes、entropy counters、
  prefetch useful/late/unused、demand MPKI、L2 latency 和 IPC。
- **A/B：** 固定总容量、way、line size、MSHR、prefetch 开关，比较 modulo、静态 XOR/现有 hash、
  ENTROPyINDEX；另做“固定 mask 不更新”和 epoch sweep，证明收益来自自适应 entropy 而非偶然 hash。
- **趋势闸门：** mask 切换频率过高不得增加错误 tag/mapping；把更新端口或 epoch 延迟调大时，性能变化
  必须能由 conflict/set stats 解释。

## P0-B：UBS Uneven Block Size 指令缓存

### 论文概述

UBS 针对 server workload 大 instruction footprint 和固定 cache block 的空间浪费。论文测得平均约
**60%** 的 cache-block bytes 在被驱逐前从未访问；支持一个 set 中不同 block size 后，storage
efficiency 比 baseline 高 **32 个百分点**，同一 storage budget 可容纳超过两倍的 blocks，约 **32 KB**
UBS 的存储达到传统 **64 KB** fixed-block I-cache 的接近性能。摘要没有给精确 IPC 或综合面积，因此
只能使用“容量/存储效率”作为论文效果证据。

### 硬件成本与可实现性

真正的变长 line 需要同时处理：

- 每 entry 的 block-size、valid/sub-block 或 byte-valid metadata；
- tag compare、set 内不同 line 的地址范围检查和 victim 选择；
- refill/evict 的 variable-beat 请求、跨 block fetch、instruction boundary 和 coherence；
- cache miss 时对上游 Fetch 的 line alignment、重复请求合并和响应拼接。

这些成本在摘要中没有绝对 bit 数。一个可控的 GEM5 原型只允许 16/32/64 B 三种 size，参数化
`size_classes`、每类 entry 数、size metadata bits、refill beats/cycle 和跨 line penalty；每次
lookup 只扫描固定 ways，禁止通过无限小块模拟理想容量。

### GEM5 落点和验证

当前 `configs/common/Caches.py` 的 `L1_ICache` 继承 2-way `L1Cache`，`kmhv3.py` 将 `cpu.icache.size`
设为 **64 KB**；Cache 的默认 block size 来自系统 64 B line。建议先在独立 `UBSInstructionCache`
中实现 variable entry，而不是改变全局 `cache_line_size`（后者会同时影响 D-cache、L2 和 checkpoint
地址语义）。

第一阶段可用两种模型逐步收敛：

1. **结构模型：** 每个 cache set 同时放 16/32/64 B entry，严格计算 tag/data/metadata 容量和 refill
   latency；验证 hit/miss、跨 line fetch 和 eviction 正确。
2. **容量消融：** 固定总 SRAM bit，比较 64 B baseline、只增加 entry 数、UBS；统计有效 byte、I-cache
   MPKI、`fetch.icacheStallCycles`、跨 line 请求、tag comparisons、L2 instruction traffic 和 IPC。

只有在结构模型通过指令边界、squash/retry 和功能回归后，才能把“接近 64 KB baseline”作为当前 CPU
   的可测假设；论文的 32 KB/64 KB 关系不能直接当作 GEM5 的 2x 加速承诺。

## P1-A：LLBP Last-Level Branch Predictor

### 论文概述

LLBP 不把几百 KB 的高容量表放在每周期关键路径，而是在 baseline TAGE predictor 后增加高容量
backing。它利用长历史分支通常跨越多个动态 program context（可近似为 call chain），预测即将进入
的 context，把对应 branch metadata 预取到小型 in-core buffer，再与 TAGE 并行访问。论文以 **64 KB
TAGE-SC-L + 512 KB LLBP backing** 为例，MPKI 相对无 LLBP baseline 降 **0.5%--25.9%**，平均
**8.9%**；论文摘要还指出 baseline server CPU 有 **3.6%--20%**（平均 9.2%）执行周期浪费在
错预测上。

### 硬件成本

- 明确成本：512 KB branch metadata backing，外加 context predictor、metadata fetch queue 和
  小型 in-core buffer；backing 的读端口、bank 数和 tag/metadata 位宽必须列入 area/timing budget。
- 关键路径：当前 fetch 不能因为 backing miss 等待；需要并行 baseline prediction、有限 buffer hit、
  受带宽限制的 metadata prefetch 和 context miss fallback。
- 更新/恢复：FTQ 中保存 context ID、prefetch request 和实际 resolve 的版本；squash 后取消错误
  context prefetch，commit/resolution 更新 backing，避免历史污染。

### 当前 GEM5 映射

`kmhv3.py` 使用 `DecoupledBPUWithBTB`，FTQ/FSQ 各 64 entries，并打开 uBTB、ABTB、MicroTAGE、
MBTB、`BTBTAGE`、ITTAGE、MGSC 和 RAS；当前并不是论文的 64 KB TAGE-SC-L。建议独立新增
`LastLevelBP`，输入 PC、path/global history 和有限 context ID，输出 prediction metadata；由
`src/cpu/pred/btb/decoupled_bpred.*` 保存 speculative/committed history。第一版只做 backing
metadata prefetch 和 buffer hit，保持原 `BTBTAGE` 为 baseline，避免直接替换整个 BPU。

### 验证与成本闸门

报告 predictor storage bits、bank/port、prefetch queue、context hit/miss、metadata useful/late/
unused、branch MPKI、FTQ redirect、fetch bubble、ROB occupancy 和 IPC。先做 32/64/128/512 KB sweep；
只有 accuracy 提升能转化为 frontend progress，且 lookup latency 不增加，才值得接受 512 KB 级别成本。

## P1-B：Timely, Efficient, and Accurate Branch Precomputation（TEA）

### 论文概述

论文指出 hard-to-predict branch 不能只靠传统 predictor，改用提前执行 branch dependence chain
的 precomputation 结果。TEA 放宽“必须及时覆盖 Fetch”的约束，用结果触发 early misprediction flush，
因此可以保持较高覆盖率和准确率；摘要称使用 on-core execution resources，并复用现有 early-flush
硬件，最终相对 aggressive OoO baseline **+10.1%**。

### Hardware-only 风险

摘要没有说明 precomputation thread 是完全由硬件从已执行指令生成，还是需要 compiler/ISA 编码的
thread description，也没有给出表项、面积、能耗或额外带宽。故本文只把 TEA 列为 **待确认的 P1-B**，
不把它纳入第一轮“已满足 hardware-only”的 P0 清单。实施前必须通过以下门槛：

- 对不改变 binary 的 checkpoint，硬件能从 fetch/resolve 记录恢复 branch 的 bounded dependence chain；
- precompute 访问和执行有独立 token/queue，不能无限复制主线程或偷走可提交资源；
- early flush 只接受版本匹配、未 squash、无 exception/side effect 的结果；
- 能明确每条 speculative precompute 的恢复和取消路径。

### GEM5 原型建议

先新增只读的 branch-chain trace/统计，不改变预测结果；确认 H2P 分支、依赖链长度、可提前 flush
窗口和资源占用。第二阶段只允许 integer/纯寄存器链，把结果作为 early-flush candidate；由 Fetch、IEW、
ROB、squash 和 BPU history recovery 共同验证。若必须从软件产生 thread metadata，就应移到排除项，而
不是为了得到论文数字修改当前 workload。

## P1-C：FSDetect/FSLite False Sharing Repair（多核条件项）

### 论文概述

FSDetect 在 MESI coherence 中按 cache block 统计 coherence miss 频率，识别 harmful false sharing；
FSLite 对检测到的 line 做透明 privatization，结束时在 LLC 进行精确 byte-level merge。摘要称 FSDetect
能准确识别已知 harmful patterns、开销 negligible；FSLite 在受 false sharing 影响的多线程应用上
平均比 unmodified baseline **1.39x**，并降低网络压力和能耗。

### 硬件成本与当前边界

需要每 cache line 的小型 coherence-miss counter/threshold、private/merge 状态、byte dirty mask、
LLC merge buffer 和失效/终止协议。摘要只给 “minimal increase in chip area”，没有 bit 或面积表；
硬件成本应按 line metadata、merge bytes、LLC port 和多核消息数参数化。

当前 KMHv3 的常用验证目标是单核或少核 checkpoint；单核不存在跨 core false sharing，不能声称有收益。
多核实现应先在 `num_cpus=2/4` 的 classic coherence 路径确认论文所需的 MESI 等价状态、snoop、
writeback 和 byte merge，再跑显式 false-sharing microbenchmark。统计 coherence miss、privatization hit/drop、
merge bytes、invalidations、NoC traffic、memory latency 和 per-core IPC；论文 1.39x 只适用于触发
该病灶的 workload。

## P2：Self-Managing DRAM（SMD）

### 论文概述

SMD 把 refresh、RowHammer protection、scrubbing 等 maintenance 的控制责任部分移到 DRAM 芯片。
DRAM 在某个 subarray/bank 维护时拒绝该区域请求、允许其它区域继续访问，因而不需要 DDRx 新 pins，
还能重叠 maintenance 与其它 bank 的访问。论文摘要报告：延迟开销 **0.4% row activation latency**，
DRAM 芯片面积 **1.1%（45.5 mm² 芯片）**，20 个四核 memory-intensive workload 相对 DDR4-based
“智能并行 maintenance” co-design 平均 **+4.1%**，并保证被拒请求最终 forward progress。

### GEM5 可实现性

`configs/common/xiangshan.py` 在 `mem_type=DRAMsim3` 且未指定 ini 时默认
`ext/dramsim3/xiangshan_configs/xiangshan_DDR4_8Gb_x8_3200_2ch.ini`。DRAMsim3/Ramulator2 当前
没有 SMD 的 device-side maintenance scheduler、region reject response 和 forward-progress contract。
因此 SMD 不是 `kmhv3.py` 核心的 P0 feature，而是需要扩展 memory-device model 的 P2。

最小模型要保留 bank/subarray maintenance state、reject/retry latency、其它 bank 并行度、refresh/
scrub 事件和 starvation-free deadline；不能简单把 maintenance latency 设为零。A/B 应比较 baseline
DDR4、controller-only overlap 和 SMD device-side overlap，统计 rejected requests、retry age、bank
parallelism、row hit、memory queue、maintenance cycles、read latency 和四核 weighted speedup。

## 其它 MICRO 2024 论文的初筛结果

下表列出与 CPU/内存相关、但没有进入上面主 shortlist 的代表性论文，避免把“硬件论文”误当成当前
CPU 的纯硬件性能 feature。

| 论文/机制 | 初筛结论 | 原因 |
| --- | --- | --- |
| [Elastic Translations](https://doi.org/10.1109/MICRO61859.2024.00012) | 排除 | 依赖 Linux memory manager/KVM 的 OS-assisted TLB coalescing；论文最高 native +39%、virtualized 平均 +30%，不是只改硬件 |
| [Secure Prefetching for Secure Cache Systems](https://doi.org/10.1109/MICRO61859.2024.00017) | 延后 | 目标是 GhostMinion 等 secure cache baseline 的性能损失；当前 KMHv3 非该安全 cache，收益不是普通 CPU baseline 的 IPC |
| [Localizing Tag Comparisons / SegWU](https://doi.org/10.1109/MICRO61859.2024.00044) | 不列性能 shortlist | SPEC2017 tag comparisons -90.3%，总能耗约 -3.0%（含前端次级效应 -6.9%），摘要明确无性能提升；适合能效项目而非本任务 |
| [Scalar Vector Runahead](https://doi.org/10.1109/MICRO61859.2024.00101) | 条件项 | 纯硬件、约 2 KiB，但目标是简单 3-wide in-order core；论文相对其 baseline 3.2x、相对 OoO 1.3x，当前是 8-wide KMHv3 O3，迁移收益未建立 |
| [Temporarily Unauthorized Stores](https://doi.org/10.1109/MICRO61859.2024.00065) | 排除当前主线 | 依赖 x86-TSO、write-combining buffer 和 coherence order；论文 114-entry SB 平均 +3.2%，但 KMHv3 是 RISC-V RVWMO，不能直接移植语义 |
| [Chaining Transactions](https://doi.org/10.1109/MICRO61859.2024.00067) | 排除 | 需要硬件 transactional memory/TSO 或特定 transaction workload；当前 ISA/binary 没有对应提交接口 |
| [Genie Cache](https://doi.org/10.1109/MICRO61859.2024.00076) | 排除 | page-table-based DRAM cache 的 miss/eviction 仍需 OS/PTE 更新；不满足只改硬件和当前 memory hierarchy 前提 |
| [Memory Allocation under Hardware Compression](https://doi.org/10.1109/MICRO61859.2024.00075) | 排除 | 论文核心问题正是硬件压缩引入的新 actual-memory allocation interface；需要 OS/allocator 协同 |
| [StarNUMA](https://doi.org/10.1109/MICRO61859.2024.00077) | 排除当前单核 | 依赖 NUMA memory pooling/系统软件，当前 KMHv3 单核 checkpoint 没有对应 tiering topology |
| [MINT/BreakHammer/ImPress](https://doi.org/10.1109/MICRO61859.2024.00071) | 排除性能主线 | 重点是 RowHammer/Row-Press 安全防护及其 overhead，不是提高未启用安全机制的当前 CPU IPC |
| GPU、NPU、PIM、CXL accelerator、LLM/quantum 论文 | 排除 | 需要不同 ISA、设备、应用映射或系统平台，不能作为当前通用 O3 CPU 的硬件-only feature |

## 当前 GEM5 事实与推荐入口

| 主题 | 当前源码事实 | 对实现的含义 |
| --- | --- | --- |
| L1I | `L1_ICache` 是 2-way L1 基类；`kmhv3.py` 设置 64 KB；系统 cache line 默认 64 B | UBS 可做局部独立 cache；不要全局改 `cache_line_size` |
| L1D/indexing | `L1_DCache` 使用 `VIPTSetAssoc`/`VIPTSetAssociative`；dynamic physical-bit index 会碰到 translation/alias timing | ENTROPyINDEX 首先落在 physically indexed L2 |
| L2 | classic path 使用 `XSDRRIPRP(mode=2)`，2 MB、8-way、64 B、4096 sets；sliced path 有 4 slices | ENTROPyINDEX 可以保持 replacement、capacity、MSHR 不变，做 index-only A/B |
| BPU | `DecoupledBPUWithBTB` 连接 uBTB/ABTB/MicroTAGE/MBTB/BTBTAGE/ITTAGE/MGSC/RAS，FTQ/FSQ 64 | LLBP/TEA 必须保留 history recovery、FTQ metadata 和 squash contract |
| Issue queue | `IssueQue::wakeUpDependents()` 维护 dependency graph、跨 IQ wakeup matrix 和 replay | SegWU/TEA/任何 wakeup 改动都需计比较、端口和 ready latency；不宜只改 IPC |
| DRAM | `DRAMsim3` 默认 XiangShan DDR4 ini；`Ramulator2` 是另一条 memory model | SMD 需 device-level maintenance/reject 事件，不能仅改 controller 参数 |

## 统一验证流程

1. **固定 baseline。** 记录 git commit、`kmhv3.py` 参数、core 数、memory ini、checkpoint、warmup/ROI，
   只比较 warmup 后 reset 的 ROI stats。单核与多核结果分开。
2. **一次只改一件硬件事。** 新开关默认关闭，容量/way/line/MSHR/带宽保持不变；P0-A 先 index-only，
   P0-B 先有限 size classes，P1/P2 分阶段启用。
3. **保留因果统计。** Cache 记录 set conflict、MPKI、MSHR、prefetch useful/late/unused；UBS 记录
   valid bytes、跨 line fetch 和 tag/refill；BPU 记录 MPKI、FTQ redirect、context hit、metadata
   prefetch；FSDetect 记录 coherence miss/privatization/merge；SMD 记录 reject/retry/maintenance。
4. **正确性优先。** 变长 I-cache 要通过指令 boundary、跨 line、squash/retry；BPU 要通过 speculative
   history recovery；coherence 要通过多核 byte merge/ordering；DRAM 要证明 rejected request 最终
   forward progress。能效-only 的 SegWU 不以 IPC 变化作为成功标准。
5. **成本记录。** 每个 GEM5 模型报告 entry 数、字段位宽、比较器/端口、queue token、每周期更新上限和
   额外 latency。论文的 512 KB、2 KiB、1.1% area 等数字仅作为论文证据，不能替代当前实现的 area/
   timing 评估。
6. **结果判定。** 只有当 IPC/simTicks 变化能由上述 stats 解释，且扩大资源或放宽带宽呈现可解释趋势，
   才把 feature 记为有效；否则记录为 workload/platform mismatch，而不是调大资源掩盖问题。

## 参考论文与目录

| 机制 | MICRO 2024 论文 | DOI |
| --- | --- | --- |
| ENTROPyINDEX | *Customizing Cache Indexing Through Entropy Estimation* | [10.1109/MICRO61859.2024.00041](https://doi.org/10.1109/MICRO61859.2024.00041) |
| LLBP | *The Last-Level Branch Predictor* | [10.1109/MICRO61859.2024.00042](https://doi.org/10.1109/MICRO61859.2024.00042) |
| TEA | *Timely, Efficient, and Accurate Branch Precomputation* | [10.1109/MICRO61859.2024.00043](https://doi.org/10.1109/MICRO61859.2024.00043) |
| SegWU | *Localizing the Tag Comparisons in the Wakeup Logic to Reduce Energy Consumption of the Issue Queue* | [10.1109/MICRO61859.2024.00044](https://doi.org/10.1109/MICRO61859.2024.00044) |
| TUS | *Temporarily Unauthorized Stores: Write First, Ask for Permission Later* | [10.1109/MICRO61859.2024.00065](https://doi.org/10.1109/MICRO61859.2024.00065) |
| FSDetect/FSLite | *Leveraging Cache Coherence to Detect and Repair False Sharing On-the-fly* | [10.1109/MICRO61859.2024.00066](https://doi.org/10.1109/MICRO61859.2024.00066) |
| SMD | *Self-Managing DRAM: A Low-Cost Framework for Enabling Autonomous and Efficient DRAM Maintenance Operations* | [10.1109/MICRO61859.2024.00074](https://doi.org/10.1109/MICRO61859.2024.00074) |
| UBS | *Weeding out Front-End Stalls with Uneven Block Size Instruction Cache* | [10.1109/MICRO61859.2024.00102](https://doi.org/10.1109/MICRO61859.2024.00102) |

其他来源：

- [MICRO 2024 official program](https://microarch.org/micro57/program/)
- [ACM DL catalog entry](https://dl.acm.org/doi/proceedings/10.5555/979-8-3503-5057-9)（2026-08-06 复核：该 catalog identifier 在 `doi.org` 返回 404，不作为 DOI 书目证据；使用上方 MICRO 官方日程和各篇 IEEE DOI）
- [OpenAlex ENTROPyINDEX record](https://api.openalex.org/works/https://doi.org/10.1109/MICRO61859.2024.00041)
- [OpenAlex UBS record](https://api.openalex.org/works/https://doi.org/10.1109/MICRO61859.2024.00102)
- [Semantic Scholar LLBP record](https://api.semanticscholar.org/graph/v1/paper/DOI:10.1109/MICRO61859.2024.00042)
- 当前模型参考：[ISCA 2026 hardware feature survey](isca2026_hardware_feature_survey.md)、
  [HPCA 2026 hardware feature survey](../../design-docs/hpca26-gem5-hardware-feature-survey.md)

**证据边界：** 本文的论文效果和明确开销均按公开摘要原始口径转述；没有公开绝对面积的地方
   明确写“摘要未给出”。所有 GEM5 收益、硬件成本和优先级都是基于当前源码的研究建议，必须通过
   相同 checkpoint、warmup/ROI、功能回归和 stats 驱动 A/B 才能确认。
