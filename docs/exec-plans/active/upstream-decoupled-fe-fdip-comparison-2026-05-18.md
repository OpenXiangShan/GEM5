# 对比上游 decoupled frontend / FDIP 与当前 Kunminghu 前端

## 背景和目标

当前 `/nfs/home/yanyue/workspace/GEM5-raw` 是已经更新到 `develop` 的上游 gem5 仓库，和当前主要工作仓库 `/nfs/home/yanyue/workspace/GEM5_review` 分叉超过三年。上游已经合入通用 O3 decoupled frontend、Fetch Directed Prefetcher（FDP/FDIP 类似机制）以及相关配置；本仓库则长期演进出 Kunminghu v3 对齐的 decoupled BTB 前端、FTQ/FSQ、BTBTAGE、MGSC、AheadBTB、MicroTAGE 和本地 prefetch 体系。

本任务的目标不是直接移植上游实现，而是建立一份可持续更新的机制对照，回答：

- 上游 decoupled frontend 和 FDP 的核心设计是什么，真实入口在哪里
- 当前 Kunminghu v3 前端已经覆盖了哪些能力，结构上有哪些不同
- 上游哪些特征值得借鉴，分别适合进入 BPU、Fetch/FTQ、ICache prefetch、统计观测或配置层
- 哪些特征只适合上游 ARM/O3 通用路径，不适合直接搬到当前 RTL-aligned 路径
- 后续若要尝试，应如何拆成低风险、可验证的小实验

最终产出应是一份分层推荐，而不是单一结论：先给出机制级差异和候选特征，再按收益潜力、接入风险、验证成本排序。

## 当前已知信息

- 当前仓库状态：
  - `GEM5_review` 位于 `xs-dev...origin/xs-dev`，当前未见工作区改动
  - `GEM5-raw` 位于 `develop...origin/develop`，存在用户已有的 `.gitignore` 修改，应保持只读对比
- 本仓库的架构文档明确指出，当前活跃路径是 `configs/example/kmhv3.py` 选择的 XiangShan/Kunminghu v3 O3 + decoupled BTB frontend；代码真实入口主要在 `src/cpu/o3/fetch.*` 和 `src/cpu/pred/btb/`
- 本仓库已有设计文档索引位于 `docs/design-docs/frontend/README.md`，相关主题包括 `bpu_top_level.md`、`mbtb_design.md`、`btb_tage_design.md`、`mgsc_design.md`、`ubtb_design.md`、`abtb_design.md`、`microtage_design.md`
- 上游 decoupled frontend 关键提交包括：
  - `719c799dc3 cpu: Implement decoupled front-end`
  - `145efd442b mem-cache: Add fetch directed prefetcher`
  - `38c7e348a5 mem-cache: Prefetch for all cache blocks in a Fetch Target`
  - `d55f46336f cpu: Branch predictor latency and overriding model for the decoupled frontend`
  - `e676195c5f cpu-o3,stdlib: Stdlib configs for decoupled FE`
- 上游实现的初步入口：
  - `src/cpu/o3/bac.*`：branch address calculation / FTQ producer
  - `src/cpu/o3/ftq.*`：通用 FetchTarget / FTQ
  - `src/cpu/o3/fetch.*`：消费 FTQ 的 fetch stage
  - `src/mem/cache/prefetch/fdp.*`：Fetch Directed Prefetcher
  - `src/cpu/o3/BaseO3CPU.py`：`decoupledFrontEnd`、`numFTQEntries`、`fetchTargetWidth` 等参数
  - `configs/common/cores/arm/neoverse_v2.py`：上游 ARM 配置示例启用 decoupled frontend
- 本仓库当前实现的初步入口：
  - `src/cpu/pred/btb/decoupled_bpred.*`
  - `src/cpu/pred/btb/ftq.*`
  - `src/cpu/pred/btb/mbtb.*`
  - `src/cpu/pred/btb/abtb.*`
  - `src/cpu/pred/btb/microtage.*`
  - `src/cpu/pred/btb/btb_tage.*`
  - `src/cpu/pred/btb/btb_mgsc.*`
  - `src/cpu/o3/fetch.*`
  - `configs/example/kmhv3.py`

## 假设和开放问题

- 假设：上游 FDP 最有可能被借鉴的部分不是预测算法本身，而是 `FTQInsert` / `FTQRemove` probe 驱动的生命周期、翻译队列、squash、cache snoop、按 FetchTarget 覆盖所有 cache block 的策略。
- 假设：上游 decoupled frontend 的 `BAC -> FTQ -> Fetch` 切分，和本仓库的 `BPU 自产 FTQ/FSQ -> Fetch` 切分不同；直接搬上游 BAC 价值有限，但其 surprise branch、prediction latency、override/resteer 建模可能有参考价值。
- 假设：本仓库已有更强的 Kunminghu 专用 BPU 结构，所以上游 ARM/Neoverse 配置中的 predictor 组合和宽度参数不能直接作为性能方向。
- 开放问题：本仓库是否已有 FDIP-like 机制或预留接口，只是命名不同；需要从 `fetch.md`、`fetch.cc`、prefetcher 配置和 cache probe 接线确认。
- 开放问题：上游 FDP 的虚拟地址翻译、跨页 prefetch、cache snoop 和 request 标记，与本仓库 RISC-V FS/checkpoint/difftest 路径是否存在副作用。
- 开放问题：上游 `d55f46336f` 的 branch predictor latency / overriding model 与当前 `AheadBTB`、`MicroTAGE`、`numOverrideBubbles` 的关系是什么，能否用于改进观测或实验建模。

## 计划步骤

1. 梳理上游实现骨架，输出文件级结构图和参数表
   - 目标：确认上游 decoupled FE / FDP 的真实控制流
   - 产出：关键文件、类、参数、probe、stats 列表

2. 梳理当前 Kunminghu v3 本地基线
   - 目标：确认本仓库已有的 FTQ/FSQ、fetch target 生命周期、BPU update、ICache/prefetch 接口
   - 产出：本地对应文件、已有能力、缺口列表

3. 建立机制对照表
   - 目标：把上游特征映射到本地模块边界
   - 产出：`上游特征 -> 本地现状 -> 可借鉴方式 -> 风险 -> 验证` 表格

4. 选出第一批候选特征
   - 目标：优先挑不改变预测语义、主要增加观测或可控实验能力的低风险项
   - 产出：推荐优先级和每项最小 patch 思路

5. 定义验证路径
   - 目标：给每个候选项配一个最小可复现验证，而不是只靠直觉
   - 产出：本地 smoke/unit 测试、micro-test 或 SPEC slice 方案

6. 如用户确认方向，再拆分实现阶段
   - 目标：把长期任务拆成可 review 的独立提交
   - 产出：每阶段改动范围、预期收益和回退方式

## 验证

本阶段是分析与方案阶段，完成标准是：

- 已确认上游和本地各自的关键代码入口和控制流
- 已明确哪些上游特征可以借鉴，哪些不建议直接搬
- 每个推荐候选都有本地接入点、风险说明和验证计划
- 若进入代码阶段，优先从观测/配置/低语义风险 patch 开始，并用本仓库既有 BPU 单测、fetch smoke、代表 SPEC slice 或 CI 统计对照验证

## 进展

- [x] 2026-05-18 16:20 创建 ExecPlan，记录任务边界、已知入口和初始假设
- [x] 2026-05-18 16:20 确认两个仓库分支状态：`GEM5_review` 在 `xs-dev`，`GEM5-raw` 在 `develop`
- [x] 2026-05-18 16:20 初步定位上游 decoupled FE/FDP 关键提交和入口文件
- [x] 2026-05-18 16:35 梳理上游 `BAC/FTQ/Fetch/FDP` 详细控制流，确认 FDP 主要通过 FTQ probe 驱动
- [x] 2026-05-18 16:40 梳理本地 `DecoupledBPUWithBTB/FTQ/Fetch/Prefetch` 对应能力，确认当前没有 BPU/FTQ 驱动的 FDIP-like instruction prefetch
- [x] 2026-05-18 16:45 形成第一版机制对照和候选特征推荐
- [x] 2026-05-18 16:55 选择第一项低风险实验：先做 FDIP observability / target lifecycle stats，不直接发 instruction prefetch
- [x] 2026-05-18 16:55 新建分支 `fdip-align`
- [x] 2026-05-18 16:58 提交 `797e2e7cbc cpu: Add FDIP opportunity statistics`
- [x] 2026-05-18 16:59 push 到 `origin/fdip-align`，触发 CI run `26023246359`
- [x] 2026-05-18 17:55 扫描 `gcc15-spec06-0.3c` 最近归档，确认 ICache miss 最高的一批切片主要来自 gcc
- [x] 2026-05-18 18:15 本地完成 `gcc_typeck_4528`、`gcc_expr2_27` 两个切片的 FDIP 观测统计
- [x] 2026-05-18 18:15 尝试本地跑最高 miss 的 `gcc_s04_7630`，但 `gem5.opt` 30 分钟超时，只保留 reset 前 20M 指令窗口作为参考，不作为最终 ROI 结论
- [x] 2026-05-18 18:20 调整后续本地实验口径：优先跑短窗口 `5M+5M` 或 `10M+10M`，并发跑多个切片，先看趋势再决定是否扩展到完整 40M
- [x] 2026-05-18 19:15 实现 fetch 侧 FDIP prototype：从未来 FTQ target 取 `startPC -> predEndPC`，经 ITLB 翻译后向 L1I 发 `SoftPFReq`
- [x] 2026-05-18 19:35 修复 prototype 稳定性问题：补 FDIP retry 队列、squash/reset generation、drain 等待、stale response 丢弃，并修复 cache 里 SoftPFReq 合并到已有 MSHR 后 `pkt == nullptr` 的返回路径
- [x] 2026-05-18 19:40 本地完成 4 个 gcc 高 I-cache-miss 候选切片的 `5M+5M` A/B：`gcc_typeck_4528`、`gcc_expr2_27`、`gcc_200_28`、`gcc_expr_4892`
- [x] 2026-05-18 19:55 试过三类过滤：target distance、跳过 target start block、target age >= 16 cycles；均未形成稳定正收益
- [x] 2026-05-18 20:05 结论：不 push FDIP 行为 patch 到 CI；保留本地 off-by-default prototype 和实验数据，下一步应转向 cache-side FDP/snoop 或更强过滤
- [x] 2026-05-19 16:05 提交 `a7624bfc77 cpu: Add off-by-default FDIP prototype`，把 fetch 侧 prototype 固化为本地备选但默认关闭
- [x] 2026-05-19 16:40 实现 cache-side `FetchDirectedPrefetcher` prototype：BPU/FTQ 发 `FTQInsert`/`FTQRemove` probe，L1I prefetcher 监听 target 生命周期，经 ITB timing translation 后以 `HardPFReq` 进入 cache 侧 snoop/MSHR 路径
- [x] 2026-05-19 16:55 完成 cache-side FDP 的稳定化：默认 `pfq_size=1`、`tq_size=1`、`min_target_distance=32`、`latency=64`，并在 L1I 使用 FDP 时把 `demand_mshr_reserve` 提到 2，避免 4-entry L1I MSHR 被 FDP 抢占过多
- [x] 2026-05-19 17:00 完成本地 `gcc_typeck_4528`、`gcc_expr2_27` 的 `5M+5M` A/B，结果为小幅正向但很接近噪声；准备以单独实验提交启用 `kmhv3.py` 默认 L1I FDP 后 push CI 观察全套 0.3c

## 发现和意外

- 上游实现把通用 O3 decoupled frontend 明确拆成 `BAC` 和 `FTQ`，并用 FTQ probe 驱动 FDP；这和本仓库把预测前端主体放在 `src/cpu/pred/btb/` 内部的组织方式明显不同。
- 上游 FDP 不是简单 next-line prefetch；它跟随 FetchTarget 生命周期，并且后续提交已经扩展到对 FetchTarget 覆盖的所有 cache block 产生候选。
- 上游 `BAC` 的基本模型是从当前 PC 按 `minInstSize` 扫描，借助 `BPredUnit::BTBValid()` 找到第一个 BTB hit，然后把最多一个控制流的 FetchTarget 放入 FTQ。这个模型适合通用 O3 解耦，但不适合直接替代本地 `UBTB/AheadBTB/MicroTAGE/MBTB/BTBTAGE/ITTAGE/MGSC/RAS` 的多级块级预测。
- 上游 `FetchDirectedPrefetcher` 的核心价值在于生命周期接口：FTQ insert 时按 FetchTarget 覆盖的 cache block 产生候选，走 MMU 翻译和 cache/MSHR snoop；FTQ remove/squash 时取消同一 target 的在途翻译和 PFQ 项。
- 本地 `FetchTarget` 已经比上游通用 FetchTarget 丰富很多：包含预测/执行 branch 信息、BTB entries、pred metas、GHR/PHR/BWHR/LHR、统计字段等。因此若做 FDIP，应复用本地结构，不应搬上游结构。
- 本地 `FetchTargetQueue` 目前是预测器内部的 deque + target id，`insert()`、`finishTarget()`、`commitTarget()`、`squashAfter()` 没有对外 probe；fetch 侧只有 demand request 的 `FetchRequestSent` probe。这意味着本地缺的不是普通 cache prefetch 框架，而是“预测 target 生命周期事件”。
- 当前 `kmhv3.py` 对 `DecoupledBPUWithBTB` 设置 `ftq_size=64`、`fsq_size=64`，并启用 UBTB/ABTB/MicroTAGE/MBTB/TAGE/ITTAGE/MGSC/RAS；但 L1I 侧没有默认 FDIP-like prefetcher。
- 当前 Fetch 每拍先处理 redirect/squash，再 `dbpbtb->tick()` 推进预测流水，最后按 `ftqFetchingTarget()` 的 startPC 发 I-cache demand request。真正接入 FDIP 时需要小心这个时序，避免 prefetch 生命周期和 resolve/update/squash 顺序不一致。
- 最近 `gcc15-spec06-0.3c` 归档中，L1I miss 最重的点集中在 gcc：`gcc_s04_7630` 约 20.1 万 misses、2.37% miss rate；`gcc_typeck_4528` 约 10.0 万、1.14%；`gcc_200_28` 约 8.2 万、0.80%；`gcc_expr_4892` 约 6.7 万、0.63%；`gcc_expr2_27` 约 5.8 万、0.61%。
- 本地 `fdip-align` 观测结果显示，`gcc_typeck_4528` 的 prediction-to-fetch 平均只有约 4.0 cycles，约 5.0% target 超过 15 cycles；`gcc_expr2_27` 平均约 5.5 cycles，约 5.4% target 超过 15 cycles。它们的 L1I miss latency 均值分别约 11.0 和 26.8 cycles。
- 本地观测也显示潜在污染压力不小：`gcc_typeck_4528` 的 candidate target 约 69.6% commit、30.4% 被 squash；`gcc_expr2_27` 约 55.5% commit、44.5% 被 squash。`gcc_s04_7630` 前半段参考窗口中 squash 比例约 45.8%。
- 初步判断：当前 BPU/FTQ 提前量对 FDIP 偏短，直接“对所有 target blocks 发 L1I prefetch”可能覆盖有限且错误路径污染较高；若继续做 prototype，应优先加距离/置信过滤，例如只对 ahead >= 16 cycles 的 target、已跨 cache block 的 target、或更可能 commit 的 target 发。
- 后续本地实验不默认完整 40M 指令。先用 `--warmup-insts-no-switch` / `--maxinsts` 风格的短窗口或等价配置跑 `5M+5M`、`10M+10M`，并发覆盖多个代表切片；只有趋势明确或需要和 CI 对齐时再补完整窗口。
- Fetch 侧直接发 `SoftPFReq` 的初版可以稳定运行，但收益不稳。默认近距离策略在 4 个短窗口上的 cycles 变化为：`gcc_typeck_4528` -0.041%、`gcc_expr2_27` -0.198%、`gcc_200_28` +0.527%、`gcc_expr_4892` +0.186%。
- 关键负面信号是 SoftPFReq 绝大多数命中 L1I，几乎没有形成 demand merge：例如默认策略在 `gcc_expr_4892` 发出 231,790 个 FDIP prefetch，其中 SoftPF miss 只有 68；`gcc_expr2_27` 发出 86,680 个，SoftPF miss 只有 73。`system.cpu.icache.demandMergedIntoPfMSHR` 和 `pfMergedWithDemand` 均为 0。
- 距离过滤能局部改善但不稳定：`fdipMinTargetDistance=3`、`fdipLookaheadTargets=8`、`fdipMaxPrefetchesPerCycle=1` 让 `gcc_expr_4892` 从 +0.186% 改到 -0.052%，但 `gcc_expr2_27` 从 -0.198% 变成 +0.290%。
- 跳过 FetchTarget 起始 cache block 的策略整体更差：`gcc_typeck_4528` +0.005%、`gcc_expr2_27` +0.344%、`gcc_200_28` +0.472%、`gcc_expr_4892` +0.351%。
- target age 过滤减小了扰动但仍不够稳：`fdipMinTargetAgeCycles=16` 结果为 `gcc_typeck_4528` +0.132%、`gcc_expr2_27` -0.189%、`gcc_200_28` +0.170%、`gcc_expr_4892` +0.017%。
- 当前 FDIP prototype 的主要问题不是翻译或 retry 稳定性，而是没有 cache-side snoop/去重，fetch 侧 `SoftPFReq` 会消耗大量已经命中的 L1I 访问。更接近上游 FDP 的下一步应放到 L1I prefetcher/cache 侧，利用 cache tag/MSHR snoop 过滤掉已命中的候选，而不是继续在 fetch 侧盲发。
- Cache-side FDP 确认可以复用上游最重要的生命周期思想，但在本仓库 L1I 上必须非常保守。默认 L1I 只有 4 个 MSHR，cache-side `HardPFReq` 会真实占用 MSHR 和下游端口；初始宽松配置在 `gcc_typeck_4528` 10K smoke 中触发 commit stuck。
- 把 cache-side FDP 收紧到 `pfq/tq=1`、`min_target_distance=32`、`latency=64`，并把 L1I `demand_mshr_reserve=2` 后，100K smoke 稳定运行：`gcc_typeck_4528` cycles 173602 -> 172758，I-cache demand misses 1941 -> 1903，但 no-MSHR blocked cycles 10849 -> 17098。
- `gcc_typeck_4528` 的 `5M+5M` 测量段结果为 cycles 1824859 -> 1822698（约 -0.12%），I-cache misses 15411 -> 15428，`pfIssued=113`、`pfUseful=41`、`pfUnused=49`、`demandMergedIntoPfMSHR=10`，no-MSHR blocked cycles 4171 -> 6655。
- `gcc_expr2_27` 的 `5M+5M` 测量段结果为 cycles 2410537 -> 2410225（约 -0.013%），I-cache misses 13294 -> 13243，`pfIssued=293`、`pfUseful=146`、`pfUnused=115`、`demandMergedIntoPfMSHR=31`，no-MSHR blocked cycles 12493 -> 14211。
- 目前 cache-side FDP 的局部信号是“有少量有效覆盖，但 MSHR/port 资源代价也可见”。这值得 push 一轮 CI 看全套 SPEC06 0.3c，但还不能说已经是可合入的收益方向。

## 第一版机制对照

| 主题 | 上游 GEM5-raw | 当前 GEM5_review | 借鉴判断 |
| --- | --- | --- | --- |
| 前端切分 | `BAC -> FTQ -> Fetch`，BAC 是 O3 stage | `DecoupledBPUWithBTB` 内部生成 FSQ/FTQ，Fetch 直接消费 BPU target | 不搬 BAC；只借鉴解耦边界描述和状态统计 |
| FetchTarget 表达 | 简化 basic-block-like target，最多记录 exit branch/pred target/history | 保存块级预测、多个 BTB entry、各组件 meta、history、resolve/update 信息 | 复用本地结构，不降级 |
| BPU 生成方式 | BTBValid 线性扫描到 branch，predict 后插 FTQ | 多组件分 stage 产生 FullBTBPrediction，按 override bubble 延迟入队 | 不搬扫描逻辑；可借鉴 `maxFTPerCycle/maxTakenPredPerCycle` 作为观测维度 |
| FTQ 生命周期 | insert/remove 都有 probe，供 FDP 监听 | insert/finish/commit/squash 都在内部，缺少 target-level probe | 值得补 target-level probe/callback |
| FDIP/FDP | FTQ insert 触发 prefetch，remove/squash 取消，支持 TQ/PFQ/cache snoop/stats | 未见 BPU/FTQ 驱动 instruction prefetch；只有 demand fetch probe 和通用 prefetch 框架 | 最值得借鉴，建议作为独立实验 |
| 配置 | Neoverse/stdlib 示例启用 decoupled FE + L1I FDP/Tagged | `kmhv3.py` 强 RTL-aligned BTB 前端，prefetch 主要在 data/L2/L3 路径 | 只借鉴配置挂法，不借 ARM 数值 |
| 统计 | FTQ occupancy、BAC 状态、FDP 队列/翻译/cache-snoop 统计 | BPU stage/override/FSQ/branch stats 很丰富，但缺 FDIP 专项 | 可以低风险补 FDIP 专项 stats |

## 第一批候选特征

1. Target-level lifecycle probe/callback
   - 接入点：`src/cpu/pred/btb/ftq.hh`、`src/cpu/pred/btb/ftq.cc`、`src/cpu/pred/btb/decoupled_bpred.cc`
   - 做法：为本地 `FetchTargetQueue::insert()`、`finishTarget()`、`squashAfter()`、必要时 `commitTarget()` 增加 target id + FetchTarget 摘要事件，先用于统计或 debug，不改变预测语义。
   - 价值：为 FDIP、FSQ ahead distance、错误路径 prefetch 污染统计提供干净挂点。
   - 风险：低到中。需要注意 `FetchTargetQueue` 当前没有 CPU/probe manager 指针，可能更适合先在 `DecoupledBPUWithBTB` 层发事件。

2. Kunminghu-FDIP prototype
   - 接入点：本地 L1I prefetcher 或新的 `FetchTarget` listener；配置入口可从 `kmhv3.py` 加显式开关。
   - 做法：参考上游 `FetchDirectedPrefetcher` 的 TQ/PFQ/cache snoop/cancel 语义，但输入改成本地 `FetchTarget` 的 `startPC -> predEndPC` 范围。
   - 价值：最可能带来前端 I-cache latency 改善，尤其对 BTB 已能提前看到后续 target 的场景。
   - 风险：中到高。会跨 BPU target 生命周期、RISC-V MMU 翻译、I-cache MSHR、错误路径 squash；必须从 off-by-default 实验开关开始。

3. FDIP observability first
   - 接入点：新 stats 或 debug flag。
   - 做法：即使暂不发 prefetch，也先统计 FSQ target 的 ahead distance、覆盖 cache block 数、被 squash/commit/finish 的比例、target 从入队到 fetch 的提前周期。
   - 价值：判断当前 BPU ahead depth 是否足以支撑 FDIP；如果提前量不够，先做 FDIP 发包意义不大。
   - 风险：低，是最适合第一步的实验。

4. Surprise branch / no-history taxonomy
   - 接入点：当前 `controlSquash`、`topMispredictsByBranch`、BTB miss/false hit stats。
   - 做法：借鉴上游“BTB 没看到但 fetch/decode 发现”的分类思路，细分当前 no-pred / false-hit / target-wrong 场景。
   - 价值：帮助区分 BPU 没学到、BTB 容量不够、方向错、target 错、fetch block 截断等原因。
   - 风险：低，主要是统计口径设计。

5. Branch predictor latency / override model 对照
   - 接入点：当前 `numOverrideBubbles`、`predsOfEachStage`、`overrideReason`。
   - 做法：不搬上游 latency 模型，但检查上游 `d55f46336f` 后的 `Prediction.latency` 和 override/resteer 分类，看是否能补充本地 stage latency 观测。
   - 价值：让 `AheadBTB/MicroTAGE` 的收益和 override bubble 成本更容易解释。
   - 风险：低到中，取决于是否只补统计还是改行为。

## 决策记录

- Decision: 本轮先做机制对比和候选特征筛选，不直接移植代码。
- Reason: 两边前端架构边界不同，直接搬上游 BAC/FTQ 容易破坏本仓库 RTL-aligned 路径；先筛选低风险特征更稳。
- Date: 2026-05-18
- Decision: 第一优先级建议从 FDIP observability / target lifecycle event 开始，而不是直接发 I-cache prefetch。
- Reason: 当前本地缺的是 target-level 生命周期挂点和 ahead-distance/污染统计；先补观测能判断 FDIP 是否有足够提前量，并降低对 Fetch/ResolveQueue 语义的扰动。
- Date: 2026-05-18
- Decision: `fdip-align` 首个 patch 只增加统计，不改变预测、fetch 或 cache 行为。
- Reason: 需要先确认当前 FSQ/FTQ ahead distance、candidate cache block 数、fetch/commit/squash 生命周期比例；如果 SPEC06 的 L1I miss 很少或预测提前量不够，FDIP prototype 的收益预期会很弱。
- Date: 2026-05-18
- Decision: 不把 fetch-side FDIP prototype push 到 CI。
- Reason: 本地 `5M+5M` 短窗口没有稳定收益，且 SoftPFReq 绝大多数是 L1I hit，说明当前直接从 fetch 端发 prefetch 缺少上游 FDP 的 cache-side snoop/去重能力。继续推 CI 大概率浪费完整 SPEC 资源。
- Date: 2026-05-18
- Decision: 若继续 FDIP，应优先做 cache-side FDP/snoop 型实现，或至少为 fetch-side prototype 增加“只对 cache/MSHR miss 候选发包”的过滤接口。
- Reason: 上游 FDP 的 TQ/PFQ/cache snoop 生命周期正是当前 prototype 缺失的关键能力；仅靠 target distance、skip-start-block、target-age 过滤无法稳定避免 L1I-hit prefetch 扰动。
- Date: 2026-05-18
