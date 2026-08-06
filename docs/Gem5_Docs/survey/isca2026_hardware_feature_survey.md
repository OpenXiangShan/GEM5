# ISCA 2026：面向当前 KMHv3/GEM5 的纯硬件 Feature 调研

> 调研日期：2026-08-04。结论面向本仓库的 `configs/example/kmhv3.py` 单核/少核
> O3 配置，而不是对任意处理器的普适排名。

## 结论

第一批值得立项的不是论文中 IPC 数字最大的机制，而是能在当前模型中保留完整性能因果链、
又能用同一 checkpoint 做出可信 A/B 的机制：

1. **Bumper**：最小、最局部的 L2 指令行管理实验。现有 L2 已使用 `XSDRRIPRP`，可先验证
   “错误路径代码污染 → 提交确认有用 → L2 保留时间/前端停顿变化”的因果链。
2. **STEP**：最稳妥的数据预取研究入口。可复用现有 spatial/region、队列、过滤与 stats
   框架，但必须将论文的多时点触发状态独立建模。
3. **EgDiff 全局值预测**：仓库已经有完整的值预测接入与 squash 框架；先完成一个
   deferred/global-history 的可恢复性原型，再决定是否进入完整实现。

随后是 **ICP**（不规则访问、潜在收益高但需要新的 commit sideband）和
**RUNLTS 的 RBias 子机制**（BPU 研究价值高但需后端寄存器值摘要回传）。
Moirai、IP-CaT、HWL 和硬件压缩 RST 分别适合后续预取、前端、时序扩展和平台级项目，
不应与上述局部 CPU/cache 改动混为同一工作量级。

**2026-08-06 审计更正。** 本轮只能通过 Crossref 确认本文 14 个候选 DOI 的题名、作者、年份和
ISCA 归属；直接访问 Bumper 的会议 PDF 端点返回 HTTP 401，IEEE 落地页返回 HTTP 202，未能在本轮
读取候选全文。因而下文的机制细节、百分比、面积和状态大小均是**原稿待原文核验转述**，不能作为
已确认的论文事实，更不能作为本仓库的预期收益。取得作者公开版或可读正式 PDF 后，须逐项恢复其
原始 workload、baseline 和单位再确认；当前 GEM5 的结论只能来自相同 checkpoint、相同 ROI、
warmup 后 reset 的最终 stats A/B。

## 范围与筛选方法

### “只改硬件”的严格含义

本调研仅把同时满足下列条件的机制列为主候选：

- 不要求编译器改写、二进制 metadata、OS 分配器/加载器、ISA 扩展、离线 profile 或未来访问
  oracle；
- 性能收益由 CPU、cache、TLB、预取器或 memory controller 中可实现的状态和控制动作产生；
- 能写出有限状态、有限队列/表项、带宽或延迟约束，而非把“预测正确”作为零成本黑盒；
- 与当前 KMHv3 单核/少核模型的范围相符。仅在大规模一致性、CXL 压缩 tier 等新平台上才有意义的
  方案会单列，而不会挤占核心 feature 的优先级。

“在 GEM5 中容易实现”不等于“RTL 的面积和时序可接受”。文中同时标记了行为模型风险和
硬件实现风险。

### 信息源与证据等级

- 官方目录：[ISCA 2026 WebPub](https://conferences.computer.org/iscapub26/)。目录含 172 篇
  AP 论文；本轮只完成题名和 DOI 的元数据初筛，没有把受保护 PDF 的内容当作已读证据。
- 每项的 DOI 和官方 PDF 链接列在相应小节及文末。PDF endpoint 需要用户授权会话；本轮访问时
  未出现可用的全文响应。DOI 记录已经开放解析，但不提供摘要或全文。本文不保存访问凭据。
- “当前树映射”来自本 checkout 的只读检查；源码优先于旧文档。关键入口包括
  `configs/example/kmhv3.py`、`src/cpu/o3/`、`src/cpu/valuepred/`、
  `src/cpu/pred/btb/`、`src/mem/cache/prefetch/` 和
  `src/mem/cache/replacement_policies/`。

## 决策总表

| 启动级别 | 论文/机制 | 原稿转述的论文结果与代价（待原文核验） | 当前 GEM5 落点 | 结论 |
| --- | --- | --- | --- | --- |
| P0-A | Bumper | 移动应用平均 +6.5%；相对 FTQ-size 优化 +5.4%；422 B | `xs_drrip_rp.*`、L1I/ROB/commit 侧 hint | 最小局部实验；工作负载迁移风险高 |
| P0-B | STEP | 总状态 10.50 KB；总体 1.28x、eBingo 1.26x（相对 no-prefetch） | `sms.*`/`Queued`/`Prefetcher.py` | 最稳妥的预取实现入口 |
| P0-C | EgDiff | 11 KB +4.28%；19 KB EgDiff+EVES +6.16% | `valuepred/`、Fetch/rename/commit/squash | 先做 deferred-prediction 可恢复性原型 |
| P1 | ICP | 相对 basic prefetch +25.51%、Triangel +13.99%、DMP +5.97%；2.1 KB | O3 commit sideband、L1D prefetch、MSHR/fill metadata | 不规则访问潜力大，接口工作量中等偏高 |
| P1 | RUNLTS/RBias | 192 KiB 总预算下 SPEC17 branch mispredictions -5.25%；RBias 6.576/7.112 KiB | `btb_mgsc.*`、decode/execute digest sideband | 先隔离 RBias，不直接移植整套 TAGE-SC-L |
| P1 | Moirai | 780 B；总体 +11.48%；DRAM 流量 +56.6% | L1D `Queued` prefetcher | 必须显式建模 1–3 cycle 与 phase/throttle |
| P1/P2 | IP-CaT | tPB+TIPRP 0.79 KB；+6.1/+8.3/+7.9%（不同 L1I PF） | IFetch/ITLB/sTLB/L2 replacement | 先建立当前树可用的 L1I PF baseline |
| P2 | HWL | L1 matrix 0.6 KB + LRP 2.0 KB；IQ cycle time -53%，原配置 IPC -0.9%；配合 1.5x 资源 +17.2% | `issue_queue.*`、scheduler config | 时序/频率扩展研究，固定频率 IPC 不足以验证 |
| P3 | RST/RAHSC | 128 B dictionary；18 ns 解压；系统约 +14% geomean | 新的压缩内存 controller/tier | 纯硬件语义可行，但超出当前核心/cache 范围 |

这里的 P0/P1 表示**风险调整后的启动顺序**，不是不同论文 IPC 的横向排序。EgDiff 和 ICP 的
研究上限可能高于 Bumper，但恢复语义和接口风险也明显更大。

## 主候选

### P0-A：Bumper —— 提交确认的 L2 指令行保留

来源：[官方 PDF](https://conferences.computer.org/iscapub26/pdfs/5065-3nexm57sBmVa6MJtFTiSrb/506501c029/506501c029.pdf) · [DOI](https://doi.org/10.1109/ISCA66397.2026.00145)

论文观察到错误路径取指和错误路径预取造成的无用 instruction line 平均占一个大 L2C 约 20%。
Bumper 将新进入 L2 的代码行放在脆弱 RRPV 位置；若该行中第一条携带 hint 的指令最终提交，
才向 L2 发送一次 promotion hint，将该行提升为高优先级。论文用 L1I tag 的 `send_hint` bit、
ROB bit、两个小 hint lookup queue 和 L2→L1I 的 `l2_vulnerable_fill` bit 实现，共 422 B。

**当前树匹配度。** `kmhv3.py` 将 2 MB L2 配为 `XSDRRIPRP(mode=2, ...)`；
`src/mem/cache/replacement_policies/xs_drrip_rp.*` 已有 RRPV、refill、prefetch 与 DRRIP
set-dueling 状态。因此 replacement policy 不是从零开始。不过当前 policy 会把普通 non-prefetch
refill 当高优先级，Bumper 需要显式区分 instruction fill 与 data fill，不能粗暴改变全部 refill 的
RRPV。`Request::isInstFetch()` 已可用于该区分。

**最小建模合同。**

- 因果链：错误路径 instruction fill → L2 中无提交证据的 line 占用/驱逐有用 line → 前端 miss/stall；
  提交一次 → 仅该 line promotion → line lifetime 改变。
- 必细建：每次 L1I residency 最多一个 hint；hint 的 L2 hit/miss、队列容量与仲裁；squash 后不能
  产生 promotion。
- 可粗建：论文的具体 VA 回送/ITLB 机会式访问可先压成有界 `hint_latency` 和 `hint_queue_entries`，
  但不得把 hint 当作零延迟、无限带宽。
- 参数：开关、instruction-fill insertion RRPV、promotion RRPV、L1I tag/ROB hint enable、队列深度、
  hint latency。默认关闭以保持旧行为。
- 观测：vulnerable instruction fills、first-commit hints、hint hit/miss/drop、被驱逐的 useful/useless
  instruction lines、L1I/L2 instruction MPKI、front-end stall cycles、L2 request traffic。

**验证建议。** 先做两级 A/B：只改 L2 insertion（无 commit hint）与完整 Bumper。若只有前者变快，
不能把收益归因于“提交确认”。论文针对有强 wrong-path FDIP 和移动代码 footprint 的处理器；当前
2 MB L2、SPEC slice 或无同等 I-prefetch 的情形下，收益可能很小甚至为负，这本身是有效结论。

### P0-B：STEP —— 多时点触发的 spatial footprint prefetcher

来源：[官方 PDF](https://conferences.computer.org/iscapub26/pdfs/5065-3nexm57sBmVa6MJtFTiSrb/506501b241/506501b241.pdf) · [DOI](https://doi.org/10.1109/ISCA66397.2026.00095)

STEP 不把一个 region 的第一次访问当作唯一 trigger。它在 footprint 的 first/second/third offset
时分别尝试触发，并以 PHT pattern intersection 与 Jaccard 相似度决定该时点的预测集合和置信度。
论文给出 10.50 KB 状态；其跨套件总体结果为 1.28x（eBingo 为 1.26x），均相对于论文自己的
no-prefetch baseline。

**当前树匹配度。** `src/mem/cache/prefetch/sms.hh` 的 `XSCompositePrefetcher` 已有 ACT、PHT、
region/offset、`Queued` 请求队列、filter 和 prefetch stats；这是复用基础设施，不是 STEP 已经存在。
推荐新增独立 `StepPrefetcher`（或作为 composite 子模块），避免先把现有 SMS 的训练规则改坏。

**最小建模合同。**

- 因果链：region access offset → ACT footprint 与 trigger-phase → PHT candidate 的交/相似度 →
  有界 prefetch requests → MSHR/带宽/污染 → covered/late/useless misses。
- 必细建：FOE/SOE/TOE 的状态转移、每个 trigger 的候选去重、置信度阈值、queued request 的
  admission/drop。
- 可粗建：PHT 的内部位级组合逻辑可由 bitset intersection/Jaccard 表达；每次访问只扫描固定的
  pattern/region bit 数，不能在热路径做无界历史扫描。
- 参数：ACT/PHT 容量和相联度、region size、三个 trigger offset、Jaccard threshold、degree、
  queue size、prefetch fill level、on-miss/on-access。
- 观测：每个 trigger 的 train/issue/useful/late/useless/hit-overprediction、PHT hit、过滤和队列
  drop、prefetch-induced bandwidth、MSHR occupancy、demand MPKI/IPC。

**验证建议。** 首先用关闭当前同层 prefetcher 的 L2 baseline；再比较 SMS、STEP 和一个“只用 FOE”
消融。若只报 IPC 而不报 coverage、late、useless 与带宽，无法判断 STEP 的多时点机制是否真的在
发挥作用。

### P0-C：EgDiff —— 可恢复的全局值预测

来源：[官方 PDF](https://conferences.computer.org/iscapub26/pdfs/5065-3nexm57sBmVa6MJtFTiSrb/506501a574/506501a574.pdf) · [DOI](https://doi.org/10.1109/ISCA66397.2026.00053)

论文以 Global Value Queue（GVQ）保存跨动态指令的值，以 PC 索引的 diff/distance table 查找
base value；通过 deferred prediction、distance polling 与非投机训练处理 value-delay、污染和
大存储问题。11 KB EgDiff 报告 +4.28%，4K/44 KB 版 +4.37%；11 KB EgDiff + 8 KB EVES 的
19 KB hybrid 报告 +6.16%。44 KB table+GVQ 的面积/能耗估算分别约为处理器的 0.22%/0.07%。

**当前树匹配度与关键缺口。** 当前已有 `src/cpu/valuepred/`、`CompositeValuePredictor`、
prediction record、commit feedback 和 squash 回调；`fetch.cc` 发起预测，`commit.cc` 用真实值训练，
所以 predictor 基础设施非常适合复用。但论文的关键动作发生在 dispatch/deferred window：现有
公共 predict hook 位于 Fetch，不能把 EgDiff 当作一个只实现 `VPUnit::predict()` 的普通 child。
必须先扩展请求时点或引入可恢复的 deferred state，明确 GVQ 中的 speculative/committed entries 和
squash 边界。

**最小建模合同。**

- 因果链：已完成/可预测的 producer value → GVQ base 命中 → 解除 consumer data dependency →
  wakeup/issue 提前；错误预测 → value-prediction squash/recovery。
- 必细建：每线程 GVQ 位置、prediction record、deferred/poll state、预测是否真正 applied、
  misspeculation 后的精确清理。
- 可粗建：预测表的物理 SRAM 访问可为参数化 1-cycle/多周期 lookup；不要模拟神经或组合逻辑的
  每一个 gate，但要保留 table port 和 prediction latency。
- 参数：table entries、GVQ depth、max distance、poll window/period、confidence threshold、
  lookup latency、enable speculative GVQ entry。
- 观测：offered/selected/applied/correct、GVQ hit/poll/defer、coverage、预测值 wakeup 数、
  `squashDueToValuePrediction`、恢复延迟、错误预测损失的 cycles。

**验证建议。** 分三步：仅 non-speculative GVQ 训练；只提供但不应用预测；最后打开 apply/squash。
这样可以区分“预测命中率高”与“真正缩短依赖链”。若不能证明切换阈值时 correctness/recovery 仍正确，
不要用完整 benchmark IPC 作为实现完成证据。

### P1：ICP —— 指令相关性驱动的不规则访问预取

来源：[官方 PDF](https://conferences.computer.org/iscapub26/pdfs/5065-3nexm57sBmVa6MJtFTiSrb/506501c046/506501c046.pdf) · [DOI](https://doi.org/10.1109/ISCA66397.2026.00146)

ICP 在提交指令流中重建 `PCpre → PCsuc` producer/consumer 依赖，记录小型 correlation table。
当携带 `PCpre` 的 cache line 返回时，Data Extractor 取出所需数据，Lightweight Calculator 重放
受限的地址计算，发出 `PCsuc` 预取；Source Predictor 为链外源操作数提供高置信度值。论文的总
状态约 2.1 KB，包含 commit FIFO、ROB/MSHR extension；相对仅 basic prefetch 的基线 +25.51%，
相对 Triangel/DMP 分别 +13.99%/+5.97%，DRAM traffic +13.98%。

**当前树匹配度。** `indirect_memory.*` 可以作为不规则 prefetch 的接口对照，`pif.hh` 也已有
监听 retired-PC probe 的先例。ICP 仍缺三类信息：提交时的 source/destination physical register
和结果、cache fill 对应 load PC/offset、以及从 core 到 prefetcher 的有界异步队列。不要为了得到
这些信息让 prefetcher 每周期扫描 ROB；应由 commit 构造固定大小记录并推送到 sideband FIFO。

**最小建模合同。**

- 因果链：commit dependency record → bounded correlation discovery → fill data + PC metadata →
  calculator → irregular prefetch → cache/DRAM 竞争与 miss 覆盖。
- 必细建：FIFO 满的丢弃/反压策略、每 epoch 的 PC 选择、有限依赖深度、calculator 支持的操作集、
  MSHR source-PC metadata。
- 可粗建：论文的 dependency-tree internal graph 用固定 node table/producer map 表达；每次 commit
  只访问常数个 source/destination register，不做 O(ROB) 搜索。
- 参数：candidate/correlation/node/producers 表项、commit FIFO 深度、epoch、top-N/threshold、
  max chain length、calculator latency/width、degree、source predictor confidence。
- 观测：candidate/correlation 数、FIFO overflow、data-extractor hit、calculator issued/suppressed、
  useful/late/bad prefetch、每条 prefetch 的 source（demand/basic-PF）、DRAM traffic、L2 MPKI。

**验证建议。** 先从只在 demand fill 上触发的 one-hop chain 开始，再增加 basic-prefetch fill 与
source prediction。每一级要比较 `no-ICP / learned-but-not-issued / issued`，避免把底层预取器本身的
收益误归给 ICP。

### P1：RUNLTS 的 RBias —— 用寄存器值摘要补充分支预测

来源：[官方 PDF](https://conferences.computer.org/iscapub26/pdfs/5065-3nexm57sBmVa6MJtFTiSrb/506501a543/506501a543.pdf) · [DOI](https://doi.org/10.1109/ISCA66397.2026.00051)

RUNLTS 在 TAGE-SC-L 上同时调整 allocation/history 组织，并加入 RBias：由近期寄存器值生成
digest，喂入 perceptron-style statistical corrector。固定 192 KiB 预算下，论文在 SPEC2017/gem5
报告 branch mispredictions -5.25%；Log-RBias/Seq-RBias 分别约 6.576/7.112 KiB。16-wide 实验中，
Log-RBias IPC +1.55%、核心面积 +0.49%，Seq-RBias IPC +1.43%、面积 +0.04%。

**当前树匹配度。** 当前 KMHv3 默认使用 `DecoupledBPUWithBTB`，同时启用 BTB TAGE、
microTAGE、MGSC 等，而不是论文的通用 TAGE-SC-L。`src/cpu/pred/btb/btb_mgsc.hh` 已有
G/L/I/P/Bias 类表和 prediction metadata，适合承载 RBias 的“SC override”部分；不能把论文的
192 KiB 整体配置数字直接套到当前 BPU。

**推荐范围。** 先实现 **RBias-only A/B**：decode 将 destination logical register 的 in-flight
producer/ROB identity 写入 digest map，execute completion 以相同 identity 广播结果并写入 digest；
前端用该 map 的 snapshot/index 读取。Log-RBias 必须 checkpoint/restore digest map，Seq-RBias 则需
精确保持其 recovery-free/wrong-path 语义。再评估是否需要论文的 allocation/history 重排。关键 stats
是 conditional MPKI、RBias lookup/hit/override/correct/wrong、decode/execute digest update、digest age、
恢复次数和前端方向误预测周期。

### P1：Moirai —— 小型 BNN/TCN L1D 预取框架

来源：[官方 PDF](https://conferences.computer.org/iscapub26/pdfs/5065-3nexm57sBmVa6MJtFTiSrb/506501c287/506501c287.pdf) · [DOI](https://doi.org/10.1109/ISCA66397.2026.00161)

论文题名是 *From Memorization to Generalization: A Practical Neural Network Prefetching Framework*；
Moirai 是其中的框架。它将约 0.27 KB CaPNet 与 0.5 KB stride fallback 组合为 780 B，在论文
套件上总体 +11.48%（SPEC06/17 为 +8.75%/+6.17%）。论文综合报告 7 nm 下约 1178 um2、8.5 mW，
并**明确**给 pipelined forward pass 加 3-cycle 延迟；DRAM traffic 增加 56.6%。

因此其 GEM5 合同不能是“调用一个即时 ML 函数”。应将 history、inference PRQ、1–3 cycle
forward latency、train/infer phase、loss-based throttle 和 stride fallback 全部建成有界状态；复用
`Queued` 的队列、filter 和 prefetch admission。至少报告 inference queue full、phase transitions、
issued/useful/late/bad、DRAM 带宽和 demand-vs-prefetch traffic。高流量场景应先验证没有伤害共享
L2/DRAM，再讨论 IPC。

### P1/P2：IP-CaT —— L1I 跨页预取的 translation/cache 协同

来源：[官方 PDF](https://conferences.computer.org/iscapub26/pdfs/5065-3nexm57sBmVa6MJtFTiSrb/506501c254/506501c254.pdf) · [DOI](https://doi.org/10.1109/ISCA66397.2026.00159)

IP-CaT 包括两个纯微结构组件：translation prefetch buffer（tPB）保存由跨页 L1I prefetch 取回的
translation，TIPRP 则区分 L2 中由 instruction prefetch 带来的 line 的保留价值。论文的 tPB+TIPRP
总开销 0.79 KB；其与 EPI/Barca/FNL+MMA 组合，分别报告 +6.1%/+8.3%/+7.9%。

它不需要软件改动，但需要贯通 IFetch、ITLB/sTLB/PTW、L1I prefetch request metadata 和 L2
replacement。当前树尚未有与论文等价、成熟的 L1I prefetch baseline，所以应先建立并校验 baseline；
再做 tPB-only、TIPRP-only、combined 三个 A/B。不要先在 `XSDRRIPRP` 中硬编码 IP-CaT 策略，
应将 instruction-prefetch provenance 作为有限 request metadata 传递并保持 data/ordinary instruction
line 的旧策略不变。

### P2：HWL —— Issue Queue 分层 wakeup

来源：[官方 PDF](https://conferences.computer.org/iscapub26/pdfs/5065-3nexm57sBmVa6MJtFTiSrb/506501a529/506501a529.pdf) · [DOI](https://doi.org/10.1109/ISCA66397.2026.00050)

HWL 将 IQ 分段：小而快的 L1 wakeup 配合 pipeline 化的 L2 和 structure-aware dispatch，以降低
wakeup-select 临界路径。论文默认的 8-segment/200-entry IQ 中，新增 L1 matrices 为 0.6 KB，
last-ready predictor（LRP）为 2.0 KB，另有轻量 RMT/control 扩展。论文报告 IQ cycle time -53%，
但在原固定资源配置 IPC -0.9%；只有在前后端/IQ 等资源提升到 1.5x 的协同配置中才达到平均 +17.2%。

当前 `issue_queue.*` 与 `configs/common/FUScheduler.py` 可承担行为模型，但 gem5 的固定时钟频率
不能自动把“临界路径缩短”变成 IPC。正确的研究形式是参数化 L1/L2 wakeup latency、segment 容量、
dispatch policy 和可选择的 core frequency/resource-scale 场景，观察 ready/issue/blocked/replay、
IQ occupancy 与依赖链长度。若没有一个明确的频率映射或资源扩展实验，不能声称复现论文收益。

### P3：Random-Access Hardware Sequence Compression（RST）

来源：[官方 PDF](https://conferences.computer.org/iscapub26/pdfs/5065-3nexm57sBmVa6MJtFTiSrb/506501b432/506501b432.pdf) · [DOI](https://doi.org/10.1109/ISCA66397.2026.00107)

论文题名是 *Random-Access Hardware Sequence Compression*，RST 是其算法名。其 memory-controller
integrated hardware compression 语义本身满足硬件-only：128 B dictionary，文中对比的随机块解压约
18 ns（TMCC ASIC Deflate 约 140 ns）；DyLeCT 系统结果约 +15% arithmetic、+14% geomean。

但忠实模型需新增 compressed-page placement、可变长 packing、OS-physical 到 machine-physical
remap、压缩/解压队列和带宽、冷热页 expand/recompress、memory tier。当前树没有这类 controller
或压缩内存 tier。故它是“硬件上可行、当前范围外”的平台级 P3 项目；若未来选择 CXL compressed
tier，论文的部署还涉及软件迁移，不能再称为当前严格范围内的无软件方案。

## 明确排除或延后

| 论文 | 为什么不进入严格 shortlist |
| --- | --- |
| Squashed-Branch Reuse Buffer（SBRB），[DOI](https://doi.org/10.1109/ISCA66397.2026.00052) | 虽然 reuse buffer 本身是硬件结构，但论文实现依赖 LLVM 生成 loop descriptors、写入 binary 的 Loop Information Segment（LIS）、ISA 可见 LIT-H/LIT-E 和 OS loader/scheduler 装载。论文把纯硬件 loop/special-edge inference 留作 future work；所以不符合“只改硬件即可得到论文机制”的严格定义。 |
| Revelator，[DOI](https://doi.org/10.1109/ISCA66397.2026.00147) | 核心是 OS tiered hash-based allocator 与硬件协同的 speculative translation；没有 OS 分配策略就不是论文方案。 |
| R-Max，[DOI](https://doi.org/10.1109/ISCA66397.2026.00160) | 使用未来访问 oracle 给出 cache/prefetch 上界，适合作为研究 oracle，不能落为硬件 feature。 |
| Dorado，[DOI](https://doi.org/10.1109/ISCA66397.2026.00049) | 目标是约千核目录一致性；纯硬件但与当前单核/少核 KMHv3 核心优化范围不匹配。 |
| SSBench，[DOI](https://doi.org/10.1109/ISCA66397.2026.00127) | 分析/表征工具，不是可实现的微结构 feature。 |

## 推荐实施路线

### Phase 0：统一基线和观测

1. 固定 KMHv3 config、binary、checkpoint、warmup/ROI 和 core count；对 checkpoint slice 不加
   `--raw-cpt`。
2. 先产生 baseline 的最终 post-reset `stats.txt`，而非混用 warmup 统计。
3. 对每个候选定义一套 feature-off、子机制-off、feature-on 配置，并保留 seed/命令行。
4. 先确认所选 slice 对应的瓶颈：Bumper 看 I-side MPKI/front-end stall，STEP/ICP/Moirai 看 demand
   miss 与带宽，EgDiff 看依赖/VP squash，RUNLTS 看 conditional MPKI，HWL 看 IQ/依赖链。

### Phase 1：低侵入、可归因的两个 A/B

- **Bumper**：先加 request provenance 与 RRPV insertion/promotion 统计，再接 L1I/ROB 的一次性
  commit hint。保持默认关闭，并保留普通 data/L2 策略。
- **STEP**：独立 SimObject；以现有 `Queued`/filter/ACT/PHT 容器为基础，实现多时点 trigger。
  从单 L2、关闭其他同级 spatial prefetch 开始。

### Phase 2：核心 sideband 原型

- **EgDiff**：定义 GVQ 的 speculative/committed ownership、deferred request 时点和 squash 合同；
  通过 microtest 与 `offered → applied → correct/wrong` stats 验证，再打开大 workload。
- **ICP**：先增加不影响原 pipeline 的 commit FIFO 和 fill-PC metadata；one-hop calculator 验证后，
  才加 source predictor 与 deeper chains。
- **RUNLTS/RBias**：先补 decode/execute 的 logical-register digest notification 与恢复语义，再把
  digest 加到当前 MGSC 旁路；保留当前 TAGE/microTAGE/MGSC 默认路径作为 baseline，评估其 override
  而不是宣称完整 RUNLTS 对齐。

### Phase 3：研究型/跨层项目

- **Moirai**：参数化 forward latency、训练和 throttle，以 traffic guardrail 作为 acceptance criterion。
- **IP-CaT**：先有 L1I PF baseline 后，按 tPB、TIPRP、combined 分段落地。
- **HWL**：与可解释的频率映射或 1.5x resource-scale 实验绑定。
- **RST**：只有在明确启动 compressed-memory/controller 项目时单独新建 ExecPlan。

## 统一验证口径与判定门槛

每项 feature 至少应报告以下四类结果：

1. **性能结果**：ROI committed instructions、IPC/执行 tick、每 benchmark 的加权/几何平均；不能只报
   平均 IPC，必须列出退化项。
2. **机制覆盖**：如 hint hit、GVQ hit、correlation learned、RBias override、STEP trigger phase；
   证明 feature 实际被使用。
3. **副作用**：prefetch useful/late/useless、L2 eviction、MSHR/queue full、DRAM traffic、squash 或
   recovery。没有该项，就不能判断“加速”是否靠不现实的无限资源取得。
4. **趋势/消融**：表大小、延迟、带宽、阈值或开关的单变量 A/B。参数变化应有可解释趋势，而不是
   只展示一个最佳点。

建议构建命令仍以仓库标准入口为准：

```bash
scons build/RISCV/gem5.opt --gold-linker -j64
GCBV_REF_SO=/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-interpreter-so \
  ./build/RISCV/gem5.opt ./configs/example/kmhv3.py \
  --generic-rv-cpt=<checkpoint-slice.zstd>
```

完整 checkpoint 回归在 feature 的建模合同、局部 smoke 和同 ROI A/B 均成立后再启动。对于本文的
调研阶段，没有声称任何 feature 已实现、已编译或在当前 GEM5 上获得论文所列收益。

## 参考论文

| 机制 | 论文题名 | 作者（官方目录） | DOI |
| --- | --- | --- | --- |
| Bumper | *Bumper: Hinting Instruction Usefulness for Robust Unified Caches* | Georgios Vavouliotis, Tom Rollet, Davide Basilio Bartolini, Boris Grot, Leeor Peled, Lixia Yang | [10.1109/ISCA66397.2026.00145](https://doi.org/10.1109/ISCA66397.2026.00145) |
| EgDiff | *Revisiting Global Value Prediction: A Resurgent Complement to Local Predictors* | Ling Yang, Libo Huang, Zhong Zheng, Bingcai Sui, Sheng Ma, Yongwen Wang, Li Shen, Junhui Wang, Gang Chen, Qianming Yang, Songwen Pei, Weixia Xu | [10.1109/ISCA66397.2026.00053](https://doi.org/10.1109/ISCA66397.2026.00053) |
| ICP | *ICP: Exploiting Instruction Correlation for Prefetching Irregular Memory Accesses* | Mengming Li, Chenlu Miao, Buqing Xu, Qijun Zhang, Xiangfeng Sun, Ceyu Xu, Yuan Xie, Wenkai Li, Shang Liu, Zhiyao Xie | [10.1109/ISCA66397.2026.00146](https://doi.org/10.1109/ISCA66397.2026.00146) |
| RUNLTS | *RUNLTS: Branch Prediction with Register-Value Correlations and Hierarchical Table Orchestration* | Toru Koizumi, Toshiki Maekawa, Masanari Mizuno, Maru Kuroki, Tomoaki Tsumura, Ryota Shioya | [10.1109/ISCA66397.2026.00051](https://doi.org/10.1109/ISCA66397.2026.00051) |
| STEP | *STEP: Spatial Footprint Prefetcher with Multi-Point Temporal Triggers* | Yuanji Ye, Oliver Lenke, Thomas Wild, Andreas Herkersdorf | [10.1109/ISCA66397.2026.00095](https://doi.org/10.1109/ISCA66397.2026.00095) |
| Moirai | *From Memorization to Generalization: A Practical Neural Network Prefetching Framework* | Xuan Tang, Zicong Wang, Shuiyi He, Hao Tang, Dezun Dong, Xiangke Liao | [10.1109/ISCA66397.2026.00161](https://doi.org/10.1109/ISCA66397.2026.00161) |
| IP-CaT | *Enhancing Instruction Prefetching via Cache and TLB Management* | Alexandre Valentin Jamet, Georgios Vavouliotis, Martí Torrents, Dimitrios Chasapis, Marc Casas | [10.1109/ISCA66397.2026.00159](https://doi.org/10.1109/ISCA66397.2026.00159) |
| HWL | *Hierarchical Wakeup Logic of the Issue Queue for High Scalability* | Hideki Ando, Hajime Shimada | [10.1109/ISCA66397.2026.00050](https://doi.org/10.1109/ISCA66397.2026.00050) |
| RST | *Random-Access Hardware Sequence Compression* | Nolan Chu, Yoon Lee, Gagandeep Panwar, Xun Jian | [10.1109/ISCA66397.2026.00107](https://doi.org/10.1109/ISCA66397.2026.00107) |
